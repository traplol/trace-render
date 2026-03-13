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

    for (const auto& series : model.counter_series_) {
        if (series.pid != pid) continue;
        if (series.points.empty()) continue;

        float track_h = view.counter_track_height;
        ImVec2 track_min(area_min.x + view.label_width, y_offset + total_height);
        ImVec2 track_max(area_min.x + width, y_offset + total_height + track_h);

        // Label
        dl->AddRectFilled(ImVec2(area_min.x, track_min.y), ImVec2(area_min.x + view.label_width, track_max.y),
                          IM_COL32(35, 35, 40, 255));
        dl->PushClipRect(ImVec2(area_min.x, track_min.y), ImVec2(area_min.x + view.label_width - 5, track_max.y), true);
        dl->AddText(ImVec2(area_min.x + 10, track_min.y + 2), IM_COL32(160, 180, 200, 255), series.name.c_str());
        dl->PopClipRect();

        // Background
        dl->AddRectFilled(track_min, track_max, IM_COL32(25, 25, 30, 255));
        dl->AddRect(track_min, track_max, IM_COL32(50, 50, 50, 255));

        ImU32 color = ColorPalette::COLORS[color_idx % ColorPalette::NUM_COLORS];
        render_series(dl, track_min, track_max, series, view, color);

        total_height += track_h + view.track_padding;
        color_idx++;
    }

    return total_height;
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

    // Find the first point before or at view_start
    auto it = std::lower_bound(
        series.points.begin(), series.points.end(), std::make_pair(view.view_start_ts, -1e300),
        [](const std::pair<double, double>& a, const std::pair<double, double>& b) { return a.first < b.first; });
    if (it != series.points.begin()) --it;

    auto val_to_y = [&](double val) -> float {
        float normalized = (float)((val - series.min_val) / value_range);
        return track_max.y - normalized * (track_h - 4) - 2;
    };

    // Draw area fill and line
    ImVec2 prev_pt;
    bool have_prev = false;

    for (; it != series.points.end(); ++it) {
        if (it->first > view.view_end_ts) {
            // Draw one more point for continuity
            float x = view.time_to_x((double)it->first, track_min.x, track_w);
            float y = val_to_y(it->second);
            if (have_prev) {
                // Step function: horizontal then vertical
                ImVec2 step_pt(x, prev_pt.y);
                dl->AddLine(prev_pt, step_pt, color, 3.0f);
                dl->AddLine(step_pt, ImVec2(x, y), color, 3.0f);
            }
            break;
        }

        float x = view.time_to_x((double)it->first, track_min.x, track_w);
        float y = val_to_y(it->second);

        if (have_prev) {
            // Step function
            ImVec2 step_pt(x, prev_pt.y);
            dl->AddLine(prev_pt, step_pt, color, 3.0f);
            dl->AddLine(step_pt, ImVec2(x, y), color, 3.0f);

            // Area fill (semi-transparent)
            ImU32 fill_color = (color & 0x00FFFFFF) | 0x70000000;
            ImVec2 quad[4] = {
                prev_pt,
                step_pt,
                ImVec2(x, track_max.y),
                ImVec2(prev_pt.x, track_max.y),
            };
            dl->AddQuadFilled(quad[0], quad[1], quad[2], quad[3], fill_color);
        }

        prev_pt = ImVec2(x, y);
        have_prev = true;
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
