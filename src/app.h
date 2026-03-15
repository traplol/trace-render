#pragma once
#include "model/trace_model.h"
#include "parser/trace_parser.h"
#include "platform/file_loader.h"
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
#include <vector>

struct SDL_Window;

class App {
public:
    void init(SDL_Window* window);
    void update();
    void shutdown();
    void open_file(const std::string& path);
    void open_buffer(std::vector<char> data, const std::string& filename);
    void set_time_unit_ns(bool ns) { view_.set_time_unit_ns(ns); }

    bool has_trace() const { return has_trace_; }

private:
    TraceModel model_;
    ViewState view_;
    FileLoader loader_;

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
    int settings_tab_ = 0;
    bool dark_theme_ = true;
    bool vsync_ = true;
    SDL_Window* window_ = nullptr;

    void finish_load();
    void render_loading_overlay();
    void render_settings_modal();
    void load_settings();
    void save_settings();
};
