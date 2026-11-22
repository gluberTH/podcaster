#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace silence {

struct Range {
    std::size_t start = 0;
    std::size_t end = 0;  // exclusive
    bool operator==(const Range&) const = default;
};

inline constexpr float kMinThresholdDb = -70.0f;
inline constexpr float kMaxThresholdDb = -15.0f;
inline constexpr int kMinDurationMs = 100;
inline constexpr int kMaxDurationMs = 5000;
inline constexpr int kMinEdgeMs = 0;
inline constexpr int kMaxEdgeMs = 500;

struct Settings {
    float thresholdDb = -42.0f;
    int minimumMs = 500;
    int keepEdgeMs = 100;
};

using Cancellation = const std::atomic<bool>*;

inline Settings ClampSettings(Settings settings) {
    settings.thresholdDb = std::isfinite(settings.thresholdDb)
        ? std::clamp(settings.thresholdDb, kMinThresholdDb, kMaxThresholdDb)
        : Settings{}.thresholdDb;
    settings.minimumMs = std::clamp(settings.minimumMs, kMinDurationMs, kMaxDurationMs);
    settings.keepEdgeMs = std::clamp(settings.keepEdgeMs, kMinEdgeMs, kMaxEdgeMs);
    return settings;
}

namespace detail {

inline bool IsCanceled(Cancellation cancel) {
    return cancel && cancel->load(std::memory_order_relaxed);
}

inline std::size_t SamplesFromMs(int milliseconds, int sampleRate) {
    return static_cast<std::size_t>((static_cast<std::uint64_t>(milliseconds) * sampleRate + 999) / 1000);
}

inline bool FrameIsQuiet(std::span<const float> frame, double powerThreshold, double peakThreshold) {
    double energy = 0.0;
    for (float sample : frame) {
        if (!std::isfinite(sample) || std::fabs(sample) > peakThreshold) return false;
        energy += static_cast<double>(sample) * sample;
    }
    return energy <= powerThreshold * frame.size();
}

inline bool CopySamples(std::vector<float>& output, std::span<const float> samples, Cancellation cancel) {
    constexpr std::size_t blockSize = 65536;
    while (!samples.empty()) {
        if (IsCanceled(cancel)) return false;
        const auto block = samples.first(std::min(blockSize, samples.size()));
        output.insert(output.end(), block.begin(), block.end());
        samples = samples.subspan(block.size());
    }
    return true;
}

inline float FadeWeight(std::size_t i, std::size_t length) {
    return length == 1 ? 1.0f : static_cast<float>(i) / static_cast<float>(length - 1);
}

} // namespace detail

// Returns sorted, nonoverlapping intervals to remove. An entirely quiet scope is left intact.
inline std::optional<std::vector<Range>> Analyze(std::span<const float> source, Range scope, int sampleRate,
                                                 Settings settings, Cancellation cancel = nullptr) {
    if (detail::IsCanceled(cancel)) return std::nullopt;
    std::vector<Range> cuts;
    scope.end = std::min(scope.end, source.size());
    if (sampleRate <= 0 || scope.start >= scope.end) return cuts;

    settings = ClampSettings(settings);
    const auto frameSize = detail::SamplesFromMs(10, sampleRate);
    const auto minimum = detail::SamplesFromMs(settings.minimumMs, sampleRate);
    const auto padding = std::max(frameSize, detail::SamplesFromMs(settings.keepEdgeMs, sampleRate));
    const auto minimumCut = detail::SamplesFromMs(5, sampleRate);
    const double powerThreshold = std::pow(10.0, settings.thresholdDb / 10.0);
    const double peakThreshold = std::sqrt(10.0 * powerThreshold);  // protect brief sounds at +10 dB

    auto finishQuietRun = [&](std::size_t start, std::size_t end) {
        const auto length = end - start;
        const auto before = start == 0 ? 0 : padding;
        const auto after = end == source.size() ? 0 : padding;
        if (length >= minimum && length >= before + after + minimumCut) {
            cuts.push_back({start + before, end - after});
        }
    };

    bool hasSound = false;
    std::size_t quietStart = scope.end;
    for (auto start = scope.start; start < scope.end;) {
        if (detail::IsCanceled(cancel)) return std::nullopt;
        const auto count = std::min(frameSize, scope.end - start);
        if (detail::FrameIsQuiet(source.subspan(start, count), powerThreshold, peakThreshold)) {
            if (quietStart == scope.end) quietStart = start;
        } else {
            if (quietStart != scope.end) finishQuietRun(quietStart, start);
            quietStart = scope.end;
            hasSound = true;
        }
        start += count;
    }

    if (hasSound && quietStart != scope.end) finishQuietRun(quietStart, scope.end);
    if (detail::IsCanceled(cancel)) return std::nullopt;
    return cuts;
}

