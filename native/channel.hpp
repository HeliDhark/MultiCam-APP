#pragma once
#include <windows.h>
#include <cstdint>
#include <string>
#include <stdexcept>
#include <cstring>
#include <vector>
#include <utility>

namespace multi {
constexpr uint32_t Magic = 0x4D434D32;
constexpr size_t Capacity = 4096ULL * 2160 * 2;
constexpr uint32_t Slots = 3;
// Header layout is unchanged in size. `published` is the sequence number of the
// newest complete frame; frame N lives in slot N % Slots. The writer always fills
// a slot that no reader is entitled to read, so no lock is needed.
struct alignas(8) Header {
    uint32_t magic, width, height, bytes;
    int64_t interval;
    uint64_t frames, tick;
    uint32_t active;
    volatile uint32_t published;
};
static_assert(sizeof(Header) == 48, "IPC ABI must match x86 and x64");
inline std::wstring objectName(const std::wstring& id, const wchar_t* suffix) {
    return L"Local\\MultiCam-" + id + suffix;
}
class Channel {
    HANDLE mapping_ = nullptr;
    void* view_ = nullptr;
    // Reader state: last complete frame and the sequence it came from.
    std::vector<uint8_t> frame_, scratch_;
    uint32_t seq_ = 0;
    uint32_t next_ = 0;
    LONGLONG lastPublish_ = 0;  // writer: QPC of the last publish
    uint8_t* slot(uint32_t sequence) { return static_cast<uint8_t*>(view_) + sizeof(Header) + size_t(sequence % Slots) * Capacity; }
public:
    enum class Read { NoFrame, Unchanged, Updated, Mismatch };
    Channel() = default;
    Channel(const Channel&) = delete;
    ~Channel() { close(); }
    void close() {
        if (view_) UnmapViewOfFile(view_);
        if (mapping_) CloseHandle(mapping_);
        view_ = nullptr; mapping_ = nullptr;
        reset();
    }
    void reset() { frame_.clear(); seq_ = 0; }
    bool open(const std::wstring& id, bool writer) {
        close();
        auto name = objectName(id, L"-frames");
        const size_t total = sizeof(Header) + size_t(Slots) * Capacity;
        mapping_ = writer ? CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
            DWORD(total), name.c_str()) : OpenFileMappingW(FILE_MAP_READ, FALSE, name.c_str());
        if (!mapping_) return false;
        view_ = MapViewOfFile(mapping_, writer ? FILE_MAP_WRITE : FILE_MAP_READ, 0, 0, total);
        if (!view_) { close(); return false; }
        // A mapping left by an older build could be smaller than this layout.
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(view_, &info, sizeof(info)) || info.RegionSize < total) { close(); return false; }
        return true;
    }
    Header* header() { return static_cast<Header*>(view_); }
    bool valid() const { return view_ != nullptr; }

    // Writer. Keeps `published` monotonic across restarts of the capture worker
    // so readers never mistake a new session for an old frame.
    void begin(uint32_t w, uint32_t h, int64_t interval) {
        auto hdr = header();
        hdr->active = 0; MemoryBarrier();
        hdr->width = w; hdr->height = h; hdr->bytes = w * h * 2; hdr->interval = interval;
        hdr->frames = 0; hdr->tick = 0; MemoryBarrier();
        hdr->magic = Magic;
    }
    uint8_t* writeSlot() {
        next_ = header()->published + 1;
        if (next_ == 0) next_ = 1;
        return slot(next_);
    }
    // A real card stalls for a few frames and then delivers a burst 1-2 ms apart. With 3 slots
    // a reader that is busy for a moment would lose the oldest frames of the burst, so frames
    // are published at least a third of the frame interval apart (5.5 ms at 60 FPS). Regular
    // frames never wait; a burst is spread over a few ms. Spins only during such a burst.
    void publish() {
        auto hdr = header();
        LARGE_INTEGER frequency, now;
        QueryPerformanceFrequency(&frequency);
        const LONGLONG minGap = frequency.QuadPart * hdr->interval / 30000000;  // interval is in 100 ns
        if (lastPublish_)
            for (;;) {
                QueryPerformanceCounter(&now);
                if (now.QuadPart - lastPublish_ >= minGap) break;
                SwitchToThread();
            }
        ++hdr->frames; hdr->tick = GetTickCount64();
        MemoryBarrier();
        hdr->published = next_;
        hdr->active = 1;
        QueryPerformanceCounter(&now);
        lastPublish_ = now.QuadPart;
    }
    void stop() { if (view_) header()->active = 0; }

    // Reader. Current source size; true only for an active, plausible YUY2 frame size.
    bool peekSize(uint32_t& width, uint32_t& height) {
        if (!view_) return false;
        auto hdr = header();
        const uint32_t w = hdr->width, h = hdr->height;
        if (hdr->magic != Magic || !hdr->active || w < 2 || w > 4096 || h < 2 || h > 2160 || (w & 1)) return false;
        width = w; height = h;
        return true;
    }

    // Reader. Never blocks. Keeps the last complete frame so callers can repeat it.
    Read read(uint32_t w, uint32_t h) {
        auto hdr = header();
        for (int attempt = 0; attempt < 3; ++attempt) {
            if (hdr->magic != Magic || !hdr->active) return Read::NoFrame;
            const uint32_t cw = hdr->width, ch = hdr->height, bytes = hdr->bytes;
            if (cw != w || ch != h || bytes != w * h * 2 || bytes > Capacity) { reset(); return Read::Mismatch; }
            const uint32_t sequence = hdr->published; MemoryBarrier();
            if (sequence == 0) return Read::NoFrame;
            if (sequence == seq_ && frame_.size() == bytes) return Read::Unchanged;
            scratch_.resize(bytes);
            memcpy(scratch_.data(), slot(sequence), bytes);
            MemoryBarrier();
            // The writer needs two further frames to reach this slot again. If it
            // got that far during the copy, the copy may be torn: discard it.
            if (uint32_t(hdr->published - sequence) <= 1 && hdr->width == cw && hdr->height == ch && hdr->bytes == bytes) {
                std::swap(frame_, scratch_); seq_ = sequence;
                return Read::Updated;
            }
        }
        return frame_.empty() ? Read::NoFrame : Read::Unchanged;
    }
    // Reader. Sequence of the newest published frame (0 when there is none) and the source
    // frame interval. Cheap, no copy: lets a client wait for a new frame instead of a timer.
    uint32_t latest() {
        if (!view_) return 0;
        auto hdr = header();
        return hdr->magic == Magic && hdr->active ? hdr->published : 0;
    }
    int64_t sourceInterval() { return view_ && header()->magic == Magic ? header()->interval : 0; }
    // Reader without a private copy (one 4 MB copy less per 1080p frame and client): copies
    // frame `sequence`, or the newest one when it is 0, straight into dst. A frame stays in
    // its slot until the writer publishes two more, so the last one can be copied again to
    // repeat it while the source is stalled. False when the frame is gone, has another size
    // or was overwritten during the copy (dst is then garbage).
    bool copyTo(uint8_t* dst, uint32_t w, uint32_t h, uint32_t& sequence) {
        if (!view_) return false;
        auto hdr = header();
        if (hdr->magic != Magic || !hdr->active) return false;
        const uint32_t bytes = w * h * 2;
        if (hdr->width != w || hdr->height != h || hdr->bytes != bytes || bytes > Capacity) return false;
        const uint32_t newest = hdr->published; MemoryBarrier();
        const uint32_t wanted = sequence ? sequence : newest;
        if (!wanted || uint32_t(newest - wanted) > 1) return false;
        memcpy(dst, slot(wanted), bytes);
        MemoryBarrier();
        if (uint32_t(hdr->published - wanted) > 1 || hdr->width != w || hdr->height != h) return false;
        sequence = wanted;
        return true;
    }
    // Sequence of the frame kept by read().
    uint32_t sequence() const { return seq_; }
    bool hasFrame(size_t bytes) const { return frame_.size() == bytes; }
    const uint8_t* frame() const { return frame_.data(); }
};
inline std::wstring guidText(REFGUID id) {
    wchar_t text[40]{}; StringFromGUID2(id, text, 40); return text;
}
inline std::string utf8(const std::wstring& text) {
    int n = WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), out.data(), n, nullptr, nullptr);
    return out;
}
inline std::string json(const std::wstring& text) {
    std::string out = "\"";
    for (unsigned char c : utf8(text)) {
        if (c == '"' || c == '\\') { out += '\\'; out += char(c); }
        else if (c < 32) { char buf[8]; sprintf_s(buf, "\\u%04x", c); out += buf; }
        else out += char(c);
    }
    return out + '"';
}
}
