#include "app.hpp"

#include "cursor-shape-v1.h"
#include "alpha-modifier-v1.h"
#include "export.hpp"
#include "recognize.hpp"
#include "xdg-shell.h"
#include "ext-image-capture-source-v1.h"
#include "ext-image-copy-capture-v1.h"
#include "viewporter.h"
// в заголовке layer-shell параметр назван namespace — в C++ это ключевое слово
#define namespace namespace_
#include "wlr-layer-shell-unstable-v1.h"
#undef namespace

#include <cmath>
#include <cstdio>
#include <cstring>
#include <linux/input-event-codes.h>
#include <malloc.h>
#include <fcntl.h>
#include <poll.h>
#include <csignal>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <sys/un.h>
#include <unistd.h>
#include <wayland-client.h>

namespace {

// --- слушатели: тонкие переходники к методам App -------------------------------------

const wl_registry_listener registry_listener = {
    .global = [](void* d, wl_registry* r, uint32_t name, const char* iface, uint32_t ver) {
        static_cast<App*>(d)->on_global(r, name, iface, ver);
    },
    .global_remove = [](void* d, wl_registry*, uint32_t name) { static_cast<App*>(d)->on_global_remove(name); },
};

const wl_output_listener output_listener = {
    .geometry = [](void*, wl_output*, int32_t, int32_t, int32_t, int32_t, int32_t, const char*, const char*, int32_t) {},
    .mode     = [](void*, wl_output*, uint32_t, int32_t, int32_t, int32_t) {},
    .done     = [](void*, wl_output*) {},
    .scale    = [](void* d, wl_output*, int32_t s) { static_cast<Output*>(d)->scale = std::clamp(s, 1, 8); },
    .name     = [](void* d, wl_output*, const char* n) { static_cast<Output*>(d)->name = n ? n : ""; },
    .description = [](void*, wl_output*, const char*) {},
};

const wl_seat_listener seat_listener = {
    .capabilities = [](void* d, wl_seat*, uint32_t caps) { static_cast<App*>(d)->on_seat_caps(caps); },
    .name         = [](void*, wl_seat*, const char*) {},
};

const wl_keyboard_listener keyboard_listener = {
    .keymap = [](void* d, wl_keyboard*, uint32_t fmt, int fd, uint32_t size) { static_cast<App*>(d)->on_keymap(fmt, fd, size); },
    .enter  = [](void* d, wl_keyboard*, uint32_t, wl_surface* s, wl_array*) { static_cast<App*>(d)->on_keyboard_enter(s); },
    .leave  = [](void* d, wl_keyboard*, uint32_t, wl_surface*) { static_cast<App*>(d)->on_key(0, 0); },
    .key = [](void* d, wl_keyboard*, uint32_t, uint32_t, uint32_t key, uint32_t st) { static_cast<App*>(d)->on_key(key, st); },
    .modifiers = [](void* d, wl_keyboard*, uint32_t, uint32_t dep, uint32_t lat, uint32_t lock, uint32_t grp) {
        static_cast<App*>(d)->on_modifiers(dep, lat, lock, grp);
    },
    .repeat_info = [](void* d, wl_keyboard*, int32_t rate, int32_t delay) { static_cast<App*>(d)->on_repeat_info(rate, delay); },
};

const wl_pointer_listener pointer_listener = {
    .enter = [](void* d, wl_pointer*, uint32_t serial, wl_surface* s, wl_fixed_t x, wl_fixed_t y) {
        static_cast<App*>(d)->on_pointer_enter(serial, s, wl_fixed_to_double(x), wl_fixed_to_double(y));
    },
    .leave  = [](void* d, wl_pointer*, uint32_t, wl_surface*) { static_cast<App*>(d)->on_pointer_leave(); },
    .motion = [](void* d, wl_pointer*, uint32_t, wl_fixed_t x, wl_fixed_t y) {
        static_cast<App*>(d)->on_motion(wl_fixed_to_double(x), wl_fixed_to_double(y));
    },
    .button = [](void* d, wl_pointer*, uint32_t serial, uint32_t, uint32_t b, uint32_t st) {
        static_cast<App*>(d)->on_button(serial, b, st);
    },
    .axis = [](void* d, wl_pointer*, uint32_t, uint32_t axis, wl_fixed_t v) {
        if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL)
            static_cast<App*>(d)->on_axis(wl_fixed_to_double(v));
    },
    .frame          = [](void* d, wl_pointer*) { static_cast<App*>(d)->on_pointer_frame(); },
    .axis_source    = [](void*, wl_pointer*, uint32_t) {},
    .axis_stop      = [](void*, wl_pointer*, uint32_t, uint32_t) {},
    .axis_discrete  = [](void*, wl_pointer*, uint32_t, int32_t) {},
    .axis_value120 = [](void* d, wl_pointer*, uint32_t axis, int32_t v) {
        if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL)
            static_cast<App*>(d)->on_axis120(v);
    },
    .axis_relative_direction = [](void*, wl_pointer*, uint32_t, uint32_t) {},
    .warp                    = [](void*, wl_pointer*, wl_fixed_t, wl_fixed_t) {},
};

const zwlr_layer_surface_v1_listener layer_listener = {
    .configure = [](void* d, zwlr_layer_surface_v1* s, uint32_t serial, uint32_t w, uint32_t h) {
        zwlr_layer_surface_v1_ack_configure(s, serial);
        auto* o = static_cast<Output*>(d);
        o->app->on_configure(*o, static_cast<int>(w), static_cast<int>(h));
    },
    .closed = [](void* d, zwlr_layer_surface_v1*) { static_cast<Output*>(d)->app->on_layer_closed(); },
};

const xdg_wm_base_listener wm_base_listener = {
    .ping = [](void*, xdg_wm_base* b, uint32_t serial) { xdg_wm_base_pong(b, serial); },
};

const wl_callback_listener frame_listener = {
    .done = [](void* d, wl_callback* cb, uint32_t) {
        auto* o = static_cast<Output*>(d);
        wl_callback_destroy(cb);
        o->frame_cb = nullptr;
        o->app->on_frame_done(*o);
    },
};

constexpr float MIN_W = 1, MAX_W = 40;

// стрелки и Backspace повторяются при удержании; остальное — нет
bool repeatable(xkb_keysym_t s) {
    return s == XKB_KEY_BackSpace || s == XKB_KEY_Left || s == XKB_KEY_Right || s == XKB_KEY_Up || s == XKB_KEY_Down;
}

} // namespace

// ======================================================================================

App::App(Options o) : m_opt(std::move(o)) {
    m_t0    = std::chrono::steady_clock::now();
    m_width = std::clamp(m_opt.width, MIN_W, MAX_W);
    m_color = std::clamp(m_opt.color, 0, 7);
    if (m_opt.save_dir.empty()) {
        const char* home = getenv("HOME");
        m_opt.save_dir   = std::string(home ? home : ".") + "/Pictures/Screenshots";
    }
}

App::~App() {
    for (auto& o : m_outputs)
        destroy_surface(*o);
    m_outputs.clear();
    if (m_repeat_fd >= 0)
        close(m_repeat_fd);
    m_renderer.shutdown();
    if (m_dpy)
        wl_display_disconnect(m_dpy);
}

double App::ms() const { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - m_t0).count(); }

void App::quit(int code) {
    m_exit    = code;
    m_running = false;
}

// --- Wayland ---------------------------------------------------------------------------

void App::on_global(wl_registry* reg, uint32_t name, const char* iface, uint32_t ver) {
    auto bind = [&](const wl_interface* i, uint32_t want) { return wl_registry_bind(reg, name, i, std::min(ver, want)); };
    std::string_view s = iface;
    if (s == wl_compositor_interface.name)
        m_compositor = static_cast<wl_compositor*>(bind(&wl_compositor_interface, 4));
    else if (s == wl_shm_interface.name)
        m_shm = static_cast<wl_shm*>(bind(&wl_shm_interface, 1));
    else if (s == wl_seat_interface.name && !m_seat) {
        m_seat = static_cast<wl_seat*>(bind(&wl_seat_interface, 9));
        wl_seat_add_listener(m_seat, &seat_listener, this);
    } else if (s == zwlr_layer_shell_v1_interface.name)
        m_layer_shell = static_cast<zwlr_layer_shell_v1*>(bind(&zwlr_layer_shell_v1_interface, 4));
    else if (s == ext_output_image_capture_source_manager_v1_interface.name)
        m_capture.sources = static_cast<ext_output_image_capture_source_manager_v1*>(
            bind(&ext_output_image_capture_source_manager_v1_interface, 1));
    else if (s == ext_image_copy_capture_manager_v1_interface.name)
        m_capture.copy = static_cast<ext_image_copy_capture_manager_v1*>(bind(&ext_image_copy_capture_manager_v1_interface, 1));
    else if (s == wp_viewporter_interface.name)
        m_viewporter = static_cast<wp_viewporter*>(bind(&wp_viewporter_interface, 1));
    else if (s == xdg_wm_base_interface.name) {
        m_wm_base = static_cast<xdg_wm_base*>(bind(&xdg_wm_base_interface, 2));
        xdg_wm_base_add_listener(m_wm_base, &wm_base_listener, this);
    } else if (s == wp_alpha_modifier_v1_interface.name)
        m_alpha_mgr = static_cast<wp_alpha_modifier_v1*>(bind(&wp_alpha_modifier_v1_interface, 1));
    else if (s == wp_cursor_shape_manager_v1_interface.name)
        m_cursor_mgr = static_cast<wp_cursor_shape_manager_v1*>(bind(&wp_cursor_shape_manager_v1_interface, 1));
    else if (s == wl_output_interface.name) {
        auto o    = std::make_unique<Output>();
        o->app    = this;
        o->global = name;
        o->output = static_cast<wl_output*>(bind(&wl_output_interface, 4));
        wl_output_add_listener(o->output, &output_listener, o.get());
        m_outputs.push_back(std::move(o));
    }
}

void App::on_global_remove(uint32_t name) {
    auto it = std::find_if(m_outputs.begin(), m_outputs.end(), [&](auto& o) { return o->global == name; });
    if (it == m_outputs.end())
        return;
    // монитор отключили посреди работы — проще всего выйти, чем чинить висящие ссылки
    Output* o = it->get();
    if (o == m_sel_out || o == m_ptr_out)
        quit(1);
    destroy_surface(*o);
    wl_output_release(o->output);
    m_outputs.erase(it);
}

void App::on_seat_caps(uint32_t caps) {
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !m_keyboard) {
        m_keyboard = wl_seat_get_keyboard(m_seat);
        wl_keyboard_add_listener(m_keyboard, &keyboard_listener, this);
    }
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !m_pointer) {
        m_pointer = wl_seat_get_pointer(m_seat);
        wl_pointer_add_listener(m_pointer, &pointer_listener, this);
        if (m_cursor_mgr)
            m_cursor_dev = wp_cursor_shape_manager_v1_get_pointer(m_cursor_mgr, m_pointer);
    }
}

