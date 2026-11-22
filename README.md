[Русский](#-подкастер--студия-записи-и-телесуфлёр-для-windows) · [English](#-podcaster--recording-studio--teleprompter-for-windows)

---

# Подкастер — студия записи и телесуфлёр для Windows

Нативное приложение на **C++20** (`Win32 + Direct2D + DirectWrite + WASAPI`) для быстрой записи сольных подкастов. В одном лёгком окне собраны телесуфлёр с автопрокруткой, звуковая волна высокой чёткости, перезапись неудачных дублей, сокращение длинных пауз, регулятор громкости, неразрушающие эффекты, проекты `.podcaster`, автобэкапы и экспорт в WAV/MP3.

## Быстрый старт и язык интерфейса

- Запустите [`Podcaster.exe`](Podcaster.exe) двойным кликом.
- Переключение языка интерфейса (**RU / EN**) — кнопка **`RU ▾` / `EN ▾`** в верхней панели. Выбранный язык сохраняется в `ui-language.txt` рядом с программой (по умолчанию используется английский, пока не выбран русский).

### Аудиофайлы и проекты (`Файл ▾`)

- **Файл → Открыть аудио или проект…** (`Ctrl+O`) открывает `.podcaster`, `WAV`, `MP3`, `FLAC`, `M4A/AAC`, `Ogg`, `Opus`, `AIFF` и `WMA`. Программа приводит импорт к моно 48 кГц. `WAV` с частотой 48 кГц и поддерживаемой разрядностью открывается напрямую; для перекодирования и остальных форматов нужен `ffmpeg.exe`.
- При открытии обычного аудиофайла текущий текст суфлёра сохраняется, эффекты и громкость сбрасываются, а курсор ставится в конец дорожки для дозаписи. Кнопка **Слушать** воспроизводит такой файл с начала.
- **Файл → Сохранить проект** (`Ctrl+S`) записывает самодостаточный файл `.podcaster`, содержащий аудиодорожку (32-bit float), текст суфлёра, состояние всех эффектов, 3-полосный EQ, громкость, порог тишины, настройки суфлёра, масштаб волны, курсор и выделение. `Ctrl+Shift+S` (**Сохранить проект как…**) сохраняет копию под новым именем.
- **Файл → Открыть резервную запись…** открывает папку `backups/` с последним автосохранением и снимками дублей `take_*.wav`.
- История отмены `Ctrl+Z` хранится в пределах текущего сеанса (до 12 шагов).

### Работа со сценарием (телесуфлёр)

- **Сценарий** (`Ctrl+Shift+O`) — открыть `.md` или `.txt` для суфлёра. Markdown-разметка упрощается для чтения вслух, маркеры пауз `//` сохраняются.
- **Вставить** (`Ctrl+V`) — вставить текст из буфера обмена.
- **Редактор** — открыть встроенное окно редактирования сценария.
- **Свежий выпуск** — в один клик загрузить сценарий самого свежего выпуска из `episodes/`.
- **В начало** (`Home`) — перемотать текст к первой строке.
- **Автоскролл вкл. / выкл.** — автоматическая прокрутка во время записи.
- **Стрелки `←` / `→`** (или кнопки у счётчика скорости) — регулировка скорости (`10–180` пкс/с, шаг `5`).
- **Стрелки `↑` / `↓`, колесо мыши или перетаскивание** — плавная ручная прокрутка текста; **`A-` / `A+`** меняют размер шрифта (`16–46` пкс). Если в Windows отключены эффекты анимации, прокрутка срабатывает мгновенно.

### Запись, прослушивание и перезапись

- **`Пробел`** (или клик по микрофону / кнопке **Запись** / **Стоп**) — старт и остановка записи. Говорить можно, когда вместо «Подключаем микрофон…» появляется красный значок **«Запись»**. Если в этот момент идёт воспроизведение, пробел останавливает его.
- **`Enter`** или **`P`** — **Слушать / Пауза**.
- **`0:00`** — перемотать курсор в начало дорожки.
- **К громкому** — 1-й клик ставит курсор сразу после последнего громкого слова (пропуская тишину в конце, чтобы сразу продолжать запись); повторный клик переносит курсор в начало последней фразы для быстрой перезаписи.
- **Прокрутка и зум волны**: колесо мыши над волной прокручивает дорожку по времени, **`Ctrl` + колесо мыши** (или кнопки масштаба и **Весь трек**) — приближает и отдаляет. При сильном приближении волна строится напрямую по сэмплам.
- **Выбор устройств** — кнопки **`Вход:`** и **`Выход:`** в верхней панели (при наличии `Samson G-Track Pro` выбирается автоматически; кнопка обновления **`↻`** перечитывает список WASAPI-устройств).
- **Перезапись ошибки**:
  1. Кликните по звуковой волне перед ошибкой и нажмите `Пробел` — новая запись перезапишет хвост от позиции курсора.
  2. Или выделите мышкой неудачный фрагмент и нажмите `Пробел` — новый дубль заменит только выделение с плавными стыками до 5 мс, сохранив аудио до и после него (замена применяется после поступления первых сэмплов с микрофона).
- **`Delete` / `Backspace`** (или **Удалить выделение**) — удалить выделенный фрагмент.
- **`Ctrl+Z`** (**Отменить**) — отменить последнее изменение дорожки, включая перезапись, удаление или сокращение пауз (до 12 шагов).
- **Очистить** — очистить текущую дорожку (можно вернуть через `Ctrl+Z`).

### Паузы и громкость

- Нажмите **Тишина** над волной, чтобы открыть панель сокращения длинных пауз:
  - Регулятор **Порог тишины** от `−70` до `−20 дБFS` (по умолчанию `−42 дБFS`).
  - Программа ищет паузы от `0,5 с` на исходной дорожке, оставляет по `0,1 с` запаса на границах речи и подсвечивает вырезаемые участки на волне, показывая число пауз и итоговое сокращение длительности.
  - **Сократить** применяет монтаж со сглаживанием стыков (отменяется через `Ctrl+Z`), **Закрыть** (или `Esc`) скрывает панель.
- **Громкость** слева от волны регулируется перетаскиванием или колесом мыши от `−24` до `+12 дБ` (**Сброс** возвращает `0 дБ`). Влияет на прослушивание и экспорт после цепочки эффектов.
- Показатель **Пик дБFS** над волной подсвечивается при перегрузе (`> 0 dBFS`); включённый **Лимитер** удерживает пики в пределах `−1 dBFS`.

### Эффекты (неразрушающие переключатели, 32-bit float / 48 кГц)

Все эффекты работают поверх исходной дорожки как переключатели **ВКЛ / ВЫКЛ** и пересчитываются в фоновом потоке, не блокируя интерфейс и телесуфлёр:

- **Очистка звука** — обрезной фильтр низких частот (`80 Гц`), гейт (`−42 dBFS`) и нормализация пиков до `−1 dBFS` (без двойного применения, если отдельно включены срез гула или гейт).
- **Фейд-ин** и **Фейд-аут** — плавное нарастание и затухание громкости по краям дорожки (до `1,5 с`).
- **Эхо** — тёплое студийное эхо (`140 мс` с затуханием верхних частот в обратной связи).
- **3-полосный эквалайзер** (`Низ 120 Гц`, `Сер 2,8 кГц`, `Выс 9 кГц`) — регулировка кнопками `−`/`+`, переключатель **`EQ вкл. / выкл.`** и кнопка **`Пресет Голос`** (`+2,0 / +2,5 / +1,5 дБ`).
- **Срез гула** — фильтр верхних частот на `80 Гц`.
- **Шумовой гейт** — подавление фона в паузах (порог `−42 dBFS`).
- **Де-эссер** — смягчение резких свистящих звуков в районе `6,8 кГц`.
- **Компрессор** — выравнивание громкости речи (порог `−18 dBFS`, `2:1`).
- **Лимитер** — ограничение пиков на уровне `−1 dBFS`.

### Экспорт и автобэкапы (`backups/`)

- **Экспорт WAV** — переключатель **`32-бит float`** или **`24-бит PCM`** (моно, `48 кГц`; 24-битный PCM с TPDF-дизерингом).
- **MP3 (320 кбит/с)** — кодирование через `ffmpeg.exe` (`libmp3lame`, `320 kbps`, `48 кГц`).
- Экспорт выполняется в фоне со снимком дорожки на момент нажатия; перед закрытием окна дождитесь сообщения о завершении.
- После остановки записи или правки дорожки исходник автоматически сохраняется в `backups/autosave_latest.wav` (`32-bit float`), хранятся до 10 последних дублей `backups/take_*.wav`, а текст суфлёра сохраняется в `backups/autosave_script.txt`. При следующем запуске последняя запись и текст восстанавливаются автоматически.

### Структура проекта и сборка

- [`src/main.cpp`](src/main.cpp) — Win32-окно, Direct2D/DirectWrite-рендеринг, WASAPI-движок, телесуфлёр, фоновые потоки, экспорт и `--self-test`.
- [`src/effects.h`](src/effects.h) — DSP-фильтры (`Biquad`, `RemoveRumble`, `ApplyNoiseGate`, `ApplyDeEsser`, `ApplyCompressor`, `ApplyLimiter`).
- [`src/silence.h`](src/silence.h) — анализ пауз и монтаж со сглаживанием стыков.
- [`src/wave_controls.h`](src/wave_controls.h) — панель громкости и интерфейс сокращения тишины.
- [`src/project_file.h`](src/project_file.h) — бинарный формат файла проекта `.podcaster`.
- [`src/button_icons.h`](src/button_icons.h) — векторные иконки кнопок.
- [`src/localization.h`](src/localization.h) — переключатель языка `EN / RU`.
- [`src/resource.rc`](src/resource.rc) и [`podcaster.ico`](podcaster.ico) — иконка приложения и метаданные `VERSIONINFO`.

Требуется Visual Studio 2022 (`MSVC C++20`) и Windows SDK:

```bat
.\build.bat
```

Сборка отдельной копии и запуск самопроверки без окна GUI:

```powershell
& .\build.bat "$env:TEMP\Podcaster-test.exe"
& .\Podcaster.exe --self-test | Out-String
```

### Лицензия

Copyright (C) 2026 Vladimir Borozenets

Проект распространяется по лицензии **[GNU General Public License v3.0 (GPL-3.0)](LICENSE)**.
Любые производные работы и распространяемые сборки на основе этого кода также должны публиковаться с открытым исходным кодом под лицензией GPLv3; включение кода в закрытые коммерческие проприетарные продукты без отдельной лицензии от автора запрещено. Для коммерческого закрытого использования возможна отдельная договорённость с автором.

---

# Podcaster — Recording Studio & Teleprompter for Windows

A lightweight native **C++20** (`Win32 + Direct2D + DirectWrite + WASAPI`) desktop studio built for solo podcast recording. One compact window combines an auto-scrolling teleprompter, high-detail waveform view, punch-in re-recording, silence trimming, master gain fader, non-destructive DSP effects, `.podcaster` project files, automatic backups, and lossless/MP3 export.

## Quick Start & Interface Language

- Launch [`Podcaster.exe`](Podcaster.exe).
- Switch between **English** and **Russian** anytime via the **`EN ▾` / `RU ▾`** button in the top bar. Your choice is saved in `ui-language.txt` next to the executable (defaults to English unless Russian is selected).

### Audio Files & Projects (`File ▾`)

- **File → Open audio or project…** (`Ctrl+O`) opens `.podcaster`, `WAV`, `MP3`, `FLAC`, `M4A/AAC`, `Ogg`, `Opus`, `AIFF`, and `WMA`. Imports are converted to 48 kHz mono (supported 48 kHz WAV files load directly; other formats/sample rates use `ffmpeg.exe`).
- Opening a standalone audio file preserves the current teleprompter text, resets effect toggles and master gain, and places the playhead at the end so you can continue recording. Pressing **Play** on an untouched file plays from the beginning.
- **File → Save project** (`Ctrl+S`) saves a self-contained `.podcaster` file containing the raw 32-bit float audio track, teleprompter script, effect toggles, 3-band EQ, master gain, silence threshold, teleprompter settings, waveform zoom/scroll, playhead, and selection. Use **`Ctrl+Shift+S`** (**Save project as…**) to save a separate copy.
- **File → Open backup recording…** opens the `backups/` folder with the latest autosave and `take_*.wav` snapshots.
- `Ctrl+Z` undo history (up to 12 edits) is kept for the active session.

### Teleprompter

- **Script** (`Ctrl+Shift+O`) — open a `.md` or `.txt` script file (Markdown syntax is simplified for reading aloud while keeping `//` pause markers visible).
- **Paste** (`Ctrl+V`) — paste text from the clipboard.
- **Editor** — open the built-in modal text editor.
- **Latest episode** — one-click load of the newest script in `episodes/`.
- **To top** (`Home`) — jump back to the start of the script.
- **Auto-scroll on / off** — automatically scroll while recording.
- **`←` / `→` Arrow keys** (or the speed buttons) — adjust scroll speed (`10–180` px/s in `5` px/s steps).
- **`↑` / `↓` Arrow keys, mouse wheel, or drag** — smooth manual scrolling; **`A-` / `A+`** adjust font size (`16–46` px). Respects Windows animation settings.

### Recording, Playback & Re-Recording

- **`Space`** (or clicking the microphone illustration / **Record** / **Stop** button) — start or stop recording. Wait for **"Connecting microphone…"** to turn into the red **REC** badge before speaking. If playback is active, `Space` stops it.
- **`Enter`** or **`P`** — **Play / Pause**.
- **`0:00`** — jump playhead to the beginning of the track.
- **Last sound** — 1st click places the playhead right after the last spoken word (skipping trailing silence so you can immediately resume recording); 2nd click moves it to the start of the last phrase for quick punch-in re-recording.
- **Waveform scrolling & zoom**: mouse wheel over the waveform scrolls horizontally; **`Ctrl` + mouse wheel** (or zoom buttons / **Fit track**) zooms in and out. Deep zoom renders exact per-pixel sample ranges and sample-to-sample curves.
- **Audio devices** — **`Input:`** and **`Output:`** selectors in the top bar (auto-selects `Samson G-Track Pro` if connected; refresh button **`↻`** rescans active WASAPI endpoints).
- **Re-recording mistakes**:
  1. Click the waveform before the mistake and press `Space` to overwrite from the playhead forward.
  2. Or drag a selection over a bad section and press `Space` — the new take replaces only the selected interval with smooth crossfades (up to 5 ms) at the splice points once the first microphone samples arrive.
- **`Delete` / `Backspace`** (or **Delete selection (Del)**) — delete the selected interval.
- **`Ctrl+Z`** (**Undo**) — undo the last track edit, including re-recording takes, cuts, or silence shortening (up to 12 steps).
- **Clear** — clear the current audio track (recoverable via `Ctrl+Z`).

### Silence Shortening & Master Gain

- Click **Silence** above the waveform to open the silence-trimming bar:
  - Adjust the threshold slider from `−70` to `−20 dBFS` (default `−42 dBFS`).
  - Scans the raw track for pauses `>= 0.5 s`, keeps `0.1 s` padding near speech boundaries, highlights cut regions on the waveform, and reports pause count plus total time saved.
  - Apply the edits with smooth splices (undoable via `Ctrl+Z`) or press `Esc` to close the bar.
- Drag or scroll the **Master Gain** fader on the left of the waveform (`−24` to `+12 dB`, with reset to `0 dB`). Scales playback and export after the effects chain.
- The **Peak dBFS** readout above the waveform highlights clipping (`> 0 dBFS`); enabling **Limiter** caps sample peaks at `−1 dBFS`.

### Non-Destructive Effects (32-bit float / 48 kHz, Background Processing)

All effects operate non-destructively as **ON / OFF** toggles over the raw recording and re-render in the background without freezing the UI or teleprompter:

- **Voice cleanup** — 80 Hz high-pass rumble cut + downward noise gate (`−42 dBFS`) + peak normalization to `−1 dBFS` (avoids double-applying HPF/gate when standalone toggles are also active).
- **Fade in** / **Fade out** — smooth S-curve fades at track edges (up to `1.5 s`).
- **Echo** — warm damped studio delay (`140 ms` with 3.5 kHz low-pass feedback damping).
- **3-band EQ** (`Low 120 Hz`, `Mid 2.8 kHz`, `Hi 9 kHz`) — `−`/`+` controls, **`EQ on / off`** toggle, and **`Voice preset`** (`+2.0 / +2.5 / +1.5 dB`).
- **Low cut** — 80 Hz Butterworth high-pass filter.
- **Noise gate** — gentle downward expander (`−42 dBFS` threshold).
- **De-esser** — reduces harsh sibilance centered around `6.8 kHz`.
- **Compressor** — smooth 2:1 vocal compression above `−18 dBFS`.
- **Limiter** — keeps output peaks at or below `−1 dBFS`.

### Export & Automatic Backups (`backups/`)

- **Export WAV** — toggle between **`32-bit float`** and **`24-bit PCM`** (`48 kHz` mono; 24-bit PCM includes 1-LSB TPDF dither).
- **MP3 (320 kbps)** — encodes via `ffmpeg.exe` (`libmp3lame`, `320 kbps`, `48 kHz`).
- Exports run asynchronously on a snapshot of the track; wait for the completion status before closing the window.
- Recording stops and track edits automatically save `backups/autosave_latest.wav` (`32-bit float`), up to 10 rolling `backups/take_*.wav` snapshots, and `backups/autosave_script.txt`. On the next startup, the latest audio and script restore automatically.

### Keyboard Shortcuts

| Shortcut | Action |
| --- | --- |
| `Space` | Record / Stop (or stop playback) |
| `Enter` / `P` | Play / Pause |
| `←` / `→` | Teleprompter speed `−` / `+` |
| `↑` / `↓` / `Home` | Scroll teleprompter up / down / to top |
| `Ctrl` + Mouse Wheel (wave) | Zoom waveform in / out |
| Mouse Wheel (wave) | Scroll waveform horizontally |
| `Delete` / `Backspace` | Delete selected audio range |
| `Ctrl+Z` | Undo (up to 12 steps) |
| `Ctrl+O` / `Ctrl+Shift+O` | Open audio or `.podcaster` project / Open text script |
| `Ctrl+S` / `Ctrl+Shift+S` | Save project / Save project as… |
| `Ctrl+V` | Paste clipboard text into teleprompter |
| `Esc` | Stop transport / close silence bar / clear selection |

### Project Layout & Building from Source

- [`src/main.cpp`](src/main.cpp) — Win32 UI, Direct2D/DirectWrite rendering, WASAPI audio engine, teleprompter, background workers, export, and `--self-test`.
- [`src/effects.h`](src/effects.h) — DSP filters (`Biquad`, `RemoveRumble`, `ApplyNoiseGate`, `ApplyDeEsser`, `ApplyCompressor`, `ApplyLimiter`).
- [`src/silence.h`](src/silence.h) — silence detection and crossfaded splice engine.
- [`src/wave_controls.h`](src/wave_controls.h) — master gain fader and silence-removal toolbar UI.
- [`src/project_file.h`](src/project_file.h) — `.podcaster` binary project reader/writer.
- [`src/button_icons.h`](src/button_icons.h) — vector button icons.
- [`src/localization.h`](src/localization.h) — `EN` / `RU` localization helpers.
- [`src/resource.rc`](src/resource.rc) & [`podcaster.ico`](podcaster.ico) — application icon and `VERSIONINFO`.

Requires Visual Studio 2022 (`MSVC C++20`) and the Windows SDK:

```bat
.\build.bat
```

Build a separate test binary and run the headless self-test suite:

```powershell
& .\build.bat "$env:TEMP\Podcaster-test.exe"
& .\Podcaster.exe --self-test | Out-String
```

### License

Copyright (C) 2026 Vladimir Borozenets

This project is licensed under the **[GNU General Public License v3.0 (GPL-3.0)](LICENSE)**.
Any derivative works or redistributed builds based on this code must also remain open-source under the GPLv3; incorporating this code into closed-source proprietary commercial software without a separate license from the author is not permitted. Separate commercial licensing for closed-source use may be arranged with the author.
