#version 330 core
layout (location = 0) in vec3 aPos;
layout (location = 1) in vec2 aTexCoords;

out vec2 TexCoords;

uniform mat4 u_Transform; // For position, scale, rotation

void main()
{
    TexCoords = aTexCoords;
    // Apply transform to standard quad vertices
    gl_Position = u_Transform * vec4(aPos.xy, 0.0, 1.0);
}