bool App::setup_wayland() {
    m_dpy = wl_display_connect(nullptr);
    if (!m_dpy) {
        std::fprintf(stderr, "impshot: cannot connect to Wayland\n");
        return false;
    }
    m_registry = wl_display_get_registry(m_dpy);
    wl_registry_add_listener(m_registry, &registry_listener, this);
    wl_display_roundtrip(m_dpy); // глобалы
    wl_display_roundtrip(m_dpy); // имена выходов, возможности seat
    if (!m_compositor || !m_layer_shell || m_outputs.empty()) {
        std::fprintf(stderr, "impshot: compositor lacks wlr-layer-shell\n");
        return false;
    }
    if (!m_capture.sources || !m_capture.copy) {
        std::fprintf(stderr, "impshot: compositor lacks ext-image-copy-capture\n");
        return false;
    }
    m_capture.display = m_dpy;
    m_capture.shm     = m_shm;
    return true;
}

void App::create_surface(Output& o) {
    o.surface = wl_compositor_create_surface(m_compositor);
    if (m_viewporter)
        o.viewport = wp_viewporter_get_viewport(m_viewporter, o.surface);
    o.layer = zwlr_layer_shell_v1_get_layer_surface(m_layer_shell, o.surface, o.output, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
                                                    "impshot");
    zwlr_layer_surface_v1_add_listener(o.layer, &layer_listener, &o);
    zwlr_layer_surface_v1_set_anchor(o.layer, ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
                                                  ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
    zwlr_layer_surface_v1_set_exclusive_zone(o.layer, -1);
    zwlr_layer_surface_v1_set_keyboard_interactivity(o.layer, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE);
    wl_surface_commit(o.surface);
}

void App::destroy_surface(Output& o) {
    if (o.target.window)
        m_renderer.destroy(o.target);
    if (o.frame_cb)
        wl_callback_destroy(o.frame_cb);
    if (o.layer)
        zwlr_layer_surface_v1_destroy(o.layer);
    if (o.viewport)
        wp_viewport_destroy(o.viewport);
    if (o.surface)
        wl_surface_destroy(o.surface);
    o.frame_cb   = nullptr;
    o.layer      = nullptr;
    o.viewport   = nullptr;
    o.surface    = nullptr;
    o.configured = false;
}

void App::on_configure(Output& o, int w, int h) {
    if (w <= 0 || h <= 0 || o.shot.empty())
        return;
    o.lw = w;
    o.lh = h;
    // буфер — ровно снимок, на логический размер его растягивает viewport: пиксель в пиксель
    int bw = o.shot.width, bh = o.shot.height;
    if (o.viewport)
        wp_viewport_set_destination(o.viewport, w, h);
    else
        wl_surface_set_buffer_scale(o.surface, o.scale);
    o.fac = static_cast<float>(bw) / static_cast<float>(w);
    if (!o.target.window) {
        if (!m_renderer.create(o.target, o.surface, bw, bh)) {
            std::fprintf(stderr, "impshot: cannot create EGL surface\n");
            quit(1);
            return;
        }
        m_renderer.set_base(o.target, o.shot);
        o.shot.free_pixels();
        o.canvas.init(bw, bh, m_opt.font);
        if (m_opt.timing)
            std::fprintf(stderr, "impshot: %s configured+uploaded %.1f ms\n", o.name.c_str(), ms());
    }
    o.configured = true;
    if (!m_toolbar.layer.surf) {
        m_toolbar.init(ui(&o), m_opt.font);
        refresh_toolbar();
        m_help.set("DRAG TO SELECT  ·  CLICK — WINDOW  ·  CTRL+A — FULL SCREEN  ·  ESC — QUIT", ui(&o), m_opt.font, 14, false);
    }
    if (m_opt.demo > 0 && !m_has_sel)
        setup_demo();
    redraw(&o);
}

void App::on_layer_closed() { quit(1); }

void App::on_frame_done(Output& o) {
    // пока идёт распознавание, по выделению бежит полоса сканера
    if (o.dirty || (m_ocr_pending && m_tool == Tool::TextSel && &o == m_sel_out))
        render(o);
}

void App::redraw(Output* o) {
    if (!o || !o->configured)
        return;
    o->dirty = true;
    if (!o->frame_cb)
        render(*o);
}

void App::redraw_all() {
    for (auto& o : m_outputs)
        redraw(o.get());
}

void App::render(Output& o) {
    if (!o.configured || !o.target.window)
        return;
    o.dirty = false;
    m_renderer.make_current(o.target);
    // выделение сдвинули — распознанные слова к нему больше не относятся
    if (m_tool == Tool::TextSel && &o == m_sel_out && !(m_sel == m_words_sel))
        exit_textsel();

    FrameParams f;
    float       u = ui(&o);
    f.ui          = u;
    f.block       = std::round(10 * u);
    bool on_sel   = m_has_sel && &o == m_sel_out;
    bool on_ptr   = &o == m_ptr_out;
    f.has_sel     = on_sel;
    f.sel         = m_sel;
    f.handles     = on_sel && m_drag != Drag::Select;
    f.has_hover   = !m_has_sel && on_ptr && m_has_hover;
    f.hover       = m_hover;
    f.cross       = !m_has_sel && on_ptr && m_drag == Drag::None;
    f.cursor      = m_ptr;
    if (o.canvas.has_live()) {
        Tool t      = o.canvas.live_tool();
        f.live_mode = t == Tool::Blur ? 1 : t == Tool::Pixelate ? 2 : 0;
    }
    // контур кисти, чтобы видеть, что попадёт под размытие
    if (on_sel && on_ptr && m_brush && (m_tool == Tool::Blur || m_tool == Tool::Pixelate) &&
        (m_drag == Drag::None || m_drag == Drag::Draw))
        f.ring = Shape::brush_px(m_width) / 2;
    if (on_sel && m_ocr_pending && m_tool == Tool::TextSel) {
        double t = std::fmod((ms() - m_scan_t0) / 1100.0, 2.0);
        f.scan   = static_cast<float>(m_sel.y + m_sel.h * (t < 1 ? t : 2 - t));
    }
    bool loupe = on_ptr && (!m_has_sel || m_drag == Drag::Select || m_drag == Drag::Resize) && m_drag != Drag::Move;
    if (loupe) {
        float r   = std::round(64 * u), off = std::round(28 * u) + r;
        f.loupe_r = r;
        f.loupe   = {m_ptr.x + off, m_ptr.y + off};
        if (f.loupe.x + r > o.shot.width)
            f.loupe.x = m_ptr.x - off;
        if (f.loupe.y + r + 30 * u > o.shot.height)
            f.loupe.y = m_ptr.y - off;
        f.zoom = std::round(8 * u);
    }

    std::vector<Sprite> sprites;
    layout_ui(o, sprites);
    if (loupe) {
        char buf[48];
        std::snprintf(buf, sizeof buf, "%d, %d", static_cast<int>(m_ptr.x), static_cast<int>(m_ptr.y));
        m_coords.set(buf, u, m_opt.font, 12);
        sprites.push_back({&m_coords.layer, std::round(f.loupe.x - m_coords.layer.w / 2.f),
                           std::round(f.loupe.y + f.loupe_r + 8 * u)});
    }

    o.frame_cb = wl_surface_frame(o.surface);
    wl_callback_add_listener(o.frame_cb, &frame_listener, &o);
    m_renderer.draw(o.target, &o.canvas, f, sprites);
    m_renderer.swap(o.target);
}

// размещение подписей и панели: под выделением, если не влезает — над ним, иначе внутри
void App::layout_ui(Output& o, std::vector<Sprite>& out) {
    float u      = ui(&o);
    bool  on_sel = m_has_sel && &o == m_sel_out;
    int   W = o.shot.width, H = o.shot.height;

    if (!m_has_sel && m_drag != Drag::Select && m_help.layer.surf)
        out.push_back({&m_help.layer, std::round((W - m_help.layer.w) / 2.f), std::round(18 * u), 1});

    if (!on_sel)
        return;
    // подсветка распознанных слов и плашка «копировать» — в режиме текста
    if (m_tool == Tool::TextSel) {
        if (m_words_layer.surf && !m_words.empty())
            out.push_back({&m_words_layer, static_cast<float>(m_words_sel.x), static_cast<float>(m_words_sel.y)});
        if (m_chip.layer.surf) {
            float cw = m_chip.layer.w, ch = m_chip.layer.h;
            float cy = m_sel.y - ch - 6 * u;
            if (cy < 0)
                cy = m_sel.y + 6 * u;
            m_chip_x = std::round(std::clamp(m_sel.right() - cw, 0.f, std::max(0.f, W - cw)));
            m_chip_y = std::round(cy);
            out.push_back({&m_chip.layer, m_chip_x, m_chip_y});
        }
    }
    char buf[48];
    std::snprintf(buf, sizeof buf, "%d × %d", m_sel.w, m_sel.h);
    m_size_label.set(buf, u, m_opt.font, 13);
    float lx = m_sel.x, ly = m_sel.y - m_size_label.layer.h - 6 * u;
    if (ly < 0)
        ly = m_sel.y + 6 * u, lx = m_sel.x + 6 * u;
    out.push_back({&m_size_label.layer, std::round(std::clamp(lx, 0.f, float(W - m_size_label.layer.w))), std::round(ly)});

    m_tb_visible = m_drag != Drag::Select && m_drag != Drag::Move && m_drag != Drag::Resize;
    if (!m_tb_visible)
        return;
    float tw = m_toolbar.layer.w, th = m_toolbar.layer.h, gap = 10 * u;
    float tx = m_sel.x + (m_sel.w - tw) / 2;
    float ty = m_sel.bottom() + gap;
    if (ty + th > H) {
        ty = m_sel.y - gap - th;
        if (ty < 0)
            ty = m_sel.bottom() - gap - th;
    }
    m_tb_x = std::round(std::clamp(tx, 0.f, std::max(0.f, W - tw)));
    m_tb_y = std::round(std::clamp(ty, 0.f, std::max(0.f, H - th)));
    out.push_back({&m_toolbar.layer, m_tb_x, m_tb_y});
    bool tb_below = m_tb_y >= m_sel.bottom();

    // выпадающее меню: с той стороны панели, что дальше от выделения
    if (m_open >= 0 && m_menu.layer.surf) {
        float mw = m_menu.layer.w, mh = m_menu.layer.h;
        float mx = m_tb_x + m_toolbar.slot_x(m_open) + m_toolbar.slot_w() / 2 - mw / 2;
        float my = tb_below ? m_tb_y + th + 4 * u : m_tb_y - mh - 4 * u;
        if (my + mh > H || my < 0)
            my = tb_below ? m_tb_y - mh - 4 * u : m_tb_y + th + 4 * u;
        m_menu_x = std::round(std::clamp(mx, 0.f, std::max(0.f, W - mw)));
        m_menu_y = std::round(std::clamp(my, 0.f, std::max(0.f, H - mh)));
        out.push_back({&m_menu.layer, m_menu_x, m_menu_y});
    }

    // подсказка: к пункту меню или к кнопке панели
    std::string tip;
    float       anchor_y = m_tb_y, anchor_h = th;
    if (m_open >= 0 && m_menu_hover >= 0) {
        if (auto* it = m_menu.find(m_menu_hover))
            tip = it->tip;
        anchor_y = m_menu_y;
        anchor_h = static_cast<float>(m_menu.layer.h);
    } else if (m_tb_hover >= 0 && m_open < 0)
        tip = Toolbar::tooltip(m_tb_hover, toolbar_state());
    if (!tip.empty()) {
        m_tip.set(tip, u, m_opt.font, 12);
        float tipx = std::clamp(m_ptr.x - m_tip.layer.w / 2.f, 0.f, float(W - m_tip.layer.w));
        float tipy = tb_below ? anchor_y + anchor_h + 6 * u : anchor_y - m_tip.layer.h - 6 * u;
        if (tipy + m_tip.layer.h > H || tipy < 0)
            tipy = tb_below ? anchor_y - m_tip.layer.h - 6 * u : anchor_y + anchor_h + 6 * u;
        out.push_back({&m_tip.layer, std::round(tipx), std::round(tipy)});
    }
}

// --- указатель -------------------------------------------------------------------------

Pin* App::pin_of(wl_surface* s) {
    for (auto& p : m_pins)
        if (p->surface() == s)
            return p.get();
    return nullptr;
}

void App::on_keyboard_enter(wl_surface* s) { m_kbd_surface = s; }

void App::on_pointer_enter(uint32_t serial, wl_surface* s, double x, double y) {
    m_enter_serial = serial;
    m_cursor_shape = 0;
    m_ptr_pin      = pin_of(s);
    if (m_ptr_pin) {
        if (m_cursor_dev)
            wp_cursor_shape_device_v1_set_shape(m_cursor_dev, serial, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_GRAB);
        return;
    }
    Output* prev   = m_ptr_out;
    m_ptr_out      = nullptr;
    for (auto& o : m_outputs)
        if (o->surface == s)
            m_ptr_out = o.get();
    if (prev && prev != m_ptr_out)
        redraw(prev);
    on_motion(x, y);
}

void App::on_pointer_leave() {
    m_ptr_pin    = nullptr;
    Output* prev = m_ptr_out;
    m_ptr_out    = nullptr;
    redraw(prev);
}

void App::on_motion(double x, double y) {
    if (!m_ptr_out)
        return;
    float f = m_ptr_out->fac;
    m_ptr   = {static_cast<float>(x) * f, static_cast<float>(y) * f};
    motion(m_ptr);
}

void App::on_button(uint32_t serial, uint32_t button, uint32_t state) {
    bool down = state == WL_POINTER_BUTTON_STATE_PRESSED;
    if (m_ptr_pin && down) {
        // закрепка: тащить ЛКМ, закрыть двойным кликом или колёсиком
        static double last = -1;
        double now = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
        bool   dbl = button == BTN_LEFT && now - last < 350;
        last       = now;
        if (dbl || button == BTN_MIDDLE)
            m_ptr_pin->closed = true;
        else if (button == BTN_LEFT)
            m_ptr_pin->drag(m_seat, serial);
        return;
    }
    if (!m_ptr_out)
        return;
    if (button == BTN_LEFT)
        down ? press(m_ptr) : release(m_ptr);
    else if (button == BTN_RIGHT && down)
        right_press();
    else if (button == BTN_MIDDLE && down && m_has_sel)
        finish(Finish::Copy);
}

void App::on_axis(double value) {
    m_axis_acc += value;
    m_axis_seen = true;
}

void App::on_axis120(int value) {
    m_v120_acc += value;
    m_v120_seen = true;
}

// колесо: шаг на щелчок (value120), тачпад — накопление непрерывной прокрутки
void App::on_pointer_frame() {
    int steps = 0;
    if (m_v120_seen) {
        steps = m_v120_acc / 120;
        m_v120_acc -= steps * 120;
        m_axis_acc = 0;
    } else if (m_axis_seen) {
        steps = static_cast<int>(m_axis_acc / 12);
        m_axis_acc -= steps * 12;
    }
    m_v120_seen = m_axis_seen = false;
    if (steps)
        scroll(steps);
}

void App::scroll(int steps) {
    if (m_ptr_pin) {
        m_kbd.ctrl() ? m_ptr_pin->fade(-steps) : m_ptr_pin->zoom(-steps);
        return;
    }
    m_width = std::clamp(m_width - static_cast<float>(steps), MIN_W, MAX_W);
    if (m_editing) {
        m_shape.width = m_width;
        m_sel_out->canvas.set_live(&m_shape);
    }
    refresh_toolbar();
    redraw(m_sel_out ? m_sel_out : m_ptr_out);
}

int App::handle_at(Pt p) const {
    if (!m_has_sel || m_ptr_out != m_sel_out || m_tool == Tool::TextSel)
        return -1;
    float hs = 10 * ui(m_sel_out);
    for (int i = 0; i < 9; i++) {
        if (i == 4)
            continue;
        float hx = m_sel.x + m_sel.w * (i % 3) * 0.5f, hy = m_sel.y + m_sel.h * (i / 3) * 0.5f;
        if (std::abs(p.x - hx) <= hs && std::abs(p.y - hy) <= hs)
            return i;
    }
    if (m_tool != Tool::Move)
        return -1;
    // в режиме выделения тянуть можно и за край целиком
    float e = 6 * ui(m_sel_out);
    bool  inx = p.x > m_sel.x - e && p.x < m_sel.right() + e, iny = p.y > m_sel.y - e && p.y < m_sel.bottom() + e;
    if (iny && std::abs(p.x - m_sel.x) <= e)
        return 3;
    if (iny && std::abs(p.x - m_sel.right()) <= e)
        return 5;
    if (inx && std::abs(p.y - m_sel.y) <= e)
        return 1;
    if (inx && std::abs(p.y - m_sel.bottom()) <= e)
        return 7;
    return -1;
}

Rect App::window_at(Pt p) {
    if (!m_windows_ready && m_win_future.valid()) {
        m_windows       = m_win_future.get();
        m_windows_ready = true;
    }
    if (!m_ptr_out)
        return {};
    float f = m_ptr_out->fac;
    for (auto& w : m_windows) {
        if (w.monitor != m_ptr_out->name)
            continue;
        Rect r{static_cast<int>(std::lround(w.x * f)), static_cast<int>(std::lround(w.y * f)),
               static_cast<int>(std::lround(w.w * f)), static_cast<int>(std::lround(w.h * f))};
        if (r.contains(p.x, p.y))
            return r.clipped(m_ptr_out->shot.width, m_ptr_out->shot.height);
    }
    return {};
}

void App::update_hover(Pt p) {
    if (m_has_sel)
        return;
    m_hover     = window_at(p);
    m_has_hover = !m_hover.empty();
}

void App::update_cursor() {
    if (!m_cursor_dev || !m_enter_serial)
        return;
    using S          = uint32_t;
    static const S resize[9] = {
        WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NW_RESIZE, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_N_RESIZE,
        WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NE_RESIZE, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_W_RESIZE,
        WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_MOVE,      WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_E_RESIZE,
        WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_SW_RESIZE, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_S_RESIZE,
        WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_SE_RESIZE};
    S shape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_CROSSHAIR;
    switch (m_drag) {
        case Drag::Move: shape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_GRABBING; break;
        case Drag::Resize: shape = resize[std::clamp(m_handle, 0, 8)]; break;
        case Drag::Select:
        case Drag::Draw: break;
        case Drag::Toolbar: shape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER; break;
        case Drag::TextSel: shape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_TEXT; break;
        case Drag::None: {
            int h = handle_at(m_ptr);
            if (m_open >= 0 && m_ptr_out == m_sel_out && m_menu.inside(m_ptr.x - m_menu_x, m_ptr.y - m_menu_y))
                shape = m_menu_hover >= 0 ? WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER : WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT;
            else if (m_tool == Tool::TextSel && m_chip.layer.surf && m_ptr_out == m_sel_out &&
                     m_chip.inside(m_ptr.x - m_chip_x, m_ptr.y - m_chip_y))
                shape = m_chip_hover >= 0 ? WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER : WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT;
            else if (m_tb_visible && m_ptr_out == m_sel_out && m_toolbar.inside(m_ptr.x - m_tb_x, m_ptr.y - m_tb_y))
                shape = m_tb_hover >= 0 ? WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER : WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT;
            else if (h >= 0)
                shape = resize[h];
            else if (m_has_sel && m_ptr_out == m_sel_out && m_tool == Tool::Move && m_sel.contains(m_ptr.x, m_ptr.y))
                shape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_GRAB;
            else if (m_has_sel && m_tool == Tool::Text)
                shape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_TEXT;
            else if (m_has_sel && m_tool == Tool::TextSel && m_sel.contains(m_ptr.x, m_ptr.y))
                shape = m_ocr_pending ? WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_PROGRESS : WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_TEXT;
            break;
        }
    }
    if (shape != m_cursor_shape) {
        m_cursor_shape = shape;
        wp_cursor_shape_device_v1_set_shape(m_cursor_dev, m_enter_serial, shape);
    }
}

// --- логика выделения и рисования ---------------------------------------------------------

Pt App::constrain(Pt a, Pt b) const {
    if (!m_kbd.shift())
        return b;
    float dx = b.x - a.x, dy = b.y - a.y;
    switch (m_shape.tool) {
        case Tool::Line:
        case Tool::Arrow: {
            // угол кратен 15°
            float len = std::hypot(dx, dy), ang = std::atan2(dy, dx);
            float step = static_cast<float>(M_PI / 12);
            ang        = std::round(ang / step) * step;
            return {a.x + len * std::cos(ang), a.y + len * std::sin(ang)};
        }
        case Tool::Rect:
        case Tool::Ellipse:
        case Tool::Blur:
        case Tool::Pixelate: {
            float m = std::max(std::abs(dx), std::abs(dy));
            return {a.x + std::copysign(m, dx), a.y + std::copysign(m, dy)};
        }
        default: return b;
    }
}

void App::set_selection(Output* o, Rect r) {
    m_sel_out = o;
    m_sel     = r.clipped(o->shot.width, o->shot.height);
    m_has_sel = !m_sel.empty();
    m_has_hover = false;
}

void App::clear_selection() {
    Output* prev = m_sel_out;
    m_has_sel    = false;
    m_tb_visible = false;
    m_tb_hover   = -1;
    redraw(prev);
    update_hover(m_ptr);
    redraw(m_ptr_out);
}

void App::select_all() {
    Output* o = m_ptr_out ? m_ptr_out : m_outputs.front().get();
    if (m_sel_out && m_sel_out != o)
        redraw(m_sel_out);
    set_selection(o, {0, 0, o->shot.width, o->shot.height});
    redraw(o);
}

void App::press(Pt p) {
    // двойной клик внутри выделения — сразу в буфер, как во flameshot
    double now = ms();
    bool   dbl = now - m_last_click < 350 && std::hypot(p.x - m_last_click_p.x, p.y - m_last_click_p.y) < 6;
    m_last_click   = now;
    m_last_click_p = p;

    bool here = m_ptr_out == m_sel_out;
    if (m_open >= 0 && here && m_menu.inside(p.x - m_menu_x, p.y - m_menu_y)) {
        m_drag = Drag::Toolbar;
        int id = m_menu.hit(p.x - m_menu_x, p.y - m_menu_y);
        if (id >= 0)
            menu_click(id);
        redraw(m_sel_out);
        return;
    }
    if (m_tb_visible && here && m_toolbar.inside(p.x - m_tb_x, p.y - m_tb_y)) {
        m_drag  = Drag::Toolbar;
        int btn = m_toolbar.hit(p.x - m_tb_x, p.y - m_tb_y);
        if (btn >= 0)
            toolbar_click(btn);
        redraw(m_sel_out);
        return;
    }
    // клик мимо открытого меню только закрывает его
    if (m_open >= 0) {
        close_menu();
        m_drag = Drag::Toolbar;
        redraw(m_sel_out);
        return;
    }
    if (m_tool == Tool::TextSel && here) {
        if (m_chip.layer.surf && m_chip.inside(p.x - m_chip_x, p.y - m_chip_y)) {
            m_drag = Drag::Toolbar;
            int id = m_chip.hit(p.x - m_chip_x, p.y - m_chip_y);
            if (id == item::copy_sel)
                copy_words(false, true);
            else if (id == item::copy_all)
                copy_words(true, true);
            else if (id == item::done)
                select_tool(Tool::Move);
            redraw(m_sel_out);
            return;
        }
        if (m_sel.contains(p.x, p.y)) {
            m_drag = Drag::TextSel;
            // с Ctrl новый фрагмент добавляется к уже выделенным, без Ctrl — заменяет их
            if (m_kbd.ctrl()) {
                if (m_ts_a >= 0)
                    m_ts_more.push_back({m_ts_a, m_ts_b});
            } else
                m_ts_more.clear();
            long w = word_at(p);
            if (dbl && w >= 0) {
                // двойной клик — вся строка
                m_ts_a = m_ts_b = w;
                while (m_ts_a > 0 && m_words[m_ts_a - 1].line == m_words[w].line)
                    m_ts_a--;
                while (m_ts_b + 1 < static_cast<long>(m_words.size()) && m_words[m_ts_b + 1].line == m_words[w].line)
                    m_ts_b++;
            } else
                m_ts_a = m_ts_b = w;
            render_words();
            build_chip();
            update_cursor();
            redraw(m_sel_out);
            return;
        }
        select_tool(Tool::Move); // клик снаружи — обычное новое выделение
    }
    if (m_editing)
        commit_text();

    if (!m_has_sel || m_ptr_out != m_sel_out) {
        if (m_sel_out && m_sel_out != m_ptr_out)
            redraw(m_sel_out);
        m_has_sel = false;
        m_sel_out = m_ptr_out;
        m_drag    = Drag::Select;
        m_anchor  = p;
        m_moved   = false;
        m_sel     = {};
        update_cursor();
        redraw(m_ptr_out);
        return;
    }
    if (dbl && m_tool == Tool::Move && m_sel.contains(p.x, p.y)) {
        finish(Finish::Copy);
        return;
    }
    if (int h = handle_at(p); h >= 0) {
        m_drag     = Drag::Resize;
        m_handle   = h;
        m_anchor   = p;
        m_drag_sel = m_sel;
    } else if (m_tool == Tool::Move) {
        if (m_sel.contains(p.x, p.y)) {
            m_drag     = Drag::Move;
            m_anchor   = p;
            m_drag_sel = m_sel;
        } else {
            m_has_sel = false;
            m_drag    = Drag::Select;
            m_anchor  = p;
            m_moved   = false;
            m_sel     = {};
        }
    } else {
        begin_shape(p);
    }
    update_cursor();
    redraw(m_sel_out);
}

void App::motion(Pt p) {
    Output* o = m_ptr_out;
    switch (m_drag) {
        case Drag::Select:
            if (o != m_sel_out)
                break;
            if (std::hypot(p.x - m_anchor.x, p.y - m_anchor.y) > 3 * o->fac)
                m_moved = true;
            if (m_moved) {
                Rect r    = Rect::from_points(m_anchor, p);
                r.w       = std::max(r.w, 1);
                r.h       = std::max(r.h, 1);
                set_selection(o, r);
            }
            break;
        case Drag::Move: {
            int W = m_sel_out->shot.width, H = m_sel_out->shot.height;
            m_sel.x = std::clamp(m_drag_sel.x + static_cast<int>(p.x - m_anchor.x), 0, W - m_sel.w);
            m_sel.y = std::clamp(m_drag_sel.y + static_cast<int>(p.y - m_anchor.y), 0, H - m_sel.h);
            break;
        }
        case Drag::Resize: {
            float x0 = m_drag_sel.x, y0 = m_drag_sel.y, x1 = m_drag_sel.right(), y1 = m_drag_sel.bottom();
            float dx = p.x - m_anchor.x, dy = p.y - m_anchor.y;
            int   col = m_handle % 3, row = m_handle / 3;
            if (col == 0) x0 += dx;
            if (col == 2) x1 += dx;
            if (row == 0) y0 += dy;
            if (row == 2) y1 += dy;
            Rect r = Rect::from_points({x0, y0}, {x1, y1});
            r.w    = std::max(r.w, 1);
            r.h    = std::max(r.h, 1);
            set_selection(m_sel_out, r);
            break;
        }
        case Drag::Draw: update_shape(p); break;
        case Drag::Toolbar: break;
        case Drag::TextSel: {
            long w = word_at(p);
            if (w >= 0 && w != m_ts_b) {
                if (m_ts_a < 0)
                    m_ts_a = w;
                m_ts_b = w;
                render_words();
            }
            break;
        }
        case Drag::None: {
            update_hover(p);
            int hov = -1, mhov = -1, chov = -1;
            if (m_open >= 0 && o == m_sel_out)
                mhov = m_menu.hit(p.x - m_menu_x, p.y - m_menu_y);
            if (mhov < 0 && m_tb_visible && o == m_sel_out)
                hov = m_toolbar.hit(p.x - m_tb_x, p.y - m_tb_y);
            if (m_tool == Tool::TextSel && m_chip.layer.surf && o == m_sel_out)
                chov = m_chip.hit(p.x - m_chip_x, p.y - m_chip_y);
            if (hov != m_tb_hover) {
                m_tb_hover = hov;
                refresh_toolbar();
                // навели на «выделить текст» — начинаем распознавать заранее, к клику будет почти готово
                if (hov == static_cast<int>(Btn::TextSel))
                    start_ocr();
            }
            if (mhov != m_menu_hover) {
                m_menu_hover = mhov;
                m_menu.render(mhov);
            }
            if (chov != m_chip_hover) {
                m_chip_hover = chov;
                m_chip.render(chov);
            }
            break;
        }
    }
    update_cursor();
    redraw(o);
    if (m_sel_out && m_sel_out != o)
        redraw(m_sel_out);
}

void App::release(Pt p) {
    Drag d = m_drag;
    m_drag = Drag::None;
    if (d == Drag::Select) {
        Output* o = m_sel_out ? m_sel_out : m_ptr_out;
        if (!m_moved || m_sel.w < 3 || m_sel.h < 3) {
            // клик без протяжки: окно под курсором, а если окна нет — весь экран
            Rect w = window_at(p);
            set_selection(o, w.empty() ? Rect{0, 0, o->shot.width, o->shot.height} : w);
        }
    } else if (d == Drag::Draw) {
        commit_shape();
    } else if (d == Drag::TextSel) {
        // выделил — сразу в буфер; с зажатым Ctrl копируем, когда Ctrl отпустят
        m_ts_pending = m_ts_a >= 0 || !m_ts_more.empty();
        if (m_ts_pending && !m_kbd.ctrl())
            copy_words(false, false);
        else
            build_chip();
    }
    motion(p);
}

void App::right_press() {
    if (m_open >= 0)
        close_menu();
    else if (m_drag == Drag::Draw) {
        m_drag = Drag::None;
        m_sel_out->canvas.set_live(nullptr);
    } else if (m_editing)
        commit_text();
    else if (m_tool != Tool::Move)
        select_tool(Tool::Move);
    else if (m_has_sel)
        clear_selection();
    update_cursor();
    redraw(m_sel_out);
}

void App::begin_shape(Pt p) {
    m_shape        = {};
    m_shape.tool   = m_tool;
    m_shape.color  = color();
    m_shape.width  = m_width;
    m_shape.fill   = m_fill;
    m_shape.brush  = m_brush && (m_tool == Tool::Blur || m_tool == Tool::Pixelate);
    m_shape.pts    = {p, p};
    Canvas& c      = m_sel_out->canvas;
    if (m_tool == Tool::Counter) {
        m_shape.pts    = {p};
        m_shape.number = c.next_counter();
        c.add(m_shape);
        refresh_toolbar();
        return;
    }
    if (m_tool == Tool::Text) {
        m_shape.pts   = {p};
        m_shape.caret = true;
        m_editing     = true;
        c.set_live(&m_shape);
        return;
    }
    if (m_tool == Tool::Pen || m_tool == Tool::Marker || m_shape.brush)
        m_shape.pts = {p};
    m_drag = Drag::Draw;
    c.set_live(&m_shape);
}

void App::update_shape(Pt p) {
    if (m_shape.tool == Tool::Pen || m_shape.tool == Tool::Marker || m_shape.brush) {
        Pt last = m_shape.pts.back();
        if (std::hypot(p.x - last.x, p.y - last.y) < 1.5f)
            return;
        m_shape.pts.push_back(p);
    } else {
        m_shape.pts.back() = constrain(m_shape.pts.front(), p);
    }
    m_sel_out->canvas.set_live(&m_shape);
}

void App::commit_shape() {
    Canvas& c = m_sel_out->canvas;
    c.set_live(nullptr);
    Pt   a = m_shape.pts.front(), b = m_shape.pts.back();
    bool tiny = std::hypot(b.x - a.x, b.y - a.y) < 2;
    // случайный клик линией/рамкой ничего не добавляет; точка карандашом — добавляет
    if (!tiny || m_shape.tool == Tool::Pen || m_shape.tool == Tool::Marker || m_shape.brush)
        c.add(m_shape);
    refresh_toolbar();
}

void App::commit_text() {
    m_editing = false;
    m_sel_out->canvas.set_live(nullptr);
    m_shape.caret = false;
    m_sel_out->canvas.add(m_shape);
    refresh_toolbar();
    redraw(m_sel_out);
}

void App::cancel_text() {
    m_editing = false;
    m_sel_out->canvas.set_live(nullptr);
    redraw(m_sel_out);
}

void App::select_tool(Tool t) {
    if (m_editing)
        commit_text();
    // повторный выбор размытия/пикселизации с клавиатуры переключает кисть и рамку
    if (t == m_tool && (t == Tool::Blur || t == Tool::Pixelate))
        m_brush = !m_brush;
    if (m_tool == Tool::TextSel && t != Tool::TextSel)
        exit_textsel();
    if (t == Tool::Pen || t == Tool::Marker)
        m_pen_var = t;
    else if (t == Tool::Rect || t == Tool::Ellipse || t == Tool::Line)
        m_shape_var = t;
    else if (t == Tool::Blur || t == Tool::Pixelate)
        m_blur_var = t;
    m_tool = t;
    close_menu();
    refresh_toolbar();
    update_cursor();
}

ToolbarState App::toolbar_state() const {
    ToolbarState st;
    st.tool  = m_tool;
    st.pen   = m_pen_var;
    st.shape = m_shape_var;
    st.blur  = m_blur_var;
    st.color = m_color;
    st.width = m_width;
    st.fill  = m_fill;
    st.brush = m_brush;
    if (m_sel_out) {
        st.can_undo = !m_sel_out->canvas.empty();
        st.can_redo = m_sel_out->canvas.can_redo();
    }
    st.hover = m_tb_hover;
    st.open  = m_open;
    return st;
}

void App::refresh_toolbar() {
    if (m_toolbar.layer.surf)
        m_toolbar.render(toolbar_state());
}

void App::toolbar_click(int btn) {
    auto in_group = [&](Btn b) {
        switch (b) {
            case Btn::Pen: return m_tool == Tool::Pen || m_tool == Tool::Marker;
            case Btn::Shape: return m_tool == Tool::Rect || m_tool == Tool::Ellipse || m_tool == Tool::Line;
            case Btn::Blur: return m_tool == Tool::Blur || m_tool == Tool::Pixelate;
            default: return false;
        }
    };
    Btn b = static_cast<Btn>(btn);
    // группа: первый клик берёт инструмент, клик по уже выбранной открывает варианты
    if (b == Btn::Style || ((b == Btn::Pen || b == Btn::Shape || b == Btn::Blur) && in_group(b))) {
        m_open == btn ? close_menu() : open_menu(btn);
        refresh_toolbar();
        return;
    }
    close_menu();
    switch (b) {
        case Btn::Move: select_tool(Tool::Move); return;
        case Btn::Pen: select_tool(m_pen_var); return;
        case Btn::Arrow: select_tool(Tool::Arrow); return;
        case Btn::Shape: select_tool(m_shape_var); return;
        case Btn::Text: select_tool(Tool::Text); return;
        case Btn::Counter: select_tool(Tool::Counter); return;
        case Btn::Blur: select_tool(m_blur_var); return;
        case Btn::Undo: m_sel_out->canvas.undo(); break;
        case Btn::Redo: m_sel_out->canvas.redo(); break;
        case Btn::TextSel: start_textsel(); return;
        case Btn::Qr: finish(Finish::Qr); return;
        case Btn::Pin: finish(Finish::Pin); return;
        case Btn::Copy: finish(Finish::Copy); return;
        case Btn::Save: finish(Finish::Save); return;
        case Btn::Close: quit(1); return;
        default: break;
    }
    refresh_toolbar();
}

void App::open_menu(int btn) {
    float                 u = ui(m_sel_out);
    std::vector<MenuItem> items;
    auto tool_item = [&](Tool t, const char* tip) {
        MenuItem it;
        it.id     = item::tool + static_cast<int>(t);
        it.icon   = static_cast<int>(t);
        it.active = m_tool == t;
        it.tip    = tip;
        items.push_back(it);
    };
    switch (static_cast<Btn>(btn)) {
        case Btn::Pen:
            tool_item(Tool::Pen, "PEN   P");
            tool_item(Tool::Marker, "MARKER   M");
            break;
        case Btn::Shape:
            tool_item(Tool::Rect, "RECTANGLE   R  ·  SHIFT — SQUARE");
            tool_item(Tool::Ellipse, "ELLIPSE   E  ·  SHIFT — CIRCLE");
            tool_item(Tool::Line, "LINE   L  ·  SHIFT — 15° STEPS");
            break;
        case Btn::Blur: {
            auto add = [&](int id, Tool t, bool brush, const char* tip) {
                MenuItem it;
                it.id     = id;
                it.icon   = static_cast<int>(t);
                it.brush  = brush;
                it.active = m_tool == t && m_brush == brush;
                it.tip    = tip;
                items.push_back(it);
            };
            add(item::blur_rect, Tool::Blur, false, "BLUR AREA");
            add(item::blur_brush, Tool::Blur, true, "BLUR BRUSH");
            add(item::pix_rect, Tool::Pixelate, false, "PIXELATE AREA");
            add(item::pix_brush, Tool::Pixelate, true, "PIXELATE BRUSH");
            break;
        }
        case Btn::Style: {
            for (int i = 0; i < 8; i++) {
                MenuItem it;
                it.kind   = MenuItem::Swatch;
                it.id     = item::color + i;
                it.color  = i;
                it.active = i == m_color;
                it.tip    = "COLOR   " + std::to_string(i + 1);
                items.push_back(it);
            }
            MenuItem minus{MenuItem::TextItem, item::thinner, 0, 0, "−", "THINNER   [  OR WHEEL"};
            MenuItem val{MenuItem::Value, -1, 0, 0, std::to_string(static_cast<int>(m_width)), ""};
            MenuItem plus{MenuItem::TextItem, item::thicker, 0, 0, "+", "THICKER   ]  OR WHEEL"};
            MenuItem fill{MenuItem::IconItem, item::fill, static_cast<int>(Icon::Fill), 0, "",
                          "FILL SHAPES / TEXT BACKGROUND   F"};
            fill.active = m_fill;
            items.insert(items.end(), {minus, val, plus, fill});
            break;
        }
        default: return;
    }
    m_menu.build(std::move(items), u, m_opt.font);
    m_open       = btn;
    m_menu_hover = -1;
    m_menu.render(-1);
}

void App::close_menu() {
    if (m_open < 0)
        return;
    m_open       = -1;
    m_menu_hover = -1;
    refresh_toolbar();
}

// набираемый текст подхватывает новый цвет, толщину и подложку
void App::apply_style_to_text() {
    if (!m_editing)
        return;
    m_shape.color = color();
    m_shape.width = m_width;
    m_shape.fill  = m_fill;
    m_sel_out->canvas.set_live(&m_shape);
}

void App::menu_click(int id) {
    if (id >= item::tool && id < item::tool + static_cast<int>(Tool::Count)) {
        select_tool(static_cast<Tool>(id - item::tool));
        return;
    }
    switch (id) {
        case item::blur_rect:
        case item::blur_brush:
        case item::pix_rect:
        case item::pix_brush:
            m_brush = id == item::blur_brush || id == item::pix_brush;
            m_tool  = Tool::Move; // чтобы select_tool не переключил кисть обратно
            select_tool(id == item::blur_rect || id == item::blur_brush ? Tool::Blur : Tool::Pixelate);
            return;
        case item::thinner:
        case item::thicker:
            m_width = std::clamp(m_width + (id == item::thicker ? 1.f : -1.f), MIN_W, MAX_W);
            break;
        case item::fill: m_fill = !m_fill; break;
        default:
            if (id >= item::color && id < item::color + 8) {
                m_color = id - item::color;
                apply_style_to_text();
                close_menu(); // цвет выбран — меню больше не нужно
                return;
            }
            return;
    }
    apply_style_to_text();
    int open = m_open;
    open_menu(open); // перестроить: новое число толщины, состояние заливки
    m_menu_hover = id;
    m_menu.render(id);
    refresh_toolbar();
}

// --- выделение текста --------------------------------------------------------------------

void App::start_ocr() {
    if (!m_has_sel || !m_sel_out || (m_ocr_job && m_words_sel == m_sel))
        return; // это выделение уже распознано или распознаётся
    Output& o = *m_sel_out;
    m_renderer.make_current(o.target);
    FrameParams f;
    auto        job = std::make_shared<OcrJob>();
    job->sel        = m_sel;
    job->scale      = static_cast<long>(m_sel.w) * m_sel.h < 400000 ? 2.f : 1.f;
    std::vector<uint8_t> rgba;
    if (!m_renderer.readback(o.target, nullptr, f, m_sel, job->scale, rgba))
        return;
    int w = static_cast<int>(std::lround(m_sel.w * job->scale)), h = static_cast<int>(std::lround(m_sel.h * job->scale));

    m_ocr_job     = job;
    m_ocr_pending = true;
    m_scan_t0     = ms();
    m_words.clear();
    m_words_sel = m_sel;
    clear_text_ranges();
    std::thread([job, rgba = std::move(rgba), w, h, lang = m_opt.ocr_lang, fd = m_wake[1]] {
        job->words = ocr_words(rgba, w, h, lang, job->ok);
        job->done.store(true, std::memory_order_release);
        char c = 1;
        if (write(fd, &c, 1) < 0) {
        } // будит цикл; если сеанс уже закончился, результат просто не нужен
    }).detach();
}

void App::start_textsel() {
    if (!m_has_sel || !m_sel_out)
        return;
    if (m_editing)
        commit_text();
    close_menu();
    m_tool = Tool::TextSel;
    refresh_toolbar();
    start_ocr();
    if (!m_ocr_pending)
        render_words();
    build_chip();
    update_cursor();
}

void App::exit_textsel() {
    if (m_tool == Tool::TextSel)
        m_tool = Tool::Move;
    clear_text_ranges(); // распознанное остаётся: вернёшься к тексту — без ожидания
    if (!(m_sel == m_words_sel)) {
        m_ocr_job.reset();
        m_ocr_pending = false;
        m_words.clear();
    }
    refresh_toolbar();
}

void App::on_ocr_ready() {
    char buf[64];
    while (read(m_wake[0], buf, sizeof buf) > 0) {
    }
    auto job = m_ocr_job;
    // будить мог и поток от прошлого выделения — берём только своё и только готовое
    if (!job || !m_ocr_pending || !job->done.load(std::memory_order_acquire))
        return;
    // результат от старого выделения (поток мог доработать позже) — не наш
    if (!(job->sel == m_sel) || !(m_words_sel == m_sel))
        return;
    m_ocr_pending = false;
    m_words       = std::move(job->words);
    if (m_opt.timing)
        std::fprintf(stderr, "impshot: ocr %zu words %.0f ms\n", m_words.size(), ms() - m_scan_t0);
    for (auto& w : m_words) {
        // из пикселей увеличенной картинки — в пиксели выхода
        w.x = m_sel.x + static_cast<int>(w.x / job->scale);
        w.y = m_sel.y + static_cast<int>(w.y / job->scale);
        w.w = static_cast<int>(std::ceil(w.w / job->scale));
        w.h = static_cast<int>(std::ceil(w.h / job->scale));
    }
    if (!job->ok)
        std::fprintf(stderr, "impshot: tesseract failed (lang %s)\n", m_opt.ocr_lang.c_str());
    if (m_opt.demo > 0 && m_words.size() > 80) {
        m_ts_more = {{60, 70}}; // как будто второй фрагмент выделили с Ctrl
        m_ts_a    = 8;
        m_ts_b    = 30;
    }
    if (m_tool != Tool::TextSel)
        return; // распознали заранее, по наведению — покажем, когда нажмут кнопку
    render_words();
    if (m_opt.demo > 0 && m_ts_a >= 0)
        copy_words(false, false); // как будто протянули мышью
    else
        build_chip();
    update_cursor();
    redraw(m_sel_out);
}

long App::word_at(Pt p) const {
    if (m_words.empty())
        return -1;
    long n = static_cast<long>(m_words.size());
    // слово под курсором (с небольшим запасом)
    for (long i = 0; i < n; i++) {
        const auto& w = m_words[i];
        if (p.x >= w.x - 3 && p.x <= w.x + w.w + 3 && p.y >= w.y - 3 && p.y <= w.y + w.h + 3)
            return i;
    }
    // строка, в высоту которой попал курсор: ближайшее по горизонтали слово в ней
    long  best = -1;
    float bd   = 1e30f;
    for (long i = 0; i < n; i++) {
        const auto& w = m_words[i];
        if (p.y < w.y - 4 || p.y > w.y + w.h + 4)
            continue;
        float d = p.x < w.x ? w.x - p.x : p.x > w.x + w.w ? p.x - (w.x + w.w) : 0;
        if (d < bd)
            bd = d, best = i;
    }
    if (best >= 0)
        return best;
    // иначе просто ближайшее слово
    for (long i = 0; i < n; i++) {
        const auto& w  = m_words[i];
        float       dx = std::max({w.x - p.x, 0.f, p.x - (w.x + w.w)});
        float       dy = std::max({w.y - p.y, 0.f, p.y - (w.y + w.h)});
        float       d  = dx * dx + dy * dy;
        if (d < bd)
            bd = d, best = i;
    }
    return best;
}

void App::render_words() {
    if (!m_words_layer.surf || m_words_layer.w != m_words_sel.w || m_words_layer.h != m_words_sel.h)
        m_words_layer.init(CAIRO_FORMAT_ARGB32, m_words_sel.w, m_words_sel.h);
    cairo_t* cr = cairo_create(m_words_layer.surf);
    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    cairo_translate(cr, -m_words_sel.x, -m_words_sel.y);
    Color c = theme::phosphor;

    // все распознанные слова — едва заметная подложка
    for (auto& w : m_words) {
        cairo_rectangle(cr, w.x - 1, w.y - 1, w.w + 2, w.h + 2);
        cairo_set_source_rgba(cr, c.r, c.g, c.b, 0.10);
        cairo_fill(cr);
    }
    // выделенное — сплошной полосой по каждой строке, как в текстовом редакторе
    for (auto [lo_, hi_] : text_ranges())
    for (long i = static_cast<long>(lo_), hi = static_cast<long>(hi_); i <= hi;) {
        long j  = i;
        int  x0 = m_words[i].x, x1 = m_words[i].x + m_words[i].w, y0 = m_words[i].y, y1 = m_words[i].y + m_words[i].h;
        while (j + 1 <= hi && m_words[j + 1].line == m_words[i].line) {
            j++;
            x0 = std::min(x0, m_words[j].x);
            x1 = std::max(x1, m_words[j].x + m_words[j].w);
            y0 = std::min(y0, m_words[j].y);
            y1 = std::max(y1, m_words[j].y + m_words[j].h);
        }
        cairo_rectangle(cr, x0 - 3, y0 - 3, x1 - x0 + 6, y1 - y0 + 6);
        cairo_set_source_rgba(cr, c.r, c.g, c.b, 0.34);
        cairo_fill_preserve(cr);
        cairo_set_source_rgba(cr, c.r, c.g, c.b, 0.8);
        cairo_set_line_width(cr, 1);
        cairo_stroke(cr);
        i = j + 1;
    }
    cairo_destroy(cr);
    cairo_surface_flush(m_words_layer.surf);
    m_words_layer.damage({0, 0, m_words_layer.w, m_words_layer.h});
}

void App::build_chip(const std::string& copied) {
    float                 u = ui(m_sel_out);
    std::vector<MenuItem> items;
    if (m_ocr_pending)
        items.push_back({MenuItem::Value, -1, 0, 0, "RECOGNIZING TEXT…", ""});
    else if (m_words.empty())
        items.push_back({MenuItem::Value, -1, 0, 0, "NO TEXT FOUND", ""});
    else {
        bool has = m_ts_a >= 0 || !m_ts_more.empty();
        if (m_ts_pending && m_kbd.ctrl()) {
            size_t n = text_ranges().size();
            items.push_back({MenuItem::Value, -1, 0, 0,
                             "RELEASE CTRL TO COPY" + (n > 1 ? " " + std::to_string(n) + " PIECES" : std::string{}), ""});
        } else if (!copied.empty())
            items.push_back({MenuItem::Value, -1, 0, 0, copied, ""});
        else if (!has)
            items.push_back({MenuItem::Value, -1, 0, 0, "DRAG OVER TEXT TO COPY · HOLD CTRL FOR SEVERAL", ""});
        else
            items.push_back({MenuItem::TextItem, item::copy_sel, 0, 0, "COPY AGAIN", ""});
        items.push_back({MenuItem::TextItem, item::copy_all, 0, 0, "ALL TEXT", ""});
    }
    items.push_back({MenuItem::TextItem, item::done, 0, 0, "DONE", ""});
    m_chip.build(std::move(items), u, m_opt.font);
    m_chip_hover = -1;
    m_chip.render(-1);
}

void App::copy_words(bool all, bool explicit_copy) {
    if (m_words.empty())
        return;
    auto ranges = text_ranges();
    if (all || ranges.empty())
        ranges = {{0, m_words.size() - 1}};
    // фрагменты — по порядку на экране, каждый с новой строки
    std::string text;
    for (auto [lo, hi] : ranges) {
        if (!text.empty())
            text += '\n';
        text += join_words(m_words, lo, hi);
    }
    m_ts_pending = false;
    // как принято в Linux: выделенное мышью — в primary (средняя кнопка), Ctrl+C — в буфер обмена
    bool to_clip    = explicit_copy || m_opt.select_to != "primary";
    bool to_primary = !explicit_copy && m_opt.select_to != "clipboard";
    // primary — после закрытия оверлея (см. m_primary_text), буфер обмена — сразу
    if (to_primary)
        m_primary_text = text;
    if (to_clip) {
        std::thread job([text] {
            copy_to_clipboard(std::vector<uint8_t>(text.begin(), text.end()), "text/plain;charset=utf-8");
        });
        // одиночный запуск дождётся wl-copy перед выходом — иначе по Esc текст не успеет уйти в буфер
        if (m_opt.daemon)
            job.detach();
        else
            m_jobs.push_back(std::move(job));
    }
    size_t chars = 0;
    for (unsigned char ch : text)
        chars += (ch & 0xC0) != 0x80;
    std::string n = std::to_string(chars) + (chars == 1 ? " CHAR" : " CHARS");
    build_chip(!to_clip    ? "✓ " + n + " · MIDDLE-CLICK AFTER CLOSING"
               : to_primary ? "✓ COPIED " + n + " + PRIMARY"
                            : "✓ COPIED " + n);
}

void App::clear_text_ranges() {
    m_ts_a = m_ts_b = -1;
    m_ts_more.clear();
    m_ts_pending = false;
}

std::vector<std::pair<size_t, size_t>> App::text_ranges() const {
    std::vector<std::pair<size_t, size_t>> r;
    auto add = [&](long a, long b) {
        if (a >= 0 && b >= 0)
            r.push_back({static_cast<size_t>(std::min(a, b)), static_cast<size_t>(std::max(a, b))});
    };
    for (auto [a, b] : m_ts_more)
        add(a, b);
    add(m_ts_a, m_ts_b);
    std::ranges::sort(r);
    // пересекающиеся и соседние фрагменты склеиваем
    std::vector<std::pair<size_t, size_t>> out;
    for (auto& x : r) {
        if (!out.empty() && x.first <= out.back().second + 1)
            out.back().second = std::max(out.back().second, x.second);
        else
            out.push_back(x);
    }
    return out;
}

void App::nudge(int dx, int dy, bool resize) {
    if (!m_has_sel)
        return;
    int  W = m_sel_out->shot.width, H = m_sel_out->shot.height;
    Rect r = m_sel;
    if (resize) {
        r.w = std::clamp(r.w + dx, 1, W - r.x);
        r.h = std::clamp(r.h + dy, 1, H - r.y);
    } else {
        r.x = std::clamp(r.x + dx, 0, W - r.w);
        r.y = std::clamp(r.y + dy, 0, H - r.h);
    }
    m_sel = r;
    redraw(m_sel_out);
}

// --- клавиатура -----------------------------------------------------------------------

void App::on_keymap(uint32_t format, int fd, uint32_t size) {
    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
        close(fd);
        return;
    }
    m_kbd.load_keymap(fd, size);
}

