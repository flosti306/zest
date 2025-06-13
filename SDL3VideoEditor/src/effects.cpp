#include <vector>
#include <memory>
#include <string>
#include <algorithm>
#include <glm/glm.hpp>
#include <glad/glad.h> // Include glad for OpenGL function loading
#include "effects.hpp"
#include "cv_utils.hpp"


GLuint LoadShaderProgram(const std::string& vertex_path, const std::string& fragment_path) {
    auto read_file = [](const std::string& path) -> std::string {
        std::ifstream file(path);
        if (!file.is_open()) {
            std::cerr << "Failed to open shader file: " << path << std::endl;
            return "";
        }
        std::stringstream buffer;
        buffer << file.rdbuf();
        return buffer.str();
    };

    std::string vertex_code = read_file(vertex_path);
    std::string fragment_code = read_file(fragment_path);

    if (vertex_code.empty() || fragment_code.empty()) {
        std::cerr << "Shader source is empty.\n";
        return 0;
    }

    auto compile_shader = [](const std::string& code, GLenum type) -> GLuint {
        GLuint shader = glCreateShader(type);
        const char* src = code.c_str();
        glShaderSource(shader, 1, &src, nullptr);
        glCompileShader(shader);

        GLint success;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
        if (!success) {
            char log[1024];
            glGetShaderInfoLog(shader, 1024, nullptr, log);
            std::cerr << "Shader compilation failed: " << log << std::endl;
            glDeleteShader(shader);
            return 0;
        }
        return shader;
    };

    GLuint vs = compile_shader(vertex_code, GL_VERTEX_SHADER);
    GLuint fs = compile_shader(fragment_code, GL_FRAGMENT_SHADER);
    if (!vs || !fs) return 0;

    GLuint program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glLinkProgram(program);

    GLint link_success;
    glGetProgramiv(program, GL_LINK_STATUS, &link_success);
    if (!link_success) {
        char log[1024];
        glGetProgramInfoLog(program, 1024, nullptr, log);
        std::cerr << "Shader linking failed: " << log << std::endl;
        glDeleteProgram(program);
        return 0;
    }

    glDeleteShader(vs);
    glDeleteShader(fs);

    return program;
}

GLuint create_temp_fbo(glm::vec2 resolution, GLuint& texture_out) {
    GLuint fbo, tex;
    glGenFramebuffers(1, &fbo);
    glGenTextures(1, &tex);

    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, (int)resolution.x, (int)resolution.y, 0, GL_RGB, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    texture_out = tex;
    return fbo;
}

void destroy_temp_fbo(GLuint fbo, GLuint tex) {
    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(1, &tex);
}

GLuint get_texture_from_fbo(GLuint fbo) {
    GLint tex;
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                          GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &tex);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return (GLuint)tex;
}

void RenderFullscreenQuad() {
    // Create and setup a VAO for the fullscreen quad (only once)
    static GLuint quadVAO = 0;
    static GLuint quadVBO = 0;
    
    if (quadVAO == 0) {
        // Positions and texture coordinates for a fullscreen quad
        float quadVertices[] = {
            // Positions (x,y)    // Texture coords (u,v)
            -1.0f, -1.0f,         0.0f, 0.0f,  // bottom left
             1.0f, -1.0f,         1.0f, 0.0f,  // bottom right
             1.0f,  1.0f,         1.0f, 1.0f,  // top right
            -1.0f,  1.0f,         0.0f, 1.0f   // top left
        };
        
        // Indices for drawing with GL_TRIANGLES
        unsigned int indices[] = {
            0, 1, 2,  // first triangle
            0, 2, 3   // second triangle
        };
        
        GLuint quadEBO;
        
        glGenVertexArrays(1, &quadVAO);
        glGenBuffers(1, &quadVBO);
        glGenBuffers(1, &quadEBO);
        
        glBindVertexArray(quadVAO);
        
        glBindBuffer(GL_ARRAY_BUFFER, quadVBO);
        glBufferData(GL_ARRAY_BUFFER, sizeof(quadVertices), quadVertices, GL_STATIC_DRAW);
        
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, quadEBO);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);
        
        // Position attribute
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(0);
        
        // Texture coordinate attribute
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
        glEnableVertexAttribArray(1);
    }
    
    // Draw the quad
    glBindVertexArray(quadVAO);
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);
    glBindVertexArray(0);
}

