#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <limits>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "localization.h"

namespace project_file {

struct Settings {
    std::uint32_t effectFlags = 0;
    float eqLowDb = 2.0f;
    float eqMidDb = 2.5f;
    float eqHighDb = 1.5f;
    float gainDb = 0.0f;
    float silenceDb = -42.0f;
    float prompterSpeed = 35.0f;
    float prompterFontSize = 26.0f;
    float prompterScroll = 0.0f;
    double waveformStart = 0.0;
    double waveformDuration = 0.0;
    std::uint64_t playhead = 0;
    bool autoScroll = true;
    bool wavFloat32 = true;
    bool hasSelection = false;
    std::uint64_t selectionStart = 0;
    std::uint64_t selectionEnd = 0;
};

struct Project {
    Settings settings;
    std::string scriptUtf8;
    std::vector<float> samples;
};

inline constexpr std::uint32_t kEffectMask = (1u << 10) - 1;
inline constexpr std::uint32_t kSampleRate = 48000;

namespace detail {

static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559);
static_assert(sizeof(double) == 8 && std::numeric_limits<double>::is_iec559);

// Version 1 is a fixed little-endian header, UTF-8 text, then mono IEEE float samples.
inline constexpr std::array<char, 8> kMagic{'P', 'O', 'D', 'C', 'A', 'S', 'T', '\x1A'};
inline constexpr std::uint32_t kVersion = 1;
inline constexpr std::size_t kHeaderBytes = kMagic.size() + 6 * 4 + 8 * 4 + 2 * 8 + 5 * 8;
inline constexpr std::uint64_t kMaxScriptBytes = 64ULL * 1024 * 1024;
inline constexpr std::uint64_t kMaxSampleCount = std::numeric_limits<std::uint32_t>::max() / sizeof(float);
inline constexpr std::size_t kBlockSamples = 16 * 1024;
inline constexpr std::uint32_t kAutoScroll = 1u << 0;
inline constexpr std::uint32_t kFloatWav = 1u << 1;
inline constexpr std::uint32_t kHasSelection = 1u << 2;
using Header = std::array<char, kHeaderBytes>;

inline bool Fail(std::wstring& error, const wchar_t* en, const wchar_t* ru) {
    error = locale_ui::Text(en, ru);
    return false;
}

template <typename UInt>
inline void PutLittleEndian(char*& cursor, UInt value) {
    for (std::size_t i = 0; i < sizeof(UInt); ++i) {
        *cursor++ = static_cast<char>(value & 0xff);
        value >>= 8;
    }
}

template <typename UInt>
inline UInt TakeLittleEndian(const char*& cursor) {
    UInt value = 0;
    for (std::size_t i = 0; i < sizeof(UInt); ++i) {
        value |= static_cast<UInt>(static_cast<unsigned char>(*cursor++)) << (i * 8);
    }
    return value;
}

inline void PutFloat(char*& cursor, float value) {
    PutLittleEndian(cursor, std::bit_cast<std::uint32_t>(value));
}

inline float TakeFloat(const char*& cursor) {
    return std::bit_cast<float>(TakeLittleEndian<std::uint32_t>(cursor));
}

inline Header EncodeHeader(const Settings& settings, std::uint64_t scriptBytes, std::uint64_t sampleCount) {
    Header header{};
    auto* cursor = std::copy(kMagic.begin(), kMagic.end(), header.data());
    PutLittleEndian(cursor, kVersion);
    PutLittleEndian(cursor, static_cast<std::uint32_t>(kHeaderBytes));
    PutLittleEndian(cursor, kSampleRate);
    PutLittleEndian(cursor, std::uint32_t{1});
    PutLittleEndian(cursor, settings.effectFlags);
    const std::uint32_t properties = (settings.autoScroll ? kAutoScroll : 0) |
                                     (settings.wavFloat32 ? kFloatWav : 0) |
                                     (settings.hasSelection ? kHasSelection : 0);
    PutLittleEndian(cursor, properties);
    PutFloat(cursor, settings.eqLowDb);
    PutFloat(cursor, settings.eqMidDb);
    PutFloat(cursor, settings.eqHighDb);
    PutFloat(cursor, settings.gainDb);
    PutFloat(cursor, settings.silenceDb);
    PutFloat(cursor, settings.prompterSpeed);
    PutFloat(cursor, settings.prompterFontSize);
    PutFloat(cursor, settings.prompterScroll);
    PutLittleEndian(cursor, std::bit_cast<std::uint64_t>(settings.waveformStart));
    PutLittleEndian(cursor, std::bit_cast<std::uint64_t>(settings.waveformDuration));
    PutLittleEndian(cursor, scriptBytes);
    PutLittleEndian(cursor, sampleCount);
    PutLittleEndian(cursor, settings.playhead);
    PutLittleEndian(cursor, settings.selectionStart);
    PutLittleEndian(cursor, settings.selectionEnd);
    return header;
}

inline bool DecodeHeader(const Header& header, Settings& settings, std::uint64_t& scriptBytes,
                         std::uint64_t& sampleCount, std::wstring& error) {
    if (!std::equal(kMagic.begin(), kMagic.end(), header.begin())) {
        return Fail(error, L"Invalid Podcaster project file.", L"Этот файл не является проектом Podcaster.");
    }
    const auto* cursor = header.data() + kMagic.size();
    if (TakeLittleEndian<std::uint32_t>(cursor) != kVersion) {
        return Fail(error, L"Unsupported project file version.", L"Эта версия файла проекта не поддерживается.");
    }
    if (TakeLittleEndian<std::uint32_t>(cursor) != kHeaderBytes) {
        return Fail(error, L"The project file header is corrupted.", L"Повреждён заголовок проекта.");
    }
    if (TakeLittleEndian<std::uint32_t>(cursor) != kSampleRate ||
        TakeLittleEndian<std::uint32_t>(cursor) != 1) {
        return Fail(error, L"The project's audio format is unsupported.",
                    L"Проект содержит аудио в неподдерживаемом формате.");
    }
    settings.effectFlags = TakeLittleEndian<std::uint32_t>(cursor);
    const auto properties = TakeLittleEndian<std::uint32_t>(cursor);
    if ((properties & ~(kAutoScroll | kFloatWav | kHasSelection)) != 0) {
        return Fail(error, L"The project settings are corrupted.", L"Повреждены настройки проекта.");
    }
    settings.autoScroll = (properties & kAutoScroll) != 0;
    settings.wavFloat32 = (properties & kFloatWav) != 0;
    settings.hasSelection = (properties & kHasSelection) != 0;
    settings.eqLowDb = TakeFloat(cursor);
    settings.eqMidDb = TakeFloat(cursor);
    settings.eqHighDb = TakeFloat(cursor);
    settings.gainDb = TakeFloat(cursor);
    settings.silenceDb = TakeFloat(cursor);
    settings.prompterSpeed = TakeFloat(cursor);
    settings.prompterFontSize = TakeFloat(cursor);
    settings.prompterScroll = TakeFloat(cursor);
    settings.waveformStart = std::bit_cast<double>(TakeLittleEndian<std::uint64_t>(cursor));
    settings.waveformDuration = std::bit_cast<double>(TakeLittleEndian<std::uint64_t>(cursor));
    scriptBytes = TakeLittleEndian<std::uint64_t>(cursor);
    sampleCount = TakeLittleEndian<std::uint64_t>(cursor);
    settings.playhead = TakeLittleEndian<std::uint64_t>(cursor);
    settings.selectionStart = TakeLittleEndian<std::uint64_t>(cursor);
    settings.selectionEnd = TakeLittleEndian<std::uint64_t>(cursor);
    return true;
}

inline bool ValidSettings(const Settings& settings, std::uint64_t sampleCount) {
    const auto inRange = [](double value, double low, double high) {
        return std::isfinite(value) && low <= value && value <= high;
    };
    constexpr double maxWaveSeconds = 2.0 * kMaxSampleCount / kSampleRate;
    return (settings.effectFlags & ~kEffectMask) == 0 &&
           inRange(settings.eqLowDb, -12.0, 12.0) && inRange(settings.eqMidDb, -12.0, 12.0) &&
           inRange(settings.eqHighDb, -12.0, 12.0) && inRange(settings.gainDb, -24.0, 12.0) &&
           inRange(settings.silenceDb, -70.0, -15.0) && inRange(settings.prompterSpeed, 10.0, 180.0) &&
           inRange(settings.prompterFontSize, 16.0, 46.0) &&
           std::isfinite(settings.prompterScroll) && settings.prompterScroll >= 0.0f &&
           inRange(settings.waveformStart, 0.0, maxWaveSeconds) &&
           inRange(settings.waveformDuration, 0.0, maxWaveSeconds) && settings.playhead <= sampleCount &&
           (!settings.hasSelection ||
                (settings.selectionStart <= sampleCount && settings.selectionEnd <= sampleCount));
}

inline bool ValidUtf8(std::string_view text) {
    std::size_t index = 0;
    while (index < text.size()) {
        const auto lead = static_cast<unsigned char>(text[index++]);
        if (lead <= 0x7f) continue;
        if (lead < 0xc2 || lead > 0xf4) return false;
        const std::size_t tail = lead < 0xe0 ? 1 : lead < 0xf0 ? 2 : 3;
        if (tail > text.size() - index) return false;
        const auto next = static_cast<unsigned char>(text[index]);
        if ((lead == 0xe0 && next < 0xa0) || (lead == 0xed && next >= 0xa0) ||
            (lead == 0xf0 && next < 0x90) || (lead == 0xf4 && next >= 0x90)) return false;
        for (std::size_t i = 0; i < tail; ++i) {
            if ((static_cast<unsigned char>(text[index++]) & 0xc0) != 0x80) return false;
        }
    }
    return true;
}

inline bool FiniteSamples(std::span<const float> samples) {
    return std::all_of(samples.begin(), samples.end(), [](float sample) { return std::isfinite(sample); });
}

inline bool ReadSamples(std::ifstream& input, std::size_t count, std::vector<float>& samples,
                        std::wstring& error) {
    samples.reserve(count);
    std::array<float, kBlockSamples> buffer;
    while (samples.size() < count) {
        const auto block = std::span(buffer).first(std::min(buffer.size(), count - samples.size()));
        auto* bytes = reinterpret_cast<char*>(block.data());
        if (!input.read(bytes, static_cast<std::streamsize>(block.size_bytes()))) {
            return Fail(error, L"The audio track in the project file is truncated.",
                        L"Аудиодорожка в файле проекта обрезана.");
        }
        if constexpr (std::endian::native != std::endian::little) {
            const char* cursor = bytes;
            for (auto& sample : block) sample = TakeFloat(cursor);
        }
        if (!FiniteSamples(block)) {
            return Fail(error, L"The project file contains invalid audio samples.",
                        L"Аудиодорожка в файле проекта повреждена.");
        }
        samples.insert(samples.end(), block.begin(), block.end());
    }
    return true;
}

inline bool WriteSamples(std::ofstream& output, std::span<const float> samples, std::wstring& error) {
    while (!samples.empty()) {
        const auto block = samples.first(std::min(kBlockSamples, samples.size()));
        if (!FiniteSamples(block)) {
            return Fail(error, L"The audio track contains invalid samples.",
                        L"Аудиодорожка содержит некорректные данные.");
        }
        if constexpr (std::endian::native == std::endian::little) {
            output.write(reinterpret_cast<const char*>(block.data()), static_cast<std::streamsize>(block.size_bytes()));
        } else {
            std::array<char, kBlockSamples * sizeof(float)> buffer;
            auto* cursor = buffer.data();
            for (float sample : block) PutFloat(cursor, sample);
            output.write(buffer.data(), static_cast<std::streamsize>(block.size_bytes()));
        }
        if (!output) {
            return Fail(error, L"Could not write the project's audio track.",
                        L"Не удалось записать аудиодорожку проекта.");
        }
        samples = samples.subspan(block.size());
    }
    return true;
}

} // namespace detail

