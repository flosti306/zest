#version 330 core
uniform sampler2D u_Texture;  // Renamed from 'image' to match your C++ code
in vec2 TexCoords;           // Input from vertex shader
out vec4 FragmentColor;

uniform float u_BlurAmount;  // Blur strength parameter
uniform vec2 u_Direction;    // Direction vector for blur (horizontal or vertical)
uniform vec2 u_Resolution;   // Screen resolution

void main() {
    // Calculate pixel size for proper sampling
    vec2 pixel = 1.0 / u_Resolution;
    
    // Sample center texel
    vec4 color = texture(u_Texture, TexCoords) * 0.4;
    
    // Blur based on direction and amount
    float totalWeight = 0.4;  // Center weight
    
    // Sample 4 pixels in each direction for a 9-tap blur
    for (int i = 1; i <= 4; i++) {
        float weight = 0.4 / (i * i);
        totalWeight += 2.0 * weight;
        
        // Sample in both positive and negative directions
        vec2 offset = u_Direction * pixel * float(i) * u_BlurAmount;
        color += texture(u_Texture, TexCoords + offset) * weight;
        color += texture(u_Texture, TexCoords - offset) * weight;
    }
    
    // Normalize the result
    FragmentColor = color / totalWeight;
}