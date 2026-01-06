#include "project_io.hpp"
#include "effects.hpp" // Required for EffectNode and subclasses
#include <imgui.h>     // Required for ImVec2
#include <fstream>
#include <sstream>
#include <nlohmann/json.hpp>
#include <miniz.h>
#include <map> // For pointer-to-index mapping
#include <vector>

using json = nlohmann::json;

// --- Helper: KeyframeTrack serialization (example for float, extend for others) ---
template<typename T>
void to_json(json& j, const KeyframeTrack<T>& track) {
    j = json::array();
    for (const auto& kf : track.keyframes) {
        j.push_back({
            {"time", kf.time},
            {"value", kf.value},
            {"interp", static_cast<int>(kf.interp)}
        });
    }
}
template<typename T>
void from_json(const json& j, KeyframeTrack<T>& track) {
    track.keyframes.clear();
    for (const auto& kf : j) {
        Keyframe<T> key;
        key.time = kf.value("time", 0.0f);
        key.value = kf.value("value", T{});
        key.interp = static_cast<InterpolationType>(kf.value("interp", 0));
        track.keyframes.push_back(key);
    }
}

// --- Helper: Basic Type Serialization ---
namespace glm {
    void to_json(json& j, const vec2& v) { j = json{{"x", v.x}, {"y", v.y}}; }
    void from_json(const json& j, vec2& v) { v.x = j.value("x", 0.0f); v.y = j.value("y", 0.0f); }

    void to_json(json& j, const vec3& v) { j = json{{"x", v.x}, {"y", v.y}, {"z", v.z}}; }
    void from_json(const json& j, vec3& v) { v.x = j.value("x", 0.0f); v.y = j.value("y", 0.0f); v.z = j.value("z", 0.0f); }

    void to_json(json& j, const vec4& v) { j = json{{"x", v.x}, {"y", v.y}, {"z", v.z}, {"w", v.w}}; }
    void from_json(const json& j, vec4& v) { v.x = j.value("x", 0.0f); v.y = j.value("y", 0.0f); v.z = j.value("z", 0.0f); v.w = j.value("w", 0.0f); }
}

void to_json(json& j, const ImVec2& v) { j = json{{"x", v.x}, {"y", v.y}}; }
void from_json(const json& j, ImVec2& v) { v.x = j.value("x", 0.0f); v.y = j.value("y", 0.0f); }

// --- Helper: Pin and Link Serialization ---
void to_json(json& j, const Pin& pin) {
    j = json{
        {"id", pin.id},
        {"name", pin.name},
        {"type", static_cast<int>(pin.type)}
    };
}

void from_json(const json& j, Pin& pin) {
    pin.id = j.value("id", -1);
    pin.name = j.value("name", "Unnamed");
    pin.type = static_cast<PinType>(j.value("type", 0));
}

void to_json(json& j, const Link& link) {
    j = json{
        {"id", link.id},
        {"from_node_id", link.from_node_id},
        {"to_node_id", link.to_node_id},
        {"from_pin_id", link.from_pin_id},
        {"to_pin_id", link.to_pin_id}
    };
}

void from_json(const json& j, Link& link) {
    link.id = j.value("id", -1);
    link.from_node_id = j.value("from_node_id", -1);
    link.to_node_id = j.value("to_node_id", -1);
    link.from_pin_id = j.value("from_pin_id", -1);
    link.to_pin_id = j.value("to_pin_id", -1);
}

// --- Helper: EffectNode Serialization ---

