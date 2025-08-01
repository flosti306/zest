#version 330 core
out vec4 FragColor;

// Input from blur.vert (as you specified)
in vec2 TexCoords;

uniform sampler2D u_Texture;
uniform vec2 u_Direction;   // (1.0, 0.0) for horizontal, (0.0, 1.0) for vertical
uniform float u_BlurAmount;
uniform vec2 u_Resolution;

// Pre-calculated weights for a 9-tap 1D Gaussian kernel.
// These values create a much smoother falloff than a simple 1/(i*i) curve.
const float weight[5] = float[] (0.2270270270, 0.1945945946, 0.1216216216, 0.0540540541, 0.0162162162);

void main() {
    // Get the original color and alpha from the center pixel. We'll preserve the alpha.
    vec4 original_sample = texture(u_Texture, TexCoords);
    vec3 final_color = original_sample.rgb * weight[0];

    // Don't perform the blur if the amount is zero or less.
    if (u_BlurAmount > 0.0) {
        // Calculate the size of a single pixel in texture coordinate space.
        vec2 tex_offset = 1.0 / u_Resolution;

        // Loop 4 times to sample 8 pixels (4 in the positive direction, 4 in the negative).
        // This, plus the center tap, creates a 9-tap high-quality blur.
        for(int i = 1; i < 5; ++i) {
            // Calculate the offset for this tap, scaled by the blur amount.
            vec2 offset = u_Direction * tex_offset * float(i) * u_BlurAmount;
            
            // Sample in the positive and negative directions and add their weighted contributions.
            final_color += texture(u_Texture, TexCoords + offset).rgb * weight[i];
            final_color += texture(u_Texture, TexCoords - offset).rgb * weight[i];
        }
    }

    // Output the final blurred color, preserving the original alpha channel.
    FragColor = vec4(final_color, original_sample.a);
}