#include <vector>
#include <memory>
#include <string>
#include <algorithm>
#include <glm/glm.hpp>
#include <glad/glad.h> // Include glad for OpenGL function loading
#include "effects.hpp"


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