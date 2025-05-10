#version 330 core
out vec4 FragColor;

in vec2 v_TexCoord;

uniform sampler2D u_Texture;     // Input image/video
uniform sampler2D u_MaskTexture; // Grayscale/Alpha mask texture

uniform int u_MaskType; // 0:None, 1:Rect, 2:Circle, 3:Texture
uniform bool u_Invert;
uniform float u_Feather; // Feather amount [0, 1]

// Rectangle uniforms
uniform vec2 u_RectCenter; // Normalized [0, 1]
uniform vec2 u_RectSize;   // Normalized [0, 1]
uniform float u_RectRotation; // Radians
uniform float u_RectCornerRadius;

// Circle uniforms
uniform vec2 u_CircleCenter; // Normalized [0, 1]
uniform float u_CircleRadius; // Normalized [0, 1]
uniform float u_CircleAspectRatio;

// Optional: For aspect ratio correction if needed
uniform vec2 u_Resolution;


// Function to rotate UVs
vec2 rotateUV(vec2 uv, vec2 pivot, float rotation) {
    float s = sin(rotation);
    float c = cos(rotation);
    uv -= pivot;
    uv = vec2(uv.x * c - uv.y * s, uv.x * s + uv.y * c);
    uv += pivot;
    return uv;
}

// Signed distance to box function (for rectangle)
// From Inigo Quilez - https://www.iquilezles.org/www/articles/distfunctions2d/distfunctions2d.htm
float sdBox(vec2 p, vec2 b) {
  vec2 d = abs(p) - b;
  return length(max(d, 0.0)) + min(max(d.x, d.y), 0.0);
}

float sdRoundedBox( in vec2 p, in vec2 b, in vec4 r )
{
    r.xy = (p.x>0.0)?r.xy : r.zw;
    r.x  = (p.y>0.0)?r.x  : r.y;
    vec2 q = abs(p)-b+r.x;
    return min(max(q.x,q.y),0.0) + length(max(q,0.0)) - r.x;
}


float getMaskAlpha() {
    float mask = 1.0; // Default opaque
    float safe_feather = max(0.001, u_Feather); // Avoid zero feather edge cases

    // Adjust UV for aspect ratio if needed, centered at 0.5, 0.5
    // This helps make circles circular and squares square regardless of screen aspect ratio
    float aspect = u_Resolution.x / u_Resolution.y;
    vec2 aspectCorrectedUV = v_TexCoord;
    //aspectCorrectedUV.x = ((v_TexCoord.x - 0.5) * aspect) + 0.5; // Uncomment if shapes look stretched

    if (u_MaskType == 1) { // Rectangle
        vec2 rotatedUV = rotateUV(v_TexCoord, u_RectCenter, -u_RectRotation); // Rotate UV coords
        vec2 halfSize = u_RectSize * 0.5;
        float dist = (u_RectCornerRadius > 0) ? sdRoundedBox(rotatedUV - u_RectCenter, halfSize, vec4(u_RectCornerRadius, u_RectCornerRadius, u_RectCornerRadius, u_RectCornerRadius)) : sdBox(rotatedUV - u_RectCenter, halfSize);

        // Use smoothstep for feathering based on distance field
        mask = smoothstep(safe_feather * 0.5, -safe_feather * 0.5, dist);

    } else if (u_MaskType == 2) { // Circle
        vec2 aspectCorrectedCenter = vec2(u_CircleCenter.x, u_CircleCenter.y / u_CircleAspectRatio);
        vec2 aspectCorrectedUV = vec2(v_TexCoord.x, v_TexCoord.y / u_CircleAspectRatio);
        float dist = distance(aspectCorrectedUV, aspectCorrectedCenter);
        mask = smoothstep(u_CircleRadius + safe_feather * 0.5, u_CircleRadius - safe_feather * 0.5, dist);

    } else if (u_MaskType == 3) { // Texture
        // Use Red channel as mask intensity (common for grayscale masks)
        // Texture filtering (GL_LINEAR) provides some basic softening already
        float texMask = texture(u_MaskTexture, v_TexCoord).r;

        // Simple feathering by scaling/biasing the texture value
        // More advanced feathering might involve blurring the texture itself
        // or multi-sampling in the shader.
        // This basic smoothstep helps soften the 0->1 transition from the texture.
        mask = smoothstep(0.5 - safe_feather * 0.5, 0.5 + safe_feather * 0.5, texMask);

    } else { // None or other
        mask = 1.0;
    }

    if (u_Invert) {
        mask = 1.0 - mask;
    }

    return clamp(mask, 0.0, 1.0);
}

void main()
{
    vec4 originalColor = texture(u_Texture, v_TexCoord);
    float alpha = getMaskAlpha();

    FragColor = vec4(originalColor.rgb, originalColor.a * alpha);
    // If you want hard masking without blending original alpha:
    // FragColor = originalColor * alpha;
}