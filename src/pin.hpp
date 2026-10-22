#pragma once
// Закреплённый снимок: обычное xdg-окно поверх остальных (правило Hyprland: float + pin).
// Картинка — shm-буфер в пикселях снимка, масштаб делает композитор через viewport.
#include <cstdint>
#include <string>
#include <vector>

struct wl_compositor;
struct wl_shm;
struct wl_seat;
struct wl_surface;
struct wl_buffer;
struct xdg_wm_base;
struct xdg_surface;
struct xdg_toplevel;
struct wp_viewporter;
struct wp_viewport;
struct wp_alpha_modifier_v1;
struct wp_alpha_modifier_surface_v1;

struct PinGlobals {
    wl_compositor*        compositor = nullptr;
    wl_shm*               shm        = nullptr;
    xdg_wm_base*          wm_base    = nullptr;
    wp_viewporter*        viewporter = nullptr;
    wp_alpha_modifier_v1* alpha      = nullptr;
};

class Pin {
public:
    // fac — пикселей снимка на логический пиксель; x, y — где показать (логические, от угла монитора)
    Pin(const PinGlobals& g, std::vector<uint8_t> rgba, int w, int h, float fac, int id, int x, int y);
    ~Pin();
    Pin(const Pin&)            = delete;
    Pin& operator=(const Pin&) = delete;

    wl_surface* surface() const { return m_surface; }
    void        zoom(int steps);       // колесо: ±10% за шаг
    void        reset_zoom();
    void        fade(int steps);       // Ctrl+колесо: прозрачность
    void        drag(wl_seat* seat, uint32_t serial);
    void        on_configure(uint32_t serial);

    const std::vector<uint8_t>& rgba() const { return m_rgba; }
    int                         width() const { return m_w; }
    int                         height() const { return m_h; }
    bool                        closed = false;

private:
    void apply();

    PinGlobals                    m_g;
    std::vector<uint8_t>          m_rgba;
    int                           m_w, m_h, m_bw, m_bh; // картинка и буфер с рамкой
    float                         m_fac, m_zoom = 1, m_alpha = 1;
    wl_surface*                   m_surface  = nullptr;
    xdg_surface*                  m_xsurface = nullptr;
    xdg_toplevel*                 m_toplevel = nullptr;
    wp_viewport*                  m_viewport = nullptr;
    wp_alpha_modifier_surface_v1* m_alpha_s  = nullptr;
    wl_buffer*                    m_buffer   = nullptr;
    bool                          m_mapped   = false;
};
