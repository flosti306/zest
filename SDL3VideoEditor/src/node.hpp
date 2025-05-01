#pragma once
#include <memory>
#include <vector>
#include <string>
#include <algorithm> // For std::max, std::clamp
#include <cmath>     // For M_PI etc. if needed later
#include <glad/glad.h> // Need OpenGL functions
#include <SDL_opengl.h> // For immediate mode functions
#include <imgui.h>      // For ImVec2 (used in RenderSelf)

#include "shared.hpp" // Includes KeyframeTrack definition, Clip, GLResources, BlendMode
#include "video_export.hpp"

// Forward declarations
// struct Clip; // Already in shared.hpp
// struct GLResources; // Already in shared.hpp

enum class NodeType {
    Media,
    Group,
    Adjustment,
    // more types later (text, particle, etc.)
};

// Base node class
struct Node : std::enable_shared_from_this<Node> { // Allow getting shared_ptr from this
    NodeType type;
    std::string name = "Unnamed Node";
    size_t id; // Unique ID for serialization/selection

    // Timeline properties
    float start_time = 0.0f;
    float duration = 1.0f;
    int layer = 0; // Primarily for sorting before rendering starts
    bool visible = true;

    // Compositing properties
    BlendMode blend_mode = BlendMode::Normal;

    // Keyframed Transform properties (relative to parent)
    KeyframeTrack<float> pos_x_track;
    KeyframeTrack<float> pos_y_track;
    KeyframeTrack<float> scale_track; // Uniform scale for now
    KeyframeTrack<float> rotation_track; // Degrees
    KeyframeTrack<float> opacity_track; // 0.0 to 1.0

    // Static fallback values if no keyframes exist for a property
    float pos_x = 0.0f; // Absolute X coordinate (relative to world origin)
    float pos_y = 0.0f; // Absolute Y coordinate (relative to world origin)
    float scale = 1.0f;
    float rotation = 0.0f;
    float opacity = 1.0f;

    // Hierarchy
    Node* parent = nullptr; // Raw pointer is okay if lifetime managed by shared_ptr from children vector
    std::vector<std::shared_ptr<Node>> children;

    // Constructor to set type and assign unique ID
    Node(NodeType t) : type(t) {
        static size_t next_id = 1; // Start ID from 1 (0 can indicate null/none)
        id = next_id++;
    }
    virtual ~Node() = default;

    // --- Render Function ---
    // Handles transforms, common state, and recursion
    virtual void Render(float current_time, GLResources& resources) {
        if (!visible) return;

        // Calculate local time relative to node's start on the timeline
        float local_time = current_time - start_time;

        // Check if node is active at the current time (with a small epsilon)
        const float epsilon = 1e-4f;
        if (local_time < -epsilon || local_time >= (duration - epsilon)) return;

        // Save the current OpenGL matrix state (parent's transform)
        glPushMatrix();

        // 1. Evaluate Transforms for this node at its local time
        float eval_pos_x = EvaluatePosX(local_time);
        float eval_pos_y = EvaluatePosY(local_time);
        float eval_scale = EvaluateScale(local_time);       // Already clamped >= 0
        float eval_rotation = EvaluateRotation(local_time); // In degrees
        float eval_opacity = EvaluateOpacity(local_time);   // Already clamped [0, 1]

        // 2. Apply Local Transforms (relative to parent's transform state)
        // Order: Translate, Rotate, Scale (applied in reverse order)
        glTranslatef(eval_pos_x, eval_pos_y, 0.0f); // Move to node's position
        // Rotate around the new origin (eval_pos_x, eval_pos_y)
        glRotatef(eval_rotation, 0.0f, 0.0f, 1.0f);
        // Scale around the new origin
        glScalef(eval_scale, eval_scale, 1.0f);

        // 3. Set Common State (Blend Mode, Opacity for this node and potentially children)
        // Set blend function based on node property
        switch (blend_mode) {
            case BlendMode::Normal:     glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); break;
            case BlendMode::Additive:   glBlendFunc(GL_SRC_ALPHA, GL_ONE); break;
            // Corrected Multiply blend for pre-multiplied alpha common setup:
            case BlendMode::Multiply:   glBlendFunc(GL_DST_COLOR, GL_ONE_MINUS_SRC_ALPHA); break; // Or GL_ZERO, GL_SRC_COLOR for basic multiply
            case BlendMode::Screen:     glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_COLOR); break;
            // Approximations or requires shaders for others...
            case BlendMode::Darken:     /* Needs shader or GL_MIN (extension) */ glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); break;
            case BlendMode::Lighten:    /* Needs shader or GL_MAX (extension) */ glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); break;
            default: glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); break;
        }

        // Store current color to restore later (avoids affecting siblings)
        GLfloat previous_color[4];
        glGetFloatv(GL_CURRENT_COLOR, previous_color);

        // Modulate current color by node's opacity. Assumes base color is white (1,1,1,1).
        // This will affect children unless they override glColor.
        glColor4f(previous_color[0], previous_color[1], previous_color[2], previous_color[3] * eval_opacity);


        // 4. Render Node's Specific Content (e.g., texture for MediaNode)
        // Derived class implementation is called here.
        // It draws relative to the current origin (0,0) which is the node's pivot point.
        RenderSelf(current_time, local_time, resources);

        // 5. Render Children
        // Children inherit the transform matrix and GL state (color, blend func) from the parent.
        for (const auto& child : children) {
            if (child) { // Check if child pointer is valid
                child->Render(current_time, resources);
            }
        }

        // Restore the previous OpenGL matrix state (removes this node's transform)
        glPopMatrix();

        // Restore the previous color state to avoid affecting sibling nodes
        glColor4fv(previous_color);
    };

    // Method for derived classes to implement their specific rendering
    virtual void RenderSelf(float current_time, float local_time, GLResources& resources) = 0;

    // --- Evaluate property methods (implementations below) ---
    float EvaluatePosX(float local_time) const;
    float EvaluatePosY(float local_time) const;
    float EvaluateScale(float local_time) const;
    float EvaluateRotation(float local_time) const;
    float EvaluateOpacity(float local_time) const;

    // Helper to add child and set parent pointer
    void AddChild(std::shared_ptr<Node> child) {
        if (child) {
            // Check if already a child? Optional.
            child->parent = this; // Set parent pointer
            children.push_back(child);
        }
    }
};

