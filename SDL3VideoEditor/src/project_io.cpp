#include "project_io.hpp"
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

// --- Helper: EffectGraph serialization (stub, extend as needed) ---
void to_json(json& j, const std::shared_ptr<EffectGraph>& graph) {
    // TODO: Implement effect graph serialization if needed
    j = nullptr;
}
void from_json(const json& j, std::shared_ptr<EffectGraph>& graph) {
    // TODO: Implement effect graph deserialization if needed
    graph = nullptr;
}

// --- Clip serialization (NO CHANGE NEEDED HERE) ---
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
        {"effect_graph", nullptr}, // <-- PATCHED: do not dereference!
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
    // Effect graph: just set to nullptr for now
    clip.effect_graph = nullptr;
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