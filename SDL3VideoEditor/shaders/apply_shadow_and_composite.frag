// shaders/apply_shadow_and_composite.frag
#version 330 core
out vec4 FragColor;
in vec2 v_TexCoord;

uniform sampler2D u_OriginalContentTexture; // The input clip (already masked by previous effects)
uniform sampler2D u_BlurredShadowMaskTexture; // The blurred alpha mask

uniform vec2 u_ShadowOffset;     // Normalized UV offset for the shadow
uniform vec4 u_ShadowColor;      // RGBA for the shadow
uniform vec2 u_PixelSize;        // 1.0 / resolution, for pixel-perfect offset if needed

void main() {
    // Sample the original content (already masked)
    vec4 original_content = texture(u_OriginalContentTexture, v_TexCoord);

    // Sample the blurred shadow mask at the offset position
    // v_TexCoord is current pixel. To get shadow, sample shadow mask from where shadow *would be cast from*.
    // So, if shadow is offset by (dx, dy), we sample the mask at (uv.x - dx, uv.y - dy).
    vec2 shadow_sample_uv = v_TexCoord - u_ShadowOffset; 
    // Alternatively, if u_ShadowOffset is defined as "where the shadow appears relative to object":
    // vec2 shadow_sample_uv = v_TexCoord - u_ShadowOffset; 
    // Let's stick with: positive offset moves shadow right and up. So for current pixel,
    // we look "left and down" on the shadow map.
    // No, that's not right. If shadow_offset.x is positive (move shadow right),
    // then for a pixel at v_TexCoord, the shadow contributing to it comes from
    // v_TexCoord - u_ShadowOffset.x on the shadow map.

    float shadow_alpha_intensity = texture(u_BlurredShadowMaskTexture, shadow_sample_uv).r; // Assuming alpha was stored in R

    // Create the shadow color with the sampled intensity
    vec4 shadow_layer = vec4(u_ShadowColor.rgb, u_ShadowColor.a * shadow_alpha_intensity);

    // Composite: Draw shadow layer, then original content on top (alpha blending)
    // FinalColor = Original AlphaBlendOver Shadow
    // C_out = C_original + C_shadow * (1 - A_original)
    FragColor = original_content + shadow_layer * (1.0 - original_content.a);

    // Alternative simpler compositing if original_content is considered fully opaque where visible:
    // FragColor = mix(shadow_layer, original_content, original_content.a); 
    // This just layers original on top of shadow, using original's alpha.
    // The first method is more standard alpha compositing.
}