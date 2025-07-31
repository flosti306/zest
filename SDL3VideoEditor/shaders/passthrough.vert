#version 330 core

// Input vertex data from the VBO
// Location 0 = vertex position
// Location 1 = texture coordinate
layout (location = 0) in vec2 a_Position;
layout (location = 1) in vec2 a_TexCoord;

// Output variable that will be interpolated and sent to the fragment shader
out vec2 v_TexCoord;

void main()
{
    // Set the final position of the vertex on the screen
    gl_Position = vec4(a_Position.x, a_Position.y, 0.0, 1.0);

    // Pass the texture coordinate to the fragment shader.
    // The GPU will automatically interpolate this value for each pixel.
    v_TexCoord = a_TexCoord;
}