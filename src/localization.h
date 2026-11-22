#pragma once

#include <atomic>

namespace locale_ui {

enum class Language { English, Russian };

inline std::atomic<Language> language{Language::English};

inline Language Current() noexcept {
    return language.load(std::memory_order_relaxed);
}

inline void Set(Language value) noexcept {
    language.store(value, std::memory_order_relaxed);
}

inline const wchar_t* Text(const wchar_t* english, const wchar_t* russian) noexcept {
    return Current() == Language::Russian ? russian : english;
}

} // namespace locale_ui
