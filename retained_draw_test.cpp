#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "hailstone_plugin.h"
#include <imgui.h>

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

struct HailstoneArgs {
    std::initializer_list<HailstoneValue> values;
    std::span<const HailstoneValue> fields;
};

struct HailstoneOut {
    HailstoneValue value{};
};

namespace {

HailstoneUiFn draw_column = nullptr;
HailstoneUiFn version = nullptr;
HailstoneUiFn begin_capture = nullptr;
HailstoneUiFn end_capture = nullptr;
HailstoneUiFn begin_replay = nullptr;
HailstoneUiFn end_replay = nullptr;
HailstoneUiFn compile_frame_quads = nullptr;
HailstoneUiFn begin_frame_quads = nullptr;
HailstoneUiFn end_frame_quads = nullptr;
HailstoneUiFn frame_quads_draws = nullptr;

[[noreturn]] void fail(const char* message) {
    std::fprintf(stderr, "%s\n", message);
    std::exit(1);
}

HailstoneValue string(const char* value) { return {HAILSTONE_STRING, 0, 0, value}; }
HailstoneValue integer(std::int64_t value) { return {HAILSTONE_INT, value, 0, nullptr}; }
HailstoneValue number(double value) { return {HAILSTONE_FLOAT, 0, value, nullptr}; }
HailstoneValue boolean(bool value) { return {HAILSTONE_BOOL, value ? 1 : 0, 0, nullptr}; }

HailstoneValue invoke(HailstoneUiFn fn, std::initializer_list<HailstoneValue> values = {},
                      std::span<const HailstoneValue> fields = {}) {
    HailstoneOut out;
    const HailstoneArgs args{values, fields};
    if (fn(nullptr, &args, &out) == 0) return {};
    return out.value;
}

void check_bool(HailstoneValue result, bool expected) {
    if (result.kind != HAILSTONE_BOOL || (result.i != 0) != expected) fail("Wrong callback result");
}

using Color = std::array<double, 4>;

Color mix(Color a, Color b, double amount) {
    for (std::size_t i = 0; i < 4; ++i) a[i] = a[i] * (1 - amount) + b[i] * amount;
    return a;
}

ImU32 pack(Color c) {
    return ImGui::GetColorU32(ImVec4(static_cast<float>(c[0]), static_cast<float>(c[1]),
                                    static_cast<float>(c[2]), static_cast<float>(c[3])));
}

void reference_row(ImDrawList& draw, ImVec2 min, ImVec2 max, Color tint, bool gradient) {
    const Color bg{0.08, 0.09, 0.10, 0.80};
    const Color outline{0.61, 0.72, 0.83, 0.9};
    const Color top = mix(bg, tint, 0.65), bottom = mix(bg, tint, 0.24);
    draw.PushClipRect(min, max, true);
    if (gradient) draw.AddRectFilledMultiColor(min, max, pack(top), pack(top), pack(bottom), pack(bottom));
    else draw.AddRectFilled(min, max, pack(mix(top, bottom, 0.5)));
    draw.AddRect(min, max, pack(mix(outline, tint, 0.32)), 0, 0, 1);
    draw.PopClipRect();
}

void compare_geometry(ImDrawList& draw, int start_v, int split_v, int start_i, int split_i) {
    if (split_v == start_v || split_v - start_v != draw.VtxBuffer.Size - split_v ||
        split_i - start_i != draw.IdxBuffer.Size - split_i) fail("Unexpected draw geometry count");
    for (int offset = 0; offset < split_v - start_v; ++offset) {
        const ImDrawVert& actual = draw.VtxBuffer[start_v + offset];
        const ImDrawVert& expect = draw.VtxBuffer[split_v + offset];
        if (actual.pos.x != expect.pos.x || actual.pos.y != expect.pos.y || actual.col != expect.col ||
            actual.uv.x != expect.uv.x || actual.uv.y != expect.uv.y) fail("Draw vertices differ from reference");
    }
    for (int offset = 0; offset < split_i - start_i; ++offset)
        if (draw.IdxBuffer[start_i + offset] - start_v != draw.IdxBuffer[split_i + offset] - split_v)
            fail("Draw indices differ from reference");
}

constexpr char kLayout[] = "HSH1\n0.08,0.09,0.10,0.80,0.61,0.72,0.83,0.9\n"
                           "0,0,64,32\n2,3,50,6\n2,12,50,6\n2,21,50,6\n";

void check_drawing(bool gradient) {
    ImDrawList& draw = *ImGui::GetWindowDrawList();
    draw.PushClipRect({110, 103}, {157, 125}, true);
    const int start_v = draw.VtxBuffer.Size, start_i = draw.IdxBuffer.Size;
    const int start_clip = draw._ClipRectStack.Size;
    const auto args = {string(kLayout), number(101.25), integer(100), number(1.5), boolean(gradient),
                       integer(750), integer(-1), integer(0x10180FF)};
    check_bool(invoke(draw_column, args), true);
    if (draw._ClipRectStack.Size != start_clip) fail("Native draw leaked a clip");
    const int split_v = draw.VtxBuffer.Size, split_i = draw.IdxBuffer.Size;
    draw.AddRectFilled({101.25f, 100}, {197.25f, 148}, pack({0.08, 0.09, 0.10, 1}));
    reference_row(draw, {104.25f, 104.5f}, {179.25f, 113.5f},
                  mix({0.82, 0.57, 0.16, 1}, {0.24, 0.69, 0.35, 1}, 0.5), gradient);
    reference_row(draw, {104.25f, 118}, {179.25f, 127}, {0.08, 0.09, 0.10, 0.8}, gradient);
    reference_row(draw, {104.25f, 131.5f}, {179.25f, 140.5f}, {1 / 255.0, 128 / 255.0, 1, 1}, gradient);
    compare_geometry(draw, start_v, split_v, start_i, split_i);
    draw.PopClipRect();
}

void check_rejections() {
    ImDrawList& draw = *ImGui::GetWindowDrawList();
    const int start_v = draw.VtxBuffer.Size, start_i = draw.IdxBuffer.Size;
    check_bool(invoke(draw_column, {string(kLayout), integer(0), integer(0), integer(1), boolean(true),
                                   integer(500)}), false);
    check_bool(invoke(draw_column, {string(kLayout), number(std::numeric_limits<double>::infinity()),
                                   integer(0), integer(1), boolean(true), integer(500), integer(500), integer(500)}),
               false);
    check_bool(invoke(draw_column, {string(kLayout), integer(0), integer(0), integer(1), boolean(true),
                                   integer(0), integer(1000), integer(1001)}), false);
    const std::string trailing = std::string(kLayout) + "bad";
    check_bool(invoke(draw_column, {string(trailing.c_str()), integer(0), integer(0), integer(1), boolean(true),
                                   integer(0), integer(1000), integer(500)}), false);
    if (draw.VtxBuffer.Size != start_v || draw.IdxBuffer.Size != start_i) fail("Rejected batch produced draw calls");
}

struct Marker {
    int vertices, indices;
};

Marker marker(const ImDrawList& list) { return {list.VtxBuffer.Size, list.IdxBuffer.Size}; }

bool operator==(const Marker& a, const Marker& b) {
    return a.vertices == b.vertices && a.indices == b.indices;
}

constexpr ImTextureID kAtlas = 73, kImage = 947, kSecondImage = 922;

void probe(ImDrawList& list, ImTextureID texture) {
    list.PushClipRect({0, 0}, {0, 0}, true);
    list.AddImage(texture, {0, 0}, {0, 0});
    list.PopClipRect();
}

void reference_scene(ImDrawList& list) {
    list.AddRectFilled({75, 83}, {170, 161}, IM_COL32(50, 100, 150, 230));
    list.PushClipRect({78, 88}, {173, 165}, true);
    list.AddText({81, 100}, IM_COL32_WHITE, "Captured Label");
    list.AddImage(kImage, {80, 107}, {94, 131}, {0.1f, 0.25f}, {0.6f, 0.8f});
    list.AddImage(kSecondImage, {100, 107}, {114, 131});
    list.AddRect({110, 110}, {119, 117}, IM_COL32_WHITE, 3);
    list.PopClipRect();
}

bool same_vertex(const ImDrawVert& a, const ImDrawVert& b) {
    return a.pos.x == b.pos.x && a.pos.y == b.pos.y && a.uv.x == b.uv.x && a.uv.y == b.uv.y && a.col == b.col;
}

void compare_replay(const ImDrawList& list, Marker source, Marker split, Marker destination,
                    const std::vector<ImDrawVert>& original_verts, const std::vector<ImDrawIdx>& original_indices) {
    if (list.VtxBuffer.Size - destination.vertices != split.vertices - source.vertices ||
        list.IdxBuffer.Size - destination.indices != split.indices - source.indices)
        fail("Retained output geometry size differs");
    for (std::size_t i = 0; i < original_verts.size(); ++i)
        if (!same_vertex(original_verts[i], list.VtxBuffer[destination.vertices + static_cast<int>(i)]))
            fail("Retained output vertex differs");
    for (std::size_t i = 0; i < original_indices.size(); ++i)
        if (original_indices[i] - source.vertices !=
            list.IdxBuffer[destination.indices + static_cast<int>(i)] - destination.vertices)
            fail("Retained output index differs");
}

void scene_window() {
    ImGui::SetNextWindowPos({60, 60});
    ImGui::SetNextWindowSize({300, 275});
    ImGui::Begin("Native UI test window");
}

void check_retained() {
    ImDrawList& list = *ImGui::GetWindowDrawList();
    const Marker source = marker(list);
    check_bool(invoke(begin_capture), true);
    check_bool(invoke(begin_capture), false); // a nested user does not disturb the active capture
    reference_scene(list);
    const auto token = invoke(end_capture);
    if (token.kind != HAILSTONE_INT || token.i <= 0) fail("Drawable scope did not yield a replay plan");
    const Marker split = marker(list);
    const std::vector<ImDrawVert> original_verts(list.VtxBuffer.Data + source.vertices,
                                                 list.VtxBuffer.Data + split.vertices);
    const std::vector<ImDrawIdx> original_indices(list.IdxBuffer.Data + source.indices,
                                                   list.IdxBuffer.Data + split.indices);
    check_bool(invoke(begin_replay, {token}), true);
    probe(list, kImage);
    const Marker stale = marker(list);
    check_bool(invoke(end_replay, {token}), false); // missing second live texture
    if (!(marker(list) == stale)) fail("Stale asset plan produced geometry");
    check_bool(invoke(begin_replay, {token}), true);
    check_bool(invoke(end_replay, {integer(0)}), false); // caller cancels when a live lookup fails
    check_bool(invoke(begin_replay, {token}), true);
    // A real texture command with visible pixels is not a liveness probe.
    list.AddImage(kImage, {80, 107}, {94, 131});
    probe(list, kSecondImage);
    const Marker malformed = marker(list);
    check_bool(invoke(end_replay, {token}), false);
    if (!(marker(list) == malformed)) fail("Malformed probe allowed partial replay");

    ImGui::End();
    ImGui::EndFrame();
    ImGui::NewFrame();
    scene_window();
    ImDrawList& next = *ImGui::GetWindowDrawList();
    check_bool(invoke(begin_replay, {token}), true);
    probe(next, kSecondImage);
    probe(next, kImage);
    const int clips = next._ClipRectStack.Size;
    const std::size_t textures = static_cast<std::size_t>(next._TextureIdStack.Size);
    const Marker destination = marker(next);
    check_bool(invoke(end_replay, {token}), true);
    if (next._ClipRectStack.Size != clips || static_cast<std::size_t>(next._TextureIdStack.Size) != textures)
        fail("Replay leaked a clip or texture scope");
    compare_replay(next, source, split, destination, original_verts, original_indices);

    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.3f);
    check_bool(invoke(begin_replay, {token}), false);
    ImGui::PopStyleVar();
    ImGui::SetWindowPos({61, 60});
    check_bool(invoke(begin_replay, {token}), false);
}

