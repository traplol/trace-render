#pragma once
#include "imgui.h"
#include <nlohmann/json_fwd.hpp>
#include <string>

enum class Action : int {
    PanLeft,
    PanRight,
    ScrollUp,
    ScrollDown,
    ZoomIn,
    ZoomOut,
    FitSelection,
    ClearSelection,
    GoToTime,
    OpenFile,
    Search,
    OpenSettings,
    RunQuery,
    Count
};

class KeyBindings {
public:
    KeyBindings();

    bool is_pressed(Action action) const;

    ImGuiKeyChord primary(Action action) const { return bindings_[static_cast<int>(action)].primary; }
    ImGuiKeyChord alt(Action action) const { return bindings_[static_cast<int>(action)].alt; }

    void reset_defaults();
    void render_settings();

    nlohmann::json save() const;
    void load(const nlohmann::json& j);

private:
    static constexpr int kCount = static_cast<int>(Action::Count);

    struct Binding {
        ImGuiKeyChord primary = ImGuiKey_None;
        ImGuiKeyChord alt = ImGuiKey_None;
    };

    Binding bindings_[kCount] = {};

    // UI state for rebinding
    int editing_action_ = -1;
    int editing_slot_ = -1;  // 0=primary, 1=alt

    void clear_conflict(int action_idx, int slot, ImGuiKeyChord chord);

    static const char* action_name(Action action);
    static const char* action_id(Action action);
    static std::string key_chord_name(ImGuiKeyChord chord);
    static bool is_modifier_key(ImGuiKey key);
};
