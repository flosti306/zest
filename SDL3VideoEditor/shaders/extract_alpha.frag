#version 330 core
out vec4 FragColor;

in vec2 TexCoords;

uniform sampler2D u_InputTexture;

void main() {
    float alpha = texture(u_InputTexture, TexCoords).a;
    FragColor = vec4(alpha, alpha, alpha, 1.0);
}