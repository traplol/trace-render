#include "ui/flamegraph_panel.h"
#include "ui/format_time.h"
#include "model/color_palette.h"
#include "imgui.h"
#include <algorithm>
#include <stack>

void FlameGraphPanel::render(const TraceModel& model, ViewState& view) {
    ImGui::Begin("Flame Graph");

    if (model.events_.empty()) {
        ImGui::TextDisabled("No trace loaded.");
        ImGui::End();
        return;
    }

    bool needs_rebuild = cached_event_count_ != model.events_.size() || cached_has_range_ != view.has_range_selection ||
                         cached_filter_gen_ != view.filter_generation;
    if (view.has_range_selection && !needs_rebuild) {
        needs_rebuild = cached_range_start_ != view.range_start_ts || cached_range_end_ != view.range_end_ts;
    }

    if (needs_rebuild) {
        // Save zoom target for restore after rebuild
        if (zoom_root_ < nodes_.size() && zoom_root_ != root_idx_) {
            zoom_name_idx_ = nodes_[zoom_root_].name_idx;
            zoom_cat_idx_ = nodes_[zoom_root_].cat_idx;
        } else {
            zoom_name_idx_ = UINT32_MAX;
        }

        rebuild(model, view);

        // Restore zoom by (name, cat) if possible
        zoom_stack_.clear();
        zoom_root_ = root_idx_;
        if (zoom_name_idx_ != UINT32_MAX) {
            size_t found = find_node(zoom_name_idx_, zoom_cat_idx_);
            if (found != SIZE_MAX) {
                zoom_stack_.push_back(root_idx_);
                zoom_root_ = found;
            }
        }
    }

    // Controls
    ImGui::Checkbox("Icicle (top-down)", &icicle_mode_);
    ImGui::SameLine();
    if (zoom_stack_.empty()) {
        ImGui::BeginDisabled();
        ImGui::Button("Reset Zoom");
        ImGui::EndDisabled();
    } else {
        if (ImGui::Button("Reset Zoom")) {
            zoom_stack_.clear();
            zoom_root_ = root_idx_;
        }
        ImGui::SameLine();
        if (ImGui::Button("Zoom Out")) {
            zoom_root_ = zoom_stack_.back();
            zoom_stack_.pop_back();
        }
    }
    if (view.has_range_selection) {
        ImGui::SameLine();
        char buf[64];
        format_time(view.range_end_ts - view.range_start_ts, buf, sizeof(buf));
        ImGui::TextDisabled("(range: %s)", buf);
    }

    ImGui::Separator();
    render_bars(model, view);
    ImGui::End();
}

