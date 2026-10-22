#include "toolbar.hpp"

#include <cmath>
#include <pango/pangocairo.h>

namespace {

void rgb(cairo_t* cr, Color c, double a = 1) { cairo_set_source_rgba(cr, c.r, c.g, c.b, a); }

void rounded(cairo_t* cr, double x, double y, double w, double h, double r) {
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2);
    cairo_arc(cr, x + r, y + h - r, r, M_PI / 2, M_PI);
    cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);
    cairo_close_path(cr);
}

PangoLayout* text_layout(cairo_t* cr, const std::string& font, double px, const std::string& text, bool bold) {
    PangoLayout* lay = pango_cairo_create_layout(cr);
    auto*        fd  = pango_font_description_from_string(font.c_str());
    pango_font_description_set_absolute_size(fd, px * PANGO_SCALE);
    if (bold)
        pango_font_description_set_weight(fd, PANGO_WEIGHT_BOLD);
    pango_layout_set_font_description(lay, fd);
    pango_font_description_free(fd);
    pango_layout_set_text(lay, text.c_str(), -1);
    return lay;
}

// текст с фосфорным ореолом: несколько полупрозрачных смещённых копий под основной
void glow_text(cairo_t* cr, PangoLayout* lay, double x, double y, Color c, double glow) {
    if (glow > 0) {
        rgb(cr, c, glow);
        for (auto [dx, dy] : {std::pair{-1, 0}, {1, 0}, {0, -1}, {0, 1}}) {
            cairo_move_to(cr, x + dx, y + dy);
            pango_cairo_show_layout(cr, lay);
        }
    }
    rgb(cr, c);
    cairo_move_to(cr, x, y);
    pango_cairo_show_layout(cr, lay);
}

