// SPDX-License-Identifier: GPL-3.0-or-later
#include "export.hpp"

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <ctime>
#include <fcntl.h>
#include <filesystem>
#include <png.h>
#include <spawn.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

extern char** environ;

namespace {

void png_write(png_structp p, png_bytep data, png_size_t len) {
    auto* out = static_cast<std::vector<uint8_t>*>(png_get_io_ptr(p));
    out->insert(out->end(), data, data + len);
}

bool write_all(int fd, const uint8_t* p, size_t n) {
    while (n) {
        ssize_t k = write(fd, p, n);
        if (k < 0 && errno == EINTR)
            continue;
        if (k <= 0)
            return false;
        p += k;
        n -= static_cast<size_t>(k);
    }
    return true;
}

// запуск без шелла; stdin_fd >= 0 — подключить к stdin, иначе /dev/null; stdout_fd — аналогично
pid_t spawn(const std::vector<std::string>& args, int stdin_fd, int stdout_fd = -1) {
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    if (stdin_fd >= 0)
        posix_spawn_file_actions_adddup2(&fa, stdin_fd, 0);
    else
        posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    if (stdout_fd >= 0)
        posix_spawn_file_actions_adddup2(&fa, stdout_fd, 1);
    else
        posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    std::vector<char*> argv;
    for (auto& a : args)
        argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    pid_t pid = -1;
    if (posix_spawnp(&pid, argv[0], &fa, nullptr, argv.data(), environ) != 0)
        pid = -1;
    posix_spawn_file_actions_destroy(&fa);
    return pid;
}

} // namespace

bool encode_png(const std::vector<uint8_t>& rgba, int w, int h, std::vector<uint8_t>& out) {
    png_structp p = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    png_infop   i = p ? png_create_info_struct(p) : nullptr;
    if (!i) {
        png_destroy_write_struct(&p, nullptr);
        return false;
    }
    out.clear();
    out.reserve(rgba.size() / 3);
    if (setjmp(png_jmpbuf(p))) {
        png_destroy_write_struct(&p, &i);
        return false;
    }
    png_set_write_fn(p, &out, png_write, nullptr);
    // уровень 1 + фильтр Sub: в разы быстрее дефолта, для скриншотов размер почти тот же
    png_set_compression_level(p, 1);
    png_set_filter(p, 0, PNG_FILTER_SUB);
    png_set_IHDR(p, i, static_cast<png_uint_32>(w), static_cast<png_uint_32>(h), 8, PNG_COLOR_TYPE_RGB, PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_write_info(p, i);
    png_set_filler(p, 0, PNG_FILLER_AFTER); // строки RGBA, четвёртый байт отбрасывается
    for (int y = 0; y < h; y++)
        png_write_row(p, const_cast<png_bytep>(rgba.data() + static_cast<size_t>(y) * w * 4));
    png_write_end(p, nullptr);
    png_destroy_write_struct(&p, &i);
    return true;
}

bool copy_to_clipboard(const std::vector<uint8_t>& png, const char* mime, bool primary) {
    int fds[2];
    if (pipe2(fds, O_CLOEXEC) != 0)
        return false;
    // wl-copy сам уходит в фон и держит буфер обмена, пока его не перезапишут
    std::vector<std::string> args{"wl-copy", "--type", mime};
    if (primary)
        args.push_back("--primary");
    pid_t pid = spawn(args, fds[0]);
    close(fds[0]);
    if (pid < 0) {
        close(fds[1]);
        return false;
    }
    signal(SIGPIPE, SIG_IGN);
    bool ok = write_all(fds[1], png.data(), png.size());
    close(fds[1]);
    int st = 0;
    waitpid(pid, &st, 0);
    return ok && WIFEXITED(st) && WEXITSTATUS(st) == 0;
}

bool run_filter(const std::vector<std::string>& args, const std::vector<uint8_t>& input, std::string& out) {
    int in[2], outp[2];
    if (pipe2(in, O_CLOEXEC) != 0)
        return false;
    if (pipe2(outp, O_CLOEXEC) != 0) {
        close(in[0]);
        close(in[1]);
        return false;
    }
    pid_t pid = spawn(args, in[0], outp[1]);
    close(in[0]);
    close(outp[1]);
    if (pid < 0) {
        close(in[1]);
        close(outp[0]);
        return false;
    }
    signal(SIGPIPE, SIG_IGN);
    // писать и читать одновременно: фильтр может начать отвечать, не дочитав вход
    std::thread writer([&] {
        write_all(in[1], input.data(), input.size());
        close(in[1]);
    });
    char buf[65536];
    for (;;) {
        ssize_t n = read(outp[0], buf, sizeof buf);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        out.append(buf, static_cast<size_t>(n));
    }
    close(outp[0]);
    writer.join();
    int st = 0;
    waitpid(pid, &st, 0);
    return WIFEXITED(st) && WEXITSTATUS(st) == 0;
}

std::string save_png(const std::vector<uint8_t>& png, const std::string& dir) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    char      name[64];
    time_t    t = time(nullptr);
    struct tm tm{};
    localtime_r(&t, &tm);
    strftime(name, sizeof name, "%Y-%m-%d_%H-%M-%S", &tm);
    std::string path = dir + "/" + name + ".png";
    for (int n = 2; std::filesystem::exists(path); n++)
        path = dir + "/" + name + "_" + std::to_string(n) + ".png";

    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (fd < 0)
        return {};
    bool ok = write_all(fd, png.data(), png.size());
    ok      = close(fd) == 0 && ok;
    if (!ok) {
        unlink(path.c_str());
        return {};
    }
    return path;
}

void notify(const std::string& summary, const std::string& body, const std::string& icon_path) {
    std::vector<std::string> args{"notify-send", "-a", "impshot", "-t", "3000"};
    if (!icon_path.empty())
        args.insert(args.end(), {"-i", icon_path});
    args.push_back(summary);
    args.push_back(body);
    pid_t pid = spawn(args, -1);
    if (pid > 0)
        waitpid(pid, nullptr, 0);
}
