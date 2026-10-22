// SPDX-License-Identifier: GPL-3.0-or-later
#include "keyboard.hpp"

#include <cstring>
#include <sys/mman.h>
#include <unistd.h>

Keyboard::Keyboard() : m_ctx(xkb_context_new(XKB_CONTEXT_NO_FLAGS)) {}

Keyboard::~Keyboard() {
    xkb_state_unref(m_state);
    xkb_keymap_unref(m_keymap);
    xkb_context_unref(m_ctx);
}

bool Keyboard::load_keymap(int fd, uint32_t size) {
    void* map = size ? mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0) : MAP_FAILED;
    close(fd);
    if (map == MAP_FAILED || !m_ctx)
        return false;
    const char* text = static_cast<const char*>(map);
    xkb_keymap* km   = nullptr;
    if (memchr(text, '\0', size))
        km = xkb_keymap_new_from_string(m_ctx, text, XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
    munmap(map, size);
    if (!km)
        return false;
    xkb_state* st = xkb_state_new(km);
    if (!st) {
        xkb_keymap_unref(km);
        return false;
    }
    xkb_state_unref(m_state);
    xkb_keymap_unref(m_keymap);
    m_keymap = km;
    m_state  = st;
    return true;
}

void Keyboard::update_modifiers(uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group) {
    if (m_state)
        xkb_state_update_mask(m_state, depressed, latched, locked, 0, 0, group);
}

bool Keyboard::shift() const {
    return m_state && xkb_state_mod_name_is_active(m_state, XKB_MOD_NAME_SHIFT, XKB_STATE_MODS_EFFECTIVE) > 0;
}

bool Keyboard::ctrl() const {
    return m_state && xkb_state_mod_name_is_active(m_state, XKB_MOD_NAME_CTRL, XKB_STATE_MODS_EFFECTIVE) > 0;
}

KeyEvent Keyboard::translate(uint32_t key) const {
    KeyEvent ev;
    if (!m_state)
        return ev;
    xkb_keycode_t code = key + 8;
    ev.raw   = xkb_state_key_get_one_sym(m_state, code);
    ev.ctrl  = ctrl();
    ev.shift = shift();
    ev.alt   = xkb_state_mod_name_is_active(m_state, XKB_MOD_NAME_ALT, XKB_STATE_MODS_EFFECTIVE) > 0;

    // хоткеи работают и в русской раскладке: берём символ из первой (латинской) группы
    ev.sym = ev.raw;
    const xkb_keysym_t* syms = nullptr;
    if (xkb_keymap_key_get_syms_by_level(m_keymap, code, 0, 0, &syms) > 0 && syms[0] != XKB_KEY_NoSymbol)
        ev.sym = xkb_keysym_to_lower(syms[0]);
    if (ev.raw >= XKB_KEY_KP_Space && ev.raw <= XKB_KEY_KP_9) // цифровой блок и спецклавиши — как есть
        ev.sym = ev.raw;

    char buf[64]{};
    int  n = xkb_state_key_get_utf8(m_state, code, buf, sizeof buf);
    if (n > 0 && n < static_cast<int>(sizeof buf)) {
        bool printable = true;
        for (int i = 0; i < n; i++) {
            unsigned char c = static_cast<unsigned char>(buf[i]);
            printable       = printable && c >= 0x20 && c != 0x7F;
        }
        if (printable)
            ev.utf8.assign(buf, static_cast<size_t>(n));
    }
    return ev;
}

bool Keyboard::repeats(uint32_t key) const { return m_keymap && xkb_keymap_key_repeats(m_keymap, key + 8); }
