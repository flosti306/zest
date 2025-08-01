#version 330 core
out vec4 FragColor;

// --- CRITICAL: Receive the interpolated texture coordinate ---
in vec2 v_TexCoord;

uniform sampler2D u_OriginalTexture;
uniform int u_GradientType; // 0=Linear, 1=Radial
uniform vec4 u_ColorStart;
uniform vec4 u_ColorEnd;
uniform float u_Intensity;

// Linear uniforms
uniform vec2 u_LinearStartPoint;
uniform vec2 u_LinearEndPoint;

// Radial uniforms
uniform vec2 u_RadialCenterPoint;
uniform float u_RadialRadiusInner;
uniform float u_RadialRadiusOuter;
uniform float u_RadialAspectRatio;

void main() {
    vec4 original_color = texture(u_OriginalTexture, v_TexCoord);
    vec4 gradient_color;

    if (u_GradientType == 0) { // Linear
        vec2 p = u_LinearEndPoint - u_LinearStartPoint;
        vec2 q = v_TexCoord - u_LinearStartPoint;
        // Project q onto p to find the progression along the gradient line
        float t = dot(q, p) / dot(p, p);
        t = clamp(t, 0.0, 1.0);
        gradient_color = mix(u_ColorStart, u_ColorEnd, t);
    } else { // Radial
        vec2 uv_from_center = v_TexCoord - u_RadialCenterPoint;
        uv_from_center.x *= u_RadialAspectRatio; // Correct for aspect ratio
        float dist = length(uv_from_center);
        float t = smoothstep(u_RadialRadiusInner, u_RadialRadiusOuter, dist);
        gradient_color = mix(u_ColorStart, u_ColorEnd, t);
    }

    // Mix the gradient with the original texture based on intensity
    vec3 final_rgb = mix(original_color.rgb, gradient_color.rgb, gradient_color.a * u_Intensity);
    FragColor = vec4(final_rgb, original_color.a);
}