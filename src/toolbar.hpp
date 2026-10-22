#pragma once
// Панель инструментов, выпадающие меню и подписи: рисуются cairo в маленькие слои,
// рендерер кладёт их поверх кадра.
#include "canvas.hpp"

#include <array>
#include <string>
#include <vector>

namespace theme {
inline constexpr Color phosphor{0.20f, 1.00f, 0.42f}; // основной зелёный люминофор
inline constexpr Color dim{0.10f, 0.55f, 0.24f};
inline constexpr Color panel{0.012f, 0.035f, 0.02f};
inline constexpr std::array<Color, 8> palette{{
    {1.00f, 0.20f, 0.27f}, // красный
    {0.20f, 1.00f, 0.42f}, // люминофор
    {1.00f, 0.69f, 0.00f}, // янтарь
    {0.00f, 0.90f, 1.00f}, // циан
    {1.00f, 0.95f, 0.15f}, // жёлтый
    {1.00f, 0.30f, 1.00f}, // маджента
    {0.96f, 0.96f, 0.96f}, // белый
    {0.00f, 0.00f, 0.00f}, // чёрный
}};
} // namespace theme

// значки: сначала инструменты (номер = Tool), затем остальное
enum class Icon : int {
    Fill = static_cast<int>(Tool::Count),
    Undo,
    Redo,
    Qr,
    Pin,
    Copy,
    Save,
    Close,
    TextSel = static_cast<int>(Tool::TextSel),
};

// кнопки панели; Pen, Shape, Blur, Style — группы с выпадающим меню
enum class Btn : int { Move, Pen, Arrow, Shape, Text, Counter, Blur, Style, Undo, Redo, TextSel, Qr, Pin, Copy, Save, Close, Count, None = -1 };

// пункты меню
namespace item {
inline constexpr int tool      = 100; // + Tool
inline constexpr int blur_rect = 200, blur_brush = 201, pix_rect = 202, pix_brush = 203;
inline constexpr int color     = 300; // + 0..7
inline constexpr int thinner = 310, thicker = 311, fill = 312;
inline constexpr int copy_sel = 400, copy_all = 401, done = 402;
} // namespace item

struct ToolbarState {
    Tool  tool      = Tool::Move;
    Tool  pen       = Tool::Pen;  // что показывает каждая группа
    Tool  shape     = Tool::Rect;
    Tool  blur      = Tool::Blur;
    bool  brush     = false;      // размытие/пикселизация кистью
    int   color     = 0;
    float width     = 4;
    bool  fill      = false;
    bool  can_undo  = false, can_redo = false;
    int   hover     = -1;
    int   open      = -1;         // группа с открытым меню
};

class Toolbar {
public:
    void init(float ui_scale, std::string font);
    void render(const ToolbarState& st);
    int  hit(float lx, float ly) const; // координаты относительно панели; -1 — мимо кнопок
    bool inside(float lx, float ly) const { return lx >= 0 && ly >= 0 && lx < layer.w && ly < layer.h; }
    // левый край и ширина кнопки — для меню под ней
    float slot_x(int btn) const;
    float slot_w() const { return m_bh; }
    static bool        is_group(int btn);
    static std::string tooltip(int btn, const ToolbarState& st);

    Layer layer;

private:
    struct Slot {
        int   btn;
        float x, w;
    };
    std::array<Slot, static_cast<int>(Btn::Count)> m_slots{};
    int                                            m_nslots = 0;
    float                                          m_s      = 1, m_bh = 40, m_pad = 6;
    std::string                                    m_font;
};

struct MenuItem {
    enum Kind { IconItem, Swatch, TextItem, Value } kind = IconItem;
    int         id     = -1;    // -1 — не нажимается
    int         icon   = 0;
    int         color  = 0;
    std::string text;
    std::string tip;            // подсказка при наведении
    bool        active = false;
    bool        brush  = false; // значок с мазком кисти
};

// горизонтальная плашка из пунктов: выпадающие меню панели и кнопки режима текста
class Menu {
public:
    void build(std::vector<MenuItem> items, float ui_scale, const std::string& font);
    void render(int hover);
    int  hit(float lx, float ly) const;
    bool inside(float lx, float ly) const { return lx >= 0 && ly >= 0 && lx < layer.w && ly < layer.h; }
    const MenuItem* find(int id) const;
    bool            empty() const { return m_items.empty(); }

    Layer layer;

private:
    std::vector<MenuItem> m_items;
    std::vector<float>    m_x, m_w;
    float                 m_s = 1, m_bh = 36, m_pad = 5;
    std::string           m_font;
};

// однострочная подпись на тёмной плашке (размер выделения, подсказки)
class Label {
public:
    void set(const std::string& text, float ui_scale, const std::string& font, float size = 15, bool bright = true);
    Layer       layer;
    std::string text;
};
