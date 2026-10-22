#include "canvas.hpp"

#include <cmath>
#include <pango/pangocairo.h>

void Layer::init(cairo_format_t fmt, int width, int height) {
    release();
    w     = width;
    h     = height;
    surf  = cairo_image_surface_create(fmt, w, h); // calloc: нулевые страницы, почти бесплатно
    dirty = {};
}

void Layer::release() {
    if (surf)
        cairo_surface_destroy(surf);
    surf = nullptr;
}

void Layer::clear(const Rect& r) {
    Rect c = r.clipped(w, h);
    if (c.empty())
        return;
    cairo_surface_flush(surf);
    int      bpp = a8() ? 1 : 4;
    uint8_t* p   = data();
    for (int y = c.y; y < c.bottom(); y++)
        std::fill_n(p + y * stride() + c.x * bpp, c.w * bpp, uint8_t{0});
    cairo_surface_mark_dirty_rectangle(surf, c.x, c.y, c.w, c.h);
    damage(c);
}

Canvas::~Canvas() { clear(); }

void Canvas::clear() {
    for (Layer* l : {&anno, &live, &mblur, &mpix})
        l->release();
    m_shapes.clear();
    m_redo.clear();
    m_live_on  = false;
    m_live_box = {};
}

void Canvas::init(int w, int h, std::string font) {
    m_font = std::move(font);
    anno.init(CAIRO_FORMAT_ARGB32, w, h);
    live.init(CAIRO_FORMAT_ARGB32, w, h);
    mblur.init(CAIRO_FORMAT_A8, w, h);
    mpix.init(CAIRO_FORMAT_A8, w, h);
}

int Canvas::next_counter() const {
    int n = 0;
    for (auto& s : m_shapes)
        if (s.tool == Tool::Counter)
            n = std::max(n, s.number);
    return n + 1;
}

void Canvas::add(Shape s) {
    if (s.tool == Tool::Text && s.text.empty())
        return;
    s.caret = false;
    Layer& l = s.tool == Tool::Blur ? mblur : s.tool == Tool::Pixelate ? mpix : anno;
    draw(l, s);
    m_shapes.push_back(std::move(s));
    m_redo.clear();
}

bool Canvas::undo() {
    if (m_shapes.empty())
        return false;
    m_redo.push_back(std::move(m_shapes.back()));
    m_shapes.pop_back();
    rebuild();
    return true;
}

bool Canvas::redo() {
    if (m_redo.empty())
        return false;
    Shape s = std::move(m_redo.back());
    m_redo.pop_back();
    auto keep = std::move(m_redo);
    add(std::move(s));
    m_redo = std::move(keep);
    return true;
}

void Canvas::rebuild() {
    for (Layer* l : {&anno, &mblur, &mpix})
        l->clear({0, 0, l->w, l->h});
    for (auto& s : m_shapes)
        draw(s.tool == Tool::Blur ? mblur : s.tool == Tool::Pixelate ? mpix : anno, s);
}

void Canvas::set_live(const Shape* s) {
    live.clear(m_live_box);
    m_live_box = {};
    m_live_on  = s != nullptr;
    if (!s)
        return;
    m_live     = *s;
    m_live_box = draw(live, m_live);
}

// --- рисование -----------------------------------------------------------------------

namespace {

void path_box(Rect& box, Pt p) { box = box.united({static_cast<int>(std::floor(p.x)), static_cast<int>(std::floor(p.y)), 1, 1}); }

void arrow(cairo_t* cr, Pt a, Pt b, float w) {
    float dx = b.x - a.x, dy = b.y - a.y, len = std::hypot(dx, dy);
    if (len < 1)
        return;
    float ux = dx / len, uy = dy / len;
    float head = std::min(len, std::max(14.f, w * 4.5f));
    float half = head * 0.5f;
    Pt    base{b.x - ux * head * 0.85f, b.y - uy * head * 0.85f};
    cairo_move_to(cr, a.x, a.y);
    cairo_line_to(cr, base.x, base.y);
    cairo_stroke(cr);
    cairo_move_to(cr, b.x, b.y);
    cairo_line_to(cr, b.x - ux * head - uy * half, b.y - uy * head + ux * half);
    cairo_line_to(cr, b.x - ux * head + uy * half, b.y - uy * head - ux * half);
    cairo_close_path(cr);
    cairo_fill(cr);
}

float luma(Color c) { return 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b; }

} // namespace