// иконки в квадрате 24×24 вокруг (0,0)-(24,24); толщина линии задаётся снаружи
void icon(cairo_t* cr, int btn, const std::string& font, bool brush) {
    auto line = [&](double x0, double y0, double x1, double y1) {
        cairo_move_to(cr, x0, y0);
        cairo_line_to(cr, x1, y1);
    };
    auto glyph = [&](const char* t, double px, bool bold) {
        PangoLayout*   lay = text_layout(cr, font, px, t, bold);
        PangoRectangle ink;
        pango_layout_get_pixel_extents(lay, &ink, nullptr);
        cairo_move_to(cr, 12 - ink.x - ink.width / 2.0, 12 - ink.y - ink.height / 2.0);
        pango_cairo_show_layout(cr, lay);
        g_object_unref(lay);
    };
    cairo_new_path(cr); // текст значка оставляет текущую точку — иначе к следующему потянется линия
    switch (btn) {
        case static_cast<int>(Tool::Move):
            line(12, 3, 12, 21);
            line(3, 12, 21, 12);
            cairo_stroke(cr);
            for (auto [x, y, dx, dy] : {std::tuple{12, 3, 0, 1}, {12, 21, 0, -1}, {3, 12, 1, 0}, {21, 12, -1, 0}}) {
                cairo_move_to(cr, x, y);
                cairo_line_to(cr, x + dx * 4 - dy * 3, y + dy * 4 - dx * 3);
                cairo_line_to(cr, x + dx * 4 + dy * 3, y + dy * 4 + dx * 3);
                cairo_close_path(cr);
                cairo_fill(cr);
            }
            break;
        case static_cast<int>(Tool::Pen):
            cairo_move_to(cr, 3, 17);
            cairo_curve_to(cr, 6, 7, 9, 7, 11, 13);
            cairo_curve_to(cr, 13, 19, 17, 19, 21, 6);
            cairo_stroke(cr);
            break;
        case static_cast<int>(Tool::Line):
            line(4, 20, 20, 4);
            cairo_stroke(cr);
            break;
        case static_cast<int>(Tool::Arrow):
            line(4, 20, 16, 8);
            cairo_stroke(cr);
            cairo_move_to(cr, 21, 3);
            cairo_line_to(cr, 19, 13);
            cairo_line_to(cr, 11, 5);
            cairo_close_path(cr);
            cairo_fill(cr);
            break;
        case static_cast<int>(Tool::Rect):
            cairo_rectangle(cr, 4, 6, 16, 12);
            cairo_stroke(cr);
            break;
        case static_cast<int>(Tool::Ellipse):
            cairo_save(cr);
            cairo_translate(cr, 12, 12);
            cairo_scale(cr, 9, 7);
            cairo_arc(cr, 0, 0, 1, 0, 2 * M_PI);
            cairo_restore(cr);
            cairo_stroke(cr);
            break;
        case static_cast<int>(Tool::Marker):
            cairo_save(cr);
            cairo_set_line_width(cr, 7);
            cairo_set_line_cap(cr, CAIRO_LINE_CAP_SQUARE);
            cairo_push_group(cr);
            line(5, 15, 19, 9);
            cairo_stroke(cr);
            cairo_pop_group_to_source(cr);
            cairo_paint_with_alpha(cr, 0.45);
            cairo_restore(cr);
            line(4, 20, 20, 20);
            cairo_stroke(cr);
            break;
        case static_cast<int>(Tool::Text): glyph("T", 22, true); break;
        case static_cast<int>(Tool::Counter):
            cairo_arc(cr, 12, 12, 9, 0, 2 * M_PI);
            cairo_stroke(cr);
            glyph("1", 12, true);
            break;
        case static_cast<int>(Tool::Blur):
            for (int i = 0; i < 4; i++) {
                cairo_save(cr);
                cairo_push_group(cr);
                cairo_arc(cr, 12, 12, 3 + i * 2.5, 0, 2 * M_PI);
                cairo_fill(cr);
                cairo_pop_group_to_source(cr);
                cairo_paint_with_alpha(cr, 0.28);
                cairo_restore(cr);
            }
            break;
        case static_cast<int>(Tool::Pixelate):
            for (int y = 0; y < 4; y++)
                for (int x = 0; x < 4; x++)
                    if ((x + y) % 2 == 0)
                        cairo_rectangle(cr, 4 + x * 4, 4 + y * 4, 4, 4);
            cairo_fill(cr);
            cairo_rectangle(cr, 4, 4, 16, 16);
            cairo_stroke(cr);
            break;
        case static_cast<int>(Icon::TextSel):
            // строки текста и курсор-«палочка»: выделить кусок текста
            line(3, 6, 14, 6);
            line(3, 11, 11, 11);
            line(3, 16, 13, 16);
            cairo_stroke(cr);
            line(18, 4, 18, 20);
            line(16, 4, 20, 4);
            line(16, 20, 20, 20);
            cairo_stroke(cr);
            break;
        case static_cast<int>(Icon::Qr):
            for (auto [x, y] : {std::pair{3, 3}, {14, 3}, {3, 14}}) {
                cairo_rectangle(cr, x + 0.5, y + 0.5, 6, 6);
                cairo_stroke(cr);
                cairo_rectangle(cr, x + 2.5, y + 2.5, 2, 2);
                cairo_fill(cr);
            }
            for (auto [x, y] : {std::pair{14, 14}, {18, 14}, {16, 17}, {14, 19}, {19, 19}})
                cairo_rectangle(cr, x, y, 2.5, 2.5);
            cairo_fill(cr);
            break;
        case static_cast<int>(Icon::Pin):
            // канцелярская кнопка
            cairo_move_to(cr, 8, 3);
            cairo_line_to(cr, 16, 3);
            cairo_move_to(cr, 9.5, 3);
            cairo_line_to(cr, 9.5, 10);
            cairo_line_to(cr, 6, 14);
            cairo_line_to(cr, 18, 14);
            cairo_line_to(cr, 14.5, 10);
            cairo_line_to(cr, 14.5, 3);
            cairo_move_to(cr, 12, 14);
            cairo_line_to(cr, 12, 21);
            cairo_stroke(cr);
            break;
        case static_cast<int>(Icon::Fill):
            cairo_rectangle(cr, 4, 4, 11, 11);
            cairo_stroke(cr);
            cairo_rectangle(cr, 9, 9, 11, 11);
            cairo_fill(cr);
            break;
        case static_cast<int>(Icon::Undo):
        case static_cast<int>(Icon::Redo):
            cairo_save(cr);
            if (btn == static_cast<int>(Icon::Redo)) {
                cairo_translate(cr, 24, 0);
                cairo_scale(cr, -1, 1);
            }
            cairo_arc(cr, 13, 14, 7, -M_PI * 0.95, M_PI * 0.5);
            cairo_stroke(cr);
            cairo_move_to(cr, 2, 13);
            cairo_line_to(cr, 10, 13);
            cairo_line_to(cr, 5, 6);
            cairo_close_path(cr);
            cairo_fill(cr);
            cairo_restore(cr);
            break;
        case static_cast<int>(Icon::Copy):
            cairo_rectangle(cr, 4, 4, 11, 13);
            cairo_stroke(cr);
            cairo_rectangle(cr, 9, 8, 11, 13);
            cairo_stroke(cr);
            break;
        case static_cast<int>(Icon::Save):
            cairo_move_to(cr, 4, 4);
            cairo_line_to(cr, 17, 4);
            cairo_line_to(cr, 20, 7);
            cairo_line_to(cr, 20, 20);
            cairo_line_to(cr, 4, 20);
            cairo_close_path(cr);
            cairo_stroke(cr);
            cairo_rectangle(cr, 8, 4, 8, 5);
            cairo_fill(cr);
            cairo_rectangle(cr, 8, 13, 8, 7);
            cairo_stroke(cr);
            break;
        case static_cast<int>(Icon::Close):
            line(5, 5, 19, 19);
            line(19, 5, 5, 19);
            cairo_stroke(cr);
            break;
        default: break;
    }
    if (brush && (btn == static_cast<int>(Tool::Blur) || btn == static_cast<int>(Tool::Pixelate))) {
        // режим кисти: мазок в углу поверх значка
        cairo_save(cr);
        cairo_set_line_width(cr, 3.2);
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
        cairo_move_to(cr, 14, 23);
        cairo_curve_to(cr, 17, 18, 20, 24, 24, 17);
        cairo_stroke(cr);
        cairo_restore(cr);
    }
}

} // namespace
namespace {

// фон кнопки: подсветка при наведении, заливка у активной
void button_bg(cairo_t* cr, double x, double y, double w, double h, double s, bool active, bool hover) {
    if (!active && !hover)
        return;
    rounded(cr, x + 2 * s, y + 2 * s, w - 4 * s, h - 4 * s, 3 * s);
    rgb(cr, theme::phosphor, active ? 0.20 : 0.09);
    cairo_fill_preserve(cr);
    rgb(cr, theme::phosphor, active ? 0.85 : 0.35);
    cairo_set_line_width(cr, 1 * s);
    cairo_stroke(cr);
}

// значок по центру кнопки, с ореолом люминофора у активной/наведённой
void draw_icon(cairo_t* cr, int id, double x, double y, double w, double h, const std::string& font, bool lit,
               bool disabled, bool brush, Color c) {
    cairo_save(cr);
    double isz = 24, s = (h * 0.55) / isz;
    cairo_translate(cr, x + (w - isz * s) / 2, y + (h - isz * s) / 2);
    cairo_scale(cr, s, s);
    cairo_set_line_width(cr, 1.9);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    if (!disabled && lit) {
        cairo_push_group(cr);
        rgb(cr, c);
        cairo_set_line_width(cr, 4.5);
        icon(cr, id, font, brush);
        cairo_pop_group_to_source(cr);
        cairo_paint_with_alpha(cr, 0.22);
        cairo_set_line_width(cr, 1.9);
    }
    rgb(cr, c, disabled ? 0.3 : 1);
    icon(cr, id, font, brush);
    cairo_restore(cr);
}

// уголок «есть ещё варианты» у кнопки-группы
void group_mark(cairo_t* cr, double x, double y, double w, double h, double s, bool open) {
    double r = x + w - 5 * s, b = y + h - 5 * s, k = 5 * s;
    cairo_move_to(cr, r, b - k);
    cairo_line_to(cr, r, b);
    cairo_line_to(cr, r - k, b);
    cairo_close_path(cr);
    rgb(cr, theme::phosphor, open ? 1 : 0.55);
    cairo_fill(cr);
}

void clear(cairo_t* cr) {
    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
}

void frame(cairo_t* cr, double W, double H, double s) {
    rounded(cr, 0.5, 0.5, W - 1, H - 1, 4 * s);
    rgb(cr, theme::panel, 0.94);
    cairo_fill_preserve(cr);
    rgb(cr, theme::phosphor, 0.75);
    cairo_set_line_width(cr, 1.2 * s);
    cairo_stroke(cr);
}

int group_icon(int btn, const ToolbarState& st) {
    switch (static_cast<Btn>(btn)) {
        case Btn::Move: return static_cast<int>(Tool::Move);
        case Btn::Pen: return static_cast<int>(st.pen);
        case Btn::Arrow: return static_cast<int>(Tool::Arrow);
        case Btn::Shape: return static_cast<int>(st.shape);
        case Btn::Text: return static_cast<int>(Tool::Text);
        case Btn::Counter: return static_cast<int>(Tool::Counter);
        case Btn::Blur: return static_cast<int>(st.blur);
        case Btn::Undo: return static_cast<int>(Icon::Undo);
        case Btn::Redo: return static_cast<int>(Icon::Redo);
        case Btn::TextSel: return static_cast<int>(Icon::TextSel);
        case Btn::Qr: return static_cast<int>(Icon::Qr);
        case Btn::Pin: return static_cast<int>(Icon::Pin);
        case Btn::Copy: return static_cast<int>(Icon::Copy);
        case Btn::Save: return static_cast<int>(Icon::Save);
        case Btn::Close: return static_cast<int>(Icon::Close);
        default: return -1;
    }
}

bool btn_active(int btn, const ToolbarState& st) {
    Tool t = st.tool;
    switch (static_cast<Btn>(btn)) {
        case Btn::Move: return t == Tool::Move;
        case Btn::Pen: return t == Tool::Pen || t == Tool::Marker;
        case Btn::Arrow: return t == Tool::Arrow;
        case Btn::Shape: return t == Tool::Rect || t == Tool::Ellipse || t == Tool::Line;
        case Btn::Text: return t == Tool::Text;
        case Btn::Counter: return t == Tool::Counter;
        case Btn::Blur: return t == Tool::Blur || t == Tool::Pixelate;
        case Btn::TextSel: return t == Tool::TextSel;
        default: return false;
    }
}

} // namespace

