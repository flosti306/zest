#version 330 core
layout (location = 0) in vec2 a_Pos;       // Vertex position (e.g., -1 to 1)
layout (location = 1) in vec2 a_TexCoord;  // Texture coordinate (e.g., 0 to 1)

out vec2 v_TexCoord;

void main() {
    gl_Position = vec4(a_Pos.x, a_Pos.y, 0.0, 1.0);
    v_TexCoord = a_TexCoord; // Pass through texture coordinates
}