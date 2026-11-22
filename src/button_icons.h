#pragma once

enum class ButtonIcon {
    None, Microphone, Speaker, Refresh, Undo, Trash, Folder, Clipboard, Pencil, Document,
    Top, Scroll, Left, Right, Record, Stop, Play, Pause, Rewind, Wave, ZoomIn, ZoomOut, Fit,
    Deselect, Cleanup, FadeIn, FadeOut, Echo, Equalizer, Highpass, Gate, DeEsser, Compressor,
    Limiter, Bits, Export, Scissors, Globe, ChevronDown
};

static ButtonIcon GetButtonIcon(int id) {
    switch (id) {
        case ID_BTN_INPUT_DEV:       return ButtonIcon::Microphone;
        case ID_BTN_OUTPUT_DEV:      return ButtonIcon::Speaker;
        case ID_BTN_REFRESH_DEV:     return ButtonIcon::Refresh;
        case ID_BTN_UNDO:            return ButtonIcon::Undo;
        case ID_BTN_CLEAR_AUDIO:
        case ID_BTN_DEL_SEL:         return ButtonIcon::Trash;
        case ID_BTN_FILE_MENU:       return ButtonIcon::Folder;
        case ID_BTN_LANGUAGE:        return ButtonIcon::Globe;
        case ID_BTN_OPEN_TEXT:       return ButtonIcon::Document;
        case ID_BTN_PASTE_TEXT:      return ButtonIcon::Clipboard;
        case ID_BTN_EDIT_TEXT:       return ButtonIcon::Pencil;
        case ID_BTN_LOAD_LATEST_EP:  return ButtonIcon::Document;
        case ID_BTN_PROMPTER_TOP:    return ButtonIcon::Top;
        case ID_BTN_AUTOSCROLL:      return ButtonIcon::Scroll;
        case ID_BTN_SPEED_DOWN:      return ButtonIcon::Left;
        case ID_BTN_SPEED_UP:        return ButtonIcon::Right;
        case ID_BTN_REC_STOP:        return g.isRecording.load() || g.isPlaying.load() ? ButtonIcon::Stop : ButtonIcon::Record;
        case ID_BTN_PLAY_STOP:       return g.isPlaying.load() ? ButtonIcon::Pause : ButtonIcon::Play;
        case ID_BTN_REWIND_START:    return ButtonIcon::Rewind;
        case ID_BTN_JUMP_LOUD:       return ButtonIcon::Wave;
        case ID_BTN_WAVE_ZOOM_IN:    return ButtonIcon::ZoomIn;
        case ID_BTN_WAVE_ZOOM_OUT:   return ButtonIcon::ZoomOut;
        case ID_BTN_WAVE_ZOOM_ALL:   return ButtonIcon::Fit;
        case ID_BTN_WAVE_DESELECT:   return ButtonIcon::Deselect;
        case ID_BTN_FX_CLEANUP:      return ButtonIcon::Cleanup;
        case ID_BTN_FX_FADE_IN:      return ButtonIcon::FadeIn;
        case ID_BTN_FX_FADE_OUT:     return ButtonIcon::FadeOut;
        case ID_BTN_FX_ECHO:         return ButtonIcon::Echo;
        case ID_BTN_FX_EQ_APPLY:     return ButtonIcon::Equalizer;
        case ID_BTN_FX_EQ_PRESET:    return ButtonIcon::Microphone;
        case ID_BTN_FX_HIGHPASS:     return ButtonIcon::Highpass;
        case ID_BTN_FX_GATE:         return ButtonIcon::Gate;
        case ID_BTN_FX_DEESSER:      return ButtonIcon::DeEsser;
        case ID_BTN_FX_COMPRESSOR:   return ButtonIcon::Compressor;
        case ID_BTN_FX_LIMITER:      return ButtonIcon::Limiter;
        case ID_BTN_WAV_BITDEPTH:    return ButtonIcon::Bits;
        case ID_BTN_EXPORT_WAV:
        case ID_BTN_EXPORT_MP3:      return ButtonIcon::Export;
        case ID_BTN_SILENCE_TOOLS:
        case ID_BTN_SILENCE_APPLY:   return ButtonIcon::Scissors;
        case ID_BTN_SILENCE_CLOSE:   return ButtonIcon::Deselect;
        default:                    return ButtonIcon::None;
    }
}

