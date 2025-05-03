#version 330 core
out vec4 FragColor;

in vec2 TexCoord;

uniform sampler2D uTexture; // Input texture from previous step
uniform vec4 uColor;      // Overall opacity/tint for the effect result

void main() {
    vec4 texColor = texture(uTexture, TexCoord);
    float gray = dot(texColor.rgb, vec3(0.299, 0.587, 0.114));
    FragColor = vec4(gray, gray, gray, texColor.a) * uColor;
}