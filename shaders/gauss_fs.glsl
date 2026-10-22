#version 300 es
precision highp float;
// разделимый гаусс, 9 отводов через линейную фильтрацию (σ≈4 текселя)
in vec2 vUv;
uniform sampler2D uTex;
uniform vec2 uDir; // (1/w, 0) или (0, 1/h)
out vec4 frag;
void main() {
    const float w0 = 0.1964825501511404;
    const vec4  wv = vec4(0.2969069646728344, 0.09447039785044732, 0.010381362401148057, 0.0);
    const vec4  of = vec4(1.411764705882353, 3.2941176470588234, 5.176470588235294, 0.0);
    vec3 c = texture(uTex, vUv).rgb * w0;
    for (int i = 0; i < 3; i++) {
        c += texture(uTex, vUv + uDir * of[i] * 1.6).rgb * wv[i];
        c += texture(uTex, vUv - uDir * of[i] * 1.6).rgb * wv[i];
    }
    frag = vec4(c, 1.0);
}