void App::on_modifiers(uint32_t dep, uint32_t lat, uint32_t lock, uint32_t group) {
    m_kbd.update_modifiers(dep, lat, lock, group);
    bool ctrl   = m_kbd.ctrl();
    bool was    = m_ctrl_held;
    m_ctrl_held = ctrl;
    // отпустили Ctrl после выделения нескольких фрагментов — теперь всё в буфер
    if (was && !ctrl && m_tool == Tool::TextSel && m_ts_pending && m_drag != Drag::TextSel) {
        copy_words(false, false);
        redraw(m_sel_out);
    }
    // Shift во время протяжки сразу выравнивает фигуру
    if (m_drag == Drag::Draw) {
        update_shape(m_ptr);
        redraw(m_sel_out);
    }
}

void App::on_repeat_info(int32_t rate, int32_t delay) {
    m_repeat_rate  = rate;
    m_repeat_delay = delay;
}

void App::arm_repeat(bool on) {
    if (m_repeat_fd < 0)
        return;
    itimerspec its{};
    if (on && m_repeat_rate > 0) {
        its.it_value.tv_sec     = m_repeat_delay / 1000;
        its.it_value.tv_nsec    = (m_repeat_delay % 1000) * 1000000L;
        its.it_interval.tv_nsec = 1000000000L / m_repeat_rate;
    }
    timerfd_settime(m_repeat_fd, 0, &its, nullptr);
}

