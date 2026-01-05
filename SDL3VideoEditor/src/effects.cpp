#include <vector>
#include <memory>
#include <string>
#include <algorithm>
#include <filesystem>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glad/glad.h> // Include glad for OpenGL function loading
#include "effects.hpp"
#include "cv_utils.hpp"

// 1. Define this to enable advanced packing and SDF functions within stb_truetype.h
#define STBTT_RASTERIZER_VERSION 2

// 2. Define the implementation for stb_truetype itself.
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

// 3. The stb_image_write library is separate and does not conflict.
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"


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
    
    // --- THIS IS THE FIX ---
    // Use GL_RGBA to create a texture with an alpha channel.
    // Using GL_RGBA8 is a good practice to specify the internal format explicitly.
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (int)resolution.x, (int)resolution.y, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    // --- END FIX ---

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    // Add clamp to edge to prevent artifacts from transforms
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

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

void EffectGraph::cleanup_transient_resources() {
    for (GLuint tex_id : transient_textures) {
        if (tex_id != 0) { // Safety check
            glDeleteTextures(1, &tex_id);
        }
    }
    transient_textures.clear();
}

// Private helper function to recursively evaluate
void EffectGraph::evaluate_node(int node_id, const EffectContext& base_ctx, GLuint source_clip_texture) {
    auto& node = nodes.at(node_id);
    if (node->is_evaluated_this_frame) {
        return; // Already processed this node, its result is ready.
    }

    // --- SPECIAL HANDLING FOR SOURCE NODES ---
    if (node_id == input_node_id) {
        // Check if it's a standard source node or an empty source node
        if (auto source_node = std::dynamic_pointer_cast<SourceClipNode>(node)) {
            // Its result is just the texture passed in from the clip.
            node->result_texture = source_clip_texture;
        }
        else if (auto empty_source_node = std::dynamic_pointer_cast<EmptySourceNode>(node)) {
            // An EmptySourceNode must generate its own texture by running its process.
            GLuint output_tex;
            GLuint temp_fbo = create_temp_fbo(base_ctx.resolution, output_tex);
            transient_textures.push_back(output_tex); // Manage memory
            EffectContext node_ctx = base_ctx;
            node_ctx.output_fbo = temp_fbo;
            node->Process({}, {}, node_ctx); // Call its process to clear the texture
            node->result_texture = output_tex;
            glDeleteFramebuffers(1, &temp_fbo);
        }
        node->is_evaluated_this_frame = true;
        return; // Source nodes have no inputs, so we are done.
    }

    // --- NEW: Passthrough logic for disabled nodes ---
    if (!node->enabled) {
        // A disabled node's result is simply the result of its first input.
        // This makes it act like a straight wire.

        // We still need to evaluate the upstream node to get its texture.
        // This logic assumes a simple chain (one input). For multi-input nodes,
        // you might pass through the primary input (e.g., input_pins[0]).
        GLuint passthrough_texture = 0;
        if (!node->input_pins.empty()) {
            // Find the link connected to this node's first input pin.
            for (const auto& link : links) {
                if (link.to_pin_id == node->input_pins[0].id) {
                    // Recursively evaluate the node that feeds into this disabled one.
                    evaluate_node(link.from_node_id, base_ctx, source_clip_texture);
                    
                    // The result we pass through is the result from that upstream node.
                    passthrough_texture = nodes.at(link.from_node_id)->result_texture;
                    break;
                }
            }
        }

        // Set this disabled node's result to be its input's result.
        node->result_texture = passthrough_texture;
        node->is_evaluated_this_frame = true;
        return; // CRITICAL: Skip the rest of the function.
    }

    // --- UPDATED: Input Gathering ---
    std::vector<GLuint> image_inputs;
    std::map<int, std::any> data_inputs;

    for (const auto& input_pin : node->input_pins) {
        for (const auto& link : links) {
            if (link.to_pin_id == input_pin.id) {
                evaluate_node(link.from_node_id, base_ctx, source_clip_texture);
                
                auto& from_node = nodes.at(link.from_node_id);
                // Find the specific output pin on the source node that this link comes from
                Pin* from_pin = find_pin_by_id(from_node.get(), link.from_pin_id);

                if (from_pin) {
                    if (from_pin->type == PinType::Image) {
                        image_inputs.push_back(from_node->result_texture);
                    } else {
                        // If it's a data pin, find the data in the source node's output map
                        auto data_it = from_node->data_outputs.find(from_pin->id);
                        if (data_it != from_node->data_outputs.end()) {
                            data_inputs[input_pin.id] = data_it->second;
                        }
                    }
                }
                break;
            }
        }
    }

    // 2. Process this node.
    GLuint output_tex_for_this_node = 0;
    GLuint temp_fbo = create_temp_fbo(base_ctx.resolution, output_tex_for_this_node);
    
    // Add the newly created texture to our list for cleanup at the end of the frame.
    transient_textures.push_back(output_tex_for_this_node);
    
    EffectContext node_ctx = base_ctx;
    node_ctx.output_fbo = temp_fbo;
    
    node->Process(image_inputs, data_inputs, node_ctx);

    // 3. Store the result and mark the node as finished for this frame.
    node->result_texture = output_tex_for_this_node;
    node->is_evaluated_this_frame = true;
    
    glDeleteFramebuffers(1, &temp_fbo); // The FBO is no longer needed, but the texture is.
}


void EffectGraph::rebuild_links_from_order() {
    // 1. Clear all existing links.
    links.clear();
    static int next_link_id = 1;

    // --- NEW: Auto-layout logic ---
    const float START_X = 50.0f;
    const float START_Y = 100.0f;
    const float HORIZONTAL_SPACING = 250.0f;
    float current_x = START_X;
    // --- END NEW ---

    // 2. Position and connect the Source Clip node.
    auto& source_node = nodes.at(input_node_id);
    source_node->editor_pos = ImVec2(current_x, START_Y);
    current_x += HORIZONTAL_SPACING;

    // 3. Handle the main chain of effects.
    if (node_order.empty()) {
        // If no effects, connect Source directly to Output.
        auto& output_node = nodes.at(output_node_id);
        links.push_back({
            next_link_id++,
            input_node_id, output_node_id,
            source_node->output_pins[0].id, output_node->input_pins[0].id
        });
    } else {
        // Connect Source to the first effect.
        int first_effect_id = node_order.front();
        auto& first_effect_node = nodes.at(first_effect_id);
        links.push_back({
            next_link_id++,
            input_node_id, first_effect_id,
            source_node->output_pins[0].id, first_effect_node->input_pins[0].id
        });

        // Loop through effects, positioning and linking them.
        for (size_t i = 0; i < node_order.size(); ++i) {
            int current_node_id = node_order[i];
            auto& current_node = nodes.at(current_node_id);
            current_node->editor_pos = ImVec2(current_x, START_Y);
            current_x += HORIZONTAL_SPACING;

            // If not the last node, link it to the next one.
            if (i < node_order.size() - 1) {
                int next_node_id = node_order[i + 1];
                auto& next_node = nodes.at(next_node_id);
                links.push_back({
                    next_link_id++,
                    current_node_id, next_node_id,
                    current_node->output_pins[0].id, next_node->input_pins[0].id
                });
            }
        }

        // Connect the last effect to the Final Output.
        int last_effect_id = node_order.back();
        auto& last_effect_node = nodes.at(last_effect_id);
        auto& output_node = nodes.at(output_node_id);
        links.push_back({
            next_link_id++,
            last_effect_id, output_node_id,
            last_effect_node->output_pins[0].id, output_node->input_pins[0].id
        });
    }
    
    // 4. Position the Final Output node last.
    auto& output_node = nodes.at(output_node_id);
    output_node->editor_pos = ImVec2(current_x, START_Y);
}

