#include "hailstone_plugin.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace {

constexpr int kMaxVertices = 32768;
constexpr int kMaxIndices = 98304;
constexpr std::size_t kMaxBatches = 512;
constexpr std::size_t kMaxTextures = 128;
constexpr std::size_t kMaxPlans = 16;
constexpr std::size_t kMaxFramePlans = 512;
constexpr std::size_t kMaxFrameQuads = 16;
using Clip = std::array<float, 4>;

struct ScopedContext {
    explicit ScopedContext(ImGuiContext* context) : previous(ImGui::GetCurrentContext()) {
        ImGui::SetCurrentContext(context);
    }
    ~ScopedContext() { ImGui::SetCurrentContext(previous); }
    ImGuiContext* previous;
};

struct AtlasKey {
    ImFontAtlas* atlas = nullptr;
    ImTextureID texture{};
    std::uint64_t metadata = 0;
    bool operator==(const AtlasKey&) const = default;
};

AtlasKey font_atlas_key(ImGuiContext* context, ImFontAtlas* atlas) {
    struct Sample {
        ImGuiContext* context = nullptr;
        int frame = -1;
        AtlasKey key;
    };
    static Sample sample;
    if (sample.context == context && sample.frame == context->FrameCount && sample.key.atlas == atlas &&
        sample.key.texture == atlas->TexID) return sample.key;

    // The atlas is locked during the frame. Compare the live descriptor and metadata, never dereference a saved
    // font after a rebuild. The Lua caller also invalidates plans on a known pack or font change.
    std::uint64_t fingerprint = 14695981039346656037ull;
    const auto add = [&fingerprint](std::uint64_t value) {
        fingerprint = (fingerprint ^ value) * 1099511628211ull;
    };
    add(static_cast<unsigned>(atlas->TexWidth));
    add(static_cast<unsigned>(atlas->TexHeight));
    add(std::bit_cast<std::uint32_t>(atlas->TexUvWhitePixel.x));
    add(std::bit_cast<std::uint32_t>(atlas->TexUvWhitePixel.y));
    add(static_cast<unsigned>(atlas->Fonts.Size));
    for (const ImFont* font : atlas->Fonts) {
        add(reinterpret_cast<std::uintptr_t>(font));
        add(reinterpret_cast<std::uintptr_t>(font->Glyphs.Data));
        add(static_cast<unsigned>(font->Glyphs.Size));
        add(std::bit_cast<std::uint32_t>(font->FontSize));
    }
    for (const ImFontConfig& config : atlas->Sources) {
        add(reinterpret_cast<std::uintptr_t>(config.FontData));
        add(static_cast<unsigned>(config.FontDataSize));
        add(std::bit_cast<std::uint32_t>(config.SizePixels));
        for (char c : config.Name) {
            if (c == '\0') break;
            add(static_cast<unsigned char>(c));
        }
    }
    sample = {context, context->FrameCount, {atlas, atlas->TexID, fingerprint}};
    return sample.key;
}

struct WindowKey {
    ImGuiContext* context;
    ImGuiID id;
    ImDrawList* draw;
    int depth;
    Clip window;
    Clip clip;
    std::array<float, 2> scroll;
    AtlasKey atlas;
    ImFont* font;
    float font_size, alpha, disabled_alpha, curve_tolerance, circle_error;
    ImDrawListFlags draw_flags;
    bool operator==(const WindowKey&) const = default;
};

struct Cursor {
    WindowKey window{};
    int frame, indices, vertices, commands;
};

ImGuiContext* active_context() {
    auto* context = static_cast<ImGuiContext*>(host->imgui_context(host->host));
    if (!context || !context->WithinFrameScope || !context->CurrentWindow ||
        context->CurrentWindow->IsFallbackWindow || !context->Font || !context->Font->ContainerAtlas ||
        !context->Font->ContainerAtlas->IsBuilt()) return nullptr;
    return context;
}

bool finite_clip(const Clip& clip) {
    return std::ranges::all_of(clip, [](float v) { return std::isfinite(v) && std::abs(v) <= 1e8f; });
}