// --- Media Node ---
struct MediaNode : public Node {
    Clip* source_clip = nullptr; // Link back to the source media info in the library
    bool is_audio = false;       // Convenience flag, could derive from source_clip->type

    // Where in the source media this node instance starts playing from
    float media_start = 0.0f;

    // Link to paired audio/video node
    std::weak_ptr<Node> linked_node;

    MediaNode(bool audio = false) : Node(NodeType::Media), is_audio(audio) {}

    // Renders the texture for video/image nodes
    void RenderSelf(float current_time, float local_time, GLResources& resources) override {
        // Don't render anything visually if this node represents only audio
        if (is_audio || !source_clip) return;
        // Also don't render if the source clip isn't visual
        if (source_clip->type == ClipType::Audio) return;


        GLuint tex_id = 0;
        float media_width = 100.0f;  // Default size if source info missing
        float media_height = 100.0f;
        // Texture coordinates (assuming OpenGL's bottom-left origin for textures,
        // but images/video frames usually load top-left. Flip V.)
        ImVec2 tex_coords_tl = ImVec2(0.0f, 1.0f); // Top-Left UV for quad vertex
        ImVec2 tex_coords_br = ImVec2(1.0f, 0.0f); // Bottom-Right UV for quad vertex

        // --- Get Texture ID and Dimensions ---
        if (source_clip->type == ClipType::Video) {
            auto vid_it = resources.video_cache.find(source_clip->path);
            if (vid_it != resources.video_cache.end() && vid_it->second.is_initialized) {
                tex_id = vid_it->second.texture_id;
                // Use dimensions from the actual video data
                media_width = (float)vid_it->second.width;
                media_height = (float)vid_it->second.height;
            }
        } else if (source_clip->type == ClipType::Image) {
            auto img_it = resources.texture_cache.find(source_clip->path);
            if (img_it != resources.texture_cache.end()) {
                tex_id = img_it->second;
                // Use dimensions stored in the clip struct (loaded previously)
                if (source_clip->source_width > 0 && source_clip->source_height > 0) {
                   media_width = (float)source_clip->source_width;
                   media_height = (float)source_clip->source_height;
                }
                // Optionally query texture if dimensions aren't stored (less efficient)
            }
        }

        // Proceed only if we have a valid texture and positive dimensions
        if (tex_id != 0 && media_width > 0 && media_height > 0) {
            glBindTexture(GL_TEXTURE_2D, tex_id);

            // Draw the quad centered around the current origin (0,0) which has been
            // transformed by the parent and this node's transforms (pos, rot, scale).
            float half_w = media_width / 2.0f;
            float half_h = media_height / 2.0f;

            // Note: The color (including opacity) is set by the base Node::Render call.
            glBegin(GL_QUADS);
                glTexCoord2f(tex_coords_tl.x, tex_coords_tl.y); glVertex2f(-half_w,  half_h); // Top Left vertex
                glTexCoord2f(tex_coords_br.x, tex_coords_tl.y); glVertex2f( half_w,  half_h); // Top Right vertex
                glTexCoord2f(tex_coords_br.x, tex_coords_br.y); glVertex2f( half_w, -half_h); // Bottom Right vertex
                glTexCoord2f(tex_coords_tl.x, tex_coords_br.y); glVertex2f(-half_w, -half_h); // Bottom Left vertex
            glEnd();

            glBindTexture(GL_TEXTURE_2D, 0); // Unbind texture
        }
        // else {
            // Optional: Draw a placeholder if texture is missing
            // e.g., Draw wireframe quad
        // }
    }
};

// --- Group Node ---
struct GroupNode : public Node {
    GroupNode() : Node(NodeType::Group) {}

    // Group node itself doesn't render anything directly.
    // Its transforms are applied in the base Node::Render, and its children are rendered recursively.
    void RenderSelf(float current_time, float local_time, GLResources& resources) override {
        // No operation needed here.
    }
};

// --- Implementations for Evaluate methods ---

inline float Node::EvaluatePosX(float local_time) const {
    if (!pos_x_track.keyframes.empty()) return pos_x_track.Evaluate(local_time);
    return pos_x;
}
inline float Node::EvaluatePosY(float local_time) const {
    if (!pos_y_track.keyframes.empty()) return pos_y_track.Evaluate(local_time);
    return pos_y;
}
inline float Node::EvaluateScale(float local_time) const {
    // Ensure scale doesn't go below zero, could even clamp to a small positive value
    float evaluated = scale;
    if (!scale_track.keyframes.empty()) evaluated = scale_track.Evaluate(local_time);
    return std::max(0.0f, evaluated); // Prevent negative scale
}
inline float Node::EvaluateRotation(float local_time) const {
    if (!rotation_track.keyframes.empty()) return rotation_track.Evaluate(local_time);
    return rotation;
}
inline float Node::EvaluateOpacity(float local_time) const {
    // Clamp opacity between 0.0 and 1.0
    float evaluated = opacity;
    if (!opacity_track.keyframes.empty()) evaluated = opacity_track.Evaluate(local_time);
    return std::clamp(evaluated, 0.0f, 1.0f);
}