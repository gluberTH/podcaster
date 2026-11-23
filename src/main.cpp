#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <windowsx.h>
#include <commdlg.h>
#include <shellapi.h>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <ksmedia.h>
#include <functiondiscoverykeys_devpkey.h>
#include <avrt.h>
#include <d2d1.h>
#include <d2d1helper.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <numbers>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include "effects.h"
#include "localization.h"
#include "project_file.h"
#include "silence.h"

using Microsoft::WRL::ComPtr;
using locale_ui::Language;
using locale_ui::Text;
namespace fs = std::filesystem;

// ============================================================================
// Constants & Theme Palette
// ============================================================================

constexpr int kSampleRate = 48000;
constexpr size_t kWaveformBucketSize = 64; // ~1.33 ms per bucket
constexpr size_t kMaxUndoSteps = 12;
constexpr float kMinPrompterSpeed = 10.0f;
constexpr float kMaxPrompterSpeed = 180.0f;
constexpr float kPrompterSpeedStep = 5.0f;
constexpr int kMinClientWidth = 960;
constexpr int kMinClientHeight = 620;
constexpr float kMinMasterGainDb = -24.0f;
constexpr float kMaxMasterGainDb = 12.0f;
constexpr UINT kAudioWorkReadyMessage = WM_APP + 3;
constexpr UINT kAudioBackupFailedMessage = WM_APP + 4;
constexpr UINT kDocumentReadyMessage = WM_APP + 5;

static D2D1_COLOR_F ColorHex(uint32_t rgb, float alpha = 1.0f) {
    return D2D1::ColorF(
        ((rgb >> 16) & 0xFF) / 255.0f,
        ((rgb >> 8) & 0xFF) / 255.0f,
        (rgb & 0xFF) / 255.0f,
        alpha
    );
}

namespace Theme {
    const D2D1_COLOR_F BgMain         = ColorHex(0x161616);
    const D2D1_COLOR_F BgPanel        = ColorHex(0x1F1F1F);
    const D2D1_COLOR_F BgPanelAlt     = ColorHex(0x262626);
    const D2D1_COLOR_F BgButton       = ColorHex(0x2B2B2B);
    const D2D1_COLOR_F BgButtonHov    = ColorHex(0x363636);
    const D2D1_COLOR_F BgButtonActive = ColorHex(0x3E3A32);
    const D2D1_COLOR_F Border         = ColorHex(0x333333);
    const D2D1_COLOR_F BorderActive   = ColorHex(0xD49B4B);
    const D2D1_COLOR_F TextPrimary    = ColorHex(0xEAEAEA);
    const D2D1_COLOR_F TextSecond     = ColorHex(0xA3A3A3);
    const D2D1_COLOR_F TextMuted      = ColorHex(0x6E6E6E);
    const D2D1_COLOR_F AccentCyan     = ColorHex(0xD49B4B); // Warm studio amber accent
    const D2D1_COLOR_F AccentBlue     = ColorHex(0x3B82F6);
    const D2D1_COLOR_F RecordRed      = ColorHex(0xD94040);
    const D2D1_COLOR_F RecordDark     = ColorHex(0x521C1C);
    const D2D1_COLOR_F GoodGreen      = ColorHex(0x3FA36C);
    const D2D1_COLOR_F WarnAmber      = ColorHex(0xE09F3E);
    const D2D1_COLOR_F WaveFill       = ColorHex(0x9E9E9E, 0.78f);
    const D2D1_COLOR_F WaveRms        = ColorHex(0xDADADA, 0.95f);
    const D2D1_COLOR_F SelOverlay     = ColorHex(0xFFFFFF, 0.12f);
}

// ============================================================================
// String & File Helpers
// ============================================================================

static std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), (int)s.size(), nullptr, 0);
    if (len <= 0) return {};
    std::wstring out(len, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), (int)s.size(), out.data(), len);
    return out;
}

static std::string WideToUtf8(const std::wstring& s) {
    if (s.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string out(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), len, nullptr, nullptr);
    return out;
}

static std::wstring FormatTime(double seconds, bool includeTenths = true) {
    if (seconds < 0.0) seconds = 0.0;
    int totalTenths = static_cast<int>(std::llround(seconds * 10.0));
    int tenths = totalTenths % 10;
    int totalSec = totalTenths / 10;
    int mins = totalSec / 60;
    int secs = totalSec % 60;
    wchar_t buf[64];
    if (includeTenths) {
        swprintf_s(buf, L"%02d:%02d.%d", mins, secs, tenths);
    } else {
        swprintf_s(buf, L"%02d:%02d", mins, secs);
    }
    return buf;
}

static std::wstring ReadTextFileSmart(const std::wstring& path) {
    std::ifstream f(fs::path(path), std::ios::binary);
    if (!f) return L"";
    std::string bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (bytes.empty()) return L"";

    // UTF-8 BOM
    if (bytes.size() >= 3 && (unsigned char)bytes[0] == 0xEF &&
        (unsigned char)bytes[1] == 0xBB && (unsigned char)bytes[2] == 0xBF) {
        return Utf8ToWide(bytes.substr(3));
    }
    // UTF-16 LE BOM
    if (bytes.size() >= 2 && (unsigned char)bytes[0] == 0xFF && (unsigned char)bytes[1] == 0xFE) {
        size_t chars = (bytes.size() - 2) / 2;
        std::wstring w(chars, L'\0');
        std::memcpy(w.data(), bytes.data() + 2, chars * 2);
        return w;
    }
    // UTF-8
    std::wstring w = Utf8ToWide(bytes);
    if (!w.empty()) return w;

    // Fallback ANSI
    int len = MultiByteToWideChar(CP_ACP, 0, bytes.data(), (int)bytes.size(), nullptr, 0);
    if (len <= 0) return L"";
    std::wstring acp(len, L'\0');
    MultiByteToWideChar(CP_ACP, 0, bytes.data(), (int)bytes.size(), acp.data(), len);
    return acp;
}

static std::wstring StripMarkdownTokens(const std::wstring& line) {
    std::wstring out;
    out.reserve(line.size());
    for (size_t i = 0; i < line.size(); ++i) {
        if (i + 1 < line.size() && ((line[i] == L'*' && line[i + 1] == L'*') ||
                                    (line[i] == L'_' && line[i + 1] == L'_'))) {
            ++i;
            continue;
        }
        if (line[i] == L'*' || line[i] == L'`') continue;
        out.push_back(line[i]);
    }
    return out;
}

static std::wstring CleanMarkdownForPrompter(const std::wstring& raw) {
    std::wstringstream ss(raw);
    std::wstring line;
    std::wstring result;
    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        size_t firstNonSpace = line.find_first_not_of(L" \t");
        if (firstNonSpace == std::wstring::npos) {
            result += L"\n";
            continue;
        }
        std::wstring trimmed = line.substr(firstNonSpace);
        if (trimmed.rfind(L"---", 0) == 0 || trimmed.rfind(L"***", 0) == 0) {
            result += L"\n";
            continue;
        }
        if (trimmed[0] == L'#') {
            size_t hashEnd = trimmed.find_first_not_of(L"# ");
            std::wstring title = (hashEnd == std::wstring::npos) ? L"" : trimmed.substr(hashEnd);
            title = StripMarkdownTokens(title);
            if (!title.empty()) {
                result += L"◆ " + title + L"\n";
            }
            continue;
        }
        result += StripMarkdownTokens(trimmed) + L"\n";
    }
    return result;
}

static std::wstring FindLatestEpisodeScript() {
    std::vector<fs::path> searchRoots = {
        fs::current_path() / "episodes",
        fs::path(L"e:\\Ai Podcast\\episodes")
    };
    wchar_t exeBuf[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, exeBuf, MAX_PATH)) {
        fs::path exeDir = fs::path(exeBuf).parent_path();
        searchRoots.push_back(exeDir / "episodes");
        searchRoots.push_back(exeDir.parent_path() / "episodes");
    }

    std::vector<fs::path> candidates;
    for (const auto& root : searchRoots) {
        std::error_code ec;
        if (!fs::exists(root, ec) || !fs::is_directory(root, ec)) continue;
        for (const auto& entry : fs::directory_iterator(root, ec)) {
            if (!entry.is_directory(ec)) continue;
            for (const wchar_t* name : { L"script-recording.md", L"script-roast.md", L"script.md" }) {
                fs::path p = entry.path() / name;
                if (fs::exists(p, ec)) {
                    candidates.push_back(p);
                    break;
                }
            }
        }
        if (!candidates.empty()) break;
    }
    if (candidates.empty()) return L"";
    std::sort(candidates.begin(), candidates.end(), [](const fs::path& a, const fs::path& b) {
        return a.parent_path().filename().wstring() > b.parent_path().filename().wstring();
    });
    return candidates.front().wstring();
}

// ============================================================================
// Audio Device & DSP Structures
// ============================================================================

struct AudioDeviceInfo {
    std::wstring id;
    std::wstring name;
    bool isDefault = false;
};

struct WaveformBucket {
    float minVal = 0.0f;
    float maxVal = 0.0f;
    float rmsVal = 0.0f;
};

struct EqParams {
    float lowDb = 0.0f;   // 120 Hz Low Shelf
    float midDb = 0.0f;   // 2800 Hz Peaking
    float highDb = 0.0f;  // 9000 Hz High Shelf
};

struct AudioTrack {
    std::vector<float> samples;
    std::vector<WaveformBucket> wave;
    float peak = 0.0f;
    bool limiter = false;
};

struct EffectSettings {
    bool cleanup = false;
    bool fadeIn = false;
    bool fadeOut = false;
    bool echo = false;
    bool equalizer = false;
    bool highPass = false;
    bool gate = false;
    bool deEsser = false;
    bool compressor = false;
    bool limiter = false;
    EqParams eq{2.0f, 2.5f, 1.5f};
};

enum class AudioWorkKind { Effects, SilencePreview, RemoveSilence };

struct AudioWork {
    AudioWorkKind kind = AudioWorkKind::Effects;
    uint64_t revision = 0;
    std::shared_ptr<const AudioTrack> source;
    std::shared_ptr<std::atomic<bool>> cancelled;
    EffectSettings effects{};
    float thresholdDb = -42.0f;
    std::vector<silence::Range> cuts;
};

struct AudioWorkResult {
    AudioWorkKind kind = AudioWorkKind::Effects;
    uint64_t revision = 0;
    std::shared_ptr<AudioTrack> track;
    std::vector<silence::Range> cuts;
    EffectSettings effects;
    bool succeeded = false;
};

struct AudioBackup {
    std::shared_ptr<const AudioTrack> source;
    bool timestamped = false;
    LPARAM revision = 0;
};

enum class DocumentOperation { OpenAudio, OpenProject, SaveProject };

struct DocumentResult {
    DocumentOperation operation = DocumentOperation::OpenAudio;
    fs::path path;
    std::shared_ptr<AudioTrack> track;
    project_file::Settings settings;
    std::wstring script;
    std::wstring error;
    uint64_t revision = 0;
};

static void CrossfadeSpliceBoundaries(
    std::vector<float>& buf,
    const std::vector<float>& origSnapshot,
    size_t start,
    size_t end
) {
    if (buf.size() != origSnapshot.size() || start >= end || end > buf.size()) return;
    constexpr size_t kFadeLen = 240; // 5 ms at 48 kHz
    if (start > 0) {
        size_t len = std::min(kFadeLen, end - start);
        for (size_t i = 0; i < len; ++i) {
            float t = static_cast<float>(i) / static_cast<float>(len);
            buf[start + i] = origSnapshot[start + i] * (1.0f - t) + buf[start + i] * t;
        }
    }
    if (end < buf.size()) {
        size_t len = std::min(kFadeLen, end - start);
        for (size_t i = 0; i < len; ++i) {
            float t = static_cast<float>(i) / static_cast<float>(len);
            size_t idx = end - len + i;
            buf[idx] = buf[idx] * (1.0f - t) + origSnapshot[idx] * t;
        }
    }
}

static void ApplyVoiceCleanupDsp(std::vector<float>& buf, size_t start, size_t end) {
    if (start >= end || end > buf.size()) return;
    std::vector<float> orig;
    if (start > 0 || end < buf.size()) {
        orig = buf;
    }

    auto audio = std::span(buf);
    auto region = audio.subspan(start, end - start);
    size_t warmStart = start - std::min<size_t>(start, 240);
    dsp::RemoveRumble(region, kSampleRate, audio.subspan(warmStart, start - warmStart));
    dsp::ApplyNoiseGate(region, kSampleRate);

    // 3. Peak Normalize to -1.0 dBFS (0.89125f)
    float peak = 0.0f;
    for (size_t i = start; i < end; ++i) {
        peak = std::max(peak, std::fabs(buf[i]));
    }
    if (peak > 0.001f) {
        float scale = std::min(15.85f, 0.89125f / peak);
        for (size_t i = start; i < end; ++i) {
            buf[i] = std::clamp(buf[i] * scale, -0.99f, 0.99f);
        }
    }

    if (!orig.empty()) {
        CrossfadeSpliceBoundaries(buf, orig, start, end);
    }
}

static void ApplyFadeInDsp(std::vector<float>& buf, size_t start, size_t end) {
    if (start >= end || end > buf.size()) return;
    size_t len = end - start;
    if (len < 2) return;
    for (size_t i = 0; i < len; ++i) {
        double t = static_cast<double>(i) / static_cast<double>(len - 1);
        float g = static_cast<float>(0.5 * (1.0 - std::cos(std::numbers::pi * t)));
        buf[start + i] *= g;
    }
}

static void ApplyFadeOutDsp(std::vector<float>& buf, size_t start, size_t end) {
    if (start >= end || end > buf.size()) return;
    size_t len = end - start;
    if (len < 2) return;
    for (size_t i = 0; i < len; ++i) {
        double t = static_cast<double>(i) / static_cast<double>(len - 1);
        float g = static_cast<float>(0.5 * (1.0 + std::cos(std::numbers::pi * t)));
        buf[start + i] *= g;
    }
}

static void ApplyEchoDsp(std::vector<float>& buf, size_t start, size_t end) {
    if (start >= end || end > buf.size()) return;
    std::vector<float> orig;
    if (start > 0 || end < buf.size()) orig = buf;

    const size_t delaySamples = static_cast<size_t>(0.140 * kSampleRate); // 140 ms
    std::vector<float> delayLine(delaySamples, 0.0f);
    size_t idx = 0;
    float lpState = 0.0f;
    // 1-pole LPF ~3.5 kHz damping in feedback
    const float lpAlpha = 1.0f - std::exp(-2.0f * static_cast<float>(std::numbers::pi) * 3500.0f / kSampleRate);
    const float feedback = 0.26f;
    const float wet = 0.24f;
    const float dry = 0.94f;

    for (size_t i = start; i < end; ++i) {
        float x = buf[i];
        float delayed = delayLine[idx];
        lpState += lpAlpha * (delayed - lpState);
        delayLine[idx] = std::clamp(x + lpState * feedback, -1.5f, 1.5f);
        idx = (idx + 1) % delaySamples;
        buf[i] = dsp::SoftenPeaks(dry * x + wet * delayed);
    }

    if (!orig.empty()) {
        CrossfadeSpliceBoundaries(buf, orig, start, end);
    }
}

static void ApplyEqualizerDsp(std::vector<float>& buf, size_t start, size_t end, const EqParams& eq) {
    if (start >= end || end > buf.size()) return;
    if (std::fabs(eq.lowDb) < 0.05f && std::fabs(eq.midDb) < 0.05f && std::fabs(eq.highDb) < 0.05f) {
        return;
    }
    std::vector<float> orig;
    if (start > 0 || end < buf.size()) orig = buf;

    dsp::Biquad low, mid, high;
    low.SetLowShelf(120.0, eq.lowDb, 0.9, kSampleRate);
    mid.SetPeaking(2800.0, eq.midDb, 1.0, kSampleRate);
    high.SetHighShelf(9000.0, eq.highDb, 0.9, kSampleRate);

    size_t warmStart = (start > 240) ? (start - 240) : 0;
    for (size_t i = warmStart; i < start; ++i) {
        float s = buf[i];
        s = low.Process(s);
        s = mid.Process(s);
        (void)high.Process(s);
    }
    for (size_t i = start; i < end; ++i) {
        float s = buf[i];
        s = low.Process(s);
        s = mid.Process(s);
        s = high.Process(s);
        buf[i] = dsp::SoftenPeaks(s);
    }

    if (!orig.empty()) {
        CrossfadeSpliceBoundaries(buf, orig, start, end);
    }
}

// ============================================================================
// WAV & MP3 Export
// ============================================================================

static bool ExportWavFile(const std::wstring& path, const std::vector<float>& samples, bool float32, std::wstring& errOut) {
    if (samples.empty()) {
        errOut = Text(L"There is no audio to export.", L"Нет записанного звука для экспорта.");
        return false;
    }

    const uint16_t channels = 1;
    const uint32_t sampleRate = kSampleRate;
    const uint16_t bitsPerSample = float32 ? 32 : 24;
    const uint16_t blockAlign = channels * (bitsPerSample / 8);
    const uint32_t byteRate = sampleRate * blockAlign;
    const uint64_t dataBytes64 = static_cast<uint64_t>(samples.size()) * blockAlign;
    if (dataBytes64 > 0x7FFFFFF0ULL) {
        errOut = Text(L"The recording exceeds the 2 GB limit for standard WAV.", L"Запись превышает лимит 2 ГБ для стандартного WAV.");
        return false;
    }
    const uint32_t dataBytes = static_cast<uint32_t>(dataBytes64);
    const uint16_t audioFormat = float32 ? 3 : 1; // 3 = IEEE_FLOAT, 1 = PCM
    const uint32_t fmtChunkSize = 16;
    const uint32_t factChunkBytes = float32 ? 12 : 0; // "fact" + size(4) + sampleLength(4)
    const uint32_t riffSize = 4 + (8 + fmtChunkSize) + factChunkBytes + (8 + dataBytes) + (dataBytes & 1);

    std::ofstream out(fs::path(path), std::ios::binary);
    if (!out) {
        errOut = Text(L"Could not create file: ", L"Не удалось создать файл: ") + path;
        return false;
    }

    auto writeU32 = [&](uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); };
    auto writeU16 = [&](uint16_t v) { out.write(reinterpret_cast<const char*>(&v), 2); };

    out.write("RIFF", 4);
    writeU32(riffSize);
    out.write("WAVE", 4);

    out.write("fmt ", 4);
    writeU32(fmtChunkSize);
    writeU16(audioFormat);
    writeU16(channels);
    writeU32(sampleRate);
    writeU32(byteRate);
    writeU16(blockAlign);
    writeU16(bitsPerSample);

    if (float32) {
        out.write("fact", 4);
        writeU32(4);
        writeU32(static_cast<uint32_t>(samples.size()));
    }

    out.write("data", 4);
    writeU32(dataBytes);

    if (float32) {
        out.write(reinterpret_cast<const char*>(samples.data()), dataBytes);
    } else {
        std::vector<uint8_t> pcmBytes(4096 * 3);
        uint32_t rng = 0x12345678u;
        auto nextRandUnit = [&]() -> float {
            rng = rng * 1664525u + 1013904223u;
            return static_cast<float>(rng >> 8) / static_cast<float>(0x01000000);
        };
        const size_t blockSamples = pcmBytes.size() / 3;
        for (size_t start = 0; start < samples.size() && out; start += blockSamples) {
            const size_t count = std::min(blockSamples, samples.size() - start);
            for (size_t i = 0; i < count; ++i) {
                const float sample = samples[start + i];
                const double value = std::isfinite(sample) ? std::clamp(sample, -1.0f, 1.0f) : 0.0;
                const double dither = static_cast<double>(nextRandUnit()) - nextRandUnit();
                const auto q = static_cast<uint32_t>(std::clamp(std::llround(value * 8388607.0 + dither),
                                                               -8388608LL, 8388607LL));
                pcmBytes[i * 3] = static_cast<uint8_t>(q);
                pcmBytes[i * 3 + 1] = static_cast<uint8_t>(q >> 8);
                pcmBytes[i * 3 + 2] = static_cast<uint8_t>(q >> 16);
            }
            out.write(reinterpret_cast<const char*>(pcmBytes.data()), count * 3);
        }
    }
    if (dataBytes & 1) out.put('\0');

    out.close();
    if (!out) {
        errOut = Text(L"Could not write the WAV file to disk.", L"Ошибка записи WAV-файла на диск.");
        return false;
    }
    return true;
}

static bool LoadWavFile(const std::wstring& path, std::vector<float>& outSamples) {
    outSamples.clear();
    std::ifstream in(fs::path(path), std::ios::binary | std::ios::ate);
    if (!in) return false;
    const auto fileSize = static_cast<std::streamoff>(in.tellg());
    if (fileSize < 12) return false;
    in.seekg(0);

    char riff[12]{};
    in.read(riff, 12);
    if (!in || std::memcmp(riff, "RIFF", 4) != 0 || std::memcmp(riff + 8, "WAVE", 4) != 0) {
        return false;
    }

    uint32_t riffSize = 0;
    std::memcpy(&riffSize, riff + 4, 4);
    const uint64_t riffEnd = 8ULL + riffSize;
    if (riffEnd < 12 || riffEnd > static_cast<uint64_t>(fileSize)) return false;

    uint16_t audioFormat = 0, channels = 0, bitsPerSample = 0, frameSize = 0;
    bool hasFmt = false;
    for (uint64_t offset = 12; offset + 8 <= riffEnd;) {
        char cid[4]{};
        uint32_t csize = 0;
        in.seekg(static_cast<std::streamoff>(offset));
        in.read(cid, 4);
        in.read(reinterpret_cast<char*>(&csize), 4);
        const uint64_t chunkEnd = offset + 8 + csize;
        if (!in || chunkEnd > riffEnd) return false;
        offset = chunkEnd + (csize & 1);

        if (std::memcmp(cid, "fmt ", 4) == 0) {
            uint8_t fmt[40]{};
            if (csize < 16 || !in.read(reinterpret_cast<char*>(fmt), std::min<uint32_t>(csize, sizeof(fmt)))) {
                return false;
            }
            uint32_t sampleRate = 0;
            std::memcpy(&audioFormat, fmt, 2);
            std::memcpy(&channels, fmt + 2, 2);
            std::memcpy(&sampleRate, fmt + 4, 4);
            std::memcpy(&frameSize, fmt + 12, 2);
            std::memcpy(&bitsPerSample, fmt + 14, 2);
            if (audioFormat == WAVE_FORMAT_EXTENSIBLE && csize >= sizeof(fmt) && fmt[16] >= 22) {
                GUID subFormat{};
                std::memcpy(&subFormat, fmt + 24, sizeof(subFormat));
                if (IsEqualGUID(subFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)) audioFormat = WAVE_FORMAT_IEEE_FLOAT;
                if (IsEqualGUID(subFormat, KSDATAFORMAT_SUBTYPE_PCM)) audioFormat = WAVE_FORMAT_PCM;
            }
            const bool isFloat = audioFormat == WAVE_FORMAT_IEEE_FLOAT && bitsPerSample == 32;
            const bool isPcm = audioFormat == WAVE_FORMAT_PCM &&
                               (bitsPerSample == 16 || bitsPerSample == 24 || bitsPerSample == 32);
            if ((!isFloat && !isPcm) || sampleRate != kSampleRate || !channels ||
                frameSize != channels * (bitsPerSample / 8)) return false;
            hasFmt = true;
        } else if (std::memcmp(cid, "data", 4) == 0 && hasFmt) {
            if (csize == 0 || csize > 0x7FFFFFF0ULL || csize % frameSize != 0) return false;
            const size_t numFrames = csize / frameSize;
            const size_t blockFrames = std::max<size_t>(1, 65536 / frameSize);
            const uint16_t bytesPerSample = bitsPerSample / 8;
            std::vector<uint8_t> buf(blockFrames * frameSize);
            std::vector<float> decoded(numFrames);
            for (size_t start = 0; start < numFrames; start += blockFrames) {
                const size_t count = std::min(blockFrames, numFrames - start);
                if (!in.read(reinterpret_cast<char*>(buf.data()), count * frameSize)) return false;
                for (size_t f = 0; f < count; ++f) {
                    double sum = 0.0;
                    for (uint16_t c = 0; c < channels; ++c) {
                        const uint8_t* p = buf.data() + f * frameSize + c * bytesPerSample;
                        float v = 0.0f;
                        if (audioFormat == WAVE_FORMAT_IEEE_FLOAT) {
                            std::memcpy(&v, p, 4);
                            if (!std::isfinite(v)) return false;
                        } else if (bitsPerSample == 16) {
                            int16_t q;
                            std::memcpy(&q, p, 2);
                            v = static_cast<float>(q) / 32768.0f;
                        } else if (bitsPerSample == 24) {
                            const int32_t q = p[0] + (p[1] << 8) + static_cast<int8_t>(p[2]) * 65536;
                            v = static_cast<float>(q) / 8388608.0f;
                        } else {
                            int32_t q;
                            std::memcpy(&q, p, 4);
                            v = static_cast<float>(static_cast<double>(q) / 2147483648.0);
                        }
                        sum += v;
                    }
                    decoded[start + f] = static_cast<float>(sum / channels);
                }
            }
            outSamples = std::move(decoded);
            return true;
        }
    }
    return false;
}

static fs::path CreateTempFileIn(const fs::path& directory) {
    wchar_t name[MAX_PATH]{};
    if (GetTempFileNameW(directory.c_str(), L"pdc", 0, name)) return name;
    return {};
}

static fs::path GetExecutableDir() {
    wchar_t exeBuf[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, exeBuf, MAX_PATH)) {
        return fs::path(exeBuf).parent_path();
    }
    return fs::current_path();
}

static fs::path GetAppDataDir() {
    static const fs::path cached = []() -> fs::path {
        const fs::path exeDir = GetExecutableDir();
        std::wstring lower = exeDir.wstring();
        std::transform(lower.begin(), lower.end(), lower.begin(), [](wchar_t ch) {
            return static_cast<wchar_t>(std::towlower(ch));
        });
        const bool packagedOrProgramFiles =
            lower.find(L"\\windowsapps\\") != std::wstring::npos ||
            lower.find(L"\\program files") != std::wstring::npos;

        if (!packagedOrProgramFiles) {
            std::error_code ec;
            const fs::path probeDir = exeDir / L"backups";
            fs::create_directories(probeDir, ec);
            if (!ec) {
                const fs::path probeFile = CreateTempFileIn(probeDir);
                if (!probeFile.empty()) {
                    fs::remove(probeFile, ec);
                    return exeDir;
                }
            }
        }

        wchar_t localAppData[MAX_PATH]{};
        const DWORD len = GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH);
        std::error_code ec;
        fs::path baseDir = (len > 0 && len < MAX_PATH) ? fs::path(localAppData) : fs::temp_directory_path(ec);
        fs::path appDir = baseDir / L"Podcaster";
        fs::create_directories(appDir, ec);
        return appDir;
    }();
    return cached;
}

static fs::path GetBackupDir() {
    return GetAppDataDir() / L"backups";
}

static LANGID InterfaceLangId() {
    return locale_ui::Current() == Language::Russian
        ? MAKELANGID(LANG_RUSSIAN, SUBLANG_RUSSIAN_RUSSIA)
        : MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US);
}

static fs::path LanguagePreferencePath() {
    return GetAppDataDir() / L"ui-language.txt";
}

static void LoadLanguagePreference() {
    std::error_code ec;
    fs::path path = LanguagePreferencePath();
    if (!fs::exists(path, ec)) {
        const fs::path bundled = GetExecutableDir() / L"ui-language.txt";
        if (fs::exists(bundled, ec)) {
            path = bundled;
        }
    }
    std::ifstream input(path, std::ios::binary);
    char buffer[8]{};
    input.read(buffer, sizeof(buffer));
    const std::string_view value(buffer, static_cast<size_t>(input.gcount()));
    locale_ui::Set(value == "ru" || value == "ru\n" || value == "ru\r\n" ? Language::Russian : Language::English);
    SetThreadUILanguage(InterfaceLangId());
}

static bool SaveLanguagePreference(Language language) {
    const auto target = LanguagePreferencePath();
    const auto temp = CreateTempFileIn(target.parent_path());
    if (temp.empty()) return false;
    std::ofstream output(temp, std::ios::binary);
    output << (language == Language::Russian ? "ru\n" : "en\n");
    output.close();
    if (output && MoveFileExW(temp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        return true;
    }
    std::error_code ec;
    fs::remove(temp, ec);
    return false;
}

static bool SaveScriptBackup(const std::wstring& rawText) {
    std::error_code ec;
    fs::path dir = GetBackupDir();
    fs::create_directories(dir, ec);
    if (ec) return false;
    const fs::path tmp = CreateTempFileIn(dir);
    if (tmp.empty()) return false;
    const fs::path dst = dir / L"autosave_script.txt";
    bool written = false;
    {
        std::ofstream out(tmp, std::ios::binary);
        std::string utf8 = WideToUtf8(rawText);
        out.write(utf8.data(), utf8.size());
        out.close();
        written = static_cast<bool>(out);
    }
    if (written && MoveFileExW(tmp.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        return true;
    }
    fs::remove(tmp, ec);
    return false;
}

static bool WriteWavAtomically(const fs::path& path, const std::vector<float>& samples, bool float32,
                               std::wstring& err, bool replace = true) {
    const fs::path tmp = CreateTempFileIn(path.parent_path());
    if (tmp.empty()) {
        err = Text(L"Could not create a temporary file next to the WAV.", L"Не удалось создать временный файл рядом с файлом WAV.");
        return false;
    }
    const DWORD flags = MOVEFILE_WRITE_THROUGH | (replace ? MOVEFILE_REPLACE_EXISTING : 0);
    if (ExportWavFile(tmp.wstring(), samples, float32, err)) {
        if (MoveFileExW(tmp.c_str(), path.c_str(), flags)) return true;
        err = Text(L"Could not save the WAV. Check whether another app has the file open.", L"Не удалось сохранить WAV. Проверьте, открыт ли файл в другой программе.");
    }
    std::error_code ec;
    fs::remove(tmp, ec);
    return false;
}

static std::wstring FindFfmpegPath() {
    wchar_t exeBuf[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, exeBuf, MAX_PATH)) {
        fs::path beside = fs::path(exeBuf).parent_path() / L"ffmpeg.exe";
        if (fs::exists(beside)) return beside.wstring();
    }

    wchar_t foundBuf[MAX_PATH]{};
    if (SearchPathW(nullptr, L"ffmpeg.exe", nullptr, MAX_PATH, foundBuf, nullptr) > 0) {
        return foundBuf;
    }

    wchar_t localAppData[MAX_PATH]{};
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH) > 0) {
        fs::path links = fs::path(localAppData) / L"Microsoft" / L"WinGet" / L"Links" / L"ffmpeg.exe";
        if (fs::exists(links)) return links.wstring();

        fs::path pkgs = fs::path(localAppData) / L"Microsoft" / L"WinGet" / L"Packages";
        std::error_code ec;
        if (fs::exists(pkgs, ec)) {
            for (const auto& entry : fs::directory_iterator(pkgs, ec)) {
                if (entry.path().filename().wstring().find(L"FFmpeg") != std::wstring::npos) {
                    for (const auto& sub : fs::recursive_directory_iterator(entry.path(), ec)) {
                        if (sub.path().filename() == L"ffmpeg.exe") {
                            return sub.path().wstring();
                        }
                    }
                }
            }
        }
    }
    return L"";
}