static void DrawButtonIcon(ButtonIcon icon, float x, float y, D2D1_COLOR_F color) {
    constexpr float stroke = 1.25f;
    g.brush->SetColor(color);
    auto line = [&](float ax, float ay, float bx, float by) {
        g.rt->DrawLine(D2D1::Point2F(x + ax, y + ay), D2D1::Point2F(x + bx, y + by), g.brush.Get(), stroke);
    };
    auto trace = [&](std::initializer_list<D2D1_POINT_2F> points) {
        for (auto p = points.begin() + 1; p != points.end(); ++p) {
            line((p - 1)->x, (p - 1)->y, p->x, p->y);
        }
    };
    auto rect = [&](float l, float t, float r, float b, float radius = 1.0f) {
        return D2D1::RoundedRect(D2D1::RectF(x + l, y + t, x + r, y + b), radius, radius);
    };
    auto circle = [&](float cx, float cy, float radiusX, float radiusY) {
        return D2D1::Ellipse(D2D1::Point2F(x + cx, y + cy), radiusX, radiusY);
    };

    switch (icon) {
        case ButtonIcon::Microphone:
            g.rt->DrawRoundedRectangle(rect(-2.5f, -7, 2.5f, 2, 2.5f), g.brush.Get(), stroke);
            trace({{-5, -1}, {-5, 1}, {-3, 4}, {0, 5}, {3, 4}, {5, 1}, {5, -1}});
            line(0, 5, 0, 7);
            line(-2.5f, 7, 2.5f, 7);
            break;
        case ButtonIcon::Speaker:
            trace({{0, -6}, {-4, -2}, {-7, -2}, {-7, 2}, {-4, 2}, {0, 6}, {0, -6}});
            trace({{3, -3}, {4.5f, 0}, {3, 3}});
            trace({{5.5f, -5}, {7.5f, 0}, {5.5f, 5}});
            break;
        case ButtonIcon::Refresh:
            trace({{5.5f, -4}, {3, -6}, {-1, -6.5f}, {-4, -5}, {-6, -2}, {-6, 2}, {-4, 5}, {-1, 6.5f}, {3, 6}, {5.5f, 3.5f}});
            trace({{5.5f, -7}, {5.5f, -3}, {1.5f, -3}});
            break;
        case ButtonIcon::Undo:
            trace({{-2, -6}, {-6, -2}, {-2, 2}});
            trace({{-6, -2}, {2, -2}, {5, -0.5f}, {6, 2}, {5, 4.5f}, {2, 6}, {-1, 6}});
            break;
        case ButtonIcon::Trash:
            trace({{-4.5f, -3}, {-3.5f, 6.5f}, {3.5f, 6.5f}, {4.5f, -3}});
            line(-6, -3.5f, 6, -3.5f);
            trace({{-2.5f, -4}, {-2.5f, -6.5f}, {2.5f, -6.5f}, {2.5f, -4}});
            line(-1.5f, -1.5f, -1.5f, 4);
            line(1.5f, -1.5f, 1.5f, 4);
            break;
        case ButtonIcon::Folder:
            trace({{-6.5f, 5}, {-6.5f, -5.5f}, {-2, -5.5f}, {0, -3.5f}, {6.5f, -3.5f}, {6.5f, 5}, {-6.5f, 5}});
            line(-6, -1, 6, -1);
            break;
        case ButtonIcon::Clipboard:
            trace({{-2.5f, -5}, {-5, -5}, {-5, 6.5f}, {5, 6.5f}, {5, -5}, {2.5f, -5}});
            g.rt->DrawRoundedRectangle(rect(-2.7f, -7, 2.7f, -3.5f), g.brush.Get(), stroke);
            line(-2.5f, 0.5f, 2.5f, 0.5f);
            line(-2.5f, 3, 1.5f, 3);
            break;
        case ButtonIcon::Pencil:
            trace({{-5.5f, 2.5f}, {3.5f, -6.5f}, {6.5f, -3.5f}, {-2.5f, 5.5f}, {-6.5f, 6.5f}, {-5.5f, 2.5f}});
            line(1.5f, -4.5f, 4.5f, -1.5f);
            break;
        case ButtonIcon::Document:
            trace({{-4.5f, -6.5f}, {1.5f, -6.5f}, {5, -3}, {5, 6.5f}, {-4.5f, 6.5f}, {-4.5f, -6.5f}});
            trace({{1.5f, -6.5f}, {1.5f, -3}, {5, -3}});
            line(-2, 0, 2.5f, 0);
            line(-2, 3, 2.5f, 3);
            break;
        case ButtonIcon::Top:
            line(-5.5f, -6, 5.5f, -6);
            line(0, -2.5f, 0, 6.5f);
            trace({{-4, 1.5f}, {0, -2.5f}, {4, 1.5f}});
            break;
        case ButtonIcon::Scroll:
            line(0, -6, 0, 6);
            trace({{-3.5f, -2.5f}, {0, -6}, {3.5f, -2.5f}});
            trace({{-3.5f, 2.5f}, {0, 6}, {3.5f, 2.5f}});
            break;
        case ButtonIcon::Left:
            trace({{2, -4}, {-2.5f, 0}, {2, 4}});
            break;
        case ButtonIcon::Right:
            trace({{-2, -4}, {2.5f, 0}, {-2, 4}});
            break;
        case ButtonIcon::Record:
            g.rt->FillEllipse(circle(0, 0, 4.4f, 4.4f), g.brush.Get());
            break;
        case ButtonIcon::Stop:
            g.rt->FillRoundedRectangle(rect(-4.5f, -4.5f, 4.5f, 4.5f), g.brush.Get());
            break;
        case ButtonIcon::Play:
            trace({{-3.5f, -5.5f}, {5, 0}, {-3.5f, 5.5f}, {-3.5f, -5.5f}});
            break;
        case ButtonIcon::Pause:
            g.rt->FillRoundedRectangle(rect(-4, -5, -1.5f, 5, 0.6f), g.brush.Get());
            g.rt->FillRoundedRectangle(rect(1.5f, -5, 4, 5, 0.6f), g.brush.Get());
            break;
        case ButtonIcon::Rewind:
            line(-5, -5, -5, 5);
            trace({{4.5f, -5}, {-2, 0}, {4.5f, 5}, {4.5f, -5}});
            break;
        case ButtonIcon::Wave:
            trace({{-7, 0}, {-4, 0}, {-2.5f, -5}, {0, 6}, {2.5f, -5}, {4, 0}, {7, 0}});
            break;
        case ButtonIcon::ZoomIn:
        case ButtonIcon::ZoomOut:
            g.rt->DrawEllipse(circle(-1.5f, -1.5f, 4.6f, 4.6f), g.brush.Get(), stroke);
            line(2, 2, 6, 6);
            line(-4, -1.5f, 1, -1.5f);
            if (icon == ButtonIcon::ZoomIn) line(-1.5f, -4, -1.5f, 1);
            break;
        case ButtonIcon::Fit:
            trace({{-3, -5.5f}, {-6, -5.5f}, {-6, -2.5f}});
            trace({{3, -5.5f}, {6, -5.5f}, {6, -2.5f}});
            trace({{-3, 5.5f}, {-6, 5.5f}, {-6, 2.5f}});
            trace({{3, 5.5f}, {6, 5.5f}, {6, 2.5f}});
            line(-2.5f, 0, 2.5f, 0);
            break;
        case ButtonIcon::Deselect:
            line(-4.5f, -4.5f, 4.5f, 4.5f);
            line(-4.5f, 4.5f, 4.5f, -4.5f);
            break;
        case ButtonIcon::Cleanup:
            trace({{-7, 4}, {-4, 4}, {-2, 0}, {1, -3}, {6, -3}});
            trace({{2, 3}, {4, 5}, {7, 1}});
            break;
        case ButtonIcon::FadeIn:
        case ButtonIcon::FadeOut: {
            const float direction = icon == ButtonIcon::FadeIn ? 1.0f : -1.0f;
            trace({{-6 * direction, 5}, {-3 * direction, 4.5f}, {0, 1}, {3 * direction, -3.5f}, {6 * direction, -4.5f}});
            line(-6, 6.5f, 6, 6.5f);
            break;
        }
        case ButtonIcon::Echo:
            line(-6.5f, -3, -6.5f, 3);
            trace({{-3.5f, -4.5f}, {-1.5f, 0}, {-3.5f, 4.5f}});
            trace({{0.25f, -5.5f}, {2.5f, 0}, {0.25f, 5.5f}});
            trace({{4.2f, -6.5f}, {6.5f, 0}, {4.2f, 6.5f}});
            break;
        case ButtonIcon::Equalizer:
            for (int band = -1; band <= 1; ++band) {
                const float cx = band * 4.5f;
                const float cy = band * -2.5f;
                line(cx, -6, cx, 6);
                g.rt->FillEllipse(circle(cx, cy, 1.8f, 1.8f), g.brush.Get());
            }
            break;
        case ButtonIcon::Highpass:
            trace({{-7, 5}, {-4.5f, 5}, {-2, 2}, {1, -3}, {3, -4.5f}, {7, -4.5f}});
            break;
        case ButtonIcon::Gate:
            trace({{-7, 5}, {-2.5f, 5}, {-2.5f, -4}, {7, -4}});
            line(1, 2, 6, 2);
            break;
        case ButtonIcon::DeEsser:
            trace({{-7, -3}, {-4, -3}, {-2, 3}, {0, 5}, {2, 3}, {4, -3}, {7, -3}});
            break;
        case ButtonIcon::Compressor:
            trace({{-7, 5}, {-2, 0}, {2, -2.5f}, {7, -4}});
            line(-6, -5.5f, 6, -5.5f);
            break;
        case ButtonIcon::Limiter:
            trace({{-6, 5.5f}, {0, -1.5f}, {6, -1.5f}});
            line(-6, -5, 6, -5);
            break;
        case ButtonIcon::Bits:
            g.rt->DrawRoundedRectangle(rect(-6.5f, -5.5f, 6.5f, 5.5f), g.brush.Get(), stroke);
            line(-3, -2.5f, -3, 2.5f);
            g.rt->DrawRoundedRectangle(rect(0, -2.5f, 3.5f, 2.5f), g.brush.Get(), stroke);
            break;
        case ButtonIcon::Export:
            trace({{-6, 2}, {-6, 6.5f}, {6, 6.5f}, {6, 2}});
            line(0, -6.5f, 0, 2);
            trace({{-3.5f, -1.5f}, {0, 2}, {3.5f, -1.5f}});
            break;
        case ButtonIcon::Scissors:
            g.rt->DrawEllipse(circle(-4.5f, -3.8f, 2.2f, 2.2f), g.brush.Get(), stroke);
            g.rt->DrawEllipse(circle(-4.5f, 3.8f, 2.2f, 2.2f), g.brush.Get(), stroke);
            line(-2.8f, -2.5f, 6, 4.5f);
            line(-2.8f, 2.5f, 6, -4.5f);
            break;
        case ButtonIcon::Globe:
            g.rt->DrawEllipse(circle(0, 0, 6.2f, 6.2f), g.brush.Get(), stroke);
            g.rt->DrawEllipse(circle(0, 0, 2.8f, 6.2f), g.brush.Get(), stroke);
            line(-6, 0, 6, 0);
            break;
        case ButtonIcon::ChevronDown:
            trace({{-3, -1.5f}, {0, 1.5f}, {3, -1.5f}});
            break;
        case ButtonIcon::None:
            break;
    }
}