void EffectGraph::rebuild_order_from_links() {
    node_order.clear(); // Start with a fresh list

    int current_node_id = input_node_id;
    int safety_break = 0; // Prevents infinite loops if user creates a cycle

    while (current_node_id != output_node_id && safety_break < nodes.size()) {
        bool found_next = false;
        // Find the link that starts at the current node
        for (const auto& link : links) {
            if (link.from_node_id == current_node_id) {
                int next_node_id = link.to_node_id;
                
                // Only add user-editable effect nodes to the order list
                if (next_node_id != output_node_id) {
                    node_order.push_back(next_node_id);
                }
                
                current_node_id = next_node_id;
                found_next = true;
                break; // Found the next node in the chain
            }
        }
        if (!found_next) {
            break; // The chain is broken, stop traversing
        }
        safety_break++;
    }
}

void EffectGraph::insert_node_before_output(std::shared_ptr<EffectNode> new_node) {
    int new_id = next_node_id++;
    new_node->id = new_id;
    
    // --- NEW: Auto-positioning logic ---
    ImVec2 from_pos, to_pos;
    // --- END NEW ---

    // Find the link that currently goes into the final output node
    auto it = std::find_if(links.begin(), links.end(), [&](const Link& link) {
        return link.to_node_id == output_node_id;
    });

    if (it != links.end()) {
        Link old_link = *it;
        
        // --- NEW: Get the positions of the nodes we are inserting between ---
        from_pos = nodes.at(old_link.from_node_id)->editor_pos;
        to_pos = nodes.at(old_link.to_node_id)->editor_pos;
        // --- END NEW ---

        links.erase(it);

        links.push_back({
            next_node_id++,
            old_link.from_node_id, new_id,
            old_link.from_pin_id, new_node->input_pins[0].id
        });
        
        links.push_back({
            next_node_id++,
            new_id, output_node_id,
            new_node->output_pins[0].id, nodes.at(output_node_id)->input_pins[0].id
        });
    }

    // --- NEW: Calculate and set the new node's position ---
    // Place it halfway between the 'from' and 'to' nodes.
    new_node->editor_pos.x = from_pos.x + (to_pos.x - from_pos.x) * 0.5f;
    new_node->editor_pos.y = from_pos.y + (to_pos.y - from_pos.y) * 0.5f;
    // Nudge the final output node to the right to make space.
    nodes.at(output_node_id)->editor_pos.x = to_pos.x + 150.0f; // Adjust spacing as needed
    // --- END NEW ---

    // Finally, add the node to the map and update the simple order list.
    nodes[new_id] = new_node;
    rebuild_order_from_links();
}