enum class FfmpegResult { Succeeded, LaunchFailed, TimedOut, Failed };

static FfmpegResult RunFfmpeg(const std::wstring& executable, const std::wstring& arguments, DWORD timeoutMs) {
    std::wstring command = L"\"" + executable + L"\" -nostdin -hide_banner -loglevel error " + arguments;
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        return FfmpegResult::LaunchFailed;
    }

    const DWORD result = WaitForSingleObject(pi.hProcess, timeoutMs);
    DWORD exitCode = 1;
    if (result == WAIT_OBJECT_0) {
        GetExitCodeProcess(pi.hProcess, &exitCode);
    } else {
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 5000);
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (result == WAIT_TIMEOUT) return FfmpegResult::TimedOut;
    return exitCode == 0 ? FfmpegResult::Succeeded : FfmpegResult::Failed;
}

static bool ImportAudioFile(const fs::path& path, std::vector<float>& samples, std::wstring& error) {
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) {
        error = Text(L"Could not open the audio file. Check that the file is accessible.", L"Не удалось открыть аудиофайл. Проверьте, что файл доступен.");
        return false;
    }
    if (LoadWavFile(path.wstring(), samples)) return true;

    const auto ffmpeg = FindFfmpegPath();
    if (ffmpeg.empty()) {
        error = Text(L"This audio file requires ffmpeg.exe. Standard 48 kHz WAV files can be opened directly.", L"Для этого аудиофайла нужен ffmpeg.exe. Стандартный WAV 48 кГц можно открыть напрямую.");
        return false;
    }
    const auto tempDir = fs::temp_directory_path(ec);
    const fs::path converted = ec ? fs::path{} : CreateTempFileIn(tempDir);
    if (converted.empty()) {
        error = Text(L"Could not create a temporary file for audio import.", L"Не удалось создать временный файл для импорта аудио.");
        return false;
    }
    // Reject output that reaches the WAV size cap rather than silently importing a partial track.
    constexpr uint64_t maxDecodedBytes = 0x7FFFF000ULL;
    const auto args = L"-y -xerror -protocol_whitelist file -i \"" + path.wstring() +
        L"\" -map 0:a:0 -ac 1 -ar 48000 -c:a pcm_f32le -fs " + std::to_wstring(maxDecodedBytes) +
        L" -f wav \"" + converted.wstring() + L"\"";
    const auto result = RunFfmpeg(ffmpeg, args, 180000);
    const auto decodedBytes = fs::file_size(converted, ec);
    const bool tooLarge = !ec && decodedBytes >= maxDecodedBytes;
    bool ok = false;
    try {
        ok = result == FfmpegResult::Succeeded && !tooLarge && LoadWavFile(converted.wstring(), samples);
    } catch (...) {
        fs::remove(converted, ec);
        throw;
    }
    fs::remove(converted, ec);
    if (!ok) {
        if (tooLarge) error = Text(L"The decoded track exceeds the 2 GB limit.", L"Дорожка превышает лимит 2 ГБ после декодирования.");
        else if (result == FfmpegResult::TimedOut) error = Text(L"Audio decoding timed out.", L"Истекло время ожидания декодирования аудио.");
        else if (result == FfmpegResult::LaunchFailed) error = Text(L"Could not start FFmpeg to import audio.", L"Не удалось запустить FFmpeg для импорта.");
        else error = Text(L"Could not read audio. The file is damaged or the format is unsupported.", L"Не удалось прочитать звук. Файл повреждён или его формат не поддерживается.");
    }
    return ok;
}

static bool ExportMp3File(const std::wstring& path, const std::vector<float>& samples, std::wstring& errOut) {
    if (samples.empty()) {
        errOut = Text(L"There is no audio to export.", L"Нет записанного звука для экспорта.");
        return false;
    }
    std::wstring ffmpeg = FindFfmpegPath();
    if (ffmpeg.empty()) {
        errOut = Text(L"ffmpeg.exe was not found. WAV export is available without FFmpeg.", L"Не найден ffmpeg.exe для кодирования MP3. Экспорт в WAV работает без FFmpeg.");
        return false;
    }

    std::error_code ec;
    const fs::path tempDir = fs::temp_directory_path(ec);
    const fs::path tempWav = ec ? fs::path{} : CreateTempFileIn(tempDir);
    if (tempWav.empty()) {
        errOut = Text(L"Could not create a temporary WAV for MP3 encoding.", L"Не удалось создать временный WAV для кодирования MP3.");
        return false;
    }
    if (!ExportWavFile(tempWav.wstring(), samples, true, errOut)) {
        fs::remove(tempWav, ec);
        return false;
    }

    const fs::path tempMp3 = CreateTempFileIn(fs::path(path).parent_path());
    if (tempMp3.empty()) {
        fs::remove(tempWav, ec);
        errOut = Text(L"Could not create a temporary file next to the MP3.", L"Не удалось создать временный файл рядом с файлом MP3.");
        return false;
    }
    const auto args = L"-y -i \"" + tempWav.wstring() +
        L"\" -codec:a libmp3lame -b:a 320k -ar 48000 -f mp3 \"" + tempMp3.wstring() + L"\"";
    const auto result = RunFfmpeg(ffmpeg, args, 120000);

    fs::remove(tempWav, ec);

    const auto bytes = fs::file_size(tempMp3, ec);
    if (result == FfmpegResult::Succeeded && !ec && bytes > 0) {
        if (MoveFileExW(tempMp3.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            return true;
        }
        errOut = Text(L"Could not save the MP3. Check whether another app has the file open.", L"Не удалось сохранить MP3. Проверьте, открыт ли файл в другой программе.");
    } else {
        if (result == FfmpegResult::TimedOut) errOut = Text(L"FFmpeg timed out while encoding MP3.", L"FFmpeg превысил время ожидания при кодировании MP3.");
        else if (result == FfmpegResult::LaunchFailed) errOut = Text(L"Could not start FFmpeg to export MP3.", L"Не удалось запустить FFmpeg для экспорта MP3.");
        else errOut = Text(L"FFmpeg could not create the MP3.", L"FFmpeg завершился с ошибкой при создании MP3.");
    }
    fs::remove(tempMp3, ec);
    return false;
}

// ============================================================================
// WASAPI Device Enumeration & Audio Threads
// ============================================================================

static std::vector<AudioDeviceInfo> EnumerateWasapiDevices(EDataFlow flow) {
    std::vector<AudioDeviceInfo> result;
    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  IID_PPV_ARGS(&enumerator));
    if (FAILED(hr) || !enumerator) return result;

    std::wstring defaultId;
    ComPtr<IMMDevice> defDev;
    if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(flow, eConsole, &defDev)) && defDev) {
        LPWSTR idStr = nullptr;
        if (SUCCEEDED(defDev->GetId(&idStr)) && idStr) {
            defaultId = idStr;
            CoTaskMemFree(idStr);
        }
    }

    ComPtr<IMMDeviceCollection> col;
    hr = enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &col);
    if (FAILED(hr) || !col) return result;

    UINT count = 0;
    col->GetCount(&count);
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> dev;
        if (FAILED(col->Item(i, &dev)) || !dev) continue;

        AudioDeviceInfo info;
        LPWSTR idStr = nullptr;
        if (SUCCEEDED(dev->GetId(&idStr)) && idStr) {
            info.id = idStr;
            CoTaskMemFree(idStr);
        }
        info.isDefault = (!defaultId.empty() && info.id == defaultId);

        ComPtr<IPropertyStore> props;
        if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props)) && props) {
            PROPVARIANT varName;
            PropVariantInit(&varName);
            if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &varName))) {
                if (varName.vt == VT_LPWSTR && varName.pwszVal) {
                    info.name = varName.pwszVal;
                }
                PropVariantClear(&varName);
            }
        }
        result.push_back(std::move(info));
    }

    std::sort(result.begin(), result.end(), [](const AudioDeviceInfo& a, const AudioDeviceInfo& b) {
        if (a.isDefault != b.isDefault) return a.isDefault > b.isDefault;
        return a.name < b.name;
    });
    return result;
}

static HRESULT OpenAudioClientWithFormat(
    IMMDevice* dev,
    bool isCapture,
    ComPtr<IAudioClient>& outClient,
    WAVEFORMATEX** outMixFmt,
    bool& outUsingNativeMix
) {
    outUsingNativeMix = false;
    HRESULT hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                               reinterpret_cast<void**>(outClient.ReleaseAndGetAddressOf()));
    if (FAILED(hr) || !outClient) return hr;

    WAVEFORMATEX desired{};
    desired.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    desired.nChannels = isCapture ? 1 : 2;
    desired.nSamplesPerSec = kSampleRate;
    desired.wBitsPerSample = 32;
    desired.nBlockAlign = desired.nChannels * 4;
    desired.nAvgBytesPerSec = desired.nSamplesPerSec * desired.nBlockAlign;
    desired.cbSize = 0;

    DWORD flags = AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    REFERENCE_TIME duration = 200000; // 20 ms
    hr = outClient->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, duration, 0, &desired, nullptr);
    if (SUCCEEDED(hr)) {
        return S_OK;
    }

    // Fallback: re-activate and use device native mix format
    hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                       reinterpret_cast<void**>(outClient.ReleaseAndGetAddressOf()));
    if (FAILED(hr) || !outClient) return hr;

    hr = outClient->GetMixFormat(outMixFmt);
    if (FAILED(hr) || !*outMixFmt) return hr;

    hr = outClient->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, duration, 0, *outMixFmt, nullptr);
    if (SUCCEEDED(hr)) {
        outUsingNativeMix = true;
    }
    return hr;
}

static bool IsFloatFormat(const WAVEFORMATEX* fmt) {
    if (fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return true;
    if (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE && fmt->cbSize >= 22) {
        const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(fmt);
        return IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) != 0;
    }
    return false;
}

static bool IsSupportedDeviceFormat(const WAVEFORMATEX* fmt) {
    if (!fmt || !fmt->nSamplesPerSec || !fmt->nChannels ||
        fmt->nBlockAlign != fmt->nChannels * (fmt->wBitsPerSample / 8)) return false;
    if (IsFloatFormat(fmt)) return fmt->wBitsPerSample == 32;
    const bool pcm = fmt->wFormatTag == WAVE_FORMAT_PCM ||
                     (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE && fmt->cbSize >= 22 &&
                      IsEqualGUID(reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(fmt)->SubFormat,
                                  KSDATAFORMAT_SUBTYPE_PCM));
    return pcm && (fmt->wBitsPerSample == 16 || fmt->wBitsPerSample == 24 || fmt->wBitsPerSample == 32);
}

static float ReadSampleAsFloat(const uint8_t* ptr, const WAVEFORMATEX* fmt) {
    if (IsFloatFormat(fmt) && fmt->wBitsPerSample == 32) {
        float v;
        std::memcpy(&v, ptr, 4);
        return v;
    }
    if (fmt->wBitsPerSample == 16) {
        int16_t v;
        std::memcpy(&v, ptr, 2);
        return static_cast<float>(v) / 32768.0f;
    }
    if (fmt->wBitsPerSample == 24) {
        const int32_t v = ptr[0] + (ptr[1] << 8) + static_cast<int8_t>(ptr[2]) * 65536;
        return static_cast<float>(v) / 8388608.0f;
    }
    if (fmt->wBitsPerSample == 32) {
        int32_t v;
        std::memcpy(&v, ptr, 4);
        return static_cast<float>(static_cast<double>(v) / 2147483648.0);
    }
    return 0.0f;
}

static void WriteFloatToDeviceFormat(uint8_t* ptr, const WAVEFORMATEX* fmt, float v) {
    v = std::clamp(v, -1.0f, 1.0f);
    if (IsFloatFormat(fmt) && fmt->wBitsPerSample == 32) {
        std::memcpy(ptr, &v, 4);
    } else if (fmt->wBitsPerSample == 16) {
        int16_t q = static_cast<int16_t>(std::llround(v * 32767.0));
        std::memcpy(ptr, &q, 2);
    } else if (fmt->wBitsPerSample == 24) {
        int32_t q = static_cast<int32_t>(std::llround(v * 8388607.0));
        ptr[0] = static_cast<uint8_t>(q & 0xFF);
        ptr[1] = static_cast<uint8_t>((q >> 8) & 0xFF);
        ptr[2] = static_cast<uint8_t>((q >> 16) & 0xFF);
    } else if (fmt->wBitsPerSample == 32) {
        int32_t q = static_cast<int32_t>(std::llround(v * 2147483647.0));
        std::memcpy(ptr, &q, 4);
    }
}

// ============================================================================
// Global Application State
// ============================================================================

struct AppState {
    HWND hwndMain = nullptr;
    HWND hwndEditor = nullptr;
    HWND hwndEditorEdit = nullptr;

    // Direct2D / DirectWrite
    ComPtr<ID2D1Factory> d2dFactory;
    ComPtr<IDWriteFactory> dwFactory;
    ComPtr<ID2D1HwndRenderTarget> rt;
    ComPtr<ID2D1SolidColorBrush> brush;
    ComPtr<ID2D1LinearGradientBrush> prompterEdge;

    ComPtr<IDWriteTextFormat> tfUi;
    ComPtr<IDWriteTextFormat> tfUiSmall;
    ComPtr<IDWriteTextFormat> tfUiBold;
    ComPtr<IDWriteTextFormat> tfMonoBig;
    ComPtr<IDWriteTextFormat> tfPrompter;
    ComPtr<IDWriteTextLayout> prompterLayout;
    float prompterLayoutWidth = 0.0f;
    float prompterFontSize = 26.0f;
    float prompterTotalHeight = 0.0f;

    // Audio devices
    std::vector<AudioDeviceInfo> inputDevices;
    std::vector<AudioDeviceInfo> outputDevices;
    std::wstring selectedInputId;
    std::wstring selectedOutputId;

    // Audio buffers & undo
    std::mutex audioMutex;
    std::shared_ptr<AudioTrack> raw = std::make_shared<AudioTrack>();
    std::shared_ptr<const AudioTrack> processed;
    std::vector<std::shared_ptr<AudioTrack>> undoStack;

    // Workers borrow immutable takes, then hand the result back to the window thread.
    std::mutex workMutex;
    std::condition_variable workWake;
    std::thread workThread;
    bool workStopping = false;
    std::optional<AudioWork> queuedEffects, queuedSilence;
    std::optional<AudioWorkResult> readyEffects, readySilence;
    std::deque<AudioBackup> queuedBackups;
    LPARAM backupRevision = 0;
    std::shared_ptr<std::atomic<bool>> cancelEffects, cancelSilence;
    uint64_t effectsRevision = 0, silenceRevision = 0;
    bool effectsPending = false;
    bool playWhenProcessed = false;
    bool silenceScanPending = false;
    bool silencePreviewReady = false;
    bool silenceApplyPending = false;
    std::vector<silence::Range> silenceRanges;
    std::atomic<float> masterGainDb{0.0f};
    std::atomic<float> masterGainLinear{1.0f};

    // Punch-in overwrite staging
    bool punchInActive = false;
    size_t punchInStartSample = 0;
    std::shared_ptr<const AudioTrack> punchInOriginal;
    size_t punchInTailStart = 0;
    bool captureHadDiscontinuity = false;

    // Transport atomics
    std::atomic<bool> isRecording{false};
    std::atomic<bool> captureReady{false};
    std::atomic<bool> isPlaying{false};
    std::atomic<bool> stopRequested{false};
    std::atomic<size_t> playheadSample{0};
    std::atomic<float> livePeakDb{-60.0f};
    std::atomic<float> liveRmsDb{-60.0f};
    std::atomic<float> peakHoldDb{-60.0f};
    std::chrono::steady_clock::time_point peakHoldTime = std::chrono::steady_clock::now();
    std::thread audioThread;
    LPARAM transportGeneration = 0;

    // Export worker owns a snapshot so recording and editing can continue.
    std::thread exportThread;
    std::wstring exportStatus;
    bool exportSucceeded = false;

    // A file operation works on a snapshot. The current session changes only on the UI thread.
    std::thread documentThread;
    DocumentResult documentResult;
    std::optional<DocumentResult> openAfterSave;
    fs::path documentPath;
    uint64_t documentRevision = 0;
    bool documentDirty = false;
    bool documentPromptOpen = false;
    bool closeAfterSave = false;

    // Selection & Waveform view
    bool hasSelection = false;
    size_t selStartSample = 0;
    size_t selEndSample = 0;
    bool isDraggingWave = false;
    size_t dragAnchorSample = 0;
    double viewStartSec = 0.0;
    double viewDurationSec = 0.0; // 0 = fit whole file (min 10s)
    double drawnViewStartSec = 0.0;
    double drawnViewDurationSec = 0.0;
    bool animatingWaveView = false;

    // Teleprompter state
    std::wstring prompterText;
    std::wstring prompterDisplayText;
    bool usingWelcomeScript = false;
    float prompterScrollY = 0.0f;
    float prompterScrollTarget = 0.0f;
    float prompterMaxScroll = 0.0f;
    float prompterAutoSpeed = 0.0f;
    float prompterSpeed = 35.0f;
    bool prompterAutoScroll = true;
    bool isDraggingPrompter = false;
    float prompterDragLastY = 0.0f;
    std::chrono::steady_clock::time_point lastFrameTime = std::chrono::steady_clock::now();
    bool animationsEnabled = true;
    bool motionTimerRunning = false;
    float recordPresence = 0.0f;
    float recordPulsePhase = 0.0f;
    float drawnPeakFraction = 0.0f;
    float drawnRmsFraction = 0.0f;

    // Non-destructive effects state (toggles applied on rawSamples -> samples)
    EffectSettings effectSettings;
    EffectSettings appliedEffectSettings;
    bool wavFloat32 = true;

    // Status message
    std::wstring statusText;
    bool statusIsError = false;

    // Hover state & Hitboxes
    int hoveredControlId = -1;
    bool trackingMouseLeave = false;
    bool fileMenuOpen = false;
    bool draggingMasterGain = false;
    bool draggingSilenceThreshold = false;
    bool silenceToolsOpen = false;
    float silenceThresholdDb = -42.0f;

    struct HitRect {
        int id;
        D2D1_RECT_F rect;
    };
    std::vector<HitRect> hitRects;
    D2D1_RECT_F rectPrompterBox{};
    D2D1_RECT_F rectMicCenter{};
    D2D1_RECT_F rectWaveArea{};
    D2D1_RECT_F rectMasterGainArea{};
    D2D1_RECT_F rectMasterGainTrack{};
    D2D1_RECT_F rectSilenceThresholdTrack{};
} g;

enum ControlId {
    ID_BTN_INPUT_DEV = 100,
    ID_BTN_OUTPUT_DEV,
    ID_BTN_REFRESH_DEV,
    ID_BTN_UNDO,
    ID_BTN_CLEAR_AUDIO,

    ID_BTN_OPEN_TEXT,
    ID_BTN_PASTE_TEXT,
    ID_BTN_EDIT_TEXT,
    ID_BTN_LOAD_LATEST_EP,
    ID_BTN_PROMPTER_TOP,
    ID_BTN_AUTOSCROLL,
    ID_BTN_SPEED_DOWN,
    ID_BTN_SPEED_UP,
    ID_BTN_FONT_DOWN,
    ID_BTN_FONT_UP,

    ID_BTN_MIC_TOGGLE,
    ID_BTN_REC_STOP,
    ID_BTN_PLAY_STOP,
    ID_BTN_DEL_SEL,
    ID_BTN_REWIND_START,
    ID_BTN_JUMP_LOUD,

    ID_BTN_WAVE_ZOOM_IN,
    ID_BTN_WAVE_ZOOM_OUT,
    ID_BTN_WAVE_ZOOM_ALL,
    ID_BTN_WAVE_DESELECT,

    ID_BTN_FX_CLEANUP,
    ID_BTN_FX_FADE_IN,
    ID_BTN_FX_FADE_OUT,
    ID_BTN_FX_ECHO,
    ID_BTN_EQ_LOW_DEC,
    ID_BTN_EQ_LOW_INC,
    ID_BTN_EQ_MID_DEC,
    ID_BTN_EQ_MID_INC,
    ID_BTN_EQ_HIGH_DEC,
    ID_BTN_EQ_HIGH_INC,
    ID_BTN_FX_EQ_APPLY,
    ID_BTN_FX_EQ_PRESET,

    ID_BTN_WAV_BITDEPTH,
    ID_BTN_EXPORT_WAV,
    ID_BTN_EXPORT_MP3,

    ID_BTN_FX_HIGHPASS,
    ID_BTN_FX_GATE,
    ID_BTN_FX_DEESSER,
    ID_BTN_FX_COMPRESSOR,
    ID_BTN_FX_LIMITER,

    ID_WAVE_MASTER_GAIN,
    ID_BTN_MASTER_GAIN_RESET,
    ID_BTN_SILENCE_TOOLS,
    ID_SILENCE_THRESHOLD,
    ID_BTN_SILENCE_APPLY,
    ID_BTN_SILENCE_CLOSE,

    ID_BTN_FILE_MENU,
    ID_BTN_LANGUAGE
};

// ============================================================================
// State Helpers
// ============================================================================

static void SetStatus(const std::wstring& msg, bool isError = false) {
    g.statusText = msg;
    g.statusIsError = isError;
    if (g.hwndMain) InvalidateRect(g.hwndMain, nullptr, FALSE);
}

static void UpdateDocumentTitle() {
    if (!g.hwndMain) return;
    std::wstring title = Text(L"Podcaster", L"Подкастер");
    if (!g.documentPath.empty()) title += L" — " + g.documentPath.filename().wstring();
    else title += Text(L" — Recording studio and teleprompter", L" — студия записи и телесуфлёр");
    if (g.documentDirty) title += L" *";
    SetWindowTextW(g.hwndMain, title.c_str());
}

static void MarkDocumentChanged() {
    ++g.documentRevision;
    if (!g.documentDirty) {
        g.documentDirty = true;
        UpdateDocumentTitle();
    }
}

static bool FileOperationPending() {
    return g.documentThread.joinable() || g.documentPromptOpen;
}

static void EnsureMotionTimer() {
    if (g.hwndMain && !g.motionTimerRunning) {
        g.lastFrameTime = std::chrono::steady_clock::now();
        g.motionTimerRunning = SetTimer(g.hwndMain, 1, 15, nullptr) != 0;
    }
}

static void RefreshAnimationPreference() {
    BOOL enabled = TRUE;
    if (SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &enabled, 0)) {
        g.animationsEnabled = enabled != FALSE;
    }
}

template <typename T>
static T EaseToward(T value, T target, float dt, float timeConstant, T tolerance) {
    if (std::abs(target - value) <= tolerance) return target;
    return std::lerp(value, target, static_cast<T>(-std::expm1(-dt / timeConstant)));
}

static void ScrollPrompter(float value, bool absolute = false) {
    const float target = absolute ? value : g.prompterScrollTarget + value;
    g.prompterScrollTarget = std::clamp(target, 0.0f, g.prompterMaxScroll);
    if (!g.animationsEnabled) g.prompterScrollY = g.prompterScrollTarget;
    EnsureMotionTimer();
    InvalidateRect(g.hwndMain, nullptr, FALSE);
}

static void PushUndoLocked() {
    if (g.undoStack.size() >= kMaxUndoSteps) {
        g.undoStack.erase(g.undoStack.begin());
    }
    g.undoStack.push_back(g.raw);
}

static const AudioTrack& CurrentTrackLocked() {
    return g.processed ? *g.processed : *g.raw;
}

static bool UpdateWaveCache(AudioTrack& track, size_t firstNewSample = 0,
                            const std::atomic<bool>* cancel = nullptr) {
    const size_t total = track.samples.size();
    const size_t numBuckets = (total + kWaveformBucketSize - 1) / kWaveformBucketSize;
    const size_t startBucket = firstNewSample / kWaveformBucketSize;
    if (!firstNewSample) track.peak = 0.0f;
    track.wave.resize(numBuckets);
    for (size_t b = startBucket; b < numBuckets; ++b) {
        if (cancel && (b % 4096 == 0) && cancel->load()) return false;
        const size_t s0 = b * kWaveformBucketSize;
        const size_t s1 = std::min(total, s0 + kWaveformBucketSize);
        float mn = track.samples[s0], mx = track.samples[s0];
        double sumSq = 0.0;
        for (size_t i = s0; i < s1; ++i) {
            const float v = track.samples[i];
            if (v < mn) mn = v;
            if (v > mx) mx = v;
            sumSq += static_cast<double>(v) * v;
        }
        track.wave[b] = {mn, mx, static_cast<float>(std::sqrt(sumSq / (s1 - s0)))};
        track.peak = std::max({track.peak, -mn, mx});
    }
    return true;
}

static float WaveformPeakAbs() {
    std::lock_guard<std::mutex> lock(g.audioMutex);
    return CurrentTrackLocked().peak;
}

static float WaveformOutputCeiling() {
    std::lock_guard<std::mutex> lock(g.audioMutex);
    return CurrentTrackLocked().limiter ? dsp::kLimiterCeiling : 1.0f;
}

static float ApplyOutputGain(float sample, float gain, float ceiling) {
    return std::clamp(sample * gain, -ceiling, ceiling);
}

static void SetMasterGainDb(float value, bool markChanged = true) {
    const float db = std::clamp(value, kMinMasterGainDb, kMaxMasterGainDb);
    if (db != g.masterGainDb.load() && markChanged) MarkDocumentChanged();
    g.masterGainDb.store(db);
    g.masterGainLinear.store(std::pow(10.0f, db / 20.0f));
    InvalidateRect(g.hwndMain, nullptr, FALSE);
}

static void GetNormalizedSelection(size_t total, size_t& s0, size_t& s1) {
    if (g.hasSelection && g.selStartSample != g.selEndSample) {
        s0 = std::min(g.selStartSample, g.selEndSample);
        s1 = std::max(g.selStartSample, g.selEndSample);
        s0 = std::min(s0, total);
        s1 = std::min(s1, total);
    } else {
        s0 = 0;
        s1 = total;
    }
}

static bool HasEffects(const EffectSettings& fx) {
    return fx.cleanup || fx.fadeIn || fx.fadeOut || fx.echo || fx.equalizer ||
           fx.highPass || fx.gate || fx.deEsser || fx.compressor || fx.limiter;
}

static bool ProcessEffects(std::vector<float>& samples, const EffectSettings& fx,
                           const std::atomic<bool>* cancel = nullptr) {
    const auto stopped = [&] { return cancel && cancel->load(); };
    const size_t total = samples.size();
    if (total == 0 || stopped()) return !stopped();
    if (fx.cleanup) {
        ApplyVoiceCleanupDsp(samples, 0, total);
    } else {
        if (fx.highPass) dsp::RemoveRumble(samples, kSampleRate);
        if (!stopped() && fx.gate) dsp::ApplyNoiseGate(samples, kSampleRate);
    }
    if (!stopped() && fx.equalizer) ApplyEqualizerDsp(samples, 0, total, fx.eq);
    if (!stopped() && fx.deEsser) dsp::ApplyDeEsser(samples, kSampleRate);
    if (!stopped() && fx.compressor) dsp::ApplyCompressor(samples, kSampleRate);
    if (!stopped() && fx.echo) ApplyEchoDsp(samples, 0, total);
    const size_t fadeLength = std::min(total, static_cast<size_t>(kSampleRate * 1.5));
    if (!stopped() && fx.fadeIn) ApplyFadeInDsp(samples, 0, fadeLength);
    if (!stopped() && fx.fadeOut) ApplyFadeOutDsp(samples, total - fadeLength, total);
    if (!stopped() && fx.limiter) dsp::ApplyLimiter(samples, kSampleRate);
    return !stopped();
}

static AudioWorkResult RunAudioWork(const AudioWork& job) {
    AudioWorkResult result{job.kind, job.revision};
    const auto& source = job.source->samples;
    const auto* cancel = job.cancelled.get();
    const silence::Range scope{0, source.size()};
    if (job.kind == AudioWorkKind::SilencePreview) {
        silence::Settings settings;
        settings.thresholdDb = job.thresholdDb;
        auto cuts = silence::Analyze(source, scope, kSampleRate, settings, cancel);
        if (cuts) {
            result.cuts = std::move(*cuts);
            result.succeeded = true;
        }
        return result;
    }
    result.track = std::make_shared<AudioTrack>();
    if (job.kind == AudioWorkKind::RemoveSilence) {
        auto edited = silence::Apply(source, scope, job.cuts, kSampleRate, cancel);
        if (!edited) return result;
        result.track->samples = std::move(*edited);
    } else {
        result.track->samples = source;
        result.track->limiter = job.effects.limiter;
        result.effects = job.effects;
        if (!ProcessEffects(result.track->samples, job.effects, cancel)) return result;
    }
    result.succeeded = UpdateWaveCache(*result.track, 0, cancel);
    return result;
}

static bool SaveAudioBackup(const AudioTrack& track, bool saveTimestampedTake);

static void AudioWorkLoop(HWND hwnd) {
    for (;;) {
        AudioWork job;
        std::optional<AudioBackup> backup;
        {
            std::unique_lock<std::mutex> lock(g.workMutex);
            g.workWake.wait(lock, [] {
                return g.workStopping || !g.queuedBackups.empty() || g.queuedEffects || g.queuedSilence;
            });
            if (!g.queuedBackups.empty()) {
                backup = std::move(g.queuedBackups.front());
                g.queuedBackups.pop_front();
            } else {
                if (g.workStopping) return;
                auto& next = g.queuedSilence ? g.queuedSilence : g.queuedEffects;
                job = std::move(*next);
                next.reset();
            }
        }
        if (backup) {
            bool saved = false;
            try {
                saved = SaveAudioBackup(*backup->source, backup->timestamped);
            } catch (const std::exception&) {
            }
            if (!saved) PostMessageW(hwnd, kAudioBackupFailedMessage, 0, backup->revision);
            continue;
        }
        AudioWorkResult result{job.kind, job.revision};
        try {
            if (!job.cancelled->load()) result = RunAudioWork(job);
        } catch (const std::exception&) {
            result.succeeded = false;
        }
        {
            std::lock_guard<std::mutex> lock(g.workMutex);
            if (g.workStopping || job.cancelled->load()) continue;
            auto& ready = job.kind == AudioWorkKind::Effects ? g.readyEffects : g.readySilence;
            ready = std::move(result);
        }
        PostMessageW(hwnd, kAudioWorkReadyMessage, 0, 0);
    }
}

static void EnsureAudioWorker() {
    if (!g.workThread.joinable()) g.workThread = std::thread(AudioWorkLoop, g.hwndMain);
}

static void CancelAudioWork(bool effects) {
    auto& cancel = effects ? g.cancelEffects : g.cancelSilence;
    if (cancel) cancel->store(true);
    std::lock_guard<std::mutex> lock(g.workMutex);
    (effects ? g.queuedEffects : g.queuedSilence).reset();
    (effects ? g.readyEffects : g.readySilence).reset();
}