Rect Canvas::draw(Layer& l, const Shape& s) {
    if (s.pts.empty())
        return {};
    cairo_t* cr = cairo_create(l.surf);
    cairo_set_antialias(cr, CAIRO_ANTIALIAS_GOOD);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_source_rgb(cr, s.color.r, s.color.g, s.color.b);
    cairo_set_line_width(cr, s.width);

    Pt   a = s.pts.front(), b = s.pts.back();
    Rect box;
    for (auto& p : s.pts)
        path_box(box, p);
    int margin = static_cast<int>(std::ceil(s.width)) + 2;

    switch (s.tool) {
        case Tool::Pen:
        case Tool::Marker: {
            if (s.tool == Tool::Marker) {
                // маркер: широкая полупрозрачная полоса, вся линия — один путь, без наложений
                cairo_set_source_rgba(cr, s.color.r, s.color.g, s.color.b, 0.38);
                cairo_set_line_width(cr, s.width * 4 + 8);
                cairo_set_line_cap(cr, CAIRO_LINE_CAP_SQUARE);
                margin = static_cast<int>(s.width * 4 + 8);
            }
            cairo_move_to(cr, a.x, a.y);
            if (s.pts.size() == 1)
                cairo_line_to(cr, a.x + 0.01, a.y);
            for (size_t i = 1; i < s.pts.size(); i++)
                cairo_line_to(cr, s.pts[i].x, s.pts[i].y);
            cairo_stroke(cr);
            break;
        }
        case Tool::Line:
            cairo_move_to(cr, a.x, a.y);
            cairo_line_to(cr, b.x, b.y);
            cairo_stroke(cr);
            break;
        case Tool::Arrow:
            arrow(cr, a, b, s.width);
            margin = static_cast<int>(std::max(14.f, s.width * 4.5f));
            break;
        case Tool::Rect: {
            Rect r = Rect::from_points(a, b);
            cairo_rectangle(cr, r.x, r.y, r.w, r.h);
            s.fill ? cairo_fill(cr) : cairo_stroke(cr);
            break;
        }
        case Tool::Ellipse: {
            Rect r = Rect::from_points(a, b);
            if (r.w >= 1 && r.h >= 1) {
                cairo_save(cr);
                cairo_translate(cr, r.x + r.w / 2.0, r.y + r.h / 2.0);
                cairo_scale(cr, r.w / 2.0, r.h / 2.0);
                cairo_arc(cr, 0, 0, 1, 0, 2 * M_PI);
                cairo_restore(cr);
                s.fill ? cairo_fill(cr) : cairo_stroke(cr);
            }
            break;
        }
        case Tool::Blur:
        case Tool::Pixelate: {
            if (s.brush) {
                float bw = Shape::brush_px(s.width);
                cairo_set_source_rgba(cr, 1, 1, 1, 1);
                cairo_set_line_width(cr, bw);
                cairo_move_to(cr, a.x, a.y);
                if (s.pts.size() == 1)
                    cairo_line_to(cr, a.x + 0.01, a.y);
                for (size_t i = 1; i < s.pts.size(); i++)
                    cairo_line_to(cr, s.pts[i].x, s.pts[i].y);
                cairo_stroke(cr);
                margin = static_cast<int>(bw / 2) + 2;
                break;
            }
            Rect r = Rect::from_points(a, b);
            cairo_set_source_rgba(cr, 1, 1, 1, 1);
            cairo_rectangle(cr, r.x, r.y, r.w, r.h);
            cairo_fill(cr);
            margin = 1;
            break;
        }
        case Tool::Counter: {
            float rad = text_px(s.width) * 0.75f;
            cairo_arc(cr, a.x, a.y, rad, 0, 2 * M_PI);
            cairo_fill_preserve(cr);
            bool dark = luma(s.color) > 0.5f;
            cairo_set_source_rgba(cr, dark ? 0 : 1, dark ? 0 : 1, dark ? 0 : 1, 0.55);
            cairo_set_line_width(cr, std::max(1.5f, rad * 0.08f));
            cairo_stroke(cr);

            PangoLayout* lay = pango_cairo_create_layout(cr);
            auto*        fd  = pango_font_description_from_string(m_font.c_str());
            pango_font_description_set_weight(fd, PANGO_WEIGHT_BOLD);
            pango_font_description_set_absolute_size(fd, rad * 1.15 * PANGO_SCALE);
            pango_layout_set_font_description(lay, fd);
            pango_font_description_free(fd);
            auto num = std::to_string(s.number);
            pango_layout_set_text(lay, num.c_str(), -1);
            PangoRectangle ink, logical;
            pango_layout_get_pixel_extents(lay, &ink, &logical);
            cairo_set_source_rgb(cr, dark ? 0 : 1, dark ? 0 : 1, dark ? 0 : 1);
            cairo_move_to(cr, a.x - ink.x - ink.width / 2.0, a.y - ink.y - ink.height / 2.0);
            pango_cairo_show_layout(cr, lay);
            g_object_unref(lay);
            margin = static_cast<int>(rad) + 4;
            break;
        }
        case Tool::Text: {
            PangoLayout* lay = pango_cairo_create_layout(cr);
            auto*        fd  = pango_font_description_from_string(m_font.c_str());
            pango_font_description_set_weight(fd, PANGO_WEIGHT_SEMIBOLD);
            pango_font_description_set_absolute_size(fd, text_px(s.width) * PANGO_SCALE);
            pango_layout_set_font_description(lay, fd);
            pango_font_description_free(fd);
            std::string t = s.text;
            if (s.caret)
                t += "█";
            pango_layout_set_text(lay, t.empty() ? " " : t.c_str(), -1);
            PangoRectangle ink, logical;
            pango_layout_get_pixel_extents(lay, &ink, &logical);
            float pad = text_px(s.width) * 0.25f;
            Rect  r{static_cast<int>(a.x), static_cast<int>(a.y), logical.width, logical.height};
            if (s.fill) {
                // подложка: тёмная плашка, текст читается на любом фоне
                cairo_set_source_rgba(cr, 0.02, 0.05, 0.03, 0.82);
                cairo_rectangle(cr, r.x - pad, r.y - pad, r.w + 2 * pad, r.h + 2 * pad);
                cairo_fill(cr);
                cairo_set_source_rgb(cr, s.color.r, s.color.g, s.color.b);
            } else {
                // тонкая обводка контрастным цветом
                bool dark = luma(s.color) > 0.5f;
                cairo_move_to(cr, r.x, r.y);
                pango_cairo_layout_path(cr, lay);
                cairo_set_source_rgba(cr, dark ? 0 : 1, dark ? 0 : 1, dark ? 0 : 1, 0.7);
                cairo_set_line_width(cr, std::max(2.f, text_px(s.width) * 0.12f));
                cairo_stroke(cr);
                cairo_set_source_rgb(cr, s.color.r, s.color.g, s.color.b);
            }
            cairo_move_to(cr, r.x, r.y);
            pango_cairo_show_layout(cr, lay);
            g_object_unref(lay);
            box    = r.united(Rect{r.x, r.y, ink.x + ink.width, ink.y + ink.height});
            margin = static_cast<int>(pad) + 4;
            break;
        }
        default: break;
    }
    cairo_destroy(cr);
    cairo_surface_flush(l.surf);
    box = box.grown(margin);
    l.damage(box);
    return box;
}
