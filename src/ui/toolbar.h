#pragma once
#include "model/trace_model.h"
#include "ui/view_state.h"
#include <string>

struct SDL_Window;

class Toolbar {
public:
    void render(const TraceModel& model, ViewState& view, float rss_mb, bool can_save = false);
    void set_window(SDL_Window* window) { window_ = window; }

    bool settings_requested() const { return settings_requested_; }
    void clear_settings_request() { settings_requested_ = false; }
    bool save_profile_requested() const { return save_profile_requested_; }
    void clear_save_profile_request() { save_profile_requested_ = false; }

private:
    SDL_Window* window_ = nullptr;
    bool settings_requested_ = false;
    bool save_profile_requested_ = false;
};
