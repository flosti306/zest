#version 330 core
out vec4 FragColor;

// Input interpolated texture coordinate from the vertex shader
in vec2 v_TexCoord;

// The texture we want to sample from
uniform sampler2D u_Texture;

void main()
{
    // Sample the texture at the given coordinate and output its color
    FragColor = texture(u_Texture, v_TexCoord);
}