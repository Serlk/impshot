// impshot — скриншот области для Wayland (Hyprland): мгновенный оверлей, аннотации, PNG в буфер/файл.
#include "app.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

namespace {

void usage() {
    std::puts("usage: impshot [options]\n"
              "  --save-dir DIR   where Ctrl+S saves (default ~/Pictures/Screenshots)\n"
              "  --font NAME      font for the UI and text annotations\n"
              "  --ui-scale K     UI scale (1.25)\n"
              "  --no-notify      no notification after saving\n"
              "  --daemon         stay resident; later impshot calls capture instantly\n"
              "  --standalone     don't use a running daemon\n"
              "  --timing         print stage timings to stderr\n"
              "  --demo SEC       show a test scene and exit\n"
              "config: ~/.config/impshot/config, lines key = value\n"
              "  save_dir, font, ui_scale, width, color (1-8), copy_on_save, notify, ocr_lang (rus+eng),\n"
              "  select_to (primary | clipboard | both — where text selected with the mouse goes)");
}

std::string trim(std::string s) {
    auto b = s.find_first_not_of(" \t\r");
    auto e = s.find_last_not_of(" \t\r");
    return b == std::string::npos ? "" : s.substr(b, e - b + 1);
}

std::string expand(const std::string& p) {
    const char* home = getenv("HOME");
    if (home && (p == "~" || p.starts_with("~/")))
        return home + p.substr(1);
    return p;
}

bool truthy(const std::string& v) { return v == "1" || v == "true" || v == "yes" || v == "on"; }

void load_config(Options& o) {
    std::string path;
    if (const char* x = getenv("XDG_CONFIG_HOME"); x && *x)
        path = std::string(x) + "/impshot/config";
    else if (const char* h = getenv("HOME"))
        path = std::string(h) + "/.config/impshot/config";
    std::ifstream in(path);
    std::string   line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#')
            continue;
        auto eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        std::string k = trim(line.substr(0, eq)), v = line.substr(eq + 1);
        // комментарий в конце строки: «key = value  # ...»
        if (auto hash = v.find(" #"); hash != std::string::npos)
            v.erase(hash);
        v = trim(v);
        if (v.size() >= 2 && v.front() == '"' && v.back() == '"')
            v = v.substr(1, v.size() - 2);
        if (k == "save_dir")
            o.save_dir = expand(v);
        else if (k == "font")
            o.font = v;
        else if (k == "ui_scale")
            o.ui_scale = std::clamp(std::strtof(v.c_str(), nullptr), 0.5f, 4.f);
        else if (k == "width")
            o.width = std::strtof(v.c_str(), nullptr);
        else if (k == "color")
            o.color = std::atoi(v.c_str()) - 1;
        else if (k == "ocr_lang")
            o.ocr_lang = v;
        else if (k == "select_to" && (v == "primary" || v == "clipboard" || v == "both"))
            o.select_to = v;
        else if (k == "copy_on_save")
            o.copy_on_save = truthy(v);
        else if (k == "notify")
            o.notify = truthy(v);
    }
}

} // namespace

int main(int argc, char** argv) {
    // параллельность OCR — наша (полосы по процессам); OpenMP внутри tesseract только мешал бы
    setenv("OMP_THREAD_LIMIT", "1", 0);
    Options o;
    bool    standalone = false;
    load_config(o);
    for (int i = 1; i < argc; i++) {
        std::string a    = argv[i];
        auto        next = [&]() -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "impshot: %s needs a value\n", a.c_str());
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--save-dir")
            o.save_dir = expand(next());
        else if (a == "--font")
            o.font = next();
        else if (a == "--ui-scale")
            o.ui_scale = std::clamp(std::strtof(next(), nullptr), 0.5f, 4.f);
        else if (a == "--no-notify")
            o.notify = false;
        else if (a == "--daemon")
            o.daemon = true;
        else if (a == "--standalone")
            standalone = true;
        else if (a == "--timing")
            o.timing = true;
        else if (a == "--demo")
            o.demo = std::max(0.1, std::strtod(next(), nullptr));
        else if (a == "-h" || a == "--help") {
            usage();
            return 0;
        } else if (a == "--version") {
            std::puts("impshot " IMPSHOT_VERSION);
            return 0;
        } else {
            std::fprintf(stderr, "impshot: unknown option %s\n", a.c_str());
            usage();
            return 2;
        }
    }
    // обычный запуск: если демон уже ждёт — снимок делает он, это мгновенно
    if (!o.daemon && !standalone && o.demo <= 0 && App::trigger_daemon())
        return 0;
    App app(std::move(o));
    return app.run();
}
