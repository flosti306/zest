#pragma once
#include <string>
#include <map>
#include <vector>
#include <SDL3/SDL.h>
#include <nlohmann/json.hpp>
#include <imgui.h>

struct Keybind {
    SDL_Keycode key;
    SDL_Keymod mod; // Bitmask of modifiers (KMOD_CTRL, KMOD_SHIFT, etc.)
    std::string description;
};

class KeybindManager {
public:
    // Registers a default keybind. If it already exists (loaded from file), the loaded value is kept.
    void Register(const std::string& action_name, SDL_Keycode default_key, SDL_Keymod default_mod, const std::string& description);

    // Checks if the given SDL_Event matches `action_name`.
    bool IsTriggered(const SDL_Event& event, const std::string& action_name);

    // Saves current bindings to a JSON file.
    void Save(const std::string& filename);

    // Loads bindings from a JSON file.
    void Load(const std::string& filename);

    // Draws the ImGui settings window.
    void DrawSettingsWindow(bool* open);

    // Handles input for rebinding. Returns true if key was captured/consumed.
    bool HandleRebindInput(const SDL_Event& event);

    // Checks if we are currently waiting for a key press
    bool IsRebindingActive() const { return m_rebinding_mode; }

    // Resets all bindings to their registered default values.
    void ResetToDefaults();

private:
    std::map<std::string, Keybind> m_bindings;
    std::map<std::string, Keybind> m_default_bindings;
    
    // For the UI, to detect key presses when rebinding
    bool m_rebinding_mode = false;
    std::string m_rebinding_action = "";
};
