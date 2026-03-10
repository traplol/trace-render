#include "timeline_view.h"
#include "model/color_palette.h"
#include "imgui.h"
#include <cmath>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// Format a duration (e.g. for tooltips, detail panel)
static void format_time(double us, char* buf, size_t buf_size) {
    double abs_us = std::abs(us);
    if (abs_us < 0.001) {
        snprintf(buf, buf_size, "%.3f ns", us * 1000.0);
    } else if (abs_us < 1.0) {
        snprintf(buf, buf_size, "%.1f ns", us * 1000.0);
    } else if (abs_us < 1000.0) {
        snprintf(buf, buf_size, "%.3f us", us);
    } else if (abs_us < 1000000.0) {
        snprintf(buf, buf_size, "%.3f ms", us / 1000.0);
    } else {
        snprintf(buf, buf_size, "%.3f s", us / 1000000.0);
    }
}

// Format a timestamp for the ruler - uses tick_interval to determine precision
static void format_ruler_time(double us, double tick_interval, char* buf, size_t buf_size) {
    // Choose unit based on absolute value
    double abs_us = std::abs(us);

    if (tick_interval < 0.001) {
        // Sub-nanosecond ticks - show in ns with sub-ns precision
        snprintf(buf, buf_size, "%.3f ns", us * 1000.0);
    } else if (tick_interval < 1.0) {
        // Nanosecond ticks - show in us with enough decimals
        int decimals = 3;
        if (tick_interval < 0.01) decimals = 4;
        else if (tick_interval < 0.1) decimals = 3;
        if (abs_us >= 1000000.0)
            snprintf(buf, buf_size, "%.*f s", decimals + 6, us / 1000000.0);
        else if (abs_us >= 1000.0)
            snprintf(buf, buf_size, "%.*f ms", decimals + 3, us / 1000.0);
        else
            snprintf(buf, buf_size, "%.*f us", decimals, us);
    } else if (tick_interval < 1000.0) {
        // Microsecond ticks
        if (abs_us >= 1000000.0) {
            // Show as seconds with enough decimal places to distinguish ticks
            int decimals = 6;
            if (tick_interval >= 100.0) decimals = 4;
            else if (tick_interval >= 10.0) decimals = 5;
            snprintf(buf, buf_size, "%.*f s", decimals, us / 1000000.0);
        } else if (abs_us >= 1000.0) {
            int decimals = 3;
            if (tick_interval >= 100.0) decimals = 1;
            else if (tick_interval >= 10.0) decimals = 2;
            snprintf(buf, buf_size, "%.*f ms", decimals, us / 1000.0);
        } else {
            snprintf(buf, buf_size, "%.1f us", us);
        }
    } else if (tick_interval < 1000000.0) {
        // Millisecond ticks
        if (abs_us >= 1000000.0) {
            int decimals = 3;
            if (tick_interval >= 100000.0) decimals = 1;
            else if (tick_interval >= 10000.0) decimals = 2;
            snprintf(buf, buf_size, "%.*f s", decimals, us / 1000000.0);
        } else {
            snprintf(buf, buf_size, "%.1f ms", us / 1000.0);
        }
    } else {
        // Second ticks
        int decimals = 0;
        if (tick_interval < 10000000.0) decimals = 1;
        snprintf(buf, buf_size, "%.*f s", decimals, us / 1000000.0);
    }
}