bool read_cursor(ImGuiContext* context, Cursor& cursor) {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 pos = ImGui::GetWindowPos(), size = ImGui::GetWindowSize();
    const ImVec2 low = draw->GetClipRectMin(), high = draw->GetClipRectMax();
    const ImGuiStyle& style = ImGui::GetStyle();
    const WindowKey key{context, context->CurrentWindow->ID, draw, context->CurrentWindowStack.Size,
                        {pos.x, pos.y, size.x, size.y}, {low.x, low.y, high.x, high.y},
                        {ImGui::GetScrollX(), ImGui::GetScrollY()},
                        font_atlas_key(context, context->Font->ContainerAtlas),
                        ImGui::GetFont(), ImGui::GetFontSize(), style.Alpha, style.DisabledAlpha,
                        style.CurveTessellationTol, style.CircleTessellationMaxError, draw->Flags};
    if (!finite_clip(key.window) || !finite_clip(key.clip)) return false;
    cursor = {key, context->FrameCount, draw->IdxBuffer.Size, draw->VtxBuffer.Size, draw->CmdBuffer.Size};
    return true;
}

struct Batch {
    Clip clip;
    std::size_t asset; // kMaxTextures means the live font atlas
    int index, index_count;
    int vertex, vertex_count;
};

struct Plan {
    std::int64_t id = 0;
    WindowKey window{};
    std::vector<ImDrawVert> verts;
    std::vector<ImDrawIdx> indices;
    std::vector<Batch> batches;
    std::vector<ImTextureID> textures;
};

std::array<Plan, kMaxPlans> plans;
Plan capture_scratch;
std::int64_t next_id = 1;

struct FrameQuad {
    std::size_t texture;
    ImVec2 min, max, uv_min, uv_max;
};

struct FramePlan {
    std::int64_t id = 0;
    std::size_t count = 0;
    std::size_t textures = 0;
    std::array<FrameQuad, kMaxFrameQuads> quads{};
};

std::array<FramePlan, kMaxFramePlans> frame_plans;
std::int64_t next_frame_id = 1, frame_draws = 0;

enum class Work { None, Capture, Replay, FrameQuads };
struct Pending {
    Work type = Work::None;
    std::int64_t id = 0;
    Cursor begin{};
    std::uint64_t window_begins = 0;
};
Pending pending;

template <class Visit>
bool for_each_command(const Cursor& begin, const Cursor& end, Visit visit) {
    if (begin.frame != end.frame || begin.window != end.window ||
        end.indices < begin.indices || end.vertices < begin.vertices) return false;
    const ImDrawList& draw = *end.window.draw;
    int expected = begin.indices;
    // ImGui can merge the first new primitive with the command at the capture boundary.
    for (int i = std::max(0, begin.commands - 2); i < draw.CmdBuffer.Size; ++i) {
        const ImDrawCmd& cmd = draw.CmdBuffer[i];
        if (cmd.UserCallback && (i >= begin.commands - 1 || cmd.IdxOffset > static_cast<unsigned>(begin.indices)))
            return false;
        const auto last = static_cast<std::uint64_t>(cmd.IdxOffset) + cmd.ElemCount;
        if (cmd.ElemCount == 0 || last <= static_cast<unsigned>(begin.indices)) continue;
        if (cmd.UserCallback || last > static_cast<unsigned>(end.indices)) return false;
        const int first = std::max(static_cast<int>(cmd.IdxOffset), begin.indices);
        if (first != expected || !visit(draw, cmd, first, static_cast<int>(last))) return false;
        expected = static_cast<int>(last);
    }
    return expected == end.indices;
}

