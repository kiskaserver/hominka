#version 450
// Малюємо квадрат на весь viewport (його ми ставимо в прямокутник чату).
// Позиції й UV рахуємо з індексу вершини — вершинного буфера не треба.
// Топологія — triangle strip, 4 вершини.
layout(location = 0) out vec2 vUV;
void main() {
    vec2 uv = vec2((gl_VertexIndex == 1 || gl_VertexIndex == 3) ? 1.0 : 0.0,
                   (gl_VertexIndex == 2 || gl_VertexIndex == 3) ? 1.0 : 0.0);
    vUV = uv;
    // NDC Vulkan: y вниз, (-1,-1) — лівий верхній кут viewport. UV(0,0) —
    // верхній рядок текстури (QImage згори вниз), тож збігається.
    gl_Position = vec4(uv.x * 2.0 - 1.0, uv.y * 2.0 - 1.0, 0.0, 1.0);
}