void App::key_repeat() {
    uint64_t n = 0;
    if (read(m_repeat_fd, &n, sizeof n) != sizeof n || !m_repeat_key)
        return;
    KeyEvent ev = m_kbd.translate(m_repeat_key);
    m_editing ? text_key(ev) : key_action(ev);
}

void App::on_key(uint32_t key, uint32_t state) {
    if (state != WL_KEYBOARD_KEY_STATE_PRESSED) {
        if (key == m_repeat_key || key == 0) {
            m_repeat_key = 0;
            arm_repeat(false);
        }
        return;
    }
    KeyEvent ev = m_kbd.translate(key);
    if (Pin* p = pin_of(m_kbd_surface)) {
        pin_key(*p, ev);
        return;
    }
    if (!m_running || m_outputs.empty() || !m_outputs.front()->surface)
        return; // вне сеанса клавиатура может достаться только закрепке
    m_editing ? text_key(ev) : key_action(ev);
    if (m_running && m_kbd.repeats(key) && (repeatable(ev.sym) || (m_editing && !ev.utf8.empty()))) {
        m_repeat_key = key;
        arm_repeat(true);
    }
}

void App::text_key(const KeyEvent& ev) {
    std::string& t = m_shape.text;
    if (ev.raw == XKB_KEY_Escape) {
        cancel_text();
        return;
    }
    if ((ev.raw == XKB_KEY_Return || ev.raw == XKB_KEY_KP_Enter) && !ev.shift) {
        commit_text();
        return;
    }
    if (ev.raw == XKB_KEY_Return || ev.raw == XKB_KEY_KP_Enter)
        t += '\n';
    else if (ev.raw == XKB_KEY_BackSpace) {
        // убрать последний символ UTF-8 целиком
        while (!t.empty() && (static_cast<unsigned char>(t.back()) & 0xC0) == 0x80)
            t.pop_back();
        if (!t.empty())
            t.pop_back();
    } else if (!ev.utf8.empty() && !ev.ctrl)
        t += ev.utf8;
    else
        return;
    m_sel_out->canvas.set_live(&m_shape);
    redraw(m_sel_out);
}

