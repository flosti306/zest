#include "project_io.hpp"
#include <fstream>
#include <iostream>
// Include a JSON library (like nlohmann/json) or use a custom binary format
#include <nlohmann/json.hpp> // Example using JSON

using json = nlohmann::json;

// --- JSON Serialization Helpers (Example) ---
void to_json(json& j, const Keyframe<float>& kf) { /* ... */ }
void from_json(const json& j, Keyframe<float>& kf) { /* ... */ }
void to_json(json& j, const KeyframeTrack<float>& t) { j = t.keyframes; }
void from_json(const json& j, KeyframeTrack<float>& t) { t.keyframes = j.get<std::vector<Keyframe<float>>>(); }
void to_json(json& j, const Clip& c) { /* ... save path, type, source_duration ... */ }
void from_json(const json& j, Clip& c) { /* ... load path, type, source_duration ... */ }

// Recursive function to serialize node hierarchy
void SerializeNode(json& j_node, const Node* node) {
    if (!node) return;
    j_node["id"] = node->id;
    j_node["type"] = node->type; // Enum to string/int
    j_node["name"] = node->name;
    j_node["start_time"] = node->start_time;
    j_node["duration"] = node->duration;
    // ... other common properties (layer, visible, blend_mode) ...
    // ... fallback transforms (pos_x, ...) ...
    // ... keyframe tracks (pos_x_track, ...) ...

    if (node->type == NodeType::Media) {
        const MediaNode* mn = static_cast<const MediaNode*>(node);
        j_node["source_clip_path"] = mn->source_clip ? mn->source_clip->path : "";
        j_node["media_start"] = mn->media_start;
        j_node["is_audio"] = mn->is_audio;
         auto linked = mn->linked_node.lock();
         j_node["linked_node_id"] = linked ? linked->id : 0; // Save ID 0 if no link
    }
     // else if (node->type == NodeType::Group) { /* Group specific */ }

    j_node["children"] = json::array();
    for (const auto& child : node->children) {
        json j_child;
        SerializeNode(j_child, child.get());
        j_node["children"].push_back(j_child);
    }
}

// Recursive function to deserialize node hierarchy
std::shared_ptr<Node> DeserializeNode(const json& j_node,
                                      const std::map<std::string, Clip*>& library_lookup,
                                      std::map<size_t, Node*>& node_id_lookup) { // Map to store created nodes by ID
    NodeType type = j_node["type"].get<NodeType>();
    std::shared_ptr<Node> node = nullptr;

    // Create node based on type
    if (type == NodeType::Media) node = std::make_shared<MediaNode>();
    else if (type == NodeType::Group) node = std::make_shared<GroupNode>();
    // ... other types ...
    if (!node) return nullptr;

    node->id = j_node["id"];
    node->name = j_node["name"];
    // ... load common properties ...
    // ... load fallback transforms ...
    // ... load keyframe tracks ...

    // Store in lookup map *before* processing children/links
    node_id_lookup[node->id] = node.get();

    if (type == NodeType::Media) {
        MediaNode* mn = static_cast<MediaNode*>(node.get());
        std::string path = j_node["source_clip_path"];
        if (!path.empty() && library_lookup.count(path)) {
            mn->source_clip = library_lookup.at(path);
        }
        mn->media_start = j_node["media_start"];
        mn->is_audio = j_node["is_audio"];
        // Link retrieval happens *after* all nodes are deserialized
    }

    // Deserialize children
    for (const auto& j_child : j_node["children"]) {
         std::shared_ptr<Node> child_node = DeserializeNode(j_child, library_lookup, node_id_lookup);
         if (child_node) {
             node->AddChild(child_node); // Sets parent pointer
         }
    }
    return node;
}


bool SaveProject(const std::string& file_path,
                 const std::map<std::string, Clip>& library,
                 const std::vector<std::shared_ptr<Node>>& root_nodes) {
    try {
        json project_json;
        project_json["version"] = 1; // Add versioning
        project_json["media_library"] = library; // Uses Clip to_json

        project_json["timeline_root_nodes"] = json::array();
        for (const auto& root : root_nodes) {
            json j_root;
            SerializeNode(j_root, root.get());
            project_json["timeline_root_nodes"].push_back(j_root);
        }

        std::ofstream file(file_path);
        file << project_json.dump(4); // Pretty print JSON
        return true;

    } catch (const std::exception& e) {
        std::cerr << "Save Error: " << e.what() << std::endl;
        return false;
    }
}

bool LoadProject(const std::string& file_path,
                 std::map<std::string, Clip>& library_out,
                 std::vector<std::shared_ptr<Node>>& root_nodes_out) {
    try {
        std::ifstream file(file_path);
        if (!file.is_open()) return false;
        json project_json = json::parse(file);

        // Load Library
        library_out = project_json["media_library"].get<std::map<std::string, Clip>>();

        // Create lookup map for library clips (path -> Clip*)
        std::map<std::string, Clip*> library_lookup;
        for (auto& [path, clip] : library_out) {
            library_lookup[path] = &clip;
        }

        // Load Nodes
        root_nodes_out.clear();
        std::map<size_t, Node*> node_id_lookup; // To relink parents/links
        std::map<size_t, size_t> node_link_requests; // Store requests: node_id -> linked_node_id

         // First pass: Deserialize all nodes and build ID lookup
        for (const auto& j_root : project_json["timeline_root_nodes"]) {
             std::shared_ptr<Node> root_node = DeserializeNode(j_root, library_lookup, node_id_lookup);
             if (root_node) {
                 root_nodes_out.push_back(root_node);
             }
             // Store link requests during first pass (needs modification in DeserializeNode)
             // Example: if j_node has "linked_node_id" > 0, store node_id -> linked_id
             // This needs a recursive way to traverse the loaded json first... or add link id to node struct temporarily.
             // Simplified approach for now: Assume linking needs a second pass.
        }


        // Second pass: Relink MediaNodes (if needed) - requires iterating through all loaded nodes
         std::function<void(Node*, const json&)> relink_node =
             [&](Node* node, const json& j_node) {
             if (!node) return;
             if (node->type == NodeType::Media) {
                 size_t linked_id = j_node.value("linked_node_id", (size_t)0); // Get linked ID from JSON
                 if (linked_id > 0 && node_id_lookup.count(linked_id)) {
                     Node* linked_target = node_id_lookup[linked_id];
                     // Use weak_ptr to link
                     static_cast<MediaNode*>(node)->linked_node = std::dynamic_pointer_cast<Node>(linked_target->shared_from_this()); // Error-prone if shared_ptr doesn't exist?
                     // Need a way to get shared_ptr from raw pointer map - maybe store shared_ptrs?

                     // Alternative: Store weak_ptr in lookup map?
                     // Or just iterate through all loaded nodes again to find the target ID.
                 }
             }
             size_t child_idx = 0;
              for (const auto& child : node->children) {
                 if (j_node.contains("children") && child_idx < j_node["children"].size()) {
                     relink_node(child.get(), j_node["children"][child_idx]);
                 }
                 child_idx++;
             }
         };

         size_t root_idx = 0;
         for(const auto& root : root_nodes_out) {
              if (root_idx < project_json["timeline_root_nodes"].size()) {
                 relink_node(root.get(), project_json["timeline_root_nodes"][root_idx]);
              }
              root_idx++;
         }


        return true;

    } catch (const std::exception& e) {
        std::cerr << "Load Error: " << e.what() << std::endl;
        library_out.clear();
        root_nodes_out.clear();
        return false;
    }
}