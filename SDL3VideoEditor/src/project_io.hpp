#pragma once
#include <string>
#include <vector>
#include <map>
#include <memory>
#include "shared.hpp" // Clip definition
#include "node.hpp"   // Node definition

bool SaveProject(const std::string& file_path,
                 const std::map<std::string, Clip>& library,
                 const std::vector<std::shared_ptr<Node>>& root_nodes);

bool LoadProject(const std::string& file_path,
                 std::map<std::string, Clip>& library_out,
                 std::vector<std::shared_ptr<Node>>& root_nodes_out);