void App::key_action(const KeyEvent& ev) {
    // Esc закрывает снимок сразу, в каком бы режиме ни был
    if (ev.raw == XKB_KEY_Escape) {
        quit(1);
        return;
    }
    if (m_tool == Tool::TextSel) {
        bool enter = ev.sym == XKB_KEY_Return || ev.sym == XKB_KEY_KP_Enter;
        if ((ev.ctrl && ev.sym == XKB_KEY_c) || enter) {
            copy_words(m_ts_a < 0, true);
            redraw(m_sel_out);
            return;
        }
        if (ev.ctrl && ev.sym == XKB_KEY_a) {
            if (!m_words.empty()) {
                m_ts_more.clear();
                m_ts_a       = 0;
                m_ts_b       = static_cast<long>(m_words.size()) - 1;
                m_ts_pending = true;
                render_words();
                build_chip(); // скопируется, когда отпустят Ctrl
            }
            redraw(m_sel_out);
            return;
        }
    }
    if (ev.ctrl) {
        switch (ev.sym) {
            case XKB_KEY_c: finish(Finish::Copy); return;
            case XKB_KEY_s: finish(Finish::Save); return;
            case XKB_KEY_a: select_all(); return;
            case XKB_KEY_p: finish(Finish::Pin); return;
            case XKB_KEY_y:
                if (m_sel_out && m_sel_out->canvas.redo())
                    refresh_toolbar();
                break;
            case XKB_KEY_z:
                if (m_sel_out && (ev.shift ? m_sel_out->canvas.redo() : m_sel_out->canvas.undo()))
                    refresh_toolbar();
                break;
            default: return;
        }
        redraw(m_sel_out);
        return;
    }
    int step = ev.shift ? 10 : 1;
    switch (ev.sym) {
        case XKB_KEY_Return:
        case XKB_KEY_KP_Enter:
            if (!m_has_sel)
                select_all();
            finish(Finish::Copy);
            return;
        case XKB_KEY_Left: nudge(-step, 0, ev.alt); return;
        case XKB_KEY_Right: nudge(step, 0, ev.alt); return;
        case XKB_KEY_Up: nudge(0, -step, ev.alt); return;
        case XKB_KEY_Down: nudge(0, step, ev.alt); return;
        case XKB_KEY_o: finish(Finish::Ocr); return;
        case XKB_KEY_q: finish(Finish::Qr); return;
        case XKB_KEY_v: select_tool(Tool::Move); break;
        case XKB_KEY_p: select_tool(Tool::Pen); break;
        case XKB_KEY_l: select_tool(Tool::Line); break;
        case XKB_KEY_a: select_tool(Tool::Arrow); break;
        case XKB_KEY_r: select_tool(Tool::Rect); break;
        case XKB_KEY_e: select_tool(Tool::Ellipse); break;
        case XKB_KEY_m: select_tool(Tool::Marker); break;
        case XKB_KEY_t: select_tool(Tool::Text); break;
        case XKB_KEY_n: select_tool(Tool::Counter); break;
        case XKB_KEY_b: select_tool(Tool::Blur); break;
        case XKB_KEY_x: select_tool(Tool::Pixelate); break;
        case XKB_KEY_f:
            m_fill = !m_fill;
            refresh_toolbar();
            break;
        case XKB_KEY_bracketleft:
        case XKB_KEY_minus:
        case XKB_KEY_KP_Subtract: scroll(1); return;
        case XKB_KEY_bracketright:
        case XKB_KEY_equal:
        case XKB_KEY_plus:
        case XKB_KEY_KP_Add: scroll(-1); return;
        default:
            if (ev.sym >= XKB_KEY_1 && ev.sym <= XKB_KEY_8) {
                m_color = static_cast<int>(ev.sym - XKB_KEY_1);
                refresh_toolbar();
                break;
            }
            return;
    }
    redraw(m_sel_out ? m_sel_out : m_ptr_out);
}

