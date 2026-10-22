// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "canvas.hpp"
#include "capture.hpp"
#include "hypr.hpp"
#include "keyboard.hpp"
#include "pin.hpp"
#include "recognize.hpp"
#include "renderer.hpp"
#include "toolbar.hpp"

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <vector>

struct wl_display;
struct wl_registry;
struct wl_compositor;
struct wl_shm;
struct wl_seat;
struct wl_keyboard;
struct wl_pointer;
struct wl_output;
struct wl_surface;
struct wl_callback;
struct wp_viewporter;
struct wp_viewport;
struct wp_cursor_shape_manager_v1;
struct wp_cursor_shape_device_v1;
struct zwlr_layer_shell_v1;
struct xdg_wm_base;
struct wp_alpha_modifier_v1;
struct zwlr_layer_surface_v1;

struct Options {
    std::string font     = "CommitMono Nerd Font";
    std::string save_dir;          // пусто — ~/Pictures/Screenshots
    float       ui_scale = 1.25f;
    float       width    = 4;
    int         color    = 0;
    bool        copy_on_save = true;
    bool        notify       = true;
    bool        timing       = false;
    std::string ocr_lang     = "rus+eng";
    std::string select_to    = "primary"; // куда идёт текст, выделенный мышью: primary | clipboard | both
    bool        daemon       = false; // резидентно: EGL и Wayland готовы заранее, снимок по сигналу из сокета
    double      demo         = 0;  // > 0 — показать готовую сцену и выйти через столько секунд (для проверки вида)
};

class App;

struct Output {
    App*        app    = nullptr;
    uint32_t    global = 0;
    wl_output*  output = nullptr;
    int32_t     scale  = 1;
    std::string name;
    Screenshot  shot;

    wl_surface*             surface    = nullptr;
    zwlr_layer_surface_v1*  layer      = nullptr;
    wp_viewport*            viewport   = nullptr;
    wl_callback*            frame_cb   = nullptr;

    int    lw = 0, lh = 0; // логический размер поверхности
    bool   configured = false, dirty = false;
    float  fac = 1;        // пикселей снимка на логический пиксель
    Target target;
    Canvas canvas;
};

class App {
public:
    explicit App(Options o);
    ~App();
    int run();

    static std::string socket_path();
    // попросить работающий демон сделать снимок; false — демона нет
    static bool trigger_daemon();

    void on_global(wl_registry* reg, uint32_t name, const char* iface, uint32_t version);
    void on_global_remove(uint32_t name);
    void on_seat_caps(uint32_t caps);
    void on_keymap(uint32_t format, int fd, uint32_t size);
    void on_key(uint32_t key, uint32_t state);
    void on_modifiers(uint32_t dep, uint32_t lat, uint32_t lock, uint32_t group);
    void on_repeat_info(int32_t rate, int32_t delay);
    void on_pointer_enter(uint32_t serial, wl_surface* s, double x, double y);
    void on_pointer_leave();
    void on_keyboard_enter(wl_surface* s);
    void on_motion(double x, double y);
    void on_button(uint32_t serial, uint32_t button, uint32_t state);
    void on_axis(double value);
    void on_axis120(int value);
    void on_pointer_frame();
    void on_configure(Output& o, int w, int h);
    void on_layer_closed();
    void on_frame_done(Output& o);
    void quit(int code);

private:
    enum class Drag { None, Select, Move, Resize, Draw, Toolbar, TextSel };
    enum class Finish { None, Copy, Save, Ocr, Qr, Pin };

    bool  setup_wayland();
    int   session();
    void  reset_session();
    void  deliver();
    void  idle_loop();
    Pin*  pin_of(wl_surface* s);
    void  pin_key(Pin& p, const KeyEvent& ev);
    void  export_image(std::vector<uint8_t> rgba, int w, int h, bool save);
    bool  listen_socket();
    void  drain_socket();
    void  create_surface(Output& o);
    void  destroy_surface(Output& o);
    void  render(Output& o);
    void  redraw(Output* o);
    void  redraw_all();
    void  loop();
    void  key_action(const KeyEvent& ev);
    void  text_key(const KeyEvent& ev);
    void  key_repeat();
    void  arm_repeat(bool on);

    // итог: пиксели выделения, снятые до скрытия оверлея
    std::vector<uint8_t> m_result;
    int                  m_result_w = 0, m_result_h = 0;
    float                m_result_fac = 1;
    int                  m_result_x = 0, m_result_y = 0; // логические, от угла монитора

    // логика
    void  press(Pt p);
    void  motion(Pt p);
    void  release(Pt p);
    void  right_press();
    void  update_hover(Pt p);
    void  update_cursor();
    void  select_tool(Tool t);
    void  toolbar_click(int btn);
    void  set_selection(Output* o, Rect r);
    void  clear_selection();
    void  begin_shape(Pt p);
    void  update_shape(Pt p);
    void  commit_shape();
    void  commit_text();
    void  cancel_text();
    void  nudge(int dx, int dy, bool resize);
    void  select_all();
    void  scroll(int steps);
    void  refresh_toolbar();
    ToolbarState toolbar_state() const;
    void  open_menu(int btn);
    void  close_menu();
    void  menu_click(int id);
    void  apply_style_to_text();
    // выделение текста на снимке
    void  start_textsel();
    void  start_ocr(); // распознать текущее выделение в фоне, если ещё не
    void  exit_textsel();
    void  on_ocr_ready();
    long  word_at(Pt p) const;
    void  render_words();
    void  build_chip(const std::string& copied = {});
    // explicit_copy — Ctrl+C / кнопка: всегда в буфер обмена; иначе — выделение мышью, по select_to
    void  copy_words(bool all, bool explicit_copy);
    void  clear_text_ranges();
    std::vector<std::pair<size_t, size_t>> text_ranges() const; // по порядку, без пересечений
    void  finish(Finish how);
    void  setup_demo();

