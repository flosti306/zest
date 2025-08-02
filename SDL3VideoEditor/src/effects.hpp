#pragma once
#include <vector>
#include <memory>
#include <string>
#include <fstream>
#include <ostream>
#include <iostream>
#include <sstream>
#include <map>
#include <glm/glm.hpp>
#include <glad/glad.h> // Include glad for OpenGL function loading
#include <SDL3/SDL.h>
#include <SDL3/SDL_image.h>
#include <opencv2/opencv.hpp>
#include <opencv2/core/types.hpp> // For cv::Rect specifically
#include <imgui.h>
#include "video_export.hpp" // For DecodedFrame (or shared.hpp if it's there)
#include "shared.hpp"
#include "stb_truetype.h"

GLuint create_temp_fbo(glm::vec2 resolution, GLuint& texture_out);

void destroy_temp_fbo(GLuint fbo, GLuint tex);

GLuint get_texture_from_fbo(GLuint fbo);

void RenderFullscreenQuad(float width, float height);

GLuint LoadShaderProgram(const std::string& vert_path, const std::string& frag_path);

struct EffectNode;

// Represents an input or output "pin" on a node
struct Pin {
    int id;
    EffectNode* node; // The node this pin belongs to
    std::string name;
};

struct EffectContext {
    GLuint input_texture = 0; // Texture to process
    GLuint output_fbo = 0;    // Where to render result
    float time = 0.0f;        // Timeline time
    glm::vec2 resolution;     // Frame resolution
};

struct EffectNode {
    int id; // Every node now needs a unique ID
    std::string name = "Unnamed Effect";
    bool enabled = true;
    ImVec2 editor_pos; // Store position in the node editor

    // Each node has a list of input and output pins
    std::vector<Pin> input_pins;
    std::vector<Pin> output_pins;

    // Cache the result of this node's processing for one frame
    GLuint result_texture = 0;
    bool is_evaluated_this_frame = false;

    virtual ~EffectNode() = default;

    // Process is now given a list of its input textures
    virtual void Process(const std::vector<GLuint>& inputs, const EffectContext& ctx) = 0;
    
    virtual std::shared_ptr<EffectNode> clone() const = 0;

protected:
    // Helper to create pins in constructors
    void add_pin(bool is_input, const std::string& pin_name) {
        static int next_pin_id = 1; // Global pin ID counter
        if (is_input) {
            input_pins.push_back({next_pin_id++, this, pin_name});
        } else {
            output_pins.push_back({next_pin_id++, this, pin_name});
        }
    }
};

// Represents a connection between two pins
struct Link {
    int id;
    int from_node_id, to_node_id;
    int from_pin_id, to_pin_id;
};

struct SourceClipNode : public EffectNode {
    SourceClipNode() {
        name = "Source Clip";
        add_pin(false, "Source"); // Add one output pin
        editor_pos = ImVec2(50.0f, 100.0f);
    }

    // This node doesn't process anything; it's just a starting point.
    // The evaluation engine will assign its texture manually.
    void Process(const std::vector<GLuint>& inputs, const EffectContext& ctx) override {}

    std::shared_ptr<EffectNode> clone() const override {
        return std::make_shared<SourceClipNode>(*this);
    }
};

// A concrete node representing the final output of the graph.
struct FinalOutputNode : public EffectNode {
    FinalOutputNode() {
        name = "Final Output";
        add_pin(true, "Final Image"); // Add one input pin
        editor_pos = ImVec2(300.0f, 100.0f);
    }

    // This node is just a sink. The evaluation engine takes its input
    // and blits it to the final framebuffer.
    void Process(const std::vector<GLuint>& inputs, const EffectContext& ctx) override {}

    std::shared_ptr<EffectNode> clone() const override {
        return std::make_shared<FinalOutputNode>(*this);
    }
};

struct EffectGraph {
    std::map<int, std::shared_ptr<EffectNode>> nodes;
    std::vector<Link> links;
    std::vector<int> node_order;
    int next_node_id = 1;