bool Toolbar::is_group(int btn) {
    return btn == static_cast<int>(Btn::Pen) || btn == static_cast<int>(Btn::Shape) || btn == static_cast<int>(Btn::Blur) ||
        btn == static_cast<int>(Btn::Style);
}

void Toolbar::init(float ui_scale, std::string font) {
    m_s    = ui_scale;
    m_font = std::move(font);
    m_bh   = std::round(40 * m_s);
    m_pad  = std::round(6 * m_s);

    // рисование | стиль | отмена | текст, QR, закрепить | итог
    float x   = m_pad;
    float gap = std::round(10 * m_s);
    for (int b = 0; b < static_cast<int>(Btn::Count); b++) {
        if (b == static_cast<int>(Btn::Style) || b == static_cast<int>(Btn::Undo) || b == static_cast<int>(Btn::TextSel) ||
            b == static_cast<int>(Btn::Copy))
            x += gap;
        m_slots[m_nslots++] = {b, x, m_bh};
        x += m_bh;
    }
    x += m_pad;
    layer.init(CAIRO_FORMAT_ARGB32, static_cast<int>(x), static_cast<int>(m_bh + 2 * m_pad));
}

int Toolbar::hit(float lx, float ly) const {
    if (ly < m_pad || ly >= m_pad + m_bh)
        return -1;
    for (int i = 0; i < m_nslots; i++)
        if (lx >= m_slots[i].x && lx < m_slots[i].x + m_slots[i].w)
            return m_slots[i].btn;
    return -1;
}

