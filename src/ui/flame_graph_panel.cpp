#include "ui/flame_graph_panel.h"
#include "ui/format_time.h"
#include "ui/string_utils.h"
#include "model/color_palette.h"
#include "tracing.h"
#include "imgui.h"
#include <algorithm>
#include <cstdio>

void FlameGraphPanel::reset() {
    TRACE_FUNCTION_CAT("ui");
    trees_.clear();
    cached_event_count_ = 0;
    cached_has_range_ = false;
    cached_range_start_ = 0.0;
    cached_range_end_ = 0.0;
    cached_hidden_hash_ = SIZE_MAX;
    zoom_root_.clear();
    selected_tree_ = 0;
    thread_filter_[0] = '\0';
    ctx_node_idx_ = UINT32_MAX;
    ctx_tree_idx_ = -1;
}

void FlameGraphPanel::on_model_changed() {
    reset();
}

int32_t FlameGraphPanel::find_longest_instance(const TraceModel& model, uint32_t pid, uint32_t tid, uint32_t name_idx) {
    TRACE_FUNCTION_CAT("ui");
    const auto* thread = model.find_thread(pid, tid);
    if (!thread) return -1;
    int32_t best = -1;
    double best_dur = -1.0;
    for (uint32_t ei : thread->event_indices) {
        const auto& ev = model.events()[ei];
        if (ev.name_idx == name_idx && ev.dur > 0 && ev.dur > best_dur) {
            best_dur = ev.dur;
            best = static_cast<int32_t>(ei);
        }
    }
    return best;
}

// ---------------------------------------------------------------------------
// Cache invalidation
// ---------------------------------------------------------------------------

size_t FlameGraphPanel::compute_hidden_hash(const ViewState& view) const {
    TRACE_FUNCTION_CAT("ui");
    size_t h = view.hidden_pids().size() * 31 + view.hidden_tids().size() * 37 + view.hidden_cats().size() * 41;
    for (uint32_t v : view.hidden_pids()) h ^= std::hash<uint32_t>{}(v) * 0x9e3779b97f4a7c15ULL;
    for (uint32_t v : view.hidden_tids()) h ^= std::hash<uint32_t>{}(v) * 0x517cc1b727220a95ULL;
    for (uint32_t v : view.hidden_cats()) h ^= std::hash<uint32_t>{}(v) * 0x6c62272e07bb0142ULL;
    return h;
}

bool FlameGraphPanel::needs_rebuild(const ViewState& view, size_t event_count) const {
    TRACE_FUNCTION_CAT("ui");
    if (cached_event_count_ != event_count) return true;
    if (cached_has_range_ != view.has_range_selection()) return true;
    if (view.has_range_selection()) {
        if (cached_range_start_ != view.range_start_ts() || cached_range_end_ != view.range_end_ts()) return true;
    }
    if (cached_hidden_hash_ != compute_hidden_hash(view)) return true;
    return false;
}

void FlameGraphPanel::update_cache_keys(const ViewState& view, size_t event_count) {
    TRACE_FUNCTION_CAT("ui");
    cached_event_count_ = event_count;
    cached_has_range_ = view.has_range_selection();
    cached_range_start_ = view.range_start_ts();
    cached_range_end_ = view.range_end_ts();
    cached_hidden_hash_ = compute_hidden_hash(view);
}

// ---------------------------------------------------------------------------
// Tree building (flat node pool, sibling-linked)
// ---------------------------------------------------------------------------

uint32_t FlameGraphPanel::find_or_create_child(FlameTree& tree, uint32_t parent_idx, uint32_t name_idx,
                                               uint32_t cat_idx) {
    for (uint32_t c = tree.nodes[parent_idx].first_child; c != UINT32_MAX; c = tree.nodes[c].next_sibling) {
        if (tree.nodes[c].name_idx == name_idx && tree.nodes[c].cat_idx == cat_idx) return c;
    }
    uint32_t idx = (uint32_t)tree.nodes.size();
    tree.nodes.push_back({});
    auto& n = tree.nodes[idx];
    n.name_idx = name_idx;
    n.cat_idx = cat_idx;
    n.parent = parent_idx;
    n.next_sibling = tree.nodes[parent_idx].first_child;
    tree.nodes[parent_idx].first_child = idx;
    return idx;
}

