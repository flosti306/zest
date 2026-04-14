#include "templating.hpp"
#include "effects.hpp" // For EffectNode, etc.
#include "shared.hpp"  // For Clip, EffectGraph
#include <cmath>
#include <iostream>
#include <regex>
#include <stack>


// --- Expression Parser Helper ---
// Basic recursive descent or shunting-yard for + - * / and variable
// substitution. We'll use a simple token substitution followed by a tiny
// evaluator.

// Helper to replace {{ var }} with value
std::string
ResolveVariables(const std::string &expression,
                 const std::map<std::string, std::string> &context) {
  std::string result = expression;
  for (const auto &[name, val] : context) {
    std::string placeholder = "{{" + name + "}}";
    // Simple replace all occurrences
    size_t pos = 0;
    while ((pos = result.find(placeholder, pos)) != std::string::npos) {
      result.replace(pos, placeholder.length(), val);
      pos += val.length();
    }
  }
  // Also try without spaces {{var}}
  for (const auto &[name, val] : context) {
    std::string placeholder = "{{" + name + "}}";
    // Simple replace all occurrences
    size_t pos = 0;
    while ((pos = result.find(placeholder, pos)) != std::string::npos) {
      result.replace(pos, placeholder.length(), val);
      pos += val.length();
    }
  }
  return result;
}

// Very basic math evaluator (handles +, -, *, /, parenthesis)
// Note: This is a placeholder. For production, use TinyExpr or similar.
// We'll implement a minimal one here for float evaluation.
float EvaluateMath(std::string expr) {
  // Remove spaces
  expr.erase(std::remove(expr.begin(), expr.end(), ' '), expr.end());
  if (expr.empty())
    return 0.0f;

  // TODO: Implement parsing. For now, we support simple "Value" or
  // "Value+Value" Ideally we link a header-only parser. Let's assume for this
  // MVP that expressions are single values or simple offsets. Real
  // implementation would go here.
  try {
    // Very basic: if it contains +, split and add.
    if (expr.find('+') != std::string::npos) {
      size_t pos = expr.find('+');
      return EvaluateMath(expr.substr(0, pos)) +
             EvaluateMath(expr.substr(pos + 1));
    }
    if (expr.find('-') != std::string::npos) {
      // Handle negative numbers at start?
      // This is quick and dirty
      size_t pos = expr.find('-', 1); // Skip first char if it's -
      if (pos != std::string::npos)
        return EvaluateMath(expr.substr(0, pos)) -
               EvaluateMath(expr.substr(pos + 1));
    }
    return std::stof(expr);
  } catch (...) {
    return 0.0f;
  }
}

void TemplateManager::RegisterVariable(const std::string &name,
                                       VariableType type,
                                       const std::string &default_val) {
  for (auto &v : variables) {
    if (v.name == name) {
      v.type = type;
      v.default_value = default_val;
      return;
    }
  }
  variables.push_back({name, type, default_val});
}

void TemplateManager::RemoveVariable(const std::string &name) {
  variables.erase(
      std::remove_if(variables.begin(), variables.end(),
                     [&](const TemplateVariable &v) { return v.name == name; }),
      variables.end());
}

void TemplateManager::SetBinding(const std::string &target_uid,
                                 const std::string &expression) {
  bindings[target_uid] = expression;
}

void TemplateManager::RemoveBinding(const std::string &target_uid) {
  bindings.erase(target_uid);
}

std::optional<std::string>
TemplateManager::GetBinding(const std::string &target_uid) {
  auto it = bindings.find(target_uid);
  if (it != bindings.end())
    return it->second;
  return std::nullopt;
}

float TemplateManager::EvaluateFloat(
    const std::string &expression,
    const std::map<std::string, float> &numeric_context) {
  // 1. Substitute known numeric variables
  std::string resolved = expression;
  for (const auto &[name, val] : numeric_context) {
    std::string placeholder = "{{" + name + "}}";
    size_t pos = 0;
    while ((pos = resolved.find(placeholder, pos)) != std::string::npos) {
      resolved.replace(pos, placeholder.length(), std::to_string(val));
      pos += std::to_string(val).length();
    }
  }
  // 2. Evaluate math
  return EvaluateMath(resolved);
}

