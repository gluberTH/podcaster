#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <span>

namespace dsp {

class Biquad {
public:
    void SetHighPass(double freq, double q, double sr) {
        double w0 = 2.0 * std::numbers::pi * freq / sr;
        double cosW = std::cos(w0);
        double sinW = std::sin(w0);
        double alpha = sinW / (2.0 * q);
        double b0 = (1.0 + cosW) * 0.5;
        double b1 = -(1.0 + cosW);
        double b2 = (1.0 + cosW) * 0.5;
        double a0 = 1.0 + alpha;
        double a1 = -2.0 * cosW;
        double a2 = 1.0 - alpha;
        SetNorm(b0, b1, b2, a0, a1, a2);
    }

    void SetBandPass(double freq, double q, double sr) {
        double w0 = 2.0 * std::numbers::pi * freq / sr;
        double alpha = std::sin(w0) / (2.0 * q);
        SetNorm(alpha, 0.0, -alpha, 1.0 + alpha, -2.0 * std::cos(w0), 1.0 - alpha);
    }

    void SetLowShelf(double freq, double gainDb, double slope, double sr) {
        double A = std::pow(10.0, gainDb / 40.0);
        double w0 = 2.0 * std::numbers::pi * freq / sr;
        double cosW = std::cos(w0);
        double sinW = std::sin(w0);
        double alpha = sinW / 2.0 * std::sqrt((A + 1.0 / A) * (1.0 / slope - 1.0) + 2.0);
        double twoSqrtAAlpha = 2.0 * std::sqrt(A) * alpha;

        double b0 = A * ((A + 1.0) - (A - 1.0) * cosW + twoSqrtAAlpha);
        double b1 = 2.0 * A * ((A - 1.0) - (A + 1.0) * cosW);
        double b2 = A * ((A + 1.0) - (A - 1.0) * cosW - twoSqrtAAlpha);
        double a0 = (A + 1.0) + (A - 1.0) * cosW + twoSqrtAAlpha;
        double a1 = -2.0 * ((A - 1.0) + (A + 1.0) * cosW);
        double a2 = (A + 1.0) + (A - 1.0) * cosW - twoSqrtAAlpha;
        SetNorm(b0, b1, b2, a0, a1, a2);
    }

    void SetHighShelf(double freq, double gainDb, double slope, double sr) {
        double A = std::pow(10.0, gainDb / 40.0);
        double w0 = 2.0 * std::numbers::pi * freq / sr;
        double cosW = std::cos(w0);
        double sinW = std::sin(w0);
        double alpha = sinW / 2.0 * std::sqrt((A + 1.0 / A) * (1.0 / slope - 1.0) + 2.0);
        double twoSqrtAAlpha = 2.0 * std::sqrt(A) * alpha;

        double b0 = A * ((A + 1.0) + (A - 1.0) * cosW + twoSqrtAAlpha);
        double b1 = -2.0 * A * ((A - 1.0) + (A + 1.0) * cosW);
        double b2 = A * ((A + 1.0) + (A - 1.0) * cosW - twoSqrtAAlpha);
        double a0 = (A + 1.0) - (A - 1.0) * cosW + twoSqrtAAlpha;
        double a1 = 2.0 * ((A - 1.0) - (A + 1.0) * cosW);
        double a2 = (A + 1.0) - (A - 1.0) * cosW - twoSqrtAAlpha;
        SetNorm(b0, b1, b2, a0, a1, a2);
    }

    void SetPeaking(double freq, double gainDb, double q, double sr) {
        double A = std::pow(10.0, gainDb / 40.0);
        double w0 = 2.0 * std::numbers::pi * freq / sr;
        double cosW = std::cos(w0);
        double sinW = std::sin(w0);
        double alpha = sinW / (2.0 * q);

        double b0 = 1.0 + alpha * A;
        double b1 = -2.0 * cosW;
        double b2 = 1.0 - alpha * A;
        double a0 = 1.0 + alpha / A;
        double a1 = -2.0 * cosW;
        double a2 = 1.0 - alpha / A;
        SetNorm(b0, b1, b2, a0, a1, a2);
    }

    inline float Process(float in) {
        double x = static_cast<double>(in);
        double y = b0_ * x + z1_;
        z1_ = b1_ * x - a1_ * y + z2_;
        z2_ = b2_ * x - a2_ * y;
        return static_cast<float>(y);
    }

private:
    void SetNorm(double b0, double b1, double b2, double a0, double a1, double a2) {
        b0_ = b0 / a0;
        b1_ = b1 / a0;
        b2_ = b2 / a0;
        a1_ = a1 / a0;
        a2_ = a2 / a0;
    }

