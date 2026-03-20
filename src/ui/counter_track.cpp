#include "counter_track.h"
#include "tracing.h"
#include "model/color_palette.h"
#include <algorithm>
#include <cstdio>

float CounterTrackRenderer::render(ImDrawList* dl, ImVec2 area_min, float y_offset, float width,
                                   const TraceModel& model, uint32_t pid, const ViewState& view) {
    TRACE_FUNCTION_CAT("timeline");
    float total_height = 0.0f;
    uint32_t color_idx = 0;

    layouts_.clear();

    for (const auto& series : model.counter_series()) {
        if (series.pid != pid) continue;
        if (series.points.empty()) continue;

        float track_h = view.counter_track_height();
        ImVec2 track_min(area_min.x + view.label_width(), y_offset + total_height);
        ImVec2 track_max(area_min.x + width, y_offset + total_height + track_h);

        // Label
        dl->AddRectFilled(ImVec2(area_min.x, track_min.y), ImVec2(area_min.x + view.label_width(), track_max.y),
                          IM_COL32(35, 35, 40, 255));
        dl->PushClipRect(ImVec2(area_min.x, track_min.y), ImVec2(area_min.x + view.label_width() - 5, track_max.y),
                         true);
        dl->AddText(ImVec2(area_min.x + 10, track_min.y + 2), IM_COL32(160, 180, 200, 255), series.name.c_str());
        dl->PopClipRect();

        // Background
        dl->AddRectFilled(track_min, track_max, IM_COL32(25, 25, 30, 255));
        dl->AddRect(track_min, track_max, IM_COL32(50, 50, 50, 255));

        ImU32 color = ColorPalette::COLORS[color_idx % ColorPalette::NUM_COLORS];
        render_series(dl, track_min, track_max, series, view, color);
        layouts_.push_back({&series, track_min, track_max});

        total_height += track_h + view.track_padding();
        color_idx++;
    }

    return total_height;
}

// Mirrors ViewState::time_to_x — keep in sync if the mapping changes.
static float time_to_x(double ts, double view_start, double view_end, float track_x, float track_w) {
    return track_x + (float)((ts - view_start) / (view_end - view_start)) * track_w;
}

std::vector<MergedCounterSegment> merge_counter_points(const std::vector<std::pair<double, double>>& points,
                                                       double view_start, double view_end, float track_x,
                                                       float track_w) {
    TRACE_FUNCTION_CAT("timeline");
    std::vector<MergedCounterSegment> result;
    if (points.empty()) return result;

    // Find the first point before or at view_start
    auto it = std::lower_bound(
        points.begin(), points.end(), std::make_pair(view_start, -1e300),
        [](const std::pair<double, double>& a, const std::pair<double, double>& b) { return a.first < b.first; });
    if (it != points.begin()) --it;

    bool have_bucket = false;
    int cur_pixel = 0;
    MergedCounterSegment bucket{};

    auto flush_bucket = [&]() {
        if (have_bucket) result.push_back(bucket);
        have_bucket = false;
    };

    for (; it != points.end(); ++it) {
        bool past_end = it->first > view_end;
        float x = time_to_x(it->first, view_start, view_end, track_x, track_w);
        int pixel = (int)x;

        if (have_bucket && pixel == cur_pixel && !past_end) {
            if (it->second < bucket.min_val) bucket.min_val = it->second;
            if (it->second > bucket.max_val) bucket.max_val = it->second;
            bucket.last_val = it->second;
            bucket.point_count++;
            continue;
        }

        flush_bucket();

        // Start new bucket (including the final past-end point for continuity)
        cur_pixel = pixel;
        bucket = {x, it->second, it->second, it->second, 1};
        have_bucket = true;

        if (past_end) {
            flush_bucket();
            break;
        }
    }

    flush_bucket();
    return result;
}