void SerializeEffectNode(json& j, const std::shared_ptr<EffectNode>& node) {
    if (!node) { j = nullptr; return; }

    j["id"] = node->id;
    j["name"] = node->name;
    j["enabled"] = node->enabled;
    j["editor_pos"] = node->editor_pos; // Uses ImVec2 overload
    j["input_pins"] = node->input_pins; // Uses Pin overload (vector<Pin> autodispatched)
    j["output_pins"] = node->output_pins;

    // Type Dispatch
    if (auto n = std::dynamic_pointer_cast<SourceClipNode>(node)) {
        j["type"] = "SourceClipNode";
    } else if (auto n = std::dynamic_pointer_cast<FinalOutputNode>(node)) {
        j["type"] = "FinalOutputNode";
    } else if (auto n = std::dynamic_pointer_cast<EmptySourceNode>(node)) {
        j["type"] = "EmptySourceNode";
    } else if (auto n = std::dynamic_pointer_cast<GaussianBlurNode>(node)) {
        j["type"] = "GaussianBlurNode";
        j["blur_amount"] = n->blur_amount;
    } else if (auto n = std::dynamic_pointer_cast<ColorGradingNode>(node)) {
        j["type"] = "ColorGradingNode";
        j["brightness"] = n->brightness;
        j["contrast"] = n->contrast;
        j["saturation"] = n->saturation;
        j["temperature"] = n->temperature;
        j["tint"] = n->tint;
        j["color_filter"] = n->color_filter; // glm::vec3
        j["gamma"] = n->gamma;
    } else if (auto n = std::dynamic_pointer_cast<LUTColorGradingNode>(node)) {
        j["type"] = "LUTColorGradingNode";
        j["lut_path"] = n->lut_path;
        j["strength"] = n->strength;
    } else if (auto n = std::dynamic_pointer_cast<MaskEffectNode>(node)) {
        j["type"] = "MaskEffectNode";
        j["mask_type"] = static_cast<int>(n->mask_type);
        j["invert"] = n->invert;
        j["feather"] = n->feather;
        j["rect_center"] = n->rect_center;
        j["rect_size"] = n->rect_size;
        j["rect_rotation"] = n->rect_rotation;
        j["rect_corner_radius"] = n->rect_corner_radius;
        j["circle_center"] = n->circle_center;
        j["circle_radius"] = n->circle_radius;
        j["circle_aspect_ratio"] = n->circle_aspect_ratio;
        j["mask_texture_path"] = n->mask_texture_path;
        
        // Serialize Tracks
        j["feather_track"] = n->feather_track;
        j["rect_center_x_track"] = n->rect_center_x_track;
        j["rect_center_y_track"] = n->rect_center_y_track;
        j["rect_size_x_track"] = n->rect_size_x_track;
        j["rect_size_y_track"] = n->rect_size_y_track;
        j["rect_rotation_track"] = n->rect_rotation_track;
        j["rect_corner_radius_track"] = n->rect_corner_radius_track;
        j["circle_center_x_track"] = n->circle_center_x_track;
        j["circle_center_y_track"] = n->circle_center_y_track;
        j["circle_radius_track"] = n->circle_radius_track;
        j["circle_aspect_ratio_track"] = n->circle_aspect_ratio_track;

    } else if (auto n = std::dynamic_pointer_cast<SolidColorEffectNode>(node)) {
        j["type"] = "SolidColorEffectNode";
        j["color"] = n->color; // glm::vec4
        j["blend_with_original"] = n->blend_with_original;
        
        j["red_track"] = n->red_track;
        j["green_track"] = n->green_track;
        j["blue_track"] = n->blue_track;
        j["alpha_track"] = n->alpha_track;
        j["blend_track"] = n->blend_track;

    } else if (auto n = std::dynamic_pointer_cast<GradientEffectNode>(node)) {
        j["type"] = "GradientEffectNode";
        j["grad_type"] = static_cast<int>(n->type);
        j["color_start"] = n->color_start;
        j["color_end"] = n->color_end;
        j["start_point"] = n->start_point;
        j["end_point"] = n->end_point;
        j["center_point"] = n->center_point;
        j["radius_inner"] = n->radius_inner;
        j["radius_outer"] = n->radius_outer;
        j["aspect_ratio"] = n->aspect_ratio;
        j["intensity"] = n->intensity;

        j["start_color_alpha_track"] = n->start_color_alpha_track;
        j["end_color_alpha_track"] = n->end_color_alpha_track;
        j["intensity_track"] = n->intensity_track;

    } else if (auto n = std::dynamic_pointer_cast<DropShadowEffectNode>(node)) {
        j["type"] = "DropShadowEffectNode";
        j["offset"] = n->offset;
        j["shadow_color"] = n->shadow_color;
        j["blur_amount"] = n->blur_amount;
        
        j["offset_x_track"] = n->offset_x_track;
        j["offset_y_track"] = n->offset_y_track;
        j["shadow_r_track"] = n->shadow_r_track;
        j["shadow_g_track"] = n->shadow_g_track;
        j["shadow_b_track"] = n->shadow_b_track;
        j["shadow_a_track"] = n->shadow_a_track;
        j["blur_amount_track"] = n->blur_amount_track;

    } else if (auto n = std::dynamic_pointer_cast<ChromaKeyNode>(node)) {
        j["type"] = "ChromaKeyNode";
        j["key_color"] = n->key_color; // glm::vec3
        j["similarity"] = n->similarity;
        j["blend"] = n->blend;
        j["spill"] = n->spill;

    } else if (auto n = std::dynamic_pointer_cast<TextEffectNode>(node)) {
        j["type"] = "TextEffectNode";
        j["text_content"] = n->text_content;
        j["font_path"] = n->font_path;
        j["font_size"] = n->font_size;
        j["position"] = n->position;
        j["text_color"] = n->text_color;
        j["rotation"] = n->rotation;
        
        j["has_background"] = n->has_background;
        j["background_color"] = n->background_color;
        j["background_padding"] = n->background_padding;
        j["has_outline"] = n->has_outline;
        j["outline_thickness"] = n->outline_thickness;
        j["outline_color"] = n->outline_color;
        j["alignment"] = static_cast<int>(n->alignment);
        j["letter_spacing"] = n->letter_spacing;

    } else if (auto n = std::dynamic_pointer_cast<MergeNode>(node)) {
        j["type"] = "MergeNode";
        j["blend_mode"] = static_cast<int>(n->blend_mode);
        j["mix"] = n->mix;

    } else if (auto n = std::dynamic_pointer_cast<TransformNode>(node)) {
        j["type"] = "TransformNode";
        j["translate"] = n->translate;
        j["scale"] = n->scale;
        j["rotation"] = n->rotation;

    } else if (auto n = std::dynamic_pointer_cast<TrackerNode>(node)) {
        j["type"] = "TrackerNode";
        j["roi"] = json{{"x", n->initial_roi_norm.x}, {"y", n->initial_roi_norm.y}, {"w", n->initial_roi_norm.width}, {"h", n->initial_roi_norm.height}};
        
        // Serialize generic simple tracking data if small enough? 
        // For now let's just save the ROI and config. Re-tracking is implied or data needs separate binary blob.
        // Assuming we rely on re-tracking or keyframes for now to keep JSON small.
        // Actually, let's serialize the cache if possible.
        json cache_json = json::object();
        for(const auto& [frame, data] : n->tracking_data_cache) {
            cache_json[std::to_string(frame)] = json{
                {"tx", data.translate.x}, {"ty", data.translate.y},
                {"sx", data.scale.x}, {"sy", data.scale.y},
                {"rot", data.rotation}
            };
        }
        j["tracking_data"] = cache_json;
    }
}

