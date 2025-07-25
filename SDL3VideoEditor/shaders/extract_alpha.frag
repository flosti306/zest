// extract_alpha.frag

#version 330 core
out vec4 FragColor;
in vec2 TexCoords; // CORRECTED: Was v_TexCoord

uniform sampler2D u_InputTexture; // This should be the already masked input

void main() {
    float alpha = texture(u_InputTexture, TexCoords).a;
    FragColor = vec4(alpha, alpha, alpha, 1.0); // Store alpha in RGB, full opaque alpha for this buffer
}