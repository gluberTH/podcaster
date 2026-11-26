# Podcaster Studio — инструкции подпроекта

## Назначение и стек

**Podcaster Studio** — нативное приложение для Windows (`x64`) для записи подкастов и озвучки со встроенным телесуфлёром, звуковой волной, измерителем уровня dBFS и недеструктивной обработкой голоса в реальном времени (`32-bit float`, `48 kHz`, интерфейс на `EN` и `RU`).

- Язык и API: `C++20`, `Win32`, `Direct2D`, `DirectWrite`, `WASAPI` (через `MSVC 2022` / `cl.exe`).
- Исходный код (`src/`):
  - `main.cpp` — основное окно, транспорт WASAPI, рендер Direct2D, телесуфлёр, экспорт WAV/MP3 и встроенный `--self-test`.
  - `effects.h` — вокальная DSP-цепочка (`Biquad`, срез гула `80 Hz`, шумовой гейт `-42 dBFS`, де-эссер `~6.8 kHz`, компрессор `2:1`, лимитер `-1 dBFS`, 3-полосный EQ, фейды, эхо).
  - `silence.h` — поиск и сокращение длинных пауз с плавными кроссфейдами.
  - `wave_controls.h` — вертикальный фейдер мастер-громкости (`-24..+12 dB`) и панель работы с тишиной.
  - `project_file.h` — формат файлов проекта `.podcaster`.
  - `button_icons.h` — векторные иконки кнопок.
  - `localization.h` — переключатель языка `EN` / `RU`.
  - `resource.rc` — иконка `../podcaster.ico` и `VS_VERSION_INFO`.

## Публикация и ссылки

- **Microsoft Store (`Podcaster Studio`, $1.49 USD):** https://apps.microsoft.com/detail/9NN92VLZZG0S
- **Прямая ссылка для открытия в клиенте Microsoft Store (Windows):** `ms-windows-store://pdp/?productid=9NN92VLZZG0S`
- **Microsoft Partner Center (управление продуктом `9NN92VLZZG0S`):** https://partner.microsoft.com/ru-ru/dashboard/products/9NN92VLZZG0S/overview
- **GitHub (GNU GPLv3):** https://github.com/gluberTH/podcaster
- **Видеодемонстрация (YouTube):** https://youtu.be/1TkvSul8Mk4
- **Политика конфиденциальности (`PRIVACY.md`):** https://github.com/gluberTH/podcaster/blob/main/PRIVACY.md

## Идентификаторы Microsoft Store (`msix/AppxManifest.xml`)

Не изменяй значения `Name`, `Publisher` и `PublisherDisplayName` при сборках для Microsoft Store:

- `DisplayName`: `Podcaster Studio`
- `Package/Identity/Name`: `VladimirBorozenets.PodcasterStudio`
- `Package/Identity/Publisher`: `CN=0B3711A8-3AD0-4347-9D4D-6D3225A56168`
- `Package/Properties/PublisherDisplayName`: `Vladimir Borozenets`
- `Store ID`: `9NN92VLZZG0S`
- Разрешения манифеста: `runFullTrust` и `DeviceCapability Name="microphone"`, ассоциация файлов `.podcaster`.

## Хранение данных (MSIX и портативный режим)

- В установленном из Microsoft Store пакете (`WindowsApps`, папка доступна только для чтения) или из `Program Files` автобэкапы (`backups/`) и настройка языка (`ui-language.txt`) автоматически сохраняются в `%LOCALAPPDATA%\Podcaster\`.
- В обычной портативной папке с правами на запись эти данные хранятся рядом с `Podcaster.exe`.
- Для чтения дефолтного языка из комплекта `LoadLanguagePreference()` использует `ui-language.txt` рядом с `.exe` как резервный вариант.

## Сборка, тесты и выпуск обновлений в Microsoft Store

1. **Локальная сборка `Podcaster.exe`:** запустить `build.bat`.
2. **Обязательная самопроверка после правок кода:** запустить `Podcaster.exe --self-test` и убедиться, что он завершился с `ExitCode=0` и выводом `[PASS] All self-tests succeeded.`.
3. **Подготовка обновления для Microsoft Store:**
   - Повысить 4-компонентную версию `Version` в `msix/AppxManifest.xml` (например, с `1.0.0.0` на `1.1.0.0`; четвёртый компонент для Store оставлять `.0`) и при необходимости обновить версию в `src/resource.rc`.
   - Запустить `package-msix.bat` — скрипт пересоберёт `Podcaster.exe`, уложит файлы (`msix/AppxManifest.xml` и 6 иконок из `msix/Assets/`) и соберёт/проверит `Podcaster.msix` через `makeappx.exe` из Windows SDK.
   - Повторно проверить `Podcaster.exe --self-test`.
   - В Partner Center на странице продукта нажать **«Начать обновление»**, загрузить новый `Podcaster.msix`, при запросе заполнить обоснование `runFullTrust` в «Параметрах отправки» и отправить на сертификацию (публикацию, отправку в Store и `git push` выполнять только по явному поручению Вовы).

## Безопасность публичного репозитория

Папка `podcaster/` связана с публичным GitHub-репозиторием `https://github.com/gluberTH/podcaster`. Никогда не сохраняй в файлах этого каталога личные платёжные, налоговые, адресные данные, номера счетов/телефонов или приватные идентификаторы аккаунта продавца.