bool snapshot(const Cursor& begin, const Cursor& end, Plan& plan) {
    if (end.indices - begin.indices > kMaxIndices || end.vertices - begin.vertices > kMaxVertices) return false;
    const ImDrawList& draw = *end.window.draw;
    plan.batches.clear();
    plan.textures.clear();
    plan.indices.clear();
    int next_vertex = begin.vertices;
    const bool valid = for_each_command(begin, end, [&](const ImDrawList& list, const ImDrawCmd& cmd,
                                                      int first, int last) {
        const Clip clip{cmd.ClipRect.x, cmd.ClipRect.y, cmd.ClipRect.z, cmd.ClipRect.w};
        if (plan.batches.size() == kMaxBatches || !finite_clip(clip) ||
            clip[0] > clip[2] || clip[1] > clip[3]) return false;
        unsigned least = static_cast<unsigned>(end.vertices), greatest = 0;
        for (int idx = first; idx < last; ++idx) {
            const auto absolute = static_cast<std::uint64_t>(cmd.VtxOffset) + list.IdxBuffer[idx];
            if (absolute < static_cast<unsigned>(next_vertex) || absolute >= static_cast<unsigned>(end.vertices))
                return false;
            least = std::min(least, static_cast<unsigned>(absolute));
            greatest = std::max(greatest, static_cast<unsigned>(absolute));
        }
        if (least != static_cast<unsigned>(next_vertex)) return false;
        const ImTextureID texture = cmd.GetTexID();
        std::size_t asset = kMaxTextures;
        if (texture != begin.window.atlas.texture) {
            const auto found = std::ranges::find(plan.textures, texture);
            asset = static_cast<std::size_t>(found - plan.textures.begin());
            if (found == plan.textures.end()) {
                if (!texture || plan.textures.size() == kMaxTextures) return false;
                plan.textures.push_back(texture);
            }
        }
        plan.batches.push_back({clip, asset, static_cast<int>(plan.indices.size()), last - first,
                                next_vertex - begin.vertices, static_cast<int>(greatest) - next_vertex + 1});
        for (int idx = first; idx < last; ++idx) {
            const unsigned absolute = cmd.VtxOffset + list.IdxBuffer[idx];
            plan.indices.push_back(static_cast<ImDrawIdx>(absolute - least));
        }
        next_vertex = static_cast<int>(greatest) + 1;
        return true;
    });
    if (!valid || plan.batches.empty() || next_vertex != end.vertices) return false;
    for (int i = begin.vertices; i < end.vertices; ++i) {
        const ImDrawVert& v = draw.VtxBuffer[i];
        if (!finite_clip({v.pos.x, v.pos.y, v.uv.x, v.uv.y})) return false;
    }
    plan.verts.assign(draw.VtxBuffer.Data + begin.vertices, draw.VtxBuffer.Data + end.vertices);
    plan.window = begin.window;
    return true;
}

using LiveTextures = std::array<ImTextureID, kMaxTextures>;

bool read_probe_textures(const Cursor& begin, const Cursor& end, LiveTextures& textures, std::size_t& count,
                         std::size_t limit) {
    if (end.indices - begin.indices > static_cast<int>(limit) * 6 ||
        end.vertices - begin.vertices > static_cast<int>(limit) * 4) return false;
    int base = begin.vertices;
    const bool valid = for_each_command(begin, end, [&](const ImDrawList& list, const ImDrawCmd& cmd,
                                                      int first, int last) {
        // The scope must contain only the exact zero-area DrawImage quads emitted by the Lua producer.
        if (cmd.ClipRect.x < cmd.ClipRect.z && cmd.ClipRect.y < cmd.ClipRect.w) return false;
        if (!finite_clip({cmd.ClipRect.x, cmd.ClipRect.y, cmd.ClipRect.z, cmd.ClipRect.w}) ||
            (last - first) % 6 != 0 || !cmd.GetTexID()) return false;
        const int quads = (last - first) / 6;
        for (int quad = 0; quad < quads; ++quad, base += 4) {
            constexpr unsigned topology[] = {0, 1, 2, 0, 2, 3};
            if (base + 4 > end.vertices || count == limit) return false;
            for (int i = 0; i < 6; ++i)
                if (static_cast<std::uint64_t>(cmd.VtxOffset) + list.IdxBuffer[first + quad * 6 + i] !=
                    static_cast<unsigned>(base) + topology[i]) return false;
            const ImVec2 pos = list.VtxBuffer[base].pos;
            for (int i = 1; i < 4; ++i)
                if (list.VtxBuffer[base + i].pos.x != pos.x || list.VtxBuffer[base + i].pos.y != pos.y) return false;
            textures[count++] = cmd.GetTexID();
        }
        return true;
    });
    return valid && base == end.vertices;
}

