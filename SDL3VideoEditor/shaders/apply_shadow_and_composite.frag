#version 330 core
out vec4 FragColor;

in vec2 TexCoords;

uniform sampler2D u_OriginalContentTexture;
uniform sampler2D u_BlurredShadowMaskTexture;
uniform vec2 u_ShadowOffset;
uniform vec4 u_ShadowColor;

void main() {
    vec4 original_color = texture(u_OriginalContentTexture, TexCoords);
    float shadow_alpha = texture(u_BlurredShadowMaskTexture, TexCoords - u_ShadowOffset).r;
    vec4 shadow = vec4(u_ShadowColor.rgb, u_ShadowColor.a * shadow_alpha);
    FragColor = mix(shadow, original_color, original_color.a);
}