# Privacy Policy — Podcaster Studio

*Last updated: November 23, 2026*

**Podcaster Studio** (developed by **Vladimir Borozenets**) is an offline, local-first podcast recording studio and teleprompter for Windows.

## 1. Data Collection & Telemetry
- **We collect zero personal data, usage analytics, crash reports, or telemetry.**
- Podcaster Studio does **not** include third-party tracking SDKs, advertising networks, or remote logging.
- Podcaster Studio does **not** send any audio recordings, scripts, device names, or files over the internet.

## 2. Microphone & Audio Access
- Podcaster Studio requests Windows **microphone** access solely so you can monitor your input level and record voice audio when you explicitly trigger monitoring or recording.
- All audio capture, DSP processing (noise gate, de-esser, compressor, EQ, limiter, silence trimming), playback, and WAV/MP3 exports are performed **100% locally on your device**.
- If `ffmpeg.exe` is present locally on your system for MP3 export or non-WAV audio import, it is invoked strictly on local files.

## 3. Local Storage & Backups
- Your scripts, `.podcaster` project files, `.wav`/`.mp3` exports, UI language preference (`ui-language.txt`), and automatic crash/take backups (`backups/`) are saved **only on your local drive** (either in `%LOCALAPPDATA%\Podcaster\` when installed from the Microsoft Store or beside the executable in portable mode).

## 4. Contact
If you have any questions about this Privacy Policy, you can contact the developer via GitHub:  
https://github.com/gluberTH/podcaster/issues

---

# Политика конфиденциальности — Podcaster Studio

*Дата обновления: 23 ноября 2026 г.*

**Podcaster Studio** (разработчик — **Владимир Борозенец / Vladimir Borozenets**) — это локальная студия записи подкастов и телесуфлёр для Windows.

## 1. Сбор данных и телеметрия
- Приложение **не собирает и не передаёт персональные данные, статистику использования, телеметрию или отчёты о сбоях**.
- В Podcaster Studio нет рекламных модулей, трекеров или сторонних аналитических SDK.
- Записи с микрофона, тексты сценариев, названия аудиоустройств и файлы проектов **никогда не отправляются на внешние серверы**.

## 2. Доступ к микрофону и аудио
- Доступ к **микрофону** Windows используется исключительно для отображения уровня сигнала и записи вашего голоса по вашей команде.
- Вся запись, обработка звука (шумоподавление, де-эссер, компрессор, эквалайзер, лимитер, обрезка пауз), воспроизведение и экспорт в WAV/MP3 выполняются **полностью локально на вашем компьютере**.

## 3. Локальное хранение и резервные копии
- Проекты `.podcaster`, аудиофайлы, настройки языка (`ui-language.txt`) и автоматические резервные копии (`backups/`) сохраняются **только на вашем локальном диске** (в папке `%LOCALAPPDATA%\Podcaster\` при установке из Microsoft Store или в каталоге программы в портативном режиме).

## 4. Контакты
По всем вопросам о конфиденциальности можно обратиться через репозиторий проекта на GitHub:  
https://github.com/gluberTH/podcaster/issues