bool validate_probes(const Cursor& begin, const Cursor& end, const Plan& plan, LiveTextures& live) {
    LiveTextures textures;
    std::size_t count = 0;
    if (!read_probe_textures(begin, end, textures, count, kMaxTextures)) return false;
    for (std::size_t i = 0; i < plan.textures.size(); ++i) {
        const auto found = std::find(textures.begin(), textures.begin() + count, plan.textures[i]);
        if (found == textures.begin() + count) return false;
        live[i] = *found;
    }
    return true;
}

bool replay_fits(const ImDrawList& draw, int vertex_count, int index_count) {
    if (draw.VtxBuffer.Size > std::numeric_limits<int>::max() - vertex_count ||
        draw.IdxBuffer.Size > std::numeric_limits<int>::max() - index_count) return false;
    if constexpr (sizeof(ImDrawIdx) == 2) {
        if ((draw.Flags & ImDrawListFlags_AllowVtxOffset) == 0)
            return draw.VtxBuffer.Size <= 65536 - vertex_count;
    }
    return true;
}

void replay(ImDrawList& draw, const Plan& plan, ImTextureID font_atlas, const LiveTextures& live) {
    for (const Batch& batch : plan.batches) {
        draw.PushClipRect({batch.clip[0], batch.clip[1]}, {batch.clip[2], batch.clip[3]}, false);
        draw.PushTextureID(batch.asset == kMaxTextures ? font_atlas : live[batch.asset]);
        draw.PrimReserve(batch.index_count, batch.vertex_count);
        const auto base = static_cast<unsigned>(draw.VtxBuffer.Size - batch.vertex_count) -
                          draw.CmdBuffer.back().VtxOffset;
        for (int i = 0; i < batch.index_count; ++i)
            draw.PrimWriteIdx(static_cast<ImDrawIdx>(base + plan.indices[batch.index + i]));
        for (int i = 0; i < batch.vertex_count; ++i) {
            const auto& v = plan.verts[batch.vertex + i];
            draw.PrimWriteVtx(v.pos, v.uv, v.col);
        }
        draw.PopTextureID();
        draw.PopClipRect();
    }
}

bool argument_id(const HailstoneArgs* args, std::int64_t& id, std::size_t count = 1) {
    HailstoneValue value{};
    if (host->arg_count(host->host, args) != count || !host->arg(host->host, args, 0, &value) ||
        value.kind != HAILSTONE_INT || value.i <= 0) return false;
    id = value.i;
    return true;
}

Plan& plan_for(std::int64_t id) {
    return plans[static_cast<std::size_t>(id - 1) % plans.size()];
}

FramePlan& frame_plan_for(std::int64_t id) {
    return frame_plans[static_cast<std::size_t>(id - 1) % frame_plans.size()];
}

int put_answer(HailstoneOut* out, int kind, std::int64_t value) {
    const HailstoneValue result{kind, value, 0, nullptr};
    host->out_value(host->host, out, &result);
    return 1;
}

std::uint64_t window_begin_count(const ImGuiContext& context) {
    std::uint64_t count = 0;
    for (const ImGuiWindow* window : context.Windows)
        count += static_cast<unsigned short>(window->BeginCount);
    return count;
}

bool start(Work type, std::int64_t id) {
    ImGuiContext* context = active_context();
    if (!context) return false;
    if (pending.type != Work::None && pending.begin.window.context == context &&
        pending.begin.frame == context->FrameCount) return false;
    pending = {};
    const ScopedContext scope(context);
    Cursor cursor;
    if (!read_cursor(context, cursor)) return false;
    if (type == Work::Replay) {
        const Plan& plan = plan_for(id);
        if (plan.id != id || plan.window != cursor.window) return false;
    }
    if (type == Work::FrameQuads && frame_plan_for(id).id != id) return false;
    pending = {type, id, cursor, type == Work::Capture ? window_begin_count(*context) : 0};
    return true;
}

int begin_capture(void*, const HailstoneArgs* args, HailstoneOut* out) {
    return put_answer(out, HAILSTONE_BOOL,
                      host->arg_count(host->host, args) == 0 && start(Work::Capture, 0));
}

