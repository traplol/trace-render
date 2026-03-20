#include "ui/key_bindings.h"
#include "tracing.h"
#include <nlohmann/json.hpp>

KeyBindings::KeyBindings() {
    reset_defaults();
}

void KeyBindings::reset_defaults() {
    TRACE_FUNCTION_CAT("ui");
    bindings_[static_cast<int>(Action::PanLeft)] = {ImGuiKey_LeftArrow, ImGuiKey_None};
    bindings_[static_cast<int>(Action::PanRight)] = {ImGuiKey_RightArrow, ImGuiKey_None};
    bindings_[static_cast<int>(Action::ScrollUp)] = {ImGuiKey_UpArrow, ImGuiKey_None};
    bindings_[static_cast<int>(Action::ScrollDown)] = {ImGuiKey_DownArrow, ImGuiKey_None};
    bindings_[static_cast<int>(Action::ZoomIn)] = {ImGuiKey_Equal, ImGuiKey_None};
    bindings_[static_cast<int>(Action::ZoomOut)] = {ImGuiKey_Minus, ImGuiKey_None};
    bindings_[static_cast<int>(Action::FitSelection)] = {ImGuiKey_F, ImGuiKey_None};
    bindings_[static_cast<int>(Action::ClearSelection)] = {ImGuiKey_Escape, ImGuiKey_None};
    bindings_[static_cast<int>(Action::GoToTime)] = {ImGuiKey_G, ImGuiKey_None};
    bindings_[static_cast<int>(Action::OpenFile)] = {ImGuiMod_Ctrl | ImGuiKey_O, ImGuiKey_None};
    bindings_[static_cast<int>(Action::Search)] = {ImGuiMod_Ctrl | ImGuiKey_F, ImGuiKey_None};
    bindings_[static_cast<int>(Action::OpenSettings)] = {ImGuiMod_Ctrl | ImGuiKey_Comma, ImGuiKey_None};
    bindings_[static_cast<int>(Action::RunQuery)] = {ImGuiMod_Ctrl | ImGuiKey_Enter, ImGuiKey_None};
    bindings_[static_cast<int>(Action::NavParent)] = {ImGuiKey_W, ImGuiKey_None};
    bindings_[static_cast<int>(Action::NavChild)] = {ImGuiKey_S, ImGuiKey_None};
    bindings_[static_cast<int>(Action::NavPrevSibling)] = {ImGuiKey_A, ImGuiKey_None};
    bindings_[static_cast<int>(Action::NavNextSibling)] = {ImGuiKey_D, ImGuiKey_None};
}

bool KeyBindings::is_pressed(Action action) const {
    TRACE_FUNCTION_CAT("ui");
    int idx = static_cast<int>(action);
    const auto& b = bindings_[idx];
    if (b.primary != ImGuiKey_None && ImGui::IsKeyChordPressed(b.primary)) return true;
    if (b.alt != ImGuiKey_None && ImGui::IsKeyChordPressed(b.alt)) return true;
    return false;
}

const char* KeyBindings::action_name(Action action) {
    TRACE_FUNCTION_CAT("ui");
    switch (action) {
        case Action::PanLeft:
            return "Pan Left";
        case Action::PanRight:
            return "Pan Right";
        case Action::ScrollUp:
            return "Scroll Up";
        case Action::ScrollDown:
            return "Scroll Down";
        case Action::ZoomIn:
            return "Zoom In";
        case Action::ZoomOut:
            return "Zoom Out";
        case Action::FitSelection:
            return "Fit Selection";
        case Action::ClearSelection:
            return "Clear Selection";
        case Action::GoToTime:
            return "Go to Time";
        case Action::OpenFile:
            return "Open File";
        case Action::Search:
            return "Search";
        case Action::OpenSettings:
            return "Settings";
        case Action::RunQuery:
            return "Run Query";
        case Action::NavParent:
            return "Nav Parent";
        case Action::NavChild:
            return "Nav Child";
        case Action::NavPrevSibling:
            return "Nav Prev Sibling";
        case Action::NavNextSibling:
            return "Nav Next Sibling";
        default:
            return "Unknown";
    }
}

std::string KeyBindings::key_chord_name(ImGuiKeyChord chord) {
    TRACE_FUNCTION_CAT("ui");
    if (chord == ImGuiKey_None) return "---";
    std::string result;
    if (chord & ImGuiMod_Ctrl) result += "Ctrl+";
    if (chord & ImGuiMod_Shift) result += "Shift+";
    if (chord & ImGuiMod_Alt) result += "Alt+";
    ImGuiKey key = static_cast<ImGuiKey>(chord & ~ImGuiMod_Mask_);
    result += ImGui::GetKeyName(key);
    return result;
}

const char* KeyBindings::action_id(Action action) {
    TRACE_FUNCTION_CAT("ui");
    switch (action) {
        case Action::PanLeft:
            return "pan_left";
        case Action::PanRight:
            return "pan_right";
        case Action::ScrollUp:
            return "scroll_up";
        case Action::ScrollDown:
            return "scroll_down";
        case Action::ZoomIn:
            return "zoom_in";
        case Action::ZoomOut:
            return "zoom_out";
        case Action::FitSelection:
            return "fit_selection";
        case Action::ClearSelection:
            return "clear_selection";
        case Action::GoToTime:
            return "go_to_time";
        case Action::OpenFile:
            return "open_file";
        case Action::Search:
            return "search";
        case Action::OpenSettings:
            return "open_settings";
        case Action::RunQuery:
            return "run_query";
        case Action::NavParent:
            return "nav_parent";
        case Action::NavChild:
            return "nav_child";
        case Action::NavPrevSibling:
            return "nav_prev_sibling";
        case Action::NavNextSibling:
            return "nav_next_sibling";
        default:
            return "unknown";
    }
}