static bool SubmitAudioWork(AudioWork job) {
    if (g.workStopping) return false;
    try {
        const bool effects = job.kind == AudioWorkKind::Effects;
        auto& cancel = effects ? g.cancelEffects : g.cancelSilence;
        job.cancelled = std::make_shared<std::atomic<bool>>(false);
        cancel = job.cancelled;
        EnsureAudioWorker();
        {
            std::lock_guard<std::mutex> lock(g.workMutex);
            (effects ? g.queuedEffects : g.queuedSilence) = std::move(job);
        }
        g.workWake.notify_one();
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

static bool EffectsPending() {
    return g.effectsPending;
}

static void QueueEffectsLocked() {
    CancelAudioWork(true);
    const auto revision = ++g.effectsRevision;
    g.effectsPending = false;
    const auto fx = g.effectSettings;
    if (g.raw->samples.empty() || !HasEffects(fx)) {
        g.processed.reset();
        return;
    }
    AudioWork job{AudioWorkKind::Effects, revision, g.raw};
    job.effects = fx;
    if (!g.hwndMain) {
        // The CLI self-test has no window loop.
        auto result = RunAudioWork(job);
        if (result.succeeded) g.processed = std::move(result.track);
        return;
    }
    g.effectsPending = SubmitAudioWork(std::move(job));
    if (!g.effectsPending && !g.workStopping) {
        g.effectSettings = g.processed ? g.appliedEffectSettings : EffectSettings{};
        SetStatus(Text(L"Could not start processing effects.", L"Не удалось запустить обработку эффектов."), true);
    }
}

static void CancelSilencePreview() {
    CancelAudioWork(false);
    ++g.silenceRevision;
    g.silenceScanPending = false;
    g.silencePreviewReady = false;
    g.silenceApplyPending = false;
    g.silenceRanges.clear();
    if (g.hwndMain) InvalidateRect(g.hwndMain, nullptr, FALSE);
}

static void RequestSilencePreview(float thresholdDb) {
    if (g.isRecording.load() || g.silenceApplyPending) return;
    CancelSilencePreview();
    std::shared_ptr<const AudioTrack> source;
    {
        std::lock_guard<std::mutex> lock(g.audioMutex);
        source = g.raw;
    }
    if (source->samples.empty()) return;
    AudioWork job{AudioWorkKind::SilencePreview, g.silenceRevision, std::move(source)};
    job.thresholdDb = thresholdDb;
    g.silenceScanPending = SubmitAudioWork(std::move(job));
    if (!g.silenceScanPending) SetStatus(Text(L"Could not start scanning for pauses.", L"Не удалось запустить поиск пауз."), true);
}

static void CloseSilenceTools() {
    g.silenceToolsOpen = false;
    if (g.draggingSilenceThreshold) {
        g.draggingSilenceThreshold = false;
        if (GetCapture() == g.hwndMain) ReleaseCapture();
    }
    CancelSilencePreview();
}

static bool SaveAudioBackup(const AudioTrack& track, bool saveTimestampedTake) {
    std::error_code ec;
    fs::path dir = GetBackupDir();
    fs::create_directories(dir, ec);
    if (ec) return false;
    fs::path latest = dir / L"autosave_latest.wav";

    const auto& raw = track.samples;
    if (raw.empty()) {
        fs::remove(latest, ec);
        return !ec;
    }

    std::wstring err;
    const bool savedLatest = WriteWavAtomically(latest, raw, true, err);

    if (saveTimestampedTake) {
        SYSTEMTIME st{};
        GetLocalTime(&st);
        static unsigned sequence = 0;
        wchar_t nameBuf[80];
        swprintf_s(nameBuf, L"take_%04d%02d%02d_%02d%02d%02d_%03d_%lu_%010u.wav",
                   st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
                   st.wMilliseconds, GetCurrentProcessId(), ++sequence);
        fs::path takePath = dir / nameBuf;
        if (!WriteWavAtomically(takePath, raw, true, err, false)) return false;

        // Keep only the 10 newest take_*.wav files
        std::vector<fs::path> takes;
        for (auto it = fs::directory_iterator(dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
            const auto& entry = *it;
            if (!entry.is_regular_file(ec)) continue;
            std::wstring fn = entry.path().filename().wstring();
            if (fn.rfind(L"take_", 0) == 0 && entry.path().extension() == L".wav") {
                takes.push_back(entry.path());
            }
        }
        if (takes.size() > 10) {
            std::sort(takes.begin(), takes.end(), [](const fs::path& a, const fs::path& b) {
                return a.filename().wstring() > b.filename().wstring();
            });
            for (size_t i = 10; i < takes.size(); ++i) {
                fs::remove(takes[i], ec);
            }
        }
    }
    return savedLatest;
}

static bool QueueAutoBackupLocked(bool saveTimestampedTake) {
    if (!g.hwndMain) return SaveAudioBackup(*g.raw, saveTimestampedTake);
    try {
        EnsureAudioWorker();
        AudioBackup job{g.raw, saveTimestampedTake, ++g.backupRevision};
        {
            std::lock_guard<std::mutex> lock(g.workMutex);
            if (!g.queuedBackups.empty() && !g.queuedBackups.back().timestamped && !saveTimestampedTake) {
                g.queuedBackups.back() = std::move(job);
            } else {
                g.queuedBackups.push_back(std::move(job));
            }
        }
        g.workWake.notify_one();
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

static void RefreshDevices(bool initial = false) {
    g.inputDevices = EnumerateWasapiDevices(eCapture);
    g.outputDevices = EnumerateWasapiDevices(eRender);

    auto existsIn = [](const std::vector<AudioDeviceInfo>& list, const std::wstring& id) {
        for (const auto& d : list) {
            if (d.id == id) return true;
        }
        return false;
    };

    if (initial || g.selectedInputId.empty() || !existsIn(g.inputDevices, g.selectedInputId)) {
        g.selectedInputId.clear();
        // Auto-select Samson G-Track Pro if connected
        for (const auto& d : g.inputDevices) {
            std::wstring lower = d.name;
            for (auto& ch : lower) ch = towlower(ch);
            if (lower.find(L"samson") != std::wstring::npos || lower.find(L"g-track") != std::wstring::npos) {
                g.selectedInputId = d.id;
                break;
            }
        }
        if (g.selectedInputId.empty() && !g.inputDevices.empty()) {
            g.selectedInputId = g.inputDevices.front().id;
        }
    }

    if (initial || g.selectedOutputId.empty() || !existsIn(g.outputDevices, g.selectedOutputId)) {
        g.selectedOutputId.clear();
        if (!g.outputDevices.empty()) {
            g.selectedOutputId = g.outputDevices.front().id;
        }
    }
}

static std::wstring AudioDeviceDisplayName(const AudioDeviceInfo& device) {
    return device.name.empty() ? Text(L"Audio device", L"Аудиоустройство") : device.name;
}

static std::wstring GetDeviceDisplayName(const std::vector<AudioDeviceInfo>& list, const std::wstring& id, const std::wstring& fallback) {
    for (const auto& d : list) {
        if (d.id == id) return AudioDeviceDisplayName(d);
    }
    return list.empty() ? fallback : AudioDeviceDisplayName(list.front());
}

static void RebuildPrompterTextFormat() {
    if (!g.dwFactory) return;
    g.tfPrompter.Reset();
    g.dwFactory->CreateTextFormat(
        L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, g.prompterFontSize, Text(L"en-US", L"ru-RU"), &g.tfPrompter
    );
    if (g.tfPrompter) {
        g.tfPrompter->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        g.tfPrompter->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_PROPORTIONAL, 1.38f, 1.05f);
    }
    g.prompterLayout.Reset();
}

static bool SetPrompterText(const std::wstring& rawText, bool saveBackup = true) {
    g.usingWelcomeScript = false;
    if (saveBackup && rawText != g.prompterText) MarkDocumentChanged();
    g.prompterText = rawText;
    g.prompterDisplayText = CleanMarkdownForPrompter(rawText);
    g.prompterLayout.Reset();
    g.prompterScrollY = g.prompterScrollTarget = 0.0f;
    if (g.hwndMain) InvalidateRect(g.hwndMain, nullptr, FALSE);
    return !saveBackup || SaveScriptBackup(rawText);
}

static void SetWelcomeScript() {
    SetPrompterText(Text(
        L"# Podcaster\n\n"
        L"Use File or Ctrl+O to open audio or a project. Ctrl+S saves the project.\n\n"
        L"Choose Script, Paste, or Latest episode to load text into the teleprompter.\n\n"
        L"◆ Recording shortcuts\n"
        L"1. Press Space to start or stop recording.\n"
        L"2. Press ← or → to adjust the scroll speed.\n"
        L"3. To replace a mistake, click the waveform or select a region, then press Space.\n"
        L"4. Press Ctrl+Z to undo the last edit.\n",
        L"# Подкастер «Без галстука»\n\n"
        L"Чтобы открыть запись или проект, используйте меню «Файл» или Ctrl+O. Ctrl+S сохраняет проект.\n\n"
        L"Для телесуфлёра нажмите «Сценарий», «Вставить» или «Свежий выпуск».\n\n"
        L"◆ Как быстро записывать\n"
        L"1. Пробел начинает или останавливает запись.\n"
        L"2. Стрелки ← / → меняют скорость прокрутки текста.\n"
        L"3. Сбились? Кликните по волне или выделите фрагмент и нажмите Пробел, чтобы перезаписать звук.\n"
        L"4. Ctrl+Z отменяет последнюю правку.\n"), false);
    g.usingWelcomeScript = true;
}

static void RebuildInterfaceTextFormats() {
    auto makeFormat = [&](const wchar_t* font, float size, DWRITE_FONT_WEIGHT weight,
                          ComPtr<IDWriteTextFormat>& out) {
        g.dwFactory->CreateTextFormat(font, nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                      size, Text(L"en-US", L"ru-RU"), &out);
        if (!out) return;
        out->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        ComPtr<IDWriteInlineObject> trimSign;
        if (SUCCEEDED(g.dwFactory->CreateEllipsisTrimmingSign(out.Get(), &trimSign)) && trimSign) {
            DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
            out->SetTrimming(&trim, trimSign.Get());
        }
    };
    makeFormat(L"Segoe UI", 14.0f, DWRITE_FONT_WEIGHT_NORMAL, g.tfUi);
    makeFormat(L"Segoe UI", 12.5f, DWRITE_FONT_WEIGHT_NORMAL, g.tfUiSmall);
    makeFormat(L"Segoe UI", 14.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, g.tfUiBold);
    makeFormat(L"Consolas", 26.0f, DWRITE_FONT_WEIGHT_BOLD, g.tfMonoBig);
    RebuildPrompterTextFormat();
}

static void ChangeInterfaceLanguage(Language language) {
    if (locale_ui::Current() != language) {
        locale_ui::Set(language);
        SetThreadUILanguage(InterfaceLangId());
        RebuildInterfaceTextFormats();
        if (g.usingWelcomeScript) SetWelcomeScript();
        UpdateDocumentTitle();
    }
    const bool saved = SaveLanguagePreference(language);
    SetStatus(saved ? Text(L"Interface language set to English.", L"Выбран русский язык интерфейса.")
                    : Text(L"Interface language changed. Could not save the preference.",
                           L"Язык интерфейса изменён. Не удалось сохранить настройку."), !saved);
}

static void LoadScriptIntoPrompter(const std::wstring& text, const std::wstring& status) {
    const bool saved = SetPrompterText(text);
    SetStatus(saved ? status : Text(L"Script loaded. Could not save the script backup.", L"Текст открыт. Не удалось сохранить резервную копию сценария."), !saved);
}

// ============================================================================
// Transport: Recording & Playback
// ============================================================================

enum class TransportResult : WPARAM { PlaybackFinished, CaptureFailed, PlaybackFailed, CaptureStarted, CaptureCancelled };

static void StopAudioTransport() {
    bool wasRecording = g.isRecording.load();
    bool wasPlaying = g.isPlaying.load();

    g.playWhenProcessed = false;
    ++g.transportGeneration; // Ignore a completion queued by a previous transport.
    g.stopRequested.store(true);
    if (g.audioThread.joinable()) {
        g.audioThread.join();
    }
    g.isRecording.store(false);
    g.captureReady.store(false);
    g.isPlaying.store(false);
    g.stopRequested.store(false);
    g.livePeakDb.store(-60.0f);
    g.liveRmsDb.store(-60.0f);
    g.peakHoldDb.store(-60.0f);

    if (wasRecording) {
        std::lock_guard<std::mutex> lk(g.audioMutex);
        if (g.punchInActive) {
            auto& raw = g.raw->samples;
            const size_t takeEnd = raw.size();
            const size_t takeLength = takeEnd - g.punchInStartSample;
            const size_t fadeLimit = std::min<size_t>(kSampleRate / 200, takeLength / 2);
            if (g.punchInStartSample > 0) {
                const float previous = raw[g.punchInStartSample - 1];
                for (size_t i = 0; i < fadeLimit; ++i) {
                    const float mix = static_cast<float>(i + 1) / static_cast<float>(fadeLimit + 1);
                    float& sample = raw[g.punchInStartSample + i];
                    sample = std::lerp(previous, sample, mix);
                }
            }
            const auto& previousTrack = g.punchInOriginal->samples;
            if (g.punchInTailStart < previousTrack.size()) {
                const float next = previousTrack[g.punchInTailStart];
                for (size_t i = 0; i < fadeLimit; ++i) {
                    const float mix = static_cast<float>(i + 1) / static_cast<float>(fadeLimit + 1);
                    float& sample = raw[takeEnd - 1 - i];
                    sample = std::lerp(next, sample, mix);
                }
                raw.insert(raw.end(), previousTrack.begin() + g.punchInTailStart, previousTrack.end());
            }
            g.punchInOriginal.reset();
            g.playheadSample.store(takeEnd);
            g.punchInActive = false;
            g.hasSelection = false;
            // Restore the exact waveform peak after the splice altered the fade edges.
            UpdateWaveCache(*g.raw, g.punchInStartSample);
            g.raw->peak = 0.0f;
            for (const auto& bucket : g.raw->wave) {
                g.raw->peak = std::max({g.raw->peak, -bucket.minVal, bucket.maxVal});
            }
            QueueEffectsLocked();
            MarkDocumentChanged();
            if (!QueueAutoBackupLocked(true)) {
                SetStatus(Text(L"The take is still in memory. The backup failed. Export a WAV to save it.", L"Запись в памяти. Не удалось создать резервную копию, экспортируйте WAV."), true);
            } else if (g.captureHadDiscontinuity) {
                SetStatus(Text(L"Recording stopped. The microphone reported missing audio. Check the take.", L"Запись остановлена. Микрофон сообщил о пропуске звука, проверьте дубль."), true);
            } else {
                SetStatus(Text(L"Recording stopped.", L"Запись остановлена."));
            }
        } else {
            if (!g.processed) QueueEffectsLocked();
            SetStatus(Text(L"The microphone did not send audio. The track is unchanged.", L"Микрофон не успел передать звук. Дорожка сохранена без изменений."));
        }
    } else if (wasPlaying) {
        SetStatus(Text(L"Playback stopped.", L"Воспроизведение остановлено."));
    }

    if (g.hwndMain) InvalidateRect(g.hwndMain, nullptr, FALSE);
}

static void RequestTransportStop() {
    if (g.isRecording.load() && !g.captureReady.load()) {
        std::lock_guard<std::mutex> lk(g.audioMutex);
        if (!g.captureReady.load()) {
            g.stopRequested.store(true);
            SetStatus(Text(L"Canceling microphone connection…", L"Отмена подключения к микрофону…"));
            return;
        }
    }
    StopAudioTransport();
}

static void StartRecording() {
    if (g.isRecording.load()) return;
    if (FileOperationPending()) {
        SetStatus(Text(L"Wait for the file operation to finish.", L"Дождитесь завершения операции с файлом."));
        return;
    }
    StopAudioTransport();

    if (g.inputDevices.empty()) {
        RefreshDevices();
        if (g.inputDevices.empty()) {
            SetStatus(Text(L"No microphone found. Connect one and click ↻.", L"Не найден ни один микрофон! Подключите устройство и нажмите ↻."), true);
            return;
        }
    }

    CloseSilenceTools();
    CancelAudioWork(true);
    ++g.effectsRevision;
    if (g.effectsPending) g.processed.reset();
    g.effectsPending = false;

    size_t takeStart = 0, replacedEnd = 0;
    {
        std::lock_guard<std::mutex> lk(g.audioMutex);
        const size_t total = g.raw->samples.size();
        size_t s0 = 0, s1 = 0;
        GetNormalizedSelection(total, s0, s1);
        if (g.hasSelection && s1 > s0) {
            takeStart = s0;
            replacedEnd = s1;
        } else {
            takeStart = std::min(g.playheadSample.load(), total);
            replacedEnd = total;
        }
    }

    std::wstring devId = g.selectedInputId;
    g.stopRequested.store(false);
    g.captureHadDiscontinuity = false;
    g.isRecording.store(true);
    g.lastFrameTime = std::chrono::steady_clock::now();

    g.audioThread = std::thread([devId, takeStart, replacedEnd, hwnd = g.hwndMain, generation = g.transportGeneration]() {
        HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        DWORD taskIdx = 0;
        HANDLE hMmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIdx);

        ComPtr<IMMDeviceEnumerator> enumerator;
        ComPtr<IMMDevice> dev;
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
        if (SUCCEEDED(hr) && enumerator) {
            hr = devId.empty() ? enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &dev)
                               : enumerator->GetDevice(devId.c_str(), &dev);
        }

        ComPtr<IAudioClient> client;
        ComPtr<IAudioCaptureClient> capture;
        WAVEFORMATEX* mixFmt = nullptr;
        bool usingNativeMix = false;

        if (dev) {
            hr = OpenAudioClientWithFormat(dev.Get(), true, client, &mixFmt, usingNativeMix);
            if (SUCCEEDED(hr) && client) {
                hr = client->GetService(IID_PPV_ARGS(&capture));
            }
        }

        WAVEFORMATEX fallbackFmt{};
        fallbackFmt.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
        fallbackFmt.nChannels = 1;
        fallbackFmt.nSamplesPerSec = kSampleRate;
        fallbackFmt.wBitsPerSample = 32;
        fallbackFmt.nBlockAlign = 4;
        const WAVEFORMATEX* fmt = usingNativeMix ? mixFmt : &fallbackFmt;

        if (!client || !capture || !IsSupportedDeviceFormat(fmt)) hr = AUDCLNT_E_UNSUPPORTED_FORMAT;
        const bool shouldStart = SUCCEEDED(hr) && !g.stopRequested.load();
        if (shouldStart) hr = client->Start();
        const bool started = shouldStart && SUCCEEDED(hr);
        int64_t resamplePhase = 0;
        float previous = 0.0f;
        auto lastPacket = std::chrono::steady_clock::now();
        std::vector<float> chunk;

        while (SUCCEEDED(hr) && !g.stopRequested.load()) {
            Sleep(8);
            UINT32 packetLength = 0;
            while (!g.stopRequested.load()) {
                hr = capture->GetNextPacketSize(&packetLength);
                if (FAILED(hr) || packetLength == 0) break;
                BYTE* data = nullptr;
                UINT32 frames = 0;
                DWORD flags = 0;
                hr = capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
                if (FAILED(hr) || hr == AUDCLNT_S_BUFFER_EMPTY) break;
                lastPacket = std::chrono::steady_clock::now();
                if (g.punchInActive && (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY)) {
                    g.captureHadDiscontinuity = true;
                }

                chunk.clear();
                float peak = 0.0f;
                double sumSq = 0.0;
                const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 || !data;
                const uint16_t channelCount = fmt->nChannels;
                const uint16_t sampleBytes = fmt->wBitsPerSample / 8;
                for (UINT32 f = 0; f < frames; ++f) {
                    double mono = 0.0;
                    if (!silent) {
                        const uint8_t* frame = data + static_cast<size_t>(f) * fmt->nBlockAlign;
                        for (uint16_t c = 0; c < channelCount; ++c) {
                            const float sample = ReadSampleAsFloat(frame + c * sampleBytes, fmt);
                            if (std::isfinite(sample)) mono += sample;
                        }
                    }
                    const float value = static_cast<float>(mono / channelCount);
                    if (fmt->nSamplesPerSec == kSampleRate) {
                        chunk.push_back(value);
                    } else {
                        while (resamplePhase < kSampleRate) {
                            const float fraction = static_cast<float>(resamplePhase) / kSampleRate;
                            chunk.push_back(std::lerp(previous, value, fraction));
                            resamplePhase += fmt->nSamplesPerSec;
                        }
                        resamplePhase -= kSampleRate;
                        previous = value;
                    }
                }

                hr = capture->ReleaseBuffer(frames);

                if (!chunk.empty()) {
                    for (float sample : chunk) {
                        peak = std::max(peak, std::fabs(sample));
                        sumSq += static_cast<double>(sample) * sample;
                    }
                    float rms = static_cast<float>(std::sqrt(sumSq / chunk.size()));
                    float peakDb = 20.0f * std::log10(std::max(peak, 1e-4f));
                    float rmsDb = 20.0f * std::log10(std::max(rms, 1e-4f));
                    g.livePeakDb.store(std::clamp(peakDb, -60.0f, 0.0f));
                    g.liveRmsDb.store(std::clamp(rmsDb, -60.0f, 0.0f));

                    std::lock_guard<std::mutex> lk(g.audioMutex);
                    // Commit the edit only after WASAPI delivers samples.
                    if (!g.punchInActive) {
                        if (g.stopRequested.load()) break;
                        PushUndoLocked();
                        g.punchInOriginal = g.raw;
                        g.punchInTailStart = replacedEnd;
                        auto take = std::make_shared<AudioTrack>();
                        take->samples.reserve(takeStart + chunk.size() + 2 * kSampleRate);
                        take->samples.assign(g.raw->samples.begin(), g.raw->samples.begin() + takeStart);
                        const size_t prefixBuckets = takeStart / kWaveformBucketSize;
                        take->wave.assign(g.raw->wave.begin(), g.raw->wave.begin() + prefixBuckets);
                        for (const auto& bucket : take->wave) {
                            take->peak = std::max({take->peak, -bucket.minVal, bucket.maxVal});
                        }
                        g.raw = std::move(take);
                        g.processed.reset();
                        g.punchInStartSample = takeStart;
                        g.punchInActive = true;
                    }
                    auto& raw = g.raw->samples;
                    const size_t firstNew = raw.size();
                    raw.insert(raw.end(), chunk.begin(), chunk.end());
                    UpdateWaveCache(*g.raw, firstNew);
                    g.playheadSample.store(raw.size());
                    if (!g.captureReady.load()) {
                        g.captureReady.store(true);
                        if (hwnd) PostMessageW(hwnd, WM_APP + 1, static_cast<WPARAM>(TransportResult::CaptureStarted), generation);
                    }
                }
                if (FAILED(hr)) break;
            }
            if (std::chrono::steady_clock::now() - lastPacket > std::chrono::seconds(3)) {
                hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
            }
        }

        if (started && client) client->Stop();
        capture.Reset();
        client.Reset();
        dev.Reset();
        enumerator.Reset();
        if (mixFmt) CoTaskMemFree(mixFmt);
        if (hMmcss) AvRevertMmThreadCharacteristics(hMmcss);
        if (SUCCEEDED(hrCo)) CoUninitialize();
        if (hwnd) {
            const auto result = g.stopRequested.load() ? TransportResult::CaptureCancelled : TransportResult::CaptureFailed;
            PostMessageW(hwnd, WM_APP + 1, static_cast<WPARAM>(result), generation);
        }
    });

    EnsureMotionTimer();
    SetStatus(Text(L"Connecting to the microphone… Wait for the recording indicator.", L"Подключение к микрофону… Дождитесь индикатора записи."));
}

static void StartPlayback() {
    if (g.isPlaying.load()) return;
    StopAudioTransport();
    if (EffectsPending()) {
        g.playWhenProcessed = true;
        SetStatus(Text(L"Playback will start after effects have been processed…", L"Прослушивание начнётся после пересчёта эффектов…"));
        return;
    }

    size_t startSample = 0;
    size_t endSample = 0;
    std::shared_ptr<const AudioTrack> playback;
    {
        std::lock_guard<std::mutex> lk(g.audioMutex);
        playback = g.processed ? g.processed : g.raw;
        const auto& samples = playback->samples;
        if (samples.empty()) {
            SetStatus(Text(L"The track is empty. Press Space to record.", L"Запись пуста. Нажмите Пробел, чтобы начать запись."));
            return;
        }
        const size_t total = samples.size();
        if (g.hasSelection && g.selEndSample != g.selStartSample) {
            size_t s0 = std::min(g.selStartSample, g.selEndSample);
            size_t s1 = std::min(total, std::max(g.selStartSample, g.selEndSample));
            size_t cur = g.playheadSample.load();
            startSample = (cur >= s0 && cur < s1) ? cur : s0;
            endSample = s1;
        } else {
            size_t cur = g.playheadSample.load();
            startSample = (cur < total) ? cur : 0;
            endSample = total;
        }
    }

    if (startSample >= endSample) startSample = 0;
    g.playheadSample.store(startSample);

    std::wstring devId = g.selectedOutputId;
    g.stopRequested.store(false);
    g.isPlaying.store(true);
    g.lastFrameTime = std::chrono::steady_clock::now();

    g.audioThread = std::thread([devId, startSample, endSample, playback = std::move(playback),
                                 hwnd = g.hwndMain, generation = g.transportGeneration]() {
        HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        DWORD taskIdx = 0;
        HANDLE hMmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIdx);

        ComPtr<IMMDeviceEnumerator> enumerator;
        ComPtr<IMMDevice> dev;
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
        if (SUCCEEDED(hr) && enumerator) {
            if (!devId.empty()) {
                hr = enumerator->GetDevice(devId.c_str(), &dev);
            }
            if (FAILED(hr) || !dev) {
                hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &dev);
            }
        }

        ComPtr<IAudioClient> client;
        ComPtr<IAudioRenderClient> render;
        WAVEFORMATEX* mixFmt = nullptr;
        bool usingNativeMix = false;

        if (dev) {
            hr = OpenAudioClientWithFormat(dev.Get(), false, client, &mixFmt, usingNativeMix);
            if (SUCCEEDED(hr) && client) {
                hr = client->GetService(IID_PPV_ARGS(&render));
            }
        }

        UINT32 bufferFrames = 0;
        if (SUCCEEDED(hr) && client && render) hr = client->GetBufferSize(&bufferFrames);
        if (!bufferFrames) hr = E_FAIL;

        WAVEFORMATEX fallbackFmt{};
        fallbackFmt.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
        fallbackFmt.nChannels = 2;
        fallbackFmt.nSamplesPerSec = kSampleRate;
        fallbackFmt.wBitsPerSample = 32;
        fallbackFmt.nBlockAlign = 8;
        const WAVEFORMATEX* fmt = usingNativeMix ? mixFmt : &fallbackFmt;

        if (!IsSupportedDeviceFormat(fmt)) hr = AUDCLNT_E_UNSUPPORTED_FORMAT;
        const double srcStep = SUCCEEDED(hr) ? static_cast<double>(kSampleRate) / fmt->nSamplesPerSec : 1.0;
        double srcPos = static_cast<double>(startSample);
        bool finished = false;
        const auto& samples = playback->samples;
        const float ceiling = playback->limiter ? dsp::kLimiterCeiling : 1.0f;
        float gain = g.masterGainLinear.load();
        float gainStep = 1.0f;
        if (SUCCEEDED(hr)) gainStep = 1.0f - dsp::SmoothingCoefficient(0.006f, fmt->nSamplesPerSec);

        const bool shouldStart = SUCCEEDED(hr) && !g.stopRequested.load();
        if (shouldStart) hr = client->Start();
        const bool started = shouldStart && SUCCEEDED(hr);
        auto lastBuffer = std::chrono::steady_clock::now();
        while (SUCCEEDED(hr) && !g.stopRequested.load() && !finished) {
            Sleep(8);
            UINT32 padding = 0;
            hr = client->GetCurrentPadding(&padding);
            if (FAILED(hr)) break;
            UINT32 avail = (bufferFrames > padding) ? (bufferFrames - padding) : 0;
            if (avail == 0) {
                if (std::chrono::steady_clock::now() - lastBuffer > std::chrono::seconds(3)) {
                    hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
                }
                continue;
            }

            BYTE* data = nullptr;
            hr = render->GetBuffer(avail, &data);
            if (FAILED(hr)) break;
            lastBuffer = std::chrono::steady_clock::now();

            float peak = 0.0f;
            double sumSq = 0.0;
            UINT32 framesWritten = 0;

            {
                const size_t limit = std::min(endSample, samples.size());
                const uint16_t chCount = fmt->nChannels;
                const uint16_t bpsBytes = fmt->wBitsPerSample / 8;
                const uint16_t blockAlign = fmt->nBlockAlign;
                const float targetGain = g.masterGainLinear.load();

                for (UINT32 f = 0; f < avail; ++f) {
                    size_t idx0 = static_cast<size_t>(srcPos);
                    if (idx0 >= limit) {
                        // Fill remaining buffer with silence
                        std::memset(data + f * blockAlign, 0, (avail - f) * blockAlign);
                        finished = true;
                        break;
                    }
                    float sample = samples[idx0];
                    if (fmt->nSamplesPerSec != kSampleRate && idx0 + 1 < limit) {
                        double frac = srcPos - static_cast<double>(idx0);
                        sample = static_cast<float>(sample + (samples[idx0 + 1] - sample) * frac);
                    }
                    srcPos += srcStep;
                    gain += (targetGain - gain) * gainStep;
                    sample = ApplyOutputGain(sample, gain, ceiling);

                    float a = std::fabs(sample);
                    if (a > peak) peak = a;
                    sumSq += static_cast<double>(sample) * static_cast<double>(sample);

                    uint8_t* framePtr = data + f * blockAlign;
                    for (uint16_t c = 0; c < chCount; ++c) {
                        WriteFloatToDeviceFormat(framePtr + c * bpsBytes, fmt, sample);
                    }
                    ++framesWritten;
                }
            }

            hr = render->ReleaseBuffer(avail, 0);
            if (FAILED(hr)) break;
            g.playheadSample.store(std::min(endSample, static_cast<size_t>(srcPos)));
            if (framesWritten > 0) {
                float rms = static_cast<float>(std::sqrt(sumSq / framesWritten));
                g.livePeakDb.store(std::clamp(20.0f * std::log10(std::max(peak, 1e-4f)), -60.0f, 0.0f));
                g.liveRmsDb.store(std::clamp(20.0f * std::log10(std::max(rms, 1e-4f)), -60.0f, 0.0f));
            }
        }

        if (finished && SUCCEEDED(hr)) {
            Sleep(static_cast<DWORD>((static_cast<uint64_t>(bufferFrames) * 1000) / fmt->nSamplesPerSec));
        }
        if (started && client) client->Stop();

        render.Reset();
        client.Reset();
        dev.Reset();
        enumerator.Reset();
        if (mixFmt) CoTaskMemFree(mixFmt);
        if (hMmcss) AvRevertMmThreadCharacteristics(hMmcss);
        if (SUCCEEDED(hrCo)) CoUninitialize();

        if (!g.stopRequested.load()) {
            g.isPlaying.store(false);
            const auto result = FAILED(hr) ? TransportResult::PlaybackFailed : TransportResult::PlaybackFinished;
            if (hwnd) PostMessageW(hwnd, WM_APP + 1, static_cast<WPARAM>(result), generation);
        }
    });

    EnsureMotionTimer();
    SetStatus(Text(L"▶ Playing… Press Enter or Space to pause.", L"▶ Воспроизведение... Нажмите Enter или Пробел для паузы."));
}

static void TogglePlayback() {
    if (g.playWhenProcessed) {
        g.playWhenProcessed = false;
        SetStatus(Text(L"Playback canceled.", L"Прослушивание отменено."));
    } else if (g.isPlaying.load()) {
        StopAudioTransport();
    } else {
        StartPlayback();
    }
}

// ============================================================================
// Actions: Undo, Delete Selection, Effects, Script Loading, Export
// ============================================================================