void FlameGraphPanel::rebuild(const TraceModel& model, const ViewState& view) {
    nodes_.clear();
    nodes_.push_back({});  // root sentinel
    nodes_[0].name_idx = UINT32_MAX;
    root_idx_ = 0;

    std::vector<uint32_t> stack;  // reused per event

    for (const auto& proc : model.processes_) {
        if (view.hidden_pids.count(proc.pid)) continue;

        for (const auto& thread : proc.threads) {
            for (uint32_t ev_idx : thread.event_indices) {
                const auto& ev = model.events_[ev_idx];

                if (ev.ph != Phase::Complete && ev.ph != Phase::DurationBegin) continue;
                if (ev.is_end_event || ev.dur <= 0.0) continue;

                if (view.has_range_selection) {
                    if (ev.end_ts() <= view.range_start_ts || ev.ts >= view.range_end_ts) continue;
                }
                if (view.hidden_tids.count(ev.tid)) continue;
                if (view.hidden_cats.count(ev.cat_idx)) continue;

                // Build call stack (root first). Parent events with hidden categories
                // still appear as interior nodes to preserve stack context.
                stack.clear();
                stack.push_back(ev_idx);
                int32_t parent = ev.parent_idx;
                while (parent >= 0) {
                    const auto& pev = model.events_[parent];
                    if (!pev.is_end_event) stack.push_back((uint32_t)parent);
                    parent = pev.parent_idx;
                }
                std::reverse(stack.begin(), stack.end());

                // Merge stack into flame tree
                size_t cur = root_idx_;
                for (size_t i = 0; i < stack.size(); i++) {
                    const auto& sev = model.events_[stack[i]];

                    // Find or create child matching (name, category)
                    size_t child = SIZE_MAX;
                    for (size_t ci : nodes_[cur].children) {
                        if (nodes_[ci].name_idx == sev.name_idx && nodes_[ci].cat_idx == sev.cat_idx) {
                            child = ci;
                            break;
                        }
                    }
                    if (child == SIZE_MAX) {
                        child = nodes_.size();
                        nodes_.push_back({});
                        nodes_[child].name_idx = sev.name_idx;
                        nodes_[child].cat_idx = sev.cat_idx;
                        nodes_[cur].children.push_back(child);
                    }

                    // Accumulate time only at leaf (the event itself)
                    if (i == stack.size() - 1) {
                        double dur = ev.dur;
                        if (view.has_range_selection) {
                            dur = std::max(
                                0.0, std::min(ev.end_ts(), view.range_end_ts) - std::max(ev.ts, view.range_start_ts));
                        }
                        nodes_[child].total_time += dur;
                        nodes_[child].call_count++;
                    }
                    cur = child;
                }
            }
        }
    }

    // Bottom-up pass: propagate total_time and compute self_time.
    // Leaf nodes already have total_time from accumulation above.
    // Interior nodes with call_count > 0 were also leaves in some stacks —
    // their total_time already includes children (it's the event duration).
    // Interior nodes with call_count == 0 are pure context parents —
    // their total_time comes entirely from children.
    std::vector<size_t> order;
    order.reserve(nodes_.size());
    {
        std::stack<size_t> work;
        work.push(root_idx_);
        while (!work.empty()) {
            size_t n = work.top();
            work.pop();
            order.push_back(n);
            for (size_t c : nodes_[n].children) work.push(c);
        }
    }
    for (auto it = order.rbegin(); it != order.rend(); ++it) {
        auto& node = nodes_[*it];
        if (node.children.empty()) {
            node.self_time = node.total_time;
            continue;
        }
        double children_total = 0.0;
        for (size_t c : node.children) children_total += nodes_[c].total_time;
        if (node.call_count == 0) {
            node.total_time = children_total;
            for (size_t c : node.children) node.call_count += nodes_[c].call_count;
        }
        node.self_time = std::max(0.0, node.total_time - children_total);
    }

    // Root total
    double root_total = 0.0;
    for (size_t c : nodes_[root_idx_].children) root_total += nodes_[c].total_time;
    nodes_[root_idx_].total_time = root_total;

    // Sort children by total_time descending
    sort_node(root_idx_);

    cached_event_count_ = model.events_.size();
    cached_has_range_ = view.has_range_selection;
    cached_range_start_ = view.range_start_ts;
    cached_range_end_ = view.range_end_ts;
    cached_filter_gen_ = view.filter_generation;
}

void FlameGraphPanel::sort_node(size_t idx) {
    auto& ch = nodes_[idx].children;
    std::sort(ch.begin(), ch.end(), [this](size_t a, size_t b) { return nodes_[a].total_time > nodes_[b].total_time; });
    for (size_t c : ch) sort_node(c);
}

size_t FlameGraphPanel::find_node(uint32_t name_idx, uint32_t cat_idx) const {
    std::stack<size_t> work;
    for (size_t c : nodes_[root_idx_].children) work.push(c);
    while (!work.empty()) {
        size_t n = work.top();
        work.pop();
        if (nodes_[n].name_idx == name_idx && nodes_[n].cat_idx == cat_idx) return n;
        for (size_t c : nodes_[n].children) work.push(c);
    }
    return SIZE_MAX;
}