std::shared_ptr<EffectNode> DeserializeEffectNode(const json& j) {
    if (j.is_null()) return nullptr;

    std::string type = j.value("type", "");
    std::shared_ptr<EffectNode> node = nullptr;

    // Factory
    if (type == "SourceClipNode") node = std::make_shared<SourceClipNode>();
    else if (type == "FinalOutputNode") node = std::make_shared<FinalOutputNode>();
    else if (type == "EmptySourceNode") node = std::make_shared<EmptySourceNode>();
    else if (type == "GaussianBlurNode") node = std::make_shared<GaussianBlurNode>();
    else if (type == "ColorGradingNode") node = std::make_shared<ColorGradingNode>();
    else if (type == "LUTColorGradingNode") node = std::make_shared<LUTColorGradingNode>();
    else if (type == "MaskEffectNode") node = std::make_shared<MaskEffectNode>();
    else if (type == "SolidColorEffectNode") node = std::make_shared<SolidColorEffectNode>();
    else if (type == "GradientEffectNode") node = std::make_shared<GradientEffectNode>();
    else if (type == "DropShadowEffectNode") node = std::make_shared<DropShadowEffectNode>();
    else if (type == "ChromaKeyNode") node = std::make_shared<ChromaKeyNode>();
    else if (type == "TextEffectNode") node = std::make_shared<TextEffectNode>();
    else if (type == "MergeNode") node = std::make_shared<MergeNode>();
    else if (type == "TransformNode") node = std::make_shared<TransformNode>();
    else if (type == "TrackerNode") node = std::make_shared<TrackerNode>();
    
    if (!node) return nullptr;

    // Base Properties
    node->id = j.value("id", -1);
    node->name = j.value("name", "Unnamed Effect");
    node->enabled = j.value("enabled", true);
    if(j.contains("editor_pos")) node->editor_pos = j["editor_pos"].get<ImVec2>();
    
    // Pins must be overwritten from file to maintain IDs
    if(j.contains("input_pins")) node->input_pins = j["input_pins"].get<std::vector<Pin>>();
    if(j.contains("output_pins")) node->output_pins = j["output_pins"].get<std::vector<Pin>>();
    
    // Fix node pointers in pins!
    for(auto& p : node->input_pins) p.node = node.get();
    for(auto& p : node->output_pins) p.node = node.get();

    // Specific Properties
    if (auto n = std::dynamic_pointer_cast<GaussianBlurNode>(node)) {
        n->blur_amount = j.value("blur_amount", 5.0f);
    } else if (auto n = std::dynamic_pointer_cast<ColorGradingNode>(node)) {
        n->brightness = j.value("brightness", 0.0f);
        n->contrast = j.value("contrast", 1.0f);
        n->saturation = j.value("saturation", 1.0f);
        n->temperature = j.value("temperature", 0.0f);
        n->tint = j.value("tint", 0.0f);
        if(j.contains("color_filter")) n->color_filter = j["color_filter"].get<glm::vec3>();
        n->gamma = j.value("gamma", 1.0f);
    } else if (auto n = std::dynamic_pointer_cast<LUTColorGradingNode>(node)) {
        n->lut_path = j.value("lut_path", "");
        n->strength = j.value("strength", 1.0f);
        if (!n->lut_path.empty()) n->loadLUT(n->lut_path); // Attempt to reload LUT
    } else if (auto n = std::dynamic_pointer_cast<MaskEffectNode>(node)) {
        n->mask_type = static_cast<MaskEffectNode::MaskType>(j.value("mask_type", 0));
        n->invert = j.value("invert", false);
        n->feather = j.value("feather", 0.05f);
        if(j.contains("rect_center")) n->rect_center = j["rect_center"].get<glm::vec2>();
        if(j.contains("rect_size")) n->rect_size = j["rect_size"].get<glm::vec2>();
        n->rect_rotation = j.value("rect_rotation", 0.0f);
        n->rect_corner_radius = j.value("rect_corner_radius", 0.0f);
        if(j.contains("circle_center")) n->circle_center = j["circle_center"].get<glm::vec2>();
        n->circle_radius = j.value("circle_radius", 0.25f);
        n->circle_aspect_ratio = j.value("circle_aspect_ratio", 1.0f);
        n->mask_texture_path = j.value("mask_texture_path", "");
        if (!n->mask_texture_path.empty()) n->loadMaskTexture(n->mask_texture_path); // Reload mask tex

        if(j.contains("feather_track")) j["feather_track"].get_to(n->feather_track);
        if(j.contains("rect_center_x_track")) j["rect_center_x_track"].get_to(n->rect_center_x_track);
        if(j.contains("rect_center_y_track")) j["rect_center_y_track"].get_to(n->rect_center_y_track);
        if(j.contains("rect_size_x_track")) j["rect_size_x_track"].get_to(n->rect_size_x_track);
        if(j.contains("rect_size_y_track")) j["rect_size_y_track"].get_to(n->rect_size_y_track);
        if(j.contains("rect_rotation_track")) j["rect_rotation_track"].get_to(n->rect_rotation_track);
        if(j.contains("rect_corner_radius_track")) j["rect_corner_radius_track"].get_to(n->rect_corner_radius_track);
        if(j.contains("circle_center_x_track")) j["circle_center_x_track"].get_to(n->circle_center_x_track);
        if(j.contains("circle_center_y_track")) j["circle_center_y_track"].get_to(n->circle_center_y_track);
        if(j.contains("circle_radius_track")) j["circle_radius_track"].get_to(n->circle_radius_track);
        if(j.contains("circle_aspect_ratio_track")) j["circle_aspect_ratio_track"].get_to(n->circle_aspect_ratio_track);

    } else if (auto n = std::dynamic_pointer_cast<SolidColorEffectNode>(node)) {
        if(j.contains("color")) n->color = j["color"].get<glm::vec4>();
        n->blend_with_original = j.value("blend_with_original", 0.0f);
        
        if(j.contains("red_track")) j["red_track"].get_to(n->red_track);
        if(j.contains("green_track")) j["green_track"].get_to(n->green_track);
        if(j.contains("blue_track")) j["blue_track"].get_to(n->blue_track);
        if(j.contains("alpha_track")) j["alpha_track"].get_to(n->alpha_track);
        if(j.contains("blend_track")) j["blend_track"].get_to(n->blend_track);

    } else if (auto n = std::dynamic_pointer_cast<GradientEffectNode>(node)) {
        n->type = static_cast<GradientEffectNode::GradientType>(j.value("grad_type", 0));
        if(j.contains("color_start")) n->color_start = j["color_start"].get<glm::vec4>();
        if(j.contains("color_end")) n->color_end = j["color_end"].get<glm::vec4>();
        if(j.contains("start_point")) n->start_point = j["start_point"].get<glm::vec2>();
        if(j.contains("end_point")) n->end_point = j["end_point"].get<glm::vec2>();
        if(j.contains("center_point")) n->center_point = j["center_point"].get<glm::vec2>();
        n->radius_inner = j.value("radius_inner", 0.0f);
        n->radius_outer = j.value("radius_outer", 0.5f);
        n->aspect_ratio = j.value("aspect_ratio", 1.0f);
        n->intensity = j.value("intensity", 1.0f);

        if(j.contains("start_color_alpha_track")) j["start_color_alpha_track"].get_to(n->start_color_alpha_track);
        if(j.contains("end_color_alpha_track")) j["end_color_alpha_track"].get_to(n->end_color_alpha_track);
        if(j.contains("intensity_track")) j["intensity_track"].get_to(n->intensity_track);

    } else if (auto n = std::dynamic_pointer_cast<DropShadowEffectNode>(node)) {
        if(j.contains("offset")) n->offset = j["offset"].get<glm::vec2>();
        if(j.contains("shadow_color")) n->shadow_color = j["shadow_color"].get<glm::vec4>();
        n->blur_amount = j.value("blur_amount", 5.0f); // Default 5.0

        if(j.contains("offset_x_track")) j["offset_x_track"].get_to(n->offset_x_track);
        if(j.contains("offset_y_track")) j["offset_y_track"].get_to(n->offset_y_track);
        if(j.contains("shadow_r_track")) j["shadow_r_track"].get_to(n->shadow_r_track);
        if(j.contains("shadow_g_track")) j["shadow_g_track"].get_to(n->shadow_g_track);
        if(j.contains("shadow_b_track")) j["shadow_b_track"].get_to(n->shadow_b_track);
        if(j.contains("shadow_a_track")) j["shadow_a_track"].get_to(n->shadow_a_track);
        if(j.contains("blur_amount_track")) j["blur_amount_track"].get_to(n->blur_amount_track);

    } else if (auto n = std::dynamic_pointer_cast<ChromaKeyNode>(node)) {
         if(j.contains("key_color")) n->key_color = j["key_color"].get<glm::vec3>();
         n->similarity = j.value("similarity", 0.4f);
         n->blend = j.value("blend", 0.1f);
         n->spill = j.value("spill", 0.2f);

    } else if (auto n = std::dynamic_pointer_cast<TextEffectNode>(node)) {
        n->text_content = j.value("text_content", "Hello");
        n->font_path = j.value("font_path", "C:/Windows/Fonts/arial.ttf");
        n->font_size = j.value("font_size", 64.0f);
        if(j.contains("position")) n->position = j["position"].get<glm::vec2>();
        if(j.contains("text_color")) n->text_color = j["text_color"].get<glm::vec4>();
        n->rotation = j.value("rotation", 0.0f);
        n->needs_rebake = true; // Force rebake on load

        n->has_background = j.value("has_background", false);
        if(j.contains("background_color")) n->background_color = j["background_color"].get<glm::vec4>();
        n->background_padding = j.value("background_padding", 10.0f);
        n->has_outline = j.value("has_outline", false);
        n->outline_thickness = j.value("outline_thickness", 0.1f);
        if(j.contains("outline_color")) n->outline_color = j["outline_color"].get<glm::vec4>();
        n->alignment = static_cast<TextEffectNode::Alignment>(j.value("alignment", 1)); // Center default
        n->letter_spacing = j.value("letter_spacing", 0.0f);

    } else if (auto n = std::dynamic_pointer_cast<MergeNode>(node)) {
        n->blend_mode = static_cast<BlendMode>(j.value("blend_mode", 0));
        n->mix = j.value("mix", 1.0f);

    } else if (auto n = std::dynamic_pointer_cast<TransformNode>(node)) {
        if(j.contains("translate")) n->translate = j["translate"].get<glm::vec2>();
        if(j.contains("scale")) n->scale = j["scale"].get<glm::vec2>();
        n->rotation = j.value("rotation", 0.0f);
    } else if (auto n = std::dynamic_pointer_cast<TrackerNode>(node)) {
        if(j.contains("roi")) {
            auto roi_j = j["roi"];
            n->initial_roi_norm.x = roi_j.value("x", 0.0f);
            n->initial_roi_norm.y = roi_j.value("y", 0.0f);
            n->initial_roi_norm.width = roi_j.value("w", 0.0f);
            n->initial_roi_norm.height = roi_j.value("h", 0.0f);
        }
        if(j.contains("tracking_data") && j["tracking_data"].is_object()) {
            for(auto& [key, val] : j["tracking_data"].items()) {
                int frame = std::stoi(key);
                TransformData dat;
                dat.translate.x = val.value("tx", 0.0f);
                dat.translate.y = val.value("ty", 0.0f);
                dat.scale.x = val.value("sx", 1.0f);
                dat.scale.y = val.value("sy", 1.0f);
                dat.rotation = val.value("rot", 0.0f);
                n->tracking_data_cache[frame] = dat;
            }
        }
        // If data was loaded, it's effectively initialized "enough" for now 
        if(!n->tracking_data_cache.empty()) n->is_initialized = true;
    }

    return node;
}

