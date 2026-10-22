#include "renderer.hpp"

#include "canvas.hpp"
#include "capture.hpp"
#include "shaders.hpp"

#include <EGL/eglext.h>
#include <cmath>
#include <cstdio>
#include <wayland-egl.h>

namespace {

GLuint compile(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetShaderInfoLog(s, sizeof log, nullptr, log);
        std::fprintf(stderr, "impshot: shader: %s\n", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

GLuint make_tex(int w, int h, GLenum ifmt, GLenum fmt, const void* data, bool bgra, GLint filter = GL_LINEAR) {
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(ifmt), w, h, 0, fmt, GL_UNSIGNED_BYTE, data);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    if (bgra) {
        // байты в памяти B,G,R,A (XRGB8888 / cairo ARGB32 на little-endian) — меняем каналы при выборке
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_BLUE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_RED);
    }
    return t;
}

GLuint make_fbo(GLuint tex) {
    GLuint f;
    glGenFramebuffers(1, &f);
    glBindFramebuffer(GL_FRAMEBUFFER, f);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    return f;
}

void bind_tex(GLuint unit, GLuint tex) {
    glActiveTexture(GL_TEXTURE0 + unit);
    glBindTexture(GL_TEXTURE_2D, tex);
}

} // namespace

Renderer::~Renderer() { shutdown(); }

