#pragma once
// Pure logic for MultiCam.Viewer: no Win32, so it can be unit tested anywhere.
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace viewer {

struct Rect { int x = 0, y = 0, w = 0, h = 0; };
struct Size { int w = 0, h = 0; };
struct Point { int x = 0, y = 0; };
enum class Fit { Fill, Contain };
enum class Hit { Client, Caption, Left, Right, Top, Bottom, TopLeft, TopRight, BottomLeft, BottomRight };
enum class CommandKind { Invalid, Lock, TopMost, Resize, Move, Quit };
struct Command { CommandKind kind = CommandKind::Invalid; int a = 0, b = 0; };

namespace detail {
// value * numerator / denominator, rounding half up, in 64-bit so 4K sizes cannot overflow.
inline int scaled(int value, int numerator, int denominator) {
    return int((static_cast<long long>(value) * numerator + denominator / 2) / denominator);
}
inline int clampInt(int value, int low, int high) { return std::max(low, std::min(high, value)); }
inline long long overlapArea(const Rect& a, const Rect& b) {
    const long long w = std::min(a.x + a.w, b.x + b.w) - std::max(a.x, b.x);
    const long long h = std::min(a.y + a.h, b.y + b.h) - std::max(a.y, b.y);
    return (w > 0 && h > 0) ? w * h : 0;
}
inline std::vector<std::string> tokens(const std::string& line) {
    std::vector<std::string> out; std::string current;
    for (char c : line) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { if (!current.empty()) { out.push_back(current); current.clear(); } }
        else current += c;
    }
    if (!current.empty()) out.push_back(current);
    return out;
}
// Strict integer: optional '-', 1..9 digits, nothing else.
inline bool parseInt(const std::string& text, int& value) {
    size_t i = text[0] == '-' ? 1 : 0;
    if (i >= text.size() || text.size() - i > 9) return false;
    long long result = 0;
    for (; i < text.size(); ++i) {
        if (text[i] < '0' || text[i] > '9') return false;
        result = result * 10 + (text[i] - '0');
    }
    value = int(text[0] == '-' ? -result : result);
    return true;
}
}

// Destination rectangle of a source frame inside a target. Fill covers the target and may
// start at negative coordinates (the overflow is cropped); Contain leaves bars.
inline Rect fitRect(Size source, Size target, Fit fit) {
    if (source.w <= 0 || source.h <= 0 || target.w <= 0 || target.h <= 0) return {0, 0, target.w, target.h};
    const bool widthLimits = static_cast<long long>(target.w) * source.h <= static_cast<long long>(target.h) * source.w;
    const bool byWidth = fit == Fit::Contain ? widthLimits : !widthLimits;
    Rect r;
    if (byWidth) { r.w = target.w; r.h = detail::scaled(source.h, target.w, source.w); }
    else { r.h = target.h; r.w = detail::scaled(source.w, target.h, source.h); }
    r.x = (target.w - r.w) / 2;
    r.y = (target.h - r.h) / 2;
    return r;
}

// Window size that keeps the source aspect ratio. The leading dimension is limited to
// [minimum, maximum]; the other one is derived and, if it leaves its own range, wins.
inline Size keepAspect(Size requested, Size source, bool widthLeads, Size minimum, Size maximum) {
    using detail::clampInt; using detail::scaled;
    if (source.w <= 0 || source.h <= 0)
        return {clampInt(requested.w, minimum.w, maximum.w), clampInt(requested.h, minimum.h, maximum.h)};
    Size r;
    if (widthLeads) {
        r.w = clampInt(requested.w, minimum.w, maximum.w);
        r.h = scaled(source.h, r.w, source.w);
        if (r.h > maximum.h || r.h < minimum.h) { r.h = clampInt(r.h, minimum.h, maximum.h); r.w = scaled(source.w, r.h, source.h); }
    } else {
        r.h = clampInt(requested.h, minimum.h, maximum.h);
        r.w = scaled(source.w, r.h, source.h);
        if (r.w > maximum.w || r.w < minimum.w) { r.w = clampInt(r.w, minimum.w, maximum.w); r.h = scaled(source.h, r.w, source.w); }
    }
    return r;
}