// --- Helper: EffectGraph serialization ---
void to_json(json& j, const std::shared_ptr<EffectGraph>& graph) {
    if (!graph) { j = nullptr; return; }
    
    j["next_node_id"] = graph->next_node_id;
    j["next_link_id"] = graph->next_link_id;
    j["input_node_id"] = graph->input_node_id;
    j["output_node_id"] = graph->output_node_id;

    // Serialize Nodes
    json nodes_json = json::array();
    for (const auto& pair : graph->nodes) {
        SerializeEffectNode(nodes_json.emplace_back(), pair.second);
    }
    j["nodes"] = nodes_json;

    // Serialize Links
    j["links"] = graph->links; // Uses to_json(Link)
}

void from_json(const json& j, std::shared_ptr<EffectGraph>& graph) {
    if (j.is_null()) { graph = nullptr; return; }

    graph = std::make_shared<EffectGraph>();
    // Clear default nodes created by constructor (Input/Output) as we will load them from file
    graph->nodes.clear();
    graph->links.clear();
    graph->node_order.clear();

    graph->next_node_id = j.value("next_node_id", 1);
    graph->next_link_id = j.value("next_link_id", 1);
    graph->input_node_id = j.value("input_node_id", 0);
    graph->output_node_id = j.value("output_node_id", 0);

    // Deserialize Nodes
    if (j.contains("nodes") && j["nodes"].is_array()) {
        for (const auto& node_json : j["nodes"]) {
            auto node = DeserializeEffectNode(node_json);
            if (node) {
                graph->nodes[node->id] = node;
            }
        }
    }

    // Deserialize Links
    if (j.contains("links") && j["links"].is_array()) {
        graph->links = j["links"].get<std::vector<Link>>();
    }

    // Rebuild execution order
    graph->rebuild_order_from_links();
}