void check_capture_rejections() {
    ImGui::Begin("Foreign native probe window");
    ImGui::End();
    check_bool(invoke(begin_capture), true);
    ImGui::Begin("Foreign native probe window"); // Existing window, so this doesn't increase WindowsActiveCount.
    ImGui::End();
    ImDrawList& list = *ImGui::GetWindowDrawList();
    list.AddRectFilled({75, 83}, {170, 161}, IM_COL32_WHITE);
    if (invoke(end_capture).kind != HAILSTONE_NIL) fail("Foreign Begin/End escaped capture validation");

    check_bool(invoke(begin_capture), true);
    list.AddCallback([](const ImDrawList*, const ImDrawCmd*) {}, nullptr);
    list.AddRectFilled({75, 83}, {170, 161}, IM_COL32_WHITE);
    if (invoke(end_capture).kind != HAILSTONE_NIL) fail("User callback escaped capture validation");
    check_bool(invoke(begin_capture), true);
    if (invoke(end_capture).kind != HAILSTONE_NIL) fail("Empty scope yielded a plan");
}

void check_frame_batch() {
    const HailstoneValue array{HAILSTONE_TABLE, 0, 0, nullptr};
    std::array fields{
        integer(1), number(83.25), number(92.5), number(123.5), number(102.25),
        number(.125), number(.25), number(.375), number(.5),
        integer(2), number(123.5), number(92.5), number(151.75), number(102.25),
        number(0), number(.125), number(.25), number(.375),
    };
    const auto plan = invoke(compile_frame_quads, {array}, fields);
    if (plan.kind != HAILSTONE_INT) fail("Static frame program failed to compile");
    fields[6] = number(std::numeric_limits<double>::infinity());
    if (invoke(compile_frame_quads, {array}, fields).kind != HAILSTONE_NIL) fail("Invalid UV was accepted");
    if (invoke(compile_frame_quads, {array}, std::span(fields).first(13)).kind != HAILSTONE_NIL)
        fail("Incomplete static frame program was accepted");

    ImDrawList& list = *ImGui::GetWindowDrawList();
    check_bool(invoke(begin_capture), true);
    check_bool(invoke(begin_frame_quads, {plan}), false); // no nested companion operation
    if (invoke(end_capture).kind != HAILSTONE_NIL) fail("Empty capture yielded a plan");
    const auto finish = {plan, number(.25), number(.5), number(.75), number(.5)};
    check_bool(invoke(begin_frame_quads, {plan}), true);
    probe(list, kImage);
    const Marker missing = marker(list);
    check_bool(invoke(end_frame_quads, finish), false);
    if (!(marker(list) == missing)) fail("Missing atlas produced a partial frame");
    check_bool(invoke(begin_frame_quads, {plan}), true);
    check_bool(invoke(end_frame_quads, {integer(0)}), false);

    list.PushClipRect({85, 90}, {145, 108}, true);
    check_bool(invoke(begin_frame_quads, {plan}), true);
    probe(list, kSecondImage);
    probe(list, kImage);
    const Marker native_start = marker(list);
    const int clip_count = list._ClipRectStack.Size;
    const int texture_count = list._TextureIdStack.Size;
    check_bool(invoke(end_frame_quads, finish), true);
    if (list._ClipRectStack.Size != clip_count || list._TextureIdStack.Size != texture_count)
        fail("Native frame leaked a clip or texture scope");
    const Marker reference_start = marker(list);
    const ImU32 tint = pack({.25, .5, .75, .5});
    list.AddImage(kSecondImage, {83.25f, 92.5f}, {123.5f, 102.25f}, {.125f, .25f}, {.375f, .5f}, tint);
    list.AddImage(kImage, {123.5f, 92.5f}, {151.75f, 102.25f}, {0.f, .125f}, {.25f, .375f}, tint);
    compare_geometry(list, native_start.vertices, reference_start.vertices, native_start.indices,
                     reference_start.indices);
    list.PopClipRect();
    if (invoke(frame_quads_draws).i != 1) fail("Frame draw counter included a rejected batch");
}

