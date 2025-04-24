#include "project_io.hpp"
#include <fstream>
#include <sstream>
#include <nlohmann/json.hpp>
#include <miniz.h>

using json = nlohmann::json;

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

bool SaveProject(const std::string& filename, const std::vector<Clip>& clips, float playhead_time, float zoom_factor) {
    json j;
    j["playhead_time"] = playhead_time;
    j["zoom_factor"] = zoom_factor;

    for (const auto& clip : clips) {
        j["clips"].push_back({
            {"name", clip.name},
            {"path", clip.path},
            {"start_time", clip.start_time},
            {"duration", clip.duration},
            {"layer", clip.layer},
            {"media_start", clip.media_start},
            {"pos_x", clip.pos_x},
            {"pos_y", clip.pos_y},
            {"scale", clip.scale},
            {"opacity", clip.opacity}
        });
    }

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

    for (const auto& jc : j["clips"]) {
        Clip clip;
        clip.name = jc.value("name", "");
        clip.path = jc.value("path", "");
        clip.start_time = jc.value("start_time", 0.0f);
        clip.duration = jc.value("duration", 0.0f);
        clip.layer = jc.value("layer", 0);
        clip.media_start = jc.value("media_start", 0.0f);
        clip.pos_x = jc.value("pos_x", 0.0f);
        clip.pos_y = jc.value("pos_y", 0.0f);
        clip.scale = jc.value("scale", 1.0f);
        clip.opacity = jc.value("opacity", 1.0f);
        clips.push_back(clip);
    }

    return true;
}