// Moves a window fully inside the monitor it overlaps most (the first monitor when it
// overlaps none). Size never changes; a window larger than the monitor goes to its origin.
inline Rect clampToMonitors(Rect window, const std::vector<Rect>& monitors) {
    if (monitors.empty()) return window;
    size_t best = 0; long long bestArea = 0;
    for (size_t i = 0; i < monitors.size(); ++i) {
        const long long area = detail::overlapArea(window, monitors[i]);
        if (area > bestArea) { bestArea = area; best = i; }
    }
    const Rect& m = monitors[best];
    Rect r = window;
    r.x = window.w >= m.w ? m.x : detail::clampInt(window.x, m.x, m.x + m.w - window.w);
    r.y = window.h >= m.h ? m.y : detail::clampInt(window.y, m.y, m.y + m.h - window.h);
    return r;
}

// What a click at `point` (window coordinates) does. A locked window ignores everything.
inline Hit pipHit(Size window, Point point, bool locked, int border = 6) {
    if (locked) return Hit::Client;
    if (point.x < 0 || point.y < 0 || point.x >= window.w || point.y >= window.h) return Hit::Client;
    const bool left = point.x < border, right = point.x >= window.w - border;
    const bool top = point.y < border, bottom = point.y >= window.h - border;
    if (top && left) return Hit::TopLeft;
    if (top && right) return Hit::TopRight;
    if (bottom && left) return Hit::BottomLeft;
    if (bottom && right) return Hit::BottomRight;
    if (left) return Hit::Left;
    if (right) return Hit::Right;
    if (top) return Hit::Top;
    if (bottom) return Hit::Bottom;
    return Hit::Caption;
}

// One line of the panel-to-viewer protocol. Anything unexpected is Invalid, never an error.
// A UTF-8 BOM at the start is skipped: .NET writes one before the first line when the
// console encoding is UTF-8 (for example with the "UTF-8 worldwide" option of Windows).
inline Command parseCommand(const std::string& line) {
    const bool bom = line.size() >= 3 && line.compare(0, 3, "\xEF\xBB\xBF") == 0;
    const auto parts = detail::tokens(bom ? line.substr(3) : line);
    Command invalid, command;
    if (parts.empty()) return invalid;
    const std::string& name = parts[0];
    if (name == "quit") { if (parts.size() != 1) return invalid; command.kind = CommandKind::Quit; return command; }
    if (name == "lock" || name == "topmost") {
        if (parts.size() != 2 || (parts[1] != "0" && parts[1] != "1")) return invalid;
        command.kind = name == "lock" ? CommandKind::Lock : CommandKind::TopMost;
        command.a = parts[1] == "1" ? 1 : 0;
        return command;
    }
    if (name == "resize" || name == "move") {
        if (parts.size() != 3) return invalid;
        int a = 0, b = 0;
        if (!detail::parseInt(parts[1], a) || !detail::parseInt(parts[2], b)) return invalid;
        if (name == "resize") {
            if (a < 1 || a > 16384 || b < 1 || b > 16384) return invalid;
            command.kind = CommandKind::Resize;
        } else {
            if (a < -1000000 || a > 1000000 || b < -1000000 || b > 1000000) return invalid;
            command.kind = CommandKind::Move;
        }
        command.a = a; command.b = b;
        return command;
    }
    return invalid;
}

inline std::string geometryEvent(Rect window) {
    char buffer[160];
    std::snprintf(buffer, sizeof(buffer), "{\"Event\":\"geometry\",\"X\":%d,\"Y\":%d,\"Width\":%d,\"Height\":%d}",
        window.x, window.y, window.w, window.h);
    return buffer;
}
inline std::string statsEvent(int fps) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "{\"Event\":\"stats\",\"Fps\":%d}", fps);
    return buffer;
}
inline std::string toggleEvent(const char* name, bool value) {
    char buffer[96];
    std::snprintf(buffer, sizeof(buffer), "{\"Event\":\"%s\",\"Value\":%d}", name, value ? 1 : 0);
    return buffer;
}

}
