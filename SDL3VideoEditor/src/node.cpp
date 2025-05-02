#include "node.hpp"
#include <algorithm>
#include <iostream>

// Node implementation
void Node::AddChild(Node* child) {
    children.push_back(child);
    child->parent = this;
}

void Node::RemoveChild(Node* child) {
    auto it = std::remove(children.begin(), children.end(), child);
    if (it != children.end()) {
        children.erase(it, children.end());
        child->parent = nullptr;
    }
}

void Node::render(GLResources& res, float current_time, float width, float height) const {
    if (current_time >= start_time && current_time < (start_time + duration)) {
        // Base node rendering logic (if any)
    }
    
    for (Node* child : children) {
        child->render(res, current_time, width, height);  // Fixed: Added res parameter
    }
}

// MediaNode implementation
MediaNode::MediaNode() {
    type = NodeType::Video;
}

void MediaNode::render(GLResources& res, float current_time, float width, float height) const {
    Node::render(res, current_time, width, height);  // Call base class first

    if (!visible) return;

    // Blend mode handling
    switch (blend_mode) {  // Fixed: Removed invalid 'clip' reference
        case BlendMode::Normal:
                        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                        break;
                    case BlendMode::Additive:
                        glBlendFunc(GL_SRC_ALPHA, GL_ONE);
                        break;
                    case BlendMode::Multiply:
                        glBlendFunc(GL_DST_COLOR, GL_ZERO);
                        break;
                    case BlendMode::Screen:
                        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_COLOR);
                        break;
                    case BlendMode::Darken:
                        glBlendFunc(GL_MIN, GL_ONE); // approximate
                        break;
                    case BlendMode::Lighten:
                        glBlendFunc(GL_MAX, GL_ONE); // approximate
                        break;
                    case BlendMode::Difference:
                        glBlendFunc(GL_ONE_MINUS_DST_COLOR, GL_ONE_MINUS_SRC_COLOR);
                        break;
                    case BlendMode::Subtract:
                        glBlendFunc(GL_ZERO, GL_ONE_MINUS_SRC_COLOR);
                        break;
                    case BlendMode::Divide:
                        // Not directly possible with glBlendFunc, would need shader
                        glBlendFunc(GL_ONE, GL_ONE); // approximate
                        break;
                    case BlendMode::Overlay:
                        // Overlay needs a shader normally. Approximate with multiply/screen hybrid
                        glBlendFunc(GL_SRC_ALPHA, GL_ONE);
                        break;
    }

    if (type == NodeType::Video &&
        current_time >= start_time &&
        current_time < (start_time + duration))
    {
        float local_time = current_time - start_time;

        // Evaluate keyframes (using class members instead of 'clip')
        float evaluated_pos_x = pos_x_track.keyframes.empty() ? pos_x : pos_x_track.Evaluate(local_time);
        float evaluated_pos_y = pos_y_track.keyframes.empty() ? pos_y : pos_y_track.Evaluate(local_time);
        float evaluated_scale = scale_track.keyframes.empty() ? scale : scale_track.Evaluate(local_time);
        float evaluated_rotation = rotation_track.keyframes.empty() ? rotation : rotation_track.Evaluate(local_time);
        float evaluated_opacity = opacity_track.keyframes.empty() ? opacity : opacity_track.Evaluate(local_time);

        evaluated_scale = std::max(0.0f, evaluated_scale);
        evaluated_opacity = std::clamp(evaluated_opacity, 0.0f, 1.0f);

        // Texture loading logic
        GLuint tex_id = 0;
        bool is_video = is_video_file(src);  // Fixed: Use src instead of clip.path

        // Find the texture
        if (is_video) {
            auto vid_it = res.video_cache.find(src);
            if (vid_it != res.video_cache.end() && vid_it->second.is_initialized) {
                tex_id = vid_it->second.texture_id;
            }
        } else {
            auto img_it = res.texture_cache.find(src);
            if (img_it != res.texture_cache.end()) {
                tex_id = img_it->second;
            }
        }

        if (tex_id != 0) {
            glBindTexture(GL_TEXTURE_2D, tex_id);
            glPushMatrix();

            glTranslatef(evaluated_pos_x * width, evaluated_pos_y * height, 0.0f);
            glRotatef(evaluated_rotation, 0.0f, 0.0f, 1.0f);
            glScalef(evaluated_scale, evaluated_scale, 1.0f);
            glColor4f(1.0f, 1.0f, 1.0f, evaluated_opacity);

            // Draw quad (consider modernizing to VAO/VBO)
            glBegin(GL_QUADS);
                glTexCoord2f(0.0f, 0.0f); glVertex2f(-1.0f * width, -1.0f * height);
                glTexCoord2f(1.0f, 0.0f); glVertex2f( 1.0f * width, -1.0f * height);
                glTexCoord2f(1.0f, 1.0f); glVertex2f( 1.0f * width,  1.0f * height);
                glTexCoord2f(0.0f, 1.0f); glVertex2f(-1.0f * width,  1.0f * height);
            glEnd();

            glPopMatrix();
        } else {
            std::cerr << "Texture not found for node: " << src << std::endl;
        }
    }
}