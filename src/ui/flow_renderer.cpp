#include "flow_renderer.h"
#include <cmath>

void FlowRenderer::render(ImDrawList* dl, const TraceModel& model, const ViewState& view, ImVec2 area_min,
                          ImVec2 area_max, float label_width) {
    if (!view.show_flows || track_positions_.empty()) return;

    float track_left = area_min.x + label_width;
    float track_width = (area_max.x - area_min.x) - label_width;
    ImU32 flow_color = IM_COL32(255, 180, 50, 160);

    float ruler_height = view.ruler_height;
    dl->PushClipRect(ImVec2(track_left, area_min.y + ruler_height), area_max, true);

    for (const auto& [id, indices] : model.flow_groups_) {
        if (indices.size() < 2) continue;

        for (size_t i = 0; i + 1 < indices.size(); i++) {
            const auto& ev1 = model.events_[indices[i]];
            const auto& ev2 = model.events_[indices[i + 1]];

            // Check if either endpoint is in the visible time range
            if (ev1.ts > view.view_end_ts && ev2.ts > view.view_end_ts) continue;
            if (ev1.end_ts() < view.view_start_ts && ev2.end_ts() < view.view_start_ts) continue;

            // Source: right edge of ev1
            float x1 = view.time_to_x(ev1.end_ts(), track_left, track_width);
            // Target: left edge of ev2
            float x2 = view.time_to_x(ev2.ts, track_left, track_width);

            // Look up Y positions from track layout
            auto it1 = track_positions_.find(make_key(ev1.pid, ev1.tid));
            auto it2 = track_positions_.find(make_key(ev2.pid, ev2.tid));
            if (it1 == track_positions_.end() || it2 == track_positions_.end()) continue;

            float y1 = it1->second.y_start + ev1.depth * view.track_height + view.track_height / 2;
            float y2 = it2->second.y_start + ev2.depth * view.track_height + view.track_height / 2;

            // Draw bezier arrow
            float dx = std::abs(x2 - x1);
            float ctrl_dx = std::max(dx * 0.3f, 30.0f);
            ImVec2 p1(x1, y1);
            ImVec2 p2(x2, y2);
            ImVec2 cp1(x1 + ctrl_dx, y1);
            ImVec2 cp2(x2 - ctrl_dx, y2);

            dl->AddBezierCubic(p1, cp1, cp2, p2, flow_color, 2.0f);

            // Arrowhead at p2
            float arrow_size = 9.0f;
            float angle = std::atan2(p2.y - cp2.y, p2.x - cp2.x);
            ImVec2 a1(p2.x - arrow_size * std::cos(angle - 0.4f), p2.y - arrow_size * std::sin(angle - 0.4f));
            ImVec2 a2(p2.x - arrow_size * std::cos(angle + 0.4f), p2.y - arrow_size * std::sin(angle + 0.4f));
            dl->AddTriangleFilled(p2, a1, a2, flow_color);
        }
    }

    dl->PopClipRect();
}