    // The start and end points of the graph
    int input_node_id = 0;
    int output_node_id = 0;

    std::vector<GLuint> transient_textures;

    EffectGraph() {
        // Create instances of our new CONCRETE node types.
        auto input_node = std::make_shared<SourceClipNode>();
        input_node_id = next_node_id++;
        input_node->id = input_node_id;

        // Give the source node a starting position on the left.
        input_node->editor_pos = ImVec2(50.0f, 100.0f);

        nodes[input_node_id] = input_node;

        auto output_node = std::make_shared<FinalOutputNode>();
        output_node_id = next_node_id++;
        output_node->id = output_node_id;

        // Give the output node a starting position to the right of the source.
        output_node->editor_pos = ImVec2(300.0f, 100.0f);

        nodes[output_node_id] = output_node;

        static int next_link_id = 1; // This could be a member variable if preferred
        links.push_back({
            next_link_id++, // Use a normal, positive, unique ID
            input_node_id, output_node_id,
            input_node->output_pins[0].id, output_node->input_pins[0].id
        });
    }

    void cleanup_transient_resources();
    void rebuild_links_from_order();
    
    // ... (deep copy constructor and processing methods)
    void ProcessNodeGraph(GLuint source_clip_texture, GLuint final_output_fbo, float time, glm::vec2 resolution);
private:
    void evaluate_node(int node_id, const EffectContext& base_ctx);
};

struct GaussianBlurNode : public EffectNode {
    float blur_amount = 5.0f;

    GaussianBlurNode() {
        name = "Gaussian Blur";
        add_pin(true, "Image");  // One input pin
        add_pin(false, "Image"); // One output pin
    }

    void Process(const std::vector<GLuint>& inputs, const EffectContext& ctx) override;

    std::shared_ptr<EffectNode> clone() const override {
        auto new_node = std::make_shared<GaussianBlurNode>();
        *new_node = *this; // Member-wise copy is fine here
        return new_node;
    }
};

struct ColorGradingNode : public EffectNode {
    float brightness = 0.0f;    // Range: [-1.0, 1.0]
    float contrast = 1.0f;      // Range: [0.0, 2.0]
    float saturation = 1.0f;    // Range: [0.0, 2.0]
    float temperature = 0.0f;   // Range: [-1.0, 1.0]
    float tint = 0.0f;          // Range: [-1.0, 1.0]
    glm::vec3 color_filter = glm::vec3(1.0f, 1.0f, 1.0f); // RGB color filter
    float gamma = 1.0f;         // Range: [0.1, 3.0]
    
    ColorGradingNode() {
        name = "Color Grading";
        add_pin(true, "Image");
        add_pin(false, "Image");
    }
    
    void Process(const std::vector<GLuint>& inputs, const EffectContext& ctx) override;

    std::shared_ptr<EffectNode> clone() const override {
        auto new_node = std::make_shared<ColorGradingNode>();
        *new_node = *this;
        return new_node;
    }

    void applyWarmVintagePreset();
    void applyColdCinematicPreset();
    void applyHighContrastBWPreset();
    void applyTechnicolorPreset();
    void applyFadedFilmPreset();
};

struct LUTColorGradingNode : public EffectNode {
    GLuint lut_texture = 0;
    std::string lut_path = "";
    float strength = 1.0f;  // Range: [0.0, 1.0] - blending between original and LUT-transformed
    
    LUTColorGradingNode() {
        name = "LUT Color Grading";
        add_pin(true, "Image");
        add_pin(false, "Image");
    }
    
    ~LUTColorGradingNode() {
        if (lut_texture != 0) {
            glDeleteTextures(1, &lut_texture);
        }
    }
    
    // Load a LUT from file (supports common 3D LUT formats)
    bool loadLUT(const std::string& path);
    
    void Process(const std::vector<GLuint>& inputs, const EffectContext& ctx) override;

