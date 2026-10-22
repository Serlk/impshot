// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <string>
#include <xkbcommon/xkbcommon.h>

struct KeyEvent {
    xkb_keysym_t sym   = XKB_KEY_NoSymbol; // без учёта раскладки (латиница) — для хоткеев
    xkb_keysym_t raw   = XKB_KEY_NoSymbol; // как напечатано — для текста
    std::string  utf8;
    bool         ctrl = false, shift = false, alt = false;
};

class Keyboard {
public:
    Keyboard();
    ~Keyboard();
    Keyboard(const Keyboard&)            = delete;
    Keyboard& operator=(const Keyboard&) = delete;

    bool     load_keymap(int fd, uint32_t size); // fd закрывает сам
    void     update_modifiers(uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group);
    KeyEvent translate(uint32_t key) const;      // key — evdev-код
    bool     repeats(uint32_t key) const;
    bool     shift() const;
    bool     ctrl() const;

private:
    xkb_context* m_ctx    = nullptr;
    xkb_keymap*  m_keymap = nullptr;
    xkb_state*   m_state  = nullptr;
};
