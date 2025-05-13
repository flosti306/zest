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
    GLuint current_smart_mask_texture = 0; // Texture ID for the GrabCut result
    cv::Rect grabcut_roi_rect; // OpenCV Rect (x, y, width, height) in *image pixel* coordinates
    bool grabcut_ran_for_current_texture = false;
    
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
        if (current_smart_mask_texture != 0) {
            glDeleteTextures(1, &current_smart_mask_texture); // Correct: pass address
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


    // Method to run GrabCut (will be called from UI)
    // frame_pixels: Raw pixel data of the current clip frame (e.g., from DecodedFrame)
    // frame_w, frame_h: Dimensions of that frame
    // roi_normalized: The user-drawn rectangle in NORMALIZED ImGui canvas coordinates [0,1]
    bool RunGrabCut(const DecodedFrame& current_clip_frame, const ImVec2& roi_norm_tl, const ImVec2& roi_norm_br);
};
