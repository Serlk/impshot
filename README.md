# impshot

Region screenshots for Hyprland/Wayland in a CRT-phosphor style: instant overlay, annotations, PNG to the clipboard or a file.

## Build

    cmake -S . -B build -G Ninja && ninja -C build && cmake --install build --prefix ~/.local

### Dependencies (Arch)

    sudo pacman -S --needed cmake ninja gcc pkgconf wayland wayland-protocols libglvnd mesa libxkbcommon \
        cairo pango libpng nlohmann-json zxing-cpp wl-clipboard tesseract tesseract-data-eng tesseract-data-rus libnotify

Build: CMake ≥ 3.25 + Ninja, a C++23 compiler, `wayland-scanner` (from `wayland`), `wayland-protocols`;
libraries — wayland-client/egl, EGL + GLESv3 (`libglvnd` with a Mesa driver), xkbcommon, cairo, pango, libpng,
nlohmann-json (header-only), zxing-cpp. The wlr-layer-shell protocol XML is vendored in `protocols/`.

Runtime:

| what | needed for |
|---|---|
| Wayland compositor with `wlr-layer-shell` and `ext-image-copy-capture` (Hyprland) | the overlay and the capture itself |
| `wl-copy` (wl-clipboard) | clipboard and primary selection |
| `tesseract` + `tesseract-data-*` for the languages in `ocr_lang` | select text, `O` |
| `notify-send` (libnotify) | save / OCR / QR notifications (optional) |
| `hyprctl` and the Hyprland IPC socket | click-to-select-window and placing pinned screenshots (optional; without Hyprland these features are skipped) |

### Hyprland config

```lua
local impshot = os.getenv("HOME") .. "/.local/bin/impshot"
hl.exec_cmd(impshot .. " --daemon")              -- in the autostart block
hl.bind("Print", hl.dsp.exec_cmd(impshot))
-- overlay must appear instantly, without the fade-in
hl.layer_rule({ name = "impshot-layer", match = { namespace = "^(impshot)$" }, no_anim = true })
-- pinned screenshots float on top of everything and draw their own frame
hl.window_rule({ name = "impshot-pin", match = { class = "^(impshot-pin)$" },
                 float = true, pin = true, border_size = 0, no_anim = true, no_shadow = true, rounding = 0 })
```

## Running

- `impshot --daemon` keeps Wayland and EGL ready (autostarted from hyprland.lua).
- `impshot` uses the daemon if it is running (~30 ms to the first frame), otherwise runs on its own (~85 ms).
- After changes: `pkill -x impshot`, reinstall, start `impshot --daemon` again.

## Toolbar

`select · pen▾ · arrow · shapes▾ · text · counter · blur▾ | style▾ | ↶ ↷ | select text · QR · pin | copy · save · quit`

Buttons with a corner mark are groups: the first click picks the tool, clicking it again opens its options
(pen: pen/marker; shapes: rectangle/ellipse/line; blur: area/brush × blur/pixelate).
"Style" holds color, width and fill. Clicking outside a menu or right-clicking closes it.

**Select text** recognizes the selection (it starts as soon as you hover the button) and highlights the words.
Drag over text and that piece is copied right away (double-click takes a line). Hold Ctrl to select
several pieces (Ctrl+A selects everything); they are copied when you release Ctrl, in screen order, one per line.
Like elsewhere in Linux, text selected with the mouse goes to the primary selection (paste with a middle click);
Ctrl+C / Enter and "All text" copy to the regular clipboard. `select_to` in the config changes this. Hyprland
ignores primary-selection changes while the overlay has keyboard focus, so primary is written right after
the overlay closes. "Done" goes back to drawing. Esc closes the screenshot right away in
any mode (except while typing a text annotation, where Esc cancels that annotation).

## Controls

| | |
|---|---|
| drag | select an area |
| click | window under the cursor (no window — whole screen) |
| Ctrl+A | whole monitor |
| handles / edges / arrow keys | resize and move the selection (Shift — ×10, Alt+arrows — resize) |
| V P L A R E M T N B X | select, pen, line, arrow, rectangle, ellipse, marker, text, counter, blur, pixelate |
| B / X again | blur and pixelate: brush ↔ area (the brush outline follows the cursor) |
| 1–8 | color |
| wheel, `[` `]` | width / text size |
| F | fill (rectangle, ellipse, text background) |
| Shift while drawing | lines in 15° steps, square/circle |
| right click | cancel current → leave tool → drop selection |
| Ctrl+Z / Ctrl+Shift+Z | undo / redo |
| Enter, Ctrl+C, double click, middle click | copy to clipboard |
| Ctrl+S | save to `~/Pictures/Screenshots` (and copy) |
| O | copy all recognized text at once |
| Q | read QR/barcodes (zxing-cpp) → clipboard |
| Ctrl+P | pin the screenshot on top of windows, in place |
| Esc | quit |

Text annotation: Enter — done, Shift+Enter — new line, Esc — cancel.

Pinned screenshot: drag with the left button, wheel — zoom, Ctrl+wheel — opacity, `+`/`-`/`0` — zoom/reset,
Ctrl+C / Ctrl+S — copy/save, Esc, double click or middle click — close. Hyprland rule `impshot-pin`
(float + pin); the position comes from a dynamic rule `impshot-pin-place` set via `hyprctl eval`.

OCR runs on the clean screenshot without annotations. The area is cut into bands at the calmest rows
(between text lines) and the bands go through tesseract in parallel (~1.5 s for 1728×1080 of dense text).
The NPU is not used: only VLMs via FastFlowLM are available there, which are slower and can invent text.

## Config

`~/.config/impshot/config`, lines `key = value`: `save_dir`, `font`, `ui_scale`, `width`, `color` (1–8), `copy_on_save`, `notify`, `ocr_lang`, `select_to` (`primary` — default, `clipboard`, `both`: where text selected with the mouse goes).

## Checking the look

`impshot --standalone --demo 1.5` shows a test scene; `IMPSHOT_DEMO_FINISH=save|copy|ocr|qr|pin` runs a finish path without the mouse, `IMPSHOT_DEMO_UI=menu|text` shows the style menu / text mode.