// --- Clip serialization ---
// This function correctly omits the `linked_clip` pointer, which is what we want.
// The pointer's representation (the index) will be added by the higher-level SaveProject function.
void to_json(json& j, const Clip& clip) {
    j = json{
        {"name", clip.name},
        {"path", clip.path},
        {"type", static_cast<int>(clip.type)},
        {"start_time", clip.start_time},
        {"duration", clip.duration},
        {"layer", clip.layer},
        {"media_start", clip.media_start},
        {"pos_x", clip.pos_x},
        {"pos_y", clip.pos_y},
        {"scale", clip.scale},
        {"rotation", clip.rotation},
        {"opacity", clip.opacity},
        {"blend_mode", static_cast<int>(clip.blend_mode)},
        {"selected", clip.selected},
        {"is_audio_only", clip.is_audio_only},
        {"has_audio", clip.has_audio},
        {"volume", clip.volume},
        {"effect_graph", clip.effect_graph}, // Use the EffectGraph serialization helper
        {"opacity_track", clip.opacity_track},
        {"pos_x_track", clip.pos_x_track},
        {"pos_y_track", clip.pos_y_track},
        {"rotation_track", clip.rotation_track},
        {"scale_track", clip.scale_track},
        {"volume_track", clip.volume_track},
        // Add more fields as needed
    };
    // Waveform is not serialized (can be regenerated)
}

