#version 330 core
out vec4 FragColor;

in vec2 TexCoord;

uniform sampler2D uTexture;
uniform vec4 uColor; // Includes opacity

void main() {
    vec4 texColor = texture(uTexture, TexCoord);
    FragColor = texColor * uColor;
    // Premultiplied alpha could be done here if needed:
    // FragColor.rgb *= FragColor.a;
}