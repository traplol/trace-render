#pragma once
#include "model/trace_model.h"
#include "ui/view_state.h"
#include <string>

struct SDL_Window;

class Toolbar {
public:
    void render(const TraceModel& model, ViewState& view);
    void set_window(SDL_Window* window) { window_ = window; }
    void set_rss_mb(float mb) { rss_mb_ = mb; }

    bool settings_requested() const { return settings_requested_; }
    void clear_settings_request() { settings_requested_ = false; }

private:
    SDL_Window* window_ = nullptr;
    bool settings_requested_ = false;
    float rss_mb_ = 0.0f;
};
