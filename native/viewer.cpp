// MultiCam.Viewer: reads a camera channel and shows it in a borderless PiP window (--pip)
// or as a live desktop wallpaper behind the icons (--wallpaper).
// Protocol: commands on stdin (see viewer::parseCommand), JSON events on stdout, one per line.
// Exit codes: 0 normal, 1 snapshot timeout, 2 invalid arguments, 3 Direct3D failure (also a lost
// device: driver update, TDR), 4 monitor not found, 5 could not attach behind the desktop icons
// (WorkerW), 6 the desktop is not available or went away (Explorer not running or restarted).
// The panel waits and tries again after 4 and 6, and gives up on 5.
#include <windows.h>
#include <windowsx.h>
#include <objbase.h>
#include <shellapi.h>
#include <mmsystem.h>
#include <wtsapi32.h>
#include <deque>
#include <mutex>
#include <string>
#include <vector>
#include <cstdio>
#include "channel.hpp"
#include "demand.hpp"
#include "renderer.hpp"
#include "viewer_core.hpp"

#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "wtsapi32.lib")

namespace {

constexpr viewer::Size MinPip{160, 90};
constexpr viewer::Size MaxPip{3840, 2160};
constexpr ULONGLONG StaleMs = 500;
constexpr ULONGLONG SnapshotTimeoutMs = 15000;
constexpr UINT MenuLock = 1, MenuTopMost = 2, MenuClose = 3;

// ------------------------------------------------------------------ stdout / stdin
std::mutex outLock;
void emit(const std::string& line) {
    std::lock_guard<std::mutex> guard(outLock);
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!out || out == INVALID_HANDLE_VALUE) return;
    const std::string text = line + "\n";
    DWORD written = 0;
    WriteFile(out, text.data(), DWORD(text.size()), &written, nullptr);
}
void fail(const std::wstring& message) {
    HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
    if (!err || err == INVALID_HANDLE_VALUE) return;
    const std::string text = multi::utf8(message) + "\n";
    DWORD written = 0;
    WriteFile(err, text.data(), DWORD(text.size()), &written, nullptr);
}

struct Input {
    std::mutex lock;
    std::deque<std::string> lines;
    HANDLE ready = CreateEventW(nullptr, FALSE, FALSE, nullptr);
} input;

DWORD WINAPI readInput(void*) {
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    if (!in || in == INVALID_HANDLE_VALUE) return 0;
    std::string pending;
    char buffer[512];
    DWORD got = 0;
    while (ReadFile(in, buffer, sizeof(buffer), &got, nullptr) && got) {
        pending.append(buffer, got);
        size_t end;
        bool any = false;
        while ((end = pending.find('\n')) != std::string::npos) {
            std::lock_guard<std::mutex> guard(input.lock);
            input.lines.push_back(pending.substr(0, end));
            pending.erase(0, end + 1);
            any = true;
        }
        if (pending.size() > 4096) pending.clear();  // garbage without newlines
        if (any) SetEvent(input.ready);
    }
    return 0;
}

// ------------------------------------------------------------------ helpers
std::vector<viewer::Rect> monitors() {
    std::vector<viewer::Rect> list;
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR monitor, HDC, LPRECT, LPARAM data) -> BOOL {
        MONITORINFO info{sizeof(info)};
        if (GetMonitorInfoW(monitor, &info)) {
            const RECT& r = info.rcMonitor;
            auto* out = reinterpret_cast<std::vector<viewer::Rect>*>(data);
            viewer::Rect rect{r.left, r.top, r.right - r.left, r.bottom - r.top};
            // Primary monitor first, so it is the fallback of clampToMonitors.
            if (info.dwFlags & MONITORINFOF_PRIMARY) out->insert(out->begin(), rect); else out->push_back(rect);
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&list));
    return list;
}

