#pragma once
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <variant>
#include <optional>
#include <nlohmann/json.hpp>
#include <glm/glm.hpp>

// Forward declarations
struct Clip;
struct EffectNode;
class EffectGraph;

enum class VariableType {
    Float,
    String,
    FilePath,
    Color // glm::vec4
};

struct TemplateVariable {
    std::string name;
    VariableType type;
    std::string default_value; // Stored as string, parsed on use
    float min_val = -1000000.0f;
    float max_val = 1000000.0f;
};

// Represents a binding between a specific property and an expression
struct TemplateBinding {
    std::string target_uid; // "ClipUID.Property" or "ClipUID.EffectUID.Property"
    std::string expression;
};

class TemplateManager {
public:
    static TemplateManager& Get() {
        static TemplateManager instance;
        return instance;
    }

    // --- Configuration ---
    void RegisterVariable(const std::string& name, VariableType type, const std::string& default_val);
    void RemoveVariable(const std::string& name);
    std::vector<TemplateVariable>& GetVariables() { return variables; }
    
    // bindings map: Key = TargetUID, Value = Expression
    std::map<std::string, std::string>& GetBindings() { return bindings; }
    void SetBinding(const std::string& target_uid, const std::string& expression);
    void RemoveBinding(const std::string& target_uid);
    std::optional<std::string> GetBinding(const std::string& target_uid);

    // --- Instantiation ---
    // Instantiates a template project into the current project
    // context_values: Map of VariableName -> ValueString
    bool Instantiate(
        std::vector<Clip>& active_project_clips, 
        const std::vector<Clip>& template_clips, 
        const std::map<std::string, std::string>& context_values,
        float insert_time
    );

    // --- Expression Evaluation ---
    // Public for testing/UI validation
    float EvaluateFloat(const std::string& expression, const std::map<std::string, float>& numeric_context);
    std::string EvaluateString(const std::string& expression, const std::map<std::string, std::string>& string_context);
    glm::vec4 EvaluateColor(const std::string& expression, const std::map<std::string, std::string>& string_context);

    // --- Serialization ---
    void SaveToJSON(nlohmann::json& j);
    void LoadFromJSON(const nlohmann::json& j);
    void Clear(); // Reset all variables and bindings

private:
    TemplateManager() = default;
    
    std::vector<TemplateVariable> variables;
    std::map<std::string, std::string> bindings;

    // Helper to generate a unique ID string for a clip (e.g. for binding targets)
    // Note: In a real system, we'd need permanent UIDs. For now, we might rely on 
    // pointer-stability during editing or temporary IDs assigned during load.
    // The implementation plan suggested adding IDs to Clips.
};
