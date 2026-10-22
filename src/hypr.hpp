#pragma once
// Прямоугольники видимых окон через сокет Hyprland — для выделения окна одним кликом.
#include <string>
#include <vector>

struct WinRect {
    std::string monitor;                    // имя выхода (как wl_output.name)
    int         x = 0, y = 0, w = 0, h = 0; // логические координаты относительно монитора
    int         layer   = 0;                // 0 тайл, 1 плавающее, 2 закреплённое/полноэкранное
    int         history = 0;                // focusHistoryID: меньше — недавнее, значит выше
    std::string title;
};

// пустой вектор, если не Hyprland или сокет не ответил
std::vector<WinRect> hypr_windows();