int end_capture(void*, const HailstoneArgs* args, HailstoneOut* out) {
    if (pending.type != Work::Capture || host->arg_count(host->host, args) != 0) return 0;
    const Pending task = pending;
    const Cursor& begin = task.begin;
    pending = {};
    ImGuiContext* context = active_context();
    if (!context) return 0;
    const ScopedContext scope(context);
    Cursor end;
    if (!read_cursor(context, end) || begin.frame != end.frame || begin.window != end.window ||
        end.indices <= begin.indices || end.vertices <= begin.vertices ||
        task.window_begins != window_begin_count(*context) ||
        next_id == std::numeric_limits<std::int64_t>::max()) return 0;
    if (!snapshot(begin, end, capture_scratch)) return 0;
    const auto id = next_id++;
    capture_scratch.id = id;
    std::swap(plan_for(id), capture_scratch);
    return put_answer(out, HAILSTONE_INT, id);
}

int begin_replay(void*, const HailstoneArgs* args, HailstoneOut* out) {
    std::int64_t id;
    return put_answer(out, HAILSTONE_BOOL, argument_id(args, id) && start(Work::Replay, id));
}

bool finish_replay(const HailstoneArgs* args) {
    if (pending.type != Work::Replay) return false;
    const Pending task = pending;
    pending = {};
    std::int64_t id;
    if (!argument_id(args, id) || id != task.id) return false;
    ImGuiContext* context = active_context();
    if (!context) return false;
    const ScopedContext scope(context);
    Cursor end;
    if (!read_cursor(context, end)) return false;
    const Plan& plan = plan_for(id);
    LiveTextures live;
    if (plan.id != id || plan.window != end.window || !validate_probes(task.begin, end, plan, live) ||
        !replay_fits(*end.window.draw, static_cast<int>(plan.verts.size()),
                     static_cast<int>(plan.indices.size()))) return false;
    replay(*end.window.draw, plan, end.window.atlas.texture, live);
    return true;
}

int end_replay(void*, const HailstoneArgs* args, HailstoneOut* out) {
    return put_answer(out, HAILSTONE_BOOL, finish_replay(args));
}

bool number(HailstoneValue value, float& result) {
    if (value.kind != HAILSTONE_INT && value.kind != HAILSTONE_FLOAT) return false;
    const double n = value.kind == HAILSTONE_INT ? static_cast<double>(value.i) : value.f;
    if (!std::isfinite(n) || std::abs(n) > 1e8) return false;
    result = static_cast<float>(n);
    return true;
}

bool frame_field(const HailstoneArgs* args, std::size_t index, HailstoneValue& result) {
    char key[8]{};
    std::to_chars(key, key + sizeof key - 1, index + 1);
    return host->arg_field(host->host, args, 0, key, &result) != 0;
}

bool compile_frame_plan(const HailstoneArgs* args, FramePlan& plan) {
    HailstoneValue array{};
    if (host->arg_count(host->host, args) != 1 || !host->arg(host->host, args, 0, &array) ||
        array.kind != HAILSTONE_TABLE) return false;
    constexpr std::size_t stride = 9;
    HailstoneValue length{};
    if (!host->arg_field(host->host, args, 0, "count", &length) || length.kind != HAILSTONE_INT ||
        length.i <= 0 || length.i > kMaxFrameQuads * stride || length.i % stride != 0) return false;
    const auto count = static_cast<std::size_t>(length.i) / stride;
    std::uint32_t slots = 0;
    for (std::size_t i = 0; i < count; ++i) {
        HailstoneValue value{};
        if (!frame_field(args, i * stride, value)) return false;
        if (value.kind != HAILSTONE_INT || value.i < 1 || value.i > kMaxFrameQuads) return false;
        const auto slot = static_cast<std::size_t>(value.i);
        slots |= std::uint32_t{1} << (slot - 1);
        std::array<float, stride - 1> fields;
        for (std::size_t n = 0; n < fields.size(); ++n) {
            if (!frame_field(args, i * stride + n + 1, value) || !number(value, fields[n])) return false;
        }
        const auto [x0, y0, x1, y1, u0, v0, u1, v1] = fields;
        if (x0 > x1 || y0 > y1 || u0 < 0 || v0 < 0 || u1 > 1 || v1 > 1 || u0 > u1 || v0 > v1)
            return false;
        plan.quads[plan.count++] = {slot - 1, {x0, y0}, {x1, y1}, {u0, v0}, {u1, v1}};
        plan.textures = std::max(plan.textures, slot);
    }
    HailstoneValue extra{};
    return !frame_field(args, count * stride, extra) &&
           slots == (std::uint32_t{1} << plan.textures) - 1;
}

