#include "../native/viewer_core.hpp"
#include <cstdio>
#include <string>

using namespace viewer;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { ++failures; std::printf("FAIL line %d: %s\n", __LINE__, #cond); } } while (0)

static bool eq(Rect a, Rect b) { return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h; }
static bool eq(Size a, Size b) { return a.w == b.w && a.h == b.h; }
static bool is(const Command& c, CommandKind kind, int a = 0, int b = 0) { return c.kind == kind && c.a == a && c.b == b; }

static void fit_contain() { CHECK(eq(fitRect({1920, 1080}, {1000, 1000}, Fit::Contain), Rect{0, 218, 1000, 563})); }
static void fit_fill() { CHECK(eq(fitRect({1920, 1080}, {1000, 1000}, Fit::Fill), Rect{-389, 0, 1778, 1000})); }
static void fit_same_aspect() { CHECK(eq(fitRect({1280, 720}, {640, 360}, Fit::Contain), Rect{0, 0, 640, 360})); }

static void aspect_width_leads() {
    const Size src{1920, 1080}, lo{160, 90}, hi{3840, 2160};
    CHECK(eq(keepAspect({700, 300}, src, true, lo, hi), Size{700, 394}));
    CHECK(eq(keepAspect({700, 300}, src, false, lo, hi), Size{533, 300}));
}
static void aspect_limits() {
    const Size src{1920, 1080}, lo{160, 90}, hi{3840, 2160};
    CHECK(eq(keepAspect({50, 10}, src, true, lo, hi), Size{160, 90}));
    CHECK(eq(keepAspect({5000, 5000}, src, true, lo, hi), Size{3840, 2160}));
}

static void clamp_offscreen() {
    std::vector<Rect> monitors{{0, 0, 1920, 1080}};
    CHECK(eq(clampToMonitors({5000, 5000, 640, 360}, monitors), Rect{1280, 720, 640, 360}));
}
static void clamp_two_monitors() {
    std::vector<Rect> monitors{{0, 0, 1920, 1080}, {1920, 0, 1280, 1024}};
    CHECK(eq(clampToMonitors({1800, 100, 640, 360}, monitors), Rect{1920, 100, 640, 360}));
}
static void clamp_no_monitors() {
    CHECK(eq(clampToMonitors({-50, 70, 640, 360}, {}), Rect{-50, 70, 640, 360}));
}

static void hit_zones() {
    const Size w{640, 360};
    CHECK(pipHit(w, {0, 0}, false) == Hit::TopLeft);
    CHECK(pipHit(w, {639, 0}, false) == Hit::TopRight);
    CHECK(pipHit(w, {0, 359}, false) == Hit::BottomLeft);
    CHECK(pipHit(w, {639, 359}, false) == Hit::BottomRight);
    CHECK(pipHit(w, {320, 0}, false) == Hit::Top);
    CHECK(pipHit(w, {320, 359}, false) == Hit::Bottom);
    CHECK(pipHit(w, {0, 180}, false) == Hit::Left);
    CHECK(pipHit(w, {639, 180}, false) == Hit::Right);
    CHECK(pipHit(w, {320, 180}, false) == Hit::Caption);
    const Point points[] = {{0, 0}, {639, 0}, {0, 359}, {639, 359}, {320, 0}, {320, 359}, {0, 180}, {639, 180}, {320, 180}};
    for (const Point& p : points) CHECK(pipHit(w, p, true) == Hit::Client);
    CHECK(pipHit(w, {-5, 10}, false) == Hit::Client);
    CHECK(pipHit(w, {700, 10}, false) == Hit::Client);
}

static void cmd_valid() {
    CHECK(is(parseCommand("lock 1"), CommandKind::Lock, 1));
    CHECK(is(parseCommand("lock 0"), CommandKind::Lock, 0));
    CHECK(is(parseCommand("topmost 1"), CommandKind::TopMost, 1));
    CHECK(is(parseCommand("resize 640 360"), CommandKind::Resize, 640, 360));
    CHECK(is(parseCommand("move -100 50"), CommandKind::Move, -100, 50));
    CHECK(is(parseCommand("quit"), CommandKind::Quit));
    CHECK(is(parseCommand("  lock   1  \r"), CommandKind::Lock, 1));
}
static void cmd_invalid() {
    const char* lines[] = {"", "lock", "lock 2", "lock 1 extra", "LOCK 1", "resize 0 360", "resize 99999 10", "resize a b", "%%%", "move 1"};
    for (const char* line : lines) CHECK(parseCommand(line).kind == CommandKind::Invalid);
}

static void events() {
    CHECK(geometryEvent({10, 20, 640, 360}) == "{\"Event\":\"geometry\",\"X\":10,\"Y\":20,\"Width\":640,\"Height\":360}");
    CHECK(statsEvent(60) == "{\"Event\":\"stats\",\"Fps\":60}");
    CHECK(toggleEvent("lock", true) == "{\"Event\":\"lock\",\"Value\":1}");
}

int main() {
    fit_contain(); fit_fill(); fit_same_aspect();
    aspect_width_leads(); aspect_limits();
    clamp_offscreen(); clamp_two_monitors(); clamp_no_monitors();
    hit_zones(); cmd_valid(); cmd_invalid(); events();
    if (failures == 0) std::printf("PASS\n");
    return failures;
}
