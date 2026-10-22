#version 300 es
precision highp float;
// Кадр: снимок + размытие/пикселизация по маскам + аннотации, вне выделения — затемнение
// с зелёной развёрткой, рамка с ореолом, ручки, перекрестие и лупа. В режиме экспорта —
// только содержимое выделения, без интерфейса.
uniform sampler2D uBase, uBlur, uMaskB, uMaskP, uAnno, uLive;
uniform vec2  uSize;     // размер выхода, px
uniform int   uLiveMode; // 0 — живая фигура рисуется, 1 — это маска размытия, 2 — пикселизации
uniform float uBlock;    // размер блока пикселизации
uniform int   uExport;
uniform vec2  uOffset;   // экспорт: левый верхний угол выделения
uniform float uExportScale; // экспорт с увеличением (для OCR)
uniform vec4  uSel;      // выделение x, y, w, h
uniform int   uHasSel;
uniform vec4  uHover;    // окно под курсором до выделения
uniform int   uHasHover;
uniform int   uHandles;
uniform vec2  uCursor;
uniform int   uCross;    // перекрестие до выделения
uniform vec3  uLoupe;    // центр лупы и радиус; z = 0 — нет лупы
uniform float uZoom;
uniform float uUi;       // масштаб интерфейса
uniform float uScan;     // полоса сканера внутри выделения при распознавании; < 0 — нет
uniform vec3  uRing;     // контур кисти у курсора: центр и радиус; z = 0 — нет
out vec4 frag;

const vec3 PHOS = vec3(0.20, 1.00, 0.42);

vec3 scene(vec2 px) {
    vec2 uv = px / uSize;
    vec3 c  = texture(uBase, uv).rgb;
    vec4 live = texture(uLive, uv);
    float mb = texture(uMaskB, uv).r;
    float mp = texture(uMaskP, uv).r;
    if (uLiveMode == 1) mb = max(mb, live.a);
    if (uLiveMode == 2) mp = max(mp, live.a);
    if (mb > 0.0) c = mix(c, texture(uBlur, uv).rgb, mb);
    if (mp > 0.0) {
        vec2 cell = (floor(px / uBlock) + 0.5) * uBlock;
        c = mix(c, texture(uBase, cell / uSize).rgb, mp);
    }
    vec4 a = texture(uAnno, uv);
    c = a.rgb + c * (1.0 - a.a);
    if (uLiveMode == 0) c = live.rgb + c * (1.0 - live.a);
    return c;
}

// знаковое расстояние до границы прямоугольника: < 0 внутри
float sdRect(vec2 p, vec4 r) {
    vec2 c = r.xy + r.zw * 0.5;
    vec2 d = abs(p - c) - r.zw * 0.5;
    return length(max(d, 0.0)) + min(max(d.x, d.y), 0.0);
}

void main() {
    if (uExport == 1) {
        frag = vec4(scene(uOffset + gl_FragCoord.xy / uExportScale), 1.0);
        return;
    }
    vec2 px = vec2(gl_FragCoord.x, uSize.y - gl_FragCoord.y);
    vec3 c  = scene(px);

    vec4  r      = uHasSel == 1 ? uSel : uHover;
    bool  hasR   = uHasSel == 1 || uHasHover == 1;
    float d      = hasR ? sdRect(px, r) : 1e9;
    bool  inside = d < 0.0;

    if (!inside) {
        // затемнение с фосфорным оттенком и строками развёртки
        float l   = dot(c, vec3(0.299, 0.587, 0.114));
        c         = mix(c * 0.32, PHOS * l * 0.42, 0.45);
        float row = mod(floor(px.y), 3.0);
        c *= row < 1.0 ? 0.72 : 1.0;
    }

    if (hasR) {
        float lw  = (uHasSel == 1 ? 1.6 : 1.1) * uUi;
        float edge = 1.0 - smoothstep(lw - 0.5, lw + 0.5, abs(d + lw * 0.5));
        float glow = d > 0.0 ? exp(-d / (7.0 * uUi)) * (uHasSel == 1 ? 0.38 : 0.22) : 0.0;
        c = mix(c, PHOS, edge * (uHasSel == 1 ? 1.0 : 0.8));
        c += PHOS * glow;

        if (uHandles == 1) {
            float hs = 4.5 * uUi;
            for (int i = 0; i < 9; i++) {
                if (i == 4) continue;
                vec2 hp = r.xy + r.zw * vec2(float(i % 3), float(i / 3)) * 0.5;
                vec2 q  = abs(px - hp);
                float m = max(q.x, q.y);
                if (m < hs) c = m < hs - 1.5 * uUi ? vec3(0.02, 0.08, 0.04) : PHOS;
            }
        }
    }

    if (uCross == 1) {
        vec2 q = abs(px - uCursor);
        float line = (q.x < 0.5 * uUi || q.y < 0.5 * uUi) ? 1.0 : 0.0;
        // пунктир, чтобы линия не путалась с содержимым
        float dash = mod(floor((q.x < 0.5 * uUi ? px.y : px.x) / (4.0 * uUi)), 2.0);
        c = mix(c, PHOS, line * dash * 0.55);
    }

    if (uScan >= 0.0 && inside) {
        float ds = abs(px.y - uScan);
        c = mix(c, PHOS, (1.0 - smoothstep(0.5 * uUi, 1.8 * uUi, ds)) * 0.9);
        c += PHOS * exp(-ds / (14.0 * uUi)) * 0.22;
    }

    if (uRing.z > 0.0) {
        float dr = abs(length(px - uRing.xy) - uRing.z);
        c = mix(c, vec3(0.0), (1.0 - smoothstep(1.0, 2.5, dr)) * 0.5);
        c = mix(c, PHOS, 1.0 - smoothstep(0.3, 1.2, dr));
    }

    if (uLoupe.z > 0.0) {
        float dl = length(px - uLoupe.xy);
        if (dl < uLoupe.z) {
            vec2  rel  = (px - uLoupe.xy) / uZoom;
            vec2  src  = floor(uCursor) + 0.5 + rel;
            vec3  m    = texelFetch(uBase, ivec2(clamp(floor(src), vec2(0.0), uSize - 1.0)), 0).rgb;
            vec2  f    = fract(src);
            float grid = (min(f.x, f.y) * uZoom < 1.0) ? 1.0 : 0.0;
            m = mix(m, m * 0.6 + PHOS * 0.12, grid * 0.6);
            vec2 cp = abs(floor(src) - floor(uCursor));
            if (cp.x + cp.y < 0.5) {
                vec2 e = min(f, 1.0 - f) * uZoom;
                if (min(e.x, e.y) < 1.5 * uUi) m = PHOS;
            }
            float scan = mod(floor(px.y), 3.0) < 1.0 ? 0.9 : 1.0;
            c = m * scan;
        }
        float ring = 1.0 - smoothstep(1.0 * uUi, 2.2 * uUi, abs(dl - uLoupe.z));
        c = mix(c, PHOS, ring);
        c += PHOS * (dl > uLoupe.z ? exp(-(dl - uLoupe.z) / (6.0 * uUi)) * 0.3 : 0.0);
    }
    frag = vec4(c, 1.0);
}
