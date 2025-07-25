// apply_shadow_and_composite.frag

#version 330 core
out vec4 FragColor;
in vec2 TexCoords; // CORRECTED: Was v_TexCoord

uniform sampler2D u_OriginalContentTexture; // The input clip (already masked by previous effects)
uniform sampler2D u_BlurredShadowMaskTexture; // The blurred alpha mask

uniform vec2 u_ShadowOffset;     // Normalized UV offset for the shadow
uniform vec4 u_ShadowColor;      // RGBA for the shadow

void main() {
    // Sample the original content (already masked)
    vec4 original_content = texture(u_OriginalContentTexture, TexCoords);

    // Sample the blurred shadow mask at the offset position
    vec2 shadow_sample_uv = TexCoords - u_ShadowOffset; 
    
    float shadow_alpha_intensity = texture(u_BlurredShadowMaskTexture, shadow_sample_uv).r; // Assuming alpha was stored in R

    // Create the shadow color with the sampled intensity
    vec4 shadow_layer = vec4(u_ShadowColor.rgb, u_ShadowColor.a * shadow_alpha_intensity);

    // Composite: Draw shadow layer, then original content on top using standard alpha blending.
    // FinalColor = C_original + C_shadow * (1 - A_original)
    // This correctly places the original content "over" the shadow.
    FragColor = original_content + shadow_layer * (1.0 - original_content.a);
}