// This function correctly doesn't try to deserialize the pointer.
void from_json(const json& j, Clip& clip) {
    clip.name = j.value("name", "");
    clip.path = j.value("path", "");
    clip.type = static_cast<ClipType>(j.value("type", 0));
    clip.start_time = j.value("start_time", 0.0f);
    clip.duration = j.value("duration", 0.0f);
    clip.layer = j.value("layer", 0);
    clip.media_start = j.value("media_start", 0.0f);
    clip.pos_x = j.value("pos_x", 0.0f);
    clip.pos_y = j.value("pos_y", 0.0f);
    clip.scale = j.value("scale", 1.0f);
    clip.rotation = j.value("rotation", 0.0f);
    clip.opacity = j.value("opacity", 1.0f);
    clip.blend_mode = static_cast<BlendMode>(j.value("blend_mode", 0));
    clip.selected = j.value("selected", false);
    clip.is_audio_only = j.value("is_audio_only", false);
    clip.has_audio = j.value("has_audio", false);
    clip.volume = j.value("volume", 1.0f);
    
    // Effect graph deserialization
    if (j.contains("effect_graph")) {
        // This will call the from_json helper for shared_ptr<EffectGraph>
        j["effect_graph"].get_to(clip.effect_graph);
    } else {
        clip.effect_graph = nullptr;
    }

    // Keyframe tracks
    if (j.contains("opacity_track")) from_json(j["opacity_track"], clip.opacity_track);
    if (j.contains("pos_x_track")) from_json(j["pos_x_track"], clip.pos_x_track);
    if (j.contains("pos_y_track")) from_json(j["pos_y_track"], clip.pos_y_track);
    if (j.contains("rotation_track")) from_json(j["rotation_track"], clip.rotation_track);
    if (j.contains("scale_track")) from_json(j["scale_track"], clip.scale_track);
    if (j.contains("volume_track")) from_json(j["volume_track"], clip.volume_track);
    // waveform is not loaded (regenerate after load)
    // linked_clip is not loaded here; it's handled in a second pass by the LoadProject function.
}