void GaussianBlurNode::Process(const EffectContext& ctx) {
    if (!enabled || ctx.input_texture == 0)
        return;
        
    // Load shader program (consider caching this instead of loading every frame)
    GLuint blur_shader_program = LoadShaderProgram("shaders/blur.vert", "shaders/blur.frag");
    if (blur_shader_program == 0)
        return;
        
    // Setup modern shader VAO/VBO for the fullscreen quad
    static GLuint quadVAO = 0;
    static GLuint quadVBO = 0;
    
    if (quadVAO == 0) {
        // Positions and texture coordinates
        float quadVertices[] = {
            // Positions (x,y)    // Texture coords (u,v)
            -1.0f, -1.0f,         0.0f, 0.0f,  // bottom left
             1.0f, -1.0f,         1.0f, 0.0f,  // bottom right
             1.0f,  1.0f,         1.0f, 1.0f,  // top right
            -1.0f,  1.0f,         0.0f, 1.0f   // top left
        };
        
        // Create indices for drawing with triangles
        unsigned int indices[] = {
            0, 1, 2,  // first triangle
            0, 2, 3   // second triangle
        };
        
        GLuint quadEBO;
        
        glGenVertexArrays(1, &quadVAO);
        glGenBuffers(1, &quadVBO);
        glGenBuffers(1, &quadEBO);
        
        glBindVertexArray(quadVAO);
        
        glBindBuffer(GL_ARRAY_BUFFER, quadVBO);
        glBufferData(GL_ARRAY_BUFFER, sizeof(quadVertices), quadVertices, GL_STATIC_DRAW);
        
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, quadEBO);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);
        
        // Position attribute
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(0);
        
        // Texture coordinate attribute
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
        glEnableVertexAttribArray(1);
    }
    
    // First pass: horizontal blur
    GLuint horizontal_tex;
    GLuint horizontal_fbo = create_temp_fbo(ctx.resolution, horizontal_tex);
    
    glBindFramebuffer(GL_FRAMEBUFFER, horizontal_fbo);
    glViewport(0, 0, (int)ctx.resolution.x, (int)ctx.resolution.y);
    glClear(GL_COLOR_BUFFER_BIT);
    
    glUseProgram(blur_shader_program);
    
    // Set uniforms
    glUniform1f(glGetUniformLocation(blur_shader_program, "u_BlurAmount"), blur_amount);
    glUniform2f(glGetUniformLocation(blur_shader_program, "u_Direction"), 1.0f, 0.0f);  // Horizontal
    glUniform2f(glGetUniformLocation(blur_shader_program, "u_Resolution"), ctx.resolution.x, ctx.resolution.y);
    
    // Bind input texture
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, ctx.input_texture);
    glUniform1i(glGetUniformLocation(blur_shader_program, "u_Texture"), 0);
    
    // Draw
    glBindVertexArray(quadVAO);
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);
    
    // Second pass: vertical blur
    glBindFramebuffer(GL_FRAMEBUFFER, ctx.output_fbo);
    glViewport(0, 0, (int)ctx.resolution.x, (int)ctx.resolution.y);
    glClear(GL_COLOR_BUFFER_BIT);
    
    // Update direction for vertical pass
    glUniform2f(glGetUniformLocation(blur_shader_program, "u_Direction"), 0.0f, 1.0f);  // Vertical
    
    // Use horizontal pass result as input
    glBindTexture(GL_TEXTURE_2D, horizontal_tex);
    
    // Draw
    glBindVertexArray(quadVAO);
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);
    
    // Cleanup
    glBindVertexArray(0);
    glUseProgram(0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    
    // Destroy temporary resources
    destroy_temp_fbo(horizontal_fbo, horizontal_tex);
}