void TimelineView::render_time_ruler(ImDrawList* dl, ImVec2 area_min, ImVec2 area_max,
                                     const ViewState& view) {
    float ruler_height = view.ruler_height;
    float width = area_max.x - area_min.x;
    double range = view.view_end_ts - view.view_start_ts;

    // Compute nice tick interval
    double pixels_per_us = width / range;
    double min_tick_pixels = 240.0;
    double min_tick_us = min_tick_pixels / pixels_per_us;

    // Round up to a "nice" interval: 1, 2, 5, 10, 20, 50, ...
    double magnitude = std::pow(10.0, std::floor(std::log10(min_tick_us)));
    double residual = min_tick_us / magnitude;
    double nice_tick;
    if (residual <= 1.0) nice_tick = magnitude;
    else if (residual <= 2.0) nice_tick = 2.0 * magnitude;
    else if (residual <= 5.0) nice_tick = 5.0 * magnitude;
    else nice_tick = 10.0 * magnitude;

    // Draw ruler background
    dl->AddRectFilled(area_min, ImVec2(area_max.x, area_min.y + ruler_height),
                      IM_COL32(40, 40, 40, 255));

    // Draw ticks
    double first_tick = std::ceil(view.view_start_ts / nice_tick) * nice_tick;
    for (double t = first_tick; t <= view.view_end_ts; t += nice_tick) {
        float x = view.time_to_x(t, area_min.x, width);

        // Major tick line extending into track area
        dl->AddLine(ImVec2(x, area_min.y + ruler_height - 30),
                   ImVec2(x, area_min.y + ruler_height),
                   IM_COL32(180, 180, 180, 255));

        // Faint grid line through tracks
        dl->AddLine(ImVec2(x, area_min.y + ruler_height),
                   ImVec2(x, area_max.y),
                   IM_COL32(60, 60, 60, 100));

        // Time label (clip to ruler area so last label doesn't overflow)
        char buf[64];
        format_ruler_time(t, nice_tick, buf, sizeof(buf));
        dl->PushClipRect(area_min, ImVec2(area_max.x, area_min.y + ruler_height), true);
        dl->AddText(ImVec2(x + 6, area_min.y + 6), IM_COL32(200, 200, 200, 255), buf);
        dl->PopClipRect();
    }

    // Ruler bottom border
    dl->AddLine(ImVec2(area_min.x, area_min.y + ruler_height),
               ImVec2(area_max.x, area_min.y + ruler_height),
               IM_COL32(80, 80, 80, 255));
}

