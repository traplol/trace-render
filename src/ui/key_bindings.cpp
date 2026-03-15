#include "ui/key_bindings.h"
#include <nlohmann/json.hpp>

KeyBindings::KeyBindings() {
    reset_defaults();
}

void KeyBindings::reset_defaults() {
    bindings_[static_cast<int>(Action::PanLeft)] = {ImGuiKey_A, ImGuiKey_LeftArrow};
    bindings_[static_cast<int>(Action::PanRight)] = {ImGuiKey_D, ImGuiKey_RightArrow};
    bindings_[static_cast<int>(Action::ScrollUp)] = {ImGuiKey_UpArrow, ImGuiKey_None};
    bindings_[static_cast<int>(Action::ScrollDown)] = {ImGuiKey_DownArrow, ImGuiKey_None};
    bindings_[static_cast<int>(Action::ZoomIn)] = {ImGuiKey_W, ImGuiKey_None};
    bindings_[static_cast<int>(Action::ZoomOut)] = {ImGuiKey_S, ImGuiKey_None};
    bindings_[static_cast<int>(Action::FitSelection)] = {ImGuiKey_F, ImGuiKey_None};
    bindings_[static_cast<int>(Action::ClearSelection)] = {ImGuiKey_Escape, ImGuiKey_None};
    bindings_[static_cast<int>(Action::GoToTime)] = {ImGuiKey_G, ImGuiKey_None};
    bindings_[static_cast<int>(Action::OpenFile)] = {ImGuiMod_Ctrl | ImGuiKey_O, ImGuiKey_None};
    bindings_[static_cast<int>(Action::Search)] = {ImGuiMod_Ctrl | ImGuiKey_F, ImGuiKey_None};
    bindings_[static_cast<int>(Action::OpenSettings)] = {ImGuiMod_Ctrl | ImGuiKey_Comma, ImGuiKey_None};
    bindings_[static_cast<int>(Action::RunQuery)] = {ImGuiMod_Ctrl | ImGuiKey_Enter, ImGuiKey_None};
}

bool KeyBindings::is_pressed(Action action) const {
    int idx = static_cast<int>(action);
    const auto& b = bindings_[idx];
    if (b.primary != ImGuiKey_None && ImGui::IsKeyChordPressed(b.primary)) return true;
    if (b.alt != ImGuiKey_None && ImGui::IsKeyChordPressed(b.alt)) return true;
    return false;
}

const char* KeyBindings::action_name(Action action) {
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
        default:
            return "Unknown";
    }
}

std::string KeyBindings::key_chord_name(ImGuiKeyChord chord) {
    if (chord == ImGuiKey_None) return "---";
    std::string result;
    if (chord & ImGuiMod_Ctrl) result += "Ctrl+";
    if (chord & ImGuiMod_Shift) result += "Shift+";
    if (chord & ImGuiMod_Alt) result += "Alt+";
    ImGuiKey key = static_cast<ImGuiKey>(chord & ~ImGuiMod_Mask_);
    result += ImGui::GetKeyName(key);
    return result;
}

bool KeyBindings::is_modifier_key(ImGuiKey key) {
    return key == ImGuiKey_LeftCtrl || key == ImGuiKey_RightCtrl || key == ImGuiKey_LeftShift ||
           key == ImGuiKey_RightShift || key == ImGuiKey_LeftAlt || key == ImGuiKey_RightAlt ||
           key == ImGuiKey_LeftSuper || key == ImGuiKey_RightSuper;
}

void KeyBindings::render_settings() {
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
    nlohmann::json j = nlohmann::json::array();
    for (int i = 0; i < kCount; i++) {
        j.push_back({{"primary", static_cast<int>(bindings_[i].primary)}, {"alt", static_cast<int>(bindings_[i].alt)}});
    }
    return j;
}

void KeyBindings::load(const nlohmann::json& j) {
    if (!j.is_array()) return;
    int count = std::min(static_cast<int>(j.size()), kCount);
    for (int i = 0; i < count; i++) {
        if (j[i].contains("primary")) bindings_[i].primary = static_cast<ImGuiKeyChord>(j[i]["primary"].get<int>());
        if (j[i].contains("alt")) bindings_[i].alt = static_cast<ImGuiKeyChord>(j[i]["alt"].get<int>());
    }
}