inline bool Read(const std::filesystem::path& path, Project& project, std::wstring& error) {
    using namespace detail;
    try {
        std::ifstream input(path, std::ios::binary | std::ios::ate);
        if (!input) return Fail(error, L"Could not open the project file.", L"Не удалось открыть файл проекта.");
        const std::streamoff fileSize = input.tellg();
        if (fileSize < static_cast<std::streamoff>(kHeaderBytes)) {
            return Fail(error, L"The project file is truncated or corrupted.", L"Файл проекта обрезан или повреждён.");
        }
        Header header;
        input.seekg(0);
        if (!input.read(header.data(), static_cast<std::streamsize>(header.size()))) {
            return Fail(error, L"Could not read the project file header.", L"Не удалось прочитать заголовок проекта.");
        }

        Project loaded;
        std::uint64_t scriptBytes = 0;
        std::uint64_t sampleCount = 0;
        if (!DecodeHeader(header, loaded.settings, scriptBytes, sampleCount, error)) return false;
        if (scriptBytes > kMaxScriptBytes || scriptBytes > loaded.scriptUtf8.max_size() ||
            sampleCount > kMaxSampleCount || sampleCount > loaded.samples.max_size()) {
            return Fail(error, L"The script or audio track exceeds the project format's size limit.",
                        L"Размер сценария или аудиодорожки превышает предел формата проекта.");
        }
        const auto payloadBytes = static_cast<std::uint64_t>(fileSize) - kHeaderBytes;
        if (payloadBytes != scriptBytes + sampleCount * sizeof(float)) {
            return Fail(error, L"The project data length differs from the size in its header.",
                        L"Длина данных не совпадает с заголовком. Файл проекта повреждён.");
        }
        if (!ValidSettings(loaded.settings, sampleCount)) {
            return Fail(error, L"The project settings are corrupted.", L"Повреждены настройки проекта.");
        }

        loaded.scriptUtf8.resize(static_cast<std::size_t>(scriptBytes));
        if (scriptBytes && !input.read(loaded.scriptUtf8.data(), static_cast<std::streamsize>(scriptBytes))) {
            return Fail(error, L"The script in the project file is truncated.", L"Сценарий в файле проекта обрезан.");
        }
        if (!ValidUtf8(loaded.scriptUtf8)) {
            return Fail(error, L"The project script contains invalid UTF-8 text.",
                        L"Сценарий в файле проекта повреждён.");
        }
        if (!ReadSamples(input, static_cast<std::size_t>(sampleCount), loaded.samples, error)) return false;

        project = std::move(loaded);
        error.clear();
        return true;
    } catch (const std::bad_alloc&) {
        return Fail(error, L"Insufficient memory to open the project.", L"Не хватает памяти для открытия проекта.");
    } catch (const std::exception&) {
        return Fail(error, L"Could not read the project file.", L"Не удалось прочитать файл проекта.");
    }
}