bool KeyBindings::is_modifier_key(ImGuiKey key) {
    TRACE_FUNCTION_CAT("ui");
    return key == ImGuiKey_LeftCtrl || key == ImGuiKey_RightCtrl || key == ImGuiKey_LeftShift ||
           key == ImGuiKey_RightShift || key == ImGuiKey_LeftAlt || key == ImGuiKey_RightAlt ||
           key == ImGuiKey_LeftSuper || key == ImGuiKey_RightSuper || key == ImGuiKey_ReservedForModCtrl ||
           key == ImGuiKey_ReservedForModShift || key == ImGuiKey_ReservedForModAlt ||
           key == ImGuiKey_ReservedForModSuper;
}

void KeyBindings::clear_conflict(int action_idx, int slot, ImGuiKeyChord chord) {
    TRACE_FUNCTION_CAT("ui");
    if (chord == ImGuiKey_None) return;
    for (int i = 0; i < kCount; i++) {
        if (i == action_idx) continue;
        if (bindings_[i].primary == chord) bindings_[i].primary = ImGuiKey_None;
        if (bindings_[i].alt == chord) bindings_[i].alt = ImGuiKey_None;
    }
    // Also clear the other slot of the same action if it matches
    if (slot == 0 && bindings_[action_idx].alt == chord) bindings_[action_idx].alt = ImGuiKey_None;
    if (slot == 1 && bindings_[action_idx].primary == chord) bindings_[action_idx].primary = ImGuiKey_None;
}

void KeyBindings::render_settings() {
    TRACE_FUNCTION_CAT("ui");
    ImGui::TextWrapped("Click a binding to change it. Press Escape to cancel, Delete to clear.");
    ImGui::Spacing();

    if (ImGui::BeginTable("keybindings", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
        ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, 160);
        ImGui::TableSetupColumn("Primary", ImGuiTableColumnFlags_WidthFixed, 200);
        ImGui::TableSetupColumn("Alternative", ImGuiTableColumnFlags_WidthFixed, 200);
        ImGui::TableHeadersRow();

        const auto& io = ImGui::GetIO();

        for (int i = 0; i < kCount; i++) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%s", action_name(static_cast<Action>(i)));

            for (int slot = 0; slot < 2; slot++) {
                ImGui::TableNextColumn();
                ImGuiKeyChord key = slot == 0 ? bindings_[i].primary : bindings_[i].alt;
                bool is_editing = (editing_action_ == i && editing_slot_ == slot);

                ImGui::PushID(i * 2 + slot);
                if (is_editing) {
                    ImGui::Button("Press a key...", ImVec2(-1, 0));

                    // Read raw key state to bypass ImGui navigation/routing
                    for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; k++) {
                        if (is_modifier_key(static_cast<ImGuiKey>(k))) continue;
                        const auto& kd = io.KeysData[k - ImGuiKey_NamedKey_BEGIN];
                        if (kd.DownDuration != 0.0f) continue;  // only trigger on first frame of press

                        if (k == ImGuiKey_Escape) {
                            editing_action_ = -1;
                        } else if (k == ImGuiKey_Delete || k == ImGuiKey_Backspace) {
                            if (slot == 0)
                                bindings_[i].primary = ImGuiKey_None;
                            else
                                bindings_[i].alt = ImGuiKey_None;
                            editing_action_ = -1;
                        } else {
                            ImGuiKeyChord chord = static_cast<ImGuiKey>(k);
                            if (io.KeyCtrl) chord |= ImGuiMod_Ctrl;
                            if (io.KeyShift) chord |= ImGuiMod_Shift;
                            if (io.KeyAlt) chord |= ImGuiMod_Alt;

                            clear_conflict(i, slot, chord);
                            if (slot == 0)
                                bindings_[i].primary = chord;
                            else
                                bindings_[i].alt = chord;
                            editing_action_ = -1;
                        }
                        break;
                    }
                } else {
                    std::string label = key_chord_name(key);
                    if (ImGui::Button(label.c_str(), ImVec2(-1, 0))) {
                        editing_action_ = i;
                        editing_slot_ = slot;
                    }
                }
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }
}

nlohmann::json KeyBindings::save() const {
    TRACE_FUNCTION_CAT("ui");
    nlohmann::json j = nlohmann::json::object();
    for (int i = 0; i < kCount; i++) {
        auto action = static_cast<Action>(i);
        j[action_id(action)] = {{"primary", static_cast<int>(bindings_[i].primary)},
                                {"alt", static_cast<int>(bindings_[i].alt)}};
    }
    return j;
}

void KeyBindings::load(const nlohmann::json& j) {
    TRACE_FUNCTION_CAT("ui");
    if (!j.is_object()) return;
    for (int i = 0; i < kCount; i++) {
        auto action = static_cast<Action>(i);
        const char* id = action_id(action);
        if (!j.contains(id)) continue;
        const auto& entry = j[id];
        if (entry.contains("primary")) bindings_[i].primary = static_cast<ImGuiKeyChord>(entry["primary"].get<int>());
        if (entry.contains("alt")) bindings_[i].alt = static_cast<ImGuiKeyChord>(entry["alt"].get<int>());
    }
}