void check_index_limit(bool allow_offsets) {
    ImGui::End();
    ImGui::EndFrame();
    ImGui::NewFrame();
    scene_window();
    ImDrawList& list = *ImGui::GetWindowDrawList();
    if (allow_offsets) list.Flags |= ImDrawListFlags_AllowVtxOffset;
    else list.Flags &= ~ImDrawListFlags_AllowVtxOffset;
    check_bool(invoke(begin_capture), true);
    reference_scene(list);
    const auto token = invoke(end_capture);
    if (token.kind != HAILSTONE_INT) fail("Index limit fixture did not capture");
    while (list.VtxBuffer.Size < 65516)
        list.AddRectFilled({100, 110}, {101, 111}, IM_COL32_WHITE);

    check_bool(invoke(begin_replay, {token}), true);
    probe(list, kImage);
    probe(list, kSecondImage);
    const Marker before = marker(list);
    check_bool(invoke(end_replay, {token}), allow_offsets);
    if (allow_offsets) {
        bool rebased = false;
        for (const ImDrawCmd& cmd : list.CmdBuffer) {
            if (cmd.IdxOffset + cmd.ElemCount <= static_cast<unsigned>(before.indices)) continue;
            rebased = rebased || cmd.VtxOffset > 0;
            const auto last = cmd.IdxOffset + cmd.ElemCount;
            for (auto i = std::max(cmd.IdxOffset, static_cast<unsigned>(before.indices)); i < last; ++i) {
                const auto vertex = cmd.VtxOffset + list.IdxBuffer[static_cast<int>(i)];
                if (vertex < static_cast<unsigned>(before.vertices) ||
                    vertex >= static_cast<unsigned>(list.VtxBuffer.Size)) fail("Rebased replay index out of range");
            }
        }
        if (!rebased || list.VtxBuffer.Size <= 65536) fail("Replay did not exercise 16-bit vertex offset");
    } else if (!(marker(list) == before)) {
        fail("Oversize replay partly emitted without renderer offset support");
    }
}

} // namespace