void EffectGraph::ProcessNodeGraph(GLuint source_clip_texture, GLuint final_output_fbo, float time, glm::vec2 resolution, int fps) {
    if (nodes.empty() || output_node_id == 0) return;

    // --- Step 2 becomes Step 1: Reset the evaluation state for the CURRENT frame ---
    for (auto& [id, node] : nodes) {
        node->is_evaluated_this_frame = false;
        node->result_texture = 0;
    }

    // --- Step 4 becomes Step 3: Start the recursive evaluation ---
    EffectContext base_ctx = { 0, 0, time, resolution, fps };
    evaluate_node(output_node_id, base_ctx, source_clip_texture);

    // --- 5. Copy the final result to the destination FBO ---
    // After evaluation, the final image is stored in the output node's input's source.
    GLuint final_texture = 0;
    for (const auto& link : links) {
        if (link.to_node_id == output_node_id) {
            final_texture = nodes.at(link.from_node_id)->result_texture;
            break;
        }
    }

    if (final_texture != 0) {
        // Use a simple passthrough shader to copy the final texture to the output FBO.
        static GLuint copy_shader = 0;
        if (copy_shader == 0) copy_shader = LoadShaderProgram("shaders/passthrough.vert", "shaders/texture.frag");
        if (copy_shader == 0) return;

        glBindFramebuffer(GL_FRAMEBUFFER, final_output_fbo);
        glViewport(0, 0, (int)resolution.x, (int)resolution.y);
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        glUseProgram(copy_shader);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, final_texture);
        glUniform1i(glGetUniformLocation(copy_shader, "u_Texture"), 0);

        RenderFullscreenQuad();

        glUseProgram(0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    } else {
        // Handle case where output node is not connected
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

void GaussianBlurNode::Process(const std::vector<GLuint>& image_inputs, const std::map<int, std::any>& data_inputs, const EffectContext& ctx) {
    if (!enabled || image_inputs.empty() || image_inputs[0] == 0) return;
        
    // Load shader program (consider caching this instead of loading every frame)
    GLuint blur_shader_program = LoadShaderProgram("shaders/blur.vert", "shaders/blur.frag");
    if (blur_shader_program == 0) return;

    GLuint input_texture = image_inputs[0];
        
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
    // --- ADD CLEAR HERE FOR FIRST PASS ---
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    
    glUseProgram(blur_shader_program);
    
    // Set uniforms
    glUniform1f(glGetUniformLocation(blur_shader_program, "u_BlurAmount"), blur_amount);
    glUniform2f(glGetUniformLocation(blur_shader_program, "u_Direction"), 1.0f, 0.0f);  // Horizontal
    glUniform2f(glGetUniformLocation(blur_shader_program, "u_Resolution"), ctx.resolution.x, ctx.resolution.y);
    
    // Bind input texture
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, input_texture);
    glUniform1i(glGetUniformLocation(blur_shader_program, "u_Texture"), 0);
    
    // Draw
    glBindVertexArray(quadVAO);
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);
    
    // Second pass: vertical blur
    glBindFramebuffer(GL_FRAMEBUFFER, ctx.output_fbo);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
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

void ColorGradingNode::Process(const std::vector<GLuint>& image_inputs, const std::map<int, std::any>& data_inputs, const EffectContext& ctx) {
    if (!enabled || image_inputs.empty() || image_inputs[0] == 0) return;

    GLuint input_texture = image_inputs[0];
    
    // Load shader program (consider caching this instead of loading every frame)
    GLuint color_grade_program = LoadShaderProgram("shaders/colorgrade.vert", "shaders/colorgrade.frag");
    if (color_grade_program == 0) return;
    
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
    glBindTexture(GL_TEXTURE_2D, input_texture);
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

void LUTColorGradingNode::Process(const std::vector<GLuint>& image_inputs, const std::map<int, std::any>& data_inputs, const EffectContext& ctx) {
    if (!enabled || image_inputs.empty() || image_inputs[0] == 0 || lut_texture == 0) return;

    GLuint input_texture = image_inputs[0];
    
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
    glBindTexture(GL_TEXTURE_2D, input_texture);
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
void MaskEffectNode::Process(const std::vector<GLuint>& image_inputs, const std::map<int, std::any>& data_inputs, const EffectContext& ctx) {
    if (!enabled || image_inputs.empty() || image_inputs[0] == 0 || mask_type == MaskType::None) return;

    GLuint input_texture = image_inputs[0];

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
    glBindTexture(GL_TEXTURE_2D, input_texture);
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
                                const cv::Rect& roi_for_rect_mode_px,
                                const cv::Mat& scribble_mask_for_mask_mode_cv,
                                cv::Mat& bgdModel, // Now an in/out parameter
                                cv::Mat& fgdModel, // Now an in/out parameter
                                bool is_refinement) {
    // ... (frame validation) ...
    cv::Mat image_bgr = DecodedFrameToCvMat(current_clip_frame);
    // ... (image validation) ...

    cv::Mat result_mask_cv;
    cv::Rect actual_roi_for_grabcut = roi_for_rect_mode_px;

    // Determine the grabCut mode based on initialization or refinement
    int grabcut_mode;
    if (!is_refinement) {
        // This is the very first run
        if (mode == GrabCutInitMode::RECT) {
            grabcut_mode = cv::GC_INIT_WITH_RECT;
        } else { // MASK
            grabcut_mode = cv::GC_INIT_WITH_MASK;
        }
    } else {
        // If we are refining, we always use the mask information.
        // GC_EVAL is for just viewing, we want to continue iterating.
        grabcut_mode = cv::GC_INIT_WITH_MASK; 
    }

    try {
        if (mode == GrabCutInitMode::RECT) {
            // ... (this path is mainly for the first stroke now) ...
            // INCREASED iteration count from 5 to 10 for better quality
            cv::grabCut(image_bgr, result_mask_cv, actual_roi_for_grabcut, bgdModel, fgdModel, 10, (cv::GrabCutModes)grabcut_mode);
        } else { // MASK (used for all refinements)
            // This is the CRITICAL FIX.
            // We must provide a mask that contains hints from the PREVIOUS iteration,
            // plus the new user-painted scribbles. Using only the scribbles
            // will fail if the user only paints one type (e.g., only background).

            // Start with the result from the last iteration.
            this->last_grabcut_mask_cv.copyTo(result_mask_cv);

            // Overlay the new user hints.
            result_mask_cv.setTo(cv::GC_FGD, scribble_mask_for_mask_mode_cv == cv::GC_FGD);
            result_mask_cv.setTo(cv::GC_BGD, scribble_mask_for_mask_mode_cv == cv::GC_BGD);

            cv::Rect processing_rect(0, 0, image_bgr.cols, image_bgr.rows);
            // Refinement only needs a few iterations to converge.
            cv::grabCut(image_bgr, result_mask_cv, processing_rect, bgdModel, fgdModel, 1, cv::GC_INIT_WITH_MASK);
        }
    } catch (const cv::Exception& e) {
        std::cerr << "OpenCV Exception in grabCut: " << e.what() << std::endl;
        return 0;
    }

    result_mask_cv.copyTo(this->last_grabcut_mask_cv);

    // Convert the resulting mask (which contains 0,1,2,3 values) to a visible texture
    int tex_w, tex_h;
    // We now just need the raw mask data for contour finding, so a texture isn't created here.
    // Instead, we will process this result_mask_cv in the main UI loop.
    // For now, we will return a temporary texture for the overlay effect.
    GLuint generated_texture_id = GrabCutMaskToRGBTexture(result_mask_cv, tex_w, tex_h, true);

    // This logic is now handled in the UI loop to prepare the mask for the *next* iteration.
    // if(mode == GrabCutInitMode::MASK){
    //     cv::compare(scribble_mask_for_mask_mode_cv, cv::GC_FGD, result_mask_cv, cv::CMP_EQ);
    //     cv::compare(scribble_mask_for_mask_mode_cv, cv::GC_BGD, result_mask_cv, cv::CMP_EQ);
    // }

    std::cout << "GrabCut successful." << std::endl;
    return generated_texture_id;
}

void SolidColorEffectNode::Process(const std::vector<GLuint>& image_inputs, const std::map<int, std::any>& data_inputs, const EffectContext& ctx) {
    if (!enabled || image_inputs.empty() || image_inputs[0] == 0) return;

    GLuint input_texture = image_inputs[0];

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
    glBindTexture(GL_TEXTURE_2D, input_texture);
    glUniform1i(glGetUniformLocation(program, "u_OriginalTexture"), 0);

    RenderFullscreenQuad(); // Your existing utility

    glUseProgram(0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void GradientEffectNode::Process(const std::vector<GLuint>& image_inputs, const std::map<int, std::any>& data_inputs, const EffectContext& ctx) {
    if (!enabled || image_inputs.empty() || image_inputs[0] == 0) return;

    GLuint input_texture = image_inputs[0];

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
    float eval_intensity = intensity;
    
    if (!start_color_alpha_track.keyframes.empty()) eval_color_start.a = start_color_alpha_track.Evaluate(ctx.time);
    if (!end_color_alpha_track.keyframes.empty()) eval_color_end.a = end_color_alpha_track.Evaluate(ctx.time);
    if (!intensity_track.keyframes.empty()) eval_intensity = intensity_track.Evaluate(ctx.time);
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

    glUniform1f(glGetUniformLocation(program, "u_Intensity"), eval_intensity);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, input_texture);
    glUniform1i(glGetUniformLocation(program, "u_OriginalTexture"), 0);

    RenderFullscreenQuad();

    glUseProgram(0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void DropShadowEffectNode::EnsureTempResources(int width, int height) {
    // This function now uses the create_temp_fbo helper for cleaner code.
    // Check if resources exist and match the required size.
    if (temp_fbo1 != 0 && temp_tex1 != 0) {
        GLint tex_w, tex_h;
        glBindTexture(GL_TEXTURE_2D, temp_tex1);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &tex_w);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &tex_h);
        glBindTexture(GL_TEXTURE_2D, 0);
        if (tex_w == width && tex_h == height) {
            return; // Resources are already valid.
        }
    }

    // If we reached here, we need to (re)create everything.
    if (temp_fbo1 != 0) destroy_temp_fbo(temp_fbo1, temp_tex1);
    if (temp_fbo2 != 0) destroy_temp_fbo(temp_fbo2, temp_tex2);

    temp_fbo1 = create_temp_fbo(glm::vec2(width, height), temp_tex1);
    temp_fbo2 = create_temp_fbo(glm::vec2(width, height), temp_tex2);

    if (temp_fbo1 == 0 || temp_fbo2 == 0) {
        std::cerr << "DropShadow: Failed to create temporary FBOs." << std::endl;
    }
}


void DropShadowEffectNode::Process(const std::vector<GLuint>& image_inputs, const std::map<int, std::any>& data_inputs, const EffectContext& ctx) {
    if (!enabled || image_inputs.empty() || image_inputs[0] == 0) {
        // If disabled, we must do a passthrough to not break the chain.
        // The evaluation engine's passthrough logic handles this.
        return;
    }
    GLuint input_texture = image_inputs[0];

    EnsureTempResources((int)ctx.resolution.x, (int)ctx.resolution.y);
    if (temp_fbo1 == 0 || temp_fbo2 == 0) return; // Can't proceed without resources

    // Shader Caching
    static GLuint extract_alpha_prog = 0;
    static GLuint blur_prog = 0;
    static GLuint composite_prog = 0;
    if (extract_alpha_prog == 0) extract_alpha_prog = LoadShaderProgram("shaders/blur.vert", "shaders/extract_alpha.frag");
    if (blur_prog == 0) blur_prog = LoadShaderProgram("shaders/blur.vert", "shaders/blur.frag");
    if (composite_prog == 0) composite_prog = LoadShaderProgram("shaders/blur.vert", "shaders/apply_shadow_and_composite.frag");
    if (!extract_alpha_prog || !blur_prog || !composite_prog) return;

    // Backup GL state
    GLint last_fbo; glGetIntegerv(GL_FRAMEBUFFER_BINDING, &last_fbo);
    GLint last_vp[4]; glGetIntegerv(GL_VIEWPORT, last_vp);
    glViewport(0, 0, (int)ctx.resolution.x, (int)ctx.resolution.y);

    // --- Pass 1: Extract Alpha from input texture -> temp_fbo1 ---
    glBindFramebuffer(GL_FRAMEBUFFER, temp_fbo1);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(extract_alpha_prog);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, input_texture);
    glUniform1i(glGetUniformLocation(extract_alpha_prog, "u_InputTexture"), 0);
    RenderFullscreenQuad();

    // --- Pass 2: Horizontal Blur from temp_tex1 -> temp_fbo2 ---
    glBindFramebuffer(GL_FRAMEBUFFER, temp_fbo2);
    glClear(GL_COLOR_BUFFER_BIT); // No need for glClearColor again
    glUseProgram(blur_prog);
    glUniform1f(glGetUniformLocation(blur_prog, "u_BlurAmount"), blur_amount);
    glUniform2f(glGetUniformLocation(blur_prog, "u_Resolution"), ctx.resolution.x, ctx.resolution.y);
    glUniform2f(glGetUniformLocation(blur_prog, "u_Direction"), 1.0f, 0.0f);
    glBindTexture(GL_TEXTURE_2D, temp_tex1); // Read from the result of Pass 1
    glUniform1i(glGetUniformLocation(blur_prog, "u_Texture"), 0);
    RenderFullscreenQuad();

    // --- Pass 3: Vertical Blur from temp_tex2 -> temp_fbo1 ---
    glBindFramebuffer(GL_FRAMEBUFFER, temp_fbo1);
    glClear(GL_COLOR_BUFFER_BIT);
    // glUseProgram(blur_prog) is still active
    glUniform2f(glGetUniformLocation(blur_prog, "u_Direction"), 0.0f, 1.0f);
    glBindTexture(GL_TEXTURE_2D, temp_tex2); // Read from the result of Pass 2
    RenderFullscreenQuad();
    // Now, temp_tex1 contains the final, fully blurred shadow mask.

    // --- Pass 4: Composite original and blurred mask -> final output_fbo ---
    glBindFramebuffer(GL_FRAMEBUFFER, ctx.output_fbo);
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(composite_prog);
    glUniform2f(glGetUniformLocation(composite_prog, "u_ShadowOffset"), offset.x, offset.y);
    glUniform4f(glGetUniformLocation(composite_prog, "u_ShadowColor"), shadow_color.r, shadow_color.g, shadow_color.b, shadow_color.a);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, input_texture); // The original content
    glUniform1i(glGetUniformLocation(composite_prog, "u_OriginalContentTexture"), 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, temp_tex1); // The final blurred shadow mask
    glUniform1i(glGetUniformLocation(composite_prog, "u_BlurredShadowMaskTexture"), 1);
    RenderFullscreenQuad();

    // Restore GL state
    glBindFramebuffer(GL_FRAMEBUFFER, last_fbo);
    glViewport(last_vp[0], last_vp[1], last_vp[2], last_vp[3]);
    glUseProgram(0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void ChromaKeyNode::Process(const std::vector<GLuint>& image_inputs, const std::map<int, std::any>& data_inputs, const EffectContext& ctx) {
    if (!enabled || image_inputs.empty() || image_inputs[0] == 0) return;

    GLuint input_texture = image_inputs[0];

    // Load shader program (consider caching this for performance)
    GLuint chroma_key_program = LoadShaderProgram("shaders/passthrough.vert", "shaders/chroma_key.frag");
    if (chroma_key_program == 0) {
        return;
    }

    // Setup render state
    glBindFramebuffer(GL_FRAMEBUFFER, ctx.output_fbo);
    glViewport(0, 0, (int)ctx.resolution.x, (int)ctx.resolution.y);

    // --- THIS IS THE CRITICAL FIX ---
    // Clear the destination framebuffer to transparent black before drawing.
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    // --- END FIX ---

    glUseProgram(chroma_key_program);

    // Set uniforms for the chroma key effect
    glUniform3f(glGetUniformLocation(chroma_key_program, "u_KeyColor"), key_color.r, key_color.g, key_color.b);
    glUniform1f(glGetUniformLocation(chroma_key_program, "u_Similarity"), similarity);
    glUniform1f(glGetUniformLocation(chroma_key_program, "u_Blend"), blend);
    glUniform1f(glGetUniformLocation(chroma_key_program, "u_Spill"), spill);

    // Bind the input texture to texture unit 0
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, input_texture);
    glUniform1i(glGetUniformLocation(chroma_key_program, "u_Texture"), 0);

    // Draw a fullscreen quad to apply the effect
    RenderFullscreenQuad();

    // Cleanup
    glUseProgram(0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void TextEffectNode::RebakeFont() {
    if (font_path.empty() || !std::filesystem::exists(font_path)) {
        needs_rebake = false;
        return;
    }

    std::ifstream file(font_path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) return;
    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);
    std::vector<char> font_buffer(size);
    if (!file.read(font_buffer.data(), size)) return;

    const int ATLAS_WIDTH = 2048;
    const int ATLAS_HEIGHT = 2048;
    // Variables formerly defined here (NUM_CHARS, PIXEL_DIST_SCALE) are now local or inlined
    unsigned char* sdf_atlas_data = new unsigned char[ATLAS_WIDTH * ATLAS_HEIGHT];
    // Clear to transparent (or 0 distance)
    std::memset(sdf_atlas_data, 0, ATLAS_WIDTH * ATLAS_HEIGHT);
    
    stbtt_fontinfo font_info;
    if (!stbtt_InitFont(&font_info, (const unsigned char*)font_buffer.data(), 0)) {
        delete[] sdf_atlas_data;
        return;
    }

    float scale = stbtt_ScaleForPixelHeight(&font_info, baked_font_size);

    stbtt_pack_context spc;
    // Just 1 pixel padding between packed glyphs for safety
    stbtt_PackBegin(&spc, sdf_atlas_data, ATLAS_WIDTH, ATLAS_HEIGHT, 0, 1, nullptr);
    
    stbtt_PackSetOversampling(&spc, 1, 1);
    
    // We need a bitmap for the atlas. 
    // SDFs are usually smaller than full raster, but with padding they take space.
    // 512x512 might be enough for small sets, but let's go 1024x1024 for safety with 96px font.
    int NEW_ATLAS_WIDTH = 2048;
    int NEW_ATLAS_HEIGHT = 2048;
    std::vector<unsigned char> atlas_pixels(NEW_ATLAS_WIDTH * NEW_ATLAS_HEIGHT);
    
    // We use stbrp logic to pack rects
    stbtt_PackBegin(&spc, atlas_pixels.data(), NEW_ATLAS_WIDTH, NEW_ATLAS_HEIGHT, 0, 1, nullptr);
    
    // We need to gather rects for our char range (32-126)
    // AND we must account for the padding required by SDF.
    int padding = 5; // SDF padding
    int onedge_value = 128; // 0-255 range, 128 is the edge
    int pixel_dist_scale = onedge_value / padding; 

    // stbtt_PackFontRangesGatherRects is not enough because it doesn't know about SDF padding needs?
    // Actually, we can just manually allocate stbrp_rects.
    
    int num_chars = 96; // 32 to 127
    std::vector<stbrp_rect> rects(num_chars);
    for (int i = 0; i < num_chars; ++i) {
        int codepoint = i + 32;
        int x0, y0, x1, y1;
        stbtt_GetCodepointBitmapBox(&font_info, codepoint, scale, scale, &x0, &y0, &x1, &y1);
        rects[i].id = i;
        rects[i].w = (stbrp_coord)((x1 - x0) + 2 * padding);
        rects[i].h = (stbrp_coord)((y1 - y0) + 2 * padding);
    }
    
    stbtt_PackFontRangesPackRects(&spc, rects.data(), num_chars);
    stbtt_PackEnd(&spc);

    // Now generated the SDFs into the allocated rects
    // We need to manually fill the texture now using the rect info
    // And populate pdata manually.
    
    for (int i = 0; i < num_chars; ++i) {
        if (rects[i].was_packed) {
            int codepoint = i + 32;
            
            // Generate SDF
            int w_sdf, h_sdf, xoff_sdf, yoff_sdf;
            unsigned char* sdf_bitmap = stbtt_GetGlyphSDF(&font_info, scale, stbtt_FindGlyphIndex(&font_info, codepoint), padding, onedge_value, pixel_dist_scale, &w_sdf, &h_sdf, &xoff_sdf, &yoff_sdf);
            
            if (sdf_bitmap) {
                 // Copy to atlas
                 int target_x = rects[i].x;
                 int target_y = rects[i].y;
                 
                 for (int r = 0; r < h_sdf && (target_y + r) < NEW_ATLAS_HEIGHT; ++r) {
                     for (int c = 0; c < w_sdf && (target_x + c) < NEW_ATLAS_WIDTH; ++c) {
                         atlas_pixels[(target_y + r) * NEW_ATLAS_WIDTH + (target_x + c)] = sdf_bitmap[r * w_sdf + c];
                     }
                 }
            
                 stbtt_FreeSDF(sdf_bitmap, nullptr);
                 
                 // Fill pdata for rendering
                 // pdata[i] is stbtt_packedchar
                 // We need to adjust coordinates to be purely regarding the quad.
                 // The SDF bitmap includes padding. The 'xoff' and 'yoff' returned by GetGlyphSDF are the offset from the pen position to the top-left of the bitmap.
                 
                 stbtt_packedchar& pc = pdata[i];
                 pc.x0 = (unsigned short)rects[i].x;
                 pc.y0 = (unsigned short)rects[i].y;
                 pc.x1 = (unsigned short)(rects[i].x + w_sdf);
                 pc.y1 = (unsigned short)(rects[i].y + h_sdf);
                 
                 pc.xoff = (float)xoff_sdf;
                 pc.yoff = (float)yoff_sdf;
                 pc.xoff2 = (float)(xoff_sdf + w_sdf);
                 pc.yoff2 = (float)(yoff_sdf + h_sdf);
                 
                 int advance, lsb;
                 stbtt_GetCodepointHMetrics(&font_info, codepoint, &advance, &lsb);
                 pc.xadvance = advance * scale;
            }
        } else {
            // If a character wasn't packed, clear its pdata entry
            std::memset(&pdata[i], 0, sizeof(stbtt_packedchar));
        }
    }
    
    // (Optional) Save the atlas to disk to see what it looks like
    // stbi_write_png("sdf_atlas.png", NEW_ATLAS_WIDTH, NEW_ATLAS_HEIGHT, 1, atlas_pixels.data(), 0);

    if (font_atlas_tex != 0) glDeleteTextures(1, &font_atlas_tex);
    glGenTextures(1, &font_atlas_tex);
    glBindTexture(GL_TEXTURE_2D, font_atlas_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, NEW_ATLAS_WIDTH, NEW_ATLAS_HEIGHT, 0, GL_RED, GL_UNSIGNED_BYTE, atlas_pixels.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    // Needed for SDF to work well at edges
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // delete[] sdf_atlas_data; // No longer used/allocated
    needs_rebake = false;
    std::cout << "Re-baked SDF font: " << font_path << " at size " << font_size << std::endl;
}

void TextEffectNode::Process(const std::vector<GLuint>& image_inputs, const std::map<int, std::any>& data_inputs, const EffectContext& ctx) {
    if (!enabled) return;
    GLuint input_texture = image_inputs.empty() ? 0 : image_inputs[0];

    if (needs_rebake) {
        RebakeFont();
    }

    // --- 1. Draw the input video as the background ---
    glBindFramebuffer(GL_FRAMEBUFFER, ctx.output_fbo);
    glViewport(0, 0, (int)ctx.resolution.x, (int)ctx.resolution.y);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    
    static GLuint passthrough_shader = 0;
    if(passthrough_shader == 0) passthrough_shader = LoadShaderProgram("shaders/passthrough.vert", "shaders/texture.frag");

    if (input_texture != 0) {
        glUseProgram(passthrough_shader);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, input_texture);
        glUniform1i(glGetUniformLocation(passthrough_shader, "u_Texture"), 0);
        RenderFullscreenQuad();
    }

    // --- 2. Render the text on top using modern OpenGL ---
    if (font_atlas_tex == 0 || text_content.empty()) {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        return;
    }

    // --- Shader and VBO/VAO setup (done once) ---
    static GLuint font_shader = 0;
    static GLuint vao = 0, vbo = 0;
    if (font_shader == 0) {
        font_shader = LoadShaderProgram("shaders/font.vert", "shaders/font.frag");
        // Create a dynamic VBO for our text quads
        glGenVertexArrays(1, &vao);
        glGenBuffers(1, &vbo);
        glBindVertexArray(vao);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        // Position attribute
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(0);
        // Texture coordinate attribute
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
        glEnableVertexAttribArray(1);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glBindVertexArray(0);
    }

    // --- Prepare rendering state ---
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    
    glUseProgram(font_shader);

    // Set projection matrix
    glm::mat4 projection = glm::ortho(0.0f, (float)ctx.resolution.x, 0.0f, (float)ctx.resolution.y);
    glUniformMatrix4fv(glGetUniformLocation(font_shader, "u_Projection"), 1, GL_FALSE, &projection[0][0]);

    // Set texture and color uniforms
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, font_atlas_tex);
    glUniform1i(glGetUniformLocation(font_shader, "u_FontTexture"), 0);
    glUniform4f(glGetUniformLocation(font_shader, "u_TextColor"), text_color.r, text_color.g, text_color.b, text_color.a);

    glUniform1i(glGetUniformLocation(font_shader, "u_HasOutline"), has_outline);
    if (has_outline) {
        glUniform4f(glGetUniformLocation(font_shader, "u_OutlineColor"), outline_color.r, outline_color.g, outline_color.b, outline_color.a);
        glUniform1f(glGetUniformLocation(font_shader, "u_OutlineThickness"), outline_thickness);
    }

    glBindVertexArray(vao);
    
    // --- Generate vertex data for the text string ---
    std::vector<float> vertices;
    
    // 1. Calculate Total Width for Alignment
    float render_scale = font_size / baked_font_size;
    float total_width = 0.0f;
    for (const char* text_ptr = text_content.c_str(); *text_ptr; text_ptr++) {
        if (*text_ptr >= 32 && *text_ptr < 128) {
            // xadvance is pre-scaled by RebakeFont (to baked_font_size)
            // We must scale it to render_font_size
            total_width += pdata[*text_ptr - 32].xadvance * render_scale;
            // Add custom letter spacing (if not at the very end, though standard is to just add it)
            if (*(text_ptr + 1)) total_width += letter_spacing;
        }
    }

    // 2. Determine Start X based on Alignment
    float x = position.x * ctx.resolution.x;
    float y = position.y * ctx.resolution.y;

    if (alignment == Alignment::Center) {
        x -= total_width * 0.5f;
    } else if (alignment == Alignment::Right) {
        x -= total_width;
    }

    // 3. Draw Loop
    // render_scale is already calculated above
    float start_x = x; // Keep track of line start if we add multiline later.

    for (const char* text_ptr = text_content.c_str(); *text_ptr; text_ptr++) {
        if (*text_ptr >= 32 && *text_ptr < 128) {
            stbtt_aligned_quad q;
            // GetPackedQuad uses the BAKED size coordinates.
            float dummy_x = 0; float dummy_y = 0;
            stbtt_GetPackedQuad(pdata, 2048, 2048, *text_ptr - 32, &dummy_x, &dummy_y, &q, 0);
            
            // We need to scale the LOCAL quad coordinates and add them to global position
            // q.x0, y0, etc are relative to (0,0) of the 'pen' because we passed dummy_x=0.
            
            float q_width = (q.x1 - q.x0) * render_scale;
            float q_height = (q.y1 - q.y0) * render_scale;
            
            float q_x0_scaled = (q.x0) * render_scale;
            float q_y0_scaled = (q.y0) * render_scale;
            
            // Final positions
            // x is the current pen position.
            // q.x0 is the offset from the pen.
            float cur_x0 = x + q_x0_scaled;
            float cur_y0 = y + q_y0_scaled;
            float cur_x1 = cur_x0 + q_width;
            float cur_y1 = cur_y0 + q_height;
            
            // Advance x manually because we detached it from GetPackedQuad for scaling
            // pdata.xadvance is also baked size, so scale it.
            float advance = pdata[*text_ptr - 32].xadvance * render_scale;
            x += advance;
            
            // Apply Manual Letter Spacing (already scaled? No, letter_spacing is pixels)
            x += letter_spacing;

            // For now, we ignore rotation for simplicity with VBOs
            // To add rotation, you would build a transformation matrix and apply it in the vertex shader

            float quad_verts[] = {
                // pos.x, pos.y, tex.u, tex.v
                // Standard quad triangle strip order usually, but here we use triangles
                cur_x0, cur_y0, q.s0, q.t0,
                cur_x1, cur_y0, q.s1, q.t0,
                cur_x1, cur_y1, q.s1, q.t1,

                cur_x0, cur_y0, q.s0, q.t0,
                cur_x1, cur_y1, q.s1, q.t1,
                cur_x0, cur_y1, q.s0, q.t1
            };
            
            // Upload quad data to VBO and draw
            glBindBuffer(GL_ARRAY_BUFFER, vbo);
            glBufferData(GL_ARRAY_BUFFER, sizeof(quad_verts), quad_verts, GL_DYNAMIC_DRAW);
            glDrawArrays(GL_TRIANGLES, 0, 6);
        }
    }

    // --- Restore OpenGL state ---
    glBindVertexArray(0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(0);
    glDisable(GL_BLEND);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

std::shared_ptr<EffectNode> TextEffectNode::clone() const {
    auto new_node = std::make_shared<TextEffectNode>(*this);
    new_node->font_atlas_tex = 0; // The clone must re-bake its own texture
    new_node->needs_rebake = true;
    return new_node;
}

std::shared_ptr<EffectNode> MergeNode::clone() const {
    return std::make_shared<MergeNode>(*this);
}

void MergeNode::Process(const std::vector<GLuint>& image_inputs, const std::map<int, std::any>& data_inputs, const EffectContext& ctx) {
    // A merge node requires exactly two inputs to function.
    if (!enabled || image_inputs.size() < 2 || image_inputs[0] == 0 || image_inputs[1] == 0) {
        // If inputs are missing, we can choose to output black, or passthrough one of the inputs.
        // Let's passthrough the 'B' input if it exists.
        if (!image_inputs.empty() && image_inputs[0] != 0) {
            // (Code to copy image_inputs[0] to ctx.output_fbo)
        }
        return;
    }
    
    GLuint tex_b = image_inputs[0]; // Background
    GLuint tex_a = image_inputs[1]; // Foreground

    static GLuint merge_shader = 0;
    if (merge_shader == 0) merge_shader = LoadShaderProgram("shaders/passthrough.vert", "shaders/merge.frag");
    if (merge_shader == 0) return;

    glBindFramebuffer(GL_FRAMEBUFFER, ctx.output_fbo);
    glViewport(0, 0, (int)ctx.resolution.x, (int)ctx.resolution.y);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glUseProgram(merge_shader);

    // Bind the two input textures to different texture units
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex_b);
    glUniform1i(glGetUniformLocation(merge_shader, "u_TextureB"), 0);

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, tex_a);
    glUniform1i(glGetUniformLocation(merge_shader, "u_TextureA"), 1);

    // Set the uniforms
    glUniform1i(glGetUniformLocation(merge_shader, "u_BlendMode"), static_cast<int>(blend_mode));
    glUniform1f(glGetUniformLocation(merge_shader, "u_Mix"), mix);

    RenderFullscreenQuad();

    // Cleanup
    glUseProgram(0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

std::shared_ptr<EffectNode> TransformNode::clone() const {
    return std::make_shared<TransformNode>(*this);
}

void TransformNode::Process(const std::vector<GLuint>& image_inputs, const std::map<int, std::any>& data_inputs, const EffectContext& ctx) {
    if (!enabled || image_inputs.empty() || image_inputs[0] == 0) {
        // Passthrough if disabled or no input
        if (!image_inputs.empty() && image_inputs[0] != 0) {
            // (You should have a helper function to copy image_inputs[0] to ctx.output_fbo)
        }
        return;
    }
    GLuint input_texture = image_inputs[0];

    glm::vec2 final_translate = this->translate;
    glm::vec2 final_scale = this->scale;
    float final_rotation = this->rotation;
    
    // Find the pin ID for our "Transform Data" input.
    // Assuming it's the second input pin (index 1).
    if (input_pins.size() > 1) {
        int data_pin_id = input_pins[1].id;
        auto it = data_inputs.find(data_pin_id);
        if (it != data_inputs.end()) {
            try {
                // Try to cast the std::any to our TransformData type
                const TransformData& tracked_data = std::any_cast<const TransformData&>(it->second);
                // If successful, override our internal values
                final_translate = tracked_data.translate;
                final_scale = tracked_data.scale;
                final_rotation = tracked_data.rotation;
            } catch (const std::bad_any_cast& e) {
                // This link is invalid, do nothing
            }
        }
    }

    static GLuint transform_shader = 0;
    if (transform_shader == 0) transform_shader = LoadShaderProgram("shaders/passthrough.vert", "shaders/transform.frag");
    if (transform_shader == 0) return;

    glBindFramebuffer(GL_FRAMEBUFFER, ctx.output_fbo);
    glViewport(0, 0, (int)ctx.resolution.x, (int)ctx.resolution.y);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glUseProgram(transform_shader);

    // Bind the input texture
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, input_texture);
    glUniform1i(glGetUniformLocation(transform_shader, "u_Texture"), 0);

    // Send the transformation uniforms to the shader
    float aspect_ratio = 1.0f;
    if (ctx.resolution.y > 0) {
        aspect_ratio = ctx.resolution.x / ctx.resolution.y;
    }
    glUniform1f(glGetUniformLocation(transform_shader, "u_AspectRatio"), aspect_ratio);
    glUniform2f(glGetUniformLocation(transform_shader, "u_Translate"), final_translate.x, final_translate.y);
    glUniform2f(glGetUniformLocation(transform_shader, "u_Scale"), final_scale.x, final_scale.y);
    glUniform1f(glGetUniformLocation(transform_shader, "u_Rotation"), glm::radians(final_rotation));

    RenderFullscreenQuad();

    // Cleanup
    glUseProgram(0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

DecodedFrame GetFrameSynchronously(const std::string& clip_path, float media_time) {
    // 1. Create a promise that will be fulfilled by the decoder thread.
    auto promise = std::make_shared<std::promise<DecodedFrame>>();
    
    // 2. Get the corresponding future. The tracker thread will wait on this.
    std::future<DecodedFrame> future = promise->get_future();

    // 3. Create and dispatch the request.
    DecodedFrameRequest request;
    request.clip_path = clip_path;
    request.timestamp = media_time;
    request.priority = RequestPriority::High; // Tracking frames are high priority
    request.sync_promise = promise;

    { // Lock the global queue to push the request
        std::lock_guard<std::mutex> lock(decoder_request_mutex);
        decoder_request_queue.push_back(std::move(request));
    }
    decoder_worker_cv.notify_one(); // Wake up the decoder thread

    // 4. Wait for the result. This blocks THIS (tracker) thread, but not the UI thread.
    // It will automatically unblock when the decoder calls set_value().
    return future.get();
}

void TrackerNode::Process(const std::vector<GLuint>& image_inputs,
                          const std::map<int, std::any>& data_inputs,
                          const EffectContext& ctx) {
    // --- Part 1: Passthrough the input image (unchanged) ---
    if (image_inputs.empty() || image_inputs[0] == 0) {
        this->result_texture = 0;
    } else {
        this->result_texture = image_inputs[0];
    }

    // --- Part 2: Output the correct tracking data for the current frame ---
    // Ensure this node actually has a transform output pin.
    if (output_pins.empty() || output_pins[0].type != PinType::Transform) {
        return;
    }
    
    // Calculate the current timeline frame based on the playhead time.
    // Note: We need access to export_fps here. We will pass it via EffectContext.
    int current_frame = static_cast<int>(ctx.time * ctx.fps);

    TransformData output_transform; // Default to an identity transform

    // Find the tracking data for the current frame in our cache.
    auto cache_it = tracking_data_cache.find(current_frame);
    if (cache_it != tracking_data_cache.end()) {
        // We found data! Use it.
        output_transform = cache_it->second;
    }
    // If no data is found, it will just use the default (no-op) transform, which is correct.
    
    // Publish the data to our output pin.
    // The key is the pin's ID, the value is the std::any-wrapped data.
    int output_pin_id = output_pins[0].id;
    this->data_outputs[output_pin_id] = output_transform;
}

// This function is called by the UI when the user finishes drawing the ROI.
void TrackerNode::InitializeAt(const DecodedFrame& frame) {
    if (initial_roi_norm.width <= 0 || initial_roi_norm.height <= 0) {
        is_initialized = false;
        return;
    }

    cv::Mat cv_frame = DecodedFrameToCvMat(frame);
    if (cv_frame.empty()) return;

    // Denormalize the ROI to pixel coordinates
    cv::Rect2f roi_pixels(
        initial_roi_norm.x * cv_frame.cols,
        initial_roi_norm.y * cv_frame.rows,
        initial_roi_norm.width * cv_frame.cols,
        initial_roi_norm.height * cv_frame.rows
    );

    tracker = InitializeTrackerByName("CSRT");
    tracker->init(cv_frame, roi_pixels);
    
    tracking_data_cache.clear();
    is_initialized = true;
    is_selecting_roi = false;
    std::cout << "Tracker initialized at ROI: " << roi_pixels.x << ", " << roi_pixels.y << std::endl;
}

/* void TrackerNode::TrackRange(const Clip& clip, GLResources& res, int start_frame, int end_frame, int export_fps) {
    // This function now runs on a worker thread.
    // It must not touch UI elements directly.

    // --- 1. Initialization ---
    { // Use a scope for the lock_guard
        std::lock_guard<std::mutex> lock(tracking_mutex);
        tracking_status = "Initializing...";
    }
    tracking_progress = 0.0f;

    if (!is_initialized || !tracker) {
        std::lock_guard<std::mutex> lock(tracking_mutex);
        tracking_status = "Error: Tracker not initialized.";
        is_tracking = false;
        return;
    }

    auto vid_it = res.video_cache.find(clip.path);
    if (vid_it == res.video_cache.end()) {
        std::lock_guard<std::mutex> lock(tracking_mutex);
        tracking_status = "Error: Video data not found.";
        is_tracking = false;
        return;
    }
    VideoData& video = vid_it->second;
    
    cv::Rect2f initial_box_pixels(
        initial_roi_norm.x * video.width,
        initial_roi_norm.y * video.height,
        initial_roi_norm.width * video.width,
        initial_roi_norm.height * video.height
    );

    cv::Rect2f current_box = initial_box_pixels;
    
    // Create a temporary map to store results. We'll transfer it at the end.
    std::map<int, TransformData> local_tracking_cache;
    int total_frames_to_track = end_frame - start_frame;

    {
        std::lock_guard<std::mutex> lock(tracking_mutex);
        tracking_status = "Tracking...";
    }

    // --- 2. The Main Tracking Loop ---
    for (int frame_idx = start_frame; frame_idx <= end_frame; ++frame_idx) {
        // Check for cancellation signal from the main thread
        if (cancel_tracking) {
            std::lock_guard<std::mutex> lock(tracking_mutex);
            tracking_status = "Cancelled by user.";
            is_tracking = false;
            return;
        }

        float time_sec = (float)frame_idx / export_fps;
        float media_time = (time_sec - clip.start_time) + clip.media_start;
        if (media_time < 0) continue;

        // Fetch the frame (this logic is now correct)
        ensure_video_decoded_upto(video, media_time);
        const DecodedFrame* frame_data = nullptr;
        double min_diff = std::numeric_limits<double>::max();
        for (const auto& cached_f : video.frame_cache) {
            double diff = std::abs(cached_f.pts - media_time);
            if (diff < min_diff) {
                min_diff = diff;
                frame_data = &cached_f;
            }
        }

        if (!frame_data) continue;
        cv::Mat cv_frame = DecodedFrameToCvMat(*frame_data);
        if (cv_frame.empty()) continue;
        
        // Update the tracker
        if (UpdateTracker(tracker, cv_frame, current_box)) {
            TransformData t_data = CalculateTransformFromBoxes(initial_box_pixels, current_box);
            t_data.translate.x /= video.width;
            t_data.translate.y /= -video.height;
            local_tracking_cache[frame_idx] = t_data;
        } else {
            std::lock_guard<std::mutex> lock(tracking_mutex);
            tracking_status = "Tracking failed at frame " + std::to_string(frame_idx);
            is_tracking = false;
            return; // Stop on failure
        }
        
        // Update progress
        if (total_frames_to_track > 0) {
            tracking_progress = (float)(frame_idx - start_frame + 1) / total_frames_to_track;
        }
    }

    // --- 3. Finalize and Transfer Data ---
    {
        std::lock_guard<std::mutex> lock(tracking_mutex);
        tracking_status = "Finished successfully!";
        // Safely swap the results from our local cache to the main node's cache
        tracking_data_cache.swap(local_tracking_cache);
    }
    is_tracking = false; // Signal that we are done
} */

void TrackerNode::TrackRange(const Clip& clip, GLResources& res, int start_frame, int end_frame, int export_fps) {
    // --- 1. Initialization ---
    {
        std::lock_guard<std::mutex> lock(tracking_mutex);
        tracking_status = "Initializing...";
    }
    tracking_progress = 0.0f;

    if (!is_initialized || !tracker) { 
        std::lock_guard<std::mutex> lock(tracking_mutex);
        tracking_status = "Error: Tracker not initialized.";
        is_tracking = false;
        return; 
    }

    // --- Safely READ video dimensions from the main thread's resources ---
    auto vid_it = res.video_cache.find(clip.path);
    if (vid_it == res.video_cache.end()) {
        std::lock_guard<std::mutex> lock(tracking_mutex);
        tracking_status = "Error: Video data not found in resources.";
        is_tracking = false;
        return;
    }
    const int frame_width = vid_it->second.width;
    const int frame_height = vid_it->second.height;
    // --- End safe read ---
    
    cv::Rect2f initial_box_pixels(
        initial_roi_norm.x * frame_width,
        initial_roi_norm.y * frame_height,
        initial_roi_norm.width * frame_width,
        initial_roi_norm.height * frame_height
    );

    cv::Rect2f current_box = initial_box_pixels;
    std::map<int, TransformData> local_tracking_cache;
    int total_frames_to_track = end_frame - start_frame + 1;

    {
        std::lock_guard<std::mutex> lock(tracking_mutex);
        tracking_status = "Tracking...";
    }

    // --- 2. Main Tracking Loop ---
    for (int frame_idx = start_frame; frame_idx <= end_frame; ++frame_idx) {
        if (cancel_tracking) break;

        float time_sec = (float)frame_idx / export_fps;
        float media_time = (time_sec - clip.start_time) + clip.media_start;
        if (media_time < 0) continue;

        try {
            // Request a frame from the central decoder service. This is thread-safe.
            DecodedFrame frame = GetFrameSynchronously(clip.path, media_time);

            cv::Mat cv_frame = DecodedFrameToCvMat(frame);
            if (cv_frame.empty()) continue;

            if (UpdateTracker(tracker, cv_frame, current_box)) {
                TransformData t_data = CalculateTransformFromBoxes(initial_box_pixels, current_box);
                t_data.translate.x /= frame_width;
                t_data.translate.y /= -frame_height; // Invert Y for OpenGL
                local_tracking_cache[frame_idx] = t_data;
            } else {
                throw std::runtime_error("UpdateTracker failed.");
            }
        } catch (...) { // Catch any exception from the decoder or tracker
            std::lock_guard<std::mutex> lock(tracking_mutex);
            tracking_status = "Error at frame " + std::to_string(frame_idx);
            is_tracking = false;
            return;
        }
        
        tracking_progress = (float)(frame_idx - start_frame + 1) / total_frames_to_track;
    }

    // --- 3. Finalize ---
    {
        std::lock_guard<std::mutex> lock(tracking_mutex);
        tracking_status = cancel_tracking ? "Cancelled by user." : "Finished successfully!";
        tracking_data_cache.swap(local_tracking_cache);
    }
    is_tracking = false;
}