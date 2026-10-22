// SPDX-License-Identifier: GPL-3.0-or-later
#include "capture.hpp"

#include "ext-image-capture-source-v1.h"
#include "ext-image-copy-capture-v1.h"

#include <chrono>
#include <cstring>
#include <poll.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

Screenshot& Screenshot::operator=(Screenshot&& o) noexcept {
    if (this != &o) {
        free_pixels();
        width  = std::exchange(o.width, 0);
        height = std::exchange(o.height, 0);
        bgr    = o.bgr;
        pixels = std::exchange(o.pixels, nullptr);
        size   = std::exchange(o.size, 0);
    }
    return *this;
}

void Screenshot::free_pixels() {
    if (pixels)
        munmap(pixels, size);
    pixels = nullptr;
    size   = 0;
}

namespace {

struct State {
    uint32_t width = 0, height = 0;
    int32_t  format    = -1;
    bool     constraints_done = false;
    bool     ready     = false;
    bool     failed    = false;
};

bool usable(uint32_t f) {
    return f == WL_SHM_FORMAT_XRGB8888 || f == WL_SHM_FORMAT_ARGB8888 || f == WL_SHM_FORMAT_XBGR8888 ||
        f == WL_SHM_FORMAT_ABGR8888;
}

const ext_image_copy_capture_session_v1_listener session_listener = {
    .buffer_size = [](void* d, ext_image_copy_capture_session_v1*, uint32_t w, uint32_t h) {
        auto* s  = static_cast<State*>(d);
        s->width = w;
        s->height = h;
    },
    .shm_format = [](void* d, ext_image_copy_capture_session_v1*, uint32_t f) {
        auto* s = static_cast<State*>(d);
        // XRGB предпочтительнее: альфа снимку не нужна
        if (usable(f) && (s->format < 0 || f == WL_SHM_FORMAT_XRGB8888))
            s->format = static_cast<int32_t>(f);
    },
    .dmabuf_device = [](void*, ext_image_copy_capture_session_v1*, wl_array*) {},
    .dmabuf_format = [](void*, ext_image_copy_capture_session_v1*, uint32_t, wl_array*) {},
    .done          = [](void* d, ext_image_copy_capture_session_v1*) { static_cast<State*>(d)->constraints_done = true; },
    .stopped       = [](void* d, ext_image_copy_capture_session_v1*) { static_cast<State*>(d)->failed = true; },
};

const ext_image_copy_capture_frame_v1_listener frame_listener = {
    .transform         = [](void*, ext_image_copy_capture_frame_v1*, uint32_t) {},
    .damage            = [](void*, ext_image_copy_capture_frame_v1*, int32_t, int32_t, int32_t, int32_t) {},
    .presentation_time = [](void*, ext_image_copy_capture_frame_v1*, uint32_t, uint32_t, uint32_t) {},
    .ready             = [](void* d, ext_image_copy_capture_frame_v1*) { static_cast<State*>(d)->ready = true; },
    .failed            = [](void* d, ext_image_copy_capture_frame_v1*, uint32_t) { static_cast<State*>(d)->failed = true; },
};

// крутит очередь событий, пока done() не станет true; false — таймаут или обрыв соединения
template <class F>
bool dispatch_until(wl_display* dpy, std::chrono::steady_clock::time_point deadline, F done) {
    while (!done()) {
        if (wl_display_dispatch_pending(dpy) < 0)
            return false;
        if (done())
            return true;
        wl_display_flush(dpy);
        auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        if (left <= 0)
            return false;
        if (wl_display_prepare_read(dpy) != 0)
            continue;
        pollfd pfd{wl_display_get_fd(dpy), POLLIN, 0};
        if (poll(&pfd, 1, static_cast<int>(left)) <= 0) {
            wl_display_cancel_read(dpy);
            return false;
        }
        if (wl_display_read_events(dpy) < 0)
            return false;
    }
    return true;
}

} // namespace

bool capture_output(const CaptureGlobals& g, wl_output* output, Screenshot& out, int timeout_ms) {
    if (!g.display || !g.shm || !g.sources || !g.copy || !output)
        return false;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);

    State st;
    auto* source  = ext_output_image_capture_source_manager_v1_create_source(g.sources, output);
    auto* session = ext_image_copy_capture_manager_v1_create_session(g.copy, source, 0);
    ext_image_copy_capture_session_v1_add_listener(session, &session_listener, &st);

    bool ok = dispatch_until(g.display, deadline, [&] { return st.constraints_done || st.failed; }) && !st.failed &&
        st.format >= 0 && st.width > 0 && st.height > 0 && st.width <= 16384 && st.height <= 16384;

    int              fd   = -1;
    void*            map  = MAP_FAILED;
    size_t           size = 0;
    wl_shm_pool*     pool = nullptr;
    wl_buffer*       buf  = nullptr;
    ext_image_copy_capture_frame_v1* frame = nullptr;

    if (ok) {
        size_t stride = static_cast<size_t>(st.width) * 4;
        size          = stride * st.height;
        fd            = memfd_create("impshot-capture", MFD_CLOEXEC | MFD_ALLOW_SEALING);
        ok            = fd >= 0 && ftruncate(fd, static_cast<off_t>(size)) == 0;
        if (ok)
            map = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        ok = ok && map != MAP_FAILED;
        if (ok) {
            pool = wl_shm_create_pool(g.shm, fd, static_cast<int32_t>(size));
            buf  = wl_shm_pool_create_buffer(pool, 0, static_cast<int32_t>(st.width), static_cast<int32_t>(st.height),
                                             static_cast<int32_t>(stride), static_cast<uint32_t>(st.format));
            frame = ext_image_copy_capture_session_v1_create_frame(session);
            ext_image_copy_capture_frame_v1_add_listener(frame, &frame_listener, &st);
            ext_image_copy_capture_frame_v1_attach_buffer(frame, buf);
            ext_image_copy_capture_frame_v1_damage_buffer(frame, 0, 0, static_cast<int32_t>(st.width),
                                                          static_cast<int32_t>(st.height));
            ext_image_copy_capture_frame_v1_capture(frame);
            ok = dispatch_until(g.display, deadline, [&] { return st.ready || st.failed; }) && st.ready;
        }
        if (ok) {
            out.width  = static_cast<int>(st.width);
            out.height = static_cast<int>(st.height);
            out.bgr    = st.format == WL_SHM_FORMAT_XRGB8888 || st.format == WL_SHM_FORMAT_ARGB8888;
            out.pixels = static_cast<uint8_t*>(map);
            out.size   = size;
            map        = MAP_FAILED; // теперь им владеет снимок
        }
    }

    if (frame)
        ext_image_copy_capture_frame_v1_destroy(frame);
    if (buf)
        wl_buffer_destroy(buf);
    if (pool)
        wl_shm_pool_destroy(pool);
    if (map != MAP_FAILED)
        munmap(map, size);
    if (fd >= 0)
        close(fd);
    ext_image_copy_capture_session_v1_destroy(session);
    ext_image_capture_source_v1_destroy(source);
    wl_display_flush(g.display);
    return ok;
}