// --- итог ------------------------------------------------------------------------------

void App::finish(Finish how) {
    if (!m_has_sel || !m_sel_out)
        return;
    if (m_editing)
        commit_text();
    if (m_drag == Drag::Draw) {
        m_drag = Drag::None;
        commit_shape();
    }
    Output& o = *m_sel_out;
    m_renderer.make_current(o.target);
    FrameParams f;
    f.block = std::round(10 * ui(&o));
    // распознаём чистый снимок: стрелки и размытие тексту только мешают
    bool  raw   = how == Finish::Ocr || how == Finish::Qr;
    // мелким выделениям увеличение помогает, большим только добавляет работы tesseract
    float scale = how == Finish::Ocr && static_cast<long>(m_sel.w) * m_sel.h < 400000 ? 2.f : 1.f;
    if (!m_renderer.readback(o.target, raw ? nullptr : &o.canvas, f, m_sel, scale, m_result)) {
        std::fprintf(stderr, "impshot: readback failed\n");
        quit(1);
        return;
    }
    Rect s       = m_sel.clipped(o.shot.width, o.shot.height);
    m_result_w   = static_cast<int>(std::lround(s.w * scale));
    m_result_h   = static_cast<int>(std::lround(s.h * scale));
    m_result_fac = o.fac;
    m_result_x   = static_cast<int>(std::lround(s.x / o.fac));
    m_result_y   = static_cast<int>(std::lround(s.y / o.fac));
    m_finish   = how;
    quit(0);
}

