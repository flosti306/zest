#version 330 core
in vec2 TexCoords;
out vec4 FragColor;

uniform sampler2D u_Texture;

// Color grading parameters
uniform float u_Brightness;   // [-1.0, 1.0], default 0.0
uniform float u_Contrast;     // [0.0, 2.0], default 1.0
uniform float u_Saturation;   // [0.0, 2.0], default 1.0
uniform float u_Temperature;  // [-1.0, 1.0], default 0.0 (negative = cooler/blue, positive = warmer/orange)
uniform float u_Tint;         // [-1.0, 1.0], default 0.0 (negative = green, positive = magenta)
uniform vec3 u_ColorFilter;   // RGB color filter, default (1.0, 1.0, 1.0)
uniform float u_Gamma;        // [0.1, 3.0], default 1.0

// Convert RGB to HSV
vec3 rgb2hsv(vec3 c) {
    vec4 K = vec4(0.0, -1.0 / 3.0, 2.0 / 3.0, -1.0);
    vec4 p = mix(vec4(c.bg, K.wz), vec4(c.gb, K.xy), step(c.b, c.g));
    vec4 q = mix(vec4(p.xyw, c.r), vec4(c.r, p.yzx), step(p.x, c.r));

    float d = q.x - min(q.w, q.y);
    float e = 1.0e-10;
    return vec3(abs(q.z + (q.w - q.y) / (6.0 * d + e)), d / (q.x + e), q.x);
}

// Convert HSV to RGB
vec3 hsv2rgb(vec3 c) {
    vec4 K = vec4(1.0, 2.0 / 3.0, 1.0 / 3.0, 3.0);
    vec3 p = abs(fract(c.xxx + K.xyz) * 6.0 - K.www);
    return c.z * mix(K.xxx, clamp(p - K.xxx, 0.0, 1.0), c.y);
}

// Apply temperature adjustment
vec3 adjustTemperature(vec3 color, float temperature) {
    // Temperature adjustment (blue <-> orange)
    vec3 warm = vec3(0.1, 0.0, -0.1);  // Warm color adjustment
    vec3 cold = vec3(-0.1, -0.0, 0.1); // Cold color adjustment
    
    vec3 temperatureAdjustment = mix(cold, warm, temperature * 0.5 + 0.5);
    return color + temperatureAdjustment * abs(temperature);
}

// Apply tint adjustment
vec3 adjustTint(vec3 color, float tint) {
    // Tint adjustment (green <-> magenta)
    vec3 green = vec3(-0.1, 0.1, -0.1);   // Green tint adjustment
    vec3 magenta = vec3(0.1, -0.1, 0.1);  // Magenta tint adjustment
    
    vec3 tintAdjustment = mix(green, magenta, tint * 0.5 + 0.5);
    return color + tintAdjustment * abs(tint);
}

void main() {
    // Sample the texture
    vec4 texColor = texture(u_Texture, TexCoords);
    vec3 color = texColor.rgb;
    
    // Apply brightness
    color = color + u_Brightness;
    
    // Apply contrast
    color = ((color - 0.5) * u_Contrast) + 0.5;
    
    // Apply color filter
    color *= u_ColorFilter;
    
    // Apply temperature adjustment
    color = adjustTemperature(color, u_Temperature);
    
    // Apply tint adjustment
    color = adjustTint(color, u_Tint);
    
    // Apply saturation
    vec3 hsv = rgb2hsv(color);
    hsv.y *= u_Saturation;  // Modify saturation
    color = hsv2rgb(hsv);
    
    // Apply gamma correction
    color = pow(color, vec3(1.0 / u_Gamma));
    
    // Ensure color is in valid range
    color = clamp(color, 0.0, 1.0);
    
    // Output the final color
    FragColor = vec4(color, texColor.a);
}