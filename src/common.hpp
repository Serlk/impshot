// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <cstdint>

struct Pt {
    float x = 0, y = 0;
};

// целочисленный прямоугольник в пикселях буфера выхода; w/h <= 0 — пустой
struct Rect {
    int x = 0, y = 0, w = 0, h = 0;

    bool empty() const { return w <= 0 || h <= 0; }
    int  right() const { return x + w; }
    int  bottom() const { return y + h; }
    bool contains(float px, float py) const { return px >= x && py >= y && px < x + w && py < y + h; }

    static Rect from_points(Pt a, Pt b) {
        int x0 = static_cast<int>(std::min(a.x, b.x)), y0 = static_cast<int>(std::min(a.y, b.y));
        int x1 = static_cast<int>(std::max(a.x, b.x)), y1 = static_cast<int>(std::max(a.y, b.y));
        return {x0, y0, x1 - x0, y1 - y0};
    }
    Rect united(const Rect& o) const {
        if (empty())
            return o;
        if (o.empty())
            return *this;
        int x0 = std::min(x, o.x), y0 = std::min(y, o.y);
        return {x0, y0, std::max(right(), o.right()) - x0, std::max(bottom(), o.bottom()) - y0};
    }
    Rect clipped(int W, int H) const {
        int x0 = std::clamp(x, 0, W), y0 = std::clamp(y, 0, H);
        int x1 = std::clamp(right(), 0, W), y1 = std::clamp(bottom(), 0, H);
        return {x0, y0, x1 - x0, y1 - y0};
    }
    bool operator==(const Rect&) const = default;
    Rect grown(int d) const { return empty() ? *this : Rect{x - d, y - d, w + 2 * d, h + 2 * d}; }
};

struct Color {
    float r = 0, g = 0, b = 0;
};

// TextSel — режим выделения распознанного текста, не рисует
enum class Tool : uint8_t { Move, Pen, Line, Arrow, Rect, Ellipse, Marker, Text, Counter, Blur, Pixelate, TextSel, Count };