void TimelineView::render_tracks(ImDrawList* dl, ImVec2 area_min, ImVec2 area_max,
                                 const TraceModel& model, ViewState& view) {
    float ruler_height = view.ruler_height;
    float width = area_max.x - area_min.x;
    float y = area_min.y + ruler_height - scroll_y_;
    float clip_top = area_min.y + ruler_height;
    float clip_bottom = area_max.y;

    track_layouts_.clear();

    dl->PushClipRect(ImVec2(area_min.x, clip_top), area_max, true);

    for (int pi = 0; pi < (int)model.processes_.size(); pi++) {
        const auto& proc = model.processes_[pi];
        if (view.hidden_pids.count(proc.pid)) continue;

        // Process header
        float proc_header_h = view.proc_header_height;
        if (y + proc_header_h > clip_top && y < clip_bottom) {
            dl->AddRectFilled(ImVec2(area_min.x, y),
                            ImVec2(area_max.x, y + proc_header_h),
                            IM_COL32(50, 50, 60, 255));
            dl->AddText(ImVec2(area_min.x + 15, y + 9),
                       IM_COL32(220, 220, 220, 255), proc.name.c_str());
        }
        y += proc_header_h;

        for (int ti = 0; ti < (int)proc.threads.size(); ti++) {
            const auto& thread = proc.threads[ti];
            if (view.hidden_tids.count(thread.tid)) continue;

            float track_h = (thread.max_depth + 1) * view.track_height + view.track_padding;

            // Store layout for hit testing
            TrackLayout layout;
            layout.pid = proc.pid;
            layout.tid = thread.tid;
            layout.y_start = y;
            layout.height = track_h;
            layout.proc_idx = pi;
            layout.thread_idx = ti;
            track_layouts_.push_back(layout);

            // Skip if entirely off screen
            if (y + track_h < clip_top || y > clip_bottom) {
                y += track_h;
                continue;
            }

            // Thread label background
            dl->AddRectFilled(ImVec2(area_min.x, y),
                            ImVec2(area_min.x + view.label_width, y + track_h - view.track_padding),
                            IM_COL32(35, 35, 40, 255));
            // Thread name (clipped to label area)
            dl->PushClipRect(ImVec2(area_min.x, y),
                           ImVec2(area_min.x + view.label_width - 15, y + track_h), true);
            dl->AddText(ImVec2(area_min.x + 30, y + 6),
                       IM_COL32(180, 180, 200, 255), thread.name.c_str());
            dl->PopClipRect();

            // Track separator line
            dl->AddLine(ImVec2(area_min.x, y + track_h - 1),
                       ImVec2(area_max.x, y + track_h - 1),
                       IM_COL32(50, 50, 50, 255));

            // Query visible events
            visible_events_.clear();
            model.query_visible(thread, view.view_start_ts,
                               view.view_end_ts, visible_events_);

            // Render slices
            float track_left = area_min.x + view.label_width;
            float track_width = width - view.label_width;

            dl->PushClipRect(ImVec2(track_left, y),
                           ImVec2(area_max.x, y + track_h), true);

            for (uint32_t ev_idx : visible_events_) {
                const auto& ev = model.events_[ev_idx];
                if (ev.is_end_event) continue;
                if (ev.ph == Phase::Instant) {
                    // Draw instant event as a thin diamond/line
                    float x = view.time_to_x(ev.ts, track_left, track_width);
                    float ey = y + ev.depth * view.track_height;
                    ImU32 col = ColorPalette::color_for_event(ev.cat_idx, ev.name_idx);
                    dl->AddLine(ImVec2(x, ey), ImVec2(x, ey + view.track_height - 1), col, 2.0f);
                    continue;
                }

                if (view.hidden_cats.count(ev.cat_idx)) continue;

                float x1 = view.time_to_x(ev.ts, track_left, track_width);
                float x2 = view.time_to_x(ev.end_ts(), track_left, track_width);
                float ey = y + ev.depth * view.track_height;

                // Clamp to visible area
                x1 = std::max(x1, track_left);
                x2 = std::min(x2, area_max.x);

                float slice_w = x2 - x1;
                if (slice_w < 0.5f) {
                    // Sub-pixel: draw a thin line
                    ImU32 col = ColorPalette::color_for_event(ev.cat_idx, ev.name_idx);
                    dl->AddLine(ImVec2(x1, ey), ImVec2(x1, ey + view.track_height - 1), col);
                    continue;
                }

                ImU32 fill = ColorPalette::color_for_event(ev.cat_idx, ev.name_idx);
                ImVec2 p1(x1, ey);
                ImVec2 p2(x2, ey + view.track_height - 1);

                // Highlight selected
                if ((int32_t)ev_idx == view.selected_event_idx) {
                    dl->AddRectFilled(ImVec2(x1 - 2, ey - 2),
                                    ImVec2(x2 + 2, ey + view.track_height + 1),
                                    IM_COL32(255, 255, 255, 100));
                }

                dl->AddRectFilled(p1, p2, fill);
                if (slice_w > 3.0f) {
                    dl->AddRect(p1, p2, ColorPalette::border_color(fill));
                }

                // Text label if wide enough
                if (slice_w > 60.0f) {
                    const std::string& name = model.get_string(ev.name_idx);
                    ImU32 text_col = ColorPalette::text_color(fill);
                    dl->PushClipRect(ImVec2(x1 + 6, ey), ImVec2(x2 - 6, ey + view.track_height), true);
                    dl->AddText(ImVec2(x1 + 9, ey + 6), text_col, name.c_str());
                    dl->PopClipRect();
                }
            }

            dl->PopClipRect();
            y += track_h;
        }

        // Counter tracks for this process
        float counter_h = counter_renderer_.render(dl, area_min, y, width, model, proc.pid, view);
        y += counter_h;
    }

    dl->PopClipRect();

    // Build flow position map from track layouts
    std::unordered_map<uint64_t, FlowRenderer::TrackPos> flow_positions;
    for (const auto& layout : track_layouts_) {
        uint64_t key = FlowRenderer::make_key(layout.pid, layout.tid);
        flow_positions[key] = {layout.y_start, layout.height};
    }
    flow_renderer_.set_track_positions(flow_positions);

    // Compute total content height (y is now at the bottom of all tracks)
    total_content_height_ = (y + scroll_y_) - (area_min.y + ruler_height);
}

