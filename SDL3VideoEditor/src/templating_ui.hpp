#pragma once
#include <vector>
#include "shared.hpp"

struct GLResources;

void DrawTemplateDesignerWindow(bool* p_open, std::vector<Clip>& active_project);
void DrawInsertTemplateDialog(bool* p_open, std::vector<Clip>& active_project, float insert_time, GLResources& gl_resources);
