#version 330 core
out vec4 FragColor;

in vec2 TexCoords;

uniform sampler2D u_Texture;
uniform float u_Opacity;

void main()
{
    vec4 texColor = texture(u_Texture, TexCoords);
    FragColor = vec4(texColor.rgb, texColor.a * u_Opacity);
}