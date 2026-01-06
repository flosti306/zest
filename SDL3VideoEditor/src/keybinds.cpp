#include "keybinds.hpp"
#include <fstream>
#include <iostream>

using json = nlohmann::json;

// --- Helper Functions for Serialization ---

// Convert SDL_Keycode to string name
std::string KeyToString(SDL_Keycode key) {
    const char* name = SDL_GetKeyName(key);
    return name ? std::string(name) : "Unknown";
}

// Convert string name to SDL_Keycode
SDL_Keycode StringToKey(const std::string& name) {
    return SDL_GetKeyFromName(name.c_str());
}

// Serialize Keybind
void to_json(json& j, const Keybind& k) {
    j = json{
        {"key", KeyToString(k.key)},
        {"mod", static_cast<int>(k.mod)},
        {"description", k.description}
    };
}

// Deserialize Keybind
void from_json(const json& j, Keybind& k) {
    std::string key_str = j.at("key").get<std::string>();
    k.key = StringToKey(key_str);
    k.mod = static_cast<SDL_Keymod>(j.at("mod").get<int>());
    k.description = j.at("description").get<std::string>();
}

// --- KeybindManager Implementation ---

void KeybindManager::Register(const std::string& action_name, SDL_Keycode default_key, SDL_Keymod default_mod, const std::string& description) {
    // Always store default
    m_default_bindings[action_name] = { default_key, default_mod, description };

    // Only insert if not already present (so we don't overwrite loaded/user bindings)
    if (m_bindings.find(action_name) == m_bindings.end()) {
        m_bindings[action_name] = { default_key, default_mod, description };
    } else {
        // If it exists (e.g. loaded from file), update description just in case dev changed it
        m_bindings[action_name].description = description; 
    }
}

void KeybindManager::ResetToDefaults() {
    m_bindings = m_default_bindings;
    Save("keybinds.json");
}

bool ModifiersMatch(SDL_Keymod current, SDL_Keymod required) {
    // Helper to check for generic modifiers
    auto CheckMod = [&](SDL_Keymod mask) -> bool {
        bool has_req = (required & mask) != 0;
        bool has_curr = (current & mask) != 0;
        return has_req == has_curr; // Must match presence
    };
    
    // Check main modifiers
    if (!CheckMod(SDL_KMOD_CTRL)) return false;
    if (!CheckMod(SDL_KMOD_SHIFT)) return false;
    if (!CheckMod(SDL_KMOD_ALT)) return false;
    if (!CheckMod(SDL_KMOD_GUI)) return false;
    
    return true;
}

bool KeybindManager::IsTriggered(const SDL_Event& event, const std::string& action_name) {
    auto it = m_bindings.find(action_name);
    if (it == m_bindings.end()) return false;

    const Keybind& bind = it->second;

    if (event.type == SDL_EVENT_KEY_DOWN) {
        if (event.key.key == bind.key) {
            // Check modifiers
            // If bind.mod is 0 (None), we generally allow it even if other mods are pressed? 
            // Or should we be strict?
            // "Space" for Play usually works even if Shift is held? Maybe not.
            // Let's implement Strict Check for now: (CurrentState & ModToCheck) == ModToCheck
            
            // Actually, we want to match EXACTLY if the user bound explicit mods.
            // But if user bound "Delete" (Mod 0), generally "Shift+Delete" should NOT trigger "Delete" if strict.
            // However, typical behavior:
            // "Ctrl+Z" -> Mod must include Ctrl. 
            // SDL_GetModState() returns all active mods.
            
            SDL_Keymod current_mod = SDL_GetModState();
            
            // Allow loose matching for bare keys?
            // E.g. "Space" works even if Shift is held?
            // If the bind explicitly has KMOD_NONE, we usually want EXACT match (no mods).
            
            return ModifiersMatch(current_mod, bind.mod);
        }
    }
    return false;
}

void KeybindManager::Save(const std::string& filename) {
    json j;
    for (const auto& [name, bind] : m_bindings) {
        j[name] = bind;
    }
    std::ofstream o(filename);
    o << j.dump(4);
}

void KeybindManager::Load(const std::string& filename) {
    std::ifstream i(filename);
    if (!i.is_open()) return;
    
    json j;
    try {
        i >> j;
        for (auto& [name, element] : j.items()) {
            Keybind k = element.get<Keybind>();
            
            // If logic relies on loaded names matching defaults:
            // Valid to just load whatever is in file.
            m_bindings[name] = k; 
        }
    } catch (const std::exception& e) {
        std::cerr << "Error loading keybinds: " << e.what() << std::endl;
    }
}