static void DoUndo() {
    StopAudioTransport();
    std::lock_guard<std::mutex> lk(g.audioMutex);
    if (g.undoStack.empty()) {
        SetStatus(Text(L"Nothing to undo.", L"Нечего отменять."));
        return;
    }
    CloseSilenceTools();
    g.raw = std::move(g.undoStack.back());
    g.undoStack.pop_back();
    g.processed.reset();
    g.hasSelection = false;
    QueueEffectsLocked();
    MarkDocumentChanged();
    const bool saved = QueueAutoBackupLocked(false);
    g.playheadSample.store(std::min(g.playheadSample.load(), g.raw->samples.size()));
    SetStatus(saved ? Text(L"Change undone.", L"Действие отменено.") : Text(L"Change undone. Could not update the backup.", L"Действие отменено. Не удалось обновить резервную копию."), !saved);
}

static void DoDeleteSelection() {
    StopAudioTransport();
    std::lock_guard<std::mutex> lk(g.audioMutex);
    const auto& source = g.raw->samples;
    if (!g.hasSelection || g.selStartSample == g.selEndSample || source.empty()) {
        SetStatus(Text(L"Select a region of the waveform first.", L"Сначала выделите участок на звуковой волне мышкой."));
        return;
    }
    size_t s0 = std::min(g.selStartSample, g.selEndSample);
    size_t s1 = std::min(source.size(), std::max(g.selStartSample, g.selEndSample));
    if (s1 <= s0) return;

    auto edited = std::make_shared<AudioTrack>();
    auto& samples = edited->samples;
    samples.reserve(source.size() - (s1 - s0));
    samples.insert(samples.end(), source.begin(), source.begin() + s0);
    samples.insert(samples.end(), source.begin() + s1, source.end());
    // Gentle 5ms crossfade at splice point
    if (s0 > 0 && s0 < samples.size()) {
        size_t len = std::min<size_t>(120, std::min(s0, samples.size() - s0));
        for (size_t i = 0; i < len; ++i) {
            float t = static_cast<float>(i) / static_cast<float>(len);
            samples[s0 - len + i] *= (1.0f - t);
            samples[s0 + i] *= t;
        }
    }
    UpdateWaveCache(*edited);
    CloseSilenceTools();
    PushUndoLocked();
    g.raw = std::move(edited);
    g.processed.reset();
    g.hasSelection = false;
    QueueEffectsLocked();
    MarkDocumentChanged();
    const bool saved = QueueAutoBackupLocked(false);
    g.playheadSample.store(std::min(s0, g.raw->samples.size()));
    SetStatus(saved ? Text(L"Selection deleted. Press Ctrl+Z to undo.", L"Выделенный фрагмент удалён (Ctrl+Z вернёт).")
                    : Text(L"Selection deleted. Could not update the backup.", L"Фрагмент удалён. Не удалось обновить резервную копию."), !saved);
}

static void DoClearAudio() {
    StopAudioTransport();
    std::lock_guard<std::mutex> lk(g.audioMutex);
    if (g.raw->samples.empty()) return;
    CloseSilenceTools();
    PushUndoLocked();
    g.raw = std::make_shared<AudioTrack>();
    g.processed.reset();
    QueueEffectsLocked();
    g.hasSelection = false;
    g.playheadSample.store(0);
    g.viewStartSec = 0.0;
    g.viewDurationSec = 0.0;
    MarkDocumentChanged();
    const bool saved = QueueAutoBackupLocked(false);
    SetStatus(saved ? Text(L"Track cleared. Press Ctrl+Z to undo.", L"Дорожка очищена (Ctrl+Z вернёт).")
                    : Text(L"Track cleared. Could not remove the previous backup.", L"Дорожка очищена. Не удалось удалить предыдущую резервную копию."), !saved);
}

static void ApplySilencePreview() {
    if (g.isRecording.load() || !g.silencePreviewReady || g.silenceRanges.empty() || g.silenceApplyPending) return;
    if (FileOperationPending()) {
        SetStatus(Text(L"Wait for the file operation to finish.", L"Дождитесь завершения операции с файлом."));
        return;
    }
    StopAudioTransport();
    std::shared_ptr<const AudioTrack> source;
    {
        std::lock_guard<std::mutex> lock(g.audioMutex);
        source = g.raw;
    }
    AudioWork job{AudioWorkKind::RemoveSilence, ++g.silenceRevision, std::move(source)};
    job.cuts = g.silenceRanges;
    g.silenceApplyPending = SubmitAudioWork(std::move(job));
    if (g.silenceApplyPending) SetStatus(Text(L"Shortening the marked pauses…", L"Удаляем отмеченные паузы…"));
    else SetStatus(Text(L"Could not start shortening pauses.", L"Не удалось запустить удаление пауз."), true);
}

static void HandleAudioWorkResults() {
    std::optional<AudioWorkResult> effects, silenceResult;
    {
        std::lock_guard<std::mutex> lock(g.workMutex);
        effects.swap(g.readyEffects);
        silenceResult.swap(g.readySilence);
    }
    if (effects && effects->revision == g.effectsRevision) {
        g.effectsPending = false;
        if (effects->succeeded) {
            std::lock_guard<std::mutex> lock(g.audioMutex);
            g.processed = std::move(effects->track);
            g.appliedEffectSettings = effects->effects;
        } else {
            g.playWhenProcessed = false;
            g.effectSettings = g.processed ? g.appliedEffectSettings : EffectSettings{};
            SetStatus(Text(L"Could not process effects. The recording is intact.", L"Не удалось пересчитать эффекты. Запись сохранена."), true);
        }
    }
    if (silenceResult && silenceResult->revision == g.silenceRevision) {
        g.silenceScanPending = false;
        g.silenceApplyPending = false;
        if (!silenceResult->succeeded) {
            SetStatus(Text(L"Could not shorten pauses. The recording is intact.", L"Не удалось обработать паузы. Запись сохранена."), true);
        } else if (silenceResult->kind == AudioWorkKind::SilencePreview) {
            g.silenceRanges = std::move(silenceResult->cuts);
            g.silencePreviewReady = true;
        } else {
            StopAudioTransport();
            std::lock_guard<std::mutex> lock(g.audioMutex);
            const size_t removed = silence::RemovedSamples(g.silenceRanges);
            g.playheadSample.store(silence::MapSample(g.silenceRanges, g.playheadSample.load()));
            const size_t viewSample = static_cast<size_t>(g.viewStartSec * kSampleRate);
            g.viewStartSec = static_cast<double>(silence::MapSample(g.silenceRanges, viewSample)) / kSampleRate;
            g.animatingWaveView = false;
            g.hasSelection = false;
            CloseSilenceTools();
            PushUndoLocked();
            g.raw = std::move(silenceResult->track);
            g.processed.reset();
            QueueEffectsLocked();
            MarkDocumentChanged();
            const bool saved = QueueAutoBackupLocked(false);
            SetStatus(saved ? Text(L"Pauses shortened by ", L"Убрано пауз: ") + FormatTime(static_cast<double>(removed) / kSampleRate) + Text(L" · Ctrl+Z to undo", L" · Ctrl+Z отменит")
                            : Text(L"Pauses shortened. Could not update the backup.", L"Паузы убраны. Не удалось обновить резервную копию."), !saved);
        }
    }
    if (g.playWhenProcessed && !EffectsPending()) StartPlayback();
    if (g.hwndMain) InvalidateRect(g.hwndMain, nullptr, FALSE);
}

enum class FxKind { Cleanup, FadeIn, FadeOut, Echo, Equalizer, HighPass, Gate, DeEsser, Compressor, Limiter };

static void ToggleEffectAction(FxKind fx, bool forceEnable = false) {
    if (g.isRecording.load()) {
        SetStatus(Text(L"Stop recording to change effects.", L"Остановите запись, чтобы изменить эффекты."));
        return;
    }
    StopAudioTransport();
    std::lock_guard<std::mutex> lk(g.audioMutex);
    if (g.raw->samples.empty()) {
        SetStatus(Text(L"There is no audio to process.", L"Нет записи для обработки эффектом."), true);
        return;
    }

    const bool includedInCleanup = g.effectSettings.cleanup && (fx == FxKind::HighPass || fx == FxKind::Gate);
    auto toggle = [forceEnable, includedInCleanup](bool& enabled, const wchar_t* name, const wchar_t* detail = L"") {
        enabled = forceEnable || !enabled;
        std::wstring message = name;
        if (includedInCleanup) message += Text(L" separately", L" отдельно");
        message += enabled ? Text(L" · ON", L" · ВКЛ") : Text(L" · OFF", L" · ВЫКЛ");
        if (enabled && *detail) {
            message += L" · ";
            message += detail;
        }
        if (includedInCleanup) message += Text(L" · already included in cleanup", L" · обработка уже в очистке");
        SetStatus(message);
    };

    switch (fx) {
        case FxKind::Cleanup:
            toggle(g.effectSettings.cleanup, Text(L"Voice cleanup", L"Очистка звука"), Text(L"Rumble filter, noise gate and peak normalization", L"Срез гула, гейт и нормализация пиков"));
            break;
        case FxKind::FadeIn:
            toggle(g.effectSettings.fadeIn, Text(L"Fade in", L"Фейд-ин"));
            break;
        case FxKind::FadeOut:
            toggle(g.effectSettings.fadeOut, Text(L"Fade out", L"Фейд-аут"));
            break;
        case FxKind::Echo:
            toggle(g.effectSettings.echo, Text(L"Echo", L"Эхо"), Text(L"140 ms", L"140 мс"));
            break;
        case FxKind::Equalizer:
            toggle(g.effectSettings.equalizer, Text(L"Equalizer", L"Эквалайзер"));
            break;
        case FxKind::HighPass:
            toggle(g.effectSettings.highPass, Text(L"Rumble filter", L"Срез гула"), Text(L"80 Hz", L"80 Гц"));
            break;
        case FxKind::Gate:
            toggle(g.effectSettings.gate, Text(L"Noise gate", L"Шумовой гейт"), Text(L"−42 dBFS threshold", L"порог −42 dBFS"));
            break;
        case FxKind::DeEsser:
            toggle(g.effectSettings.deEsser, Text(L"De-esser", L"Де-эссер"), Text(L"softens sharp sibilants", L"смягчает резкие «с» и «ш»"));
            break;
        case FxKind::Compressor:
            toggle(g.effectSettings.compressor, Text(L"Compressor", L"Компрессор"), Text(L"2:1, −18 dBFS threshold", L"2:1, порог −18 dBFS"));
            break;
        case FxKind::Limiter:
            toggle(g.effectSettings.limiter, Text(L"Limiter", L"Лимитер"), Text(L"−1 dBFS peak ceiling", L"пики до −1 dBFS"));
            break;
    }
    MarkDocumentChanged();
    if (!includedInCleanup) QueueEffectsLocked();
}

static void DoJumpToLastLoudMoment() {
    StopAudioTransport();
    std::lock_guard<std::mutex> lk(g.audioMutex);
    const auto& samples = CurrentTrackLocked().samples;
    if (samples.empty()) {
        SetStatus(Text(L"The track is empty.", L"Запись пуста."));
        return;
    }

    const size_t win = kSampleRate / 50; // 20 ms (960 samples)
    size_t total = samples.size();
    size_t numWins = (total + win - 1) / win;
    std::vector<float> winDb(numWins, -100.0f);
    float maxRmsDb = -100.0f;

    for (size_t w = 0; w < numWins; ++w) {
        size_t s0 = w * win;
        size_t s1 = std::min(total, s0 + win);
        double sumSq = 0.0;
        for (size_t i = s0; i < s1; ++i) {
            double v = static_cast<double>(samples[i]);
            sumSq += v * v;
        }
        float rms = static_cast<float>(std::sqrt(sumSq / std::max<size_t>(1, s1 - s0)));
        float db = 20.0f * std::log10(std::max(rms, 1e-5f));
        winDb[w] = db;
        if (db > maxRmsDb) maxRmsDb = db;
    }

    if (maxRmsDb < -52.0f) {
        g.playheadSample.store(0);
        g.hasSelection = false;
        SetStatus(Text(L"No loud passages found. The track is silent.", L"Громких фрагментов не найдено (в записи только тишина)."));
        return;
    }

    float threshDb = std::max(-38.0f, maxRmsDb - 24.0f);
    size_t lastLoudWin = 0;
    bool found = false;
    for (size_t w = numWins; w > 0; --w) {
        if (winDb[w - 1] >= threshDb) {
            lastLoudWin = w - 1;
            found = true;
            break;
        }
    }
    if (!found) {
        g.playheadSample.store(0);
        g.hasSelection = false;
        SetStatus(Text(L"No loud passages found.", L"Громких фрагментов не найдено."));
        return;
    }

    size_t lastLoudEnd = std::min(total, (lastLoudWin + 1) * win + kSampleRate / 10); // +0.10s tail

    // Walk backward from lastLoudWin until >= 13 quiet windows (~0.26s gap) to find phrase start
    size_t quietRun = 0;
    size_t phraseStartWin = 0;
    for (size_t w = lastLoudWin; w > 0; --w) {
        if (winDb[w - 1] < threshDb) {
            ++quietRun;
            if (quietRun >= 13) {
                phraseStartWin = w - 1 + quietRun;
                break;
            }
        } else {
            quietRun = 0;
        }
    }
    size_t phraseStartSample = phraseStartWin * win;
    if (phraseStartSample > kSampleRate / 20) phraseStartSample -= kSampleRate / 20;

    size_t cur = g.playheadSample.load();
    size_t tol = kSampleRate / 5; // 0.20s
    bool nearEnd = (cur + tol >= lastLoudEnd && cur <= lastLoudEnd + tol);
    size_t target = (nearEnd && phraseStartSample + kSampleRate / 10 < lastLoudEnd) ? phraseStartSample : lastLoudEnd;

    g.playheadSample.store(target);
    g.hasSelection = false;

    if (g.viewDurationSec > 0.1) {
        double targetSec = static_cast<double>(target) / kSampleRate;
        double totalSec = static_cast<double>(total) / kSampleRate;
        double maxViewStart = std::max(0.0, std::max(totalSec, g.viewDurationSec) - g.viewDurationSec);
        if (targetSec < g.viewStartSec || targetSec > g.viewStartSec + g.viewDurationSec) {
            g.viewStartSec = std::clamp(targetSec - g.viewDurationSec * 0.65, 0.0, maxViewStart);
        }
    }

    if (target == phraseStartSample && nearEnd) {
        SetStatus(Text(L"Cursor at the start of the last phrase (", L"Курсор в начале последней фразы (") + FormatTime(target / (double)kSampleRate) + Text(L"). Press Space to record over it.", L") — Пробел перезапишет её."));
    } else {
        SetStatus(Text(L"Cursor after the last loud word (", L"Курсор после последнего громкого слова (") + FormatTime(target / (double)kSampleRate) + Text(L"). Click again to jump to the start of the phrase.", L") — повторный клик вернёт к началу фразы."));
    }
}

static void DoOpenTextFile() {
    wchar_t fileBuf[MAX_PATH]{};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g.hwndMain;
    ofn.lpstrTitle = Text(L"Open script", L"Открыть сценарий");
    ofn.lpstrFilter = Text(L"Scripts (*.md;*.txt)\0*.md;*.txt\0All files (*.*)\0*.*\0",
                         L"Сценарии (*.md;*.txt)\0*.md;*.txt\0Все файлы (*.*)\0*.*\0");
    ofn.lpstrFile = fileBuf;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (fs::exists(L"e:\\Ai Podcast\\episodes")) {
        ofn.lpstrInitialDir = L"e:\\Ai Podcast\\episodes";
    }
    if (GetOpenFileNameW(&ofn)) {
        std::wstring text = ReadTextFileSmart(fileBuf);
        if (!text.empty()) {
            LoadScriptIntoPrompter(text, Text(L"Script loaded into the teleprompter: ", L"Сценарий загружен в суфлёр: ") + fs::path(fileBuf).filename().wstring());
        } else {
            SetStatus(Text(L"Could not read the text file.", L"Не удалось прочитать текстовый файл."), true);
        }
    }
}

static void DoPasteClipboardText() {
    if (!OpenClipboard(g.hwndMain)) return;
    HANDLE hData = GetClipboardData(CF_UNICODETEXT);
    if (hData) {
        const wchar_t* pszText = static_cast<const wchar_t*>(GlobalLock(hData));
        if (pszText && pszText[0] != L'\0') {
            LoadScriptIntoPrompter(pszText, Text(L"Script pasted from the clipboard.", L"Текст вставлен в телесуфлёр из буфера обмена."));
        }
        if (pszText) GlobalUnlock(hData);
    }
    CloseClipboard();
}

static std::wstring NormalizeCrLf(const std::wstring& s) {
    std::wstring out;
    out.reserve(s.size() + 32);
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == L'\n' && (i == 0 || s[i - 1] != L'\r')) {
            out.push_back(L'\r');
        }
        out.push_back(s[i]);
    }
    return out;
}

static LRESULT CALLBACK EditorWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            HFONT hFont = CreateFontW(-18, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                      DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                      CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(hFont));
            std::wstring editText = NormalizeCrLf(g.prompterText);
            g.hwndEditorEdit = CreateWindowExW(
                WS_EX_CLIENTEDGE, L"EDIT", editText.c_str(),
                WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_LEFT | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN,
                12, 12, 680, 400, hwnd, reinterpret_cast<HMENU>(1001), nullptr, nullptr
            );
            SendMessageW(g.hwndEditorEdit, WM_SETFONT, reinterpret_cast<WPARAM>(hFont), TRUE);

            HWND btnSave = CreateWindowExW(
                0, L"BUTTON", Text(L"Use in prompter", L"Сохранить в телесуфлёр"),
                WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                420, 424, 180, 34, hwnd, reinterpret_cast<HMENU>(IDOK), nullptr, nullptr
            );
            HWND btnCancel = CreateWindowExW(
                0, L"BUTTON", Text(L"Cancel", L"Отмена"),
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                610, 424, 82, 34, hwnd, reinterpret_cast<HMENU>(IDCANCEL), nullptr, nullptr
            );
            SendMessageW(btnSave, WM_SETFONT, reinterpret_cast<WPARAM>(hFont), TRUE);
            SendMessageW(btnCancel, WM_SETFONT, reinterpret_cast<WPARAM>(hFont), TRUE);
            SetFocus(g.hwndEditorEdit);
            return 0;
        }
        case WM_SIZE: {
            int w = LOWORD(lParam);
            int h = HIWORD(lParam);
            if (g.hwndEditorEdit) MoveWindow(g.hwndEditorEdit, 12, 12, std::max(100, w - 24), std::max(100, h - 68), TRUE);
            HWND bOk = GetDlgItem(hwnd, IDOK);
            HWND bCan = GetDlgItem(hwnd, IDCANCEL);
            if (bOk) MoveWindow(bOk, std::max(12, w - 292), h - 46, 186, 34, TRUE);
            if (bCan) MoveWindow(bCan, std::max(12, w - 98), h - 46, 86, 34, TRUE);
            return 0;
        }
        case WM_COMMAND: {
            if (LOWORD(wParam) == IDOK) {
                int len = GetWindowTextLengthW(g.hwndEditorEdit);
                std::wstring buf(len + 1, L'\0');
                GetWindowTextW(g.hwndEditorEdit, buf.data(), len + 1);
                buf.resize(len);
                LoadScriptIntoPrompter(buf, Text(L"Teleprompter script updated.", L"Текст телесуфлёра обновлён."));
                DestroyWindow(hwnd);
                return 0;
            }
            if (LOWORD(wParam) == IDCANCEL) {
                DestroyWindow(hwnd);
                return 0;
            }
            break;
        }
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            EnableWindow(g.hwndMain, TRUE);
            SetForegroundWindow(g.hwndMain);
            g.hwndEditor = nullptr;
            g.hwndEditorEdit = nullptr;
            return 0;
        case WM_NCDESTROY:
            if (auto font = reinterpret_cast<HFONT>(GetWindowLongPtrW(hwnd, GWLP_USERDATA))) DeleteObject(font);
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static void DoOpenTextEditorModal() {
    if (g.hwndEditor) return;
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = EditorWndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        wc.lpszClassName = L"PodcasterTextEditor";
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        RegisterClassW(&wc);
        registered = true;
    }
    EnableWindow(g.hwndMain, FALSE);
    g.hwndEditor = CreateWindowExW(
        WS_EX_DLGMODALFRAME, L"PodcasterTextEditor", Text(L"Teleprompter script editor", L"Редактор текста для телесуфлёра"),
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT, 720, 520,
        g.hwndMain, nullptr, GetModuleHandleW(nullptr), nullptr
    );
    if (!g.hwndEditor) {
        EnableWindow(g.hwndMain, TRUE);
        SetStatus(Text(L"Could not open the script editor.", L"Не удалось открыть редактор текста."), true);
    }
}

static bool IsProjectPath(const fs::path& path) {
    return CompareStringOrdinal(path.extension().c_str(), -1, L".podcaster", -1, TRUE) == CSTR_EQUAL;
}

static constexpr bool EffectSettings::* kProjectEffectFlags[] = {
    &EffectSettings::cleanup, &EffectSettings::fadeIn, &EffectSettings::fadeOut, &EffectSettings::echo,
    &EffectSettings::equalizer, &EffectSettings::highPass, &EffectSettings::gate, &EffectSettings::deEsser,
    &EffectSettings::compressor, &EffectSettings::limiter
};

static project_file::Settings SnapshotProjectSettings() {
    project_file::Settings settings;
    const auto& fx = g.effectSettings;
    for (size_t i = 0; i < std::size(kProjectEffectFlags); ++i) {
        if (fx.*kProjectEffectFlags[i]) settings.effectFlags |= 1u << i;
    }
    settings.eqLowDb = fx.eq.lowDb;
    settings.eqMidDb = fx.eq.midDb;
    settings.eqHighDb = fx.eq.highDb;
    settings.gainDb = g.masterGainDb.load();
    settings.silenceDb = g.silenceThresholdDb;
    settings.prompterSpeed = g.prompterSpeed;
    settings.prompterFontSize = g.prompterFontSize;
    settings.prompterScroll = g.prompterScrollTarget;
    settings.autoScroll = g.prompterAutoScroll;
    settings.waveformStart = g.viewStartSec;
    settings.waveformDuration = g.viewDurationSec;
    settings.playhead = g.playheadSample.load();
    settings.hasSelection = g.hasSelection;
    settings.selectionStart = g.selStartSample;
    settings.selectionEnd = g.selEndSample;
    settings.wavFloat32 = g.wavFloat32;
    return settings;
}

static bool WriteProjectAtomically(const fs::path& path, const project_file::Settings& settings,
                                   const std::wstring& script, const std::vector<float>& samples,
                                   std::wstring& error) {
    const auto temp = CreateTempFileIn(path.parent_path());
    if (temp.empty()) {
        error = Text(L"Could not create a temporary file next to the project.", L"Не удалось создать временный файл рядом с проектом.");
        return false;
    }
    std::error_code ec;
    try {
        if (project_file::Write(temp, settings, WideToUtf8(script), samples, error)) {
            if (MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                return true;
            }
            error = Text(L"Could not save the project. Check whether another app has the file open.", L"Не удалось сохранить проект. Проверьте, открыт ли файл в другой программе.");
        }
    } catch (...) {
        fs::remove(temp, ec);
        throw;
    }
    fs::remove(temp, ec);
    return false;
}

template <typename Work>
static bool LaunchDocumentWork(DocumentResult result, Work work) {
    try {
        g.documentThread = std::thread([result = std::move(result), work = std::move(work),
                                        hwnd = g.hwndMain]() mutable {
            try {
                work(result);
            } catch (const std::exception&) {
                result.error = Text(L"Could not finish the file operation. Check available disk space and memory.", L"Не удалось завершить операцию с файлом. Проверьте свободное место и доступную память.");
            }
            g.documentResult = std::move(result);
            PostMessageW(hwnd, kDocumentReadyMessage, 0, 0);
        });
        return true;
    } catch (const std::exception&) {
        SetStatus(Text(L"Could not start the file operation.", L"Не удалось запустить операцию с файлом."), true);
        return false;
    }
}

static void BeginOpenDocument(fs::path path) {
    DocumentResult result;
    result.path = std::move(path);
    result.operation = IsProjectPath(result.path) ? DocumentOperation::OpenProject : DocumentOperation::OpenAudio;
    SetStatus(Text(L"Opening “", L"Открываем «") + result.path.filename().wstring() + Text(L"”…", L"»…"));
    LaunchDocumentWork(std::move(result), [](DocumentResult& opened) {
        auto track = std::make_shared<AudioTrack>();
        if (opened.operation == DocumentOperation::OpenProject) {
            project_file::Project project;
            if (!project_file::Read(opened.path, project, opened.error)) return;
            opened.settings = project.settings;
            opened.script = Utf8ToWide(project.scriptUtf8);
            track->samples = std::move(project.samples);
        } else if (!ImportAudioFile(opened.path, track->samples, opened.error)) {
            return;
        }
        UpdateWaveCache(*track);
        opened.track = std::move(track);
    });
}

static void DoOpenAudioOrProject(bool fromBackups) {
    if (FileOperationPending() || g.isRecording.load() || g.silenceApplyPending) return;
    const wchar_t* openFilter = Text(
        L"Audio and projects\0*.podcaster;*.wav;*.wave;*.mp3;*.flac;*.m4a;*.aac;*.ogg;*.oga;*.opus;*.aif;*.aiff;*.wma\0"
        L"Podcaster projects\0*.podcaster\0"
        L"Audio files\0*.wav;*.wave;*.mp3;*.flac;*.m4a;*.aac;*.ogg;*.oga;*.opus;*.aif;*.aiff;*.wma\0"
        L"All files\0*.*\0",
        L"Аудио и проекты\0*.podcaster;*.wav;*.wave;*.mp3;*.flac;*.m4a;*.aac;*.ogg;*.oga;*.opus;*.aif;*.aiff;*.wma\0"
        L"Проекты Podcaster\0*.podcaster\0"
        L"Аудиофайлы\0*.wav;*.wave;*.mp3;*.flac;*.m4a;*.aac;*.ogg;*.oga;*.opus;*.aif;*.aiff;*.wma\0"
        L"Все файлы\0*.*\0");
    const auto directory = fromBackups ? GetBackupDir() : g.documentPath.parent_path();
    wchar_t buffer[MAX_PATH]{};
    OPENFILENAMEW dialog{sizeof(dialog)};
    dialog.hwndOwner = g.hwndMain;
    dialog.lpstrTitle = fromBackups ? Text(L"Open backup recording", L"Открыть резервную запись") : Text(L"Open audio or project", L"Открыть аудио или проект");
    dialog.lpstrFilter = openFilter;
    dialog.nFilterIndex = fromBackups ? 3 : 1;
    dialog.lpstrInitialDir = directory.empty() ? nullptr : directory.c_str();
    dialog.lpstrFile = buffer;
    dialog.nMaxFile = MAX_PATH;
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    g.documentPromptOpen = true;
    const bool selected = GetOpenFileNameW(&dialog) != FALSE;
    g.documentPromptOpen = false;
    if (selected) BeginOpenDocument(buffer);
}

static bool StartSaveProject(bool saveAs) {
    if (FileOperationPending() || g.isRecording.load() || g.silenceApplyPending) return false;
    fs::path path = g.documentPath;
    if (saveAs || !IsProjectPath(path)) {
        const auto directory = path.parent_path();
        const auto title = path.empty() ? Text(L"New project", L"Новый проект") : path.stem().wstring();
        wchar_t buffer[MAX_PATH]{};
        wcsncpy_s(buffer, title.c_str(), _TRUNCATE);
        OPENFILENAMEW dialog{sizeof(dialog)};
        dialog.hwndOwner = g.hwndMain;
        dialog.lpstrTitle = Text(L"Save Podcaster project", L"Сохранить проект Podcaster");
        dialog.lpstrFilter = Text(L"Podcaster project (*.podcaster)\0*.podcaster\0",
                                 L"Проект Podcaster (*.podcaster)\0*.podcaster\0");
        dialog.lpstrDefExt = L"podcaster";
        dialog.lpstrInitialDir = directory.empty() ? nullptr : directory.c_str();
        dialog.lpstrFile = buffer;
        dialog.nMaxFile = MAX_PATH;
        dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        g.documentPromptOpen = true;
        const bool selected = GetSaveFileNameW(&dialog) != FALSE;
        g.documentPromptOpen = false;
        if (!selected) return false;
        path = buffer;
        if (!IsProjectPath(path)) {
            path += L".podcaster";
            std::error_code ec;
            if (fs::exists(path, ec)) {
                const auto text = Text(L"Replace project “", L"Заменить проект «") + path.filename().wstring() + Text(L"”?", L"»?");
                if (MessageBoxExW(g.hwndMain, text.c_str(), Text(L"Save project", L"Сохранение проекта"),
                                  MB_YESNO | MB_ICONQUESTION, InterfaceLangId()) != IDYES) {
                    return false;
                }
            }
        }
    }
    DocumentResult result;
    result.operation = DocumentOperation::SaveProject;
    result.path = std::move(path);
    result.revision = g.documentRevision;
    const auto settings = SnapshotProjectSettings();
    std::shared_ptr<const AudioTrack> track;
    {
        std::lock_guard<std::mutex> lock(g.audioMutex);
        track = g.raw;
    }
    SetStatus(Text(L"Saving “", L"Сохраняем «") + result.path.filename().wstring() + Text(L"”…", L"»…"));
    return LaunchDocumentWork(std::move(result), [settings, track = std::move(track), script = g.prompterText](
                                                   DocumentResult& saved) {
        WriteProjectAtomically(saved.path, settings, script, track->samples, saved.error);
    });
}

static void DoSaveProject(bool saveAs) {
    StartSaveProject(saveAs);
}

static void InstallDocument(DocumentResult opened) {
    StopAudioTransport();
    CloseSilenceTools();
    const bool native = opened.operation == DocumentOperation::OpenProject;
    const auto total = opened.track->samples.size();
    if (native) {
        const auto& settings = opened.settings;
        g.prompterSpeed = std::clamp(settings.prompterSpeed, kMinPrompterSpeed, kMaxPrompterSpeed);
        g.prompterFontSize = std::clamp(settings.prompterFontSize, 16.0f, 46.0f);
        g.prompterAutoScroll = settings.autoScroll;
        RebuildPrompterTextFormat();
        SetPrompterText(opened.script, false);
        g.prompterScrollY = g.prompterScrollTarget = std::max(0.0f, settings.prompterScroll);
        g.silenceThresholdDb = std::clamp(settings.silenceDb, -70.0f, -20.0f);
        g.wavFloat32 = settings.wavFloat32;
        SetMasterGainDb(settings.gainDb, false);
        auto& fx = g.effectSettings;
        for (size_t i = 0; i < std::size(kProjectEffectFlags); ++i) {
            fx.*kProjectEffectFlags[i] = (settings.effectFlags & (1u << i)) != 0;
        }
        fx.eq.lowDb = std::clamp(settings.eqLowDb, -12.0f, 12.0f);
        fx.eq.midDb = std::clamp(settings.eqMidDb, -12.0f, 12.0f);
        fx.eq.highDb = std::clamp(settings.eqHighDb, -12.0f, 12.0f);
    } else {
        g.effectSettings = {};
        SetMasterGainDb(0.0f, false);
    }
    const auto& settings = opened.settings;
    const double trackSeconds = static_cast<double>(total) / kSampleRate;
    g.playheadSample.store(native ? std::min<uint64_t>(settings.playhead, total) : total);
    g.viewStartSec = native ? std::clamp(settings.waveformStart, 0.0, trackSeconds) : 0.0;
    g.viewDurationSec = native ? std::clamp(settings.waveformDuration, 0.0, std::max(10.0, trackSeconds * 1.04)) : 0.0;
    g.animatingWaveView = false;
    g.selStartSample = native ? std::min<uint64_t>(settings.selectionStart, total) : 0;
    g.selEndSample = native ? std::min<uint64_t>(settings.selectionEnd, total) : 0;
    g.hasSelection = native && settings.hasSelection && g.selStartSample != g.selEndSample;
    {
        std::lock_guard<std::mutex> lock(g.audioMutex);
        g.raw = std::move(opened.track);
        g.processed.reset();
        g.undoStack.clear();
        QueueEffectsLocked();
    }
    g.documentPath = std::move(opened.path);
    g.documentDirty = false;
    ++g.documentRevision;
    UpdateDocumentTitle();
    SetStatus((native ? Text(L"Opened project “", L"Открыт проект «") : Text(L"Opened audio “", L"Открыто аудио «")) + g.documentPath.filename().wstring() + Text(L"”", L"»"));
}