    double b0_ = 1.0, b1_ = 0.0, b2_ = 0.0, a1_ = 0.0, a2_ = 0.0;
    double z1_ = 0.0, z2_ = 0.0;
};

inline float SmoothingCoefficient(float seconds, int sampleRate) {
    return std::exp(-1.0f / (seconds * sampleRate));
}

inline float Follow(float value, float target, float coefficient) {
    return target + coefficient * (value - target);
}

inline float SoftenPeaks(float sample) {
    constexpr float knee = 0.95f;
    constexpr float headroom = 1.0f - knee;
    const float magnitude = std::fabs(sample);
    return magnitude <= knee ? sample : std::copysign(
        knee + headroom * std::tanh((magnitude - knee) / headroom), sample);
}

inline void RemoveRumble(std::span<float> samples, int sampleRate, std::span<const float> warmup = {}) {
    Biquad highPass;
    highPass.SetHighPass(80.0, 0.70710678, sampleRate);
    for (float sample : warmup) highPass.Process(sample);
    for (float& sample : samples) sample = highPass.Process(sample);
}

// Downward expander with an open hold and a finite attenuation floor.
inline void ApplyNoiseGate(std::span<float> samples, int sampleRate) {
    constexpr float threshold = 0.00794328f;  // -42 dBFS
    constexpr float minimumGain = 0.12589254f;  // -18 dB
    const float attack = SmoothingCoefficient(0.005f, sampleRate);
    const float release = SmoothingCoefficient(0.120f, sampleRate);
    const auto holdSamples = static_cast<std::size_t>(0.060f * sampleRate);
    float envelope = 0.0f;
    float gain = 1.0f;
    std::size_t hold = 0;

    for (float& sample : samples) {
        const float magnitude = std::fabs(sample);
        envelope = Follow(envelope, magnitude, magnitude > envelope ? attack : release);

        float targetGain = 1.0f;
        if (envelope >= threshold) {
            hold = holdSamples;
        } else if (hold > 0) {
            --hold;
        } else {
            // Squaring the amplitude ratio gives 3:1 downward expansion.
            const float ratio = envelope / threshold;
            targetGain = std::max(minimumGain, ratio * ratio);
        }
        gain = Follow(gain, targetGain, targetGain > gain ? attack : release);
        sample *= gain;
    }
}

// Attenuate the sibilance band when it dominates and reaches an audible level.
inline void ApplyDeEsser(std::span<float> samples, int sampleRate) {
    Biquad sibilance;
    sibilance.SetBandPass(6800.0, 1.2, sampleRate);
    const float attack = SmoothingCoefficient(0.0015f, sampleRate);
    const float release = SmoothingCoefficient(0.045f, sampleRate);
    constexpr float threshold = 0.04466836f;  // -27 dBFS
    constexpr float maxReduction = 0.5f;
    float bandEnvelope = 0.0f;
    float fullEnvelope = 0.0f;
    float reduction = 0.0f;

    for (float& sample : samples) {
        const float band = sibilance.Process(sample);
        const float bandLevel = std::fabs(band);
        const float fullLevel = std::fabs(sample);
        bandEnvelope = Follow(bandEnvelope, bandLevel, bandLevel > bandEnvelope ? attack : release);
        fullEnvelope = Follow(fullEnvelope, fullLevel, fullLevel > fullEnvelope ? attack : release);
        const float trigger = std::max(threshold, 0.4f * fullEnvelope);
        const float target = bandEnvelope > trigger
            ? std::min(maxReduction, 1.0f - trigger / bandEnvelope) : 0.0f;
        reduction = Follow(reduction, target, target > reduction ? attack : release);
        sample -= reduction * band;
    }
}

inline void ApplyCompressor(std::span<float> samples, int sampleRate) {
    constexpr float threshold = 0.12589254f;  // -18 dBFS
    const float attack = SmoothingCoefficient(0.006f, sampleRate);
    const float release = SmoothingCoefficient(0.090f, sampleRate);
    float envelope = 0.0f;

    for (float& sample : samples) {
        const float magnitude = std::fabs(sample);
        envelope = Follow(envelope, magnitude, magnitude > envelope ? attack : release);
        if (envelope > threshold) {
            sample *= std::sqrt(threshold / envelope); // 2:1 above threshold
        }
    }
}

inline constexpr float kLimiterCeiling = 0.89125094f;  // -1 dBFS sample peak

inline void ApplyLimiter(std::span<float> samples, int sampleRate) {
    const float release = SmoothingCoefficient(0.070f, sampleRate);
    float gain = 1.0f;
    for (float& sample : samples) {
        const float magnitude = std::fabs(sample);
        gain = Follow(gain, 1.0f, release);
        if (magnitude * gain > kLimiterCeiling) {
            gain = kLimiterCeiling / magnitude;
        }
        sample = std::clamp(sample * gain, -kLimiterCeiling, kLimiterCeiling);
    }
}

} // namespace dsp
