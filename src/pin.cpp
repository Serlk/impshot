#include "pin.hpp"

#include "alpha-modifier-v1.h"
#include "viewporter.h"
#include "xdg-shell.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <spawn.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wayland-client.h>

extern char** environ;

namespace {

constexpr int BORDER = 2; // фосфорная рамка вокруг снимка, px буфера

const xdg_surface_listener xsurface_listener = {
    .configure = [](void* d, xdg_surface*, uint32_t serial) { static_cast<Pin*>(d)->on_configure(serial); },
};

const xdg_toplevel_listener toplevel_listener = {
    .configure        = [](void*, xdg_toplevel*, int32_t, int32_t, wl_array*) {}, // размер задаём сами
    .close            = [](void* d, xdg_toplevel*) { static_cast<Pin*>(d)->closed = true; },
    .configure_bounds = [](void*, xdg_toplevel*, int32_t, int32_t) {},
    .wm_capabilities  = [](void*, xdg_toplevel*, wl_array*) {},
};

// правило Hyprland на лету: окно с этим заголовком откроется ровно на месте выделения
void place_rule(const std::string& title, int x, int y) {
    std::string lua = "hl.window_rule({ name = \"impshot-pin-place\", match = { title = \"^(" + title +
        ")$\" }, float = true, move = \"" + std::to_string(x) + " " + std::to_string(y) + "\" })";
    const char* argv[] = {"hyprctl", "eval", lua.c_str(), nullptr};
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    pid_t pid = -1;
    if (posix_spawnp(&pid, argv[0], &fa, nullptr, const_cast<char**>(argv), environ) == 0)
        waitpid(pid, nullptr, 0);
    posix_spawn_file_actions_destroy(&fa);
}

} // namespace

Pin::Pin(const PinGlobals& g, std::vector<uint8_t> rgba, int w, int h, float fac, int id, int x, int y)
    : m_g(g), m_rgba(std::move(rgba)), m_w(w), m_h(h), m_bw(w + 2 * BORDER), m_bh(h + 2 * BORDER), m_fac(fac) {
    std::string title = "impshot-pin-" + std::to_string(id);
    place_rule(title, x - static_cast<int>(std::lround(BORDER / fac)), y - static_cast<int>(std::lround(BORDER / fac)));

    // XRGB-буфер: картинка в рамке цвета люминофора
    size_t stride = static_cast<size_t>(m_bw) * 4, size = stride * m_bh;
    int    fd     = memfd_create("impshot-pin", MFD_CLOEXEC);
    if (fd >= 0 && ftruncate(fd, static_cast<off_t>(size)) == 0) {
        void* map = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (map != MAP_FAILED) {
            auto* px = static_cast<uint32_t*>(map);
            std::fill_n(px, static_cast<size_t>(m_bw) * m_bh, 0xFF33FF6Bu);
            for (int yy = 0; yy < h; yy++) {
                const uint8_t* src = &m_rgba[static_cast<size_t>(yy) * w * 4];
                uint32_t*      dst = px + static_cast<size_t>(yy + BORDER) * m_bw + BORDER;
                for (int xx = 0; xx < w; xx++, src += 4)
                    dst[xx] = 0xFF000000u | (uint32_t(src[0]) << 16) | (uint32_t(src[1]) << 8) | src[2];
            }
            munmap(map, size);
            wl_shm_pool* pool = wl_shm_create_pool(g.shm, fd, static_cast<int32_t>(size));
            m_buffer = wl_shm_pool_create_buffer(pool, 0, m_bw, m_bh, static_cast<int32_t>(stride), WL_SHM_FORMAT_XRGB8888);
            wl_shm_pool_destroy(pool);
        }
    }
    if (fd >= 0)
        close(fd);

    m_surface = wl_compositor_create_surface(g.compositor);
    if (g.viewporter)
        m_viewport = wp_viewporter_get_viewport(g.viewporter, m_surface);
    if (g.alpha)
        m_alpha_s = wp_alpha_modifier_v1_get_surface(g.alpha, m_surface);
    m_xsurface = xdg_wm_base_get_xdg_surface(g.wm_base, m_surface);
    xdg_surface_add_listener(m_xsurface, &xsurface_listener, this);
    m_toplevel = xdg_surface_get_toplevel(m_xsurface);
    xdg_toplevel_add_listener(m_toplevel, &toplevel_listener, this);
    xdg_toplevel_set_app_id(m_toplevel, "impshot-pin");
    xdg_toplevel_set_title(m_toplevel, title.c_str());
    wl_surface_commit(m_surface);
}

Pin::~Pin() {
    if (m_alpha_s)
        wp_alpha_modifier_surface_v1_destroy(m_alpha_s);
    if (m_viewport)
        wp_viewport_destroy(m_viewport);
    if (m_toplevel)
        xdg_toplevel_destroy(m_toplevel);
    if (m_xsurface)
        xdg_surface_destroy(m_xsurface);
    if (m_surface)
        wl_surface_destroy(m_surface);
    if (m_buffer)
        wl_buffer_destroy(m_buffer);
}

void Pin::on_configure(uint32_t serial) {
    xdg_surface_ack_configure(m_xsurface, serial);
    if (!m_mapped) {
        m_mapped = true;
        wl_surface_attach(m_surface, m_buffer, 0, 0);
        wl_surface_damage_buffer(m_surface, 0, 0, m_bw, m_bh);
    }
    apply();
}

void Pin::apply() {
    int lw = std::max(8, static_cast<int>(std::lround(m_bw / m_fac * m_zoom)));
    int lh = std::max(8, static_cast<int>(std::lround(m_bh / m_fac * m_zoom)));
    if (m_viewport)
        wp_viewport_set_destination(m_viewport, lw, lh);
    if (m_alpha_s)
        wp_alpha_modifier_surface_v1_set_multiplier(m_alpha_s, static_cast<uint32_t>(m_alpha * 4294967295.0));
    wl_surface_commit(m_surface);
}

void Pin::zoom(int steps) {
    if (!m_viewport)
        return;
    m_zoom = std::clamp(m_zoom * std::pow(1.1f, static_cast<float>(steps)), 0.1f, 8.f);
    apply();
}

void Pin::reset_zoom() {
    m_zoom  = 1;
    m_alpha = 1;
    apply();
}

void Pin::fade(int steps) {
    if (!m_alpha_s)
        return;
    m_alpha = std::clamp(m_alpha + 0.1f * static_cast<float>(steps), 0.15f, 1.f);
    apply();
}

void Pin::drag(wl_seat* seat, uint32_t serial) { xdg_toplevel_move(m_toplevel, seat, serial); }
