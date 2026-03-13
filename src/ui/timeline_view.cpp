#include "timeline_view.h"
#include "format_time.h"
#include "tracing.h"
#include "model/color_palette.h"
#include "imgui.h"
#include <cmath>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

void TimelineView::render_time_ruler(ImDrawList* dl, ImVec2 area_min, ImVec2 area_max, const ViewState& view) {
    TRACE_SCOPE_CAT("TimeRuler", "timeline");
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
    if (residual <= 1.0)
        nice_tick = magnitude;
    else if (residual <= 2.0)
        nice_tick = 2.0 * magnitude;
    else if (residual <= 5.0)
        nice_tick = 5.0 * magnitude;
    else
        nice_tick = 10.0 * magnitude;

    // Draw ruler background
    dl->AddRectFilled(area_min, ImVec2(area_max.x, area_min.y + ruler_height), IM_COL32(40, 40, 40, 255));

    // Draw ticks
    double first_tick = std::ceil(view.view_start_ts / nice_tick) * nice_tick;
    for (double t = first_tick; t <= view.view_end_ts; t += nice_tick) {
        float x = view.time_to_x(t, area_min.x, width);

        // Major tick line extending into track area
        dl->AddLine(ImVec2(x, area_min.y + ruler_height - 30), ImVec2(x, area_min.y + ruler_height),
                    IM_COL32(180, 180, 180, 255));

        // Faint grid line through tracks
        dl->AddLine(ImVec2(x, area_min.y + ruler_height), ImVec2(x, area_max.y), IM_COL32(60, 60, 60, 100));

        // Time label (clip to ruler area so last label doesn't overflow)
        char buf[64];
        format_ruler_time(t, nice_tick, buf, sizeof(buf));
        dl->PushClipRect(area_min, ImVec2(area_max.x, area_min.y + ruler_height), true);
        dl->AddText(ImVec2(x + 6, area_min.y + 6), IM_COL32(200, 200, 200, 255), buf);
        dl->PopClipRect();
    }

    // Ruler bottom border
    dl->AddLine(ImVec2(area_min.x, area_min.y + ruler_height), ImVec2(area_max.x, area_min.y + ruler_height),
                IM_COL32(80, 80, 80, 255));
}