    int   handle_at(Pt p) const; // 0..8 без 4; -1 — нет
    Rect  window_at(Pt p);
    Pt    constrain(Pt a, Pt b) const;
    Color color() const { return theme::palette[m_color]; }
    float ui(const Output* o) const { return (o ? o->fac : 1.f) * m_opt.ui_scale; }
    void  layout_ui(Output& o, std::vector<Sprite>& out);
    double ms() const;

    Options m_opt;

    wl_display*                     m_dpy        = nullptr;
    wl_registry*                    m_registry   = nullptr;
    wl_compositor*                  m_compositor = nullptr;
    wl_shm*                         m_shm        = nullptr;
    wl_seat*                        m_seat       = nullptr;
    wl_keyboard*                    m_keyboard   = nullptr;
    wl_pointer*                     m_pointer    = nullptr;
    wp_viewporter*                  m_viewporter = nullptr;
    wp_cursor_shape_manager_v1*     m_cursor_mgr = nullptr;
    wp_cursor_shape_device_v1*      m_cursor_dev = nullptr;
    zwlr_layer_shell_v1*            m_layer_shell = nullptr;
    xdg_wm_base*                    m_wm_base     = nullptr;
    wp_alpha_modifier_v1*           m_alpha_mgr   = nullptr;
    CaptureGlobals                  m_capture;

    std::vector<std::unique_ptr<Output>> m_outputs;
    std::vector<std::unique_ptr<Pin>>    m_pins;
    int                                  m_pin_seq = 0;
    Pin*                                 m_ptr_pin = nullptr; // окно-закрепка под курсором
    wl_surface*                          m_kbd_surface = nullptr;
    std::vector<std::thread>             m_jobs; // кодирование и отдача результата — уже без оверлея
    Renderer                             m_renderer;
    Keyboard                             m_kbd;
    bool                                 m_running = true;
    int                                  m_exit    = 1;
    Finish                               m_finish  = Finish::None;
    std::chrono::steady_clock::time_point m_t0;

    // указатель
    Output*  m_ptr_out      = nullptr;
    Pt       m_ptr;
    uint32_t m_enter_serial = 0;
    uint32_t m_cursor_shape = 0;
    double   m_last_click   = -1;
    double   m_axis_acc     = 0;
    int      m_v120_acc     = 0;
    bool     m_axis_seen    = false, m_v120_seen = false;
    Pt       m_last_click_p;

    // выделение
    Output* m_sel_out = nullptr;
    bool    m_has_sel = false;
    Rect    m_sel;
    Drag    m_drag = Drag::None;
    Pt      m_anchor;
    Rect    m_drag_sel;
    int     m_handle = -1;
    bool    m_moved  = false;

    // окна Hyprland: запрос уходит в фоне сразу при старте
    std::future<std::vector<WinRect>> m_win_future;
    std::vector<WinRect>              m_windows;
    bool                              m_windows_ready = false;
    Rect                              m_hover;
    bool                              m_has_hover = false;

    // инструменты
    Tool  m_tool  = Tool::Move;
    int   m_color = 0;
    float m_width = 4;
    bool  m_fill  = false;
    bool  m_brush = false; // размытие и пикселизация кистью
    Shape m_shape;
    bool  m_editing = false; // набор текста

    Toolbar m_toolbar;
    int     m_tb_hover = -1;
    float   m_tb_x = 0, m_tb_y = 0;
    bool    m_tb_visible = false;
    Label   m_size_label, m_tip, m_help, m_coords;

    // группы панели: какой вариант показан, какое меню открыто
    Tool  m_pen_var = Tool::Pen, m_shape_var = Tool::Rect, m_blur_var = Tool::Blur;
    int   m_open = -1, m_menu_hover = -1;
    Menu  m_menu;
    float m_menu_x = 0, m_menu_y = 0;

    // режим выделения текста: распознавание идёт в потоке, о готовности сообщает m_wake
    struct OcrJob {
        Rect                 sel;
        float                scale = 1;
        std::vector<OcrWord> words;
        bool                 ok = false;
        std::atomic<bool>    done{false};
    };
    std::shared_ptr<OcrJob> m_ocr_job;
    bool                    m_ocr_pending = false;
    double                  m_scan_t0     = 0;
    std::vector<OcrWord>    m_words;   // рамки — в пикселях выхода
    Rect                    m_words_sel; // для какого выделения распознано
    long                    m_ts_a = -1, m_ts_b = -1; // текущий фрагмент (номера слов)
    std::vector<std::pair<long, long>> m_ts_more;      // уже выделенные с Ctrl фрагменты
    bool                    m_ts_pending = false;     // выделение ещё не ушло в буфер (ждём отпускания Ctrl)
    bool                    m_ctrl_held  = false;
    // Hyprland не даёт сменить primary, пока фокус у оверлея, — пишем его уже после закрытия
    std::string             m_primary_text;
    Layer                   m_words_layer;
    Menu                    m_chip;
    int                     m_chip_hover = -1;
    float                   m_chip_x = 0, m_chip_y = 0;
    int                     m_wake[2] = {-1, -1};

    int      m_repeat_fd = -1, m_listen_fd = -1;
    bool     m_trigger   = false;
    int32_t  m_repeat_rate = 25, m_repeat_delay = 600;
    uint32_t m_repeat_key  = 0;
};