static int AskSaveBeforeLeaving() {
    g.documentPromptOpen = true;
    const int choice = MessageBoxExW(g.hwndMain,
        Text(L"Save your changes to a Podcaster project?", L"Сохранить текущие изменения в проект Podcaster?"),
        Text(L"Unsaved changes", L"Текущий проект изменён"), MB_YESNOCANCEL | MB_ICONQUESTION, InterfaceLangId());
    g.documentPromptOpen = false;
    return choice;
}

static void HandleDocumentResult() {
    if (!g.documentThread.joinable()) return;
    g.documentThread.join();
    auto result = std::move(g.documentResult);
    if (!result.error.empty()) {
        g.openAfterSave.reset();
        g.closeAfterSave = false;
        SetStatus(result.error, true);
        return;
    }
    if (result.operation == DocumentOperation::SaveProject) {
        const bool current = g.documentRevision == result.revision;
        g.documentPath = std::move(result.path);
        if (current) g.documentDirty = false;
        UpdateDocumentTitle();
        auto next = std::move(g.openAfterSave);
        g.openAfterSave.reset();
        const bool close = g.closeAfterSave;
        g.closeAfterSave = false;
        if (current && next) {
            std::error_code ec;
            if (g.documentPath == next->path || fs::equivalent(g.documentPath, next->path, ec)) {
                SetStatus(Text(L"Project “", L"Проект «") + g.documentPath.filename().wstring() + Text(L"” saved.", L"» сохранён."));
            } else {
                InstallDocument(std::move(*next));
            }
        } else if (current && close) {
            PostMessageW(g.hwndMain, WM_CLOSE, 0, 0);
        } else {
            SetStatus(current ? Text(L"Project “", L"Проект «") + g.documentPath.filename().wstring() + Text(L"” saved.", L"» сохранён.")
                              : Text(L"Project saved. Changes made after saving started are still unsaved.", L"Проект сохранён. После начала сохранения появились новые изменения."));
        }
        return;
    }
    if (!result.track) {
        SetStatus(Text(L"Could not open the selected file.", L"Не удалось открыть выбранный файл."), true);
        return;
    }
    if (!g.documentDirty) {
        InstallDocument(std::move(result));
        return;
    }
    const int choice = AskSaveBeforeLeaving();
    if (choice == IDNO) {
        InstallDocument(std::move(result));
    } else if (choice == IDYES && StartSaveProject(false)) {
        g.openAfterSave = std::move(result);
    } else {
        SetStatus(Text(L"Opening canceled.", L"Открытие отменено."));
    }
}

static void DoExportDialog(bool asMp3) {
    if (g.exportThread.joinable()) {
        SetStatus(Text(L"An export is already running.", L"Экспорт уже идёт."));
        return;
    }
    StopAudioTransport();
    {
        std::lock_guard<std::mutex> lk(g.audioMutex);
        if (g.raw->samples.empty()) {
            SetStatus(Text(L"Record or import audio before exporting.", L"Нечего экспортировать — сначала запишите звук."), true);
            return;
        }
    }

    wchar_t fileBuf[MAX_PATH] = L"podcast-recording";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g.hwndMain;
    ofn.lpstrFile = fileBuf;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (asMp3) {
        ofn.lpstrTitle = Text(L"Export MP3", L"Экспорт MP3");
        ofn.lpstrFilter = Text(L"MP3 audio 320 kbps (*.mp3)\0*.mp3\0", L"Звук MP3 320 кбит/с (*.mp3)\0*.mp3\0");
        ofn.lpstrDefExt = L"mp3";
    } else {
        ofn.lpstrTitle = Text(L"Export WAV", L"Экспорт WAV");
        ofn.lpstrFilter = Text(L"Lossless WAV (*.wav)\0*.wav\0", L"WAV без потерь (*.wav)\0*.wav\0");
        ofn.lpstrDefExt = L"wav";
    }
    if (fs::exists(L"e:\\Ai Podcast\\episodes")) {
        ofn.lpstrInitialDir = L"e:\\Ai Podcast\\episodes";
    }

    if (!GetSaveFileNameW(&ofn)) return;

    std::shared_ptr<const AudioTrack> exportTrack;
    const bool renderEffects = EffectsPending();
    const auto fx = g.effectSettings;
    {
        std::lock_guard<std::mutex> lk(g.audioMutex);
        exportTrack = !renderEffects && g.processed ? g.processed : g.raw;
    }
    SetStatus(asMp3 ? Text(L"Encoding MP3 (320 kbps, 48 kHz)…", L"Кодирование MP3 (320 кбит/с, 48 кГц)…") : Text(L"Writing WAV…", L"Запись WAV…"));
    try {
        g.exportThread = std::thread([path = fs::path(fileBuf), track = std::move(exportTrack), asMp3,
                                     renderEffects, fx, gain = g.masterGainLinear.load(),
                                     float32 = g.wavFloat32, hwnd = g.hwndMain]() {
            std::wstring error;
            try {
                std::vector<float> adjusted;
                const float ceiling = (renderEffects ? fx.limiter : track->limiter) ? dsp::kLimiterCeiling : 1.0f;
                if (renderEffects || gain != 1.0f || track->peak > ceiling) {
                    adjusted = track->samples;
                    if (renderEffects) ProcessEffects(adjusted, fx);
                    for (float& sample : adjusted) sample = ApplyOutputGain(sample, gain, ceiling);
                }
                const auto& samples = adjusted.empty() ? track->samples : adjusted;
                g.exportSucceeded = asMp3 ? ExportMp3File(path.wstring(), samples, error)
                                          : WriteWavAtomically(path, samples, float32, error);
                const wchar_t* format = asMp3 ? Text(L"MP3 (320 kbps)", L"MP3 (320 кбит/с)") : float32 ? L"WAV (32-bit Float)" : L"WAV (24-bit PCM)";
                g.exportStatus = g.exportSucceeded ? Text(L"Exported as ", L"Сохранено в ") + std::wstring(format) + L": " + path.filename().wstring()
                                                  : error;
            } catch (const std::exception&) {
                g.exportSucceeded = false;
                g.exportStatus = Text(L"Could not finish exporting audio.", L"Не удалось завершить экспорт аудио.");
            }
            PostMessageW(hwnd, WM_APP + 2, 0, 0);
        });
    } catch (const std::system_error&) {
        SetStatus(Text(L"Could not start exporting audio.", L"Не удалось запустить экспорт аудио."), true);
    }
}

static UINT TrackAnchoredMenu(HMENU menu, D2D1_RECT_F anchorRect) {
    float dpiX = 96.0f, dpiY = 96.0f;
    if (g.rt) g.rt->GetDpi(&dpiX, &dpiY);
    POINT pt{ static_cast<LONG>(anchorRect.left * dpiX / 96.0f),
              static_cast<LONG>(anchorRect.bottom * dpiY / 96.0f) };
    ClientToScreen(g.hwndMain, &pt);
    return TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y, 0, g.hwndMain, nullptr);
}

enum FileMenuCommand : UINT { OpenAudioOrProject = 6001, OpenBackup, SaveProject, SaveProjectAs };
enum LanguageMenuCommand : UINT { SelectEnglish = 6101, SelectRussian };

static void ShowLanguageMenu(D2D1_RECT_F anchorRect) {
    HMENU menu = CreatePopupMenu();
    if (!menu) {
        SetStatus(Text(L"Could not open the language menu.", L"Не удалось открыть меню выбора языка."), true);
        return;
    }
    AppendMenuW(menu, MF_STRING, SelectEnglish, L"English");
    AppendMenuW(menu, MF_STRING, SelectRussian, L"Русский");
    const UINT active = locale_ui::Current() == Language::Russian ? SelectRussian : SelectEnglish;
    CheckMenuRadioItem(menu, SelectEnglish, SelectRussian, active, MF_BYCOMMAND);
    const UINT command = TrackAnchoredMenu(menu, anchorRect);
    DestroyMenu(menu);
    if (command) SendMessageW(g.hwndMain, WM_COMMAND, command, 0);
}

static void ShowFileMenu(D2D1_RECT_F anchorRect) {
    HMENU menu = CreatePopupMenu();
    if (!menu) {
        SetStatus(Text(L"Could not open the File menu.", L"Не удалось открыть меню «Файл»."), true);
        return;
    }

    const bool enabled = !FileOperationPending() && !g.isRecording.load() && !g.silenceApplyPending;
    const UINT itemFlags = MF_STRING | (enabled ? MF_ENABLED : MF_GRAYED);
    AppendMenuW(menu, itemFlags, OpenAudioOrProject, Text(L"Open audio or project…\tCtrl+O", L"Открыть аудио или проект…\tCtrl+O"));
    AppendMenuW(menu, itemFlags, OpenBackup, Text(L"Open backup recording…", L"Открыть резервную запись…"));
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, itemFlags, SaveProject, Text(L"Save project\tCtrl+S", L"Сохранить проект\tCtrl+S"));
    AppendMenuW(menu, itemFlags, SaveProjectAs, Text(L"Save project as…\tCtrl+Shift+S", L"Сохранить проект как…\tCtrl+Shift+S"));

    g.fileMenuOpen = true;
    InvalidateRect(g.hwndMain, nullptr, FALSE);
    UpdateWindow(g.hwndMain);
    const UINT command = TrackAnchoredMenu(menu, anchorRect);
    DestroyMenu(menu);
    g.fileMenuOpen = false;
    InvalidateRect(g.hwndMain, nullptr, FALSE);

    if (command != 0) SendMessageW(g.hwndMain, WM_COMMAND, command, 0);
}

static void ShowDeviceMenu(bool isInput, D2D1_RECT_F anchorRect) {
    RefreshDevices(false);
    const auto& list = isInput ? g.inputDevices : g.outputDevices;
    const std::wstring& currentId = isInput ? g.selectedInputId : g.selectedOutputId;

    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    if (list.empty()) {
        AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, Text(L"No audio devices found", L"Устройства не найдены"));
    } else {
        for (size_t i = 0; i < list.size(); ++i) {
            UINT flags = MF_STRING | (list[i].id == currentId ? MF_CHECKED : MF_UNCHECKED);
            std::wstring label = AudioDeviceDisplayName(list[i]) + (list[i].isDefault ? Text(L" (default)", L" (по умолчанию)") : L"");
            AppendMenuW(menu, flags, 5000 + i, label.c_str());
        }
    }

    const UINT cmd = TrackAnchoredMenu(menu, anchorRect);
    DestroyMenu(menu);

    if (cmd >= 5000) {
        size_t idx = static_cast<size_t>(cmd - 5000);
        if (idx < list.size()) {
            if (isInput && g.isRecording.load()) StopAudioTransport();
            if (!isInput && g.isPlaying.load()) StopAudioTransport();
            if (isInput) {
                g.selectedInputId = list[idx].id;
                SetStatus(Text(L"Microphone selected: ", L"Выбран микрофон: ") + AudioDeviceDisplayName(list[idx]));
            } else {
                g.selectedOutputId = list[idx].id;
                SetStatus(Text(L"Output selected: ", L"Выбрано устройство вывода: ") + AudioDeviceDisplayName(list[idx]));
            }
            InvalidateRect(g.hwndMain, nullptr, FALSE);
        }
    }
}

// ============================================================================
// Direct2D Rendering
// ============================================================================

static float LayoutDpi(HWND hwnd, D2D1_SIZE_U client) {
    const float windowDpi = static_cast<float>(GetDpiForWindow(hwnd));
    const float fitWidth = client.width * 96.0f / kMinClientWidth;
    const float fitHeight = client.height * 96.0f / kMinClientHeight;
    return std::max(24.0f, std::min({windowDpi, fitWidth, fitHeight}));
}

static HRESULT EnsureDeviceResources() {
    if (g.rt) return S_OK;
    RECT rc{};
    GetClientRect(g.hwndMain, &rc);
    D2D1_SIZE_U size = D2D1::SizeU(std::max<UINT>(1, rc.right - rc.left), std::max<UINT>(1, rc.bottom - rc.top));
    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties();
    props.dpiX = props.dpiY = LayoutDpi(g.hwndMain, size);
    D2D1_HWND_RENDER_TARGET_PROPERTIES hwndProps = D2D1::HwndRenderTargetProperties(g.hwndMain, size);
    HRESULT hr = g.d2dFactory->CreateHwndRenderTarget(props, hwndProps, &g.rt);
    if (SUCCEEDED(hr) && g.rt) {
        hr = g.rt->CreateSolidColorBrush(Theme::TextPrimary, &g.brush);
        if (FAILED(hr)) {
            g.rt.Reset();
            return hr;
        }
        const D2D1_GRADIENT_STOP stops[] = {
            D2D1::GradientStop(0, Theme::BgPanel),
            D2D1::GradientStop(1, ColorHex(0x1F1F1F, 0)),
        };
        ComPtr<ID2D1GradientStopCollection> edges;
        if (SUCCEEDED(g.rt->CreateGradientStopCollection(stops, ARRAYSIZE(stops), &edges))) {
            g.rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({}, {0, 24}), edges.Get(), &g.prompterEdge);
        }
    }
    return hr;
}

#include "button_icons.h"

static float ButtonLabelWidth(int id, const std::wstring& text, IDWriteTextFormat* format) {
    struct LabelMetrics {
        std::wstring text;
        IDWriteTextFormat* format = nullptr;
        float width = 0.0f;
    };
    static std::unordered_map<int, LabelMetrics> cache;
    auto& label = cache[id];
    if (label.text != text || label.format != format) {
        label = { text, format, 0.0f };
        ComPtr<IDWriteTextLayout> layout;
        if (SUCCEEDED(g.dwFactory->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()),
                                                   format, 500.0f, 32.0f, &layout))) {
            DWRITE_TEXT_METRICS metrics{};
            if (SUCCEEDED(layout->GetMetrics(&metrics))) label.width = std::ceil(metrics.width);
        }
    }
    return label.width;
}

static void DrawButton(
    int id,
    D2D1_RECT_F r,
    const std::wstring& label,
    D2D1_COLOR_F bg,
    D2D1_COLOR_F border,
    D2D1_COLOR_F textColor,
    IDWriteTextFormat* tf,
    bool enabled = true,
    float radius = 6.0f
) {
    if (enabled) g.hitRects.push_back({ id, r });
    bool hov = enabled && (g.hoveredControlId == id);
    if (hov) {
        bg.r = std::min(1.0f, bg.r + 0.06f);
        bg.g = std::min(1.0f, bg.g + 0.06f);
        bg.b = std::min(1.0f, bg.b + 0.06f);
    }
    if (!enabled) {
        bg.a *= 0.45f;
        border.a *= 0.4f;
        textColor.a *= 0.4f;
    }

    D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(r, radius, radius);
    g.brush->SetColor(bg);
    g.rt->FillRoundedRectangle(rr, g.brush.Get());
    g.brush->SetColor(border);
    g.rt->DrawRoundedRectangle(rr, g.brush.Get(), 1.0f);

    const bool alignLeft = id == ID_BTN_INPUT_DEV || id == ID_BTN_OUTPUT_DEV;
    const ButtonIcon icon = GetButtonIcon(id);
    const bool hasIcon = icon != ButtonIcon::None;
    const float midY = (r.top + r.bottom) * 0.5f;
    D2D1_RECT_F textRect = D2D1::RectF(r.left + 6, r.top, r.right - 6, r.bottom);
    if (alignLeft) {
        textRect.right = r.right - 18.0f;
        DrawButtonIcon(ButtonIcon::ChevronDown, r.right - 10.0f, midY, textColor);
    }
    if (hasIcon) {
        constexpr float iconWidth = 16.0f;
        constexpr float iconGap = 5.0f;
        float iconLeft = (r.left + r.right - iconWidth) * 0.5f;
        if (!label.empty() && tf) {
            if (alignLeft) {
                iconLeft = r.left + 8.0f;
            } else {
                const float labelWidth = ButtonLabelWidth(id, label, tf);
                const float contentWidth = std::min(r.right - r.left - 12.0f, iconWidth + iconGap + labelWidth);
                iconLeft = r.left + (r.right - r.left - contentWidth) * 0.5f;
            }
            textRect.left = iconLeft + iconWidth + iconGap;
        }
        auto iconColor = textColor;
        if (id == ID_BTN_REC_STOP) iconColor = ColorHex(0xFF8B82, textColor.a);
        DrawButtonIcon(icon, iconLeft + iconWidth * 0.5f, midY, iconColor);
    }
    if (tf && !label.empty() && textRect.right > textRect.left) {
        tf->SetTextAlignment(alignLeft || hasIcon ? DWRITE_TEXT_ALIGNMENT_LEADING : DWRITE_TEXT_ALIGNMENT_CENTER);
        tf->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        g.brush->SetColor(textColor);
        g.rt->DrawTextW(label.c_str(), static_cast<UINT32>(label.size()), tf, textRect, g.brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
}

#include "wave_controls.h"

static void DrawStudioMicrophone(D2D1_RECT_F bounds, bool isRec) {
    g.hitRects.push_back({ ID_BTN_MIC_TOGGLE, bounds });
    float cx = (bounds.left + bounds.right) * 0.5f;
    float cy = (bounds.top + bounds.bottom) * 0.5f;
    float h = bounds.bottom - bounds.top;
    float s = std::min(1.0f, h / 136.0f);

    // Only the active microphone gets a pulse. Its inner ring reacts to the voice level.
    if (g.recordPresence > 0.0f) {
        const auto center = D2D1::Point2F(cx, cy - 8 * s);
        const float ringR = (46.0f + g.drawnRmsFraction * 8.0f) * s;
        g.brush->SetColor(ColorHex(0xE6786C, g.recordPresence * (0.36f + g.drawnRmsFraction * 0.29f)));
        g.rt->DrawEllipse(D2D1::Ellipse(center, ringR, ringR), g.brush.Get(), 1.25f);
        if (g.animationsEnabled) {
            const float phase = g.recordPulsePhase;
            const float pulseR = ringR + (4.0f + 9.0f * phase) * s;
            const float opacity = std::sin(phase * std::numbers::pi_v<float>) * g.recordPresence;
            g.brush->SetColor(ColorHex(0xE6786C, opacity * (0.16f + 0.26f * g.drawnRmsFraction)));
            g.rt->DrawEllipse(D2D1::Ellipse(center, pulseR, pulseR), g.brush.Get(), 1.1f);
        }
    } else if (g.hoveredControlId == ID_BTN_MIC_TOGGLE) {
        g.brush->SetColor(ColorHex(0xD49B4B, 0.45f));
        g.rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy - 8 * s), 51.0f * s, 51.0f * s), g.brush.Get(), 1.25f);
    }

    // Desk Stand Base & Stem
    g.brush->SetColor(ColorHex(0x2E2E2E));
    g.rt->FillRoundedRectangle(D2D1::RoundedRect(
        D2D1::RectF(cx - 26 * s, cy + 52 * s, cx + 26 * s, cy + 60 * s), 4 * s, 4 * s), g.brush.Get());
    g.brush->SetColor(ColorHex(0x4E4E4E));
    g.rt->FillRectangle(D2D1::RectF(cx - 3.5f * s, cy + 38 * s, cx + 3.5f * s, cy + 52 * s), g.brush.Get());

    // U-shaped Cradle Yoke
    g.brush->SetColor(ColorHex(0x6E6E6E));
    g.rt->DrawLine(D2D1::Point2F(cx - 27 * s, cy + 2 * s), D2D1::Point2F(cx - 27 * s, cy + 36 * s), g.brush.Get(), 3.0f * s);
    g.rt->DrawLine(D2D1::Point2F(cx + 27 * s, cy + 2 * s), D2D1::Point2F(cx + 27 * s, cy + 36 * s), g.brush.Get(), 3.0f * s);
    g.rt->DrawLine(D2D1::Point2F(cx - 27 * s, cy + 36 * s), D2D1::Point2F(cx + 27 * s, cy + 36 * s), g.brush.Get(), 3.2f * s);
    g.rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx - 27 * s, cy + 3 * s), 3.8f * s, 3.8f * s), g.brush.Get());
    g.rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx + 27 * s, cy + 3 * s), 3.8f * s, 3.8f * s), g.brush.Get());

    // Microphone Body (lower cylinder)
    D2D1_ROUNDED_RECT bodyRect = D2D1::RoundedRect(
        D2D1::RectF(cx - 20 * s, cy - 12 * s, cx + 20 * s, cy + 28 * s), 8 * s, 8 * s);
    g.brush->SetColor(ColorHex(0x242424));
    g.rt->FillRoundedRectangle(bodyRect, g.brush.Get());
    g.brush->SetColor(ColorHex(0x4D4D4D));
    g.rt->DrawRoundedRectangle(bodyRect, g.brush.Get(), 1.3f);

    // Metallic Capsule Grille (upper dome)
    D2D1_ROUNDED_RECT capRect = D2D1::RoundedRect(
        D2D1::RectF(cx - 20 * s, cy - 52 * s, cx + 20 * s, cy - 6 * s), 19 * s, 19 * s);
    g.brush->SetColor(ColorHex(0x636363));
    g.rt->FillRoundedRectangle(capRect, g.brush.Get());
    g.brush->SetColor(ColorHex(0x989898));
    g.rt->DrawRoundedRectangle(capRect, g.brush.Get(), 1.4f);

    // Capsule grille mesh lines
    g.brush->SetColor(ColorHex(0x303030, 0.8f));
    for (float gy = cy - 42 * s; gy <= cy - 14 * s; gy += 6.0f * s) {
        float halfW = (gy < cy - 33 * s) ? 14.0f * s : 17.5f * s;
        g.rt->DrawLine(D2D1::Point2F(cx - halfW, gy), D2D1::Point2F(cx + halfW, gy), g.brush.Get(), 1.1f);
    }
    // Center collar ring
    g.brush->SetColor(ColorHex(0x989898));
    g.rt->FillRectangle(D2D1::RectF(cx - 21 * s, cy - 12 * s, cx + 21 * s, cy - 7 * s), g.brush.Get());

    // Gain knob + LED status light on body
    g.brush->SetColor(isRec ? Theme::RecordRed : Theme::GoodGreen);
    g.rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy - 1 * s), 3.0f * s, 3.0f * s), g.brush.Get());

    g.brush->SetColor(ColorHex(0x4A4A4A));
    g.rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy + 14 * s), 7.0f * s, 7.0f * s), g.brush.Get());
    g.brush->SetColor(ColorHex(0x989898));
    g.rt->DrawLine(D2D1::Point2F(cx, cy + 14 * s), D2D1::Point2F(cx + 4 * s, cy + 10 * s), g.brush.Get(), 1.6f);
}

