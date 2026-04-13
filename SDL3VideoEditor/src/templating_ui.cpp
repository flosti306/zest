#include "templating_ui.hpp"
#include "templating.hpp"
#include <imgui.h>
#include <string>
#include <vector>
#include <cstring>
#include <iostream>
#include <fstream>
#include "project_io.hpp" // For LoadProject used in preview/instantiation
#include "effects.hpp"    // For EffectGraph, TextEffectNode, etc.
#include <tinyfiledialogs.h>

// Helper to handle string input with buffers
// defined in main.cpp usually, but we'll inline a local helper if needed.
// Actually, let's just use fixed buffers for simplicity as per codebase style.

// Helper to draw a binding field
void DrawBindingField(const char* label, const std::string& target_uid, TemplateManager& tm) {
    std::string current_expr = tm.GetBinding(target_uid).value_or("");
    char buf[256];
    strncpy(buf, current_expr.c_str(), sizeof(buf));
    
    ImGui::Text("%s", label);
    ImGui::SameLine();
    ImGui::PushID(target_uid.c_str());
    ImGui::SetNextItemWidth(200);
    if (ImGui::InputText("##expr", buf, sizeof(buf))) {
        if (strlen(buf) > 0)
            tm.SetBinding(target_uid, buf);
        else
            tm.RemoveBinding(target_uid);
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Enter variable name (e.g. {{SongFile}}) or formula");
    ImGui::PopID();
}

void DrawTemplateDesignerWindow(bool* p_open, std::vector<Clip>& active_project) {
    if (!*p_open) return;

    ImGui::SetNextWindowSize(ImVec2(800, 600), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Template Designer", p_open)) {
        
        TemplateManager& tm = TemplateManager::Get();
        auto& variables = tm.GetVariables();

        if (ImGui::BeginTabBar("DesignerTabs")) {
            
            // --- Tab 1: Variables ---
            if (ImGui::BeginTabItem("Variables")) {
                ImGui::Text("Define variables that can be customized when instantiating this project.");
                ImGui::Separator();

                if (ImGui::BeginTable("VariablesTable", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable)) {
                    // ... (Table logic same as before, condensed for brevity/correctness) ...
                    ImGui::TableSetupColumn("Name"); ImGui::TableSetupColumn("Type"); ImGui::TableSetupColumn("Default Value"); ImGui::TableSetupColumn("Min/Max"); ImGui::TableSetupColumn("Actions");
                    ImGui::TableHeadersRow();

                    int to_remove = -1;
                    for (int i = 0; i < variables.size(); ++i) {
                        ImGui::PushID(i);
                        auto& var = variables[i];
                        ImGui::TableNextRow();
                        
                        ImGui::TableSetColumnIndex(0);
                        char name_buf[64]; strncpy(name_buf, var.name.c_str(), sizeof(name_buf));
                        if (ImGui::InputText("##Name", name_buf, sizeof(name_buf))) var.name = name_buf;

                        ImGui::TableSetColumnIndex(1);
                        const char* types[] = { "Float", "String", "FilePath", "Color" };
                        int type_idx = static_cast<int>(var.type);
                        if (ImGui::Combo("##Type", &type_idx, types, IM_ARRAYSIZE(types))) var.type = static_cast<VariableType>(type_idx);

                        ImGui::TableSetColumnIndex(2);
                        if (var.type == VariableType::Color) {
                             glm::vec4 col(1.0f);
                             std::string val = var.default_value;
                             if (!val.empty()) {
                                 std::stringstream ss(val); char comma;
                                 if (val.find(',') != std::string::npos) { ss >> col.r >> comma >> col.g >> comma >> col.b >> comma >> col.a; }
                                 else { ss >> col.r >> col.g >> col.b >> col.a; }
                             }
                             if (ImGui::ColorEdit4("##Color", &col[0], ImGuiColorEditFlags_NoInputs)) {
                                 char cbuf[128];
                                 snprintf(cbuf, sizeof(cbuf), "%.3f,%.3f,%.3f,%.3f", col.r, col.g, col.b, col.a);
                                 var.default_value = cbuf;
                             }
                        } else {
                            char def_buf[256]; strncpy(def_buf, var.default_value.c_str(), sizeof(def_buf));
                            if (ImGui::InputText("##Default", def_buf, sizeof(def_buf))) var.default_value = def_buf;
                        }

                        ImGui::TableSetColumnIndex(3);
                        if (var.type == VariableType::Float) {
                            ImGui::SetNextItemWidth(60); ImGui::DragFloat("##Min", &var.min_val); ImGui::SameLine();
                            ImGui::SetNextItemWidth(60); ImGui::DragFloat("##Max", &var.max_val);
                        } else ImGui::TextDisabled("--");

                        ImGui::TableSetColumnIndex(4);
                        if (ImGui::Button("Delete")) to_remove = i;
                        ImGui::PopID();
                    }
                    ImGui::EndTable();

                    if (to_remove != -1) variables.erase(variables.begin() + to_remove);
                }
                if (ImGui::Button("+ Add Variable")) tm.RegisterVariable("NewVar", VariableType::Float, "0");
                ImGui::EndTabItem();
            }

            // --- Tab 2: Bindings ---
            if (ImGui::BeginTabItem("Bindings")) {
                ImGui::Text("Bind Project Properties to Variables");
                ImGui::Separator();
                
                ImGui::BeginChild("BindingTree");
                for (auto& clip : active_project) {
                    std::string clip_uid = "Clip:" + std::to_string(clip.uid);
                    
                    if (ImGui::TreeNode((void*)(intptr_t)clip.uid, "Clip: %s (ID: %d)", clip.name.c_str(), clip.uid)) {
                        DrawBindingField("Start Time", clip_uid + ".start_time", tm);
                        DrawBindingField("Duration", clip_uid + ".duration", tm);
                        DrawBindingField("File Path", clip_uid + ".path", tm); // For media replacement
                        
                        if (clip.effect_graph) {
                            if (ImGui::TreeNode("Effects")) {
                                for(auto& [id, node] : clip.effect_graph->nodes) {
                                    std::string node_uid = clip_uid + ".Effect:" + std::to_string(id);
                                    if (ImGui::TreeNode((void*)(intptr_t)id, "%s", node->name.c_str())) {
                                        // Common
                                        DrawBindingField("Enabled", node_uid + ".enabled", tm);

                                        // Specific
                                        if (auto n = std::dynamic_pointer_cast<TextEffectNode>(node)) {
                                            DrawBindingField("Content", node_uid + ".text_content", tm);
                                            DrawBindingField("Font Size", node_uid + ".font_size", tm);
                                            DrawBindingField("Text Color", node_uid + ".text_color", tm);
                                        } else if (auto n = std::dynamic_pointer_cast<GaussianBlurNode>(node)) {
                                            DrawBindingField("Blur Amount", node_uid + ".blur_amount", tm);
                                        } else if (auto n = std::dynamic_pointer_cast<TransformNode>(node)) {
                                            DrawBindingField("Scale X", node_uid + ".scale.x", tm); // Simplify accessing vector components?
                                        } else if (auto n = std::dynamic_pointer_cast<SolidColorEffectNode>(node)) {
                                            DrawBindingField("Color", node_uid + ".color", tm);
                                        } else if (auto n = std::dynamic_pointer_cast<DropShadowEffectNode>(node)) {
                                            DrawBindingField("Shadow Color", node_uid + ".shadow_color", tm);
                                        } else if (auto n = std::dynamic_pointer_cast<ChromaKeyNode>(node)) {
                                            DrawBindingField("Key Color", node_uid + ".key_color", tm);
                                        }
                                        ImGui::TreePop();
                                    }
                                }
                                ImGui::TreePop();
                            }
                        }
                        ImGui::TreePop();
                    }
                }
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    }
    ImGui::End();
}

// Global state for invalidation
static std::vector<Clip> cached_template_clips;
static std::string cached_template_path = "";
static std::map<std::string, std::string> current_input_values;

void DrawInsertTemplateDialog(bool* p_open, std::vector<Clip>& active_project, float insert_time, GLResources& gl_resources) {
    if (!*p_open) return;

    if (ImGui::Begin("Insert Template", p_open)) {
        static char template_path_buf[1024] = "";
        
        // File Selector
        if (ImGui::Button("Browse...")) {
             const char* filterPatterns[2] = { "*.zestt", "*.json" };
            const char* path = tinyfd_openFileDialog("Open Template", "", 2, filterPatterns, "Zest Template", 0);
            if (path) {
                strncpy(template_path_buf, path, sizeof(template_path_buf));
                // Load the template to find variables
                // We use a dummy project load just to get the template data?
                // Actually LoadProject clears the template manager... that's tricky.
                // We need to load the template variables WTIHOUT affecting the global TemplateManager.
                // We'll manually parse the JSON for this dialog.
                std::ifstream in(path, std::ios::binary);
                if(in) {
                     // Decompress/Parse logic duplicated? 
                     // Ideally we refactor LoadProject to LoadProjectData(json& j).
                     // For now, let's just assume we can use a temporary TemplateManager or manual parsing?
                     // Manual parsing is safest to avoid global state impacts.
                     // But we need to use the `LoadProject` function to get the clips!
                     
                     // Helper: Load just the variables from the file for UI
                     // ...
                     // Actually, let's load the whole project into `cached_template_clips`
                     // and populate `current_input_values` with defaults.
                     cached_template_path = path;
                     
                     float dummy_ph, dummy_zoom;
                     (void)dummy_ph; (void)dummy_zoom; // Silence warnings
                     std::vector<Clip> temp_clips;
                     // We need to temporarily backup the global TemplateManager state? 
                     // Or just instatiate a local one?
                     // Issue: LoadProject calls TemplateManager::Get().LoadFromJSON...
                     
                     // FIX: We should probably make LoadProject take an optional TemplateManager*?
                     // Or, we just accept that loading a template overwrites the global template vars...
                     // WAIT! If we are inserting a template into a MASTER project, the master project might NOT have variables.
                     // BUT, if we overwrite the global TM, we lose any master project/current settings if there were any!
                     // However, "Template Designer" defines variables for the *current* project.
                     // When *instantiating*, we represent a *consumer* of a template.
                     // The global TemplateManager is for *defining* the current project's template interface.
                     // It is NOT used for *instantiating* other templates (which have their own definitions).
                     
                     // START CORRECT ARCHICTECTURE:
                     // The `Instantiate` function takes `template_clips` and `context`.
                     // We need to know what variables the template EXPECTS.
                     // So we need to deserialize the template's variable definitions into a LOCAL list.
                     
                     // I will implement a helper `LoadTemplateMetadata` in templating.cpp later?
                     // For now, I'll inline a dirty JSON load here since I have access to nlohmann headers via templating.hpp/project_io includes.
                     
                     // Read file...
                     // Decompress...
                     // Parse JSON...
                     // Extract "template_data" -> "variables".
                }
            }
        }
        ImGui::SameLine();
        ImGui::InputText("Path", template_path_buf, sizeof(template_path_buf), ImGuiInputTextFlags_ReadOnly);

        // Logic to load/parse if path changed
        static std::vector<TemplateVariable> instance_variables;
        
        static std::string last_processed_path = "";
        if (std::string(template_path_buf) != last_processed_path && strlen(template_path_buf) > 0) {
            // Reload metadata
            instance_variables.clear();
            current_input_values.clear();
            cached_template_clips.clear();
            
            // Re-use logic from project_io to get the JSON... 
            // This is duplicative but unavoidable without refactoring project_io.
            // I'll skip decompression for now and assume the user saves uncompressed or I copy the decompress logic...
            // Wait, `project_io.cpp` has `decompress_string` but it's not in the header.
            // I should have exposed `decompress_string` in `project_io.hpp`.
            // Check project_io.hpp...
            
            // Assuming I can't easily access decompress right now, I'll assume users use valid files
            // and maybe I'll skip the preview if it's complex.
            // Actually, I can `LoadProject` into a dummy vector!
            // But `LoadProject` calls `TemplateManager::Get().LoadFromJSON`.
            // This IS a side effect.
            // I need to save/restore the global TemplateManager state.
            
            nlohmann::json saved_state;
            TemplateManager::Get().SaveToJSON(saved_state);
            
            float d1, d2;
            LoadProject(template_path_buf, cached_template_clips, d1, d2); 
            
            // Now TemplateManager has the TEMPLATE'S variables. copy them.
            instance_variables = TemplateManager::Get().GetVariables();
            // initialize defaults
            for(const auto& v : instance_variables) {
                current_input_values[v.name] = v.default_value;
            }
            
            // Restore global state
            TemplateManager::Get().LoadFromJSON(saved_state);
            
            last_processed_path = template_path_buf;
        }

        ImGui::Separator();

        // Form
        for (const auto& var : instance_variables) {
            std::string val = current_input_values[var.name];
            char buf[256];
            strncpy(buf, val.c_str(), sizeof(buf));
            
            std::string label = var.name; 
            if (var.type == VariableType::FilePath) {
                if (ImGui::Button(("Browse##" + var.name).c_str())) {
                    const char* p = tinyfd_openFileDialog("Select Media", "", 0, NULL, NULL, 0);
                    if(p) {
                         strncpy(buf, p, sizeof(buf));
                         current_input_values[var.name] = p;
                    }
                }
                ImGui::SameLine();
                if (ImGui::InputText(label.c_str(), buf, sizeof(buf))) {
                    current_input_values[var.name] = buf;
                }
            } else if (var.type == VariableType::Color) {
                 glm::vec4 col(1.0f);
                 if (!val.empty()) {
                     std::stringstream ss(val); char comma;
                     if (val.find(',') != std::string::npos) { ss >> col.r >> comma >> col.g >> comma >> col.b >> comma >> col.a; }
                     else { ss >> col.r >> col.g >> col.b >> col.a; }
                 }
                 if (ImGui::ColorEdit4(label.c_str(), &col[0])) {
                     char cbuf[128];
                     snprintf(cbuf, sizeof(cbuf), "%.3f,%.3f,%.3f,%.3f", col.r, col.g, col.b, col.a);
                     current_input_values[var.name] = cbuf;
                 }
            } else {
                if (ImGui::InputText(label.c_str(), buf, sizeof(buf))) {
                    current_input_values[var.name] = buf;
                }
            }
        }

        ImGui::Separator();
        
        static float insertion_offset = 0.0f; 
        (void)insertion_offset; // Silence warning until used for advanced offsetting 
        // We default insertion_offset to insert_time passed in.
        // But we want to allow editing?
        // Let's just use the passed insert_time as the base.
        ImGui::Text("Insert at: %.2fs", insert_time);

        if (ImGui::Button("Insert")) {
            // Instantiate!
            // 1. We need the template bindings.
            // We need to get them from the file again? 
            // We loaded the project, so `cached_template_clips` are loaded.
            // BUT `LoadProject` loads CLIPS, but the BINDINGS are inside `TemplateManager`.
            // When we did the Save/Restore dance above, we lost the bindings of the template!
            // We preserved the *Variables* in `instance_variables`, but `Instantiate` needs the `bindings` map!
            
            // FIX: We need `instance_bindings` too.
            // I will add that to the "Reload metadata" block.
            
            // ... (Logic will be fixed in next step when I realize I need to actually code it)
            
            // For now, let's assume we reload it one last time to be sure.
             nlohmann::json saved_state;
            TemplateManager::Get().SaveToJSON(saved_state);
            
            float d1, d2;
            LoadProject(template_path_buf, cached_template_clips, d1, d2); 
            
            // Now TM has the template's bindings.
            // Instantiating uses the GLOBAL TM bindings?
            // No, `Instantiate` is a method on TM. It uses `this->bindings`.
            // So we are in the perfect state to call Instantiate!
            
            size_t old_size = active_project.size();
            TemplateManager::Get().Instantiate(active_project, cached_template_clips, current_input_values, insert_time);
            
            for (size_t i = old_size; i < active_project.size(); ++i) {
                load_resources_for_clip(gl_resources, active_project[i]);
            }
            
            // Now Restore.
            TemplateManager::Get().LoadFromJSON(saved_state);
            
            *p_open = false;
        }
    }
    ImGui::End();
}
