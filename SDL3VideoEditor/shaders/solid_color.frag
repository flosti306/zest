// shaders/solid_color.frag
#version 330 core
out vec4 FragColor;

in vec2 v_TexCoord;

uniform sampler2D u_OriginalTexture; // The input image/video frame
uniform vec4 u_SolidColor;           // The solid color to apply (RGBA)
uniform float u_BlendWithOriginal;   // 0.0 = solid color, 1.0 = original, 0.5 = 50/50 mix

void main() {
    vec4 original_color = texture(u_OriginalTexture, v_TexCoord);
    
    // The color to output is a mix of the solid color and the original texture
    // The solid color's alpha component determines its own transparency
    vec4 output_color_if_solid_only = u_SolidColor;

    // Blend the solid color (respecting its alpha) with the original image
    FragColor = mix(output_color_if_solid_only, original_color, u_BlendWithOriginal);
    
    // If you want the solid color to simply overlay and its alpha to blend with what's beneath it:
    // vec4 final_solid_color = vec4(u_SolidColor.rgb * u_SolidColor.a, u_SolidColor.a);
    // FragColor = mix(final_solid_color, original_color, u_BlendWithOriginal);
    // If u_BlendWithOriginal is 0, this is like alpha blending 'final_solid_color' over a transparent bg
    // then taking that result. A more common overlay is:
    // FragColor = mix(original_color, u_SolidColor, u_SolidColor.a * (1.0 - u_BlendWithOriginal));
    // Let's stick to the simpler first mix, it's more intuitive for "blend with original"
}