static void RenderWindow() {
    if (FAILED(EnsureDeviceResources()) || !g.rt) return;

    // Use wall-clock time so motion keeps its pace when rendering is briefly delayed.
    auto now = std::chrono::steady_clock::now();
    float dt = std::chrono::duration<float>(now - g.lastFrameTime).count();
    g.lastFrameTime = now;
    dt = std::clamp(dt, 0.0f, 0.1f);

    const bool rec = g.isRecording.load();
    const bool captureLive = rec && g.captureReady.load();
    const bool play = g.isPlaying.load();
    const bool showSilenceTools = g.silenceToolsOpen && !rec;
    const float silenceToolsHeight = showSilenceTools ? kSilenceToolsHeight : 0.0f;
    const float recordTarget = captureLive ? 1.0f : 0.0f;
    g.recordPresence = g.animationsEnabled ? EaseToward(g.recordPresence, recordTarget, dt, 0.085f, 0.005f) : recordTarget;
    if (captureLive && g.animationsEnabled) g.recordPulsePhase = std::fmod(g.recordPulsePhase + dt / 1.55f, 1.0f);
    else if (g.recordPresence == 0.0f) g.recordPulsePhase = 0.0f;

    D2D1_SIZE_F sz = g.rt->GetSize();
    float W = sz.width;
    float H = sz.height;
    g.hitRects.clear();

    g.rt->BeginDraw();
    g.rt->Clear(Theme::BgMain);

    // ========================================================================
    // 1. Top Device & Utility Bar (y: 8..48)
    // ========================================================================
    const float topBarY = 8.0f;
    const float topBarH = 38.0f;
    g.brush->SetColor(Theme::BgPanel);
    g.rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(12, topBarY, W - 12, topBarY + topBarH), 8, 8), g.brush.Get());
    g.brush->SetColor(Theme::Border);
    g.rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(12, topBarY, W - 12, topBarY + topBarH), 8, 8), g.brush.Get(), 1.0f);

    // Title
    g.tfUiBold->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    g.tfUiBold->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    g.brush->SetColor(Theme::TextPrimary);
    const wchar_t* appTitle = Text(L"Podcaster", L"Подкастер");
    g.rt->DrawTextW(appTitle, static_cast<UINT32>(wcslen(appTitle)), g.tfUiBold.Get(), D2D1::RectF(24, topBarY, 108, topBarY + topBarH), g.brush.Get());
    DrawButton(ID_BTN_FILE_MENU, D2D1::RectF(112, topBarY + 5, 200, topBarY + topBarH - 5), Text(L"File ▾", L"Файл ▾"),
               g.fileMenuOpen ? Theme::BgButtonActive : Theme::BgButton,
               g.fileMenuOpen ? Theme::BorderActive : Theme::Border,
               Theme::TextPrimary, g.tfUiSmall.Get(), !FileOperationPending());

    std::wstring inName = GetDeviceDisplayName(g.inputDevices, g.selectedInputId, Text(L"No microphone", L"Нет микрофона"));
    std::wstring outName = GetDeviceDisplayName(g.outputDevices, g.selectedOutputId, Text(L"No output", L"Нет вывода"));
    constexpr float deviceStart = 208.0f;
    constexpr float deviceGap = 8.0f;
    constexpr float refreshGap = 6.0f;
    constexpr float refreshWidth = 32.0f;
    constexpr float utilitiesGap = 12.0f;
    constexpr float languageWidth = 70.0f;
    constexpr float languageGap = 6.0f;
    const float undoLeft = W - 246.0f;
    const float languageLeft = undoLeft - languageGap - languageWidth;
    const float devicesSpace = languageLeft - deviceStart - deviceGap - refreshGap - refreshWidth - utilitiesGap;
    const float devWidth = std::clamp(devicesSpace * 0.5f, 170.0f, 290.0f);

    DrawButton(ID_BTN_INPUT_DEV, D2D1::RectF(deviceStart, topBarY + 5, deviceStart + devWidth, topBarY + topBarH - 5),
               Text(L"Input: ", L"Вход: ") + inName, Theme::BgButton, Theme::Border, Theme::TextPrimary, g.tfUiSmall.Get());
    const float outX = deviceStart + devWidth + deviceGap;
    DrawButton(ID_BTN_OUTPUT_DEV, D2D1::RectF(outX, topBarY + 5, outX + devWidth, topBarY + topBarH - 5),
               Text(L"Output: ", L"Выход: ") + outName, Theme::BgButton, Theme::Border, Theme::TextPrimary, g.tfUiSmall.Get());
    const float refX = outX + devWidth + refreshGap;
    DrawButton(ID_BTN_REFRESH_DEV, D2D1::RectF(refX, topBarY + 5, refX + refreshWidth, topBarY + topBarH - 5),
               L"", Theme::BgButton, Theme::Border, Theme::TextSecond, g.tfUi.Get());
    DrawButton(ID_BTN_LANGUAGE, D2D1::RectF(languageLeft, topBarY + 5, languageLeft + languageWidth, topBarY + topBarH - 5),
               Text(L"EN ▾", L"RU ▾"), Theme::BgButton, Theme::Border, Theme::TextPrimary, g.tfUiSmall.Get());

    bool canUndo = false;
    bool hasAudio = false;
    {
        std::lock_guard<std::mutex> lk(g.audioMutex);
        canUndo = !g.undoStack.empty();
        hasAudio = !g.raw->samples.empty();
    }
    DrawButton(ID_BTN_CLEAR_AUDIO, D2D1::RectF(W - 116, topBarY + 5, W - 20, topBarY + topBarH - 5),
               Text(L"Clear", L"Очистить"), Theme::BgButton, Theme::Border, Theme::TextSecond, g.tfUiSmall.Get(), hasAudio);
    DrawButton(ID_BTN_UNDO, D2D1::RectF(undoLeft, topBarY + 5, W - 122, topBarY + topBarH - 5),
               Text(L"Undo", L"Отменить"), Theme::BgButton, Theme::Border, Theme::TextPrimary, g.tfUiSmall.Get(), canUndo);

    // ========================================================================
    // 2. Dynamic Vertical Zones
    // ========================================================================
    const float bottomDockH = 112.0f;
    const float bottomDockY = H - bottomDockH - 8.0f;
    const float prompterY = 54.0f;
    const float availMidH = bottomDockY - prompterY;
    const float micRowH = 148.0f;
    const float maxPrompterH = std::clamp(availMidH - micRowH - 100.0f - silenceToolsHeight - 24.0f, 132.0f, 340.0f);
    const float prompterTotalH = std::clamp(availMidH * 0.42f, 132.0f, maxPrompterH);

    const float micRowY = prompterY + prompterTotalH + 8.0f;
    const float waveY = micRowY + micRowH + 8.0f;
    const float waveH = bottomDockY - waveY - 8.0f;

    // ========================================================================
    // 3. Teleprompter Panel
    // ========================================================================
    D2D1_RECT_F pPanel = D2D1::RectF(12, prompterY, W - 12, prompterY + prompterTotalH);
    g.brush->SetColor(Theme::BgPanel);
    g.rt->FillRoundedRectangle(D2D1::RoundedRect(pPanel, 8, 8), g.brush.Get());
    g.brush->SetColor(Theme::Border);
    g.rt->DrawRoundedRectangle(D2D1::RoundedRect(pPanel, 8, 8), g.brush.Get(), 1.0f);

    // Teleprompter Toolbar
    float ptY = prompterY + 6;
    DrawButton(ID_BTN_OPEN_TEXT, D2D1::RectF(20, ptY, 114, ptY + 26), Text(L"Script", L"Сценарий"), Theme::BgButton, Theme::Border, Theme::TextPrimary, g.tfUiSmall.Get());
    DrawButton(ID_BTN_PASTE_TEXT, D2D1::RectF(119, ptY, 214, ptY + 26), Text(L"Paste", L"Вставить"), Theme::BgButton, Theme::Border, Theme::TextPrimary, g.tfUiSmall.Get());
    DrawButton(ID_BTN_EDIT_TEXT, D2D1::RectF(219, ptY, 314, ptY + 26), Text(L"Editor", L"Редактор"), Theme::BgButton, Theme::Border, Theme::TextPrimary, g.tfUiSmall.Get());
    DrawButton(ID_BTN_LOAD_LATEST_EP, D2D1::RectF(319, ptY, 449, ptY + 26), Text(L"Latest episode", L"Свежий выпуск"), Theme::BgButton, Theme::Border, Theme::AccentCyan, g.tfUiSmall.Get());

    DrawButton(ID_BTN_PROMPTER_TOP, D2D1::RectF(W - 504, ptY, W - 421, ptY + 26), Text(L"To top", L"Наверх"), Theme::BgButton, Theme::Border, Theme::TextSecond, g.tfUiSmall.Get());
    DrawButton(ID_BTN_AUTOSCROLL, D2D1::RectF(W - 416, ptY, W - 275, ptY + 26),
               g.prompterAutoScroll ? Text(L"Auto-scroll on", L"Автоскролл вкл.") : Text(L"Auto-scroll off", L"Автоскролл выкл."),
               g.prompterAutoScroll ? Theme::BgButtonActive : Theme::BgButton,
               g.prompterAutoScroll ? Theme::BorderActive : Theme::Border, Theme::TextPrimary, g.tfUiSmall.Get());
    DrawButton(ID_BTN_SPEED_DOWN, D2D1::RectF(W - 270, ptY, W - 244, ptY + 26), L"", Theme::BgButton, Theme::Border, Theme::TextPrimary, g.tfUiSmall.Get());

    wchar_t speedBuf[64];
    swprintf_s(speedBuf, Text(L"%.0f px/s (←/→)", L"%.0f пкс/с (←/→)"), g.prompterSpeed);
    g.tfUiSmall->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    g.tfUiSmall->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    g.brush->SetColor(Theme::TextPrimary);
    g.rt->DrawTextW(speedBuf, static_cast<UINT32>(wcslen(speedBuf)), g.tfUiSmall.Get(),
                    D2D1::RectF(W - 242, ptY, W - 134, ptY + 26), g.brush.Get());

    DrawButton(ID_BTN_SPEED_UP, D2D1::RectF(W - 132, ptY, W - 106, ptY + 26), L"", Theme::BgButton, Theme::Border, Theme::TextPrimary, g.tfUiSmall.Get());
    DrawButton(ID_BTN_FONT_DOWN, D2D1::RectF(W - 98, ptY, W - 62, ptY + 26), L"A-", Theme::BgButton, Theme::Border, Theme::TextSecond, g.tfUiSmall.Get());
    DrawButton(ID_BTN_FONT_UP, D2D1::RectF(W - 56, ptY, W - 20, ptY + 26), L"A+", Theme::BgButton, Theme::Border, Theme::TextSecond, g.tfUiSmall.Get());

    // Teleprompter Viewport
    g.rectPrompterBox = D2D1::RectF(20, prompterY + 38, W - 20, prompterY + prompterTotalH - 8);
    float boxW = g.rectPrompterBox.right - g.rectPrompterBox.left;
    float boxH = g.rectPrompterBox.bottom - g.rectPrompterBox.top;
    float textColW = std::min(780.0f, boxW - 40.0f);

    if (!g.prompterLayout || std::fabs(g.prompterLayoutWidth - textColW) > 2.0f) {
        g.prompterLayoutWidth = textColW;
        g.prompterTotalHeight = 0.0f;
        if (!g.tfPrompter) RebuildPrompterTextFormat();
        if (g.dwFactory && g.tfPrompter) {
            g.dwFactory->CreateTextLayout(
                g.prompterDisplayText.c_str(),
                static_cast<UINT32>(g.prompterDisplayText.size()),
                g.tfPrompter.Get(), textColW, 100000.0f, &g.prompterLayout
            );
            if (g.prompterLayout) {
                DWRITE_TEXT_METRICS tm{};
                g.prompterLayout->GetMetrics(&tm);
                g.prompterTotalHeight = tm.height;
            }
        }
    }

    const float lineHeight = g.prompterFontSize * 1.38f;
    g.prompterMaxScroll = std::max(0.0f, g.prompterTotalHeight - lineHeight);
    const bool autoScrolling = captureLive && g.prompterAutoScroll && !g.isDraggingPrompter;
    if (autoScrolling) {
        g.prompterAutoSpeed = g.animationsEnabled ? EaseToward(g.prompterAutoSpeed, g.prompterSpeed, dt, 0.15f, 0.4f) : g.prompterSpeed;
        g.prompterScrollTarget += g.prompterAutoSpeed * dt;
    } else {
        g.prompterAutoSpeed = 0.0f;
    }
    g.prompterScrollTarget = std::clamp(g.prompterScrollTarget, 0.0f, g.prompterMaxScroll);
    g.prompterScrollY = std::clamp(g.prompterScrollY, 0.0f, g.prompterMaxScroll);
    if (g.animationsEnabled && !g.isDraggingPrompter) {
        g.prompterScrollY = EaseToward(g.prompterScrollY, g.prompterScrollTarget, dt, 0.054f, 0.15f);
    } else {
        g.prompterScrollY = g.prompterScrollTarget;
    }

    // Highlight active reading line right above microphone center
    float guideY = g.rectPrompterBox.top + boxH * 0.48f;
    g.brush->SetColor(ColorHex(0xFFFFFF, 0.045f));
    g.rt->FillRoundedRectangle(D2D1::RoundedRect(
        D2D1::RectF(g.rectPrompterBox.left + 8, guideY - 22, g.rectPrompterBox.right - 8, guideY + 22), 6, 6), g.brush.Get());
    g.brush->SetColor(ColorHex(0xD49B4B, 0.65f));
    g.rt->FillRectangle(D2D1::RectF(g.rectPrompterBox.left + 8, guideY - 8, g.rectPrompterBox.left + 13, guideY + 8), g.brush.Get());

    g.rt->PushAxisAlignedClip(g.rectPrompterBox, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    if (g.prompterLayout) {
        float textLeft = (g.rectPrompterBox.left + g.rectPrompterBox.right - textColW) * 0.5f;
        float textTop = guideY - lineHeight * 0.5f - g.prompterScrollY;
        g.brush->SetColor(Theme::TextPrimary);
        const auto textOptions = g.animationsEnabled && g.prompterScrollY != g.prompterScrollTarget
            ? D2D1_DRAW_TEXT_OPTIONS_NO_SNAP : D2D1_DRAW_TEXT_OPTIONS_NONE;
        g.rt->DrawTextLayout(D2D1::Point2F(textLeft, textTop), g.prompterLayout.Get(), g.brush.Get(), textOptions);
    }
    if (g.prompterEdge) {
        const auto& box = g.rectPrompterBox;
        constexpr float fadeHeight = 24.0f;
        g.prompterEdge->SetStartPoint({0, box.top});
        g.prompterEdge->SetEndPoint({0, box.top + fadeHeight});
        g.rt->FillRectangle(D2D1::RectF(box.left, box.top, box.right, box.top + fadeHeight), g.prompterEdge.Get());
        g.prompterEdge->SetStartPoint({0, box.bottom});
        g.prompterEdge->SetEndPoint({0, box.bottom - fadeHeight});
        g.rt->FillRectangle(D2D1::RectF(box.left, box.bottom - fadeHeight, box.right, box.bottom), g.prompterEdge.Get());
    }
    if (g.prompterMaxScroll > 0.0f && g.prompterTotalHeight > boxH) {
        const float position = g.prompterScrollY / g.prompterMaxScroll;
        const float trackHeight = boxH - 20.0f;
        const float thumbHeight = std::max(16.0f, trackHeight * boxH / g.prompterTotalHeight);
        const float thumbTop = g.rectPrompterBox.top + 10.0f + (trackHeight - thumbHeight) * position;
        const float trackX = g.rectPrompterBox.right - 5.0f;
        g.brush->SetColor(ColorHex(0x45423C));
        g.rt->DrawLine({trackX, g.rectPrompterBox.top + 10.0f}, {trackX, g.rectPrompterBox.bottom - 10.0f}, g.brush.Get(), 1.0f);
        g.brush->SetColor(ColorHex(0xD49B4B, 0.72f));
        g.rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(trackX - 1, thumbTop, trackX + 1, thumbTop + thumbHeight), 1, 1), g.brush.Get());
    }
    g.rt->PopAxisAlignedClip();

    // ========================================================================
    // 4. Studio Microphone Centerpiece & Transport
    // ========================================================================
    D2D1_RECT_F micPanel = D2D1::RectF(12, micRowY, W - 12, micRowY + micRowH);
    g.brush->SetColor(Theme::BgPanel);
    g.rt->FillRoundedRectangle(D2D1::RoundedRect(micPanel, 8, 8), g.brush.Get());
    g.brush->SetColor(Theme::Border);
    g.rt->DrawRoundedRectangle(D2D1::RoundedRect(micPanel, 8, 8), g.brush.Get(), 1.2f);
    if (g.recordPresence > 0.0f) {
        g.brush->SetColor(ColorHex(0xD94040, g.recordPresence * 0.45f));
        g.rt->DrawRoundedRectangle(D2D1::RoundedRect(micPanel, 8, 8), g.brush.Get(), 1.2f);
    }

    size_t totalSamples = 0;
    {
        std::lock_guard<std::mutex> lk(g.audioMutex);
        totalSamples = g.raw->samples.size();
    }
    size_t curSample = std::min(g.playheadSample.load(), totalSamples);
    double curSec = static_cast<double>(curSample) / kSampleRate;
    double totalSec = static_cast<double>(totalSamples) / kSampleRate;

    // Left Column: Timecode & Mode Status
    float leftColW = (W - 240.0f) * 0.45f;
    std::wstring timeStr = FormatTime(curSec) + L" / " + FormatTime(totalSec);
    g.tfMonoBig->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    g.tfMonoBig->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    g.brush->SetColor(captureLive ? ColorHex(0xFF8B82) : Theme::TextPrimary);
    g.rt->DrawTextW(timeStr.c_str(), static_cast<UINT32>(timeStr.size()), g.tfMonoBig.Get(),
                    D2D1::RectF(28, micRowY + 14, 28 + leftColW, micRowY + 62), g.brush.Get());

    std::wstring modeLabel;
    D2D1_COLOR_F modeColor = Theme::TextSecond;
    float modeLeft = 28.0f;
    if (captureLive) {
        const auto badge = D2D1::RectF(modeLeft, micRowY + 68, modeLeft + 76, micRowY + 91);
        g.brush->SetColor(Theme::RecordDark);
        g.rt->FillRoundedRectangle(D2D1::RoundedRect(badge, 4, 4), g.brush.Get());
        g.brush->SetColor(ColorHex(0xFF8B82));
        g.rt->FillEllipse(D2D1::Ellipse({modeLeft + 12, micRowY + 79.5f}, 3, 3), g.brush.Get());
        const wchar_t* badgeLabel = Text(L"REC", L"Запись");
        g.tfUiSmall->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        g.tfUiSmall->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        g.rt->DrawTextW(badgeLabel, static_cast<UINT32>(wcslen(badgeLabel)), g.tfUiSmall.Get(), D2D1::RectF(modeLeft + 22, badge.top, badge.right, badge.bottom), g.brush.Get());
        modeLeft = badge.right + 10;
        modeLabel = Text(L"Space to stop", L"Пробел — остановить");
    } else if (rec) {
        modeLabel = Text(L"Connecting microphone…", L"Подключаем микрофон…");
    } else if (play) {
        modeLabel = Text(L"Playing · Enter or Space to pause", L"Воспроизведение · Enter или пробел — пауза");
        modeColor = Theme::AccentCyan;
    } else if (g.hasSelection && g.selStartSample != g.selEndSample) {
        size_t s0 = std::min(g.selStartSample, g.selEndSample);
        size_t s1 = std::max(g.selStartSample, g.selEndSample);
        modeLabel = Text(L"Space to replace selection (", L"Пробел — перезаписать выделение (") + FormatTime(s0 / (double)kSampleRate) +
                    L" – " + FormatTime(s1 / (double)kSampleRate) + L")";
        modeColor = Theme::WarnAmber;
    } else if (totalSamples > 480 && curSample + 480 < totalSamples) {
        modeLabel = Text(L"Space to replace from ", L"Пробел — перезаписать с ") + FormatTime(curSec) + L" · Ctrl+Z";
        modeColor = Theme::WarnAmber;
    } else {
        modeLabel = Text(L"Space to start recording", L"Пробел — начать запись");
    }
    g.tfUi->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    g.tfUi->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    g.brush->SetColor(modeColor);
    g.rt->DrawTextW(modeLabel.c_str(), static_cast<UINT32>(modeLabel.size()), g.tfUi.Get(),
                    D2D1::RectF(modeLeft, micRowY + 64, 28 + leftColW, micRowY + 94), g.brush.Get());

    // Transport Buttons on left
    DrawButton(ID_BTN_REC_STOP, D2D1::RectF(28, micRowY + 98, 142, micRowY + 134),
               rec || play ? Text(L"Stop", L"Стоп") : Text(L"Record", L"Запись"),
               rec ? Theme::RecordDark : ColorHex(0x521C1C, 0.80f), Theme::RecordRed, Theme::TextPrimary, g.tfUiSmall.Get());
    DrawButton(ID_BTN_PLAY_STOP, D2D1::RectF(148, micRowY + 98, 244, micRowY + 134),
               play ? Text(L"Pause", L"Пауза") : g.playWhenProcessed ? Text(L"Cancel", L"Отмена") : Text(L"Play", L"Слушать"),
               play ? Theme::BgButtonActive : Theme::BgButton, play ? Theme::BorderActive : Theme::Border, Theme::TextPrimary, g.tfUiSmall.Get(), hasAudio && !rec);
    DrawButton(ID_BTN_REWIND_START, D2D1::RectF(250, micRowY + 98, 310, micRowY + 134),
               L"0:00", Theme::BgButton, Theme::Border, Theme::TextSecond, g.tfUiSmall.Get(), hasAudio && !rec);
    DrawButton(ID_BTN_JUMP_LOUD, D2D1::RectF(316, micRowY + 98, 430, micRowY + 134),
               Text(L"Last sound", L"К громкому"), Theme::BgButton, Theme::Border, Theme::TextPrimary, g.tfUiSmall.Get(), hasAudio && !rec);

    // Center: Illustrated Studio Microphone
    float micBoxW = 96.0f;
    g.rectMicCenter = D2D1::RectF((W - micBoxW) * 0.5f, micRowY + 6, (W + micBoxW) * 0.5f, micRowY + micRowH - 6);
    float rmsDb = g.liveRmsDb.load();
    float peakDb = g.livePeakDb.load();
    if (captureLive || play) {
        if (peakDb > g.peakHoldDb.load()) {
            g.peakHoldDb.store(peakDb);
            g.peakHoldTime = now;
        } else if (std::chrono::duration<float>(now - g.peakHoldTime).count() > 1.2f) {
            g.peakHoldDb.store(std::max(-60.0f, g.peakHoldDb.load() - 18.0f * dt));
        }
    } else {
        g.peakHoldDb.store(-60.0f);
    }
    const float peakFraction = std::clamp((peakDb + 60.0f) / 60.0f, 0.0f, 1.0f);
    const float rmsFraction = captureLive ? std::clamp((rmsDb + 50.0f) / 46.0f, 0.0f, 1.0f) : 0.0f;
    g.drawnPeakFraction = g.animationsEnabled ? EaseToward(g.drawnPeakFraction, peakFraction, dt, peakFraction > g.drawnPeakFraction ? 0.028f : 0.135f, 0.002f) : peakFraction;
    g.drawnRmsFraction = g.animationsEnabled ? EaseToward(g.drawnRmsFraction, rmsFraction, dt, 0.11f, 0.002f) : rmsFraction;
    DrawStudioMicrophone(g.rectMicCenter, captureLive);

    // Right Column: Live dBFS VU Meter & Delete Slice Button
    float rightColX = g.rectMicCenter.right + 28.0f;
    float rightColR = W - 28.0f;
    if (rightColR > rightColX + 120.0f) {
        wchar_t dbText[64];
        swprintf_s(dbText, Text(L"Signal level: %.1f dBFS  (target −12…−6 dB)", L"Уровень сигнала: %.1f dBFS  (цель −12…−6 дБ)"), peakDb);
        g.tfUiSmall->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        g.brush->SetColor(Theme::TextSecond);
        g.rt->DrawTextW(dbText, static_cast<UINT32>(wcslen(dbText)), g.tfUiSmall.Get(),
                        D2D1::RectF(rightColX, micRowY + 18, rightColR, micRowY + 42), g.brush.Get());

        // Meter bar (-60 .. 0 dB)
        D2D1_RECT_F meterBg = D2D1::RectF(rightColX, micRowY + 48, rightColR, micRowY + 68);
        g.brush->SetColor(ColorHex(0x121212));
        g.rt->FillRoundedRectangle(D2D1::RoundedRect(meterBg, 4, 4), g.brush.Get());

        float meterW = meterBg.right - meterBg.left;
        // Sweet spot marker (-12..-6 dB => 0.80..0.90)
        g.brush->SetColor(ColorHex(0x3FA36C, 0.20f));
        g.rt->FillRectangle(D2D1::RectF(meterBg.left + meterW * 0.80f, meterBg.top,
                                        meterBg.left + meterW * 0.90f, meterBg.bottom), g.brush.Get());

        float fillFrac = g.drawnPeakFraction;
        D2D1_COLOR_F meterColor = Theme::GoodGreen;
        if (peakDb > -1.0f) meterColor = Theme::RecordRed;
        else if (peakDb > -6.0f) meterColor = Theme::WarnAmber;

        if (fillFrac > 0.005f) {
            g.brush->SetColor(meterColor);
            g.rt->FillRoundedRectangle(D2D1::RoundedRect(
                D2D1::RectF(meterBg.left, meterBg.top, meterBg.left + meterW * fillFrac, meterBg.bottom), 4, 4), g.brush.Get());
        }

        // Peak hold tick
        float holdFrac = std::clamp((g.peakHoldDb.load() + 60.0f) / 60.0f, 0.0f, 1.0f);
        if (holdFrac > 0.01f) {
            float hx = meterBg.left + meterW * holdFrac;
            g.brush->SetColor(Theme::TextPrimary);
            g.rt->FillRectangle(D2D1::RectF(hx - 1.5f, meterBg.top, hx + 1.5f, meterBg.bottom), g.brush.Get());
        }

        g.brush->SetColor(Theme::Border);
        g.rt->DrawRoundedRectangle(D2D1::RoundedRect(meterBg, 4, 4), g.brush.Get(), 1.0f);

        bool canDelSel = !rec && g.hasSelection && (g.selStartSample != g.selEndSample);
        DrawButton(ID_BTN_DEL_SEL, D2D1::RectF(rightColX, micRowY + 98, rightColX + 218, micRowY + 134),
                   Text(L"Delete selection (Del)", L"Удалить выделение (Del)"), Theme::BgButton, Theme::Border, Theme::TextPrimary, g.tfUiSmall.Get(), canDelSel);
    }

    // ========================================================================
    // 5. Interactive Waveform Area
    // ========================================================================
    D2D1_RECT_F wavePanel = D2D1::RectF(12, waveY, W - 12, waveY + waveH);
    g.brush->SetColor(Theme::BgPanel);
    g.rt->FillRoundedRectangle(D2D1::RoundedRect(wavePanel, 8, 8), g.brush.Get());
    g.brush->SetColor(Theme::Border);
    g.rt->DrawRoundedRectangle(D2D1::RoundedRect(wavePanel, 8, 8), g.brush.Get(), 1.0f);

    // Waveform header strip (separated cleanly from time ruler below)
    g.tfUiSmall->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    g.tfUiSmall->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    std::wstring waveHeaderLabel = Text(L"Waveform", L"Звуковая волна");
    if (!rec && g.hasSelection && g.selStartSample != g.selEndSample) {
        size_t s0 = std::min(g.selStartSample, g.selEndSample);
        size_t s1 = std::max(g.selStartSample, g.selEndSample);
        waveHeaderLabel += Text(L"  ·  Selected: ", L"  ·  Выделено: ") + FormatTime(s0 / (double)kSampleRate) +
                           L" – " + FormatTime(s1 / (double)kSampleRate);
    }
    DrawWaveControlLabel(waveHeaderLabel, D2D1::RectF(kWavePlotLeft + 2, waveY + 4, W - 509, waveY + 28), Theme::TextSecond);

    const float masterGainDb = g.masterGainDb.load();
    const float masterGain = std::pow(10.0f, masterGainDb / 20.0f);
    const float outputCeiling = WaveformOutputCeiling();
    if (hasAudio) {
        const float unboundedPeak = WaveformPeakAbs() * masterGain;
        const bool clipping = outputCeiling >= 1.0f && unboundedPeak > 1.0f;
        const float peak = std::min(unboundedPeak, outputCeiling);
        wchar_t peakText[32];
        if (clipping) swprintf_s(peakText, Text(L"Peak +%.1f dBFS", L"Пик +%.1f дБFS"), 20.0f * std::log10(unboundedPeak));
        else if (peak > 0) swprintf_s(peakText, Text(L"Peak %.1f dBFS", L"Пик %.1f дБFS"), 20.0f * std::log10(peak));
        else wcscpy_s(peakText, Text(L"Peak −∞ dBFS", L"Пик −∞ дБFS"));
        DrawWaveControlLabel(peakText, D2D1::RectF(W - 505, waveY + 4, W - 392, waveY + 28),
                             clipping ? ColorHex(0xFF8B82) : Theme::TextSecond, DWRITE_TEXT_ALIGNMENT_TRAILING);
    }

    DrawButton(ID_BTN_SILENCE_TOOLS, D2D1::RectF(W - 384, waveY + 5, W - 272, waveY + 27), Text(L"Silence", L"Тишина"),
               showSilenceTools ? Theme::BgButtonActive : Theme::BgButton,
               showSilenceTools ? Theme::BorderActive : Theme::Border, Theme::TextPrimary, g.tfUiSmall.Get(),
               !rec && !g.silenceApplyPending && (hasAudio || showSilenceTools));

    DrawButton(ID_BTN_WAVE_ZOOM_IN, D2D1::RectF(W - 264, waveY + 5, W - 232, waveY + 27), L"", Theme::BgButton, Theme::Border, Theme::TextPrimary, g.tfUiSmall.Get(), hasAudio && !rec);
    DrawButton(ID_BTN_WAVE_ZOOM_OUT, D2D1::RectF(W - 228, waveY + 5, W - 196, waveY + 27), L"", Theme::BgButton, Theme::Border, Theme::TextPrimary, g.tfUiSmall.Get(), hasAudio && !rec);
    DrawButton(ID_BTN_WAVE_ZOOM_ALL, D2D1::RectF(W - 192, waveY + 5, W - 96, waveY + 27), Text(L"Fit track", L"Весь трек"), Theme::BgButton, Theme::Border, Theme::TextSecond, g.tfUiSmall.Get(), hasAudio && !rec);
    DrawButton(ID_BTN_WAVE_DESELECT, D2D1::RectF(W - 92, waveY + 5, W - 20, waveY + 27), Text(L"Clear", L"Снять"), Theme::BgButton, Theme::Border, Theme::TextSecond, g.tfUiSmall.Get(), !rec && g.hasSelection);

    g.brush->SetColor(ColorHex(0x2C2C2C));
    g.rt->DrawLine(D2D1::Point2F(18, waveY + 30), D2D1::Point2F(W - 18, waveY + 30), g.brush.Get(), 1.0f);

    if (showSilenceTools) DrawSilenceTools(waveY + 32, W, hasAudio);
    const float waveTop = waveY + 32 + silenceToolsHeight;
    const float waveBottom = waveY + waveH - 8;
    DrawMasterGain(waveY, waveTop, waveBottom, masterGainDb);
    g.rectWaveArea = D2D1::RectF(kWavePlotLeft, waveTop, W - 20, waveBottom);
    const float rulerH = 18.0f;
    float waveBodyTop = g.rectWaveArea.top + rulerH;
    float wWidth = g.rectWaveArea.right - g.rectWaveArea.left;
    float wBodyHeight = std::max(20.0f, g.rectWaveArea.bottom - waveBodyTop);
    float wMidY = (waveBodyTop + g.rectWaveArea.bottom) * 0.5f;
    const float halfWaveHeight = wBodyHeight * 0.46f;
    const auto sampleY = [=](float value) {
        return wMidY - std::clamp(value * masterGain, -outputCeiling, outputCeiling) * halfWaveHeight;
    };

    const double targetDuration = (g.viewDurationSec > 0.1) ? g.viewDurationSec : std::max(10.0, totalSec * 1.04);
    const double maxViewStart = std::max(0.0, totalSec - targetDuration);
    if (rec && (curSec < g.viewStartSec || curSec > g.viewStartSec + targetDuration * 0.9)) {
        g.viewStartSec = curSec - targetDuration * 0.9;
    }
    g.viewStartSec = std::clamp(g.viewStartSec, 0.0, maxViewStart);
    if (g.animationsEnabled && g.animatingWaveView && !rec && g.drawnViewDurationSec > 0.0) {
        g.drawnViewStartSec = EaseToward(g.drawnViewStartSec, g.viewStartSec, dt, 0.049f, targetDuration * 0.0002);
        g.drawnViewDurationSec = EaseToward(g.drawnViewDurationSec, targetDuration, dt, 0.049f, targetDuration * 0.0002);
        g.animatingWaveView = g.drawnViewStartSec != g.viewStartSec || g.drawnViewDurationSec != targetDuration;
    } else {
        g.drawnViewStartSec = g.viewStartSec;
        g.drawnViewDurationSec = targetDuration;
        g.animatingWaveView = false;
    }
    const double viewStartSec = g.drawnViewStartSec;
    const double effDurSec = g.drawnViewDurationSec;

    // Everything inside waveform (ruler ticks/labels, waveform buckets, selection, playhead) is strictly clipped
    g.rt->PushAxisAlignedClip(g.rectWaveArea, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    // Subtle time ruler background strip & center line
    g.brush->SetColor(ColorHex(0x181818, 0.85f));
    g.rt->FillRectangle(D2D1::RectF(g.rectWaveArea.left, g.rectWaveArea.top, g.rectWaveArea.right, waveBodyTop), g.brush.Get());
    g.brush->SetColor(ColorHex(0x2F2F2F));
    g.rt->DrawLine(D2D1::Point2F(g.rectWaveArea.left, wMidY), D2D1::Point2F(g.rectWaveArea.right, wMidY), g.brush.Get(), 1.0f);
    if (showSilenceTools) DrawSilencePreview(viewStartSec, effDurSec, waveBodyTop);

    double stepSec = 1.0;
    if (effDurSec > 600) stepSec = 60.0;
    else if (effDurSec > 180) stepSec = 30.0;
    else if (effDurSec > 60) stepSec = 10.0;
    else if (effDurSec > 20) stepSec = 5.0;
    else if (effDurSec > 8) stepSec = 2.0;

    double firstTick = std::ceil(viewStartSec / stepSec) * stepSec;
    g.tfUiSmall->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    g.tfUiSmall->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    for (double t = firstTick; t <= viewStartSec + effDurSec; t += stepSec) {
        float x = g.rectWaveArea.left + static_cast<float>((t - viewStartSec) / effDurSec) * wWidth;
        g.brush->SetColor(ColorHex(0x2A2A2A));
        g.rt->DrawLine(D2D1::Point2F(x, g.rectWaveArea.top), D2D1::Point2F(x, g.rectWaveArea.bottom), g.brush.Get(), 1.0f);
        if (x + 42.0f <= g.rectWaveArea.right) {
            std::wstring lbl = FormatTime(t, false);
            g.brush->SetColor(Theme::TextMuted);
            g.rt->DrawTextW(lbl.c_str(), static_cast<UINT32>(lbl.size()), g.tfUiSmall.Get(),
                            D2D1::RectF(x + 4, g.rectWaveArea.top, std::min(x + 58.0f, g.rectWaveArea.right - 2.0f), waveBodyTop),
                            g.brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
    }

    // Draw High-Detail Waveform
    {
        std::lock_guard<std::mutex> lk(g.audioMutex);
        const auto& track = CurrentTrackLocked();
        const auto& samples = track.samples;
        const auto& buckets = track.wave;
        if (!samples.empty() && wWidth > 10.0f) {
            int pixW = static_cast<int>(wWidth);
            double startSampleF = viewStartSec * kSampleRate;
            double endSampleF = (viewStartSec + effDurSec) * kSampleRate;
            double samplesPerPx = (endSampleF - startSampleF) / pixW;

            if (samplesPerPx <= 2.0) {
                // Ultra-close zoom: draw exact sample-to-sample waveform curve
                size_t sStart = static_cast<size_t>(std::max(0.0, std::floor(startSampleF)));
                size_t sEnd = std::min(samples.size() - 1, static_cast<size_t>(std::ceil(endSampleF)) + 1);
                if (sEnd > sStart) {
                    g.brush->SetColor(Theme::WaveRms);
                    float prevX = g.rectWaveArea.left + static_cast<float>((static_cast<double>(sStart) - startSampleF) / samplesPerPx);
                    float prevY = sampleY(samples[sStart]);
                    for (size_t s = sStart + 1; s <= sEnd; ++s) {
                        float curX = g.rectWaveArea.left + static_cast<float>((static_cast<double>(s) - startSampleF) / samplesPerPx);
                        float curY = sampleY(samples[s]);
                        g.rt->DrawLine(D2D1::Point2F(prevX, prevY), D2D1::Point2F(curX, curY), g.brush.Get(), 1.3f);
                        prevX = curX;
                        prevY = curY;
                    }
                }
            } else if (samplesPerPx <= static_cast<double>(kWaveformBucketSize)) {
                // Detailed zoom: compute min/max/RMS directly from the displayed take.
                for (int px = 0; px < pixW; ++px) {
                    double sA = startSampleF + px * samplesPerPx;
                    double sB = sA + samplesPerPx;
                    if (sB <= 0.0 || sA >= static_cast<double>(samples.size())) continue;

                    size_t s0 = static_cast<size_t>(std::max(0.0, sA));
                    size_t s1 = std::min(samples.size(), static_cast<size_t>(std::ceil(std::max(0.0, sB))));
                    if (s1 <= s0) s1 = std::min(samples.size(), s0 + 1);

                    float mn = 1.0f, mx = -1.0f;
                    double sumSq = 0.0;
                    for (size_t s = s0; s < s1; ++s) {
                        float v = samples[s];
                        if (v < mn) mn = v;
                        if (v > mx) mx = v;
                        sumSq += static_cast<double>(v) * static_cast<double>(v);
                    }
                    float rms = static_cast<float>(std::sqrt(sumSq / std::max<size_t>(1, s1 - s0)));

                    float x = g.rectWaveArea.left + px + 0.5f;
                    float yTop = sampleY(mx);
                    float yBot = sampleY(mn);
                    if (yBot - yTop < 1.2f) {
                        yTop = wMidY - 0.6f;
                        yBot = wMidY + 0.6f;
                    }
                    g.brush->SetColor(Theme::WaveFill);
                    g.rt->DrawLine(D2D1::Point2F(x, yTop), D2D1::Point2F(x, yBot), g.brush.Get(), 1.0f);

                    float rH = wMidY - sampleY(rms);
                    if (rH > 0.8f) {
                        g.brush->SetColor(Theme::WaveRms);
                        g.rt->DrawLine(D2D1::Point2F(x, wMidY - rH), D2D1::Point2F(x, wMidY + rH), g.brush.Get(), 1.0f);
                    }
                }
            } else {
                // Wide overview: fast 64-sample bucket cache
                for (int px = 0; px < pixW; ++px) {
                    double sA = startSampleF + px * samplesPerPx;
                    double sB = sA + samplesPerPx;
                    if (sB <= 0.0 || sA >= static_cast<double>(samples.size())) continue;

                    size_t b0 = static_cast<size_t>(std::max(0.0, sA)) / kWaveformBucketSize;
                    size_t b1 = static_cast<size_t>(std::min(static_cast<double>(samples.size() - 1), std::max(0.0, sB))) / kWaveformBucketSize;
                    b0 = std::min(b0, buckets.size() - 1);
                    b1 = std::min(b1, buckets.size() - 1);

                    float mn = 1.0f, mx = -1.0f, rmsMax = 0.0f;
                    for (size_t b = b0; b <= b1; ++b) {
                        mn = std::min(mn, buckets[b].minVal);
                        mx = std::max(mx, buckets[b].maxVal);
                        rmsMax = std::max(rmsMax, buckets[b].rmsVal);
                    }

                    float x = g.rectWaveArea.left + px + 0.5f;
                    float yTop = sampleY(mx);
                    float yBot = sampleY(mn);
                    if (yBot - yTop < 1.5f) {
                        yTop = wMidY - 0.75f;
                        yBot = wMidY + 0.75f;
                    }
                    g.brush->SetColor(Theme::WaveFill);
                    g.rt->DrawLine(D2D1::Point2F(x, yTop), D2D1::Point2F(x, yBot), g.brush.Get(), 1.0f);

                    float rH = wMidY - sampleY(rmsMax);
                    if (rH > 1.0f) {
                        g.brush->SetColor(Theme::WaveRms);
                        g.rt->DrawLine(D2D1::Point2F(x, wMidY - rH), D2D1::Point2F(x, wMidY + rH), g.brush.Get(), 1.0f);
                    }
                }
            }
        } else {
            g.tfUi->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            g.tfUi->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            g.brush->SetColor(Theme::TextMuted);
            const std::wstring emptyHint = Text(L"Your waveform appears here. Drag to select part of the recording.",
                                                L"Здесь появится звуковая волна. Потяните мышью, чтобы выделить часть записи.");
            g.rt->DrawTextW(emptyHint.c_str(), static_cast<UINT32>(emptyHint.size()), g.tfUi.Get(),
                            D2D1::RectF(g.rectWaveArea.left, waveBodyTop, g.rectWaveArea.right, g.rectWaveArea.bottom), g.brush.Get());
        }
    }

    // Selection overlay
    if (!rec && g.hasSelection && g.selStartSample != g.selEndSample) {
        size_t s0 = std::min(g.selStartSample, g.selEndSample);
        size_t s1 = std::max(g.selStartSample, g.selEndSample);
        float x0 = g.rectWaveArea.left + static_cast<float>((s0 / (double)kSampleRate - viewStartSec) / effDurSec) * wWidth;
        float x1 = g.rectWaveArea.left + static_cast<float>((s1 / (double)kSampleRate - viewStartSec) / effDurSec) * wWidth;
        g.brush->SetColor(Theme::SelOverlay);
        g.rt->FillRectangle(D2D1::RectF(x0, g.rectWaveArea.top, x1, g.rectWaveArea.bottom), g.brush.Get());
        g.brush->SetColor(Theme::AccentCyan);
        g.rt->DrawLine(D2D1::Point2F(x0, g.rectWaveArea.top), D2D1::Point2F(x0, g.rectWaveArea.bottom), g.brush.Get(), 1.5f);
        g.rt->DrawLine(D2D1::Point2F(x1, g.rectWaveArea.top), D2D1::Point2F(x1, g.rectWaveArea.bottom), g.brush.Get(), 1.5f);
    }

    // Playhead cursor
    float playX = g.rectWaveArea.left + static_cast<float>((curSec - viewStartSec) / effDurSec) * wWidth;
    if (playX >= g.rectWaveArea.left - 2 && playX <= g.rectWaveArea.right + 2) {
        g.brush->SetColor(captureLive ? Theme::RecordRed : Theme::WarnAmber);
        g.rt->DrawLine(D2D1::Point2F(playX, g.rectWaveArea.top), D2D1::Point2F(playX, g.rectWaveArea.bottom), g.brush.Get(), 2.0f);
        g.rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(playX, g.rectWaveArea.top + 4), 4.5f, 4.5f), g.brush.Get());
    }
    g.rt->PopAxisAlignedClip();

    // ========================================================================
    // 6. Bottom Effects & Export Dock
    // ========================================================================
    D2D1_RECT_F dockPanel = D2D1::RectF(12, bottomDockY, W - 12, bottomDockY + bottomDockH);
    g.brush->SetColor(Theme::BgPanel);
    g.rt->FillRoundedRectangle(D2D1::RoundedRect(dockPanel, 8, 8), g.brush.Get());
    g.brush->SetColor(Theme::Border);
    g.rt->DrawRoundedRectangle(D2D1::RoundedRect(dockPanel, 8, 8), g.brush.Get(), 1.0f);

    const bool canEditEffects = hasAudio && !g.isRecording.load();
    float fxY = bottomDockY + 6;
    constexpr float fxH = 28;
    float fxX = 20;
    auto fxBg = [](bool on) { return on ? Theme::BgButtonActive : Theme::BgButton; };
    auto fxBd = [](bool on) { return on ? Theme::BorderActive : Theme::Border; };
    auto drawFxToggle = [&](int id, const wchar_t* label, bool on, float width) {
        DrawButton(id, D2D1::RectF(fxX, fxY, fxX + width, fxY + fxH), label,
                   fxBg(on), fxBd(on), on ? Theme::AccentCyan : Theme::TextPrimary, g.tfUiSmall.Get(), canEditEffects);
        fxX += width + 6;
    };
    drawFxToggle(ID_BTN_FX_CLEANUP, Text(L"Voice cleanup", L"Очистка звука"), g.effectSettings.cleanup, 132);
    drawFxToggle(ID_BTN_FX_FADE_IN, Text(L"Fade in", L"Фейд-ин"), g.effectSettings.fadeIn, 84);
    drawFxToggle(ID_BTN_FX_FADE_OUT, Text(L"Fade out", L"Фейд-аут"), g.effectSettings.fadeOut, 88);
    drawFxToggle(ID_BTN_FX_ECHO, Text(L"Echo", L"Эхо"), g.effectSettings.echo, 72);

    // Inline 3-band EQ controls
    float eqX = fxX + 4;
    auto drawEqBand = [&](int decId, int incId, const wchar_t* name, float valDb) {
        wchar_t buf[32];
        swprintf_s(buf, L"%ls %+.1f", name, valDb);
        DrawButton(decId, D2D1::RectF(eqX, fxY + 2, eqX + 20, fxY + fxH - 2), L"−", Theme::BgButton, Theme::Border, Theme::TextSecond, g.tfUiSmall.Get(), canEditEffects);
        g.tfUiSmall->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        g.tfUiSmall->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        g.brush->SetColor(canEditEffects ? Theme::TextPrimary : Theme::TextMuted);
        g.rt->DrawTextW(buf, static_cast<UINT32>(wcslen(buf)), g.tfUiSmall.Get(), D2D1::RectF(eqX + 20, fxY, eqX + 74, fxY + fxH), g.brush.Get());
        DrawButton(incId, D2D1::RectF(eqX + 74, fxY + 2, eqX + 94, fxY + fxH - 2), L"+", Theme::BgButton, Theme::Border, Theme::TextSecond, g.tfUiSmall.Get(), canEditEffects);
        eqX += 100;
    };
    drawEqBand(ID_BTN_EQ_LOW_DEC, ID_BTN_EQ_LOW_INC, Text(L"Low", L"Низ"), g.effectSettings.eq.lowDb);
    drawEqBand(ID_BTN_EQ_MID_DEC, ID_BTN_EQ_MID_INC, Text(L"Mid", L"Сер"), g.effectSettings.eq.midDb);
    drawEqBand(ID_BTN_EQ_HIGH_DEC, ID_BTN_EQ_HIGH_INC, Text(L"Hi", L"Выс"), g.effectSettings.eq.highDb);

    fxX = eqX;
    drawFxToggle(ID_BTN_FX_EQ_APPLY, g.effectSettings.equalizer ? Text(L"EQ on", L"EQ вкл.") : Text(L"EQ off", L"EQ выкл."), g.effectSettings.equalizer, 96);
    DrawButton(ID_BTN_FX_EQ_PRESET, D2D1::RectF(fxX, fxY, fxX + 114, fxY + fxH), Text(L"Voice preset", L"Пресет Голос"),
               Theme::BgButton, Theme::Border, Theme::TextSecond, g.tfUiSmall.Get(), canEditEffects);

    fxX = 20;
    fxY += 34;
    drawFxToggle(ID_BTN_FX_HIGHPASS, Text(L"Low cut", L"Срез гула"), g.effectSettings.highPass, 112);
    drawFxToggle(ID_BTN_FX_GATE, Text(L"Noise gate", L"Шумовой гейт"), g.effectSettings.gate, 128);
    drawFxToggle(ID_BTN_FX_DEESSER, Text(L"De-esser", L"Де-эссер"), g.effectSettings.deEsser, 112);
    drawFxToggle(ID_BTN_FX_COMPRESSOR, Text(L"Compressor", L"Компрессор"), g.effectSettings.compressor, 124);
    drawFxToggle(ID_BTN_FX_LIMITER, Text(L"Limiter", L"Лимитер"), g.effectSettings.limiter, 104);
    if (EffectsPending()) {
        DrawWaveControlLabel(Text(L"Processing effects…", L"Обработка эффектов…"), D2D1::RectF(fxX + 8, fxY, W - 20, fxY + fxH), Theme::AccentCyan);
    }

    // Status & Export
    float expY = bottomDockY + 74;
    float expH = 32;
    g.tfUiSmall->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    g.tfUiSmall->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    g.brush->SetColor(g.statusIsError ? Theme::RecordRed : Theme::TextSecond);
    g.rt->DrawTextW(g.statusText.c_str(), static_cast<UINT32>(g.statusText.size()), g.tfUiSmall.Get(),
                    D2D1::RectF(22, expY, W - 445, expY + expH), g.brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);

    DrawButton(ID_BTN_WAV_BITDEPTH, D2D1::RectF(W - 436, expY + 2, W - 318, expY + expH - 2),
               g.wavFloat32 ? Text(L"32-bit float", L"32-бит float") : Text(L"24-bit PCM", L"24-бит PCM"),
               Theme::BgButton, Theme::Border, Theme::TextPrimary, g.tfUiSmall.Get());
    DrawButton(ID_BTN_EXPORT_WAV, D2D1::RectF(W - 312, expY + 2, W - 178, expY + expH - 2),
               Text(L"Export WAV", L"Экспорт WAV"), Theme::BgButtonActive, Theme::BorderActive, Theme::TextPrimary, g.tfUiSmall.Get(), hasAudio);
    DrawButton(ID_BTN_EXPORT_MP3, D2D1::RectF(W - 172, expY + 2, W - 20, expY + expH - 2),
               Text(L"MP3 (320 kbps)", L"MP3 (320 кбит/с)"), Theme::BgButtonActive, Theme::BorderActive, Theme::TextPrimary, g.tfUiSmall.Get(), hasAudio);

    HRESULT hr = g.rt->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        g.prompterEdge.Reset();
        g.rt.Reset();
        g.brush.Reset();
    }
}

