#version 330 core
in vec2 v_TexCoord;
out vec4 FragColor;

uniform sampler2D u_FontTexture;
uniform vec4 u_TextColor;

// --- NEW UNIFORMS for SDF and Outlines ---
uniform bool u_HasOutline;
uniform vec4 u_OutlineColor;
uniform float u_OutlineThickness; // e.g., 0.1

const float SMOOTHING = 0.05; // Anti-aliasing amount

void main() {
    // The SDF value is the distance from the edge (0.0 to 1.0 range)
    float distance = texture(u_FontTexture, v_TexCoord).r;

    // --- The SDF thresholds ---
    float outline_start = 0.5 - u_OutlineThickness;
    float fill_start = 0.5;

    // --- Calculate anti-aliased alpha values ---
    // Alpha for the outline part
    float outline_alpha = smoothstep(outline_start - SMOOTHING, outline_start + SMOOTHING, distance);
    // Alpha for the inner fill part
    float fill_alpha = smoothstep(fill_start - SMOOTHING, fill_start + SMOOTHING, distance);

    // --- Determine the final color ---
    vec4 final_color;
    if (u_HasOutline) {
        // Blend the outline color with the fill color
        final_color = mix(u_OutlineColor, u_TextColor, fill_alpha);
        // The final alpha is the alpha of the outermost edge (the outline)
        final_color.a *= outline_alpha;
    } else {
        final_color = u_TextColor;
        // The final alpha is just the alpha of the fill
        final_color.a *= fill_alpha;
    }

    FragColor = final_color;
}