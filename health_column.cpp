#include "hailstone_plugin.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <string_view>

HAILSTONE_PLUGIN

namespace {

constexpr std::size_t kMaxRows = 6;
constexpr std::size_t kMaxLayoutBytes = 2048;
constexpr std::uint32_t kCustomTint = 0x1000000;
constexpr double kCoordinateLimit = 1000000.0;
using Color = std::array<double, 4>;
using Rect = std::array<double, 4>; // x, y, width, height in layout coordinates

struct Layout {
    Color background;
    Color outline;
    Rect mask;
    std::array<Rect, kMaxRows> rows;
    std::size_t row_count = 0;
};

struct CachedLayout {
    std::array<char, kMaxLayoutBytes> bytes;
    std::size_t length = 0;
    Layout layout;
};

std::array<CachedLayout, 16> cached_layouts{};
std::size_t next_layout = 0;

bool read_line(std::string_view& text, std::string_view& line) {
    const auto end = text.find('\n');
    if (end == std::string_view::npos) return false;
    line = text.substr(0, end);
    text.remove_prefix(end + 1);
    return true;
}

template <std::size_t N>
bool read_numbers(std::string_view& text, std::array<double, N>& values) {
    std::string_view line;
    if (!read_line(text, line)) return false;
    for (std::size_t index = 0; index < N; ++index) {
        const auto comma = line.find(',');
        if ((comma == std::string_view::npos) != (index + 1 == N)) return false;
        const auto field = line.substr(0, comma);
        if (field.empty()) return false;
        const char* end = field.data() + field.size();
        const auto parsed = std::from_chars(field.data(), end, values[index]);
        if (parsed.ec != std::errc{} || parsed.ptr != end || !std::isfinite(values[index])) return false;
        if (comma != std::string_view::npos) line.remove_prefix(comma + 1);
    }
    return true;
}

bool read_rect(std::string_view& text, Rect& rect) {
    return read_numbers(text, rect) && std::abs(rect[0]) <= kCoordinateLimit &&
           std::abs(rect[1]) <= kCoordinateLimit &&
           rect[2] > 0 && rect[2] <= kCoordinateLimit && rect[3] > 0 && rect[3] <= kCoordinateLimit;
}

bool parse_layout(std::string_view text, Layout& layout) {
    constexpr std::string_view header = "HSH1\n";
    if (!text.starts_with(header)) return false;
    text.remove_prefix(header.size());
    std::array<double, 8> colors;
    if (!read_numbers(text, colors)) return false;
    for (double channel : colors)
        if (channel < 0 || channel > 1) return false;
    for (std::size_t channel = 0; channel < 4; ++channel) {
        layout.background[channel] = colors[channel];
        layout.outline[channel] = colors[channel + 4];
    }
    if (!read_rect(text, layout.mask)) return false;
    while (!text.empty()) {
        if (layout.row_count == kMaxRows || !read_rect(text, layout.rows[layout.row_count])) return false;
        ++layout.row_count;
    }
    return true;
}

const Layout* find_layout(const char* bytes) {
    // The SDK supplies a NUL-terminated string; impose a small bound before inspecting the payload.
    std::size_t count = 0;
    while (count < kMaxLayoutBytes && bytes[count] != '\0') ++count;
    if (count == 0 || count == kMaxLayoutBytes) return nullptr;
    const std::string_view key(bytes, count);
    for (const auto& entry : cached_layouts)
        if (entry.length == count && std::string_view(entry.bytes.data(), count) == key) return &entry.layout;

    Layout layout{};
    if (!parse_layout(key, layout)) return nullptr;
    auto& entry = cached_layouts[next_layout];
    key.copy(entry.bytes.data(), key.size());
    entry.length = count;
    entry.layout = layout;
    next_layout = (next_layout + 1) % cached_layouts.size();
    return &entry.layout;
}

Color mix(const Color& a, const Color& b, double amount) {
    Color result;
    for (std::size_t i = 0; i < result.size(); ++i)
        result[i] = a[i] * (1 - amount) + b[i] * amount;
    return result;
}

Color status_tint(const Color& background, std::int64_t gauge) {
    constexpr Color low{0.82, 0.22, 0.18, 1}, mid{0.82, 0.57, 0.16, 1}, high{0.24, 0.69, 0.35, 1};
    if (gauge == -1) return background;
    if (gauge >= kCustomTint) {
        const auto rgb = static_cast<std::uint32_t>(gauge);
        return {((rgb >> 16) & 255) / 255.0, ((rgb >> 8) & 255) / 255.0, (rgb & 255) / 255.0, 1};
    }
    const double fraction = static_cast<double>(gauge) / 1000.0;
    return fraction < 0.5 ? mix(low, mid, fraction * 2) : mix(mid, high, (fraction - 0.5) * 2);
}

ImU32 packed(const Color& color) {
    return ImGui::GetColorU32(ImVec4(static_cast<float>(color[0]), static_cast<float>(color[1]),
                                    static_cast<float>(color[2]), static_cast<float>(color[3])));
}

struct ScreenRect {
    ImVec2 min, max;
};

ScreenRect screen_rect(const Rect& rect, double x, double y, double scale) {
    // Preserve the Lua operation order: translate the near corner, then add the scaled extent.
    const double left = x + rect[0] * scale;
    const double top = y + rect[1] * scale;
    return {{static_cast<float>(left), static_cast<float>(top)},
            {static_cast<float>(left + rect[2] * scale), static_cast<float>(top + rect[3] * scale)}};
}

void draw_column(ImDrawList& draw, const Layout& layout, double x, double y, double scale, bool gradient,
                 const std::array<std::int64_t, kMaxRows>& gauges) {
    const ScreenRect mask = screen_rect(layout.mask, x, y, scale);
    const Color opaque{layout.background[0], layout.background[1], layout.background[2], 1};
    draw.AddRectFilled(mask.min, mask.max, packed(opaque));
    for (std::size_t row = 0; row < layout.row_count; ++row) {
        const ScreenRect box = screen_rect(layout.rows[row], x, y, scale);
        const Color tint = status_tint(layout.background, gauges[row]);
        const Color top = mix(layout.background, tint, 0.65);
        const Color bottom = mix(layout.background, tint, 0.24);
        draw.PushClipRect(box.min, box.max, true);
        if (gradient) {
            const ImU32 upper = packed(top), lower = packed(bottom);
            draw.AddRectFilledMultiColor(box.min, box.max, upper, upper, lower, lower);
        } else {
            draw.AddRectFilled(box.min, box.max, packed(mix(top, bottom, 0.5)));
        }
        draw.AddRect(box.min, box.max, packed(mix(layout.outline, tint, 0.32)), 0, 0, 1);
        draw.PopClipRect();
    }
}

bool arg_number(const HailstoneArgs* args, std::size_t index, double& number) {
    HailstoneValue value{};
    if (!host->arg(host->host, args, index, &value)) return false;
    if (value.kind == HAILSTONE_FLOAT) number = value.f;
    else if (value.kind == HAILSTONE_INT) number = static_cast<double>(value.i);
    else return false;
    return std::isfinite(number);
}

struct ScopedContext {
    explicit ScopedContext(ImGuiContext* context) : previous(ImGui::GetCurrentContext()) {
        ImGui::SetCurrentContext(context);
    }
    ~ScopedContext() { ImGui::SetCurrentContext(previous); }
    ImGuiContext* previous;
};

bool try_health_column(const HailstoneArgs* args) {
    const auto count = host->arg_count(host->host, args);
    if (count < 5 || count > 5 + kMaxRows) return false;
    HailstoneValue encoded{}, gradient{};
    if (!host->arg(host->host, args, 0, &encoded) || encoded.kind != HAILSTONE_STRING || !encoded.s ||
        !host->arg(host->host, args, 4, &gradient) || gradient.kind != HAILSTONE_BOOL) return false;
    double x, y, scale;
    if (!arg_number(args, 1, x) || std::abs(x) > kCoordinateLimit ||
        !arg_number(args, 2, y) || std::abs(y) > kCoordinateLimit ||
        !arg_number(args, 3, scale) || scale <= 0 || scale > 100) return false;
    const Layout* layout = find_layout(encoded.s);
    if (!layout || count != 5 + layout->row_count) return false;

    std::array<std::int64_t, kMaxRows> gauges{};
    for (std::size_t row = 0; row < layout->row_count; ++row) {
        HailstoneValue gauge{};
        if (!host->arg(host->host, args, row + 5, &gauge) || gauge.kind != HAILSTONE_INT) return false;
        const auto value = gauge.i;
        const bool normal = value >= -1 && value <= 1000;
        const bool custom = value >= kCustomTint && value < kCustomTint * 2;
        if (!normal && !custom) return false;
        gauges[row] = value;
    }

    auto* context = static_cast<ImGuiContext*>(host->imgui_context(host->host));
    if (!context || !context->WithinFrameScope || !context->CurrentWindow ||
        context->CurrentWindow->IsFallbackWindow) return false;
    const ScopedContext active(context);
    draw_column(*ImGui::GetWindowDrawList(), *layout, x, y, scale, gradient.i != 0, gauges);
    return true;
}

int health_column(void*, const HailstoneArgs* args, HailstoneOut* out) {
    const HailstoneValue result{HAILSTONE_BOOL, try_health_column(args) ? 1 : 0, 0, nullptr};
    host->out_value(host->host, out, &result);
    return 1;
}

int version(void*, const HailstoneArgs*, HailstoneOut* out) {
    const HailstoneValue result{HAILSTONE_INT, 2, 0, nullptr};
    host->out_value(host->host, out, &result);
    return 1;
}

} // namespace

static_assert(IMGUI_VERSION_NUM == 19190, "Use the client's pinned Dear ImGui v1.91.9");

bool register_retained_draw();
bool register_frame_quads();

HAILSTONE_API void InitializePlugin() {
    if (!HAILSTONE_HOST_HAS(host, imgui_context) || !HAILSTONE_HOST_HAS(host, arg_count) ||
        !HAILSTONE_HOST_HAS(host, arg) || !HAILSTONE_HOST_HAS(host, out_value) ||
        !HAILSTONE_HOST_HAS(host, add_ui_function)) return;
    if (register_retained_draw() && host->add_ui_function(host->host, "HealthColumn", health_column, nullptr)) {
        register_frame_quads();
        host->add_ui_function(host->host, "Version", version, nullptr);
    }
}