bool KeybindManager::HandleRebindInput(const SDL_Event& event) {
    if (!m_rebinding_mode || m_rebinding_action.empty()) return false;

    if (event.type == SDL_EVENT_KEY_DOWN) {
        SDL_Keycode key = event.key.key;
        
        // --- FIX: Ignore pure modifier keys ---
        // We don't want to bind "Ctrl" itself when user tries to bind "Ctrl + B"
        if (key == SDLK_LCTRL || key == SDLK_RCTRL || 
            key == SDLK_LSHIFT || key == SDLK_RSHIFT ||
            key == SDLK_LALT || key == SDLK_RALT ||
            key == SDLK_LGUI || key == SDLK_RGUI ||
            key == SDLK_MODE || key == SDLK_CAPSLOCK) // Ignore locks too maybe
        {
            return true; // Consume it but don't bind
        }

        // Escape cancels
        if (key == SDLK_ESCAPE) {
            m_rebinding_mode = false;
            m_rebinding_action = "";
            return true;
        }

        // Apply binding
        // Capture modifiers from the event
        // Note: SDL_GetModState() gives current state. 
        // We usually want the modifiers pressed *with* this key.
        SDL_Keymod mod = SDL_GetModState();
        
        // Remove Lock state noise if present in mod?
        // standard mask for "real" modifiers:
        SDL_Keymod real_mods = (SDL_Keymod)(mod & (SDL_KMOD_CTRL | SDL_KMOD_SHIFT | SDL_KMOD_ALT | SDL_KMOD_GUI));
        
        m_bindings[m_rebinding_action].key = key;
        m_bindings[m_rebinding_action].mod = real_mods;
        
        m_rebinding_mode = false;
        m_rebinding_action = "";
        return true;
    }
    
    // Consume key up to prevent accidental triggering
    if (event.type == SDL_EVENT_KEY_UP || event.type == SDL_EVENT_TEXT_INPUT) return true;
    
    return false;
}

void KeybindManager::DrawSettingsWindow(bool* open) {
    if (!*open) return;

    ImGui::SetNextWindowSize(ImVec2(500, 400), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Key Bindings", open)) {
        
        ImGui::Text("Double-click a key to rebind.");
        ImGui::SameLine();
        float avail = ImGui::GetContentRegionAvail().x;
        ImGui::SameLine(avail - 60);
        if (ImGui::Button("Reset")) {
            ResetToDefaults();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Reset all bindings to defaults");

        ImGui::Separator();

        if (ImGui::BeginTable("KeybindsTable", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable)) {
            ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, 200.0f);
            ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();

            for (auto& [name, bind] : m_bindings) {
                ImGui::TableNextRow();
                
                ImGui::TableSetColumnIndex(0);
                ImGui::Text("%s", bind.description.empty() ? name.c_str() : bind.description.c_str());

                ImGui::TableSetColumnIndex(1);
                
                if (m_rebinding_mode && m_rebinding_action == name) {
                    ImGui::TextColored(ImVec4(1, 1, 0, 1), "Press key combo... (Esc to cancel)");
                } else {
                    std::string key_label = KeyToString(bind.key);
                    // Add Modifiers string
                    std::string mod_str = "";
                    if (bind.mod & SDL_KMOD_CTRL) mod_str += "Ctrl+";
                    if (bind.mod & SDL_KMOD_SHIFT) mod_str += "Shift+";
                    if (bind.mod & SDL_KMOD_ALT) mod_str += "Alt+";
                    if (bind.mod & SDL_KMOD_GUI) mod_str += "Cmd+";
                    
                    key_label = mod_str + key_label;
                    
                    if (ImGui::Selectable(key_label.c_str(), false, ImGuiSelectableFlags_SpanAllColumns)) {
                        m_rebinding_mode = true;
                        m_rebinding_action = name;
                    }
                }
            }
            ImGui::EndTable();
        }
        
        if (m_rebinding_mode) {
             ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "Rebinding active...");
        }

        ImGui::Separator();
        if (ImGui::Button("Save")) {
            Save("keybinds.json");
            *open = false;
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            Load("keybinds.json"); // Revert
            *open = false;
        }
    }
    ImGui::End();
}