// --- Compression helpers (unchanged) ---
bool compress_string(const std::string& input, std::string& output) {
    uLong src_len = input.size();
    uLong dest_len = compressBound(src_len);
    std::vector<char> buffer(dest_len);

    if (compress(reinterpret_cast<Bytef*>(buffer.data()), &dest_len, reinterpret_cast<const Bytef*>(input.data()), src_len) != Z_OK)
        return false;

    output.assign(buffer.data(), dest_len);
    return true;
}

bool decompress_string(const std::string& input, std::string& output) {
    uLong dest_len = input.size() * 5; // estimated size
    std::vector<char> buffer(dest_len);

    int result = uncompress(reinterpret_cast<Bytef*>(buffer.data()), &dest_len,
                            reinterpret_cast<const Bytef*>(input.data()), input.size());

    while (result == Z_BUF_ERROR) {
        dest_len *= 2;
        buffer.resize(dest_len);
        result = uncompress(reinterpret_cast<Bytef*>(buffer.data()), &dest_len,
                            reinterpret_cast<const Bytef*>(input.data()), input.size());
    }

    if (result != Z_OK) return false;

    output.assign(buffer.data(), dest_len);
    return true;
}

// --- File-based Save/Load ---
// REWRITTEN to handle linked_clip pointers by saving their index instead.
bool SaveProject(const std::string& filename, const std::vector<Clip>& clips, float playhead_time, float zoom_factor) {
    json j;
    j["playhead_time"] = playhead_time;
    j["zoom_factor"] = zoom_factor;

    // Create a map from clip address to index for fast lookups
    std::map<const Clip*, size_t> clip_to_index_map;
    for(size_t i = 0; i < clips.size(); ++i) {
        clip_to_index_map[&clips[i]] = i;
    }

    json clips_json = json::array();
    for (const auto& clip : clips) {
        // Convert clip to json using the existing ADL function
        json clip_json = clip; 

        // Find the index of the linked clip and add it to the json object
        ptrdiff_t linked_idx = -1;
        if (clip.linked_clip) {
            auto it = clip_to_index_map.find(clip.linked_clip);
            if (it != clip_to_index_map.end()) {
                linked_idx = it->second;
            }
        }
        clip_json["linked_clip_index"] = linked_idx;
        clips_json.push_back(clip_json);
    }
    j["clips"] = clips_json;

    std::string json_str = j.dump();
    std::string compressed;
    if (!compress_string(json_str, compressed)) return false;

    std::ofstream out(filename, std::ios::binary);
    if (!out) return false;
    out.write(compressed.data(), compressed.size());
    return true;
}