void CounterTrackRenderer::render_series(ImDrawList* dl, ImVec2 track_min, ImVec2 track_max,
                                         const CounterSeries& series, const ViewState& view, ImU32 color) {
    TRACE_FUNCTION_CAT("timeline");
    if (series.points.empty()) return;

    float track_w = track_max.x - track_min.x;
    float track_h = track_max.y - track_min.y;
    double value_range = series.max_val - series.min_val;
    if (value_range < 1e-9) value_range = 1.0;

    dl->PushClipRect(track_min, track_max, true);

    auto val_to_y = [&](double val) -> float {
        float normalized = (float)((val - series.min_val) / value_range);
        return track_max.y - normalized * (track_h - 4) - 2;
    };

    ImU32 fill_color = (color & 0x00FFFFFF) | 0x70000000;
    auto segments = merge_counter_points(series.points, view.view_start_ts(), view.view_end_ts(), track_min.x, track_w);

    float prev_x = 0.0f;
    float prev_y = 0.0f;
    bool have_prev = false;

    for (const auto& seg : segments) {
        float y_last = val_to_y(seg.last_val);

        if (seg.point_count > 1 && have_prev) {
            // Merged bucket: draw horizontal connector, vertical min/max band, and area fill
            float y_min = val_to_y(seg.max_val);
            float y_max = val_to_y(seg.min_val);
            dl->AddLine(ImVec2(prev_x, prev_y), ImVec2(seg.x, prev_y), color, 1.5f);
            dl->AddRectFilled(ImVec2(seg.x - 0.5f, y_min), ImVec2(seg.x + 0.5f, y_max), color);
            dl->AddRectFilled(ImVec2(prev_x, prev_y), ImVec2(seg.x, track_max.y), fill_color);
            prev_x = seg.x;
            prev_y = y_last;
        } else {
            // Single point or first segment: draw step function
            if (have_prev) {
                dl->AddLine(ImVec2(prev_x, prev_y), ImVec2(seg.x, prev_y), color, 1.5f);
                dl->AddLine(ImVec2(seg.x, prev_y), ImVec2(seg.x, y_last), color, 1.5f);
                dl->AddRectFilled(ImVec2(prev_x, prev_y), ImVec2(seg.x, track_max.y), fill_color);
            }
            prev_x = seg.x;
            prev_y = y_last;
            have_prev = true;
        }
    }

    // Min/max labels
    char buf[64];
    float font_h = ImGui::GetFontSize();
    snprintf(buf, sizeof(buf), "%.1f", series.max_val);
    dl->AddText(ImVec2(track_min.x + 6, track_min.y + 2), IM_COL32(140, 140, 140, 200), buf);
    snprintf(buf, sizeof(buf), "%.1f", series.min_val);
    dl->AddText(ImVec2(track_min.x + 6, track_max.y - font_h - 4), IM_COL32(140, 140, 140, 200), buf);

    dl->PopClipRect();
}

bool counter_lookup_value(const CounterSeries& series, double time, double& out_timestamp, double& out_value) {
    TRACE_FUNCTION_CAT("timeline");
    if (series.points.empty()) return false;

    auto it = std::upper_bound(series.points.begin(), series.points.end(), time,
                               [](double t, const std::pair<double, double>& p) { return t < p.first; });

    // Before first data point — no value to show
    if (it == series.points.begin()) return false;

    --it;
    out_timestamp = it->first;
    out_value = it->second;
    return true;
}

bool CounterTrackRenderer::hit_test(float mouse_x, float mouse_y, const ViewState& view,
                                    CounterHitResult& result) const {
    TRACE_FUNCTION_CAT("timeline");
    for (const auto& layout : layouts_) {
        if (mouse_y < layout.track_min.y || mouse_y >= layout.track_max.y) continue;
        if (mouse_x < layout.track_min.x || mouse_x >= layout.track_max.x) continue;

        const auto& series = *layout.series;
        float track_w = layout.track_max.x - layout.track_min.x;
        double mouse_time = view.x_to_time(mouse_x, layout.track_min.x, track_w);

        double ts, val;
        if (!counter_lookup_value(series, mouse_time, ts, val)) continue;

        result.series = &series;
        result.timestamp = ts;
        result.value = val;
        return true;
    }
    return false;
}
