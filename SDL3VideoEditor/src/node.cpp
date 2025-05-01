/* #include "Node.hpp"
#include "shared.hpp"
#include <algorithm> // for clamp

float Node::EvaluatePosX(float local_time) const {
    if (pos_x_track.keyframes.empty()) return pos_x;
    return pos_x_track.Evaluate(local_time);
}

float Node::EvaluatePosY(float local_time) const {
    if (pos_y_track.keyframes.empty()) return pos_y;
    return pos_y_track.Evaluate(local_time);
}

float Node::EvaluateScale(float local_time) const {
    if (scale_track.keyframes.empty()) return scale;
    return scale_track.Evaluate(local_time);
}

float Node::EvaluateRotation(float local_time) const {
    if (rotation_track.keyframes.empty()) return rotation;
    return rotation_track.Evaluate(local_time);
}

float Node::EvaluateOpacity(float local_time) const {
    if (opacity_track.keyframes.empty()) return opacity;
    return std::clamp(opacity_track.Evaluate(local_time), 0.0f, 1.0f);
}

// MediaNode Constructor
MediaNode::MediaNode() {
    type = NodeType::Media;
}

// MediaNode render function (for now)
void MediaNode::Render(float current_time) {
    if (!visible || !source_clip) return;

    float local_time = current_time - start_time;
    if (local_time < 0 || local_time > duration) return;

    // TODO: Insert your rendering code here later (bind texture, draw quad)
}

// GroupNode Constructor
GroupNode::GroupNode() {
    type = NodeType::Group;
}

// GroupNode rendering (render children)
void GroupNode::Render(float current_time) {
    if (!visible) return;
    for (auto& child : children) {
        if (child) {
            child->Render(current_time);
        }
    }
} */
