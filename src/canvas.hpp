#pragma once
// Аннотации: список фигур + растровые слои cairo, которые рендерер грузит в текстуры по damage.
// anno — готовые фигуры, live — фигура под курсором, mblur/mpix — маски размытия и пикселизации.
#include "common.hpp"

#include <cairo.h>
#include <string>
#include <vector>

struct Shape {
    Tool             tool  = Tool::Pen;
    Color            color = {};
    float            width = 4;
    bool             fill  = false;
    std::vector<Pt>  pts;    // Pen/Marker — путь; остальные — pts[0] и pts[1]
    std::string      text;   // Text
    int              number = 0; // Counter
    bool             caret  = false; // Text во время набора
    bool             brush  = false; // Blur/Pixelate: путь кистью вместо прямоугольника

    // ширина кисти размытия от толщины
    static float brush_px(float width) { return width * 5.f + 10.f; }
};

struct Layer {
    cairo_surface_t* surf = nullptr;
    int              w = 0, h = 0;
    Rect             dirty;   // что поменялось с последней загрузки в текстуру
    uint32_t         tex = 0; // GL-текстура, заводит рендерер при первой загрузке
    int              tw = 0, th = 0; // её размер: слой мог смениться

    void     init(cairo_format_t fmt, int width, int height);
    void     release();
    void     damage(const Rect& r) { dirty = dirty.united(r.clipped(w, h)); }
    void     clear(const Rect& r);
    uint8_t* data() const { return cairo_image_surface_get_data(surf); }
    int      stride() const { return cairo_image_surface_get_stride(surf); }
    bool     a8() const { return cairo_image_surface_get_format(surf) == CAIRO_FORMAT_A8; }
};

class Canvas {
public:
    ~Canvas();
    void init(int w, int h, std::string font);

    void clear(); // освободить слои и фигуры до следующего снимка
    void add(Shape s);
    bool undo();
    bool redo();
    void set_live(const Shape* s); // nullptr — убрать

    int  next_counter() const;
    bool has_live() const { return m_live_on; }
    Tool live_tool() const { return m_live.tool; }
    bool empty() const { return m_shapes.empty(); }
    bool can_redo() const { return !m_redo.empty(); }

    // размер шрифта текста и бейджа счётчика от толщины
    static float text_px(float width) { return 14.f + width * 3.f; }

    Layer anno, live, mblur, mpix;

private:
    Rect draw(Layer& l, const Shape& s); // возвращает задетый прямоугольник
    void rebuild();

    std::vector<Shape> m_shapes, m_redo;
    Shape              m_live;
    bool               m_live_on = false;
    Rect               m_live_box;
    std::string        m_font;
};
