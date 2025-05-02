#pragma once

#include <vector>
#include <string>
#include "shared.hpp"  // or wherever `Node` is defined
#include "node.hpp"    // For MediaNode and NodeType

bool SaveProject(const std::string& path, 
    const std::vector<MediaNode>& nodes,
    float project_duration,
    float frame_rate);

bool LoadProject(const std::string& path,
    std::vector<MediaNode>& nodes,
    float& project_duration,
    float& frame_rate);