void Renderer::shutdown() {
    if (m_dpy == EGL_NO_DISPLAY)
        return;
    eglMakeCurrent(m_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (m_ctx != EGL_NO_CONTEXT)
        eglDestroyContext(m_dpy, m_ctx);
    eglTerminate(m_dpy);
    eglReleaseThread();
    m_ctx = EGL_NO_CONTEXT;
    m_dpy = EGL_NO_DISPLAY;
}

bool Renderer::init(wl_display* dpy) {
    auto get_platform = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
    m_dpy = get_platform ? get_platform(EGL_PLATFORM_WAYLAND_EXT, dpy, nullptr) : eglGetDisplay(dpy);
    if (m_dpy == EGL_NO_DISPLAY || !eglInitialize(m_dpy, nullptr, nullptr) || !eglBindAPI(EGL_OPENGL_ES_API))
        return false;

    const EGLint cfg_attr[] = {EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
                               EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 0, EGL_NONE};
    EGLint n = 0;
    if (!eglChooseConfig(m_dpy, cfg_attr, &m_cfg, 1, &n) || n < 1)
        return false;
    const EGLint ctx_attr[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_NONE};
    m_ctx = eglCreateContext(m_dpy, m_cfg, EGL_NO_CONTEXT, ctx_attr);
    if (m_ctx == EGL_NO_CONTEXT || !eglMakeCurrent(m_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, m_ctx))
        return false;

    if (!build(m_compose, shaders::quad_vs, shaders::compose_fs) || !build(m_sprite, shaders::sprite_vs, shaders::sprite_fs) ||
        !build(m_down, shaders::quad_vs, shaders::down_fs) || !build(m_gauss, shaders::quad_vs, shaders::gauss_fs))
        return false;

    glGenVertexArrays(1, &m_vao);
    const uint8_t zero[4]{};
    m_empty = make_tex(1, 1, GL_RGBA8, GL_RGBA, zero, false);
    return true;
}

bool Renderer::build(Program& p, const char* vert, const char* frag) {
    GLuint vs = compile(GL_VERTEX_SHADER, vert);
    GLuint fs = compile(GL_FRAGMENT_SHADER, frag);
    if (!vs || !fs)
        return false;
    p.id = glCreateProgram();
    glAttachShader(p.id, vs);
    glAttachShader(p.id, fs);
    glLinkProgram(p.id);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(p.id, GL_LINK_STATUS, &ok);
    return ok;
}

bool Renderer::create(Target& t, wl_surface* surface, int w, int h) {
    t.window  = wl_egl_window_create(surface, w, h);
    t.surface = t.window ? eglCreateWindowSurface(m_dpy, m_cfg, reinterpret_cast<EGLNativeWindowType>(t.window), nullptr)
                         : EGL_NO_SURFACE;
    if (t.surface == EGL_NO_SURFACE || !make_current(t))
        return false;
    eglSwapInterval(m_dpy, 0); // темп кадров задаёт wl_surface.frame
    t.w = w;
    t.h = h;
    return true;
}

void Renderer::resize(Target& t, int w, int h) {
    if (t.w == w && t.h == h)
        return;
    wl_egl_window_resize(t.window, w, h, 0, 0);
    t.w = w;
    t.h = h;
}

void Renderer::destroy(Target& t) {
    if (m_dpy != EGL_NO_DISPLAY) {
        eglMakeCurrent(m_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, m_ctx);
        if (t.base)
            glDeleteTextures(1, &t.base);
        if (t.blur)
            glDeleteTextures(1, &t.blur);
        if (t.surface != EGL_NO_SURFACE)
            eglDestroySurface(m_dpy, t.surface);
    }
    if (t.window)
        wl_egl_window_destroy(t.window);
    t = {};
}

void Renderer::set_base(Target& t, const Screenshot& shot) {
    t.iw = shot.width;
    t.ih = shot.height;
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    t.base = make_tex(t.iw, t.ih, GL_RGBA8, GL_RGBA, shot.pixels, shot.bgr);

    // размытая копия на 1/6 разрешения: уменьшение + два прохода гаусса, всё на GPU за доли мс
    int bw = std::max(1, t.iw / 6), bh = std::max(1, t.ih / 6);
    int mw = std::max(1, t.iw / 3), mh = std::max(1, t.ih / 3);
    GLuint mid = make_tex(mw, mh, GL_RGBA8, GL_RGBA, nullptr, false);
    GLuint a   = make_tex(bw, bh, GL_RGBA8, GL_RGBA, nullptr, false);
    t.blur     = make_tex(bw, bh, GL_RGBA8, GL_RGBA, nullptr, false);
    GLuint fmid = make_fbo(mid), fa = make_fbo(a), fb = make_fbo(t.blur);
    glBindVertexArray(m_vao);
    glDisable(GL_BLEND);

    glUseProgram(m_down.id);
    glUniform1i(m_down.loc("uTex"), 0);
    glBindFramebuffer(GL_FRAMEBUFFER, fmid);
    glViewport(0, 0, mw, mh);
    bind_tex(0, t.base);
    glUniform2f(m_down.loc("uTexel"), 1.f / t.iw, 1.f / t.ih);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindFramebuffer(GL_FRAMEBUFFER, fa);
    glViewport(0, 0, bw, bh);
    bind_tex(0, mid);
    glUniform2f(m_down.loc("uTexel"), 0.5f / mw, 0.5f / mh);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    glUseProgram(m_gauss.id);
    glUniform1i(m_gauss.loc("uTex"), 0);
    for (int pass = 0; pass < 2; pass++) {
        glBindFramebuffer(GL_FRAMEBUFFER, fb);
        bind_tex(0, a);
        glUniform2f(m_gauss.loc("uDir"), 1.f / bw, 0);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glBindFramebuffer(GL_FRAMEBUFFER, fa);
        bind_tex(0, t.blur);
        glUniform2f(m_gauss.loc("uDir"), 0, 1.f / bh);
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }
    // результат в a — меняем местами, чтобы t.blur указывал на него
    std::swap(a, t.blur);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    GLuint fbos[] = {fmid, fa, fb};
    glDeleteFramebuffers(3, fbos);
    GLuint texs[] = {mid, a};
    glDeleteTextures(2, texs);
}

void Renderer::sync(Layer& l) {
    if (!l.surf)
        return;
    bool   a8     = l.a8();
    GLenum fmt    = a8 ? GL_RED : GL_RGBA;
    int    bpp    = a8 ? 1 : 4;
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, l.stride() / bpp);
    if (!l.tex || l.tw != l.w || l.th != l.h) {
        if (l.tex) {
            GLuint t = l.tex;
            glDeleteTextures(1, &t);
        }
        l.tex = make_tex(l.w, l.h, a8 ? GL_R8 : GL_RGBA8, fmt, l.data(), !a8);
        l.tw  = l.w;
        l.th  = l.h;
    } else if (!l.dirty.empty()) {
        const Rect& d = l.dirty;
        glBindTexture(GL_TEXTURE_2D, l.tex);
        glTexSubImage2D(GL_TEXTURE_2D, 0, d.x, d.y, d.w, d.h, fmt, GL_UNSIGNED_BYTE, l.data() + d.y * l.stride() + d.x * bpp);
    }
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    l.dirty = {};
}

void Renderer::release(Layer& l) {
    if (!l.tex)
        return;
    GLuint t = l.tex;
    glDeleteTextures(1, &t);
    l.tex = 0;
    l.tw = l.th = 0;
}