int32_t TimelineView::hit_test(float click_x, float click_y, ImVec2 area_min, ImVec2 area_max,
                                const TraceModel& model, const ViewState& view) {
    float track_left = area_min.x + view.label_width;
    float track_width = (area_max.x - area_min.x) - view.label_width;

    // Find which track was clicked
    for (const auto& layout : track_layouts_) {
        if (click_y < layout.y_start || click_y >= layout.y_start + layout.height)
            continue;
        if (click_x < track_left) continue;

        const auto& proc = model.processes_[layout.proc_idx];
        const auto& thread = proc.threads[layout.thread_idx];

        // Check if click is within the actual slice rows (not in padding below)
        float rows_height = (thread.max_depth + 1) * view.track_height;
        float rel_y = click_y - layout.y_start;
        if (rel_y >= rows_height) return -1; // In padding area

        int clicked_depth = (int)(rel_y / view.track_height);

        double click_time = view.x_to_time(click_x, track_left, track_width);

        // Pixel tolerance: convert 3 pixels into time units for near-miss selection
        double px_tolerance = 3.0;
        double time_per_px = (view.view_end_ts - view.view_start_ts) / (double)track_width;
        double tolerance = px_tolerance * time_per_px;

        // Find the best matching event at this depth and time
        int32_t best = -1;
        double best_dur = 1e18;

        std::vector<uint32_t> candidates;
        model.query_visible(thread, click_time - tolerance, click_time + tolerance, candidates);

        for (uint32_t idx : candidates) {
            const auto& ev = model.events_[idx];
            if (ev.is_end_event) continue;
            if (ev.depth != clicked_depth) continue;
            if (click_time >= ev.ts - tolerance && click_time <= ev.end_ts() + tolerance) {
                if (ev.dur < best_dur) {
                    best_dur = ev.dur;
                    best = (int32_t)idx;
                }
            }
        }

        return best;
    }
    return -1;
}