uint32_t FlameGraphPanel::find_or_create_root(FlameTree& tree, uint32_t name_idx, uint32_t cat_idx) {
    for (uint32_t c = tree.first_root; c != UINT32_MAX; c = tree.nodes[c].next_sibling) {
        if (tree.nodes[c].name_idx == name_idx && tree.nodes[c].cat_idx == cat_idx) return c;
    }
    uint32_t idx = (uint32_t)tree.nodes.size();
    tree.nodes.push_back({});
    auto& n = tree.nodes[idx];
    n.name_idx = name_idx;
    n.cat_idx = cat_idx;
    n.next_sibling = tree.first_root;
    tree.first_root = idx;
    return idx;
}

uint32_t FlameGraphPanel::sort_children(FlameTree& tree, uint32_t first_child) {
    TRACE_FUNCTION_CAT("ui");
    if (first_child == UINT32_MAX) return UINT32_MAX;

    // Collect child indices, sort by total_time descending, re-link.
    std::vector<uint32_t> kids;
    for (uint32_t c = first_child; c != UINT32_MAX; c = tree.nodes[c].next_sibling) {
        kids.push_back(c);
    }
    if (kids.size() <= 1) return first_child;

    std::sort(kids.begin(), kids.end(),
              [&](uint32_t a, uint32_t b) { return tree.nodes[a].total_time > tree.nodes[b].total_time; });
    for (size_t i = 0; i + 1 < kids.size(); i++) {
        tree.nodes[kids[i]].next_sibling = kids[i + 1];
    }
    tree.nodes[kids.back()].next_sibling = UINT32_MAX;
    return kids[0];
}

void FlameGraphPanel::compute_self_times(FlameTree& tree) {
    // Reverse iteration: children are always appended after parents in the pool,
    // so processing in reverse guarantees children are resolved before parents.
    for (int i = (int)tree.nodes.size() - 1; i >= 0; --i) {
        auto& node = tree.nodes[i];
        double children_total = 0.0;
        for (uint32_t c = node.first_child; c != UINT32_MAX; c = tree.nodes[c].next_sibling) {
            children_total += tree.nodes[c].total_time;
        }
        // Nodes with call_count == 0 are pure context parents; their total comes from children.
        if (node.call_count == 0) {
            node.total_time = children_total;
        }
        node.self_time = std::max(0.0, node.total_time - children_total);
    }
}

