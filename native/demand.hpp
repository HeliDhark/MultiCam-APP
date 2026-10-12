#pragma once
// Capture on demand: every client that receives video from a camera (the filter inside an
// app, a viewer) holds a slot in Local\MultiCam-{GUID}-demand and refreshes its tick. The
// panel captures a card only while some slot is fresh (and for a grace minute after).
// A crashed client stops counting by itself once its tick is DemandFreshMs old.
#include <windows.h>
#include <objbase.h>
#include <cstdint>
#include <string>
#include "channel.hpp"

namespace multi {
constexpr uint32_t DemandMagic = 0x444D434D, DemandVersion = 1, DemandSlots = 16;
constexpr ULONGLONG DemandFreshMs = 3000, DemandBeatMs = 500;
constexpr wchar_t DemandWakeName[] = L"Local\\MultiCam-wake";

// Same layout on x86 and x64 (the panel reads slot i at offset 16 + i * 16).
struct DemandSlot {
    volatile LONG owner;   // 0 = free, otherwise the holder's token
    uint32_t pid;
    volatile LONG64 tick;  // GetTickCount64 of the last beat
};
struct DemandArea {
    uint32_t magic, version;
    volatile LONG nextToken;
    uint32_t reserved;
    DemandSlot slots[DemandSlots];
};
static_assert(sizeof(DemandSlot) == 16, "demand slot ABI");
static_assert(sizeof(DemandArea) == 272, "demand area ABI must match x86, x64 and the panel");

inline std::wstring demandName(const std::wstring& id) { return objectName(id, L"-demand"); }

inline bool demandActive(const std::wstring& id, ULONGLONG now = GetTickCount64()) {
    // Read-write view: the interlocked 64-bit read (safe on x86) faults on a read-only page.
    HANDLE mapping = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, demandName(id).c_str());
    if (!mapping) return false;
    bool active = false;
    if (auto* area = static_cast<DemandArea*>(MapViewOfFile(mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(DemandArea)))) {
        if (area->magic == DemandMagic)
            for (auto& slot : area->slots) {
                const LONG64 tick = InterlockedCompareExchange64(&slot.tick, 0, 0);
                if (slot.owner && now - ULONGLONG(tick) < DemandFreshMs) { active = true; break; }
            }
        UnmapViewOfFile(area);
    }
    CloseHandle(mapping);
    return active;
}

class DemandClient {
    HANDLE mapping_ = nullptr;
    DemandArea* area_ = nullptr;
    DemandSlot* slot_ = nullptr;
    LONG token_ = 0;
    ULONGLONG last_ = 0;

    void close() {
        if (area_) UnmapViewOfFile(area_);
        if (mapping_) CloseHandle(mapping_);
        area_ = nullptr; mapping_ = nullptr;
    }

public:
    DemandClient() = default;
    DemandClient(const DemandClient&) = delete;
    DemandClient& operator=(const DemandClient&) = delete;
    ~DemandClient() { end(); }

    bool holding() const { return slot_ != nullptr; }

    // Takes a free slot and wakes the panel. False when the area cannot be opened or all
    // slots are taken: the client then works without signalling.
    bool begin(const std::wstring& id, ULONGLONG now = GetTickCount64()) {
        end();
        mapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(DemandArea), demandName(id).c_str());
        if (!mapping_) return false;
        area_ = static_cast<DemandArea*>(MapViewOfFile(mapping_, FILE_MAP_WRITE, 0, 0, sizeof(DemandArea)));
        if (!area_) { close(); return false; }
        InterlockedCompareExchange(reinterpret_cast<volatile LONG*>(&area_->magic), LONG(DemandMagic), 0);
        if (area_->magic != DemandMagic) { close(); return false; }
        area_->version = DemandVersion;
        do token_ = InterlockedIncrement(&area_->nextToken); while (token_ == 0);
        for (auto& slot : area_->slots) {
            if (InterlockedCompareExchange(&slot.owner, token_, 0) == 0) {
                InterlockedExchange64(&slot.tick, LONG64(now));
                slot.pid = GetCurrentProcessId();
                slot_ = &slot;
                break;
            }
        }
        if (!slot_) { close(); return false; }
        last_ = now;
        if (HANDLE wake = CreateEventW(nullptr, FALSE, FALSE, DemandWakeName)) { SetEvent(wake); CloseHandle(wake); }
        return true;
    }

    // Refreshes the tick, at most every DemandBeatMs.
    void beat(ULONGLONG now = GetTickCount64()) {
        if (!slot_ || now - last_ < DemandBeatMs) return;
        InterlockedExchange64(&slot_->tick, LONG64(now));
        last_ = now;
    }

    void end() {
        if (slot_) InterlockedCompareExchange(&slot_->owner, 0, token_);
        slot_ = nullptr;
        close();
    }
};
}