void TimelineView::render_tracks(ImDrawList* dl, ImVec2 area_min, ImVec2 area_max, const TraceModel& model,
                                 ViewState& view) {
    TRACE_SCOPE_CAT("RenderTracks", "timeline");
    diag_stats = {};
    float ruler_height = view.ruler_height;
    float width = area_max.x - area_min.x;
    float y = area_min.y + ruler_height - scroll_y_;
    float clip_top = area_min.y + ruler_height;
    float clip_bottom = area_max.y;

    track_layouts_.clear();
    sel_rect_valid_ = false;

    dl->PushClipRect(ImVec2(area_min.x, clip_top), area_max, true);

    for (int pi = 0; pi < (int)model.processes_.size(); pi++) {
        const auto& proc = model.processes_[pi];
        if (view.hidden_pids.count(proc.pid)) continue;

        // Process header
        float proc_header_h = view.proc_header_height;
        if (y + proc_header_h > clip_top && y < clip_bottom) {
            dl->AddRectFilled(ImVec2(area_min.x, y), ImVec2(area_max.x, y + proc_header_h), IM_COL32(50, 50, 60, 255));
            dl->AddText(ImVec2(area_min.x + 15, y + 9), IM_COL32(220, 220, 220, 255), proc.name.c_str());
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

            diag_stats.tracks_visible++;

            // Thread label background
            dl->AddRectFilled(ImVec2(area_min.x, y),
                              ImVec2(area_min.x + view.label_width, y + track_h - view.track_padding),
                              IM_COL32(35, 35, 40, 255));
            // Thread name (clipped to label area)
            dl->PushClipRect(ImVec2(area_min.x, y), ImVec2(area_min.x + view.label_width - 15, y + track_h), true);
            dl->AddText(ImVec2(area_min.x + 30, y + 6), IM_COL32(180, 180, 200, 255), thread.name.c_str());
            dl->PopClipRect();

            // Track separator line
            dl->AddLine(ImVec2(area_min.x, y + track_h - 1), ImVec2(area_max.x, y + track_h - 1),
                        IM_COL32(50, 50, 50, 255));

            // Render slices — iterate block index directly, no intermediate vector.
            // For blocks that are entirely sub-pixel, skip individual events and
            // just extend merge runs using the block's depth_mask.
            float track_left = area_min.x + view.label_width;
            float track_width = width - view.label_width;

            dl->PushClipRect(ImVec2(track_left, y), ImVec2(area_max.x, y + track_h), true);

            constexpr float MERGE_THRESHOLD_PX = 2.0f;

            struct ActiveMerge {
                float x_start;
                float x_end;
            };
            uint8_t num_depths = thread.max_depth + 1;
            ActiveMerge depth_merges[256];
            for (uint8_t d = 0; d < num_depths; d++) {
                depth_merges[d].x_end = -1e30f;
            }

            {
                const auto& bi = thread.block_index;
                const auto& event_indices = thread.event_indices;
                const float track_height = view.track_height;
                const auto* events = model.events_.data();
                const int32_t sel_idx = view.selected_event_idx;
                const bool has_hidden_cats = !view.hidden_cats.empty();
                const double view_start = view.view_start_ts;
                const double view_end = view.view_end_ts;
                const double view_range = view_end - view_start;
                const double ppu = (view_range > 0.0) ? (double)track_width / view_range : 0.0;

                size_t first_block = bi.find_first_block(view_start);

                TRACE_SCOPE_ARGS("RenderTracks_blocks", "timeline", "first_block", (int)first_block, "total_blocks",
                                 (int)bi.blocks.size());

                for (size_t bli = first_block; bli < bi.blocks.size(); bli++) {
                    const auto& blk = bi.blocks[bli];
                    if (blk.min_ts > view_end) break;
                    if (blk.max_end_ts < view_start) continue;

                    // Check if this block's entire time span is sub-pixel
                    float blk_x1 = track_left + (float)((blk.min_ts - view_start) * ppu);
                    float blk_x2 = track_left + (float)((blk.local_max_end_ts - view_start) * ppu);
                    blk_x1 = std::max(blk_x1, track_left);
                    blk_x2 = std::min(blk_x2, area_max.x);
                    float blk_w = blk_x2 - blk_x1;

                    if (blk_w < MERGE_THRESHOLD_PX && blk.count > 1) {
                        // Entire block is sub-pixel — extend merge runs for all depths
                        // present in this block without iterating individual events
                        uint32_t mask = blk.depth_mask;
                        while (mask) {
                            int d = __builtin_ctz(mask);
                            mask &= mask - 1;
                            auto& run = depth_merges[d];
                            if (blk_x1 <= run.x_end + MERGE_THRESHOLD_PX) {
                                run.x_end = std::max(run.x_end, blk_x2);
                            } else {
                                if (run.x_end > -1e29f) {
                                    float ey = y + d * track_height;
                                    dl->AddRectFilled(ImVec2(run.x_start, ey), ImVec2(run.x_end, ey + track_height - 1),
                                                      IM_COL32(120, 120, 140, 180));
                                    diag_stats.merge_runs++;
                                }
                                run.x_start = blk_x1;
                                run.x_end = std::max(blk_x2, blk_x1 + 1.0f);
                            }
                        }
                        diag_stats.merged_slices += blk.count;
                        continue;
                    }

                    // Expand individual events in this block
                    for (uint32_t j = 0; j < blk.count; j++) {
                        uint32_t ev_idx = event_indices[blk.start_idx + j];
                        const auto& ev = events[ev_idx];
                        if (ev.ts > view_end) break;
                        if (ev.end_ts() < view_start) continue;
                        if (ev.is_end_event) continue;
                        diag_stats.visible_slices++;

                        if (ev.ph == Phase::Instant) {
                            float x = track_left + (float)((ev.ts - view_start) * ppu);
                            float ey = y + ev.depth * track_height;
                            ImU32 col = ColorPalette::color_for_event(ev.cat_idx, ev.name_idx);
                            dl->AddLine(ImVec2(x, ey), ImVec2(x, ey + track_height - 1), col, 2.0f);
                            diag_stats.instant_events++;
                            continue;
                        }

                        if (has_hidden_cats && view.hidden_cats.count(ev.cat_idx)) continue;

                        float x1 = track_left + (float)((ev.ts - view_start) * ppu);
                        float x2 = track_left + (float)((ev.end_ts() - view_start) * ppu);

                        x1 = std::max(x1, track_left);
                        x2 = std::min(x2, area_max.x);

                        float slice_w = x2 - x1;

                        if (slice_w < MERGE_THRESHOLD_PX) {
                            auto& run = depth_merges[ev.depth];
                            if (x1 <= run.x_end + MERGE_THRESHOLD_PX) {
                                run.x_end = std::max(run.x_end, x2);
                            } else {
                                if (run.x_end > -1e29f) {
                                    float ey = y + ev.depth * track_height;
                                    dl->AddRectFilled(ImVec2(run.x_start, ey), ImVec2(run.x_end, ey + track_height - 1),
                                                      IM_COL32(120, 120, 140, 180));
                                    diag_stats.merge_runs++;
                                }
                                run.x_start = x1;
                                run.x_end = std::max(x2, x1 + 1.0f);
                            }
                            diag_stats.merged_slices++;
                            continue;
                        }

                        diag_stats.drawn_slices++;

                        float ey = y + ev.depth * track_height;
                        ImU32 fill = ColorPalette::color_for_event(ev.cat_idx, ev.name_idx);
                        ImVec2 p1(x1, ey);
                        ImVec2 p2(x2, ey + track_height - 1);

                        dl->AddRectFilled(p1, p2, fill);
                        if ((int32_t)ev_idx == sel_idx) {
                            sel_rect_min_ = ImVec2(x1 - 2, ey - 2);
                            sel_rect_max_ = ImVec2(x2 + 2, ey + track_height + 1);
                            sel_rect_valid_ = true;
                        } else if (slice_w > 3.0f) {
                            dl->AddRect(p1, p2, ColorPalette::border_color(fill));
                        }

                        if (slice_w > 60.0f) {
                            const std::string& name = model.get_string(ev.name_idx);
                            ImU32 text_col = ColorPalette::text_color(fill);
                            dl->PushClipRect(ImVec2(x1 + 6, ey), ImVec2(x2 - 6, ey + track_height), true);
                            dl->AddText(ImVec2(x1 + 9, ey + 6), text_col, name.c_str());
                            dl->PopClipRect();
                            diag_stats.labels_drawn++;
                        }
                    }
                }
            }

            // Flush remaining active merge runs
            for (uint8_t d = 0; d < num_depths; d++) {
                if (depth_merges[d].x_end > -1e29f) {
                    diag_stats.merge_runs++;
                    float ey = y + d * view.track_height;
                    dl->AddRectFilled(ImVec2(depth_merges[d].x_start, ey),
                                      ImVec2(depth_merges[d].x_end, ey + view.track_height - 1),
                                      IM_COL32(120, 120, 140, 180));
                }
            }

            dl->PopClipRect();
            y += track_h;
        }

        // Counter tracks for this process
        {
            TRACE_SCOPE_CAT("RenderTracks_counters", "timeline");
            float counter_h = counter_renderer_.render(dl, area_min, y, width, model, proc.pid, view);
            y += counter_h;
        }
    }

    // Draw range selection overlay (below ruler only)
    if (view.has_range_selection) {
        float track_left = area_min.x + view.label_width;
        float track_width = width - view.label_width;
        float rx1 = view.time_to_x(view.range_start_ts, track_left, track_width);
        float rx2 = view.time_to_x(view.range_end_ts, track_left, track_width);
        rx1 = std::max(rx1, track_left);
        rx2 = std::min(rx2, area_max.x);
        float overlay_top = area_min.y + view.ruler_height;

        if (rx2 > rx1) {
            dl->AddRectFilled(ImVec2(rx1, overlay_top), ImVec2(rx2, area_max.y), IM_COL32(80, 130, 220, 50));
            dl->AddLine(ImVec2(rx1, overlay_top), ImVec2(rx1, area_max.y), IM_COL32(80, 130, 220, 200), 1.5f);
            dl->AddLine(ImVec2(rx2, overlay_top), ImVec2(rx2, area_max.y), IM_COL32(80, 130, 220, 200), 1.5f);
        }
    }

    // Draw selected event border on top of everything
    if (sel_rect_valid_) {
        dl->AddRect(sel_rect_min_, sel_rect_max_, view.sel_border_color_u32(), 0.0f, 0, 3.0f);
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

int32_t TimelineView::hit_test(float click_x, float click_y, ImVec2 area_min, ImVec2 area_max, const TraceModel& model,
                               const ViewState& view) {
    TRACE_FUNCTION_CAT("timeline");
    float track_left = area_min.x + view.label_width;
    float track_width = (area_max.x - area_min.x) - view.label_width;

    // Find which track was clicked
    for (const auto& layout : track_layouts_) {
        if (click_y < layout.y_start || click_y >= layout.y_start + layout.height) continue;
        if (click_x < track_left) continue;

        const auto& proc = model.processes_[layout.proc_idx];
        const auto& thread = proc.threads[layout.thread_idx];

        // Check if click is within the actual slice rows (not in padding below)
        float rows_height = (thread.max_depth + 1) * view.track_height;
        float rel_y = click_y - layout.y_start;
        if (rel_y >= rows_height) return -1;  // In padding area

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
    TRACE_SCOPE_CAT("Timeline", "ui");
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
        ImU32 splitter_col =
            ImGui::IsItemHovered() || ImGui::IsItemActive() ? IM_COL32(120, 120, 140, 255) : IM_COL32(60, 60, 70, 255);
        dl->AddLine(ImVec2(line_x, canvas_min.y), ImVec2(line_x, canvas_max.y), splitter_col, 2.0f);
    }

    ImGuiIO& io = ImGui::GetIO();

    // Ruler interaction area (for range selection drag)
    {
        float ruler_h = view.ruler_height;
        ImGui::SetCursorScreenPos(canvas_min);
        ImGui::InvisibleButton("timeline_ruler", ImVec2(canvas_size.x, ruler_h), ImGuiButtonFlags_MouseButtonLeft);
        bool ruler_hovered = ImGui::IsItemHovered();

        float track_left = canvas_min.x + view.label_width;
        float track_width = canvas_size.x - view.label_width;

        if (ruler_hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && io.MousePos.x >= track_left) {
            ruler_dragging_ = true;
            ruler_drag_start_ts_ = view.x_to_time(io.MousePos.x, track_left, track_width);
            view.clear_range_selection();
        }

        if (ruler_dragging_) {
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left) || !ImGui::IsWindowFocused()) {
                ruler_dragging_ = false;
            } else {
                double current_ts = view.x_to_time(io.MousePos.x, track_left, track_width);
                if (std::abs(current_ts - ruler_drag_start_ts_) > 0.0) {
                    view.set_range_selection(ruler_drag_start_ts_, current_ts);
                }
            }
        }
    }

    // Make the canvas area interactive (below the ruler)
    ImGui::SetCursorScreenPos(ImVec2(canvas_min.x, canvas_min.y + view.ruler_height));
    ImVec2 track_canvas_size(canvas_size.x, canvas_size.y - view.ruler_height);
    ImGui::InvisibleButton("timeline_canvas", track_canvas_size,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);
    bool is_hovered = ImGui::IsItemHovered();
    bool is_active = ImGui::IsItemActive();

    // Also consider ruler area for hover/zoom
    bool any_hovered =
        is_hovered || (io.MousePos.y >= canvas_min.y && io.MousePos.y < canvas_min.y + view.ruler_height &&
                       io.MousePos.x >= canvas_min.x && io.MousePos.x < canvas_max.x);

    // Zoom with mouse wheel (Ctrl+wheel = horizontal zoom, Shift+wheel = vertical scroll)
    if (any_hovered && io.MouseWheel != 0.0f) {
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

    // Shift+drag in track area for range selection
    {
        float track_left = canvas_min.x + view.label_width;
        float track_width = canvas_size.x - view.label_width;

        if (is_hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && io.KeyShift && io.MousePos.x >= track_left) {
            ruler_dragging_ = true;
            ruler_drag_start_ts_ = view.x_to_time(io.MousePos.x, track_left, track_width);
            view.clear_range_selection();
        }
    }

    // Click to select (only if not starting a range drag)
    if (is_hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !io.KeyCtrl && !io.KeyShift) {
        int32_t hit = hit_test(io.MousePos.x, io.MousePos.y, canvas_min, canvas_max, model, view);
        view.selected_event_idx = hit;
        view.clear_range_selection();
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
            if (view.has_range_selection) {
                view.zoom_to_fit(view.range_start_ts, view.range_end_ts);
            } else if (view.selected_event_idx >= 0) {
                view.navigate_to_event(view.selected_event_idx, model.events_[view.selected_event_idx]);
            } else if (model.min_ts_ < model.max_ts_) {
                view.zoom_to_fit(model.min_ts_, model.max_ts_);
            }
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            if (view.has_range_selection) {
                view.clear_range_selection();
            } else {
                view.selected_event_idx = -1;
            }
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
        bool submitted =
            ImGui::InputText("##goto_time", goto_buf_, sizeof(goto_buf_), ImGuiInputTextFlags_EnterReturnsTrue);
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
                    target_us = val;  // default to us
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

    // Scroll vertically to show the pending event's track
    if (view.pending_scroll_event_idx >= 0 && view.pending_scroll_event_idx < (int32_t)model.events_.size()) {
        const auto& ev = model.events_[view.pending_scroll_event_idx];
        for (const auto& layout : track_layouts_) {
            if (layout.pid == ev.pid && layout.tid == ev.tid) {
                // layout.y_start is in screen coords: area_min.y + ruler_height - scroll_y_ + offset_within_content
                // Recover the content-space offset of this track
                float content_y = layout.y_start - (canvas_min.y + view.ruler_height) + scroll_y_;
                // Offset to the specific depth row within the track
                float row_y = content_y + ev.depth * view.track_height;
                float row_h = view.track_height;
                float visible_h = canvas_size.y - view.ruler_height;
                // Center the event's row vertically in the visible area
                scroll_y_ = row_y - (visible_h - row_h) * 0.5f;
                float max_scroll = std::max(0.0f, total_content_height_ - visible_h);
                scroll_y_ = std::max(0.0f, std::min(scroll_y_, max_scroll));
                break;
            }
        }
        view.pending_scroll_event_idx = -1;
    }

    // Render flow arrows on top of tracks
    flow_renderer_.render(dl, model, view, canvas_min, canvas_max, view.label_width);

    // --- Vertical scrollbar (right side) ---
    {
        float visible_h = canvas_size.y - view.ruler_height;  // subtract ruler
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
        float min_thumb = scrollbar_size * 3.0f;  // minimum grabbable size
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
        int32_t hover = hit_test(io.MousePos.x, io.MousePos.y, canvas_min, canvas_max, model, view);
        if (hover >= 0) {
            const auto& ev = model.events_[hover];
            char time_buf[64];
            ImGui::BeginTooltip();

            // Name (bold-ish via separator)
            ImGui::TextUnformatted(model.get_string(ev.name_idx).c_str());
            ImGui::Separator();

            // Category
            ImGui::TextDisabled("Category:");
            ImGui::SameLine();
            ImGui::TextUnformatted(model.get_string(ev.cat_idx).c_str());

            // Duration and self time
            if (ev.dur > 0) {
                format_time((double)ev.dur, time_buf, sizeof(time_buf));
                ImGui::TextDisabled("Duration:");
                ImGui::SameLine();
                ImGui::Text("%s", time_buf);

                if (ev.self_time >= 0 && ev.self_time < ev.dur) {
                    format_time(ev.self_time, time_buf, sizeof(time_buf));
                    float self_pct = (float)(ev.self_time / ev.dur * 100.0);
                    ImGui::TextDisabled("Self Time:");
                    ImGui::SameLine();
                    ImGui::Text("%s (%.1f%%)", time_buf, self_pct);
                }
            }

            // Thread name
            for (const auto& proc : model.processes_) {
                if (proc.pid == ev.pid) {
                    for (const auto& t : proc.threads) {
                        if (t.tid == ev.tid) {
                            ImGui::TextDisabled("Thread:");
                            ImGui::SameLine();
                            ImGui::Text("%s", t.name.c_str());
                            break;
                        }
                    }
                    break;
                }
            }

            ImGui::EndTooltip();
        } else {
            // Check counter tracks
            CounterHitResult counter_hit;
            if (counter_renderer_.hit_test(io.MousePos.x, io.MousePos.y, view, counter_hit)) {
                char time_buf[64];
                ImGui::BeginTooltip();

                ImGui::TextUnformatted(counter_hit.series->name.c_str());
                ImGui::Separator();

                ImGui::TextDisabled("Value:");
                ImGui::SameLine();
                ImGui::Text("%.4g", counter_hit.value);

                format_time(counter_hit.timestamp, time_buf, sizeof(time_buf));
                ImGui::TextDisabled("Timestamp:");
                ImGui::SameLine();
                ImGui::Text("%s", time_buf);

                ImGui::EndTooltip();
            }
        }
    }

    ImGui::End();
}
