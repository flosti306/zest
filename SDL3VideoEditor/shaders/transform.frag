#version 330 core
out vec4 FragColor;

in vec2 v_TexCoord;

uniform sampler2D u_Texture;
uniform vec2 u_Translate;
uniform vec2 u_Scale;
uniform float u_Rotation;
uniform float u_AspectRatio;

void main() {
    vec2 center = vec2(0.5, 0.5);
    vec2 transformed_coords = v_TexCoord;
    
    // --- NEW: Aspect Ratio Correction ---
    // We work in a "corrected" space where the image is treated as a square.
    mat2 aspect_matrix = mat2(1.0, 0.0, 0.0, u_AspectRatio);
    mat2 inverse_aspect_matrix = mat2(1.0, 0.0, 0.0, 1.0 / u_AspectRatio);
    
    transformed_coords = inverse_aspect_matrix * (transformed_coords - center) + center;
    // --- END NEW ---

    // Inverse Scale
    transformed_coords = ((transformed_coords - center) / u_Scale) + center;

    // Inverse Rotate
    float cos_angle = cos(-u_Rotation);
    float sin_angle = sin(-u_Rotation);
    mat2 rotation_matrix = mat2(cos_angle, -sin_angle, sin_angle, cos_angle);
    transformed_coords = rotation_matrix * (transformed_coords - center) + center;

    // Inverse Translate
    transformed_coords -= u_Translate;
    
    // --- NEW: Re-apply aspect ratio correction at the end ---
    transformed_coords = aspect_matrix * (transformed_coords - center) + center;
    // --- END NEW ---

    if (transformed_coords.x < 0.0 || transformed_coords.x > 1.0 ||
        transformed_coords.y < 0.0 || transformed_coords.y > 1.0) {
        FragColor = vec4(0.0, 0.0, 0.0, 0.0);
    } else {
        FragColor = texture(u_Texture, transformed_coords);
    }
}