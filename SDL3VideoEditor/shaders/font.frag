#version 330 core
in vec2 v_TexCoord;
out vec4 FragColor;

uniform sampler2D u_FontTexture; // The single-channel font atlas
uniform vec4 u_TextColor;       // The color we want the text to be

void main() {
    // Sample the texture. The value we care about is in the .r channel.
    float alpha = texture(u_FontTexture, v_TexCoord).r;

    // The final color is the user's chosen color, but with an alpha
    // value determined by the brightness of the font texture at that pixel.
    FragColor = vec4(u_TextColor.rgb, u_TextColor.a * alpha);
}