#pragma once
#include "model/trace_model.h"
#include "ui/view_state.h"
#include <string>

struct SDL_Window;

class Toolbar {
public:
    void render(const TraceModel& model, ViewState& view);
    void set_window(SDL_Window* window) { window_ = window; }

    bool file_open_requested() const { return file_open_requested_; }
    const std::string& file_path() const { return file_path_; }
    void clear_request() { file_open_requested_ = false; }

    bool settings_requested() const { return settings_requested_; }
    void clear_settings_request() { settings_requested_ = false; }

    // Called by SDL dialog callback
    void on_file_selected(const char* path);

private:
    SDL_Window* window_ = nullptr;
    char path_buf_[4096] = {};
    bool file_open_requested_ = false;
    std::string file_path_;
    bool show_fallback_dialog_ = false;
    bool settings_requested_ = false;
};
