#pragma once
#include <vector>
#include <memory>
#include <string>
#include <fstream>
#include <ostream>
#include <iostream>
#include <sstream>
#include <glm/glm.hpp>
#include <glad/glad.h> // Include glad for OpenGL function loading
#include <SDL3/SDL.h>
#include <SDL3/SDL_image.h>
#include <opencv2/opencv.hpp>
#include <opencv2/core/types.hpp> // For cv::Rect specifically
#include <imgui.h>
#include "video_export.hpp" // For DecodedFrame (or shared.hpp if it's there)
#include "shared.hpp"

GLuint create_temp_fbo(glm::vec2 resolution, GLuint& texture_out);

void destroy_temp_fbo(GLuint fbo, GLuint tex);

GLuint get_texture_from_fbo(GLuint fbo);

void RenderFullscreenQuad(float width, float height);

GLuint LoadShaderProgram(const std::string& vert_path, const std::string& frag_path);


struct EffectContext {
    GLuint input_texture = 0; // Texture to process
    GLuint output_fbo = 0;    // Where to render result
    float time = 0.0f;        // Timeline time
    glm::vec2 resolution;     // Frame resolution
};

struct EffectNode {
    std::string name = "Unnamed Effect";
    bool enabled = true;

    virtual ~EffectNode() = default;
    virtual void Process(const EffectContext& ctx) = 0;
};

struct GaussianBlurNode : public EffectNode {
    float blur_amount = 5.0f;

    void Process(const EffectContext& ctx) override;
};

struct EffectGraph {
    std::vector<std::shared_ptr<EffectNode>> nodes;

    void Process(GLuint input_tex, GLuint output_fbo, float time, glm::vec2 resolution);

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
    }
    
    void Process(const EffectContext& ctx) override;

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
    }
    
    ~LUTColorGradingNode() {
        if (lut_texture != 0) {
            glDeleteTextures(1, &lut_texture);
        }
    }
    
    // Load a LUT from file (supports common 3D LUT formats)
    bool loadLUT(const std::string& path);
    
    void Process(const EffectContext& ctx) override;
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

    void Process(const EffectContext& ctx) override;


    enum class GrabCutInitMode { RECT, MASK };
    GLuint RunGrabCut(const DecodedFrame& frame,
                    GrabCutInitMode mode,
                    const cv::Rect& roi_for_rect_mode, // Pixel coords
                    const cv::Mat& scribble_mask_for_mask_mode, // CV_8UC1 with GC_ values
                    cv::Mat& bgdModel, // Pass by reference to be stored
                    cv::Mat& fgdModel, // Pass by reference to be stored
                    bool is_refinement); // Flag to tell the function to refine instead of init
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
    }

    void Process(const EffectContext& ctx) override;
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
    }

    void Process(const EffectContext& ctx) override;
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

    // Internal state for multipass rendering
    GLuint temp_fbo1 = 0;
    GLuint temp_tex1_alpha_mask = 0; // Stores the isolated alpha of the masked input
    GLuint temp_tex2_blurred_alpha = 0; // Stores the blurred alpha mask

    DropShadowEffectNode() {
        name = "Drop Shadow";
    }

    ~DropShadowEffectNode() override {
        if (temp_fbo1 != 0) glDeleteFramebuffers(1, &temp_fbo1);
        if (temp_tex1_alpha_mask != 0) glDeleteTextures(1, &temp_tex1_alpha_mask);
        if (temp_tex2_blurred_alpha != 0) glDeleteTextures(1, &temp_tex2_blurred_alpha);
    }

    // Helper to manage internal FBOs/textures
    void EnsureTempResources(int width, int height);

    void Process(const EffectContext& ctx) override;
};