float Toolbar::slot_x(int btn) const {
    for (int i = 0; i < m_nslots; i++)
        if (m_slots[i].btn == btn)
            return m_slots[i].x;
    return 0;
}

void Toolbar::render(const ToolbarState& st) {
    cairo_t* cr = cairo_create(layer.surf);
    clear(cr);
    frame(cr, layer.w, layer.h, m_s);

    for (int i = 0; i < m_nslots; i++) {
        auto [btn, x, w] = m_slots[i];
        double y         = m_pad;
        bool   active    = btn_active(btn, st);
        bool   hover     = btn == st.hover || btn == st.open;
        bool   disabled  = (btn == static_cast<int>(Btn::Undo) && !st.can_undo) ||
            (btn == static_cast<int>(Btn::Redo) && !st.can_redo);
        button_bg(cr, x, y, w, m_bh, m_s, active, hover);

        if (btn == static_cast<int>(Btn::Style)) {
            // стиль: квадрат текущего цвета и точка толщины на нём
            Color  c  = theme::palette[st.color];
            double sz = m_bh * 0.5, cx = x + w / 2, cy = y + m_bh / 2;
            rounded(cr, cx - sz / 2, cy - sz / 2, sz, sz, 3 * m_s);
            rgb(cr, c);
            cairo_fill_preserve(cr);
            rgb(cr, theme::phosphor, hover ? 1 : 0.6);
            cairo_set_line_width(cr, 1.2 * m_s);
            cairo_stroke(cr);
            double rad   = std::clamp(st.width * 0.45, 1.2, sz * 0.38) * m_s;
            bool   light = c.r * 0.3 + c.g * 0.6 + c.b * 0.1 > 0.5;
            cairo_arc(cr, cx, cy, rad, 0, 2 * M_PI);
            cairo_set_source_rgb(cr, light ? 0 : 1, light ? 0 : 1, light ? 0 : 1);
            cairo_fill(cr);
            if (st.fill) {
                // заливка включена: маленький залитый уголок
                cairo_move_to(cr, cx + sz / 2, cy + sz / 2 - 6 * m_s);
                cairo_line_to(cr, cx + sz / 2, cy + sz / 2);
                cairo_line_to(cr, cx + sz / 2 - 6 * m_s, cy + sz / 2);
                cairo_close_path(cr);
                rgb(cr, theme::phosphor);
                cairo_fill(cr);
            }
            group_mark(cr, x, y, w, m_bh, m_s, btn == st.open);
            continue;
        }

        Color c = active || hover ? theme::phosphor : theme::dim;
        if (btn == static_cast<int>(Btn::Close) && hover)
            c = theme::palette[0];
        bool brush = btn == static_cast<int>(Btn::Blur) && st.brush;
        draw_icon(cr, group_icon(btn, st), x, y, w, m_bh, m_font, active || hover, disabled, brush, c);
        if (is_group(btn))
            group_mark(cr, x, y, w, m_bh, m_s, btn == st.open);
    }

    // разделители групп
    rgb(cr, theme::dim, 0.5);
    cairo_set_line_width(cr, 1);
    for (int i = 1; i < m_nslots; i++) {
        double prev_end = m_slots[i - 1].x + m_slots[i - 1].w;
        if (m_slots[i].x - prev_end > 6 * m_s) {
            double sx = std::round((prev_end + m_slots[i].x) / 2) + 0.5;
            cairo_move_to(cr, sx, m_pad + m_bh * 0.2);
            cairo_line_to(cr, sx, m_pad + m_bh * 0.8);
            cairo_stroke(cr);
        }
    }
    cairo_destroy(cr);
    cairo_surface_flush(layer.surf);
    layer.damage({0, 0, layer.w, layer.h});
}

