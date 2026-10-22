// SPDX-License-Identifier: GPL-3.0-or-later
#include "hypr.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <nlohmann/json.hpp>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

std::string request(const std::string& cmd) {
    const char* rt  = getenv("XDG_RUNTIME_DIR");
    const char* sig = getenv("HYPRLAND_INSTANCE_SIGNATURE");
    if (!rt || !sig)
        return {};
    sockaddr_un addr{};
    addr.sun_family  = AF_UNIX;
    std::string path = std::string(rt) + "/hypr/" + sig + "/.socket.sock";
    if (path.size() >= sizeof addr.sun_path)
        return {};
    std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return {};
    std::string out;
    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0 &&
        write(fd, cmd.data(), cmd.size()) == static_cast<ssize_t>(cmd.size())) {
        char buf[65536];
        for (;;) {
            pollfd p{fd, POLLIN, 0};
            if (poll(&p, 1, 300) <= 0)
                break;
            ssize_t n = read(fd, buf, sizeof buf);
            if (n <= 0)
                break;
            out.append(buf, static_cast<size_t>(n));
        }
    }
    close(fd);
    return out;
}

} // namespace

std::vector<WinRect> hypr_windows() {
    std::vector<WinRect> res;
    try {
        auto mons    = nlohmann::json::parse(request("j/monitors"));
        auto clients = nlohmann::json::parse(request("j/clients"));
        std::vector<int>         active;
        std::vector<std::string> special;
        struct Mon {
            int         id, x, y;
            std::string name;
        };
        std::vector<Mon> mon;
        for (auto& m : mons) {
            mon.push_back({m.value("id", -1), m.value("x", 0), m.value("y", 0), m.value("name", "")});
            active.push_back(m.value("activeWorkspace", nlohmann::json::object()).value("id", -9999));
            special.push_back(m.value("specialWorkspace", nlohmann::json::object()).value("name", ""));
        }
        for (auto& c : clients) {
            if (!c.value("mapped", false) || c.value("hidden", false))
                continue;
            auto ws     = c.value("workspace", nlohmann::json::object());
            int  wsid   = ws.value("id", -1);
            auto wsname = ws.value("name", std::string{});
            bool pinned = c.value("pinned", false);
            bool vis    = pinned || std::ranges::find(active, wsid) != active.end() ||
                (!wsname.empty() && std::ranges::find(special, wsname) != special.end());
            if (!vis)
                continue;
            int  mid = c.value("monitor", -1);
            auto mi  = std::ranges::find(mon, mid, &Mon::id);
            if (mi == mon.end())
                continue;
            WinRect r;
            r.monitor = mi->name;
            r.x       = c["at"][0].get<int>() - mi->x;
            r.y       = c["at"][1].get<int>() - mi->y;
            r.w       = c["size"][0];
            r.h       = c["size"][1];
            r.layer   = (pinned || c.value("fullscreen", 0) != 0) ? 2 : c.value("floating", false) ? 1 : 0;
            r.history = c.value("focusHistoryID", 1000);
            r.title   = c.value("title", "");
            if (r.w > 0 && r.h > 0)
                res.push_back(std::move(r));
        }
    } catch (const std::exception&) {
        return {};
    }
    // сверху вниз: сначала то, что вероятнее нарисовано поверх
    std::ranges::sort(res, [](const WinRect& a, const WinRect& b) {
        return a.layer != b.layer ? a.layer > b.layer : a.history < b.history;
    });
    return res;
}
