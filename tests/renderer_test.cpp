#include "../native/renderer.hpp"
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using viewer::Fit;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { ++failures; std::printf("FAIL line %d: %s\n", __LINE__, #cond); } } while (0)

struct Pixel { int b, g, r; };
static Pixel at(const std::vector<uint8_t>& bgra, int width, int x, int y) {
    const uint8_t* p = &bgra[(size_t(y) * width + x) * 4];
    return {p[0], p[1], p[2]};
}
static bool closeTo(Pixel p, int r, int g, int b, int tolerance = 6) {
    return std::abs(p.r - r) <= tolerance && std::abs(p.g - g) <= tolerance && std::abs(p.b - b) <= tolerance;
}
// Solid YUY2 frame: every macropixel is Y U Y V.
static std::vector<uint8_t> solid(int w, int h, uint8_t y, uint8_t u, uint8_t v) {
    std::vector<uint8_t> frame(size_t(w) * h * 2);
    for (size_t i = 0; i < frame.size(); i += 4) { frame[i] = y; frame[i + 1] = u; frame[i + 2] = y; frame[i + 3] = v; }
    return frame;
}
static bool makeRenderer(Renderer& r) {
    std::wstring error;
    if (!r.init(nullptr, error)) { std::printf("init failed: %ls\n", error.c_str()); return false; }
    r.resize(100, 100);
    return true;
}
static bool drawAndRead(Renderer& r, Fit fit, std::vector<uint8_t>& bgra) {
    int w = 0, h = 0;
    r.draw(fit);
    return r.readback(bgra, w, h) && w == 100 && h == 100;
}

static void solid_black() {
    Renderer r; std::vector<uint8_t> out; if (!makeRenderer(r)) { ++failures; return; }
    auto f = solid(4, 2, 16, 128, 128);
    CHECK(r.upload(f.data(), 4, 2)); CHECK(drawAndRead(r, Fit::Fill, out));
    CHECK(closeTo(at(out, 100, 50, 50), 0, 0, 0));
}
static void solid_white() {
    Renderer r; std::vector<uint8_t> out; if (!makeRenderer(r)) { ++failures; return; }
    auto f = solid(4, 2, 235, 128, 128);
    CHECK(r.upload(f.data(), 4, 2)); CHECK(drawAndRead(r, Fit::Fill, out));
    CHECK(closeTo(at(out, 100, 50, 50), 255, 255, 255));
}
static void red_601() {
    Renderer r; std::vector<uint8_t> out; if (!makeRenderer(r)) { ++failures; return; }
    auto f = solid(4, 2, 82, 90, 240);
    CHECK(r.upload(f.data(), 4, 2)); CHECK(drawAndRead(r, Fit::Fill, out));
    CHECK(closeTo(at(out, 100, 50, 50), 255, 0, 0));
}
static void red_709() {
    Renderer r; std::vector<uint8_t> out; if (!makeRenderer(r)) { ++failures; return; }
    auto f = solid(1280, 720, 63, 102, 240);
    CHECK(r.upload(f.data(), 1280, 720)); CHECK(drawAndRead(r, Fit::Fill, out));
    CHECK(closeTo(at(out, 100, 50, 50), 255, 0, 0));
}
static void contain_letterbox() {
    Renderer r; std::vector<uint8_t> out; if (!makeRenderer(r)) { ++failures; return; }
    auto f = solid(4, 2, 82, 90, 240);
    CHECK(r.upload(f.data(), 4, 2)); CHECK(drawAndRead(r, Fit::Contain, out));
    CHECK(closeTo(at(out, 100, 50, 50), 255, 0, 0));
    CHECK(closeTo(at(out, 100, 50, 5), 0, 0, 0));
}
static void fill_covers() {
    Renderer r; std::vector<uint8_t> out; if (!makeRenderer(r)) { ++failures; return; }
    auto f = solid(4, 2, 82, 90, 240);
    CHECK(r.upload(f.data(), 4, 2)); CHECK(drawAndRead(r, Fit::Fill, out));
    CHECK(closeTo(at(out, 100, 5, 5), 255, 0, 0));
    CHECK(closeTo(at(out, 100, 50, 50), 255, 0, 0));
}
static void size_change() {
    Renderer r; std::vector<uint8_t> out; if (!makeRenderer(r)) { ++failures; return; }
    auto tiny = solid(4, 2, 82, 90, 240), big = solid(1280, 720, 63, 102, 240);
    CHECK(r.upload(tiny.data(), 4, 2));
    CHECK(r.upload(big.data(), 1280, 720));
    CHECK(r.upload(tiny.data(), 4, 2));
    CHECK(drawAndRead(r, Fit::Fill, out));
    CHECK(closeTo(at(out, 100, 50, 50), 255, 0, 0));
}
static void no_frame_is_black() {
    Renderer r; std::vector<uint8_t> out; if (!makeRenderer(r)) { ++failures; return; }
    CHECK(drawAndRead(r, Fit::Fill, out));
    CHECK(closeTo(at(out, 100, 50, 50), 0, 0, 0));
}
static void odd_width_rejected() {
    Renderer r; if (!makeRenderer(r)) { ++failures; return; }
    std::vector<uint8_t> f(3 * 2 * 2, 0);
    CHECK(!r.upload(f.data(), 3, 2));
}

int main() {
    solid_black(); solid_white(); red_601(); red_709();
    contain_letterbox(); fill_covers(); size_change(); no_frame_is_black(); odd_width_rejected();
    if (failures == 0) std::printf("PASS\n");
    return failures;
}