void setDpiAwareness() {
    using Fn = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
    if (auto fn = reinterpret_cast<Fn>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext")))
        fn(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
}

bool writeBmp(const std::wstring& path, const std::vector<uint8_t>& bgra, int width, int height) {
    const int row = (width * 3 + 3) & ~3;
    BITMAPFILEHEADER file{};
    BITMAPINFOHEADER info{};
    info.biSize = sizeof(info);
    info.biWidth = width;
    info.biHeight = height;  // bottom-up
    info.biPlanes = 1;
    info.biBitCount = 24;
    info.biCompression = BI_RGB;
    info.biSizeImage = DWORD(row) * height;
    file.bfType = 0x4D42;
    file.bfOffBits = sizeof(file) + sizeof(info);
    file.bfSize = file.bfOffBits + info.biSizeImage;
    std::vector<uint8_t> pixels(info.biSizeImage, 0);
    for (int y = 0; y < height; ++y) {
        const uint8_t* src = &bgra[size_t(height - 1 - y) * width * 4];
        uint8_t* dst = &pixels[size_t(y) * row];
        for (int x = 0; x < width; ++x) { dst[x * 3] = src[x * 4]; dst[x * 3 + 1] = src[x * 4 + 1]; dst[x * 3 + 2] = src[x * 4 + 2]; }
    }
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool ok = WriteFile(f, &file, sizeof(file), &written, nullptr) && WriteFile(f, &info, sizeof(info), &written, nullptr) &&
              WriteFile(f, pixels.data(), DWORD(pixels.size()), &written, nullptr);
    CloseHandle(f);
    return ok;
}

bool parseInt(const wchar_t* text, int& value) {
    if (!text || !*text) return false;
    wchar_t* end = nullptr;
    const long v = wcstol(text, &end, 10);
    if (*end || v < -1000000 || v > 1000000) return false;
    value = int(v);
    return true;
}
bool parseFlag(const wchar_t* text, bool& value) {
    if (wcscmp(text, L"0") == 0) { value = false; return true; }
    if (wcscmp(text, L"1") == 0) { value = true; return true; }
    return false;
}
bool parseGuid(const wchar_t* text) {
    GUID id;
    return SUCCEEDED(CLSIDFromString(text, &id));
}

// ------------------------------------------------------------------ viewer state
struct Options {
    std::wstring camera;
    viewer::Rect rect;
    bool topMost = false, locked = false;
    DWORD parentPid = 0;
    std::wstring snapshot;
    // Wallpaper mode.
    bool wallpaper = false;
    std::wstring monitor;
    int fps = 60;
    viewer::Fit fit = viewer::Fit::Fill;
};

double nowMs() {
    static LARGE_INTEGER frequency = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return double(counter.QuadPart) * 1000.0 / double(frequency.QuadPart);
}

// Monitor rectangle (virtual-screen pixels) by Windows device name, e.g. \\.\DISPLAY1.
bool findMonitor(const std::wstring& device, RECT& rect) {
    struct Search { const std::wstring* device; RECT rect; bool found; } search{&device, {}, false};
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR monitor, HDC, LPRECT, LPARAM data) -> BOOL {
        auto* s = reinterpret_cast<Search*>(data);
        MONITORINFOEXW info{};
        info.cbSize = sizeof(info);
        if (GetMonitorInfoW(monitor, &info) && _wcsicmp(info.szDevice, s->device->c_str()) == 0) {
            s->rect = info.rcMonitor; s->found = true; return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&search));
    rect = search.rect;
    return search.found;
}

// The WorkerW that Explorer puts between the wallpaper and the icons (Windows 10).
HWND findWorkerW() {
    HWND progman = FindWindowW(L"Progman", nullptr);
    if (!progman) return nullptr;
    DWORD_PTR result = 0;
    SendMessageTimeoutW(progman, 0x052C, 0xD, 1, SMTO_NORMAL, 1000, &result);
    HWND worker = nullptr;
    EnumWindows([](HWND top, LPARAM data) -> BOOL {
        if (FindWindowExW(top, nullptr, L"SHELLDLL_DefView", nullptr)) {
            *reinterpret_cast<HWND*>(data) = FindWindowExW(nullptr, top, L"WorkerW", nullptr);
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&worker));
    return worker;
}

// Puts the wallpaper window over the monitor again after a change of resolution, scale or
// monitor layout (a child window gets no WM_DISPLAYCHANGE). False when the monitor is gone.
bool keepOnMonitor(HWND hwnd, const std::wstring& device, RECT& monitor) {
    if (!findMonitor(device, monitor)) return false;
    RECT now{};
    GetWindowRect(hwnd, &now);
    if (EqualRect(&now, &monitor)) return true;
    POINT origin{monitor.left, monitor.top};
    ScreenToClient(GetParent(hwnd), &origin);
    SetWindowPos(hwnd, nullptr, origin.x, origin.y, monitor.right - monitor.left, monitor.bottom - monitor.top,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    return true;
}

// A foreground window (not the shell, not us) that covers the whole monitor.
bool fullscreenOver(const RECT& monitor) {
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    if (pid == GetCurrentProcessId()) return false;
    wchar_t cls[64]{};
    GetClassNameW(fg, cls, 64);
    if (!wcscmp(cls, L"Progman") || !wcscmp(cls, L"WorkerW") || !wcscmp(cls, L"Shell_TrayWnd")) return false;
    RECT r{};
    if (!GetWindowRect(fg, &r)) return false;
    return r.left <= monitor.left && r.top <= monitor.top && r.right >= monitor.right && r.bottom >= monitor.bottom;
}

struct Viewer {
    Options opt;
    HWND hwnd = nullptr;
    Renderer renderer;
    multi::Channel channel;
    uint32_t srcW = 0, srcH = 0;
    bool picture = false;          // the renderer holds a live frame
    ULONGLONG lastFrame = 0, lastOpen = 0;
    int drawn = 0;
    bool quit = false;
    int exitCode = 0;
    bool closing = false;          // we are destroying our own window
    bool paused = false, sessionLocked = false;
    double lastDraw = -1e9;

    void stop(int code, const std::wstring& reason) {
        if (quit) return;
        fail(reason);
        exitCode = code;
        quit = true;
    }
    // A lost device never recovers by itself: exit so the panel starts a fresh viewer.
    void checkDevice() {
        if (renderer.lost()) stop(3, L"O dispositivo Direct3D foi perdido (driver de vídeo reiniciado?).");
    }

    viewer::Rect windowRect() const {
        RECT r{};
        GetWindowRect(hwnd, &r);
        return {r.left, r.top, r.right - r.left, r.bottom - r.top};
    }
    viewer::Size source() const { return {int(srcW), int(srcH)}; }
    void emitGeometry() { emit(viewer::geometryEvent(windowRect())); }

    void place(viewer::Rect r) {
        r = viewer::clampToMonitors(r, monitors());
        SetWindowPos(hwnd, nullptr, r.x, r.y, r.w, r.h, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    void setLocked(bool value) { opt.locked = value; emit(viewer::toggleEvent("lock", value)); }
    void setTopMost(bool value) {
        opt.topMost = value;
        SetWindowPos(hwnd, value ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        emit(viewer::toggleEvent("topmost", value));
    }

    void redraw() {
        renderer.draw(opt.fit);
        if (!renderer.present()) checkDevice();
    }

    void command(const viewer::Command& c) {
        using K = viewer::CommandKind;
        switch (c.kind) {
        case K::Quit: quit = true; break;
        case K::Lock: setLocked(c.a != 0); break;
        case K::TopMost: setTopMost(c.a != 0); break;
        case K::Resize: {
            viewer::Rect r = windowRect();
            const viewer::Size s = viewer::keepAspect({c.a, c.b}, source(), true, MinPip, MaxPip);
            r.w = s.w; r.h = s.h;
            place(r); emitGeometry();
            break;
        }
        case K::Move: {
            viewer::Rect r = windowRect();
            r.x = c.a; r.y = c.b;
            place(r); emitGeometry();
            break;
        }
        default: break;  // invalid lines are ignored
        }
    }

    void sourceChanged(uint32_t w, uint32_t h) {
        srcW = w; srcH = h;
        char buffer[96];
        std::snprintf(buffer, sizeof(buffer), "{\"Event\":\"source\",\"Width\":%u,\"Height\":%u}", w, h);
        emit(buffer);
        if (opt.wallpaper) return;  // the wallpaper always covers its monitor
        viewer::Rect r = windowRect();
        const viewer::Size s = viewer::keepAspect({r.w, r.h}, source(), true, MinPip, MaxPip);
        r.w = s.w; r.h = s.h;
        place(r);
        emitGeometry();
    }

    // Returns true when a frame was drawn.
    bool poll(ULONGLONG now) {
        if (!channel.valid()) {
            if (now - lastOpen < 100) return false;
            lastOpen = now;
            if (!channel.open(opt.camera, false)) return false;
        }
        uint32_t w = 0, h = 0;
        if (channel.peekSize(w, h)) {
            if (w != srcW || h != srcH) sourceChanged(w, h);
            if (channel.read(w, h) == multi::Channel::Read::Updated && channel.hasFrame(size_t(w) * h * 2)) {
                // The wallpaper draws at most `fps` frames per second (3/4 of the period absorbs
                // jitter of the source); a paused wallpaper keeps the frame but draws nothing.
                const double t = nowMs();
                const double period = opt.wallpaper ? 750.0 / opt.fps : 0.0;
                if (paused || (picture && t - lastDraw < period)) { lastFrame = now; return false; }
                if (renderer.upload(channel.frame(), int(w), int(h))) {
                    lastDraw = t;
                    picture = true;
                    lastFrame = now;
                    redraw();
                    ++drawn;
                    return true;
                }
                checkDevice();
            }
        }
        if (picture && now - lastFrame > StaleMs) {
            picture = false;
            renderer.clear();
            redraw();
        }
        return false;
    }

    bool snapshot() {
        RECT rc{};
        GetClientRect(hwnd, &rc);
        Renderer off;
        std::wstring error;
        if (!off.init(nullptr, error)) return false;
        off.resize(rc.right - rc.left, rc.bottom - rc.top);
        if (!off.upload(channel.frame(), int(srcW), int(srcH))) return false;
        off.draw(opt.fit);
        std::vector<uint8_t> bgra;
        int w = 0, h = 0;
        return off.readback(bgra, w, h) && writeBmp(opt.snapshot, bgra, w, h);
    }

    void contextMenu(int x, int y) {
        if (x == -1 && y == -1) { const viewer::Rect r = windowRect(); x = r.x + r.w / 2; y = r.y + r.h / 2; }
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING | (opt.locked ? MF_CHECKED : 0), MenuLock, L"Travar");
        AppendMenuW(menu, MF_STRING | (opt.topMost ? MF_CHECKED : 0), MenuTopMost, L"Sempre à frente");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, MenuClose, L"Fechar PiP");
        SetForegroundWindow(hwnd);
        const UINT chosen = UINT(TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, x, y, 0, hwnd, nullptr));
        DestroyMenu(menu);
        if (chosen == MenuLock) setLocked(!opt.locked);
        else if (chosen == MenuTopMost) setTopMost(!opt.topMost);
        else if (chosen == MenuClose) quit = true;
    }
} app;

LRESULT hitCode(viewer::Hit hit) {
    using H = viewer::Hit;
    switch (hit) {
    case H::Caption: return HTCAPTION;
    case H::Left: return HTLEFT;
    case H::Right: return HTRIGHT;
    case H::Top: return HTTOP;
    case H::Bottom: return HTBOTTOM;
    case H::TopLeft: return HTTOPLEFT;
    case H::TopRight: return HTTOPRIGHT;
    case H::BottomLeft: return HTBOTTOMLEFT;
    case H::BottomRight: return HTBOTTOMRIGHT;
    default: return HTCLIENT;
    }
}

LRESULT CALLBACK windowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (app.opt.wallpaper) {
        switch (msg) {
        case WM_SIZE:
            if (app.hwnd) { app.renderer.resize(LOWORD(lp), HIWORD(lp)); app.redraw(); }
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            EndPaint(hwnd, &ps);
            if (app.hwnd) app.redraw();
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_WTSSESSION_CHANGE:
            if (wp == WTS_SESSION_LOCK) app.sessionLocked = true;
            else if (wp == WTS_SESSION_UNLOCK) app.sessionLocked = false;
            return 0;
        case WM_DESTROY:
            // Explorer restarted or crashed and took the WorkerW (and us) with it.
            if (!app.closing) app.stop(6, L"A área de trabalho foi recriada (Explorer reiniciado).");
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    switch (msg) {
    case WM_NCHITTEST: {
        RECT r{};
        GetWindowRect(hwnd, &r);
        const viewer::Point p{GET_X_LPARAM(lp) - r.left, GET_Y_LPARAM(lp) - r.top};
        return hitCode(viewer::pipHit({r.right - r.left, r.bottom - r.top}, p, app.opt.locked));
    }
    case WM_SIZING: {
        auto* r = reinterpret_cast<RECT*>(lp);
        const bool heightLeads = wp == WMSZ_TOP || wp == WMSZ_BOTTOM;
        const viewer::Size s = viewer::keepAspect({r->right - r->left, r->bottom - r->top}, app.source(), !heightLeads, MinPip, MaxPip);
        if (wp == WMSZ_LEFT || wp == WMSZ_TOPLEFT || wp == WMSZ_BOTTOMLEFT) r->left = r->right - s.w; else r->right = r->left + s.w;
        if (wp == WMSZ_TOP || wp == WMSZ_TOPLEFT || wp == WMSZ_TOPRIGHT) r->top = r->bottom - s.h; else r->bottom = r->top + s.h;
        return TRUE;
    }
    case WM_EXITSIZEMOVE:
        app.emitGeometry();
        return 0;
    case WM_SIZE:
        if (app.hwnd) { app.renderer.resize(LOWORD(lp), HIWORD(lp)); app.redraw(); }
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        if (app.hwnd) app.redraw();
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEWHEEL:
        return 0;  // the wheel never resizes
    case WM_CONTEXTMENU:
        app.contextMenu(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        return 0;
    case WM_NCRBUTTONUP:
        // Right-click on the draggable area: show our menu, never the system menu.
        app.contextMenu(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        return 0;
    case WM_CLOSE:
        app.quit = true;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int usage() {
    fail(L"uso: MultiCam.Viewer.exe --pip <guid> <x> <y> <w> <h> <topmost 0|1> <locked 0|1> <parentPid> [--snapshot <arquivo.bmp>]\n"
         L"     MultiCam.Viewer.exe --wallpaper <guid> <monitor> <fps 30|60> <fill|contain> <parentPid> [--snapshot <arquivo.bmp>]");
    return 2;
}

bool parseWallpaper(int argc, wchar_t** argv, Options& o) {
    if (argc != 7 && argc != 9) return false;
    o.wallpaper = true;
    o.camera = argv[2];
    o.monitor = argv[3];
    int parent = 0;
    if (!parseGuid(argv[2]) || o.monitor.empty() || !parseInt(argv[4], o.fps) || (o.fps != 30 && o.fps != 60) ||
        !parseInt(argv[6], parent) || parent <= 0)
        return false;
    if (wcscmp(argv[5], L"fill") == 0) o.fit = viewer::Fit::Fill;
    else if (wcscmp(argv[5], L"contain") == 0) o.fit = viewer::Fit::Contain;
    else return false;
    o.parentPid = DWORD(parent);
    if (argc == 9) {
        if (wcscmp(argv[7], L"--snapshot") != 0) return false;
        o.snapshot = argv[8];
    }
    return true;
}

bool parseArgs(int argc, wchar_t** argv, Options& o) {
    if (argc >= 2 && wcscmp(argv[1], L"--wallpaper") == 0) return parseWallpaper(argc, argv, o);
    if (argc < 10 || wcscmp(argv[1], L"--pip") != 0) return false;
    o.camera = argv[2];
    int parent = 0;
    if (!parseGuid(argv[2]) || !parseInt(argv[3], o.rect.x) || !parseInt(argv[4], o.rect.y) || !parseInt(argv[5], o.rect.w) ||
        !parseInt(argv[6], o.rect.h) || !parseFlag(argv[7], o.topMost) || !parseFlag(argv[8], o.locked) || !parseInt(argv[9], parent) ||
        parent <= 0 || o.rect.w < 1 || o.rect.h < 1)
        return false;
    o.parentPid = DWORD(parent);
    if (argc == 12 && wcscmp(argv[10], L"--snapshot") == 0) o.snapshot = argv[11];
    else if (argc != 10) return false;
    return true;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv || !parseArgs(argc, argv, app.opt)) return usage();
    LocalFree(argv);

    HANDLE parent = OpenProcess(SYNCHRONIZE, FALSE, app.opt.parentPid);
    if (!parent) return 0;  // the panel is already gone
    setDpiAwareness();
    timeBeginPeriod(1);

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = windowProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.lpszClassName = L"MultiCamPip";
    RegisterClassExW(&wc);

    HWND hwnd = nullptr;
    RECT monitor{};
    wchar_t savedWallpaper[MAX_PATH]{};
    if (app.opt.wallpaper) {
        if (!findMonitor(app.opt.monitor, monitor)) { fail(L"monitor não encontrado: " + app.opt.monitor); return 4; }
        if (!FindWindowW(L"Progman", nullptr)) { fail(L"A área de trabalho ainda não está pronta (Explorer não está rodando)."); return 6; }
        HWND worker = findWorkerW();
        if (!worker) {
            fail(L"Não foi possível ancorar o papel de parede atrás dos ícones (WorkerW não encontrada). "
                 L"Esta versão do Windows não é suportada pelo modo papel de parede.");
            return 5;
        }
        SystemParametersInfoW(SPI_GETDESKWALLPAPER, MAX_PATH, savedWallpaper, 0);
        POINT origin{monitor.left, monitor.top};
        ScreenToClient(worker, &origin);
        hwnd = CreateWindowExW(WS_EX_NOACTIVATE, wc.lpszClassName, L"MultiCam Wallpaper", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
                               origin.x, origin.y, monitor.right - monitor.left, monitor.bottom - monitor.top, worker, nullptr, instance, nullptr);
        if (!hwnd) { fail(L"Não foi possível criar a janela do papel de parede (WorkerW)."); return 5; }
        WTSRegisterSessionNotification(hwnd, NOTIFY_FOR_THIS_SESSION);
    } else {
        viewer::Rect r = app.opt.rect;
        r.w = std::max(MinPip.w, std::min(MaxPip.w, r.w));
        r.h = std::max(MinPip.h, std::min(MaxPip.h, r.h));
        r = viewer::clampToMonitors(r, monitors());
        const DWORD exStyle = WS_EX_TOOLWINDOW | (app.opt.topMost ? WS_EX_TOPMOST : 0);
        hwnd = CreateWindowExW(exStyle, wc.lpszClassName, L"MultiCam PiP", WS_POPUP, r.x, r.y, r.w, r.h, nullptr, nullptr, instance, nullptr);
        if (!hwnd) { fail(L"Não foi possível criar a janela."); return 3; }
    }
    std::wstring error;
    if (!app.renderer.init(hwnd, error)) { fail(error); app.closing = true; DestroyWindow(hwnd); return 3; }
    app.hwnd = hwnd;
    if (!app.opt.wallpaper) ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    app.redraw();

    char ready[64];
    std::snprintf(ready, sizeof(ready), "{\"Event\":\"ready\",\"Hwnd\":%lld}", static_cast<long long>(reinterpret_cast<intptr_t>(hwnd)));
    emit(ready);
    if (HANDLE t = CreateThread(nullptr, 0, readInput, nullptr, 0, nullptr)) CloseHandle(t);
    // An open viewer is a use of the camera: the panel keeps the card capturing.
    multi::DemandClient demand;
    // Not on stderr: the panel shows the last stderr line as the reason of an exit.
    if (!demand.begin(app.opt.camera)) OutputDebugStringW(L"MultiCam.Viewer: não foi possível sinalizar uso da câmera.\n");

    const ULONGLONG start = GetTickCount64();
    ULONGLONG lastStats = start, lastFullscreenCheck = 0;
    bool fullscreen = false;
    HANDLE waits[] = {parent, input.ready};
    while (!app.quit) {
        const DWORD w = MsgWaitForMultipleObjects(2, waits, FALSE, 4, QS_ALLINPUT);
        if (w == WAIT_OBJECT_0) break;  // parent exited
        if (w == WAIT_OBJECT_0 + 1) {
            std::deque<std::string> lines;
            { std::lock_guard<std::mutex> guard(input.lock); lines.swap(input.lines); }
            for (const auto& line : lines) app.command(viewer::parseCommand(line));
        }
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        if (app.quit) break;

        const ULONGLONG now = GetTickCount64();
        demand.beat(now);
        if (app.opt.wallpaper) {
            if (now - lastFullscreenCheck >= 500) {
                lastFullscreenCheck = now;
                if (!IsWindow(hwnd)) { app.stop(6, L"A área de trabalho foi recriada (Explorer reiniciado)."); break; }
                if (!keepOnMonitor(hwnd, app.opt.monitor, monitor)) { app.stop(4, L"monitor não encontrado: " + app.opt.monitor); break; }
                fullscreen = fullscreenOver(monitor);
            }
            app.paused = app.opt.snapshot.empty() && (fullscreen || app.sessionLocked);
        }
        const bool drew = app.poll(now);
        if (drew && !app.opt.snapshot.empty()) {
            app.exitCode = app.snapshot() ? 0 : 1;
            break;
        }
        if (!app.opt.snapshot.empty() && now - start > SnapshotTimeoutMs) { app.exitCode = 1; break; }
        if (now - lastStats >= 1000) {
            emit(viewer::statsEvent(app.drawn));
            app.drawn = 0;
            lastStats = now;
        }
    }
    demand.end();
    if (app.opt.wallpaper) {
        WTSUnRegisterSessionNotification(hwnd);
        HWND worker = GetParent(hwnd);
        app.closing = true;
        DestroyWindow(hwnd);
        if (worker) RedrawWindow(worker, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
        // Ask Windows to paint the user's own wallpaper again, without touching the saved setting.
        SystemParametersInfoW(SPI_SETDESKWALLPAPER, 0, savedWallpaper, 0);
    } else {
        DestroyWindow(hwnd);
    }
    timeEndPeriod(1);
    CloseHandle(parent);
    return app.exitCode;
}