void App::setup_demo() {
    Output& o = *m_outputs.front();
    int     W = o.shot.width, H = o.shot.height;
    m_ptr_out = &o;
    set_selection(&o, {W / 5, H / 6, W * 3 / 5, H * 3 / 5});
    Rect    s = m_sel;
    Canvas& c = o.canvas;
    auto    P = [&](float fx, float fy) { return Pt{s.x + s.w * fx, s.y + s.h * fy}; };
    auto    add = [&](Tool t, int col, std::vector<Pt> pts, float w = 4, bool fill = false) {
        Shape sh;
        sh.tool  = t;
        sh.color = theme::palette[col];
        sh.width = w;
        sh.fill  = fill;
        sh.pts   = std::move(pts);
        return sh;
    };
    c.add(add(Tool::Rect, 0, {P(0.05f, 0.08f), P(0.35f, 0.3f)}));
    c.add(add(Tool::Arrow, 2, {P(0.55f, 0.55f), P(0.37f, 0.32f)}, 5));
    c.add(add(Tool::Ellipse, 3, {P(0.6f, 0.08f), P(0.9f, 0.3f)}, 3));
    c.add(add(Tool::Blur, 0, {P(0.05f, 0.62f), P(0.4f, 0.8f)}));
    c.add(add(Tool::Pixelate, 0, {P(0.45f, 0.65f), P(0.7f, 0.85f)}));
    std::vector<Pt> stroke;
    for (int i = 0; i <= 30; i++)
        stroke.push_back(P(0.06f + i * 0.01f, 0.9f + 0.03f * std::sin(i * 0.3f)));
    Shape brush = add(Tool::Blur, 0, stroke, 4);
    brush.brush = true;
    c.add(brush);
    c.add(add(Tool::Marker, 4, {P(0.62f, 0.45f), P(0.75f, 0.45f), P(0.9f, 0.46f)}, 6));
    std::vector<Pt> pen;
    for (int i = 0; i <= 40; i++)
        pen.push_back(P(0.75f + i * 0.004f, 0.6f + 0.05f * std::sin(i * 0.4f)));
    c.add(add(Tool::Pen, 5, pen, 3));
    Shape t = add(Tool::Text, 1, {P(0.06f, 0.36f)}, 5, true);
    t.text  = "impshot // phosphor test";
    c.add(t);
    for (int i = 0; i < 3; i++) {
        Shape n  = add(Tool::Counter, i == 1 ? 6 : 0, {P(0.42f + i * 0.06f, 0.15f)}, 4);
        n.number = i + 1;
        c.add(n);
    }
    m_tool     = Tool::Blur;
    m_blur_var = Tool::Blur;
    m_brush    = true;
    m_ptr      = P(0.5f, 1.0f);
    refresh_toolbar();
    // IMPSHOT_DEMO_UI=menu|text — показать открытое меню стиля или режим выделения текста
    std::string_view ui = getenv("IMPSHOT_DEMO_UI") ? getenv("IMPSHOT_DEMO_UI") : "";
    if (ui == "menu") {
        open_menu(static_cast<int>(Btn::Style));
        refresh_toolbar();
    } else if (ui == "text")
        start_textsel();
}

// --- цикл ------------------------------------------------------------------------------

// один снимок: крутится, пока сеанс не закончится; в демоне попутно слушает сокет
void App::loop() {
    double deadline = m_opt.demo > 0 ? ms() + m_opt.demo * 1000 : 0;
    while (m_running) {
        while (wl_display_prepare_read(m_dpy) != 0)
            if (wl_display_dispatch_pending(m_dpy) < 0)
                return quit(1);
        if (!m_running) {
            wl_display_cancel_read(m_dpy);
            break;
        }
        wl_display_flush(m_dpy);
        pollfd fds[4] = {{wl_display_get_fd(m_dpy), POLLIN, 0},
                         {m_repeat_fd, POLLIN, 0},
                         {m_listen_fd, POLLIN, 0},
                         {m_wake[0], POLLIN, 0}};
        int    timeout = deadline > 0 ? std::max(0, static_cast<int>(deadline - ms())) : -1;
        int    n       = poll(fds, 4, timeout);
        if (n < 0) {
            wl_display_cancel_read(m_dpy);
            if (errno == EINTR)
                continue;
            return quit(1);
        }
        if (fds[0].revents & POLLIN) {
            if (wl_display_read_events(m_dpy) < 0)
                return quit(1);
        } else {
            wl_display_cancel_read(m_dpy);
        }
        if (fds[0].revents & (POLLERR | POLLHUP))
            return quit(1);
        if (wl_display_dispatch_pending(m_dpy) < 0)
            return quit(1);
        if (fds[1].revents & POLLIN)
            key_repeat();
        if (fds[2].revents & POLLIN)
            drain_socket(); // повторный PrtScr во время сеанса ничего не делает
        if (fds[3].revents & POLLIN)
            on_ocr_ready();
        if (deadline > 0 && ms() >= deadline) {
            // проверка всего пути без мыши: IMPSHOT_DEMO_FINISH=save|copy|ocr|qr|pin
            const char* fin = getenv("IMPSHOT_DEMO_FINISH");
            std::string_view f = fin ? fin : "";
            if (f == "save")
                finish(Finish::Save);
            else if (f == "copy")
                finish(Finish::Copy);
            else if (f == "ocr")
                finish(Finish::Ocr);
            else if (f == "qr")
                finish(Finish::Qr);
            else if (f == "pin")
                finish(Finish::Pin);
            else
                quit(0);
        }
    }
}

std::string App::socket_path() {
    const char* rt = getenv("XDG_RUNTIME_DIR");
    return std::string(rt && *rt ? rt : "/tmp") + "/impshot.sock";
}

bool App::trigger_daemon() {
    std::string path = socket_path();
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof addr.sun_path)
        return false;
    std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return false;
    bool ok = connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0 && write(fd, "shot\n", 5) == 5;
    close(fd);
    return ok;
}