int compile_frame_quads(void*, const HailstoneArgs* args, HailstoneOut* out) {
    FramePlan plan;
    if (next_frame_id == std::numeric_limits<std::int64_t>::max() || !compile_frame_plan(args, plan)) return 0;
    plan.id = next_frame_id++;
    frame_plan_for(plan.id) = plan;
    return put_answer(out, HAILSTONE_INT, plan.id);
}

int begin_frame_quads(void*, const HailstoneArgs* args, HailstoneOut* out) {
    std::int64_t id;
    return put_answer(out, HAILSTONE_BOOL, argument_id(args, id) && start(Work::FrameQuads, id));
}

bool draw_frame_quads(const HailstoneArgs* args) {
    if (pending.type != Work::FrameQuads) return false;
    const Pending task = pending;
    pending = {};
    std::int64_t id;
    if (!argument_id(args, id, 5) || id != task.id) return false;
    std::array<float, 4> tint;
    for (std::size_t i = 0; i < tint.size(); ++i) {
        HailstoneValue value{};
        if (!host->arg(host->host, args, i + 1, &value) || !number(value, tint[i])) return false;
    }
    ImGuiContext* context = active_context();
    if (!context) return false;
    const ScopedContext scope(context);
    Cursor end;
    if (!read_cursor(context, end)) return false;
    const FramePlan& plan = frame_plan_for(id);
    ImDrawList& draw = *end.window.draw;
    LiveTextures textures;
    std::size_t count = 0;
    if (plan.id != id || !read_probe_textures(task.begin, end, textures, count, plan.textures) ||
        count != plan.textures || !replay_fits(draw, static_cast<int>(plan.count) * 4,
                                             static_cast<int>(plan.count) * 6)) return false;

    const ImU32 color = ImGui::GetColorU32(ImVec4(tint[0], tint[1], tint[2], tint[3]));
    for (std::size_t i = 0; i < plan.count; ++i) {
        const FrameQuad& quad = plan.quads[i];
        draw.AddImage(textures[quad.texture], quad.min, quad.max, quad.uv_min, quad.uv_max, color);
    }
    if (frame_draws < std::numeric_limits<std::int64_t>::max()) ++frame_draws;
    return true;
}

int end_frame_quads(void*, const HailstoneArgs* args, HailstoneOut* out) {
    return put_answer(out, HAILSTONE_BOOL, draw_frame_quads(args));
}

int frame_quads_draws(void*, const HailstoneArgs* args, HailstoneOut* out) {
    if (host->arg_count(host->host, args) != 0) return 0;
    return put_answer(out, HAILSTONE_INT, frame_draws);
}

} // namespace

bool register_retained_draw() {
    return host->add_ui_function(host->host, "BeginCapture", begin_capture, nullptr) &&
           host->add_ui_function(host->host, "EndCapture", end_capture, nullptr) &&
           host->add_ui_function(host->host, "BeginReplay", begin_replay, nullptr) &&
           host->add_ui_function(host->host, "EndReplay", end_replay, nullptr);
}

bool register_frame_quads() {
    return HAILSTONE_HOST_HAS(host, arg_field) &&
           host->add_ui_function(host->host, "CompileFrameQuads", compile_frame_quads, nullptr) &&
           host->add_ui_function(host->host, "BeginFrameQuads", begin_frame_quads, nullptr) &&
           host->add_ui_function(host->host, "EndFrameQuads", end_frame_quads, nullptr) &&
           host->add_ui_function(host->host, "FrameQuadsDraws", frame_quads_draws, nullptr);
}