inline bool Write(const std::filesystem::path& path, const Settings& settings, std::string_view script,
                  std::span<const float> samples, std::wstring& error) {
    using namespace detail;
    try {
        if (script.size() > kMaxScriptBytes || samples.size() > kMaxSampleCount) {
            return Fail(error, L"The script or audio track exceeds the project format's size limit.",
                        L"Размер сценария или аудиодорожки превышает предел формата проекта.");
        }
        if (!ValidSettings(settings, samples.size())) {
            return Fail(error, L"The project settings are invalid.", L"Некорректные настройки проекта.");
        }
        if (!ValidUtf8(script)) {
            return Fail(error, L"The script contains invalid UTF-8 text.",
                        L"Сценарий содержит некорректный текст в кодировке UTF-8.");
        }
        const auto header = EncodeHeader(settings, script.size(), samples.size());

        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (!output) return Fail(error, L"Could not create the project file.", L"Не удалось создать файл проекта.");
        output.write(header.data(), static_cast<std::streamsize>(header.size()));
        if (!script.empty()) output.write(script.data(), static_cast<std::streamsize>(script.size()));
        if (!output) {
            return Fail(error, L"Could not write the project file header or script.",
                        L"Не удалось записать заголовок или сценарий проекта.");
        }
        if (!WriteSamples(output, samples, error)) return false;
        output.close();
        if (!output) {
            return Fail(error, L"Could not finish saving the project file.",
                        L"Не удалось завершить сохранение файла проекта.");
        }
        error.clear();
        return true;
    } catch (const std::bad_alloc&) {
        return Fail(error, L"Insufficient memory to save the project.", L"Не хватает памяти для сохранения проекта.");
    } catch (const std::exception&) {
        return Fail(error, L"Could not write the project file.", L"Не удалось записать файл проекта.");
    }
}

} // namespace project_file
