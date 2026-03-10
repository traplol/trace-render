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
#include "model/query_db.h"
#include <string>

struct SDL_Window;

class App {
public:
    void init(SDL_Window* window);
    void update();
    void shutdown();
    void open_file(const std::string& path);
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
    QueryDb query_db_;

    bool has_trace_ = false;
    bool loading_ = false;
    float load_progress_ = 0.0f;
    std::string status_message_;
    bool first_layout_ = true;
    bool show_settings_ = false;
    bool dark_theme_ = true;

    void render_settings_modal();
    void load_settings();
    void save_settings();
    std::string settings_path() const;
};