std::string Toolbar::tooltip(int btn, const ToolbarState& st) {
    switch (static_cast<Btn>(btn)) {
        case Btn::Move: return "SELECT / MOVE   V";
        case Btn::Pen: return st.pen == Tool::Marker ? "MARKER   M  ·  ▾ PEN" : "PEN   P  ·  ▾ MARKER";
        case Btn::Arrow: return "ARROW   A";
        case Btn::Shape:
            return st.shape == Tool::Ellipse ? "ELLIPSE   E  ·  ▾ SHAPES"
                : st.shape == Tool::Line     ? "LINE   L  ·  ▾ SHAPES"
                                             : "RECTANGLE   R  ·  ▾ SHAPES";
        case Btn::Text: return "TEXT   T";
        case Btn::Counter: return "COUNTER   N";
        case Btn::Blur: return st.blur == Tool::Pixelate ? "PIXELATE   X  ·  ▾ OPTIONS" : "BLUR   B  ·  ▾ OPTIONS";
        case Btn::Style: return "COLOR, WIDTH, FILL";
        case Btn::Undo: return "UNDO   CTRL+Z";
        case Btn::Redo: return "REDO   CTRL+SHIFT+Z";
        case Btn::TextSel: return "SELECT TEXT IN SCREENSHOT";
        case Btn::Qr: return "READ QR CODE   Q";
        case Btn::Pin: return "PIN ON TOP   CTRL+P";
        case Btn::Copy: return "COPY   ENTER / CTRL+C";
        case Btn::Save: return "SAVE   CTRL+S";
        case Btn::Close: return "QUIT   ESC";
        default: return {};
    }
}