void FlameGraphPanel::rebuild(const TraceModel& model, const ViewState& view) {
    TRACE_FUNCTION_CAT("ui");
    trees_.clear();

    bool has_range = view.has_range_selection();
    double range_start = view.range_start_ts();
    double range_end = view.range_end_ts();
    const auto& hidden_pids = view.hidden_pids();
    const auto& hidden_tids = view.hidden_tids();
    const auto& hidden_cats = view.hidden_cats();

    std::vector<std::pair<uint32_t, uint32_t>> path;  // scratch: (name_idx, cat_idx)

    for (const auto& proc : model.processes()) {
        if (hidden_pids.count(proc.pid)) continue;

        for (const auto& thread : proc.threads) {
            if (hidden_tids.count(thread.tid)) continue;

            FlameTree tree;
            tree.pid = proc.pid;
            tree.tid = thread.tid;
            tree.thread_name = thread.name;

            for (uint32_t ev_idx : thread.event_indices) {
                const auto& ev = model.events()[ev_idx];
                if (ev.is_end_event) continue;
                if (ev.ph != Phase::Complete && ev.ph != Phase::DurationBegin) continue;
                if (ev.dur <= 0.0) continue;
                if (hidden_cats.count(ev.cat_idx)) continue;

                if (has_range && (ev.end_ts() <= range_start || ev.ts >= range_end)) continue;

                double contribution = ev.dur;
                if (has_range) {
                    contribution = std::min(ev.end_ts(), range_end) - std::max(ev.ts, range_start);
                    if (contribution <= 0.0) continue;
                }

                // Build call-stack path (root first) by walking parent chain.
                path.clear();
                path.push_back({ev.name_idx, ev.cat_idx});
                int32_t p = ev.parent_idx;
                while (p >= 0) {
                    const auto& pev = model.events()[p];
                    if (!pev.is_end_event && !hidden_cats.count(pev.cat_idx)) {
                        path.push_back({pev.name_idx, pev.cat_idx});
                    }
                    p = pev.parent_idx;
                }
                std::reverse(path.begin(), path.end());

                // Merge into flat tree.
                uint32_t cur = find_or_create_root(tree, path[0].first, path[0].second);
                for (size_t i = 1; i < path.size(); i++) {
                    cur = find_or_create_child(tree, cur, path[i].first, path[i].second);
                }
                tree.nodes[cur].total_time += contribution;
                tree.nodes[cur].call_count++;
            }

            if (tree.nodes.empty()) continue;

            compute_self_times(tree);

            // Sort children at every level by total_time descending.
            for (size_t i = 0; i < tree.nodes.size(); i++) {
                tree.nodes[i].first_child = sort_children(tree, tree.nodes[i].first_child);
            }
            tree.first_root = sort_children(tree, tree.first_root);

            tree.root_total_time = 0.0;
            for (uint32_t r = tree.first_root; r != UINT32_MAX; r = tree.nodes[r].next_sibling) {
                tree.root_total_time += tree.nodes[r].total_time;
            }

            if (tree.root_total_time > 0.0) {
                trees_.push_back(std::move(tree));
            }
        }
    }

    std::sort(trees_.begin(), trees_.end(),
              [](const FlameTree& a, const FlameTree& b) { return a.root_total_time > b.root_total_time; });
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

void FlameGraphPanel::render(const TraceModel& model, ViewState& view) {
    TRACE_FUNCTION_CAT("ui");
    ImGui::Begin("Flame Graph");

    if (model.events().empty()) {
        ImGui::TextDisabled("No trace loaded.");
        ImGui::End();
        return;
    }

    if (needs_rebuild(view, model.events().size())) {
        rebuild(model, view);
        update_cache_keys(view, model.events().size());
        zoom_root_.assign(trees_.size(), UINT32_MAX);
        ctx_tree_idx_ = -1;
        if (selected_tree_ >= (int)trees_.size()) selected_tree_ = 0;
    }

    if (trees_.empty()) {
        ImGui::TextDisabled("No duration events to display.");
        ImGui::End();
        return;
    }

    if (view.has_range_selection()) {
        char buf[64];
        format_time(view.range_end_ts() - view.range_start_ts(), buf, sizeof(buf));
        ImGui::TextDisabled("Range: %s", buf);
    }

    // Left sidebar: filterable thread list.
    float avail_w = ImGui::GetContentRegionAvail().x;
    float avail_h = ImGui::GetContentRegionAvail().y;
    sidebar_width_ = std::clamp(sidebar_width_, 100.0f, avail_w - 100.0f);

    ImGui::BeginChild("##ThreadList", ImVec2(sidebar_width_, avail_h), ImGuiChildFlags_Borders);
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##ThreadFilter", "Filter threads...", thread_filter_, sizeof(thread_filter_));

    for (int i = 0; i < (int)trees_.size(); i++) {
        const auto& tree = trees_[i];
        if (thread_filter_[0] != '\0' && !contains_case_insensitive(tree.thread_name, thread_filter_)) continue;

        char time_buf[64];
        format_time(tree.root_total_time, time_buf, sizeof(time_buf));

        bool is_selected = (selected_tree_ == i);
        char label[256];
        snprintf(label, sizeof(label), "%s\n%s", tree.thread_name.c_str(), time_buf);
        if (ImGui::Selectable(label, is_selected, 0, ImVec2(0, ImGui::GetTextLineHeight() * 2 + 4))) {
            selected_tree_ = i;
        }
    }
    ImGui::EndChild();

    // Draggable splitter.
    ImGui::SameLine();
    constexpr float kSplitterW = 6.0f;
    ImVec2 sp_pos = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##Splitter", ImVec2(kSplitterW, avail_h));
    if (ImGui::IsItemActive()) sidebar_width_ += ImGui::GetIO().MouseDelta.x;
    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    ImDrawList* sdl = ImGui::GetWindowDrawList();
    ImU32 splitter_col = ImGui::IsItemActive()    ? IM_COL32(130, 130, 130, 255)
                         : ImGui::IsItemHovered() ? IM_COL32(100, 100, 100, 255)
                                                  : IM_COL32(60, 60, 60, 255);
    float sx = sp_pos.x + kSplitterW * 0.5f;
    sdl->AddLine({sx, sp_pos.y}, {sx, sp_pos.y + avail_h}, splitter_col, 2.0f);

    ImGui::SameLine();

    // Right side: icicle chart.
    ImGui::BeginChild("##IcicleArea", ImVec2(0, avail_h));
    if (selected_tree_ >= 0 && selected_tree_ < (int)trees_.size()) {
        render_icicle(model, view, selected_tree_);
    } else {
        ImGui::TextDisabled("Select a thread.");
    }
    ImGui::EndChild();

    ImGui::End();
}

void FlameGraphPanel::render_icicle(const TraceModel& model, ViewState& view, int tree_idx) {
    TRACE_FUNCTION_CAT("ui");
    const auto& tree = trees_[tree_idx];
    uint32_t zoom = zoom_root_[tree_idx];
    bool zoomed = (zoom != UINT32_MAX);

    double zoom_total = zoomed ? tree.node(zoom).total_time : tree.root_total_time;
    if (zoom_total <= 0.0) return;

    // Breadcrumb bar: walk from zoom node to root.
    if (zoomed) {
        // Build breadcrumb path (root first).
        std::vector<uint32_t> crumbs;
        for (uint32_t n = zoom; n != UINT32_MAX; n = tree.node(n).parent) crumbs.push_back(n);
        std::reverse(crumbs.begin(), crumbs.end());

        if (ImGui::SmallButton("Root")) zoom_root_[tree_idx] = UINT32_MAX;
        for (size_t i = 0; i < crumbs.size(); i++) {
            ImGui::SameLine();
            ImGui::TextUnformatted(">");
            ImGui::SameLine();
            const std::string& name = model.get_string(tree.node(crumbs[i]).name_idx);
            if (i < crumbs.size() - 1) {
                char bid[64];
                snprintf(bid, sizeof(bid), "%s###bc_%zu", name.c_str(), i);
                if (ImGui::SmallButton(bid)) zoom_root_[tree_idx] = crumbs[i];
            } else {
                ImGui::TextUnformatted(name.c_str());
            }
        }
        ImGui::Separator();
    }

    const float BAR_H = view.flame_bar_height(), BAR_GAP = view.flame_bar_gap();

    float canvas_w = ImGui::GetContentRegionAvail().x;
    if (canvas_w < 10.0f) return;

    // BFS render queue. Index-based iteration: push_back may reallocate the
    // vector, but existing indices remain valid.
    struct Entry {
        uint32_t node_idx;
        int depth;
        float x, w;
    };
    std::vector<Entry> queue;
    queue.reserve(256);

    // Seed with children of zoom root (or top-level roots).
    uint32_t seed = zoomed ? tree.node(zoom).first_child : tree.first_root;
    float cx = 0.0f;
    for (uint32_t c = seed; c != UINT32_MAX; c = tree.node(c).next_sibling) {
        float cw = (float)(tree.node(c).total_time / zoom_total) * canvas_w;
        if (cw >= 0.5f) queue.push_back({c, 0, cx, cw});
        cx += cw;
    }

    for (size_t qi = 0; qi < queue.size(); qi++) {
        auto [ni, depth, x_off, x_w] = queue[qi];
        float child_x = x_off;
        for (uint32_t c = tree.node(ni).first_child; c != UINT32_MAX; c = tree.node(c).next_sibling) {
            float cw = (float)(tree.node(c).total_time / zoom_total) * canvas_w;
            if (cw >= 0.5f) queue.push_back({c, depth + 1, child_x, cw});
            child_x += cw;
        }
    }

    int max_depth = 0;
    for (const auto& e : queue) {
        if (e.depth > max_depth) max_depth = e.depth;
    }
    float total_h = (max_depth + 1) * (BAR_H + BAR_GAP);

    ImGui::BeginChild("##FlameCanvas", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
    ImVec2 origin = ImGui::GetCursorScreenPos();
    canvas_w = ImGui::GetContentRegionAvail().x;
    ImGui::Dummy(ImVec2(canvas_w, total_h));

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 mouse = ImGui::GetMousePos();
    bool hoverable = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);
    bool has_search = !view.search_query().empty();

    uint32_t hovered_idx = UINT32_MAX;

    for (const auto& entry : queue) {
        const auto& node = tree.node(entry.node_idx);
        float x0 = origin.x + entry.x;
        float x1 = x0 + entry.w;
        float y = origin.y + entry.depth * (BAR_H + BAR_GAP);

        if (entry.w < 1.0f) continue;

        ImU32 fill = ColorPalette::color_for_event(node.cat_idx, node.name_idx);

        bool matches_search = false;
        if (has_search) {
            matches_search = contains_case_insensitive(model.get_string(node.name_idx), view.search_query());
            if (!matches_search) {
                int r = (fill >> 0) & 0xFF, g = (fill >> 8) & 0xFF, b = (fill >> 16) & 0xFF;
                fill = IM_COL32(r / 3, g / 3, b / 3, 0xFF);
            }
        }

        dl->AddRectFilled({x0, y}, {x1, y + BAR_H}, fill);
        if (matches_search) {
            dl->AddRect({x0, y}, {x1, y + BAR_H}, IM_COL32(255, 220, 50, 255), 0.0f, 0, 2.0f);
        } else if (entry.w > 3.0f) {
            dl->AddRect({x0, y}, {x1, y + BAR_H}, ColorPalette::border_color(fill));
        }

        if (entry.w > 40.0f) {
            const std::string& name = model.get_string(node.name_idx);
            if (!name.empty()) {
                ImU32 tcol = ColorPalette::text_color(fill);
                ImVec2 tsz = ImGui::CalcTextSize(name.c_str());
                float ty = y + (BAR_H - tsz.y) * 0.5f;
                if (tsz.x <= entry.w - 4.0f) {
                    dl->AddText({x0 + (entry.w - tsz.x) * 0.5f, ty}, tcol, name.c_str());
                } else {
                    dl->PushClipRect({x0 + 2, y}, {x1 - 2, y + BAR_H});
                    dl->AddText({x0 + 2, ty}, tcol, name.c_str());
                    dl->PopClipRect();
                }
            }
        }

        float hit_x0 = x0, hit_x1 = x1;
        if (entry.w < 3.0f) {
            float mid = (x0 + x1) * 0.5f;
            hit_x0 = mid - 1.5f;
            hit_x1 = mid + 1.5f;
        }

        if (hoverable && mouse.x >= hit_x0 && mouse.x < hit_x1 && mouse.y >= y && mouse.y < y + BAR_H) {
            hovered_idx = entry.node_idx;
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                int32_t best = find_longest_instance(model, tree.pid, tree.tid, node.name_idx);
                if (best >= 0) view.set_selected_event_idx(best);
            }
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                ctx_node_idx_ = entry.node_idx;
                ctx_tree_idx_ = tree_idx;
                ImGui::OpenPopup("##FlameCtx");
            }
        }
    }

    // Tooltip.
    if (hovered_idx != UINT32_MAX) {
        const auto& node = tree.node(hovered_idx);
        char tbuf[64], sbuf[64];
        format_time(node.total_time, tbuf, sizeof(tbuf));
        format_time(node.self_time, sbuf, sizeof(sbuf));

        ImGui::BeginTooltip();
        ImGui::Text("%s", model.get_string(node.name_idx).c_str());
        const auto& cat = model.get_string(node.cat_idx);
        if (!cat.empty()) ImGui::TextDisabled("Category: %s", cat.c_str());
        ImGui::Separator();
        ImGui::Text("Total: %s (%.1f%%)", tbuf, (float)(node.total_time / zoom_total * 100.0));
        ImGui::Text("Self:  %s", sbuf);
        ImGui::Text("Calls: %u", node.call_count);
        if (node.call_count > 1) {
            char abuf[64];
            format_time(node.total_time / node.call_count, abuf, sizeof(abuf));
            ImGui::Text("Avg:   %s", abuf);
        }
        ImGui::EndTooltip();
    }

    // Context menu — all state is indices, safe across rebuilds.
    if (ImGui::BeginPopup("##FlameCtx")) {
        bool valid = ctx_tree_idx_ == tree_idx && ctx_node_idx_ < (uint32_t)tree.nodes.size();
        if (valid) {
            const auto& ctx = tree.node(ctx_node_idx_);
            ImGui::TextDisabled("%s", model.get_string(ctx.name_idx).c_str());
            ImGui::Separator();

            if (ctx.first_child != UINT32_MAX && ImGui::MenuItem("Zoom In")) {
                zoom_root_[tree_idx] = ctx_node_idx_;
            }
            if (zoomed && ImGui::MenuItem("Zoom Out")) {
                zoom_root_[tree_idx] = tree.node(zoom).parent;
            }
            if (zoomed && ImGui::MenuItem("Reset Zoom")) {
                zoom_root_[tree_idx] = UINT32_MAX;
            }
            if (ImGui::MenuItem("Hide Category")) {
                view.hide_cat(ctx.cat_idx);
            }
            if (ImGui::MenuItem("Show in Instances")) {
                int32_t best = find_longest_instance(model, tree.pid, tree.tid, ctx.name_idx);
                if (best >= 0) view.set_selected_event_idx(best);
            }
        }
        ImGui::EndPopup();
    }

    ImGui::EndChild();
}
