#version 300 es
precision mediump float;
in vec2 vUv;
uniform sampler2D uTex;
uniform float uAlpha;
out vec4 frag;
void main() {
    vec4 c = texture(uTex, vUv); // cairo: премультиплицированная альфа
    // строчная развёртка кинескопа — едва заметно, текст остаётся чётким
    float scan = mod(floor(gl_FragCoord.y), 3.0) < 1.0 ? 0.88 : 1.0;
    frag = vec4(c.rgb * scan, c.a) * uAlpha;
}
