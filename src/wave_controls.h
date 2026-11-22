#pragma once

constexpr float kSilenceToolsHeight = 34.0f;
constexpr float kMinSilenceThresholdDb = -70.0f;
constexpr float kMaxSilenceThresholdDb = -20.0f;
constexpr float kWavePlotLeft = 132.0f;

static void DrawWaveControlLabel(std::wstring_view text, D2D1_RECT_F bounds, D2D1_COLOR_F color,
                                 DWRITE_TEXT_ALIGNMENT alignment = DWRITE_TEXT_ALIGNMENT_LEADING) {
    g.tfUiSmall->SetTextAlignment(alignment);
    g.tfUiSmall->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    g.brush->SetColor(color);
    g.rt->DrawTextW(text.data(), static_cast<UINT32>(text.size()), g.tfUiSmall.Get(), bounds,
                    g.brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

static void DrawWaveSliderThumb(float x, float y, bool active, bool enabled) {
    g.brush->SetColor(enabled ? Theme::AccentCyan : Theme::TextMuted);
    g.rt->FillEllipse(D2D1::Ellipse({x, y}, 5.25f, 5.25f), g.brush.Get());
    if (active) {
        g.brush->SetColor(Theme::TextPrimary);
        g.rt->DrawEllipse(D2D1::Ellipse({x, y}, 5.25f, 5.25f), g.brush.Get(), 1.2f);
    }
}

static void DrawMasterGain(float headerTop, float top, float bottom, float gainDb) {
    const auto panel = D2D1::RectF(20, top, kWavePlotLeft - 8, bottom);
    const float trackX = panel.left + 13;
    const float trackTop = panel.top + 8;
    const float trackBottom = panel.bottom - 8;
    g.rectMasterGainArea = panel;
    g.rectMasterGainTrack = D2D1::RectF(panel.left, trackTop, panel.left + 49, trackBottom);
    g.hitRects.push_back({ID_WAVE_MASTER_GAIN, D2D1::RectF(panel.left, panel.top, panel.left + 51, panel.bottom)});
    DrawWaveControlLabel(Text(L"Gain", L"Громкость"), D2D1::RectF(22, headerTop + 4, panel.right, headerTop + 28), Theme::TextSecond);

    g.brush->SetColor(ColorHex(0x191919));
    g.rt->FillRoundedRectangle(D2D1::RoundedRect(panel, 4, 4), g.brush.Get());
    g.brush->SetColor(Theme::Border);
    g.rt->DrawRoundedRectangle(D2D1::RoundedRect(panel, 4, 4), g.brush.Get(), 1.0f);

    const auto dbToY = [=](float db) {
        return std::lerp(trackBottom, trackTop, (db - kMinMasterGainDb) / (kMaxMasterGainDb - kMinMasterGainDb));
    };
    const float unityY = dbToY(0);
    const float knobY = dbToY(gainDb);
    g.brush->SetColor(ColorHex(0x46433D));
    g.rt->DrawLine({trackX, trackTop}, {trackX, trackBottom}, g.brush.Get(), 2.0f);
    g.brush->SetColor(Theme::AccentCyan);
    if (std::abs(knobY - unityY) > 0.5f) g.rt->DrawLine({trackX, unityY}, {trackX, knobY}, g.brush.Get(), 2.0f);

    struct Tick { float db; const wchar_t* text; };
    const Tick ticks[] = {{kMaxMasterGainDb, L"+12"}, {0, L"0"}, {kMinMasterGainDb, L"−24"}};
    for (const auto& tick : ticks) {
        const float y = dbToY(tick.db);
        g.brush->SetColor(Theme::TextMuted);
        g.rt->DrawLine({trackX + 6, y}, {trackX + 10, y}, g.brush.Get(), 1.0f);
        DrawWaveControlLabel(tick.text, D2D1::RectF(trackX + 13, y - 8, panel.left + 50, y + 8), Theme::TextSecond);
    }
    DrawWaveSliderThumb(trackX, knobY, g.draggingMasterGain || g.hoveredControlId == ID_WAVE_MASTER_GAIN, true);

    wchar_t value[22];
    const float absoluteDb = std::abs(gainDb);
    const wchar_t* sign = gainDb < 0 ? L"−" : gainDb > 0 ? L"+" : L"";
    swprintf_s(value, L"%ls%.1f", sign, absoluteDb);
    DrawWaveControlLabel(value, D2D1::RectF(panel.left + 50, panel.top + 3, panel.right - 5, panel.top + 20), Theme::TextPrimary, DWRITE_TEXT_ALIGNMENT_CENTER);
    DrawWaveControlLabel(Text(L"dB", L"дБ"), D2D1::RectF(panel.left + 50, panel.top + 17, panel.right - 5, panel.top + 33), Theme::TextSecond, DWRITE_TEXT_ALIGNMENT_CENTER);
    DrawButton(ID_BTN_MASTER_GAIN_RESET, D2D1::RectF(panel.left + 50, panel.bottom - 22, panel.right - 4, panel.bottom - 3),
               Text(L"Reset", L"Сброс"), Theme::BgButton, Theme::Border, Theme::TextSecond, g.tfUiSmall.Get(), gainDb != 0);
}

static void DrawSilenceTools(float top, float width, bool hasAudio) {
    const auto panel = D2D1::RectF(20, top, width - 20, top + 30);
    g.brush->SetColor(Theme::BgPanelAlt);
    g.rt->FillRoundedRectangle(D2D1::RoundedRect(panel, 4, 4), g.brush.Get());
    DrawWaveControlLabel(Text(L"Threshold", L"Порог тишины"), D2D1::RectF(28, top, 117, top + 30), Theme::TextPrimary);

    g.rectSilenceThresholdTrack = D2D1::RectF(120, top + 4, 272, top + 26);
    const auto& track = g.rectSilenceThresholdTrack;
    const bool thresholdEnabled = hasAudio && !g.silenceApplyPending;
    if (thresholdEnabled) g.hitRects.push_back({ID_SILENCE_THRESHOLD, D2D1::RectF(track.left - 6, track.top, track.right + 6, track.bottom)});
    const float y = (track.top + track.bottom) * 0.5f;
    const float amount = (g.silenceThresholdDb - kMinSilenceThresholdDb) / (kMaxSilenceThresholdDb - kMinSilenceThresholdDb);
    const float thumbX = std::lerp(track.left, track.right, amount);
    g.brush->SetColor(ColorHex(0x57534B));
    g.rt->DrawLine({track.left, y}, {track.right, y}, g.brush.Get(), 2.0f);
    g.brush->SetColor(thresholdEnabled ? Theme::AccentCyan : Theme::TextMuted);
    g.rt->DrawLine({track.left, y}, {thumbX, y}, g.brush.Get(), 2.0f);
    DrawWaveSliderThumb(thumbX, y, g.draggingSilenceThreshold || g.hoveredControlId == ID_SILENCE_THRESHOLD, thresholdEnabled);

    const auto thresholdText = L"−" + std::to_wstring(static_cast<int>(-g.silenceThresholdDb)) + Text(L" dBFS", L" дБFS");
    DrawWaveControlLabel(thresholdText, D2D1::RectF(284, top, 353, top + 30), Theme::TextPrimary);

    std::wstring status = Text(L"No scan results", L"Нет результатов поиска");
    if (g.silenceApplyPending) status = Text(L"Trimming pauses…", L"Сокращаем паузы…");
    else if (!hasAudio) status = Text(L"No audio yet", L"Пока нет записи");
    else if (g.silenceScanPending) status = Text(L"Finding pauses…", L"Поиск пауз…");
    else if (g.silencePreviewReady) {
        if (g.silenceRanges.empty()) status = Text(L"No long pauses found", L"Длинных пауз не найдено");
        else {
            const size_t removedSamples = silence::RemovedSamples(g.silenceRanges);
            status = Text(L"Marked pauses · ", L"Пауз к сокращению · ") + std::to_wstring(g.silenceRanges.size()) +
                L"  /  −" + FormatTime(static_cast<double>(removedSamples) / kSampleRate);
        }
    }
    DrawWaveControlLabel(status, D2D1::RectF(360, top, width - 265, top + 30), Theme::TextSecond);

    const bool ready = hasAudio && g.silencePreviewReady && !g.silenceScanPending && !g.silenceApplyPending && !g.silenceRanges.empty();
    DrawButton(ID_BTN_SILENCE_APPLY, D2D1::RectF(width - 253, top + 3, width - 113, top + 27), Text(L"Trim pauses", L"Сократить"),
               Theme::BgButtonActive, Theme::BorderActive, Theme::TextPrimary, g.tfUiSmall.Get(), ready);
    DrawButton(ID_BTN_SILENCE_CLOSE, D2D1::RectF(width - 107, top + 3, width - 24, top + 27), Text(L"Close", L"Закрыть"),
               Theme::BgButton, Theme::Border, Theme::TextSecond, g.tfUiSmall.Get(), !g.silenceApplyPending);
}

static void DrawSilencePreview(double viewStartSec, double viewDurationSec, float waveBodyTop) {
    if (g.silenceScanPending || !g.silencePreviewReady) return;
    const auto& wave = g.rectWaveArea;
    const double firstSample = viewStartSec * kSampleRate;
    const double samplesToPixels = (wave.right - wave.left) / (viewDurationSec * kSampleRate);
    const auto tail = g.silenceRanges.end();
    auto range = std::lower_bound(g.silenceRanges.begin(), tail, firstSample,
                                  [](const auto& r, double first) { return r.end < first; });
    for (; range != tail; ++range) {
        const float left = std::max(wave.left, wave.left + static_cast<float>((range->start - firstSample) * samplesToPixels));
        if (left >= wave.right) break;
        const float right = std::min(wave.right, wave.left + static_cast<float>((range->end - firstSample) * samplesToPixels));
        if (left >= right) continue;
        g.brush->SetColor(ColorHex(0x76C3AC, 0.20f));
        g.rt->FillRectangle(D2D1::RectF(left, waveBodyTop, right, wave.bottom), g.brush.Get());
        g.brush->SetColor(ColorHex(0x76C3AC, 0.83f));
        g.rt->DrawLine({left, waveBodyTop + 1}, {right, waveBodyTop + 1}, g.brush.Get(), 2.0f);
    }
}
