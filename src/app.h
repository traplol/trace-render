#pragma once
#include "model/trace_model.h"
#include "parser/trace_parser.h"
#include "ui/view_state.h"
#include "ui/toolbar.h"
#include "ui/timeline_view.h"
#include "ui/detail_panel.h"
#include "ui/search_panel.h"
#include "ui/filter_panel.h"
#include "ui/counter_track.h"
#include "ui/flow_renderer.h"
#include "ui/stats_panel.h"
#include "ui/instance_panel.h"
#include "ui/diagnostics_panel.h"
#include "ui/source_panel.h"
#include "model/query_db.h"
#include <string>
#ifndef __EMSCRIPTEN__
#include <thread>
#include <atomic>
#include <mutex>
#endif

struct SDL_Window;

class App {
public:
    void init(SDL_Window* window);
    void update();
    void shutdown();
    void open_file(const std::string& path);
    void open_buffer(const char* data, size_t size, const std::string& filename);
    void set_time_unit_ns(bool ns) { view_.time_unit_ns = ns; }

    bool has_trace() const { return has_trace_; }

private:
    TraceModel model_;
    ViewState view_;
    TraceParser parser_;

    Toolbar toolbar_;
    TimelineView timeline_;
    DetailPanel detail_;
    SearchPanel search_;
    FilterPanel filter_;
    StatsPanel stats_;
    InstancePanel instances_;
    DiagnosticsPanel diagnostics_;
    SourcePanel source_;
    QueryDb query_db_;

    bool has_trace_ = false;
    std::string status_message_;
    bool first_layout_ = true;
    bool show_settings_ = false;
    bool dark_theme_ = true;
    bool vsync_ = true;
    SDL_Window* window_ = nullptr;

    // Background loading
#ifdef __EMSCRIPTEN__
    bool loading_ = false;
    float load_progress_ = 0.0f;
    float load_phase_progress_ = 0.0f;
    bool load_finished_ = false;
#else
    std::atomic<bool> loading_{false};
    std::atomic<float> load_progress_{0.0f};        // global progress 0-1
    std::atomic<float> load_phase_progress_{0.0f};  // current phase progress 0-1
    std::atomic<bool> load_finished_{false};
    std::thread load_thread_;
    std::mutex load_mutex_;
    std::mutex phase_mutex_;
#endif
    bool load_success_ = false;
    std::string load_error_;
    std::string loading_filename_;
    std::string loading_phase_;

    void finish_load();
    void render_loading_overlay();
    void render_settings_modal();
    void load_settings();
    void save_settings();
    std::string settings_path() const;
};