void ColorGradingNode::Process(const EffectContext& ctx) {
    if (!enabled || ctx.input_texture == 0)
        return;
    
    // Load shader program (consider caching this instead of loading every frame)
    GLuint color_grade_program = LoadShaderProgram("shaders/colorgrade.vert", "shaders/colorgrade.frag");
    if (color_grade_program == 0)
        return;
    
    // Setup render state
    glBindFramebuffer(GL_FRAMEBUFFER, ctx.output_fbo);
    glViewport(0, 0, (int)ctx.resolution.x, (int)ctx.resolution.y);
    
    glUseProgram(color_grade_program);
    
    // Set uniforms
    glUniform1f(glGetUniformLocation(color_grade_program, "u_Brightness"), brightness);
    glUniform1f(glGetUniformLocation(color_grade_program, "u_Contrast"), contrast);
    glUniform1f(glGetUniformLocation(color_grade_program, "u_Saturation"), saturation);
    glUniform1f(glGetUniformLocation(color_grade_program, "u_Temperature"), temperature);
    glUniform1f(glGetUniformLocation(color_grade_program, "u_Tint"), tint);
    glUniform3f(glGetUniformLocation(color_grade_program, "u_ColorFilter"), 
                color_filter.r, color_filter.g, color_filter.b);
    glUniform1f(glGetUniformLocation(color_grade_program, "u_Gamma"), gamma);
    
    // Bind input texture
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, ctx.input_texture);
    glUniform1i(glGetUniformLocation(color_grade_program, "u_Texture"), 0);
    
    // Draw fullscreen quad using our improved RenderFullscreenQuad function
    RenderFullscreenQuad();
    
    // Cleanup
    glUseProgram(0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

// Add these methods to the ColorGradingNode class

// Preset: Warm Vintage Look
void ColorGradingNode::applyWarmVintagePreset() {
    ColorGradingNode::brightness = 0.05f;
    ColorGradingNode::contrast = 1.2f;
    ColorGradingNode::saturation = 0.85f;
    ColorGradingNode::temperature = 0.3f;  // Warm
    ColorGradingNode::tint = 0.1f;         // Slight magenta
    ColorGradingNode::color_filter = glm::vec3(1.02f, 0.98f, 0.9f); // Slightly yellow tint
    ColorGradingNode::gamma = 1.1f;
}

// Preset: Cold Cinematic Look
void ColorGradingNode::applyColdCinematicPreset() {
    ColorGradingNode::brightness = -0.05f;
    ColorGradingNode::contrast = 1.3f;
    ColorGradingNode::saturation = 0.8f;
    ColorGradingNode::temperature = -0.3f;  // Cold
    ColorGradingNode::tint = 0.0f;
    ColorGradingNode::color_filter = glm::vec3(0.9f, 0.97f, 1.0f); // Slight blue tint
    ColorGradingNode::gamma = 1.1f;
}

// Preset: High Contrast B&W
void ColorGradingNode::applyHighContrastBWPreset() {
    ColorGradingNode::brightness = 0.0f;
    ColorGradingNode::contrast = 1.5f;
    ColorGradingNode::saturation = 0.0f;    // Desaturated (B&W)
    ColorGradingNode::temperature = 0.0f;
    ColorGradingNode::tint = 0.0f;
    ColorGradingNode::color_filter = glm::vec3(1.0f, 1.0f, 1.0f);
    ColorGradingNode::gamma = 1.2f;
}

// Preset: Technicolor
void ColorGradingNode::applyTechnicolorPreset() {
    ColorGradingNode::brightness = 0.0f;
    ColorGradingNode::contrast = 1.2f;
    ColorGradingNode::saturation = 1.5f;    // Highly saturated
    ColorGradingNode::temperature = 0.1f;   // Slightly warm
    ColorGradingNode::tint = -0.1f;         // Slight green
    ColorGradingNode::color_filter = glm::vec3(1.05f, 1.0f, 0.95f);
    ColorGradingNode::gamma = 0.9f;
}

// Preset: Faded Film
void ColorGradingNode::applyFadedFilmPreset() {
    ColorGradingNode::brightness = 0.1f;
    ColorGradingNode::contrast = 0.85f;
    ColorGradingNode::saturation = 0.7f;
    ColorGradingNode::temperature = 0.2f;   // Warm
    ColorGradingNode::tint = 0.0f;
    ColorGradingNode::color_filter = glm::vec3(0.95f, 0.95f, 0.9f); // Yellowish
    ColorGradingNode::gamma = 1.0f;
}


void EffectGraph::Process(GLuint input_tex, GLuint output_fbo, float time, glm::vec2 resolution) {
    // If no effects or all are disabled, just copy the input to output
    if (nodes.empty() || std::none_of(nodes.begin(), nodes.end(), [](auto& n){ return n->enabled; })) {
        // Create a simple passthrough shader program
        GLuint copy_shader = LoadShaderProgram("shaders/passthrough.vert", "shaders/texture.frag");
        if (copy_shader == 0) return;
        
        glBindFramebuffer(GL_FRAMEBUFFER, output_fbo);
        glViewport(0, 0, (int)resolution.x, (int)resolution.y);
        
        glUseProgram(copy_shader);
        
        // Set uniforms for the passthrough shader
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, input_tex);
        glUniform1i(glGetUniformLocation(copy_shader, "u_Texture"), 0);
        
        // Draw fullscreen quad
        RenderFullscreenQuad();
        
        glUseProgram(0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        return;
    }

    // We have effects to process in sequence
    GLuint current_input = input_tex;
    GLuint temp_tex = 0;
    GLuint temp_fbo = 0;
    
    // We'll need ping-pong FBOs for multi-effect chains
    if (nodes.size() > 1) {
        temp_fbo = create_temp_fbo(resolution, temp_tex);
    }
    
    for (size_t i = 0; i < nodes.size(); ++i) {
        if (!nodes[i]->enabled) continue;
        
        // Determine target framebuffer
        // For the last effect or single effect, render to the output FBO
        // For intermediate effects, render to the temporary FBO
        GLuint target_fbo = (i == nodes.size() - 1 || nodes.size() == 1) ? output_fbo : temp_fbo;
        
        // Set up context for the effect
        EffectContext ctx{
            .input_texture = current_input,
            .output_fbo = target_fbo,
            .time = time,
            .resolution = resolution
        };
        
        // Process the effect
        nodes[i]->Process(ctx);
        
        // Update current_input for the next effect
        if (i < nodes.size() - 1) {
            current_input = temp_tex;
        }
    }
    
    // Clean up temporary resources
    if (temp_fbo != 0) {
        destroy_temp_fbo(temp_fbo, temp_tex);
    }
}

// Load a LUT from file (supports common 3D LUT formats)
bool LUTColorGradingNode::loadLUT(const std::string& path) {
    // If we already have a texture, delete it
    if (lut_texture != 0) {
        glDeleteTextures(1, &lut_texture);
        lut_texture = 0;
    }
    
    // Load the image using a library like stb_image
    int width, height, channels;
    SDL_Surface* surface = SDL_LoadBMP(path.c_str());
    if (!surface) {
        std::cerr << "Failed to load LUT image: " << path << " - " << SDL_GetError() << std::endl;
        return false;
    }

    // Ensure the surface is in the correct format (RGBA)
    SDL_Surface* converted_surface = SDL_ConvertSurface(surface, SDL_PIXELFORMAT_RGBA32);
    SDL_DestroySurface(surface);

    if (!converted_surface) {
        std::cerr << "Failed to convert LUT image to RGBA format: " << SDL_GetError() << std::endl;
        return false;
    }

    unsigned char* data = static_cast<unsigned char*>(converted_surface->pixels);
    width = converted_surface->w;
    height = converted_surface->h;
    channels = 4; // RGBA

    // Free the surface after extracting the data
    SDL_DestroySurface(converted_surface);
    
    if (!data) {
        std::cerr << "Failed to load LUT image: " << path << std::endl;
        return false;
    }
    
    // Create the texture
    glGenTextures(1, &lut_texture);
    glBindTexture(GL_TEXTURE_2D, lut_texture);
    
    // Set texture parameters
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    
    // Upload the texture data
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
    
    // Save the path
    lut_path = path;
    
    return true;
}

void LUTColorGradingNode::Process(const EffectContext& ctx) {
    if (!enabled || ctx.input_texture == 0 || lut_texture == 0)
        return;
    
    // Load shader program (consider caching this instead of loading every frame)
    GLuint lut_program = LoadShaderProgram("shaders/lut.vert", "shaders/lut.frag");
    if (lut_program == 0)
        return;
    
    // Setup render state
    glBindFramebuffer(GL_FRAMEBUFFER, ctx.output_fbo);
    glViewport(0, 0, (int)ctx.resolution.x, (int)ctx.resolution.y);
    
    glUseProgram(lut_program);
    
    // Set uniforms
    glUniform1f(glGetUniformLocation(lut_program, "u_Strength"), strength);
    
    // Bind the input texture
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, ctx.input_texture);
    glUniform1i(glGetUniformLocation(lut_program, "u_Texture"), 0);
    
    // Bind the LUT texture
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, lut_texture);
    glUniform1i(glGetUniformLocation(lut_program, "u_LUT"), 1);
    
    // Draw fullscreen quad
    RenderFullscreenQuad();
    
    // Cleanup
    glUseProgram(0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

bool MaskEffectNode::loadMaskTexture(const std::string& path) {
    if (mask_texture != 0) {
        glDeleteTextures(1, &mask_texture);
        mask_texture = 0;
    }
    mask_texture_path = ""; // Clear path initially

    SDL_Surface* surface = IMG_Load(path.c_str()); // Use IMG_Load for flexibility
    if (!surface) {
        std::cerr << "Failed to load mask texture image: " << path << " - " << SDL_GetError() << std::endl;
        return false;
    }

    // Prefer grayscale or single channel if possible, otherwise RGBA/RGB
    SDL_Surface* converted_surface = nullptr;
    GLenum gl_format = GL_RGB; // Default
    GLint gl_internal_format = GL_RGB8; // Request 8-bit internal format

    // Check if already grayscale (approximation)
    if (surface->format == SDL_PIXELFORMAT_INDEX8 || surface->format == SDL_PIXELFORMAT_RGB332) {
         // Try converting to a single channel format like GL_R8 if supported, or stick to RGB/RGBA
         // For simplicity, let's convert to RGBA and use the Red channel in the shader
        converted_surface = SDL_ConvertSurface(surface, SDL_PIXELFORMAT_RGBA32);
        gl_format = GL_RGBA;
        gl_internal_format = GL_RGBA8;
    } else if (SDL_ISPIXELFORMAT_ALPHA(surface->format)) {
        converted_surface = SDL_ConvertSurface(surface, SDL_PIXELFORMAT_RGBA32);
        gl_format = GL_RGBA;
        gl_internal_format = GL_RGBA8;
    } else {
        converted_surface = SDL_ConvertSurface(surface, SDL_PIXELFORMAT_RGB24);
        gl_format = GL_RGB;
        gl_internal_format = GL_RGB8;
    }

    SDL_DestroySurface(surface); // Free original

    if (!converted_surface) {
        std::cerr << "Failed to convert mask texture image to required format: " << path << " - " << SDL_GetError() << std::endl;
        return false;
    }

    glGenTextures(1, &mask_texture);
    glBindTexture(GL_TEXTURE_2D, mask_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, gl_internal_format, converted_surface->w, converted_surface->h, 0, gl_format, GL_UNSIGNED_BYTE, converted_surface->pixels);

    GLenum err = glGetError();
    if (err != GL_NO_ERROR) {
         std::cerr << "OpenGL Error (" << err << ") loading mask texture: " << path << std::endl;
         glDeleteTextures(1, &mask_texture);
         mask_texture = 0;
         SDL_DestroySurface(converted_surface);
         return false;
    }

    // Generate mipmaps for potential smoother feathering via sampling if needed
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glBindTexture(GL_TEXTURE_2D, 0);
    SDL_DestroySurface(converted_surface);

    mask_texture_path = path; // Store path on success
    std::cout << "Loaded mask texture: " << path << " (ID: " << mask_texture << ")" << std::endl;
    return true;
}


// NEW: Implementation for MaskEffectNode::Process
void MaskEffectNode::Process(const EffectContext& ctx) {
    if (!enabled || ctx.input_texture == 0 || mask_type == MaskType::None)
        return; // Don't process if disabled, no input, or mask type is None

    // Load shader program (consider caching)
    GLuint mask_program = LoadShaderProgram("shaders/mask.vert", "shaders/mask.frag");
    if (mask_program == 0)
        return;

    // Setup render state
    glBindFramebuffer(GL_FRAMEBUFFER, ctx.output_fbo);
    glViewport(0, 0, (int)ctx.resolution.x, (int)ctx.resolution.y);
    // Don't clear here, assume previous effect or clip rendered content

    glUseProgram(mask_program);

    GLuint active_mask_texture_for_shader = 0;
    GLint mask_sampler_loc = glGetUniformLocation(mask_program, "u_MaskTexture");

    // --- Evaluate Keyframes ---
    float eval_feather = feather; // Default to base value
    if (!feather_track.keyframes.empty()) eval_feather = feather_track.Evaluate(ctx.time);

    glm::vec2 eval_rect_center = rect_center;
    if (!rect_center_x_track.keyframes.empty()) eval_rect_center.x = rect_center_x_track.Evaluate(ctx.time);
    if (!rect_center_y_track.keyframes.empty()) eval_rect_center.y = rect_center_y_track.Evaluate(ctx.time);

    glm::vec2 eval_rect_size = rect_size;
    if (!rect_size_x_track.keyframes.empty()) eval_rect_size.x = rect_size_x_track.Evaluate(ctx.time);
    if (!rect_size_y_track.keyframes.empty()) eval_rect_size.y = rect_size_y_track.Evaluate(ctx.time);

    float eval_rect_rotation = rect_rotation;
    if (!rect_rotation_track.keyframes.empty()) eval_rect_rotation = rect_rotation_track.Evaluate(ctx.time);

    float eval_rect_corner_radius = rect_corner_radius;
    if(!rect_corner_radius_track.keyframes.empty()) eval_rect_corner_radius = rect_corner_radius_track.Evaluate(ctx.time);

    glm::vec2 eval_circle_center = circle_center;
    if (!circle_center_x_track.keyframes.empty()) eval_circle_center.x = circle_center_x_track.Evaluate(ctx.time);
    if (!circle_center_y_track.keyframes.empty()) eval_circle_center.y = circle_center_y_track.Evaluate(ctx.time);

    float eval_circle_radius = circle_radius;
    if (!circle_radius_track.keyframes.empty()) eval_circle_radius = circle_radius_track.Evaluate(ctx.time);

    float eval_circle_aspect_ratio = circle_aspect_ratio;
    if (!circle_aspect_ratio_track.keyframes.empty()) eval_circle_aspect_ratio = circle_aspect_ratio_track.Evaluate(ctx.time);

    // Set common uniforms
    glUniform1i(glGetUniformLocation(mask_program, "u_MaskType"), static_cast<int>(mask_type));
    glUniform1i(glGetUniformLocation(mask_program, "u_Invert"), invert);
    // ***** USE EVALUATED VALUE *****
    glUniform1f(glGetUniformLocation(mask_program, "u_Feather"), eval_feather);
    glUniform2f(glGetUniformLocation(mask_program, "u_Resolution"), ctx.resolution.x, ctx.resolution.y);

    // Set type-specific uniforms
    switch (mask_type) {
        case MaskType::Rectangle:
            active_mask_texture_for_shader = mask_texture;
            // ***** USE EVALUATED VALUES *****
            glUniform2f(glGetUniformLocation(mask_program, "u_RectCenter"), eval_rect_center.x, eval_rect_center.y);
            glUniform2f(glGetUniformLocation(mask_program, "u_RectSize"), eval_rect_size.x, eval_rect_size.y);
            glUniform1f(glGetUniformLocation(mask_program, "u_RectRotation"), glm::radians(eval_rect_rotation)); // Pass radians
            glUniform1f(glGetUniformLocation(mask_program, "u_RectCornerRadius"), eval_rect_corner_radius);
            break;
        case MaskType::Circle:
            active_mask_texture_for_shader = mask_texture;
            // ***** USE EVALUATED VALUES *****
            glUniform2f(glGetUniformLocation(mask_program, "u_CircleCenter"), eval_circle_center.x, eval_circle_center.y);
            glUniform1f(glGetUniformLocation(mask_program, "u_CircleRadius"), eval_circle_radius);
            glUniform1f(glGetUniformLocation(mask_program, "u_CircleAspectRatio"), eval_circle_aspect_ratio);
            break;
        case MaskType::Texture:
            if (mask_texture == 0) { 
                glUseProgram(0);
                glBindFramebuffer(GL_FRAMEBUFFER, 0);
                // To prevent breaking the effect chain if a mask texture is expected but missing,
                // we should ideally render a passthrough or a fully opaque/transparent mask.
                // For now, let's just copy input to output FBO if mask_texture is 0.
                // This requires a simple copy shader. For simplicity of this fix, we'll return.
                // If this is an intermediate effect, returning means the FBO has old data.
                // A robust solution would be to draw input_texture to output_fbo.
                // For now, this matches previous behavior of effectively disabling the effect.
                return;
            }
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, mask_texture);
            glUniform1i(glGetUniformLocation(mask_program, "u_MaskTexture"), 1);
            break;
        case MaskType::Smart_Interactive:
            active_mask_texture_for_shader = smart_interactive_mask_texture;
            // The shader's u_MaskType for 'Texture' (3) can be used
            // as we're providing a texture containing the mask.
            glUniform1i(glGetUniformLocation(mask_program, "u_MaskType"), 3); 
            break;
        case MaskType::None: 
             glUseProgram(0);
             glBindFramebuffer(GL_FRAMEBUFFER, 0);
             return; 
    }

    if (active_mask_texture_for_shader != 0) {
        glActiveTexture(GL_TEXTURE1); // Or your chosen texture unit for masks
        glBindTexture(GL_TEXTURE_2D, active_mask_texture_for_shader);
        if (mask_sampler_loc != -1) {
            glUniform1i(mask_sampler_loc, 1); // Texture unit 1
        } else {
            // std::cerr << "Warning: u_MaskTexture uniform not found in mask shader." << std::endl;
        }
    } else if (mask_type == MaskType::Rectangle || mask_type == MaskType::Circle) {
        // Procedural, no texture needed for the mask itself, uniforms already set
    } else {
        // No mask texture active (e.g. MaskType::None or texture is 0)
        // Ensure a texture isn't accidentally bound to GL_TEXTURE1 from a previous operation.
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, ctx.input_texture);
    glUniform1i(glGetUniformLocation(mask_program, "u_Texture"), 0);

    RenderFullscreenQuad(); 

    glUseProgram(0);
    if (mask_type == MaskType::Texture) {
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, 0);
        glActiveTexture(GL_TEXTURE0); 
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

GLuint MaskEffectNode::RunGrabCut(const DecodedFrame& current_clip_frame,
                                GrabCutInitMode mode,
                                const cv::Rect& roi_for_rect_mode_px, // Already in pixel coords
                                const cv::Mat& scribble_mask_for_mask_mode_cv) { // CV_8UC1
    if (current_clip_frame.pixels.empty()) {
        std::cerr << "RunGrabCut: Current clip frame is empty." << std::endl;
        return false;
    }

    cv::Mat image_bgr = DecodedFrameToCvMat(current_clip_frame);
    if (image_bgr.empty()) {
        std::cerr << "RunGrabCut: Failed to convert frame to cv::Mat." << std::endl;
        return false;
    }

    int img_w = image_bgr.cols;
    int img_h = image_bgr.rows;

    cv::Mat result_mask_cv;
    cv::Mat bgdModel, fgdModel;
    cv::Rect actual_roi_for_grabcut = roi_for_rect_mode_px; // Default

    try {
        if (mode == GrabCutInitMode::RECT) {
            if (actual_roi_for_grabcut.width <= 0 || actual_roi_for_grabcut.height <= 0) {
                 std::cerr << "RunGrabCut (RECT): Invalid ROI." << std::endl; return 0;
            }
            cv::grabCut(image_bgr, result_mask_cv, actual_roi_for_grabcut, bgdModel, fgdModel, 5, cv::GC_INIT_WITH_RECT);
        } else { // GrabCutInitMode::MASK
            if (scribble_mask_for_mask_mode_cv.empty() || 
                scribble_mask_for_mask_mode_cv.size() != image_bgr.size() ||
                scribble_mask_for_mask_mode_cv.type() != CV_8UC1) {
                std::cerr << "RunGrabCut (MASK): Invalid scribble mask." << std::endl; return 0;
            }
            // When using GC_INIT_WITH_MASK, result_mask_cv is also the input hint mask.
            scribble_mask_for_mask_mode_cv.copyTo(result_mask_cv); 
            // The ROI can still be passed to potentially speed up, but GrabCut mainly uses the mask.
            // If no meaningful ROI, you can pass a rect covering the whole image.
            // For simplicity, if scribbles are used, let's assume the scribble mask dictates areas.
            // The rect parameter is still required by the API signature even with GC_INIT_WITH_MASK.
            // It often defines the area where "probable" values in the mask will be considered.
            // If your scribble_mask_cv *only* contains GC_FGD and GC_BGD, then the rect might be less critical.
            // If it contains GC_PR_FGD/BGD, the rect helps define the "unknown" region.
            // For now, let's use the full image if mode is MASK and no specific ROI is also provided for it.
            cv::Rect processing_rect(0, 0, image_bgr.cols, image_bgr.rows); 
            if(roi_for_rect_mode_px.width > 0 && roi_for_rect_mode_px.height > 0 && mode == GrabCutInitMode::MASK) {
                // Optionally, if an ROI is ALSO provided with scribble mode, use it.
                // This is advanced: the mask has scribbles, and rect defines "unknowns".
                // For now, if scribbles, let's assume they are comprehensive enough or rect is full image.
            }

            cv::grabCut(image_bgr, result_mask_cv, processing_rect, bgdModel, fgdModel, 5, cv::GC_INIT_WITH_MASK);
        }
    } catch (const cv::Exception& e) { /* ... */ return 0; }

    int tex_w, tex_h;
    GLuint generated_texture_id = GrabCutMaskToRGBTexture(result_mask_cv, tex_w, tex_h, true);

    if (generated_texture_id == 0) {
        std::cerr << "RunGrabCut: Failed to convert GrabCut mask to texture." << std::endl;
        return 0;
    }

    std::cout << "GrabCut successful. Generated temporary mask texture ID: " << generated_texture_id << std::endl;
    return generated_texture_id; // Return the new texture ID
}

void SolidColorEffectNode::Process(const EffectContext& ctx) {
    if (!enabled || ctx.input_texture == 0) {
        // If disabled, should we pass through or do nothing?
        // For an effect chain, a disabled effect usually means pass-through.
        // This needs to be handled by EffectGraph or by this node outputting input_texture.
        // For now, assume EffectGraph handles passthrough if node is disabled.
        // If called directly and disabled, it just returns, output_fbo isn't touched.
        return;
    }

    // Consider caching shader programs
    static GLuint program = 0;
    if (program == 0) {
        program = LoadShaderProgram("shaders/passthrough.vert", "shaders/solid_color.frag"); // Use your actual vertex shader path
        if (program == 0) return;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, ctx.output_fbo);
    glViewport(0, 0, (int)ctx.resolution.x, (int)ctx.resolution.y);
    // glClearColor(0,0,0,0); glClear(GL_COLOR_BUFFER_BIT); // Usually not needed if drawing fullscreen

    glUseProgram(program);

    // Evaluate Keyframes
    glm::vec4 eval_color = color;
    float eval_blend = blend_with_original;

    if (!red_track.keyframes.empty()) eval_color.r = red_track.Evaluate(ctx.time);
    if (!green_track.keyframes.empty()) eval_color.g = green_track.Evaluate(ctx.time);
    if (!blue_track.keyframes.empty()) eval_color.b = blue_track.Evaluate(ctx.time);
    if (!alpha_track.keyframes.empty()) eval_color.a = alpha_track.Evaluate(ctx.time);
    if (!blend_track.keyframes.empty()) eval_blend = blend_track.Evaluate(ctx.time);


    glUniform4f(glGetUniformLocation(program, "u_SolidColor"), eval_color.r, eval_color.g, eval_color.b, eval_color.a);
    glUniform1f(glGetUniformLocation(program, "u_BlendWithOriginal"), eval_blend);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, ctx.input_texture);
    glUniform1i(glGetUniformLocation(program, "u_OriginalTexture"), 0);

    RenderFullscreenQuad(); // Your existing utility

    glUseProgram(0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void GradientEffectNode::Process(const EffectContext& ctx) {
    if (!enabled || ctx.input_texture == 0) {
        return;
    }

    static GLuint program = 0;
    if (program == 0) {
        program = LoadShaderProgram("shaders/passthrough.vert", "shaders/gradient.frag");
        if (program == 0) return;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, ctx.output_fbo);
    glViewport(0, 0, (int)ctx.resolution.x, (int)ctx.resolution.y);

    glUseProgram(program);

    // Evaluate Keyframes (example for alpha and blend)
    glm::vec4 eval_color_start = color_start;
    glm::vec4 eval_color_end = color_end;
    float eval_blend = blend_with_original;

    if (!start_color_alpha_track.keyframes.empty()) eval_color_start.a = start_color_alpha_track.Evaluate(ctx.time);
    if (!end_color_alpha_track.keyframes.empty()) eval_color_end.a = end_color_alpha_track.Evaluate(ctx.time);
    if (!blend_track.keyframes.empty()) eval_blend = blend_track.Evaluate(ctx.time);
    // Add more keyframe evaluations for colors, points, radii if implemented


    glUniform1i(glGetUniformLocation(program, "u_GradientType"), static_cast<int>(type));
    glUniform4f(glGetUniformLocation(program, "u_ColorStart"), eval_color_start.r, eval_color_start.g, eval_color_start.b, eval_color_start.a);
    glUniform4f(glGetUniformLocation(program, "u_ColorEnd"), eval_color_end.r, eval_color_end.g, eval_color_end.b, eval_color_end.a);

    if (type == GradientType::Linear) {
        glUniform2f(glGetUniformLocation(program, "u_LinearStartPoint"), start_point.x, start_point.y);
        glUniform2f(glGetUniformLocation(program, "u_LinearEndPoint"), end_point.x, end_point.y);
    } else if (type == GradientType::Radial) {
        glUniform2f(glGetUniformLocation(program, "u_RadialCenterPoint"), center_point.x, center_point.y);
        glUniform1f(glGetUniformLocation(program, "u_RadialRadiusInner"), radius_inner);
        glUniform1f(glGetUniformLocation(program, "u_RadialRadiusOuter"), radius_outer);
        float aspect = (ctx.resolution.y > 0) ? (float)ctx.resolution.x / ctx.resolution.y : 1.0f;
        glUniform1f(glGetUniformLocation(program, "u_RadialAspectRatio"), aspect_ratio != 1.0f ? aspect_ratio : aspect); // Allow override or use viewport
    }

    glUniform1f(glGetUniformLocation(program, "u_BlendWithOriginal"), eval_blend);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, ctx.input_texture);
    glUniform1i(glGetUniformLocation(program, "u_OriginalTexture"), 0);

    RenderFullscreenQuad();

    glUseProgram(0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void DropShadowEffectNode::EnsureTempResources(int width, int height) {
    bool recreate = false;
    if (temp_fbo1 == 0) recreate = true;

    // Check if texture sizes match current resolution (optional, but good for performance)
    if (!recreate && temp_tex1_alpha_mask != 0) {
        GLint tex_w, tex_h;
        glBindTexture(GL_TEXTURE_2D, temp_tex1_alpha_mask);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &tex_w);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &tex_h);
        glBindTexture(GL_TEXTURE_2D, 0);
        if (tex_w != width || tex_h != height) recreate = true;
    }

    if (recreate) {
        if (temp_fbo1 != 0) glDeleteFramebuffers(1, &temp_fbo1);
        if (temp_tex1_alpha_mask != 0) glDeleteTextures(1, &temp_tex1_alpha_mask);
        if (temp_tex2_blurred_alpha != 0) glDeleteTextures(1, &temp_tex2_blurred_alpha);

        glGenFramebuffers(1, &temp_fbo1);

        // Texture for isolated alpha mask (single channel needed, but RGB often easier with FBOs)
        glGenTextures(1, &temp_tex1_alpha_mask);
        glBindTexture(GL_TEXTURE_2D, temp_tex1_alpha_mask);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, width, height, 0, GL_RGB, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        // Texture for blurred alpha mask
        glGenTextures(1, &temp_tex2_blurred_alpha);
        glBindTexture(GL_TEXTURE_2D, temp_tex2_blurred_alpha);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, width, height, 0, GL_RGB, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        glBindTexture(GL_TEXTURE_2D, 0);
        std::cout << "DropShadowEffectNode: Recreated temp resources." << std::endl;
    }
}


void DropShadowEffectNode::Process(const EffectContext& ctx) {
    if (!enabled || ctx.input_texture == 0) {
        // If this node is disabled, the EffectGraph should ideally just pass
        // ctx.input_texture to the next node or to the final output_fbo.
        // If this Process is called directly, we might need to copy input to output here.
        // For now, assume EffectGraph handles passthrough of disabled nodes correctly.
        return;
    }

    EnsureTempResources((int)ctx.resolution.x, (int)ctx.resolution.y);
    if (temp_fbo1 == 0) { // Resources couldn't be created
        std::cerr << "DropShadow: Failed to ensure temporary resources. Passing through input." << std::endl;
        
        // --- CORRECTED FALLBACK ---
        // Render ctx.input_texture to ctx.output_fbo using a passthrough shader
        static GLuint passthrough_prog = 0; // Cache this simple shader
        if (passthrough_prog == 0) {
            // Assuming you have a "texture.frag" that just samples u_Texture and outputs it
            // and a "passthrough.vert"
            passthrough_prog = LoadShaderProgram("shaders/passthrough.vert", "shaders/texture.frag");
        }

        if (passthrough_prog != 0) {
            GLint last_fbo_fb; glGetIntegerv(GL_FRAMEBUFFER_BINDING, &last_fbo_fb); // Renamed to avoid conflict
            GLint last_vp_fb[4]; glGetIntegerv(GL_VIEWPORT, last_vp_fb);

            glBindFramebuffer(GL_FRAMEBUFFER, ctx.output_fbo);
            glViewport(0, 0, (int)ctx.resolution.x, (int)ctx.resolution.y);
            glUseProgram(passthrough_prog);

            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, ctx.input_texture);
            glUniform1i(glGetUniformLocation(passthrough_prog, "u_Texture"), 0); // Ensure your texture.frag uses u_Texture

            RenderFullscreenQuad(); // Your existing utility

            glUseProgram(0);
            glBindFramebuffer(GL_FRAMEBUFFER, last_fbo_fb);
            glViewport(last_vp_fb[0], last_vp_fb[1], last_vp_fb[2], last_vp_fb[3]);
        } else {
            std::cerr << "DropShadow: Passthrough shader for fallback failed to load." << std::endl;
            // If even passthrough fails, the output FBO will contain whatever was in it before.
        }
        return; // Exit Process method after fallback
    }

    // Shader Caching (basic)
    static GLuint extract_alpha_prog = 0;
    static GLuint blur_prog = 0; // Assuming you have a blur shader program accessible
    static GLuint composite_prog = 0;

    if (extract_alpha_prog == 0) extract_alpha_prog = LoadShaderProgram("shaders/passthrough.vert", "shaders/extract_alpha.frag");
    if (blur_prog == 0) blur_prog = LoadShaderProgram("shaders/blur.vert", "shaders/blur.frag"); // Your existing blur shader
    if (composite_prog == 0) composite_prog = LoadShaderProgram("shaders/passthrough.vert", "shaders/apply_shadow_and_composite.frag");

    if (extract_alpha_prog == 0 || blur_prog == 0 || composite_prog == 0) {
        std::cerr << "DropShadow: Failed to load one or more shaders." << std::endl;
        // Fallback: copy input to output
        // (Same blit logic as above if temp_fbo1 creation failed)
        return;
    }

    GLint last_fbo; glGetIntegerv(GL_FRAMEBUFFER_BINDING, &last_fbo);
    GLint last_vp[4]; glGetIntegerv(GL_VIEWPORT, last_vp);
    glViewport(0, 0, (int)ctx.resolution.x, (int)ctx.resolution.y);

    // --- Pass 1: Extract Alpha from (already masked) input ---
    glBindFramebuffer(GL_FRAMEBUFFER, temp_fbo1);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, temp_tex1_alpha_mask, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) { /* handle error */ glBindFramebuffer(GL_FRAMEBUFFER, last_fbo); return; }
    
    glUseProgram(extract_alpha_prog);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, ctx.input_texture); // Input is the result of previous effects (e.g. masking)
    glUniform1i(glGetUniformLocation(extract_alpha_prog, "u_InputTexture"), 0);
    RenderFullscreenQuad();

    // --- Pass 2: Horizontal Blur of the Alpha Mask ---
    // (Using your existing GaussianBlurNode logic as a template, simplified here)
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, temp_tex2_blurred_alpha, 0); // Output to blurred_alpha
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) { /* handle error */ }

    float eval_blur = blur_amount;
    if(!blur_amount_track.keyframes.empty()) eval_blur = blur_amount_track.Evaluate(ctx.time);


    glUseProgram(blur_prog);
    glUniform1f(glGetUniformLocation(blur_prog, "u_BlurAmount"), eval_blur);
    glUniform2f(glGetUniformLocation(blur_prog, "u_Direction"), 1.0f / ctx.resolution.x, 0.0f); // Horizontal
    glUniform2f(glGetUniformLocation(blur_prog, "u_Resolution"), ctx.resolution.x, ctx.resolution.y);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, temp_tex1_alpha_mask); // Input is the extracted alpha
    glUniform1i(glGetUniformLocation(blur_prog, "u_Texture"), 0);
    RenderFullscreenQuad();

    // --- Pass 3: Vertical Blur of the Alpha Mask (result stored in temp_tex1_alpha_mask for reuse) ---
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, temp_tex1_alpha_mask, 0); // Output back to tex1 (ping-pong)
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) { /* handle error */ }

    // glUseProgram(blur_prog); // Already active
    glUniform2f(glGetUniformLocation(blur_prog, "u_Direction"), 0.0f, 1.0f / ctx.resolution.y); // Vertical
    // u_BlurAmount and u_Resolution are still set
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, temp_tex2_blurred_alpha); // Input is H-blurred alpha
    // u_Texture uniform still set to 0
    RenderFullscreenQuad();
    // Now temp_tex1_alpha_mask contains the fully blurred shadow shape

    // --- Pass 4: Composite shadow and original content to final output FBO ---
    glBindFramebuffer(GL_FRAMEBUFFER, ctx.output_fbo);
    // glViewport is already set
    
    glUseProgram(composite_prog);

    glm::vec2 eval_offset = offset;
    if(!offset_x_track.keyframes.empty()) eval_offset.x = offset_x_track.Evaluate(ctx.time);
    if(!offset_y_track.keyframes.empty()) eval_offset.y = offset_y_track.Evaluate(ctx.time);

    glm::vec4 eval_shadow_color = shadow_color;
    if(!shadow_r_track.keyframes.empty()) eval_shadow_color.r = shadow_r_track.Evaluate(ctx.time);
    // ... evaluate G, B, A for shadow_color similarly ...


    // Convert normalized UV offset to pixel offset for precise control if needed,
    // or keep it normalized for resolution independence. The shader uses it as UV offset.
    glUniform2f(glGetUniformLocation(composite_prog, "u_ShadowOffset"), eval_offset.x, eval_offset.y);
    glUniform4f(glGetUniformLocation(composite_prog, "u_ShadowColor"), eval_shadow_color.r, eval_shadow_color.g, eval_shadow_color.b, eval_shadow_color.a);
    glUniform2f(glGetUniformLocation(composite_prog, "u_PixelSize"), 1.0f/ctx.resolution.x, 1.0f/ctx.resolution.y);


    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, ctx.input_texture); // Original (masked) content
    glUniform1i(glGetUniformLocation(composite_prog, "u_OriginalContentTexture"), 0);

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, temp_tex1_alpha_mask); // Blurred shadow mask
    glUniform1i(glGetUniformLocation(composite_prog, "u_BlurredShadowMaskTexture"), 1);

    RenderFullscreenQuad();

    // Restore
    glUseProgram(0);
    glBindFramebuffer(GL_FRAMEBUFFER, last_fbo);
    glViewport(last_vp[0], last_vp[1], last_vp[2], last_vp[3]);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, 0);
}

