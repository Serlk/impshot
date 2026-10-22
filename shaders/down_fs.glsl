#version 300 es
precision highp float;
// уменьшение снимка: 4 билинейных выборки = среднее по 4×4 исходных пикселей
in vec2 vUv;
uniform sampler2D uTex;
uniform vec2 uTexel; // 1/размер источника
out vec4 frag;
void main() {
    vec4 s = texture(uTex, vUv + uTexel * vec2(-1.0, -1.0)) + texture(uTex, vUv + uTexel * vec2(1.0, -1.0)) +
        texture(uTex, vUv + uTexel * vec2(-1.0, 1.0)) + texture(uTex, vUv + uTexel * vec2(1.0, 1.0));
    frag = vec4(s.rgb * 0.25, 1.0);
}
