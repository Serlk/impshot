// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// EGL/GLES3: на выход — снимок, его размытая копия и слои аннотаций; один проход compose.
#include "common.hpp"

#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <vector>

struct wl_display;
struct wl_surface;
struct wl_egl_window;
struct Screenshot;
struct Layer;
class Canvas;

struct Target {
    wl_egl_window* window  = nullptr;
    EGLSurface     surface = EGL_NO_SURFACE;
    int            w = 0, h = 0; // буфер окна
    int            iw = 0, ih = 0; // снимок
    GLuint         base = 0, blur = 0;
};

struct Sprite {
    Layer* layer;
    float  x, y;
    float  alpha = 1;
};

struct FrameParams {
    bool  has_sel = false, handles = false, has_hover = false, cross = false;
    Rect  sel, hover;
    Pt    cursor;
    Pt    loupe;          // центр лупы
    float loupe_r = 0;    // 0 — без лупы
    float zoom    = 8;
    float ui      = 1;
    int   live_mode = 0;  // 0 обычная, 1 размытие, 2 пикселизация
    float block     = 12;
    float ring      = 0;  // радиус контура кисти у курсора
    float scan      = -1; // y полосы сканера при распознавании; < 0 — нет
};

class Renderer {
public:
    ~Renderer();
    bool init(wl_display* dpy);
    void shutdown(); // до wl_display_disconnect

    bool create(Target& t, wl_surface* surface, int w, int h);
    void resize(Target& t, int w, int h);
    void destroy(Target& t);
    void set_base(Target& t, const Screenshot& shot);

    void sync(Layer& l); // загрузить изменившуюся часть слоя в текстуру
    void release(Layer& l);
    void draw(Target& t, Canvas* c, const FrameParams& f, const std::vector<Sprite>& sprites);
    // содержимое выделения без интерфейса, RGBA сверху вниз
    // c == nullptr — чистый снимок без аннотаций; scale > 1 — увеличенный (OCR)
    bool readback(Target& t, Canvas* c, const FrameParams& f, Rect sel, float scale, std::vector<uint8_t>& rgba);
    bool make_current(Target& t);
    bool swap(Target& t);

private:
    struct Program {
        GLuint id = 0;
        GLint  loc(const char* name) const { return glGetUniformLocation(id, name); }
    };
    bool build(Program& p, const char* vert, const char* frag);
    void compose_uniforms(Target& t, Canvas* c, const FrameParams& f);

    EGLDisplay m_dpy = EGL_NO_DISPLAY;
    EGLContext m_ctx = EGL_NO_CONTEXT;
    EGLConfig  m_cfg = nullptr;
    GLuint     m_vao = 0, m_empty = 0;
    Program    m_compose, m_sprite, m_down, m_gauss;
};
