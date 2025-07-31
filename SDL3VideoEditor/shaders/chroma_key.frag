#version 330 core
out vec4 FragColor;

in vec2 v_TexCoord;

uniform sampler2D u_Texture;
uniform vec3 u_KeyColor;
uniform float u_Similarity; // How close a color must be to the key color to be transparent
uniform float u_Blend;      // The softness of the edge
uniform float u_Spill;      // How much of the key color to remove from the edges

// Function to convert RGB to a color space that is better for distance calculations (simplified YCbCr)
vec3 rgb2yuv(vec3 rgb) {
    return vec3(
        0.299 * rgb.r + 0.587 * rgb.g + 0.114 * rgb.b,  // Y (Luma)
        -0.169 * rgb.r - 0.331 * rgb.g + 0.5 * rgb.b,   // Cb
        0.5 * rgb.r - 0.419 * rgb.g - 0.081 * rgb.b     // Cr
    );
}

void main() {
    vec4 source_color = texture(u_Texture, v_TexCoord);

    // Calculate color distance in YUV space for better perceptual results.
    // This makes the keying less sensitive to brightness variations in the background.
    vec3 yuv_source = rgb2yuv(source_color.rgb);
    vec3 yuv_key = rgb2yuv(u_KeyColor);

    // We primarily care about the chroma components (Cb, Cr) for the keying distance.
    float chroma_dist = distance(yuv_source.yz, yuv_key.yz);

    // Calculate the base alpha value using smoothstep for a soft, anti-aliased transition.
    // If the distance is less than (similarity - blend), alpha is 0.
    // If the distance is greater than (similarity + blend), alpha is 1.
    // It smoothly interpolates between these two thresholds.
    float base_alpha = smoothstep(u_Similarity - u_Blend, u_Similarity + u_Blend, chroma_dist);

    // --- Spill Suppression ---
    // This helps remove the "green halo" from the subject's edges.
    // Calculate how much spill to remove. This is strongest on pixels that are
    // almost fully keyed out (where base_alpha is close to 0).
    float spill_amount = (1.0 - base_alpha) * u_Spill;

    // A simple way to despill green is to average the red and blue channels
    // and mix that value into the green channel.
    float despill_green = (source_color.r + source_color.b) * 0.5;
    vec3 desaturated_color = vec3(source_color.r, despill_green, source_color.b);

    // Linearly interpolate between the original color and the despilled color
    // based on the calculated spill_amount.
    vec3 final_rgb = mix(source_color.rgb, desaturated_color, spill_amount);

    // Combine the original texture's alpha with our calculated keying alpha.
    // This correctly preserves any transparency already in the source image.
    FragColor = vec4(final_rgb, source_color.a * base_alpha);
}