// --- меню ------------------------------------------------------------------------------

void Menu::build(std::vector<MenuItem> items, float s, const std::string& font) {
    m_items = std::move(items);
    m_s     = s;
    m_font  = font;
    m_bh    = std::round(36 * s);
    m_pad   = std::round(5 * s);
    m_x.clear();
    m_w.clear();

    cairo_surface_t* tmp = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
    cairo_t*         tcr = cairo_create(tmp);
    float            x   = m_pad;
    for (auto& it : m_items) {
        float w = m_bh;
        if (it.kind == MenuItem::Swatch)
            w = std::round(m_bh * 0.75f);
        else if (it.kind == MenuItem::TextItem || it.kind == MenuItem::Value) {
            PangoLayout*   lay = text_layout(tcr, m_font, 13 * s, it.text, true);
            PangoRectangle lg;
            pango_layout_get_pixel_extents(lay, nullptr, &lg);
            g_object_unref(lay);
            w = std::max(static_cast<float>(lg.width) + 22 * s, it.kind == MenuItem::Value ? m_bh * 0.8f : m_bh);
        }
        m_x.push_back(x);
        m_w.push_back(w);
        x += w;
    }
    cairo_destroy(tcr);
    cairo_surface_destroy(tmp);
    layer.init(CAIRO_FORMAT_ARGB32, static_cast<int>(x + m_pad), static_cast<int>(m_bh + 2 * m_pad));
}

const MenuItem* Menu::find(int id) const {
    for (auto& it : m_items)
        if (it.id == id)
            return &it;
    return nullptr;
}

int Menu::hit(float lx, float ly) const {
    if (ly < m_pad || ly >= m_pad + m_bh)
        return -1;
    for (size_t i = 0; i < m_items.size(); i++)
        if (lx >= m_x[i] && lx < m_x[i] + m_w[i])
            return m_items[i].id;
    return -1;
}