void FlameGraphPanel::render_bars(const TraceModel& model, ViewState& view) {
    if (nodes_.empty()) return;

    constexpr float BAR_H = 20.0f, BAR_GAP = 1.0f, MIN_W = 1.0f;
    float canvas_w = ImGui::GetContentRegionAvail().x;
    float canvas_h = ImGui::GetContentRegionAvail().y;
    if (canvas_w < 10.0f || canvas_h < BAR_H) return;

    double root_time = nodes_[zoom_root_].total_time;
    if (root_time <= 0.0) {
        ImGui::TextDisabled("No duration events in range.");
        return;
    }

    // Max depth from zoom root
    int max_depth = 0;
    {
        std::stack<std::pair<size_t, int>> work;
        work.push({zoom_root_, 0});
        while (!work.empty()) {
            auto [n, d] = work.top();
            work.pop();
            if (d > max_depth) max_depth = d;
            for (size_t c : nodes_[n].children) work.push({c, d + 1});
        }
    }

    float total_h = (max_depth + 1) * (BAR_H + BAR_GAP);

    ImGui::BeginChild("##FlameCanvas", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
    bool hoverable = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);
    ImVec2 origin = ImGui::GetCursorScreenPos();
    canvas_w = ImGui::GetContentRegionAvail().x;
    ImGui::Dummy(ImVec2(canvas_w, total_h));

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 mouse = ImGui::GetMousePos();
    size_t hovered = SIZE_MAX;

    struct Entry {
        size_t node;
        int depth;
        float x, w;
    };
    std::vector<Entry> queue;
    queue.reserve(nodes_.size());
    queue.push_back({zoom_root_, 0, 0.0f, canvas_w});

    for (size_t qi = 0; qi < queue.size(); qi++) {
        auto [ni, depth, x_off, x_w] = queue[qi];
        const auto& node = nodes_[ni];
        bool is_sentinel = (ni == zoom_root_ && node.name_idx == UINT32_MAX);

        float y =
            icicle_mode_ ? origin.y + depth * (BAR_H + BAR_GAP) : origin.y + total_h - (depth + 1) * (BAR_H + BAR_GAP);

        if (!is_sentinel && x_w >= MIN_W) {
            float x0 = origin.x + x_off, x1 = x0 + x_w;
            ImU32 fill = ColorPalette::color_for_event(node.cat_idx, node.name_idx);

            dl->AddRectFilled({x0, y}, {x1, y + BAR_H}, fill);
            dl->AddRect({x0, y}, {x1, y + BAR_H}, ColorPalette::border_color(fill));

            const std::string& name = model.get_string(node.name_idx);
            if (x_w > 40.0f && !name.empty()) {
                ImU32 tcol = ColorPalette::text_color(fill);
                ImVec2 tsz = ImGui::CalcTextSize(name.c_str());
                float ty = y + (BAR_H - tsz.y) * 0.5f;
                if (tsz.x <= x_w - 4.0f) {
                    dl->AddText({x0 + (x_w - tsz.x) * 0.5f, ty}, tcol, name.c_str());
                } else {
                    dl->PushClipRect({x0 + 2, y}, {x1 - 2, y + BAR_H});
                    dl->AddText({x0 + 2, ty}, tcol, name.c_str());
                    dl->PopClipRect();
                }
            }

            if (hoverable && mouse.x >= x0 && mouse.x < x1 && mouse.y >= y && mouse.y < y + BAR_H) {
                hovered = ni;
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !node.children.empty()) {
                    zoom_stack_.push_back(zoom_root_);
                    zoom_root_ = ni;
                }
            }
        }

        // Enqueue children
        float cx = is_sentinel ? 0.0f : x_off;
        for (size_t ci : node.children) {
            float cw = (float)(nodes_[ci].total_time / root_time) * canvas_w;
            if (cw >= 0.5f) queue.push_back({ci, depth + 1, cx, cw});
            cx += cw;
        }
    }

    if (hovered != SIZE_MAX) {
        const auto& node = nodes_[hovered];
        char tbuf[64], sbuf[64];
        format_time(node.total_time, tbuf, sizeof(tbuf));
        format_time(node.self_time, sbuf, sizeof(sbuf));
        float pct = (float)(node.total_time / root_time * 100.0);

        ImGui::BeginTooltip();
        ImGui::Text("%s", model.get_string(node.name_idx).c_str());
        const auto& cat = model.get_string(node.cat_idx);
        if (!cat.empty()) ImGui::TextDisabled("Category: %s", cat.c_str());
        ImGui::Separator();
        ImGui::Text("Total: %s (%.1f%%)", tbuf, pct);
        ImGui::Text("Self:  %s", sbuf);
        ImGui::Text("Calls: %u", node.call_count);
        if (node.call_count > 1) {
            char abuf[64];
            format_time(node.total_time / node.call_count, abuf, sizeof(abuf));
            ImGui::Text("Avg:   %s", abuf);
        }
        ImGui::TextDisabled("Click to zoom in");
        ImGui::EndTooltip();
    }

    ImGui::EndChild();
}
