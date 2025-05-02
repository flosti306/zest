#pragma once
#include <vector>
#include <string>
#include "shared.hpp"        // Contains BlendMode and other shared definitions
#include "video_export.hpp"  // For GLresources and KeyframeTrack

struct GLResources;

enum class NodeType {
    Video, Audio, Image, Text, Group, Effect, Mask
};

class Node {
public:
    std::string name;
    std::vector<Node*> children;
    Node* parent = nullptr;
    
    float start_time = 0.0f;
    float duration = 0.0f;
    bool selected = false;
    bool visible = true;
    NodeType type = NodeType::Group;

    Node() = default;
    virtual ~Node() = default;

    void AddChild(Node* child);
    void RemoveChild(Node* child);
    virtual void render(GLResources& res, float current_time, float width, float height) const;
};

class MediaNode : public Node {  // Fixed: public inheritance
public:
    int layer = 0;
    std::string src;
    float media_start = 0.0f;
    
    // Transform properties
    float pos_x = 0.0f;
    float pos_y = 0.0f;
    float scale = 1.0f;
    float opacity = 1.0f;
    float rotation = 0.0f;
    BlendMode blend_mode = BlendMode::Normal;

    KeyframeTrack<float> pos_x_track;
    KeyframeTrack<float> pos_y_track;
    KeyframeTrack<float> scale_track;
    KeyframeTrack<float> opacity_track;
    KeyframeTrack<float> rotation_track;

    bool has_audio = true;
    bool is_audio_only = false;
    std::vector<float> waveform;
    MediaNode* linked_node = nullptr;

    MediaNode();

    void render(GLResources& res, float current_time, float width, float height) const override;
};