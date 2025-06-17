#include "project_io.hpp"
#include <fstream>
#include <sstream>
#include <nlohmann/json.hpp>
#include <miniz.h>

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

// --- Clip serialization ---
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
bool SaveProject(const std::string& filename, const std::vector<Clip>& clips, float playhead_time, float zoom_factor) {
    json j;
    j["playhead_time"] = playhead_time;
    j["zoom_factor"] = zoom_factor;
    j["clips"] = clips;

    std::string json_str = j.dump();
    std::string compressed;
    if (!compress_string(json_str, compressed)) return false;

    std::ofstream out(filename, std::ios::binary);
    if (!out) return false;
    out.write(compressed.data(), compressed.size());
    return true;
}

bool LoadProject(const std::string& filename, std::vector<Clip>& clips, float& playhead_time, float& zoom_factor) {
    std::ifstream in(filename, std::ios::binary | std::ios::ate);
    if (!in) return false;

    std::streamsize size = in.tellg();
    in.seekg(0, std::ios::beg);
    std::string buffer(size, '\0');
    in.read(buffer.data(), size);

    std::string json_str;
    if (!decompress_string(buffer, json_str)) return false;

    json j = json::parse(json_str);

    playhead_time = j.value("playhead_time", 0.0f);
    zoom_factor = j.value("zoom_factor", 1.0f);
    clips.clear();
    if (j.contains("clips")) {
        for (const auto& jc : j["clips"]) {
            clips.push_back(jc.get<Clip>());
        }
    }
    return true;
}

// --- Stream-based Save/Load for Undo/Redo ---
bool SaveProjectToStream(std::ostream& out, const std::vector<Clip>& clips, float playhead_time, float zoom_factor) {
    json j;
    j["playhead_time"] = playhead_time;
    j["zoom_factor"] = zoom_factor;
    j["clips"] = clips;
    out << j.dump();
    return true;
}
bool LoadProjectFromStream(std::istream& in, std::vector<Clip>& clips, float& playhead_time, float& zoom_factor) {
    json j;
    in >> j;
    playhead_time = j.value("playhead_time", 0.0f);
    zoom_factor = j.value("zoom_factor", 1.0f);
    clips.clear();
    if (j.contains("clips")) {
        for (const auto& jc : j["clips"]) {
            clips.push_back(jc.get<Clip>());
        }
    }
    return true;
}
