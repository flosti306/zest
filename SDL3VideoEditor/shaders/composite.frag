#version 330 core
out vec4 FragColor;

in vec2 TexCoords;

uniform sampler2D u_Texture;
uniform float u_Opacity;
uniform int u_BlendMode; // 0:Normal, 1:Add, 2:Multiply, 3:Screen, etc.

void main()
{
    vec4 texColor = texture(u_Texture, TexCoords);
    
    // This is where we would blend with the background if we were
    // doing multi-pass rendering. For single-pass, we just apply opacity.
    // The FBO's blend function will handle the composition.
    
    FragColor = vec4(texColor.rgb, texColor.a * u_Opacity);
}