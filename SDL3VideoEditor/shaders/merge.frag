#version 330 core
out vec4 FragColor;

in vec2 v_TexCoord;

uniform sampler2D u_TextureB; // Background Layer
uniform sampler2D u_TextureA; // Foreground Layer

uniform int u_BlendMode;
uniform float u_Mix; // Opacity of the foreground

// Common blend functions
vec3 blend_screen(vec3 base, vec3 blend) {
    return 1.0 - (1.0 - base) * (1.0 - blend);
}

vec3 blend_multiply(vec3 base, vec3 blend) {
    return base * blend;
}

vec3 blend_overlay(vec3 base, vec3 blend) {
    return mix(
        1.0 - 2.0 * (1.0 - base) * (1.0 - blend),
        2.0 * base * blend,
        step(base, vec3(0.5))
    );
}

void main() {
    vec4 base = texture(u_TextureB, v_TexCoord);
    vec4 blend = texture(u_TextureA, v_TexCoord);

    // Apply the mix/opacity to the foreground layer
    blend.a *= u_Mix;

    vec3 result_rgb;
    
    // Switch on the blend mode uniform
    if (u_BlendMode == 1) { // Additive
        result_rgb = base.rgb + blend.rgb;
    } else if (u_BlendMode == 2) { // Multiply
        result_rgb = blend_multiply(base.rgb, blend.rgb);
    } else if (u_BlendMode == 3) { // Screen
        result_rgb = blend_screen(base.rgb, blend.rgb);
    } else if (u_BlendMode == 9) { // Overlay
        result_rgb = blend_overlay(base.rgb, blend.rgb);
    }
    // Add else-if blocks for your other blend modes (Darken, Lighten, etc.)
    else { // Normal
        result_rgb = mix(base.rgb, blend.rgb, blend.a);
    }

    // The final alpha is a simple layering of the foreground over the background
    float final_alpha = blend.a + base.a * (1.0 - blend.a);

    FragColor = vec4(result_rgb, final_alpha);
}