// Helper to get FBO containing a texture (simplified, assumes texture is ATTACHMENT0)
// This is a HACK. A robust solution would involve tracking FBO-texture relationships
// or passing FBOs directly. For now, if input_texture is from another FBO,
// we'd need that FBO's ID. If it's just a texture, we can't directly get its source FBO.
// This highlights a potential design issue if effects need to read from arbitrary source FBOs.
// For blitting, if ctx.input_texture is the color attachment of some FBO (say, `src_fbo`), then:
// GLuint get_fbo_containing_texture(GLuint texture_id) { /* ... complex ... */ return 0; }
// For the fallback, let's assume input_texture is directly usable for drawing.
// If blitting is needed, the source FBO of ctx.input_texture would be required.
// Since RenderFullscreenQuad directly samples input_texture, the fallback might be:
// just call RenderFullscreenQuad(ctx.input_texture) into ctx.output_fbo with a passthrough shader.
// For simplicity in the error path of Process:
// (Inside DropShadowEffectNode::Process, error handling)
// ...
// Fallback: copy input to output using a passthrough shader
// static GLuint passthrough_prog = 0;
// if(passthrough_prog == 0) passthrough_prog = LoadShaderProgram("shaders/passthrough.vert", "shaders/texture.frag");
// if(passthrough_prog != 0) {
//     glBindFramebuffer(GL_FRAMEBUFFER, ctx.output_fbo);
//     glUseProgram(passthrough_prog);
//     glActiveTexture(GL_TEXTURE0);
//     glBindTexture(GL_TEXTURE_2D, ctx.input_texture);
//     glUniform1i(glGetUniformLocation(passthrough_prog, "u_Texture"),0);
//     RenderFullscreenQuad();
//     glBindFramebuffer(GL_FRAMEBUFFER,0);
// }
// return;