    std::shared_ptr<EffectNode> clone() const override {
        auto new_node = std::make_shared<LUTColorGradingNode>();
        *new_node = *this;
        // Note: The GLuint texture ID is copied, which is correct.
        // Both nodes will share the texture, but can be changed independently.
        return new_node;
    }
};

struct MaskEffectNode : public EffectNode {
    enum class MaskType {
        None,
        Rectangle,
        Circle,
        Texture, // For loaded images or drawn masks
        Smart_Interactive
    };

    MaskType mask_type = MaskType::Rectangle;
    bool invert = false;
    float feather = 0.05f; // Range [0.0, 1.0] - softness of the edge

    // Rectangle parameters (normalized coordinates [0, 1])
    glm::vec2 rect_center = glm::vec2(0.5f, 0.5f);
    glm::vec2 rect_size = glm::vec2(0.5f, 0.5f);
    float rect_rotation = 0.0f; // Degrees
    float rect_corner_radius = 0.0f; // Range [0.0, 0.5] - Normalized relative to smaller dimension? Let's start with relative to half-size.

    // Circle parameters (normalized coordinates [0, 1])
    glm::vec2 circle_center = glm::vec2(0.5f, 0.5f);
    float circle_radius = 0.25f;
    float circle_aspect_ratio = 1.0f;

    // Texture mask parameters
    GLuint mask_texture = 0;
    std::string mask_texture_path = ""; // For loading/saving

    // Smart mask parameters
    GLuint smart_interactive_mask_texture = 0;
    cv::Rect grabcut_roi_rect; // OpenCV Rect (x, y, width, height) in *image pixel* coordinates
    cv::Mat last_grabcut_mask_cv; // To hold the raw mask for UI feedback
    
    std::map<float, GLuint> smart_mask_sequence;
    float smart_mask_start_time = 0.0f;
    float smart_mask_fps = 0.0f;

    KeyframeTrack<float> feather_track;
    KeyframeTrack<float> rect_center_x_track;
    KeyframeTrack<float> rect_center_y_track;
    KeyframeTrack<float> rect_size_x_track;
    KeyframeTrack<float> rect_size_y_track;
    KeyframeTrack<float> rect_rotation_track;
    KeyframeTrack<float> rect_corner_radius_track;
    KeyframeTrack<float> circle_center_x_track;
    KeyframeTrack<float> circle_center_y_track;
    KeyframeTrack<float> circle_radius_track;
    KeyframeTrack<float> circle_aspect_ratio_track;

    // TODO: Add parameters for drawing or smart mask if/when implemented

    MaskEffectNode() {
        name = "Mask";
        add_pin(true, "Image");
        add_pin(false, "Image");
    }

    ~MaskEffectNode() override {
        if (mask_texture != 0) {
            glDeleteTextures(1, &mask_texture); // Correct: pass address
        }
        if (smart_interactive_mask_texture != 0) { // Updated member name
            glDeleteTextures(1, &smart_interactive_mask_texture);
        }

        // Correctly iterate and delete textures from smart_mask_sequence
        for (auto const& pair : this->smart_mask_sequence) { // Use this-> or ensure it's in scope
            if (pair.second != 0) { // pair.second is the GLuint tex_id
                GLuint id_to_delete = pair.second; // Make a non-const copy
                glDeleteTextures(1, &id_to_delete);
            }
        }
        this->smart_mask_sequence.clear(); // Use this-> or ensure it's in scope
    }

    // Function to load a texture mask
    bool loadMaskTexture(const std::string& path);

    void Process(const std::vector<GLuint>& inputs, const EffectContext& ctx) override;


    enum class GrabCutInitMode { RECT, MASK };
    GLuint RunGrabCut(const DecodedFrame& frame,
                    GrabCutInitMode mode,
                    const cv::Rect& roi_for_rect_mode, // Pixel coords
                    const cv::Mat& scribble_mask_for_mask_mode, // CV_8UC1 with GC_ values
                    cv::Mat& bgdModel, // Pass by reference to be stored
                    cv::Mat& fgdModel, // Pass by reference to be stored
                    bool is_refinement); // Flag to tell the function to refine instead of init

