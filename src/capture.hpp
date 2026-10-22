#pragma once
// Снимок выхода через ext-image-copy-capture: shm-буфер, без кодирования — единицы миллисекунд.
#include <cstdint>
#include <cstddef>
#include <utility>

struct wl_display;
struct wl_shm;
struct wl_output;
struct ext_output_image_capture_source_manager_v1;
struct ext_image_copy_capture_manager_v1;

// пиксели — прямо shm-отображение, в которое писал композитор: без лишней копии 20+ МБ
struct Screenshot {
    int      width = 0, height = 0;
    bool     bgr    = true;    // порядок байт B,G,R,A (XRGB8888) или R,G,B,A (XBGR8888)
    uint8_t* pixels = nullptr; // width*height*4, строки сверху вниз; после загрузки в GPU не нужны
    size_t   size   = 0;

    Screenshot() = default;
    Screenshot(Screenshot&& o) noexcept { *this = std::move(o); }
    Screenshot& operator=(Screenshot&& o) noexcept;
    Screenshot(const Screenshot&)            = delete;
    Screenshot& operator=(const Screenshot&) = delete;
    ~Screenshot() { free_pixels(); }

    bool empty() const { return width == 0; }
    void free_pixels();
};

struct CaptureGlobals {
    wl_display*                                 display = nullptr;
    wl_shm*                                     shm     = nullptr;
    ext_output_image_capture_source_manager_v1* sources = nullptr;
    ext_image_copy_capture_manager_v1*          copy    = nullptr;
};

// синхронно; false — протокол недоступен, отказ или таймаут
bool capture_output(const CaptureGlobals& g, wl_output* output, Screenshot& out, int timeout_ms = 800);