// REWRITTEN to use a two-pass approach to re-link pointers after loading.
bool LoadProject(const std::string& filename, std::vector<Clip>& clips, float& playhead_time, float& zoom_factor) {
    std::ifstream in(filename, std::ios::binary | std::ios::ate);
    if (!in) return false;

    std::streamsize size = in.tellg();
    in.seekg(0, std::ios::beg);
    std::string buffer(size, '\0');
    if(size > 0) in.read(buffer.data(), size);

    std::string json_str;
    if (!decompress_string(buffer, json_str)) return false;

    json j = json::parse(json_str, nullptr, false); // No-throw parse
    if (j.is_discarded()) return false; // Handle corrupted file

    playhead_time = j.value("playhead_time", 0.0f);
    zoom_factor = j.value("zoom_factor", 1.0f);
    
    // --- Two-pass loading to re-establish links ---
    clips.clear();
    if (!j.contains("clips") || !j["clips"].is_array()) {
        return true; // Project might be empty, which is valid.
    }

    const json& clips_json = j["clips"];
    std::vector<ptrdiff_t> link_indices;
    link_indices.reserve(clips_json.size());

    // First pass: Deserialize all clip data and store link indices
    for (const auto& jc : clips_json) {
        clips.push_back(jc.get<Clip>());
        link_indices.push_back(jc.value("linked_clip_index", (ptrdiff_t)-1));
    }

    // Second pass: Re-establish pointers
    for (size_t i = 0; i < clips.size(); ++i) {
        ptrdiff_t linked_idx = link_indices[i];
        if (linked_idx >= 0 && static_cast<size_t>(linked_idx) < clips.size()) {
            clips[i].linked_clip = &clips[linked_idx];
        } else {
            clips[i].linked_clip = nullptr;
        }
    }
    return true;
}

// --- Stream-based Save/Load for Undo/Redo ---
// REWRITTEN to handle linked_clip pointers correctly.
bool SaveProjectToStream(std::ostream& out, const std::vector<Clip>& clips, float playhead_time, float zoom_factor) {
    json j;
    j["playhead_time"] = playhead_time;
    j["zoom_factor"] = zoom_factor;

    std::map<const Clip*, size_t> clip_to_index_map;
    for(size_t i = 0; i < clips.size(); ++i) {
        clip_to_index_map[&clips[i]] = i;
    }

    json clips_json = json::array();
    for (const auto& clip : clips) {
        json clip_json = clip;
        ptrdiff_t linked_idx = -1;
        if (clip.linked_clip) {
            auto it = clip_to_index_map.find(clip.linked_clip);
            if (it != clip_to_index_map.end()) {
                linked_idx = it->second;
            }
        }
        clip_json["linked_clip_index"] = linked_idx;
        clips_json.push_back(clip_json);
    }
    j["clips"] = clips_json;
    
    out << j.dump();
    return true;
}

// REWRITTEN to handle re-linking pointers correctly.
bool LoadProjectFromStream(std::istream& in, std::vector<Clip>& clips, float& playhead_time, float& zoom_factor) {
    json j;
    in >> j;
    if (in.fail()) return false;

    playhead_time = j.value("playhead_time", 0.0f);
    zoom_factor = j.value("zoom_factor", 1.0f);
    
    clips.clear();
    if (!j.contains("clips") || !j["clips"].is_array()) {
        return true;
    }

    const json& clips_json = j["clips"];
    std::vector<ptrdiff_t> link_indices;
    link_indices.reserve(clips_json.size());

    // Pass 1: Deserialize clips and get link indices
    for (const auto& jc : clips_json) {
        clips.push_back(jc.get<Clip>());
        link_indices.push_back(jc.value("linked_clip_index", (ptrdiff_t)-1));
    }

    // Pass 2: Re-establish links
    for (size_t i = 0; i < clips.size(); ++i) {
        ptrdiff_t linked_idx = link_indices[i];
        if (linked_idx >= 0 && static_cast<size_t>(linked_idx) < clips.size()) {
            clips[i].linked_clip = &clips[linked_idx];
        } else {
            clips[i].linked_clip = nullptr;
        }
    }
    return true;
}