std::string TemplateManager::EvaluateString(
    const std::string &expression,
    const std::map<std::string, std::string> &string_context) {
  return ResolveVariables(expression, string_context);
}

glm::vec4 TemplateManager::EvaluateColor(
    const std::string &expression,
    const std::map<std::string, std::string> &string_context) {
  std::string resolved = ResolveVariables(expression, string_context);
  glm::vec4 color(1.0f);
  std::stringstream ss(resolved);
  char comma;
  if (resolved.find(',') != std::string::npos) {
    ss >> color.r >> comma >> color.g >> comma >> color.b >> comma >> color.a;
  } else {
    ss >> color.r >> color.g >> color.b >> color.a;
  }
  return color;
}

// --- Instantiation Core ---

bool TemplateManager::Instantiate(
    std::vector<Clip> &active_project_clips,
    const std::vector<Clip> &template_clips,
    const std::map<std::string, std::string> &context_string_values,
    float insert_time) {
  // 0. Protect existing linked_clip pointers against vector reallocation
  std::map<int, int> existing_links;
  for (const auto &c : active_project_clips) {
    if (c.linked_clip) {
      existing_links[c.uid] = c.linked_clip->uid;
    }
  }

  // 1. Prepare numeric context from string values asking for strict conversions
  std::map<std::string, float> context_float_values;
  for (const auto &[name, str_val] : context_string_values) {
    try {
      context_float_values[name] = std::stof(str_val);
    } catch (...) {
      context_float_values[name] = 0.0f;
    }
  }

  std::map<const Clip*, int> old_ptr_to_new_uid; // Map original template clip pointers to their new UIDs

  // 2. Deep Copy and Process Clips
  for (const auto &t_clip : template_clips) {
    Clip new_clip = t_clip; // Copy struct
    // Note: effect_graph is a shared_ptr. We must DEEP COPY it.
    if (t_clip.effect_graph) {
      new_clip.effect_graph = t_clip.effect_graph->Clone();
    }

    // 3. Apply Bindings
    // Generate the TargetUID prefix for this clip in the template
    // "Clip:UID"
    std::string clip_uid_prefix = "Clip:" + std::to_string(t_clip.uid);

    // Check property bindings
    // A. Clip Properties
    std::string bind_key = clip_uid_prefix + ".start_time";
    if (bindings.count(bind_key)) {
      new_clip.start_time =
          EvaluateFloat(bindings[bind_key], context_float_values);
    } else {
      // Default behavior: Shift by insert time if NOT bound?
      // Actually, usually we want RELATIVE time preserved + OFFSET.
      // If bound, we assume the formula handles logic.
      // If NOT bound, we just shift.
      new_clip.start_time += insert_time;
    }

    bind_key = clip_uid_prefix + ".path";
    if (bindings.count(bind_key)) {
      new_clip.path = EvaluateString(bindings[bind_key], context_string_values);
      // If path changed, we might want to reload duration/metadata?
      // This is complex. For valid MVP, user sets it.
    }

    bind_key = clip_uid_prefix + ".duration";
    if (bindings.count(bind_key)) {
      new_clip.duration =
          EvaluateFloat(bindings[bind_key], context_float_values);
    }

    // B. Effect Properties
    if (new_clip.effect_graph) {
      for (auto &[node_id, node] : new_clip.effect_graph->nodes) {
        std::string node_prefix =
            clip_uid_prefix + ".Effect:" + std::to_string(node_id);

        // Example: Blur Amount
        if (auto n = std::dynamic_pointer_cast<GaussianBlurNode>(node)) {
          std::string key = node_prefix + ".blur_amount";
          if (bindings.count(key))
            n->blur_amount = EvaluateFloat(bindings[key], context_float_values);
        }
        if (auto n = std::dynamic_pointer_cast<TextEffectNode>(node)) {
          std::string key = node_prefix + ".text_content";
          if (bindings.count(key))
            n->text_content =
                EvaluateString(bindings[key], context_string_values);
          key = node_prefix + ".font_size";
          if (bindings.count(key))
            n->font_size = EvaluateFloat(bindings[key], context_float_values);
          key = node_prefix + ".text_color";
          if (bindings.count(key))
            n->text_color = EvaluateColor(bindings[key], context_string_values);
        }
        if (auto n = std::dynamic_pointer_cast<SolidColorEffectNode>(node)) {
          std::string key = node_prefix + ".color";
          if (bindings.count(key))
            n->color = EvaluateColor(bindings[key], context_string_values);
        }
        if (auto n = std::dynamic_pointer_cast<DropShadowEffectNode>(node)) {
          std::string key = node_prefix + ".shadow_color";
          if (bindings.count(key))
            n->shadow_color =
                EvaluateColor(bindings[key], context_string_values);
        }
        if (auto n = std::dynamic_pointer_cast<ChromaKeyNode>(node)) {
          std::string key = node_prefix + ".key_color";
          if (bindings.count(key)) {
            glm::vec4 col = EvaluateColor(bindings[key], context_string_values);
            n->key_color = glm::vec3(col.r, col.g, col.b);
          }
        }
        // ... Add other node types ...
      }
    }

    // 4. Generate NEW UID for the active instance to avoid conflicts
    new_clip.uid = GetNextClipUID();

    // Important: map the old template clip pointer to the new instance's UID
    old_ptr_to_new_uid[&t_clip] = new_clip.uid;

    // Important: null out the linked clip for now, we will rebuild it.
    new_clip.linked_clip = nullptr;

    active_project_clips.push_back(new_clip);
  }

  // Now populate new_template_links based on how the original template clips
  // were connected
  for (const auto &t_clip : template_clips) {
    if (t_clip.linked_clip) {
      // Find the new UID for the clip that t_clip points to
      auto target_it = old_ptr_to_new_uid.find(t_clip.linked_clip);
      if (target_it != old_ptr_to_new_uid.end()) {
        int source_uid = old_ptr_to_new_uid[&t_clip];
        int target_uid = target_it->second;
        existing_links[source_uid] =
            target_uid; // Merge into existing_links map
      }
    }
  }

  // Re-link ALL clips in active project based on UIDs since vector
  // reallocations invalidate object pointers
  for (auto &c : active_project_clips) {
    if (existing_links.count(c.uid)) {
      int target_uid = existing_links[c.uid];
      for (auto &target : active_project_clips) {
        if (target.uid == target_uid) {
          c.linked_clip = &target;
          break;
        }
      }
    }
  }

  return true;
}

void TemplateManager::SaveToJSON(nlohmann::json &j) {
  j["variables"] = nlohmann::json::array();
  for (const auto &var : variables) {
    j["variables"].push_back({{"name", var.name},
                              {"type", static_cast<int>(var.type)},
                              {"default", var.default_value},
                              {"min", var.min_val},
                              {"max", var.max_val}});
  }

  j["bindings"] = bindings; // std::map serializes seamlessly
}

void TemplateManager::LoadFromJSON(const nlohmann::json &j) {
  Clear();
  if (j.contains("variables") && j["variables"].is_array()) {
    for (const auto &v : j["variables"]) {
      TemplateVariable var;
      var.name = v.value("name", "Untitled");
      var.type = static_cast<VariableType>(v.value("type", 0));
      var.default_value = v.value("default", "");
      var.min_val = v.value("min", -1000000.0f);
      var.max_val = v.value("max", 1000000.0f);
      variables.push_back(std::move(var));
    }
  }

  if (j.contains("bindings")) {
    bindings = j["bindings"].get<std::map<std::string, std::string>>();
  }
}

void TemplateManager::Clear() {
  variables.clear();
  bindings.clear();
}