void TimelineView::render(const TraceModel& model, ViewState& view) {
    ImGui::Begin("Timeline", nullptr, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    ImVec2 canvas_min = ImGui::GetCursorScreenPos();
    ImVec2 canvas_size = ImGui::GetContentRegionAvail();

    float scrollbar_size = ImGui::GetStyle().ScrollbarSize * view.scrollbar_scale;

    // Reserve space for scrollbars
    canvas_size.x -= scrollbar_size;
    canvas_size.y -= scrollbar_size;

    if (canvas_size.x < 50.0f || canvas_size.y < 50.0f) {
        ImGui::End();
        return;
    }
    ImVec2 canvas_max(canvas_min.x + canvas_size.x, canvas_min.y + canvas_size.y);

    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Background
    dl->AddRectFilled(canvas_min, canvas_max, IM_COL32(30, 30, 30, 255));

    // Label gutter resize handle
    {
        float handle_w = 12.0f;
        float handle_x = canvas_min.x + view.label_width - handle_w / 2;
        ImVec2 handle_min(handle_x, canvas_min.y);
        ImVec2 handle_max(handle_x + handle_w, canvas_max.y);

        ImGui::SetCursorScreenPos(handle_min);
        ImGui::InvisibleButton("##gutter_resize", ImVec2(handle_w, canvas_size.y));
        if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        }
        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            view.label_width += ImGui::GetIO().MouseDelta.x;
            view.label_width = std::max(100.0f, std::min(view.label_width, canvas_size.x - 200.0f));
        }
        // Draw splitter line
        float line_x = canvas_min.x + view.label_width;
        ImU32 splitter_col = ImGui::IsItemHovered() || ImGui::IsItemActive()
            ? IM_COL32(120, 120, 140, 255) : IM_COL32(60, 60, 70, 255);
        dl->AddLine(ImVec2(line_x, canvas_min.y), ImVec2(line_x, canvas_max.y), splitter_col, 2.0f);
    }

    // Make the canvas area interactive
    ImGui::SetCursorScreenPos(canvas_min);
    ImGui::InvisibleButton("timeline_canvas", canvas_size,
                           ImGuiButtonFlags_MouseButtonLeft |
                           ImGuiButtonFlags_MouseButtonMiddle);
    bool is_hovered = ImGui::IsItemHovered();
    bool is_active = ImGui::IsItemActive();

    ImGuiIO& io = ImGui::GetIO();

    // Zoom with mouse wheel (Ctrl+wheel = horizontal zoom, Shift+wheel = vertical scroll)
    if (is_hovered && io.MouseWheel != 0.0f) {
        if (io.KeyShift) {
            // Shift+wheel: vertical scroll
            scroll_y_ -= io.MouseWheel * view.track_height * 3.0f;
            scroll_y_ = std::max(0.0f, scroll_y_);
            float max_scroll = std::max(0.0f, total_content_height_ - canvas_size.y + view.ruler_height);
            scroll_y_ = std::min(scroll_y_, max_scroll);
        } else {
            // Wheel: horizontal time zoom
            float track_left = canvas_min.x + view.label_width;
            float track_width = canvas_size.x - view.label_width;
            double mouse_time = view.x_to_time(io.MousePos.x, track_left, track_width);
            double zoom_factor = (io.MouseWheel > 0) ? 0.8 : 1.25;

            double new_start = mouse_time + (view.view_start_ts - mouse_time) * zoom_factor;
            double new_end = mouse_time + (view.view_end_ts - mouse_time) * zoom_factor;

            // Minimum range: 10 us
            if (new_end - new_start > 0.001) {
                view.view_start_ts = new_start;
                view.view_end_ts = new_end;
            }
        }
    }

    // Pan with middle mouse or ctrl+left
    if (is_active && (ImGui::IsMouseDragging(ImGuiMouseButton_Middle) ||
                      (ImGui::IsMouseDragging(ImGuiMouseButton_Left) && io.KeyCtrl))) {
        float track_width = canvas_size.x - view.label_width;
        double range = view.view_end_ts - view.view_start_ts;
        double dx_time = (double)io.MouseDelta.x / track_width * range;
        view.view_start_ts -= dx_time;
        view.view_end_ts -= dx_time;
        scroll_y_ -= io.MouseDelta.y;
        scroll_y_ = std::max(0.0f, scroll_y_);
        float max_scroll = std::max(0.0f, total_content_height_ - canvas_size.y + view.ruler_height);
        scroll_y_ = std::min(scroll_y_, max_scroll);
    }

    // Click to select
    if (is_hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !io.KeyCtrl) {
        int32_t hit = hit_test(io.MousePos.x, io.MousePos.y,
                               canvas_min, canvas_max, model, view);
        view.selected_event_idx = hit;
    }

    // Keyboard shortcuts (only when no text input is active)
    if (ImGui::IsWindowFocused() && !ImGui::GetIO().WantTextInput) {
        float track_width = canvas_size.x - view.label_width;
        double range = view.view_end_ts - view.view_start_ts;
        double pan_amount = range * 0.1;

        if (ImGui::IsKeyPressed(ImGuiKey_A) || ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) {
            view.view_start_ts -= pan_amount;
            view.view_end_ts -= pan_amount;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_D) || ImGui::IsKeyPressed(ImGuiKey_RightArrow)) {
            view.view_start_ts += pan_amount;
            view.view_end_ts += pan_amount;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) {
            scroll_y_ -= view.track_height * 3.0f;
            scroll_y_ = std::max(0.0f, scroll_y_);
        }
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) {
            scroll_y_ += view.track_height * 3.0f;
            float max_scroll = std::max(0.0f, total_content_height_ - canvas_size.y + view.ruler_height);
            scroll_y_ = std::min(scroll_y_, max_scroll);
        }
        if (ImGui::IsKeyPressed(ImGuiKey_W)) {
            double center = (view.view_start_ts + view.view_end_ts) / 2.0;
            double half = range * 0.4;
            view.view_start_ts = center - half;
            view.view_end_ts = center + half;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_S)) {
            double center = (view.view_start_ts + view.view_end_ts) / 2.0;
            double half = range * 0.625;
            view.view_start_ts = center - half;
            view.view_end_ts = center + half;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_F)) {
            if (view.selected_event_idx >= 0) {
                const auto& ev = model.events_[view.selected_event_idx];
                double pad = std::max(ev.dur * 0.5, 100.0);
                view.view_start_ts = ev.ts - pad;
                view.view_end_ts = ev.end_ts() + pad;
            } else if (model.min_ts_ < model.max_ts_) {
                view.zoom_to_fit(model.min_ts_, model.max_ts_);
            }
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            view.selected_event_idx = -1;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_G)) {
            show_goto_ = true;
            goto_buf_[0] = '\0';
        }
    }

    // Go-to-time popup
    if (show_goto_) {
        ImGui::OpenPopup("Go to Time");
        show_goto_ = false;
    }
    if (ImGui::BeginPopup("Go to Time")) {
        ImGui::Text("Enter time (e.g. 63.4231s, 500ms, 1234us, 5000ns):");
        ImGui::SetNextItemWidth(400);
        bool submitted = ImGui::InputText("##goto_time", goto_buf_, sizeof(goto_buf_),
                                           ImGuiInputTextFlags_EnterReturnsTrue);
        if (ImGui::IsWindowAppearing()) {
            ImGui::SetKeyboardFocusHere(-1);
        }
        ImGui::SameLine();
        if (submitted || ImGui::Button("Go")) {
            // Parse time string to microseconds
            char* end = nullptr;
            double val = std::strtod(goto_buf_, &end);
            if (end != goto_buf_) {
                // Skip whitespace
                while (*end == ' ') end++;
                double target_us = 0.0;
                if (strncmp(end, "ns", 2) == 0) {
                    target_us = val / 1000.0;
                } else if (strncmp(end, "us", 2) == 0 || *end == '\0') {
                    target_us = val;
                } else if (strncmp(end, "ms", 2) == 0) {
                    target_us = val * 1000.0;
                } else if (*end == 's') {
                    target_us = val * 1000000.0;
                } else {
                    target_us = val; // default to us
                }
                // Center view on target, keep current zoom level
                double range = view.view_end_ts - view.view_start_ts;
                view.view_start_ts = target_us - range / 2.0;
                view.view_end_ts = target_us + range / 2.0;
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // Render timeline content
    render_time_ruler(dl, canvas_min, canvas_max, view);
    render_tracks(dl, canvas_min, canvas_max, model, view);

    // Render flow arrows on top of tracks
    flow_renderer_.render(dl, model, view, canvas_min, canvas_max, view.label_width);

    // --- Vertical scrollbar (right side) ---
    {
        float visible_h = canvas_size.y - view.ruler_height; // subtract ruler
        float max_scroll = std::max(1.0f, total_content_height_ - visible_h);
        scroll_y_ = std::min(scroll_y_, std::max(0.0f, max_scroll));

        ImVec2 sb_min(canvas_max.x, canvas_min.y);
        ImVec2 sb_max(canvas_max.x + scrollbar_size, canvas_max.y);

        dl->AddRectFilled(sb_min, sb_max, IM_COL32(25, 25, 28, 255));

        if (total_content_height_ > visible_h) {
            float sb_range = sb_max.y - sb_min.y;
            float thumb_h = std::max(scrollbar_size, sb_range * visible_h / total_content_height_);
            float thumb_travel = sb_range - thumb_h;
            float thumb_y = sb_min.y + thumb_travel * (scroll_y_ / max_scroll);

            ImVec2 thumb_min(sb_min.x + 2, thumb_y);
            ImVec2 thumb_max(sb_max.x - 2, thumb_y + thumb_h);

            // Hit test for scrollbar dragging
            ImGui::SetCursorScreenPos(sb_min);
            ImGui::InvisibleButton("##vscroll", ImVec2(scrollbar_size, sb_range));
            bool sb_hovered = ImGui::IsItemHovered();
            bool sb_active = ImGui::IsItemActive();

            ImU32 thumb_col = IM_COL32(100, 100, 110, 200);
            if (sb_active) {
                thumb_col = IM_COL32(160, 160, 170, 255);
                float drag_ratio = io.MouseDelta.y / thumb_travel;
                scroll_y_ += drag_ratio * max_scroll;
                scroll_y_ = std::max(0.0f, std::min(scroll_y_, max_scroll));
            } else if (sb_hovered) {
                thumb_col = IM_COL32(130, 130, 140, 230);
            }

            dl->AddRectFilled(thumb_min, thumb_max, thumb_col, scrollbar_size * 0.3f);
        }
    }

    // --- Horizontal scrollbar (bottom) ---
    {
        double total_time = 1.0;
        if (model.min_ts_ < model.max_ts_) {
            total_time = (double)(model.max_ts_ - model.min_ts_);
        }
        double visible_time = view.view_end_ts - view.view_start_ts;

        ImVec2 sb_min(canvas_min.x, canvas_max.y);
        ImVec2 sb_max(canvas_max.x, canvas_max.y + scrollbar_size);

        dl->AddRectFilled(sb_min, sb_max, IM_COL32(25, 25, 28, 255));

        float sb_range = sb_max.x - sb_min.x;
        float min_thumb = scrollbar_size * 3.0f; // minimum grabbable size
        float thumb_w = std::max(min_thumb, (float)(sb_range * visible_time / total_time));
        thumb_w = std::min(thumb_w, sb_range);
        float thumb_travel = sb_range - thumb_w;

        float scroll_frac = 0.0f;
        if (total_time > visible_time) {
            scroll_frac = (float)((view.view_start_ts - model.min_ts_) / (total_time - visible_time));
            scroll_frac = std::max(0.0f, std::min(1.0f, scroll_frac));
        }
        float thumb_x = sb_min.x + thumb_travel * scroll_frac;

        ImVec2 thumb_min(thumb_x, sb_min.y + 2);
        ImVec2 thumb_max(thumb_x + thumb_w, sb_max.y - 2);

        // Hit test for scrollbar dragging
        ImGui::SetCursorScreenPos(sb_min);
        ImGui::InvisibleButton("##hscroll", ImVec2(sb_range, scrollbar_size));
        bool sb_hovered = ImGui::IsItemHovered();
        bool sb_active = ImGui::IsItemActive();

        ImU32 thumb_col = IM_COL32(100, 100, 110, 200);
        if (sb_active && thumb_travel > 0) {
            thumb_col = IM_COL32(160, 160, 170, 255);
            float drag_ratio = io.MouseDelta.x / thumb_travel;
            double shift = drag_ratio * (total_time - visible_time);
            view.view_start_ts += shift;
            view.view_end_ts += shift;
        } else if (sb_hovered) {
            thumb_col = IM_COL32(130, 130, 140, 230);
        }

        dl->AddRectFilled(thumb_min, thumb_max, thumb_col, scrollbar_size * 0.3f);
    }

    // Hover tooltip
    if (is_hovered && !ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
        int32_t hover = hit_test(io.MousePos.x, io.MousePos.y,
                                 canvas_min, canvas_max, model, view);
        if (hover >= 0) {
            const auto& ev = model.events_[hover];
            ImGui::BeginTooltip();
            ImGui::Text("%s", model.get_string(ev.name_idx).c_str());
            if (ev.dur > 0) {
                char dur_buf[64];
                format_time((double)ev.dur, dur_buf, sizeof(dur_buf));
                ImGui::Text("Duration: %s", dur_buf);
            }
            ImGui::Text("Category: %s", model.get_string(ev.cat_idx).c_str());
            ImGui::EndTooltip();
        }
    }

    ImGui::End();
}
