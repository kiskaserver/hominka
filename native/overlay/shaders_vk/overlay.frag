#version 450
// Семплимо кадр чату й гасимо загальною прозорістю (push-константа).
// Формат текстури B8G8R8A8_UNORM збігається з байтами QImage (BGRA), тож
// texture() повертає правильний колір без перестановки.
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;
layout(set = 0, binding = 0) uniform sampler2D tex;
layout(push_constant) uniform P { float opacity; } pc;
void main() {
    vec4 c = texture(tex, vUV);
    c.a *= pc.opacity;
    outColor = c;
}