int main() {
    HMODULE dll = LoadLibraryW(L".\\ui-pack-native.dll");
    if (!dll) fail("Failed to load ui-pack-native.dll");
    const auto set_host = std::bit_cast<void (*)(const PluginHostApi*)>(
        GetProcAddress(dll, "hailstone_plugin_set_host"));
    const auto initialize = std::bit_cast<void (*)()>(GetProcAddress(dll, "InitializePlugin"));
    if (!set_host || !initialize) fail("Missing SDK exports");

    PluginHostApi api{};
    api.abi = HAILSTONE_PLUGIN_ABI;
    api.size = sizeof(api);
    api.imgui_context = [](void*) -> void* { return ImGui::GetCurrentContext(); };
    api.arg_count = [](void*, const HailstoneArgs* args) { return args->values.size(); };
    api.arg = [](void*, const HailstoneArgs* args, std::size_t index, HailstoneValue* out) {
        if (index >= args->values.size()) return 0;
        *out = args->values.begin()[index];
        return 1;
    };
    api.arg_field = [](void*, const HailstoneArgs* args, std::size_t index, const char* key, HailstoneValue* out) {
        if (index != 0) return 0;
        const std::string_view name(key);
        if (name == "count") {
            *out = integer(static_cast<std::int64_t>(args->fields.size()));
            return 1;
        }
        std::size_t field = 0;
        const auto result = std::from_chars(name.data(), name.data() + name.size(), field);
        if (result.ec != std::errc() || field == 0 || field > args->fields.size()) return 0;
        *out = args->fields[field - 1];
        return 1;
    };
    api.out_value = [](void*, HailstoneOut* out, const HailstoneValue* value) { out->value = *value; };
    api.add_ui_function = [](void*, const char* name, HailstoneUiFn fn, void*) {
        if (std::string_view(name) == "HealthColumn") draw_column = fn;
        else if (std::string_view(name) == "Version") version = fn;
        else if (std::string_view(name) == "BeginCapture") begin_capture = fn;
        else if (std::string_view(name) == "EndCapture") end_capture = fn;
        else if (std::string_view(name) == "BeginReplay") begin_replay = fn;
        else if (std::string_view(name) == "EndReplay") end_replay = fn;
        else if (std::string_view(name) == "CompileFrameQuads") compile_frame_quads = fn;
        else if (std::string_view(name) == "BeginFrameQuads") begin_frame_quads = fn;
        else if (std::string_view(name) == "EndFrameQuads") end_frame_quads = fn;
        else if (std::string_view(name) == "FrameQuadsDraws") frame_quads_draws = fn;
        else return 0;
        return 1;
    };
    set_host(&api);
    initialize();
    if (!version || !draw_column || !begin_capture || !end_capture || !begin_replay || !end_replay ||
        !compile_frame_quads || !begin_frame_quads || !end_frame_quads || !frame_quads_draws)
        fail("Plugin did not register all callbacks");
    const HailstoneValue advertised = invoke(version);
    if (advertised.kind != HAILSTONE_INT || advertised.i != 2) fail("Wrong protocol version");
    check_bool(invoke(begin_capture), false);
    check_bool(invoke(draw_column, {string(kLayout), integer(0), integer(0), integer(1), boolean(true),
                                   integer(250), integer(500), integer(750)}), false);

    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = {800, 600};
    io.Fonts->Build();
    io.Fonts->SetTexID(kAtlas);
    ImGui::GetStyle().Alpha = .61f;
    ImGui::NewFrame();
    scene_window();
    check_drawing(true);
    check_drawing(false);
    check_rejections();
    check_frame_batch();
    check_retained();
    check_capture_rejections();
    check_index_limit(false);
    check_index_limit(true);
    ImGui::End();
    ImGui::EndFrame();
    FreeLibrary(dll);
    ImGui::DestroyContext();
    std::puts("Native static frame drawing, capture, cross-frame replay, clipping, and rejection checks passed.");
}
