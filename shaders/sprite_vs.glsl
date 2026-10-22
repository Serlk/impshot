#version 300 es
// прямоугольник uRect (x, y, w, h в пикселях, ось y вниз) поверх кадра uSize
uniform vec4 uRect;
uniform vec2 uSize;
out vec2 vUv;
void main() {
    vec2 c = vec2(float(gl_VertexID & 1), float((gl_VertexID >> 1) & 1));
    vUv = c;
    vec2 px = uRect.xy + c * uRect.zw;
    gl_Position = vec4(px.x / uSize.x * 2.0 - 1.0, 1.0 - px.y / uSize.y * 2.0, 0.0, 1.0);
}
