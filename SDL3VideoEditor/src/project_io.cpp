#include "video_export.hpp"
#include <fstream>
#include <nlohmann/json.hpp>  // If using JSON for serialization

using json = nlohmann::json;

bool SaveProject(const std::string& path, 
                const std::vector<MediaNode>& nodes,
                float project_duration,
                float frame_rate) 
{
    try {
        json project;
        
        // Serialize nodes
        json json_nodes;
        for (const auto& node : nodes) {
            json jnode;
            jnode["name"] = node.name;
            jnode["src"] = node.src;
            jnode["start_time"] = node.start_time;
            // ... add other properties
            json_nodes.push_back(jnode);
        }
        
        project["nodes"] = json_nodes;
        project["duration"] = project_duration;
        project["frame_rate"] = frame_rate;

        std::ofstream file(path);
        file << project.dump(4);
        return true;
    }
    catch (const std::exception& e) {
        std::cerr << "Save failed: " << e.what() << std::endl;
        return false;
    }
}

bool LoadProject(const std::string& path,
                std::vector<MediaNode>& nodes,
                float& project_duration,
                float& frame_rate)
{
    try {
        std::ifstream file(path);
        json project = json::parse(file);
        
        // Clear existing nodes
        nodes.clear();
        
        // Load nodes
        for (const auto& jnode : project["nodes"]) {
            MediaNode node;
            node.name = jnode["name"];
            node.src = jnode["src"];
            node.start_time = jnode["start_time"];
            // ... load other properties
            nodes.push_back(node);
        }
        
        project_duration = project["duration"];
        frame_rate = project["frame_rate"];
        
        return true;
    }
    catch (const std::exception& e) {
        std::cerr << "Load failed: " << e.what() << std::endl;
        return false;
    }
}