// Demand slots: a client holding a fresh slot means "this camera is in use".
#include "../native/demand.hpp"
#include <cstdio>
#include <memory>
#include <vector>

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { ++failures; std::printf("FAIL line %d: %s\n", __LINE__, #cond); } } while (0)

static std::wstring newId() {
    GUID g;
    CoCreateGuid(&g);
    return multi::guidText(g);
}
// Tick of the first held slot, read straight from the shared area like the panel does.
static LONG64 heldTick(const std::wstring& id) {
    HANDLE m = OpenFileMappingW(FILE_MAP_READ, FALSE, multi::demandName(id).c_str());
    if (!m) return -1;
    LONG64 tick = -1;
    if (auto* a = static_cast<const multi::DemandArea*>(MapViewOfFile(m, FILE_MAP_READ, 0, 0, sizeof(multi::DemandArea)))) {
        for (const auto& s : a->slots) if (s.owner) { tick = s.tick; break; }
        UnmapViewOfFile(a);
    }
    CloseHandle(m);
    return tick;
}

static void slot_counts() {
    const auto id = newId();
    multi::DemandClient c;
    CHECK(c.begin(id));
    CHECK(c.holding());
    CHECK(multi::demandActive(id));
}
static void end_releases() {
    const auto id = newId();
    multi::DemandClient c;
    c.begin(id);
    c.end();
    CHECK(!c.holding());
    CHECK(!multi::demandActive(id));
}
static void stale_does_not_count() {
    const auto id = newId();
    multi::DemandClient c;
    c.begin(id, GetTickCount64() - 10000);
    CHECK(!multi::demandActive(id));
    c.beat();
    CHECK(multi::demandActive(id));
}
static void beat_throttled() {
    const auto id = newId();
    multi::DemandClient c;
    const ULONGLONG t = GetTickCount64();
    c.begin(id, t);
    c.beat(t + 100);
    CHECK(heldTick(id) == LONG64(t));
    c.beat(t + 600);
    CHECK(heldTick(id) == LONG64(t + 600));
}
static void table_full() {
    const auto id = newId();
    std::vector<std::unique_ptr<multi::DemandClient>> clients;
    for (uint32_t i = 0; i < multi::DemandSlots; ++i) {
        clients.push_back(std::make_unique<multi::DemandClient>());
        CHECK(clients.back()->begin(id));
    }
    multi::DemandClient extra;
    CHECK(!extra.begin(id));
    CHECK(!extra.holding());
    CHECK(multi::demandActive(id));
}
static void two_clients_same_process() {
    const auto id = newId();
    multi::DemandClient a, b;
    a.begin(id); b.begin(id);
    a.end();
    CHECK(multi::demandActive(id));
    b.end();
    CHECK(!multi::demandActive(id));
}
static void destructor_releases() {
    const auto id = newId();
    { multi::DemandClient c; c.begin(id); CHECK(multi::demandActive(id)); }
    CHECK(!multi::demandActive(id));
}
static void no_area_no_demand() {
    CHECK(!multi::demandActive(newId()));
}
static void wake_event() {
    // A running MultiCam panel waits on the same auto-reset event and takes the signal first.
    if (HANDLE app = OpenMutexW(SYNCHRONIZE, FALSE, L"Local\\MultiCam-App")) {
        CloseHandle(app);
        std::printf("SKIP wake_event: o MultiCam está aberto e consome o evento\n");
        return;
    }
    HANDLE wake = CreateEventW(nullptr, FALSE, FALSE, multi::DemandWakeName);
    WaitForSingleObject(wake, 0);  // drop a signal left by another test
    multi::DemandClient c;
    c.begin(newId());
    CHECK(WaitForSingleObject(wake, 0) == WAIT_OBJECT_0);
    CloseHandle(wake);
}

int main() {
    slot_counts(); end_releases(); stale_does_not_count(); beat_throttled(); table_full();
    two_clients_same_process(); destructor_releases(); no_area_no_demand(); wake_event();
    if (failures == 0) std::printf("PASS\n");
    return failures;
}