void Renderer::compose_uniforms(Target& t, Canvas* c, const FrameParams& f) {
    const Program& p = m_compose;
    glUseProgram(p.id);
    auto tex = [&](Layer* l) { return l && l->tex ? static_cast<GLuint>(l->tex) : m_empty; };
    const char* names[] = {"uBase", "uBlur", "uMaskB", "uMaskP", "uAnno", "uLive"};
    GLuint      texs[]  = {t.base, t.blur, tex(c ? &c->mblur : nullptr), tex(c ? &c->mpix : nullptr),
                           tex(c ? &c->anno : nullptr), tex(c ? &c->live : nullptr)};
    for (int i = 0; i < 6; i++) {
        glUniform1i(p.loc(names[i]), i);
        bind_tex(static_cast<GLuint>(i), texs[i]);
    }
    glUniform2f(p.loc("uSize"), static_cast<float>(t.iw), static_cast<float>(t.ih));
    glUniform1i(p.loc("uLiveMode"), f.live_mode);
    glUniform1f(p.loc("uBlock"), f.block);
    glUniform4f(p.loc("uSel"), f.sel.x, f.sel.y, f.sel.w, f.sel.h);
    glUniform1i(p.loc("uHasSel"), f.has_sel);
    glUniform4f(p.loc("uHover"), f.hover.x, f.hover.y, f.hover.w, f.hover.h);
    glUniform1i(p.loc("uHasHover"), f.has_hover);
    glUniform1i(p.loc("uHandles"), f.handles);
    glUniform2f(p.loc("uCursor"), f.cursor.x, f.cursor.y);
    glUniform1i(p.loc("uCross"), f.cross);
    glUniform3f(p.loc("uLoupe"), f.loupe.x, f.loupe.y, f.loupe_r);
    glUniform1f(p.loc("uZoom"), f.zoom);
    glUniform1f(p.loc("uUi"), f.ui);
    glUniform3f(p.loc("uRing"), f.cursor.x, f.cursor.y, f.ring);
    glUniform1f(p.loc("uScan"), f.scan);
}

void Renderer::draw(Target& t, Canvas* c, const FrameParams& f, const std::vector<Sprite>& sprites) {
    if (c)
        for (Layer* l : {&c->anno, &c->live, &c->mblur, &c->mpix})
            if (!l->dirty.empty())
                sync(*l);
    for (auto& s : sprites)
        if (!s.layer->tex || !s.layer->dirty.empty())
            sync(*s.layer);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, t.w, t.h);
    glBindVertexArray(m_vao);
    glDisable(GL_BLEND);
    compose_uniforms(t, c, f);
    glUniform1i(m_compose.loc("uExport"), 0);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    if (sprites.empty())
        return;
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(m_sprite.id);
    glUniform1i(m_sprite.loc("uTex"), 0);
    glUniform2f(m_sprite.loc("uSize"), static_cast<float>(t.iw), static_cast<float>(t.ih));
    for (auto& s : sprites) {
        bind_tex(0, s.layer->tex);
        glUniform4f(m_sprite.loc("uRect"), s.x, s.y, static_cast<float>(s.layer->w), static_cast<float>(s.layer->h));
        glUniform1f(m_sprite.loc("uAlpha"), s.alpha);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }
    glDisable(GL_BLEND);
}

bool Renderer::readback(Target& t, Canvas* c, const FrameParams& f, Rect sel, float scale, std::vector<uint8_t>& rgba) {
    sel = sel.clipped(t.iw, t.ih);
    if (sel.empty())
        return false;
    if (c)
        for (Layer* l : {&c->anno, &c->live, &c->mblur, &c->mpix})
            if (!l->dirty.empty())
                sync(*l);
    int    ow  = static_cast<int>(std::lround(sel.w * scale)), oh = static_cast<int>(std::lround(sel.h * scale));
    GLuint tex = make_tex(ow, oh, GL_RGBA8, GL_RGBA, nullptr, false);
    GLuint fbo = make_fbo(tex);
    glViewport(0, 0, ow, oh);
    glBindVertexArray(m_vao);
    glDisable(GL_BLEND);
    compose_uniforms(t, c, f);
    glUniform1i(m_compose.loc("uExport"), 1);
    glUniform2f(m_compose.loc("uOffset"), static_cast<float>(sel.x), static_cast<float>(sel.y));
    glUniform1f(m_compose.loc("uExportScale"), scale);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    rgba.resize(static_cast<size_t>(ow) * oh * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, ow, oh, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(1, &tex);
    return glGetError() == GL_NO_ERROR;
}

bool Renderer::make_current(Target& t) { return eglMakeCurrent(m_dpy, t.surface, t.surface, m_ctx); }

bool Renderer::swap(Target& t) { return eglSwapBuffers(m_dpy, t.surface); }
