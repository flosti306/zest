// shaders/gradient.frag
#version 330 core
out vec4 FragColor;

in vec2 v_TexCoord; // Normalized UV [0,1], (0,0) is usually bottom-left

uniform sampler2D u_OriginalTexture;
uniform int u_GradientType; // 0 for Linear, 1 for Radial

uniform vec4 u_ColorStart; // RGBA
uniform vec4 u_ColorEnd;   // RGBA

// Linear
uniform vec2 u_LinearStartPoint; // Normalized UV [0,1]
uniform vec2 u_LinearEndPoint;   // Normalized UV [0,1]

// Radial
uniform vec2 u_RadialCenterPoint; // Normalized UV [0,1]
uniform float u_RadialRadiusInner;  // Normalized
uniform float u_RadialRadiusOuter;  // Normalized
uniform float u_RadialAspectRatio;  // To correct for non-square viewports (width/height)

uniform float u_BlendWithOriginal;


// Helper to project point P onto line segment AB, returns t [0,1]
float projectPointOnLineSegment(vec2 p, vec2 a, vec2 b) {
    vec2 ab = b - a;
    vec2 ap = p - a;
    float len_sq_ab = dot(ab, ab);
    if (len_sq_ab == 0.0) return 0.0; // a and b are the same point
    float t = dot(ap, ab) / len_sq_ab;
    return clamp(t, 0.0, 1.0);
}

void main() {
    vec4 original_color = texture(u_OriginalTexture, v_TexCoord);
    vec4 gradient_color;

    if (u_GradientType == 0) { // Linear
        float t = projectPointOnLineSegment(v_TexCoord, u_LinearStartPoint, u_LinearEndPoint);
        gradient_color = mix(u_ColorStart, u_ColorEnd, t);
    } else if (u_GradientType == 1) { // Radial
        vec2 uv = v_TexCoord;
        // Correct UVs for aspect ratio to make radial gradient circular
        uv.y = (uv.y - u_RadialCenterPoint.y) * u_RadialAspectRatio + u_RadialCenterPoint.y;
        
        float dist = distance(uv, u_RadialCenterPoint);
        float t = smoothstep(u_RadialRadiusInner, u_RadialRadiusOuter, dist);
        gradient_color = mix(u_ColorStart, u_ColorEnd, t);
    } else {
        gradient_color = u_ColorStart; // Default or error
    }

    FragColor = mix(gradient_color, original_color, u_BlendWithOriginal);
}