void Menu::render(int hover) {
    cairo_t* cr = cairo_create(layer.surf);
    clear(cr);
    frame(cr, layer.w, layer.h, m_s);
    for (size_t i = 0; i < m_items.size(); i++) {
        const MenuItem& it = m_items[i];
        double          x = m_x[i], y = m_pad, w = m_w[i];
        bool            hov = it.id >= 0 && it.id == hover;
        switch (it.kind) {
            case MenuItem::IconItem:
                button_bg(cr, x, y, w, m_bh, m_s, it.active, hov);
                draw_icon(cr, it.icon, x, y, w, m_bh, m_font, it.active || hov, false, it.brush,
                          it.active || hov ? theme::phosphor : theme::dim);
                break;
            case MenuItem::Swatch: {
                double sz = w - 10 * m_s, cx = x + w / 2, cy = y + m_bh / 2;
                if (it.active || hov) {
                    rounded(cr, cx - sz / 2 - 3 * m_s, cy - sz / 2 - 3 * m_s, sz + 6 * m_s, sz + 6 * m_s, 3 * m_s);
                    rgb(cr, theme::phosphor, it.active ? 0.9 : 0.45);
                    cairo_set_line_width(cr, 1.6 * m_s);
                    cairo_stroke(cr);
                }
                rounded(cr, cx - sz / 2, cy - sz / 2, sz, sz, 2 * m_s);
                rgb(cr, theme::palette[it.color]);
                cairo_fill_preserve(cr);
                rgb(cr, theme::dim, 0.6);
                cairo_set_line_width(cr, 1);
                cairo_stroke(cr);
                break;
            }
            case MenuItem::TextItem:
            case MenuItem::Value: {
                if (it.kind == MenuItem::TextItem)
                    button_bg(cr, x, y, w, m_bh, m_s, it.active, hov);
                PangoLayout*   lay = text_layout(cr, m_font, 13 * m_s, it.text, true);
                PangoRectangle ink;
                pango_layout_get_pixel_extents(lay, &ink, nullptr);
                Color c = it.kind == MenuItem::Value || hov || it.active ? theme::phosphor : theme::dim;
                glow_text(cr, lay, x + (w - ink.width) / 2 - ink.x, y + (m_bh - ink.height) / 2 - ink.y, c, 0.14);
                g_object_unref(lay);
                break;
            }
        }
    }
    cairo_destroy(cr);
    cairo_surface_flush(layer.surf);
    layer.damage({0, 0, layer.w, layer.h});
}

void Label::set(const std::string& t, float s, const std::string& font, float size, bool bright) {
    if (t == text && layer.surf)
        return;
    text = t;
    // размер слоя считаем на временном контексте
    cairo_surface_t* tmp  = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
    cairo_t*         tcr  = cairo_create(tmp);
    PangoLayout*     tlay = text_layout(tcr, font, size * s, t, true);
    PangoRectangle   logical;
    pango_layout_get_pixel_extents(tlay, nullptr, &logical);
    g_object_unref(tlay);
    cairo_destroy(tcr);
    cairo_surface_destroy(tmp);

    double px = 9 * s, py = 4 * s;
    int    W = static_cast<int>(logical.width + 2 * px), H = static_cast<int>(logical.height + 2 * py);
    if (!layer.surf || layer.w != W || layer.h != H)
        layer.init(CAIRO_FORMAT_ARGB32, W, H);

    cairo_t* cr = cairo_create(layer.surf);
    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    rounded(cr, 0.5, 0.5, W - 1, H - 1, 3 * s);
    rgb(cr, theme::panel, 0.92);
    cairo_fill_preserve(cr);
    rgb(cr, theme::phosphor, bright ? 0.7 : 0.35);
    cairo_set_line_width(cr, 1);
    cairo_stroke(cr);
    PangoLayout* lay = text_layout(cr, font, size * s, t, true);
    glow_text(cr, lay, px - logical.x, py - logical.y, bright ? theme::phosphor : theme::dim, 0.14);
    g_object_unref(lay);
    cairo_destroy(cr);
    cairo_surface_flush(layer.surf);
    layer.damage({0, 0, W, H});
}