    std::shared_ptr<EffectNode> clone() const override {
        auto new_node = std::make_shared<MaskEffectNode>();
        *new_node = *this;
        return new_node;
    }
};

struct SolidColorEffectNode : public EffectNode {
    glm::vec4 color = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f); // Default: Red, RGBA
    float blend_with_original = 0.0f; // 0.0 = solid color, 1.0 = original image, 0.5 = 50/50 mix

    // Keyframe Tracks (optional, but good for consistency)
    KeyframeTrack<float> red_track;
    KeyframeTrack<float> green_track;
    KeyframeTrack<float> blue_track;
    KeyframeTrack<float> alpha_track;
    KeyframeTrack<float> blend_track;


    SolidColorEffectNode() {
        name = "Solid Color Overlay";
        add_pin(true, "Image");
        add_pin(false, "Image");
    }

    void Process(const std::vector<GLuint>& inputs, const EffectContext& ctx) override;

    std::shared_ptr<EffectNode> clone() const override {
        auto new_node = std::make_shared<SolidColorEffectNode>();
        *new_node = *this;
        return new_node;
    }
};

struct GradientEffectNode : public EffectNode {
    enum class GradientType {
        Linear,
        Radial
        // Bilinear, etc. (can be added later)
    };

    GradientType type = GradientType::Linear;
    glm::vec4 color_start = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f); // RGBA
    glm::vec4 color_end = glm::vec4(0.0f, 0.0f, 1.0f, 1.0f);   // RGBA

    // For Linear Gradient
    glm::vec2 start_point = glm::vec2(0.0f, 0.5f); // Normalized UV [0,1]
    glm::vec2 end_point = glm::vec2(1.0f, 0.5f);   // Normalized UV [0,1]

    // For Radial Gradient
    glm::vec2 center_point = glm::vec2(0.5f, 0.5f); // Normalized UV [0,1]
    float radius_inner = 0.0f; // Normalized UV [0,1] (where start color is fully opaque)
    float radius_outer = 0.5f; // Normalized UV [0,1] (where end color is fully opaque)
    float aspect_ratio = 1.0f; // To make radial gradients circular if viewport is not square

    float intensity = 1.0f; // Default to 1.0 (fully visible overlay). 0.0 would be invisible.

    // Keyframe Tracks (optional)
    // Example for start color alpha, you can add for R,G,B, points, radii etc.
    KeyframeTrack<float> start_color_alpha_track;
    KeyframeTrack<float> end_color_alpha_track;
    KeyframeTrack<float> intensity_track;
    // Keyframes for vec2 (start_point, end_point, center_point) would require KeyframeTrack<glm::vec2>
    // or separate tracks for X and Y components. For simplicity, let's omit full keyframing for points/radii for now.

    GradientEffectNode() {
        name = "Gradient Overlay";
        add_pin(true, "Image");
        add_pin(false, "Image");
    }

    void Process(const std::vector<GLuint>& inputs, const EffectContext& ctx) override;

    std::shared_ptr<EffectNode> clone() const override {
        auto new_node = std::make_shared<GradientEffectNode>();
        *new_node = *this;
        return new_node;
    }
};

struct DropShadowEffectNode : public EffectNode {
    glm::vec2 offset = glm::vec2(0.01f, -0.01f); // Normalized UV offset (e.g., 1% right, 1% down from top-left if UVs are top-left)
                                                // Or pixel offset if preferred, but normalized is more resolution-independent.
                                                // For standard GL UVs (bottom-left 0,0), positive Y is up.
                                                // Let's assume positive X = right, positive Y = up for offset.
    glm::vec4 shadow_color = glm::vec4(0.0f, 0.0f, 0.0f, 0.5f); // Black, semi-transparent
    float blur_amount = 5.0f;       // Similar to Gaussian blur amount
    
