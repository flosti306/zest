#version 330 core
in vec2 TexCoords;
out vec4 FragColor;

uniform sampler2D u_Texture;  // Original image texture
uniform sampler2D u_LUT;      // 3D LUT stored as 2D texture
uniform float u_Strength;     // Blend amount between original and LUT-processed

// LUT dimensions
const float LUT_SIZE = 64.0;  // Typical LUT size (adjust as needed)

// Function to sample the LUT
vec3 applyLUT(vec3 color) {
    // Scale the input color to the LUT range
    color = clamp(color, 0.0, 1.0);
    
    // Scale input color from [0, 1] to [0, lutSize-1]
    color *= (LUT_SIZE - 1.0);
    
    // Calculate the slices and offsets
    float blueSlice = floor(color.b);
    float blueOffset = color.b - blueSlice;
    
    // Calculate the 2D LUT coordinates for both slices
    float halfPixel = 0.5 / LUT_SIZE;
    
    // First slice coordinates (b)
    float x1 = halfPixel + color.r / LUT_SIZE;
    float y1 = halfPixel + (blueSlice + color.g) / LUT_SIZE;
    
    // Second slice coordinates (b+1)
    float x2 = halfPixel + color.r / LUT_SIZE;
    float y2 = halfPixel + (blueSlice + 1.0 + color.g) / LUT_SIZE;
    
    // Sample both slices
    vec3 color1 = texture(u_LUT, vec2(x1, y1)).rgb;
    vec3 color2 = texture(u_LUT, vec2(x2, y2)).rgb;
    
    // Interpolate between slices
    return mix(color1, color2, blueOffset);
}

void main() {
    // Sample the original texture
    vec4 originalColor = texture(u_Texture, TexCoords);
    
    // Apply the LUT transformation
    vec3 lutColor = applyLUT(originalColor.rgb);
    
    // Blend between original and LUT-processed color
    vec3 finalColor = mix(originalColor.rgb, lutColor, u_Strength);
    
    // Output the final color
    FragColor = vec4(finalColor, originalColor.a);
}