bool App::listen_socket() {
    if (trigger_daemon()) {
        std::fprintf(stderr, "impshot: daemon already running\n");
        return false;
    }
    std::string path = socket_path();
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof addr.sun_path)
        return false;
    std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
    unlink(path.c_str()); // остался от упавшего демона
    m_listen_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (m_listen_fd < 0 || bind(m_listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 ||
        listen(m_listen_fd, 4) != 0) {
        std::fprintf(stderr, "impshot: cannot listen on %s: %s\n", path.c_str(), strerror(errno));
        return false;
    }
    chmod(path.c_str(), 0600);
    return true;
}

void App::drain_socket() {
    for (;;) {
        int c = accept4(m_listen_fd, nullptr, nullptr, SOCK_CLOEXEC);
        if (c < 0)
            break;
        close(c);
        m_trigger = true;
    }
}

void App::reset_session() {
    m_t0        = std::chrono::steady_clock::now();
    m_running   = true;
    m_exit      = 1;
    m_finish    = Finish::None;
    m_ptr_out   = nullptr;
    m_ptr       = {};
    m_enter_serial = 0;
    m_cursor_shape = 0;
    m_last_click   = -1;
    m_axis_acc     = 0;
    m_v120_acc     = 0;
    m_axis_seen = m_v120_seen = false;
    m_sel_out   = nullptr;
    m_has_sel   = false;
    m_sel       = {};
    m_drag      = Drag::None;
    m_handle    = -1;
    m_moved     = false;
    m_windows.clear();
    m_windows_ready = false;
    m_hover         = {};
    m_has_hover     = false;
    m_tool          = Tool::Move; // цвет и толщина запоминаются между снимками
    m_shape         = {};
    m_editing       = false;
    m_tb_hover      = -1;
    m_tb_visible    = false;
    m_open          = -1;
    m_menu_hover    = -1;
    m_ocr_job.reset();
    m_primary_text.clear();
    m_ocr_pending = false;
    m_words.clear();
    m_words_sel = {};
    clear_text_ranges();
    m_chip_hover    = -1;
    m_result.clear();
    m_result_w = m_result_h = 0;
    m_repeat_key = 0;
    arm_repeat(false);
}

int App::session() {
    reset_session();
    m_win_future = std::async(std::launch::async, hypr_windows);

    int captured = 0;
    for (auto& o : m_outputs) {
        o->shot = {};
        if (capture_output(m_capture, o->output, o->shot))
            captured++;
        else
            std::fprintf(stderr, "impshot: capture of %s failed\n", o->name.c_str());
    }
    if (m_opt.timing)
        std::fprintf(stderr, "impshot: captured %.1f ms\n", ms());
    if (!captured) {
        m_win_future.wait();
        return 1;
    }
    // поверхности — только после снимка, иначе оверлей попадёт в кадр
    for (auto& o : m_outputs)
        if (!o->shot.empty())
            create_surface(*o);
    wl_display_flush(m_dpy);

    loop();

    // оверлей убираем сразу; кодирование и запись идут уже без него
    for (auto& o : m_outputs) {
        destroy_surface(*o);
        for (Layer* l : {&o->canvas.anno, &o->canvas.live, &o->canvas.mblur, &o->canvas.mpix})
            m_renderer.release(*l);
        o->canvas.clear();
        o->shot   = {};
    }
    wl_display_roundtrip(m_dpy);
    if (!m_primary_text.empty()) {
        std::thread job([text = std::move(m_primary_text)] {
            copy_to_clipboard(std::vector<uint8_t>(text.begin(), text.end()), "text/plain;charset=utf-8", true);
        });
        m_primary_text.clear();
        if (m_opt.daemon)
            job.detach();
        else
            m_jobs.push_back(std::move(job));
    }
    if (m_win_future.valid())
        m_win_future.wait();
    if (m_finish != Finish::None)
        deliver();
    malloc_trim(0); // в простое демону не нужны десятки МБ от снимка и слоёв
    return m_exit;
}

// PNG, буфер обмена, файл, распознавание — в отдельном потоке, чтобы демон сразу был готов к следующему снимку
void App::export_image(std::vector<uint8_t> rgba, int w, int h, bool save) {
    std::thread job([rgba = std::move(rgba), w, h, save, opt = m_opt, t0 = m_t0] {
        auto ms = [&] { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(); };
        std::vector<uint8_t> png;
        if (!encode_png(rgba, w, h, png)) {
            std::fprintf(stderr, "impshot: PNG encoding failed\n");
            return;
        }
        if (opt.timing)
            std::fprintf(stderr, "impshot: png %zu KiB %.1f ms\n", png.size() / 1024, ms());
        if ((!save || opt.copy_on_save) && !copy_to_clipboard(png))
            std::fprintf(stderr, "impshot: wl-copy failed\n");
        if (!save)
            return;
        std::string path = save_png(png, opt.save_dir);
        if (path.empty()) {
            std::fprintf(stderr, "impshot: cannot write to %s\n", opt.save_dir.c_str());
            if (opt.notify)
                notify("Screenshot not saved", "Cannot write to " + opt.save_dir, "");
            return;
        }
        std::printf("%s\n", path.c_str());
        std::fflush(stdout);
        if (opt.notify)
            notify("Screenshot saved", path, path);
    });
    // демон живёт дальше — поток сам доработает; одиночный запуск дождётся его перед выходом
    if (m_opt.daemon)
        job.detach();
    else
        m_jobs.push_back(std::move(job));
}

namespace {

// первые строки распознанного — для текста уведомления
std::string preview(const std::string& t, size_t max = 160) {
    if (t.size() <= max)
        return t;
    size_t n = max;
    while (n > 0 && (static_cast<unsigned char>(t[n]) & 0xC0) == 0x80)
        n--;
    return t.substr(0, n) + "…";
}

void copy_text(const std::string& t) {
    copy_to_clipboard(std::vector<uint8_t>(t.begin(), t.end()), "text/plain;charset=utf-8");
}

} // namespace

void App::deliver() {
    switch (m_finish) {
        case Finish::Copy:
        case Finish::Save:
            export_image(std::move(m_result), m_result_w, m_result_h, m_finish == Finish::Save);
            return;
        case Finish::Pin: {
            if (!m_wm_base) {
                std::fprintf(stderr, "impshot: compositor lacks xdg-shell\n");
                return;
            }
            PinGlobals g{m_compositor, m_shm, m_wm_base, m_viewporter, m_alpha_mgr};
            m_pins.push_back(std::make_unique<Pin>(g, std::move(m_result), m_result_w, m_result_h, m_result_fac, ++m_pin_seq,
                                                   m_result_x, m_result_y));
            wl_display_flush(m_dpy);
            return;
        }
        case Finish::Ocr:
        case Finish::Qr: break;
        case Finish::None: return;
    }
    std::thread job([rgba = std::move(m_result), w = m_result_w, h = m_result_h, how = m_finish, opt = m_opt] {
        if (how == Finish::Ocr) {
            bool        ok   = false;
            std::string text = ocr_text(rgba, w, h, opt.ocr_lang, ok);
            if (!ok) {
                std::fprintf(stderr, "impshot: tesseract failed (lang %s)\n", opt.ocr_lang.c_str());
                notify("Text recognition failed", "tesseract exited with an error", "");
            } else if (text.empty()) {
                notify("No text found", "Nothing recognized in the selection", "");
            } else {
                copy_text(text);
                notify("Text copied", preview(text), "");
            }
            return;
        }
        auto codes = read_codes(rgba, w, h);
        if (codes.empty()) {
            notify("No QR code found", "No readable codes in the selection", "");
            return;
        }
        std::string all;
        for (auto& c : codes)
            all += (all.empty() ? "" : "\n") + c;
        copy_text(all);
        notify(codes.size() == 1 ? "QR code copied" : "Codes copied (" + std::to_string(codes.size()) + ")", preview(all), "");
    });
    if (m_opt.daemon)
        job.detach();
    else
        m_jobs.push_back(std::move(job));
}

void App::pin_key(Pin& p, const KeyEvent& ev) {
    if (ev.raw == XKB_KEY_Escape)
        p.closed = true;
    else if (ev.ctrl && (ev.sym == XKB_KEY_c || ev.sym == XKB_KEY_s))
        export_image(p.rgba(), p.width(), p.height(), ev.sym == XKB_KEY_s);
    else if (ev.sym == XKB_KEY_0)
        p.reset_zoom();
    else if (ev.sym == XKB_KEY_equal || ev.sym == XKB_KEY_plus || ev.sym == XKB_KEY_KP_Add)
        p.zoom(1);
    else if (ev.sym == XKB_KEY_minus || ev.sym == XKB_KEY_KP_Subtract)
        p.zoom(-1);
}

// простой между снимками: события Wayland (мониторы, закрепки) и сокет демона
void App::idle_loop() {
    while (m_opt.daemon || !m_pins.empty()) {
        while (wl_display_prepare_read(m_dpy) != 0)
            if (wl_display_dispatch_pending(m_dpy) < 0)
                return;
        wl_display_flush(m_dpy);
        pollfd fds[2] = {{wl_display_get_fd(m_dpy), POLLIN, 0}, {m_listen_fd, POLLIN, 0}};
        if (poll(fds, 2, -1) < 0) {
            wl_display_cancel_read(m_dpy);
            if (errno == EINTR)
                continue;
            return;
        }
        if (fds[0].revents & POLLIN) {
            if (wl_display_read_events(m_dpy) < 0)
                return;
        } else {
            wl_display_cancel_read(m_dpy);
        }
        if (fds[0].revents & (POLLERR | POLLHUP)) {
            std::fprintf(stderr, "impshot: Wayland connection lost\n");
            return;
        }
        if (wl_display_dispatch_pending(m_dpy) < 0)
            return;
        // закрытые закрепки удаляем вне обработчиков их же событий
        std::erase_if(m_pins, [&](auto& p) {
            if (!p->closed)
                return false;
            if (m_ptr_pin == p.get())
                m_ptr_pin = nullptr;
            if (m_kbd_surface == p->surface())
                m_kbd_surface = nullptr;
            return true;
        });
        if (fds[1].revents & POLLIN)
            drain_socket();
        if (m_trigger) {
            m_trigger = false;
            session();
            m_trigger = false; // нажатия во время сеанса не копим
        }
    }
}

int App::run() {
    m_t0 = std::chrono::steady_clock::now();
    if (!setup_wayland())
        return 1;
    if (!m_renderer.init(m_dpy)) {
        std::fprintf(stderr, "impshot: EGL/GLES3 init failed\n");
        return 1;
    }
    if (m_opt.timing)
        std::fprintf(stderr, "impshot: wayland+egl %.1f ms\n", ms());
    m_repeat_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    if (pipe2(m_wake, O_CLOEXEC | O_NONBLOCK) != 0)
        return 1;

    if (m_opt.daemon && !listen_socket())
        return 1;
    signal(SIGPIPE, SIG_IGN);
    int rc = m_opt.daemon ? 0 : session();
    // одиночный запуск живёт, пока открыты его закрепки
    idle_loop();
    for (auto& t : m_jobs)
        t.join();
    return m_opt.daemon ? 1 : rc;
}