inline std::size_t RemovedSamples(std::span<const Range> cuts) {
    std::size_t removed = 0;
    for (const auto& cut : cuts) removed += cut.end - cut.start;
    return removed;
}

// Positions inside a deleted interval land on its new splice, including a trailing cut at EOF.
inline std::size_t MapSample(std::span<const Range> cuts, std::size_t sample) {
    std::size_t removed = 0;
    for (const auto& cut : cuts) {
        if (sample < cut.start) break;
        if (sample < cut.end) return cut.start - removed;
        removed += cut.end - cut.start;
    }
    return sample - removed;
}

// The outgoing side of each splice blends with the last samples in the cut. This keeps
// the exact analyzed removal length and leaves samples outside the selected scope intact.
inline std::optional<std::vector<float>> Apply(std::span<const float> source, Range scope,
                                               std::span<const Range> cuts, int sampleRate,
                                               Cancellation cancel = nullptr) {
    if (detail::IsCanceled(cancel) || sampleRate <= 0 || scope.start > scope.end || scope.end > source.size()) {
        return std::nullopt;
    }
    auto previousEnd = scope.start;
    for (const auto& cut : cuts) {
        if (cut.start < previousEnd || cut.end <= cut.start || cut.end > scope.end) return std::nullopt;
        if (detail::IsCanceled(cancel)) return std::nullopt;
        previousEnd = cut.end;
    }
    const auto removed = RemovedSamples(cuts);
    if (!cuts.empty() && removed == source.size()) return std::nullopt;

    const auto maxFade = detail::SamplesFromMs(5, sampleRate);
    std::vector<float> output;
    output.reserve(source.size() - removed);
    std::size_t cursor = 0;
    bool fadeFromStart = false;
    for (const auto& cut : cuts) {
        if (!detail::CopySamples(output, source.subspan(cursor, cut.start - cursor), cancel)) return std::nullopt;
        const auto available = cut.start - std::max(cursor, scope.start);
        const auto fade = std::min({maxFade, available, cut.end - cut.start});
        const auto outputStart = output.size() - fade;
        for (std::size_t i = 0; i < fade; ++i) {
            const float target = cut.end == source.size() ? 0.0f : source[cut.end - fade + i];
            output[outputStart + i] = std::lerp(output[outputStart + i], target, detail::FadeWeight(i, fade));
        }
        if (cut.start == 0) fadeFromStart = true;
        cursor = cut.end;
    }
    if (!detail::CopySamples(output, source.subspan(cursor), cancel)) return std::nullopt;

    if (fadeFromStart) {
        const auto firstRetainedEnd = cuts.size() > 1 ? cuts[1].start : scope.end;
        const auto fade = std::min(maxFade, firstRetainedEnd - cuts.front().end);
        for (std::size_t i = 0; i < fade; ++i) {
            output[i] *= fade == 1 ? 0.0f : detail::FadeWeight(i, fade);
        }
    }
    if (detail::IsCanceled(cancel)) return std::nullopt;
    return output;
}

} // namespace silence
