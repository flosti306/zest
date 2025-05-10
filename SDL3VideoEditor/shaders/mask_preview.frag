#version 330 core
in vec2 v_TexCoord; // Comes from the vertex shader, (0,0) is bottom-left typically
out vec4 FragColor;

uniform sampler2D u_BackgroundTexture;    // Original clip content
uniform sampler2D u_DrawnMaskTexture;     // The R/G/B mask texture you are painting on

// Uniforms for visualizing procedural masks (rect, circle) IF you want to see them live
uniform int u_ProceduralMaskType; // 0:None, 1:Rect, 2:Circle (distinct from texture mask type)
uniform bool u_InvertProcedural;
uniform float u_ProceduralFeather;

// Rectangle uniforms
uniform vec2 u_RectCenter; 
uniform vec2 u_RectSize;   
uniform float u_RectRotation; 
uniform float u_RectCornerRadius;

// Circle uniforms
uniform vec2 u_CircleCenter; 
uniform float u_CircleRadius; 
uniform float u_CircleAspectRatio;

uniform vec2 u_Resolution; // For aspect correction if procedural masks need it


// --- Re-use your mask calculation functions from mask.frag ---
// (Slightly adapted to take v_TexCoord directly and use procedural uniforms)

vec2 rotateUV(vec2 uv, vec2 pivot, float rotation) {
    float s = sin(rotation);
    float c = cos(rotation);
    uv -= pivot;
    uv = vec2(uv.x * c - uv.y * s, uv.x * s + uv.y * c);
    uv += pivot;
    return uv;
}

float sdBox(vec2 p, vec2 b) {
  vec2 d = abs(p) - b;
  return length(max(d, 0.0)) + min(max(d.x, d.y), 0.0);
}

float sdRoundedBox( in vec2 p, in vec2 b, in vec4 r ) {
    r.xy = (p.x>0.0)?r.xy : r.zw;
    r.x  = (p.y>0.0)?r.x  : r.y;
    vec2 q = abs(p)-b+r.x;
    return min(max(q.x,q.y),0.0) + length(max(q,0.0)) - r.x;
}

float getProceduralMaskAlpha(vec2 texCoordToUse) {
    float mask = 1.0;
    float safe_feather = max(0.001, u_ProceduralFeather);

    // vec2 aspectCorrectedUV = texCoordToUse; // You might need aspect correction here too
    // aspectCorrectedUV.x = ((texCoordToUse.x - 0.5) * (u_Resolution.x / u_Resolution.y)) + 0.5;


    if (u_ProceduralMaskType == 1) { // Rectangle
        vec2 rotatedUV = rotateUV(texCoordToUse, u_RectCenter, -u_RectRotation);
        vec2 halfSize = u_RectSize * 0.5;
        float dist = (u_RectCornerRadius > 0.0) ? 
                     sdRoundedBox(rotatedUV - u_RectCenter, halfSize, vec4(u_RectCornerRadius)) : 
                     sdBox(rotatedUV - u_RectCenter, halfSize);
        mask = smoothstep(safe_feather * 0.5, -safe_feather * 0.5, dist);
    } else if (u_ProceduralMaskType == 2) { // Circle
        vec2 centerToUse = u_CircleCenter;
        vec2 uvToUse = texCoordToUse;
        // Apply aspect ratio correction for circle drawing
        // This makes the circle appear round in normalized UV space even if viewport is not square
        // Assuming u_CircleAspectRatio = desired_width_ratio / desired_height_ratio
        // If u_CircleAspectRatio > 1, it's wider. If < 1, it's taller.
        // We adjust one of the UV components or the center.
        // Let's scale the Y component of the UV relative to the center.
        uvToUse.y = (uvToUse.y - centerToUse.y) / u_CircleAspectRatio + centerToUse.y;
        
        float dist = distance(uvToUse, centerToUse);
        mask = smoothstep(u_CircleRadius + safe_feather * 0.5, u_CircleRadius - safe_feather * 0.5, dist);
    } else {
        mask = 1.0; // No procedural mask
    }

    if (u_InvertProcedural) {
        mask = 1.0 - mask;
    }
    return clamp(mask, 0.0, 1.0);
}


void main() {
    vec4 backgroundColor = texture(u_BackgroundTexture, v_TexCoord);
    
    // Sample the drawn mask (texture being painted)
    // u_DrawnMaskTexture was painted with top-left origin.
    // v_TexCoord is bottom-left origin. So, flip Y for sampling.
    float drawnMaskValue = texture(u_DrawnMaskTexture, vec2(v_TexCoord.x, 1.0 - v_TexCoord.y)).r; 

    // Get alpha from procedural mask (if active for preview)
    // For procedural masks, v_TexCoord (0-1) usually represents the normalized space they operate in.
    float proceduralMaskAlpha = getProceduralMaskAlpha(v_TexCoord);

    // How to combine/visualize:
    // Option 1: Show background, overlay with semi-transparent red where EITHER mask is.
    // vec3 mask_visualization_color = vec3(1.0, 0.0, 0.0); // Red
    // float combined_mask_strength = max(drawnMaskValue, 1.0 - proceduralMaskAlpha); // If procedural is "mask out"
                                                                                  // or use proceduralMaskAlpha directly if it's "mask in"
    // vec3 final_color = mix(backgroundColor.rgb, mask_visualization_color, combined_mask_strength * 0.4);
    // FragColor = vec4(final_color, backgroundColor.a);

    // Option 2: Apply the DRAWN mask to the background, then optionally overlay procedural for guidance
    vec4 maskedBackground = vec4(backgroundColor.rgb, backgroundColor.a * drawnMaskValue);
    
    // Overlay procedural mask guide (e.g., a semi-transparent shape)
    if (u_ProceduralMaskType != 0) {
        vec3 procedural_guide_color = vec3(0.2, 0.2, 1.0); // Blueish guide
        float guide_strength = proceduralMaskAlpha * 0.3; // Make it semi-transparent
        maskedBackground.rgb = mix(maskedBackground.rgb, procedural_guide_color, guide_strength);
    }
    
    FragColor = maskedBackground;

    // Option 3: Just show background with drawn mask applied (simplest for drawing)
    // FragColor = vec4(backgroundColor.rgb, backgroundColor.a * drawnMaskValue);
}