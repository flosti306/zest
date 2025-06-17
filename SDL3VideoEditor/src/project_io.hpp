#pragma once

#include <vector>
#include <string>
#include <sstream>
#include "shared.hpp"  // or wherever `Clip` is defined

bool SaveProject(const std::string& filename, const std::vector<Clip>& clips, float playhead_time, float zoom_factor);
bool LoadProject(const std::string& filename, std::vector<Clip>& clips, float& playhead_time, float& zoom_factor);

// New: stream-based for undo/redo
bool SaveProjectToStream(std::ostream& out, const std::vector<Clip>& clips, float playhead_time, float zoom_factor);
bool LoadProjectFromStream(std::istream& in, std::vector<Clip>& clips, float& playhead_time, float& zoom_factor);