    // Keyframe Tracks (optional)
    KeyframeTrack<float> offset_x_track;
    KeyframeTrack<float> offset_y_track;
    KeyframeTrack<float> shadow_r_track;
    KeyframeTrack<float> shadow_g_track;
    KeyframeTrack<float> shadow_b_track;
    KeyframeTrack<float> shadow_a_track;
    KeyframeTrack<float> blur_amount_track;

     // --- UPDATED: Internal state for multipass rendering ---
    // We now use two FBOs for a more robust ping-pong.
    GLuint temp_fbo1 = 0;
    GLuint temp_tex1 = 0; // Will be used for alpha mask, then final blurred alpha
    GLuint temp_fbo2 = 0;
    GLuint temp_tex2 = 0; // Will be used for the intermediate horizontal blur

    DropShadowEffectNode() {
        name = "Drop Shadow";
        add_pin(true, "Image");
        add_pin(false, "Image");
    }

    ~DropShadowEffectNode() override {
        if (temp_fbo1 != 0) glDeleteFramebuffers(1, &temp_fbo1);
        if (temp_tex1 != 0) glDeleteTextures(1, &temp_tex1);
        if (temp_fbo2 != 0) glDeleteFramebuffers(1, &temp_fbo2);
        if (temp_tex2 != 0) glDeleteTextures(1, &temp_tex2);
    }

    // Helper to manage internal FBOs/textures
    void EnsureTempResources(int width, int height);

    void Process(const std::vector<GLuint>& inputs, const EffectContext& ctx) override;

    std::shared_ptr<EffectNode> clone() const override {
        auto new_node = std::make_shared<DropShadowEffectNode>();
        *new_node = *this;
        // IMPORTANT: Do not share transient resources. The cloned node must create its own.
        new_node->temp_fbo1 = 0;
        new_node->temp_tex1 = 0;
        new_node->temp_fbo2 = 0;
        new_node->temp_tex2 = 0;
        return new_node;
    }
};

struct ChromaKeyNode : public EffectNode {
    glm::vec3 key_color = glm::vec3(0.0f, 1.0f, 0.0f); // Default to green
    float similarity = 0.4f;  // Range [0.0, 1.0] - how similar colors must be to be keyed
    float blend = 0.1f;       // Range [0.0, 1.0] - softness of the edge
    float spill = 0.2f;       // Range [0.0, 1.0] - amount of color spill suppression

    ChromaKeyNode() {
        name = "Chroma Key";
        add_pin(true, "Image");
        add_pin(false, "Image");
    }

    void Process(const std::vector<GLuint>& inputs, const EffectContext& ctx) override;

    std::shared_ptr<EffectNode> clone() const override {
        auto new_node = std::make_shared<ChromaKeyNode>();
        *new_node = *this;
        return new_node;
    }
};

struct TextEffectNode : public EffectNode {
    // --- Text Properties ---
    std::string text_content = "Hello, World!";
    std::string font_path = "C:/Windows/Fonts/arial.ttf"; // A sensible default
    float font_size = 64.0f;
    glm::vec2 position = {0.5f, 0.5f}; // Normalized screen coords [0,1]
    glm::vec4 text_color = {1.0f, 1.0f, 1.0f, 1.0f}; // White
    float rotation = 0.0f; // In degrees

    // --- Style Properties ---
    bool has_background = false;
    glm::vec4 background_color = {0.0f, 0.0f, 0.0f, 0.5f};
    float background_padding = 10.0f; // In pixels

    // --- Internal Font Rendering State ---
    bool needs_rebake = true; // Flag to re-generate the font texture
    GLuint font_atlas_tex = 0;
    stbtt_bakedchar cdata[96]; // Character data for ASCII 32-127

    TextEffectNode() {
        name = "Text";
        add_pin(true, "Image");
        add_pin(false, "Image");
    }
    
    ~TextEffectNode() override {
        if (font_atlas_tex != 0) {
            glDeleteTextures(1, &font_atlas_tex);
        }
    }

    // Public method to trigger a font re-bake
    void RebakeFont();

    void Process(const std::vector<GLuint>& inputs, const EffectContext& ctx) override;
    std::shared_ptr<EffectNode> clone() const override;
};