// ============================================================================
// Mouse & Keyboard Interaction
// ============================================================================

static int HitTestControl(float x, float y) {
    for (auto it = g.hitRects.rbegin(); it != g.hitRects.rend(); ++it) {
        const auto& r = it->rect;
        if (x >= r.left && x <= r.right && y >= r.top && y <= r.bottom) {
            return it->id;
        }
    }
    return -1;
}

static size_t WaveXToSample(float x) {
    size_t total = 0;
    {
        std::lock_guard<std::mutex> lk(g.audioMutex);
        total = g.raw->samples.size();
    }
    float w = std::max(1.0f, g.rectWaveArea.right - g.rectWaveArea.left);
    double rel = std::clamp(static_cast<double>(x - g.rectWaveArea.left) / w, 0.0, 1.0);
    double sec = g.drawnViewStartSec + rel * g.drawnViewDurationSec;
    size_t sample = static_cast<size_t>(std::max(0.0, sec * kSampleRate));
    return std::min(sample, total);
}

static void SetWaveView(double start, double duration) {
    g.viewStartSec = start;
    g.viewDurationSec = duration;
    g.animatingWaveView = g.animationsEnabled;
    EnsureMotionTimer();
    InvalidateRect(g.hwndMain, nullptr, FALSE);
}

static double WaveTrackSeconds() {
    std::lock_guard<std::mutex> lock(g.audioMutex);
    return static_cast<double>(g.raw->samples.size()) / kSampleRate;
}

static void ZoomWave(double factor, double anchorFrac, bool anchorPlayhead = false) {
    const double total = WaveTrackSeconds();
    if (total <= 0.0) return;
    const double fit = std::max(10.0, total * 1.04);
    const double duration = g.viewDurationSec > 0.1 ? g.viewDurationSec : fit;
    const double nextDuration = std::clamp(duration * factor, 2.0, fit);
    if (nextDuration >= fit) {
        SetWaveView(0.0, 0.0);
        return;
    }
    const double anchorSec = anchorPlayhead ? static_cast<double>(g.playheadSample.load()) / kSampleRate
                                           : g.viewStartSec + anchorFrac * duration;
    const double start = std::clamp(anchorSec - anchorFrac * nextDuration, 0.0, std::max(0.0, total - nextDuration));
    SetWaveView(start, nextDuration);
}

static bool InRect(float x, float y, const D2D1_RECT_F& rect) {
    return x >= rect.left && x <= rect.right && y >= rect.top && y <= rect.bottom;
}

static void DragMasterGainTo(float y) {
    const auto& track = g.rectMasterGainTrack;
    const float amount = std::clamp((track.bottom - y) / (track.bottom - track.top), 0.0f, 1.0f);
    const float db = std::lerp(kMinMasterGainDb, kMaxMasterGainDb, amount);
    SetMasterGainDb(std::round(db * 2.0f) * 0.5f);
}

static void ChangeSilenceThreshold(float db) {
    if (!g.silenceToolsOpen || g.isRecording.load() || g.silenceApplyPending) return;
    const float next = std::clamp(std::round(db), kMinSilenceThresholdDb, kMaxSilenceThresholdDb);
    if (g.silenceThresholdDb != next) {
        g.silenceThresholdDb = next;
        MarkDocumentChanged();
        RequestSilencePreview(next);
        InvalidateRect(g.hwndMain, nullptr, FALSE);
    }
}

static void DragSilenceThresholdTo(float x) {
    const auto& track = g.rectSilenceThresholdTrack;
    const float fraction = std::clamp((x - track.left) / (track.right - track.left), 0.0f, 1.0f);
    ChangeSilenceThreshold(std::lerp(kMinSilenceThresholdDb, kMaxSilenceThresholdDb, fraction));
}

static void DismissSilenceTools() {
    if (g.silenceApplyPending) return;
    CloseSilenceTools();
}

static void HandleControlClick(int id) {
    switch (id) {
        case ID_BTN_LANGUAGE:
            for (const auto& hr : g.hitRects) if (hr.id == id) { ShowLanguageMenu(hr.rect); break; }
            break;
        case ID_BTN_FILE_MENU:
            for (const auto& hr : g.hitRects) if (hr.id == id) { ShowFileMenu(hr.rect); break; }
            break;
        case ID_BTN_INPUT_DEV:
            for (const auto& hr : g.hitRects) if (hr.id == id) { ShowDeviceMenu(true, hr.rect); break; }
            break;
        case ID_BTN_OUTPUT_DEV:
            for (const auto& hr : g.hitRects) if (hr.id == id) { ShowDeviceMenu(false, hr.rect); break; }
            break;
        case ID_BTN_REFRESH_DEV:
            RefreshDevices(false);
            SetStatus(Text(L"↻ Audio device list refreshed.", L"↻ Список аудиоустройств обновлён."));
            break;
        case ID_BTN_UNDO: DoUndo(); break;
        case ID_BTN_CLEAR_AUDIO: DoClearAudio(); break;

        case ID_BTN_OPEN_TEXT: DoOpenTextFile(); break;
        case ID_BTN_PASTE_TEXT: DoPasteClipboardText(); break;
        case ID_BTN_EDIT_TEXT: DoOpenTextEditorModal(); break;
        case ID_BTN_LOAD_LATEST_EP: {
            std::wstring p = FindLatestEpisodeScript();
            if (!p.empty()) {
                std::wstring txt = ReadTextFileSmart(p);
                if (!txt.empty()) {
                    LoadScriptIntoPrompter(txt, Text(L"Script loaded: ", L"Загружен сценарий: ") + fs::path(p).parent_path().filename().wstring());
                    break;
                }
            }
            SetStatus(Text(L"No scripts found in episodes/. Use Script or Edit to add one.", L"Сценарии в папке episodes/ не найдены — используйте Открыть или Редактор."), true);
            break;
        }
        case ID_BTN_PROMPTER_TOP:
            ScrollPrompter(0.0f, true);
            break;
        case ID_BTN_AUTOSCROLL:
            g.prompterAutoScroll = !g.prompterAutoScroll;
            MarkDocumentChanged();
            InvalidateRect(g.hwndMain, nullptr, FALSE);
            break;
        case ID_BTN_SPEED_DOWN:
            g.prompterSpeed = std::max(kMinPrompterSpeed, g.prompterSpeed - kPrompterSpeedStep);
            MarkDocumentChanged();
            InvalidateRect(g.hwndMain, nullptr, FALSE);
            break;
        case ID_BTN_SPEED_UP:
            g.prompterSpeed = std::min(kMaxPrompterSpeed, g.prompterSpeed + kPrompterSpeedStep);
            MarkDocumentChanged();
            InvalidateRect(g.hwndMain, nullptr, FALSE);
            break;
        case ID_BTN_FONT_DOWN:
            g.prompterFontSize = std::max(16.0f, g.prompterFontSize - 2.0f);
            MarkDocumentChanged();
            RebuildPrompterTextFormat();
            InvalidateRect(g.hwndMain, nullptr, FALSE);
            break;
        case ID_BTN_FONT_UP:
            g.prompterFontSize = std::min(46.0f, g.prompterFontSize + 2.0f);
            MarkDocumentChanged();
            RebuildPrompterTextFormat();
            InvalidateRect(g.hwndMain, nullptr, FALSE);
            break;

        case ID_BTN_MIC_TOGGLE:
        case ID_BTN_REC_STOP:
            if (g.isRecording.load() || g.isPlaying.load()) RequestTransportStop();
            else StartRecording();
            break;
        case ID_BTN_PLAY_STOP: TogglePlayback(); break;
        case ID_BTN_DEL_SEL: DoDeleteSelection(); break;
        case ID_BTN_REWIND_START:
            if (g.isRecording.load() || g.isPlaying.load()) StopAudioTransport();
            g.playheadSample.store(0);
            g.hasSelection = false;
            g.viewStartSec = 0.0;
            g.animatingWaveView = false;
            InvalidateRect(g.hwndMain, nullptr, FALSE);
            break;
        case ID_BTN_JUMP_LOUD:
            g.animatingWaveView = false;
            DoJumpToLastLoudMoment();
            break;

        case ID_BTN_WAVE_ZOOM_IN:
            ZoomWave(0.65, 0.5, true);
            break;
        case ID_BTN_WAVE_ZOOM_OUT:
            ZoomWave(1.0 / 0.65, 0.5);
            break;
        case ID_BTN_WAVE_ZOOM_ALL:
            SetWaveView(0.0, 0.0);
            break;
        case ID_BTN_WAVE_DESELECT:
            g.hasSelection = false;
            InvalidateRect(g.hwndMain, nullptr, FALSE);
            break;
        case ID_BTN_MASTER_GAIN_RESET:
            SetMasterGainDb(0.0f);
            break;
        case ID_BTN_SILENCE_TOOLS:
            if (g.silenceToolsOpen) {
                DismissSilenceTools();
            } else if (!g.isRecording.load()) {
                g.silenceToolsOpen = true;
                RequestSilencePreview(g.silenceThresholdDb);
                InvalidateRect(g.hwndMain, nullptr, FALSE);
            }
            break;
        case ID_BTN_SILENCE_APPLY:
            ApplySilencePreview();
            break;
        case ID_BTN_SILENCE_CLOSE:
            DismissSilenceTools();
            break;

        case ID_BTN_FX_CLEANUP: ToggleEffectAction(FxKind::Cleanup); break;
        case ID_BTN_FX_FADE_IN: ToggleEffectAction(FxKind::FadeIn); break;
        case ID_BTN_FX_FADE_OUT: ToggleEffectAction(FxKind::FadeOut); break;
        case ID_BTN_FX_ECHO: ToggleEffectAction(FxKind::Echo); break;
        case ID_BTN_FX_HIGHPASS: ToggleEffectAction(FxKind::HighPass); break;
        case ID_BTN_FX_GATE: ToggleEffectAction(FxKind::Gate); break;
        case ID_BTN_FX_DEESSER: ToggleEffectAction(FxKind::DeEsser); break;
        case ID_BTN_FX_COMPRESSOR: ToggleEffectAction(FxKind::Compressor); break;
        case ID_BTN_FX_LIMITER: ToggleEffectAction(FxKind::Limiter); break;

        case ID_BTN_EQ_LOW_DEC:
            g.effectSettings.eq.lowDb = std::max(-12.0f, g.effectSettings.eq.lowDb - 1.5f);
            ToggleEffectAction(FxKind::Equalizer, true);
            break;
        case ID_BTN_EQ_LOW_INC:
            g.effectSettings.eq.lowDb = std::min(12.0f, g.effectSettings.eq.lowDb + 1.5f);
            ToggleEffectAction(FxKind::Equalizer, true);
            break;
        case ID_BTN_EQ_MID_DEC:
            g.effectSettings.eq.midDb = std::max(-12.0f, g.effectSettings.eq.midDb - 1.5f);
            ToggleEffectAction(FxKind::Equalizer, true);
            break;
        case ID_BTN_EQ_MID_INC:
            g.effectSettings.eq.midDb = std::min(12.0f, g.effectSettings.eq.midDb + 1.5f);
            ToggleEffectAction(FxKind::Equalizer, true);
            break;
        case ID_BTN_EQ_HIGH_DEC:
            g.effectSettings.eq.highDb = std::max(-12.0f, g.effectSettings.eq.highDb - 1.5f);
            ToggleEffectAction(FxKind::Equalizer, true);
            break;
        case ID_BTN_EQ_HIGH_INC:
            g.effectSettings.eq.highDb = std::min(12.0f, g.effectSettings.eq.highDb + 1.5f);
            ToggleEffectAction(FxKind::Equalizer, true);
            break;
        case ID_BTN_FX_EQ_APPLY: ToggleEffectAction(FxKind::Equalizer); break;
        case ID_BTN_FX_EQ_PRESET:
            g.effectSettings.eq = { 2.0f, 2.5f, 1.5f };
            ToggleEffectAction(FxKind::Equalizer, true);
            break;

        case ID_BTN_WAV_BITDEPTH:
            g.wavFloat32 = !g.wavFloat32;
            MarkDocumentChanged();
            InvalidateRect(g.hwndMain, nullptr, FALSE);
            break;
        case ID_BTN_EXPORT_WAV: DoExportDialog(false); break;
        case ID_BTN_EXPORT_MP3: DoExportDialog(true); break;
    }
}

static LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto toDip = [&](int px, int py) -> std::pair<float, float> {
        float dpiX = 96.0f, dpiY = 96.0f;
        if (g.rt) g.rt->GetDpi(&dpiX, &dpiY);
        return { px * 96.0f / dpiX, py * 96.0f / dpiY };
    };

    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            BeginPaint(hwnd, &ps);
            RenderWindow();
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_SIZE:
            if (g.rt && wParam != SIZE_MINIMIZED) {
                const auto client = D2D1::SizeU(std::max<UINT>(1, LOWORD(lParam)), std::max<UINT>(1, HIWORD(lParam)));
                const float dpi = LayoutDpi(hwnd, client);
                g.rt->SetDpi(dpi, dpi);
                g.rt->Resize(client);
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;

        case WM_GETMINMAXINFO: {
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
            const auto dpi = std::max<UINT>(96, GetDpiForWindow(hwnd));
            RECT minimum{0, 0, MulDiv(kMinClientWidth, dpi, 96), MulDiv(kMinClientHeight, dpi, 96)};
            AdjustWindowRectExForDpi(&minimum, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
            mmi->ptMinTrackSize.x = minimum.right - minimum.left;
            mmi->ptMinTrackSize.y = minimum.bottom - minimum.top;
            MONITORINFO monitor{sizeof(MONITORINFO)};
            if (GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor)) {
                mmi->ptMinTrackSize.x = std::min(mmi->ptMinTrackSize.x, monitor.rcWork.right - monitor.rcWork.left);
                mmi->ptMinTrackSize.y = std::min(mmi->ptMinTrackSize.y, monitor.rcWork.bottom - monitor.rcWork.top);
            }
            return 0;
        }

        case WM_DPICHANGED: {
            const auto& suggested = *reinterpret_cast<const RECT*>(lParam);
            g.prompterEdge.Reset();
            g.brush.Reset();
            g.rt.Reset();
            SetWindowPos(hwnd, nullptr, suggested.left, suggested.top, suggested.right - suggested.left,
                         suggested.bottom - suggested.top, SWP_NOACTIVATE | SWP_NOZORDER);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        case WM_SETTINGCHANGE:
            RefreshAnimationPreference();
            InvalidateRect(hwnd, nullptr, FALSE);
            break;

        case WM_TIMER:
            if (wParam == 1) {
                const bool needsFrame = g.isRecording.load() || g.isPlaying.load() ||
                    g.prompterScrollY != g.prompterScrollTarget || g.animatingWaveView ||
                    g.recordPresence > 0.0f || g.drawnPeakFraction > 0.0f;
                if (!needsFrame) {
                    KillTimer(hwnd, 1);
                    g.motionTimerRunning = false;
                } else if (!IsIconic(hwnd)) {
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
            }
            return 0;

        case WM_APP + 1: {
            if (lParam != g.transportGeneration) return 0;
            const auto result = static_cast<TransportResult>(wParam);
            if (result == TransportResult::CaptureStarted) {
                if (g.stopRequested.load()) return 0;
                SetStatus(Text(L"Recording. Press Space to stop.", L"Идёт запись. Нажмите Пробел для остановки."));
                return 0;
            }
            const bool hadCapture = g.captureReady.load();
            StopAudioTransport();
            switch (result) {
                case TransportResult::CaptureFailed:
                    SetStatus(hadCapture ? Text(L"Microphone signal lost. ", L"Сигнал микрофона прервался. ") + g.statusText
                                         : Text(L"The microphone sent no audio. Check the device and microphone permissions in Windows.", L"Микрофон не передал звук. Проверьте устройство и права доступа в Windows."), true);
                    break;
                case TransportResult::PlaybackFailed:
                    SetStatus(Text(L"Playback failed. Check the selected output device.", L"Ошибка воспроизведения. Проверьте выбранное устройство вывода."), true);
                    break;
                case TransportResult::PlaybackFinished:
                    SetStatus(Text(L"Playback finished.", L"Воспроизведение завершено."));
                    break;
                case TransportResult::CaptureStarted:
                case TransportResult::CaptureCancelled:
                    break;
            }
            return 0;
        }

        case WM_APP + 2:
            if (g.exportThread.joinable()) {
                g.exportThread.join();
                SetStatus(g.exportStatus, !g.exportSucceeded);
            }
            return 0;

        case kAudioWorkReadyMessage:
            HandleAudioWorkResults();
            return 0;

        case kAudioBackupFailedMessage:
            if (lParam == g.backupRevision) {
                SetStatus(Text(L"Could not update the backup. Export a WAV to save the track.", L"Не удалось обновить резервную копию. Чтобы сохранить дорожку, экспортируйте WAV."), true);
            }
            return 0;

        case kDocumentReadyMessage:
            HandleDocumentResult();
            return 0;

        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case OpenAudioOrProject: DoOpenAudioOrProject(false); return 0;
                case OpenBackup: DoOpenAudioOrProject(true); return 0;
                case SaveProject: DoSaveProject(false); return 0;
                case SaveProjectAs: DoSaveProject(true); return 0;
                case SelectEnglish: ChangeInterfaceLanguage(Language::English); return 0;
                case SelectRussian: ChangeInterfaceLanguage(Language::Russian); return 0;
            }
            break;

        case WM_MOUSEMOVE: {
            auto [x, y] = toDip(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            if (g.draggingMasterGain) {
                DragMasterGainTo(y);
                return 0;
            }
            if (g.draggingSilenceThreshold) {
                DragSilenceThresholdTo(x);
                return 0;
            }
            if (g.isDraggingWave) {
                size_t cur = WaveXToSample(x);
                g.selStartSample = g.dragAnchorSample;
                g.selEndSample = cur;
                g.hasSelection = (std::llabs(static_cast<long long>(g.selEndSample) - static_cast<long long>(g.selStartSample)) > 1200);
                g.playheadSample.store(std::min(g.selStartSample, g.selEndSample));
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (g.isDraggingPrompter) {
                float dy = g.prompterDragLastY - y;
                g.prompterDragLastY = y;
                g.prompterScrollTarget = std::clamp(g.prompterScrollY + dy, 0.0f, g.prompterMaxScroll);
                g.prompterScrollY = g.prompterScrollTarget;
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (!g.trackingMouseLeave) {
                TRACKMOUSEEVENT tracking{sizeof(TRACKMOUSEEVENT), TME_LEAVE, hwnd, 0};
                g.trackingMouseLeave = TrackMouseEvent(&tracking) != FALSE;
            }
            int hit = HitTestControl(x, y);
            if (hit != g.hoveredControlId) {
                g.hoveredControlId = hit;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }

        case WM_MOUSELEAVE:
            g.trackingMouseLeave = false;
            g.hoveredControlId = -1;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;

        case WM_LBUTTONDOWN: {
            auto [x, y] = toDip(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            int hit = HitTestControl(x, y);
            if (hit == ID_WAVE_MASTER_GAIN || hit == ID_SILENCE_THRESHOLD) {
                g.draggingMasterGain = hit == ID_WAVE_MASTER_GAIN;
                g.draggingSilenceThreshold = hit == ID_SILENCE_THRESHOLD;
                SetCapture(hwnd);
                if (g.draggingMasterGain) DragMasterGainTo(y);
                else DragSilenceThresholdTo(x);
                return 0;
            }
            if (hit != -1) {
                HandleControlClick(hit);
                return 0;
            }
            if (x >= g.rectWaveArea.left && x <= g.rectWaveArea.right &&
                y >= g.rectWaveArea.top && y <= g.rectWaveArea.bottom && !g.isRecording.load()) {
                if (g.isPlaying.load() || g.audioThread.joinable()) StopAudioTransport();
                if (g.animatingWaveView) {
                    g.viewStartSec = g.drawnViewStartSec;
                    g.viewDurationSec = g.drawnViewDurationSec;
                    g.animatingWaveView = false;
                }
                size_t s = WaveXToSample(x);
                g.isDraggingWave = true;
                g.dragAnchorSample = s;
                g.selStartSample = s;
                g.selEndSample = s;
                g.hasSelection = false;
                g.playheadSample.store(s);
                SetCapture(hwnd);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (x >= g.rectPrompterBox.left && x <= g.rectPrompterBox.right &&
                y >= g.rectPrompterBox.top && y <= g.rectPrompterBox.bottom) {
                g.isDraggingPrompter = true;
                g.prompterDragLastY = y;
                g.prompterScrollTarget = g.prompterScrollY;
                SetCapture(hwnd);
                return 0;
            }
            return 0;
        }

        case WM_LBUTTONUP: {
            if (g.draggingMasterGain || g.draggingSilenceThreshold) {
                auto [x, y] = toDip(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
                if (g.draggingMasterGain) DragMasterGainTo(y);
                else DragSilenceThresholdTo(x);
                ReleaseCapture();
                return 0;
            }
            if (g.isDraggingWave || g.isDraggingPrompter) {
                g.isDraggingWave = false;
                g.isDraggingPrompter = false;
                ReleaseCapture();
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }

        case WM_CAPTURECHANGED:
            g.isDraggingWave = false;
            g.isDraggingPrompter = false;
            g.draggingMasterGain = false;
            g.draggingSilenceThreshold = false;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;

        case WM_MOUSEWHEEL: {
            POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            ScreenToClient(hwnd, &pt);
            auto [x, y] = toDip(pt.x, pt.y);
            int delta = GET_WHEEL_DELTA_WPARAM(wParam);
            if (InRect(x, y, g.rectMasterGainArea)) {
                SetMasterGainDb(g.masterGainDb.load() + delta / 120.0f);
                return 0;
            }
            if (HitTestControl(x, y) == ID_SILENCE_THRESHOLD) {
                ChangeSilenceThreshold(g.silenceThresholdDb + delta / 120.0f);
                return 0;
            }
            if (x >= g.rectPrompterBox.left && x <= g.rectPrompterBox.right &&
                y >= g.rectPrompterBox.top && y <= g.rectPrompterBox.bottom) {
                if (!g.isDraggingPrompter) ScrollPrompter(-(delta / 120.0f) * 42.0f);
            } else if (!g.isRecording.load() && !g.isDraggingWave &&
                       x >= g.rectWaveArea.left && x <= g.rectWaveArea.right &&
                       y >= g.rectWaveArea.top && y <= g.rectWaveArea.bottom) {
                bool ctrl = (GET_KEYSTATE_WPARAM(wParam) & MK_CONTROL) != 0;
                if (ctrl) {
                    const double anchor = (x - g.rectWaveArea.left) / std::max(1.0f, g.rectWaveArea.right - g.rectWaveArea.left);
                    ZoomWave(std::pow(0.65, delta / 120.0), anchor);
                } else {
                    const double totalSec = WaveTrackSeconds();
                    double effDurSec = (g.viewDurationSec > 0.1) ? g.viewDurationSec : std::max(10.0, totalSec * 1.04);
                    double maxViewStart = std::max(0.0, totalSec - effDurSec);
                    const double start = std::clamp(g.viewStartSec - (delta / 120.0) * (effDurSec * 0.18), 0.0, maxViewStart);
                    if (start != g.viewStartSec) SetWaveView(start, g.viewDurationSec);
                }
            }
            return 0;
        }

        case WM_KEYDOWN: {
            bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            if ((lParam & (1LL << 30)) && (wParam == VK_SPACE || wParam == VK_RETURN || wParam == 'P')) return 0;
            if (wParam == VK_SPACE) {
                if (g.isRecording.load() || g.isPlaying.load()) RequestTransportStop();
                else StartRecording();
                return 0;
            }
            if (wParam == VK_RETURN || wParam == 'P') {
                TogglePlayback();
                return 0;
            }
            if (wParam == VK_LEFT) {
                HandleControlClick(ID_BTN_SPEED_DOWN);
                return 0;
            }
            if (wParam == VK_RIGHT) {
                HandleControlClick(ID_BTN_SPEED_UP);
                return 0;
            }
            if (wParam == VK_UP) {
                ScrollPrompter(-40.0f);
                return 0;
            }
            if (wParam == VK_DOWN) {
                ScrollPrompter(40.0f);
                return 0;
            }
            if (wParam == VK_HOME) {
                ScrollPrompter(0.0f, true);
                return 0;
            }
            if (wParam == VK_DELETE || wParam == VK_BACK) {
                DoDeleteSelection();
                return 0;
            }
            if (wParam == VK_ESCAPE) {
                if (g.isRecording.load() || g.isPlaying.load()) RequestTransportStop();
                else if (g.playWhenProcessed) TogglePlayback();
                else if (g.silenceToolsOpen) DismissSilenceTools();
                else if (g.hasSelection) {
                    g.hasSelection = false;
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                return 0;
            }
            if (ctrl && wParam == 'Z') { DoUndo(); return 0; }
            if (ctrl && wParam == 'V') { DoPasteClipboardText(); return 0; }
            if (ctrl && (wParam == 'O' || wParam == 'S')) {
                if (lParam & (1LL << 30)) return 0;
                const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
                if (wParam == 'S') DoSaveProject(shift);
                else if (shift) DoOpenTextFile();
                else DoOpenAudioOrProject(false);
                return 0;
            }
            return 0;
        }

        case WM_CLOSE:
            if (g.exportThread.joinable()) {
                SetStatus(Text(L"Wait for the export to finish before closing.", L"Дождитесь завершения экспорта перед закрытием."));
                return 0;
            }
            if (FileOperationPending() || g.silenceApplyPending) {
                SetStatus(Text(L"Wait for the current operation to finish before closing.", L"Дождитесь завершения текущей операции перед закрытием."));
                return 0;
            }
            if (g.isRecording.load()) StopAudioTransport();
            if (g.documentDirty) {
                const int choice = AskSaveBeforeLeaving();
                if (choice == IDYES) g.closeAfterSave = StartSaveProject(false);
                if (choice != IDNO) return 0;
            }
            break;

        case WM_DESTROY:
            StopAudioTransport();
            CancelAudioWork(true);
            CancelAudioWork(false);
            {
                std::lock_guard<std::mutex> lock(g.workMutex);
                g.workStopping = true;
            }
            g.workWake.notify_one();
            if (g.workThread.joinable()) g.workThread.join();
            if (g.documentThread.joinable()) g.documentThread.join();
            if (g.exportThread.joinable()) g.exportThread.join();
            g.rt.Reset();
            g.brush.Reset();
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ============================================================================
// Self-Test Runner (--self-test)
// ============================================================================

static void LogSelfTest(const std::string& line) {
    std::string msg = line + "\n";
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut && hOut != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(hOut, msg.data(), static_cast<DWORD>(msg.size()), &written, nullptr);
    }
}

static bool TestProjectRoundTrip(const fs::path& path) {
    project_file::Settings settings;
    settings.effectFlags = 0x271;
    settings.eqMidDb = -6.5f;
    settings.gainDb = 4.5f;
    settings.silenceDb = -37.0f;
    settings.autoScroll = false;
    settings.wavFloat32 = false;
    settings.prompterSpeed = 60.0f;
    settings.prompterFontSize = 30.0f;
    settings.prompterScroll = 135.5f;
    settings.playhead = 2;
    settings.hasSelection = true;
    settings.selectionStart = 3;
    settings.selectionEnd = 1;
    settings.waveformDuration = 2.0;
    const std::wstring script = L"# Проект\n\nПроверка записи. 🎙";
    const std::vector<float> samples{0.125f, -0.625f, 0.876f, 0.0f};
    std::wstring error;
    if (!WriteProjectAtomically(path, settings, script, samples, error)) return false;
    project_file::Project project;
    if (!project_file::Read(path, project, error) || project.samples != samples ||
        Utf8ToWide(project.scriptUtf8) != script) return false;
    const auto& restored = project.settings;
    if (restored.effectFlags != settings.effectFlags || restored.eqMidDb != settings.eqMidDb ||
        restored.gainDb != settings.gainDb || restored.silenceDb != settings.silenceDb ||
        restored.autoScroll || restored.wavFloat32 || restored.prompterSpeed != settings.prompterSpeed ||
        restored.prompterFontSize != settings.prompterFontSize || restored.prompterScroll != settings.prompterScroll ||
        restored.playhead != settings.playhead || !restored.hasSelection ||
        restored.selectionStart != settings.selectionStart || restored.selectionEnd != settings.selectionEnd ||
        restored.waveformDuration != settings.waveformDuration) return false;
    std::error_code ec;
    fs::resize_file(path, fs::file_size(path) - 1, ec);
    return !ec && !project_file::Read(path, project, error) && project.samples == samples;
}

static bool TestSilenceTrimAndGain() {
    const size_t second = kSampleRate;
    std::vector<float> source(3 * second, 0.0f);
    std::fill(source.begin(), source.begin() + second, 0.05f);
    std::fill(source.begin() + 2 * second, source.end(), -0.05f);
    const silence::Range scope{0, source.size()};
    const std::vector<silence::Range> expected{{11 * second / 10, 19 * second / 10}};
    const auto cuts = silence::Analyze(source, scope, kSampleRate, {});
    if (!cuts || *cuts != expected) return false;
    const auto edited = silence::Apply(source, scope, *cuts, kSampleRate);
    if (!edited || edited->size() != 22 * second / 10 ||
        silence::MapSample(*cuts, 2 * second) != 12 * second / 10 ||
        !std::equal(edited->begin(), edited->begin() + second, source.begin()) ||
        !std::equal(edited->end() - second, edited->end(), source.end() - second)) {
        return false;
    }
    std::atomic<bool> cancelled{true};
    if (silence::Analyze(source, scope, kSampleRate, {}, &cancelled)) return false;
    const float boost = std::pow(10.0f, 6.0f / 20.0f);
    return std::fabs(ApplyOutputGain(0.2f, boost, 1.0f) - 0.39905247f) < 1e-6f &&
        ApplyOutputGain(0.9f, boost, 1.0f) == 1.0f &&
        ApplyOutputGain(-0.9f, boost, dsp::kLimiterCeiling) == -dsp::kLimiterCeiling;
}

static bool TestVoiceEffectToggles() {
    const float nearKnee = dsp::SoftenPeaks(0.951f);
    if (dsp::SoftenPeaks(0.949f) != 0.949f || nearKnee < 0.95f || nearKnee > 0.952f ||
        dsp::SoftenPeaks(-0.951f) != -nearKnee || dsp::SoftenPeaks(2.0f) > 1.0f) {
        LogSelfTest("[FAIL] EQ/Echo peak saturation has a discontinuity or exceeds full scale");
        return false;
    }
    LogSelfTest("[OK] Continuous EQ/Echo peak saturation");

    const auto preservedAudio = g.raw;
    std::vector<float> source(kSampleRate * 5);
    for (size_t i = 0; i < source.size(); ++i) {
        const int section = static_cast<int>(i / kSampleRate);
        constexpr double frequencies[] = {30.0, 220.0, 6800.0, 800.0, 800.0};
        constexpr float amplitudes[] = {0.25f, 0.45f, 0.2f, 0.002f, 0.002f};
        source[i] = amplitudes[section] * static_cast<float>(std::sin(
            2.0 * std::numbers::pi * frequencies[section] * i / kSampleRate));
    }
    source[source.size() - 200] = 1.37f;
    source.back() = -1.1f;

    auto rmsRatio = [&](int section) {
        const size_t start = section * kSampleRate + kSampleRate / 2;
        const size_t end = start + kSampleRate / 4;
        double dryEnergy = 0.0;
        double wetEnergy = 0.0;
        for (size_t i = start; i < end; ++i) {
            dryEnergy += source[i] * source[i];
            wetEnergy += CurrentTrackLocked().samples[i] * CurrentTrackLocked().samples[i];
        }
        return std::sqrt(wetEnergy / dryEnergy);
    };
    auto withinLimiterCeiling = [](float sample) {
        return std::isfinite(sample) && std::fabs(sample) <= dsp::kLimiterCeiling + 1e-6f;
    };

    struct Check {
        FxKind kind;
        const char* name;
        int section;
        double maxRatio;
    };
    constexpr Check checks[] = {
        {FxKind::HighPass, "Rumble filter", 0, 0.17},
        {FxKind::Gate, "Soft noise gate", 4, 0.20},
        {FxKind::DeEsser, "De-esser", 2, 0.55},
        {FxKind::Compressor, "Voice compressor", 1, 0.57},
        {FxKind::Limiter, "Peak limiter", -1, 0.0}
    };

    g.raw = std::make_shared<AudioTrack>();
    g.raw->samples = source;
    UpdateWaveCache(*g.raw);
    for (const auto& check : checks) {
        ToggleEffectAction(check.kind);
        if (g.raw->samples != source || CurrentTrackLocked().samples.size() != source.size() || CurrentTrackLocked().samples == source ||
            !std::all_of(CurrentTrackLocked().samples.begin(), CurrentTrackLocked().samples.end(), [](float x) { return std::isfinite(x); })) {
            LogSelfTest(std::string("[FAIL] Invalid or unchanged processed audio: ") + check.name);
            return false;
        }
        const bool meetsTarget = check.kind == FxKind::Limiter
            ? std::all_of(CurrentTrackLocked().samples.begin(), CurrentTrackLocked().samples.end(), withinLimiterCeiling)
            : rmsRatio(check.section) < check.maxRatio;
        if (!meetsTarget || ((check.kind == FxKind::HighPass || check.kind == FxKind::DeEsser) && rmsRatio(1) < 0.96)) {
            LogSelfTest(std::string("[FAIL] Audio response out of range: ") + check.name);
            return false;
        }
        ToggleEffectAction(check.kind);
        if (CurrentTrackLocked().samples != source) {
            LogSelfTest(std::string("[FAIL] Toggling off did not restore dry audio: ") + check.name);
            return false;
        }
        LogSelfTest(std::string("[OK] ") + check.name + " DSP response and toggle round-trip");
    }

    ToggleEffectAction(FxKind::Cleanup);
    const auto cleaned = CurrentTrackLocked().samples;
    ToggleEffectAction(FxKind::HighPass);
    ToggleEffectAction(FxKind::Gate);
    if (CurrentTrackLocked().samples != cleaned) {
        LogSelfTest("[FAIL] Cleanup duplicated rumble filtering or gating");
        return false;
    }
    ToggleEffectAction(FxKind::Cleanup);
    const auto filtered = CurrentTrackLocked().samples;
    ToggleEffectAction(FxKind::Gate);
    ToggleEffectAction(FxKind::HighPass);
    if (CurrentTrackLocked().samples != source || filtered == source || filtered == cleaned) {
        LogSelfTest("[FAIL] Cleanup changed standalone effect switches");
        return false;
    }
    LogSelfTest("[OK] Cleanup and standalone filter/gate combination");

    constexpr FxKind chain[] = {
        FxKind::HighPass, FxKind::Gate, FxKind::Equalizer, FxKind::DeEsser,
        FxKind::Compressor, FxKind::Echo, FxKind::FadeIn, FxKind::FadeOut, FxKind::Limiter
    };
    for (auto kind : chain) ToggleEffectAction(kind);
    if (CurrentTrackLocked().samples.size() != source.size() || CurrentTrackLocked().samples.front() != 0.0f || CurrentTrackLocked().samples.back() != 0.0f ||
        !std::all_of(CurrentTrackLocked().samples.begin(), CurrentTrackLocked().samples.end(), withinLimiterCeiling) || g.raw->samples != source) {
        LogSelfTest("[FAIL] Combined effects damaged length, fades, peak ceiling, or raw audio");
        return false;
    }
    for (auto kind : chain) ToggleEffectAction(kind);
    if (CurrentTrackLocked().samples != source) {
        LogSelfTest("[FAIL] Full chain bypass did not restore dry audio");
        return false;
    }
    LogSelfTest("[OK] Combined effect chain and bypass");

    g.raw = preservedAudio;
    QueueEffectsLocked();
    return true;
}

static int RunSelfTest() {
    LogSelfTest("=== Podcaster Self-Test ===");
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    auto inputs = EnumerateWasapiDevices(eCapture);
    auto outputs = EnumerateWasapiDevices(eRender);
    LogSelfTest("WASAPI capture devices: " + std::to_string(inputs.size()));
    for (const auto& d : inputs) {
        LogSelfTest("  [IN ] " + WideToUtf8(d.name) + (d.isDefault ? " (default)" : ""));
    }
    LogSelfTest("WASAPI render devices: " + std::to_string(outputs.size()));
    for (const auto& d : outputs) {
        LogSelfTest("  [OUT] " + WideToUtf8(d.name) + (d.isDefault ? " (default)" : ""));
    }

    // Generate 3s synthetic signal: 220 Hz voice + 40 Hz rumble + quiet noise section
    const size_t N = kSampleRate * 3;
    std::vector<float> sig(N, 0.0f);
    for (size_t i = 0; i < N; ++i) {
        double t = static_cast<double>(i) / kSampleRate;
        if (i < kSampleRate * 2) {
            sig[i] = static_cast<float>(0.35 * std::sin(2.0 * std::numbers::pi * 220.0 * t) +
                                        0.15 * std::sin(2.0 * std::numbers::pi * 40.0 * t));
        } else {
            sig[i] = static_cast<float>(0.002 * std::sin(2.0 * std::numbers::pi * 1000.0 * t));
        }
    }

    auto hasNonFinite = [](const std::vector<float>& v) {
        for (float x : v) if (!std::isfinite(x)) return true;
        return false;
    };

    auto testBuf = sig;
    ApplyVoiceCleanupDsp(testBuf, 0, testBuf.size());
    float peakAfterClean = 0.0f;
    for (float x : testBuf) peakAfterClean = std::max(peakAfterClean, std::fabs(x));
    if (hasNonFinite(testBuf) || peakAfterClean < 0.80f || peakAfterClean > 0.99f) {
        LogSelfTest("[FAIL] VoiceCleanup DSP check failed");
        if (SUCCEEDED(hr)) CoUninitialize();
        return 1;
    }
    LogSelfTest("[OK] VoiceCleanup DSP (normalized peak = " + std::to_string(peakAfterClean) + ")");

    ApplyFadeInDsp(testBuf, 0, kSampleRate);
    ApplyFadeOutDsp(testBuf, N - kSampleRate, N);
    if (std::fabs(testBuf.front()) > 1e-4f || std::fabs(testBuf.back()) > 1e-4f) {
        LogSelfTest("[FAIL] FadeIn/FadeOut check failed");
        if (SUCCEEDED(hr)) CoUninitialize();
        return 1;
    }
    LogSelfTest("[OK] FadeIn / FadeOut DSP");

    ApplyEchoDsp(testBuf, 0, testBuf.size());
    if (hasNonFinite(testBuf)) {
        LogSelfTest("[FAIL] Echo DSP produced non-finite");
        if (SUCCEEDED(hr)) CoUninitialize();
        return 1;
    }
    LogSelfTest("[OK] Echo DSP");

    EqParams eq{2.0f, 2.5f, 1.5f};
    ApplyEqualizerDsp(testBuf, 0, testBuf.size(), eq);
    if (hasNonFinite(testBuf)) {
        LogSelfTest("[FAIL] Equalizer DSP produced non-finite");
        if (SUCCEEDED(hr)) CoUninitialize();
        return 1;
    }
    LogSelfTest("[OK] 3-Band Equalizer DSP");

    // Verify non-destructive toggle pipeline and JumpToLastLoudMoment
    {
        g.raw->samples = sig;
        UpdateWaveCache(*g.raw);
        g.effectSettings.eq = eq;
        g.effectSettings.equalizer = true;
        {
            std::lock_guard<std::mutex> lk(g.audioMutex);
            QueueEffectsLocked();
        }
        if (CurrentTrackLocked().samples == g.raw->samples) {
            LogSelfTest("[FAIL] Non-destructive EQ toggle ON did not alter processed buffer");
            return 1;
        }
        g.effectSettings.equalizer = false;
        {
            std::lock_guard<std::mutex> lk(g.audioMutex);
            QueueEffectsLocked();
        }
        if (CurrentTrackLocked().samples != g.raw->samples) {
            LogSelfTest("[FAIL] Non-destructive EQ toggle OFF did not restore raw buffer");
            return 1;
        }
        g.playheadSample.store(0);
        DoJumpToLastLoudMoment();
        size_t pos1 = g.playheadSample.load();
        DoJumpToLastLoudMoment();
        size_t pos2 = g.playheadSample.load();
        if (pos1 < kSampleRate * 1.9 || pos1 > kSampleRate * 2.2 || pos2 != 0) {
            LogSelfTest("[FAIL] DoJumpToLastLoudMoment positions unexpected: " + std::to_string(pos1) + " / " + std::to_string(pos2));
            return 1;
        }
        LogSelfTest("[OK] Non-destructive FX toggles & JumpToLastLoudMoment");
    }

    if (!TestVoiceEffectToggles()) {
        if (SUCCEEDED(hr)) CoUninitialize();
        return 1;
    }
    if (!TestSilenceTrimAndGain()) {
        LogSelfTest("[FAIL] Silence preview/trim or master gain");
        if (SUCCEEDED(hr)) CoUninitialize();
        return 1;
    }
    LogSelfTest("[OK] Silence preview/trim, sample remapping and master gain");

    // Each run owns its temporary files, including when a check fails.
    struct ExportTestFiles {
        fs::path floatWav, pcmWav, mp3;
        ~ExportTestFiles() {
            std::error_code ec;
            for (const auto* path : {&floatWav, &pcmWav, &mp3}) {
                if (!path->empty()) fs::remove(*path, ec);
            }
        }
    };
    std::error_code ec;
    const auto tempDir = fs::temp_directory_path(ec);
    if (ec) {
        LogSelfTest("[FAIL] Windows temporary directory unavailable");
        return 1;
    }
    const ExportTestFiles files{CreateTempFileIn(tempDir), CreateTempFileIn(tempDir), CreateTempFileIn(tempDir)};
    const auto& [tmp32, tmp24, tmpMp3] = files;
    if (tmp32.empty() || tmp24.empty() || tmpMp3.empty()) {
        LogSelfTest("[FAIL] Could not create temporary test files");
        return 1;
    }
    std::wstring err;

    if (!WriteWavAtomically(tmp32, testBuf, true, err) || fs::file_size(tmp32) != 56 + N * 4) {
        LogSelfTest("[FAIL] Export 32-bit float WAV: " + WideToUtf8(err));
        return 1;
    }
    LogSelfTest("[OK] Export 32-bit IEEE Float WAV (" + std::to_string(fs::file_size(tmp32)) + " bytes)");

    {
        std::vector<float> loaded;
        if (!LoadWavFile(tmp32.wstring(), loaded) || loaded.size() != testBuf.size()) {
            LogSelfTest("[FAIL] LoadWavFile 32-bit float WAV size mismatch");
            return 1;
        }
        float maxDiff = 0.0f;
        for (size_t i = 0; i < loaded.size(); ++i) {
            maxDiff = std::max(maxDiff, std::fabs(loaded[i] - testBuf[i]));
        }
        if (maxDiff > 1e-6f) {
            LogSelfTest("[FAIL] LoadWavFile round-trip diff too large: " + std::to_string(maxDiff));
            return 1;
        }
        LogSelfTest("[OK] LoadWavFile 32-bit float round-trip");

        // Verify StopAudioTransport joins a finished thread safely even when isPlaying == false
        g.isPlaying.store(false);
        g.isRecording.store(false);
        g.audioThread = std::thread([]() {});
        Sleep(20);
        StopAudioTransport();
        if (g.audioThread.joinable()) {
            LogSelfTest("[FAIL] StopAudioTransport failed to join finished thread");
            return 1;
        }
        LogSelfTest("[OK] StopAudioTransport finished-thread join safety");
    }

    if (!ExportWavFile(tmp24.wstring(), testBuf, false, err) || fs::file_size(tmp24) != 44 + N * 3) {
        LogSelfTest("[FAIL] Export 24-bit PCM WAV: " + WideToUtf8(err));
        return 1;
    }
    LogSelfTest("[OK] Export 24-bit PCM WAV (" + std::to_string(fs::file_size(tmp24)) + " bytes)");

    const std::vector<float> shortPcm{0.5f, -0.5f, 0.125f};
    std::vector<float> decoded;
    if (!ExportWavFile(tmp24.wstring(), shortPcm, false, err) || fs::file_size(tmp24) != 54 ||
        !LoadWavFile(tmp24.wstring(), decoded) || decoded.size() != shortPcm.size() ||
        !std::equal(decoded.begin(), decoded.end(), shortPcm.begin(),
                    [](float a, float b) { return std::fabs(a - b) < 3e-7f; })) {
        LogSelfTest("[FAIL] Odd-length PCM24 padding or round-trip");
        return 1;
    }
    fs::resize_file(tmp32, fs::file_size(tmp32) - 1, ec);
    if (ec || LoadWavFile(tmp32.wstring(), decoded) || !decoded.empty()) {
        LogSelfTest("[FAIL] Truncated WAV must be rejected without partial audio");
        return 1;
    }
    LogSelfTest("[OK] Odd PCM24 padding and rejection of truncated float WAV");

    std::wstring ffmpeg = FindFfmpegPath();
    LogSelfTest("FFmpeg path: " + (ffmpeg.empty() ? std::string("(not found)") : WideToUtf8(ffmpeg)));
    if (!ExportMp3File(tmpMp3.wstring(), testBuf, err) || fs::file_size(tmpMp3) < 1000) {
        LogSelfTest("[FAIL] Export 320 kbps MP3: " + WideToUtf8(err));
        return 1;
    }
    LogSelfTest("[OK] Export 320 kbps MP3 (" + std::to_string(fs::file_size(tmpMp3)) + " bytes)");

    std::vector<float> imported;
    if (!ImportAudioFile(tmpMp3, imported, err) || imported.size() != N ||
        std::none_of(imported.begin(), imported.end(), [](float sample) { return std::abs(sample) > 0.1f; })) {
        LogSelfTest("[FAIL] Import MP3 as useful mono 48 kHz audio: " + WideToUtf8(err));
        return 1;
    }
    LogSelfTest("[OK] Import MP3 to mono 48 kHz float");

    if (!TestProjectRoundTrip(tmp32)) {
        LogSelfTest("[FAIL] Project file round-trip or damaged file rejection");
        return 1;
    }
    LogSelfTest("[OK] Project file round-trip and damaged file rejection");

    {
        const fs::path backupDir = GetBackupDir();
        fs::create_directories(backupDir, ec);
        const fs::path probe = ec ? fs::path{} : CreateTempFileIn(backupDir);
        if (probe.empty()) {
            LogSelfTest("[FAIL] Backup/settings directory is not writable: " + WideToUtf8(backupDir.wstring()));
            return 1;
        }
        std::error_code rmEc;
        fs::remove(probe, rmEc);
        if (rmEc) {
            LogSelfTest("[FAIL] Could not clean up probe file in backup/settings directory");
            return 1;
        }
        LogSelfTest("[OK] Writable backup/settings directory: " + WideToUtf8(backupDir.wstring()));
    }

    if (SUCCEEDED(hr)) CoUninitialize();
    LogSelfTest("[PASS] All self-tests succeeded.");
    return 0;
}

// ============================================================================
// Entry Point
// ============================================================================

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int nCmdShow) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool selfTest = false;
    fs::path startupFile;
    if (argv) {
        for (int i = 1; i < argc; ++i) {
            if (wcscmp(argv[i], L"--self-test") == 0) selfTest = true;
            else if (startupFile.empty() && argv[i][0] != L'-') startupFile = argv[i];
        }
        LocalFree(argv);
    }
    if (selfTest) {
        return RunSelfTest();
    }

    LoadLanguagePreference();
    SetStatus(Text(L"Ready to record. Press Space or click the microphone to start.",
                   L"Готов к записи. Нажмите Пробел или микрофон для старта."));
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    RefreshAnimationPreference();
    HRESULT hrCo = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory), nullptr, reinterpret_cast<void**>(g.d2dFactory.GetAddressOf())))) {
        return 1;
    }
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(g.dwFactory.GetAddressOf())))) {
        return 1;
    }

    RebuildInterfaceTextFormats();
    RefreshDevices(true);
    SetWelcomeScript();

    // Restore autosaved script & recording backup if present
    {
        fs::path backupDir = GetBackupDir();
        std::error_code ec;
        fs::path scriptBackup = backupDir / L"autosave_script.txt";
        if (fs::exists(scriptBackup, ec)) {
            std::wstring savedScript = ReadTextFileSmart(scriptBackup.wstring());
            SetPrompterText(savedScript, false);
        }
        fs::path audioBackup = backupDir / L"autosave_latest.wav";
        if (fs::exists(audioBackup, ec)) {
            std::vector<float> restored;
            if (LoadWavFile(audioBackup.wstring(), restored) && !restored.empty()) {
                std::lock_guard<std::mutex> lk(g.audioMutex);
                g.raw->samples = std::move(restored);
                UpdateWaveCache(*g.raw);
                g.playheadSample.store(g.raw->samples.size());
                SetStatus(Text(L"Recording restored from backups/autosave_latest.wav.", L"Восстановлена запись из автобэкапа (backups/autosave_latest.wav)."));
            } else {
                SetStatus(Text(L"Could not read backups/autosave_latest.wav.", L"Не удалось прочитать резервную копию backups/autosave_latest.wav."), true);
            }
        }
    }

    HICON hIconBig = static_cast<HICON>(LoadImageW(hInst, MAKEINTRESOURCEW(101), IMAGE_ICON, 48, 48, LR_DEFAULTCOLOR));
    HICON hIconSmall = static_cast<HICON>(LoadImageW(hInst, MAKEINTRESOURCEW(101), IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR));

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = MainWndProc;
    wc.hInstance = hInst;
    wc.hIcon = hIconBig ? hIconBig : LoadIconW(nullptr, IDI_APPLICATION);
    wc.hIconSm = hIconSmall ? hIconSmall : LoadIconW(nullptr, IDI_APPLICATION);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"PodcasterStudioWindow";
    RegisterClassExW(&wc);

    const UINT launchDpi = GetDpiForSystem();
    RECT workArea{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);
    const int launchWidth = std::min<int>(MulDiv(1140, launchDpi, 96), std::max(960L, workArea.right - workArea.left));
    const int launchHeight = std::min<int>(MulDiv(820, launchDpi, 96), std::max(620L, workArea.bottom - workArea.top));
    g.hwndMain = CreateWindowExW(
        0, wc.lpszClassName, Text(L"Podcaster — Recording studio and teleprompter", L"Подкастер — студия записи и телесуфлёр"),
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, launchWidth, launchHeight,
        nullptr, nullptr, hInst, nullptr
    );
    if (!g.hwndMain) return 1;
    if (hIconBig) SendMessageW(g.hwndMain, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(hIconBig));
    if (hIconSmall) SendMessageW(g.hwndMain, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(hIconSmall));

    ShowWindow(g.hwndMain, nCmdShow);
    UpdateWindow(g.hwndMain);
    if (!startupFile.empty()) {
        std::error_code ec;
        auto absoluteFile = fs::absolute(startupFile, ec);
        if (!ec) BeginOpenDocument(std::move(absoluteFile));
        else SetStatus(Text(L"Could not open the specified path.", L"Не удалось открыть указанный путь."), true);
    }

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (SUCCEEDED(hrCo)) CoUninitialize();
    return 0;
}
