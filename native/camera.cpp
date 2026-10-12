#include "source/output-filter.hpp"
#include "channel.hpp"
#include "demand.hpp"
#include <atomic>
#include <thread>
#include <chrono>
#include <vector>
#include <algorithm>

static std::atomic<long> objects{0};
struct Settings { int width=0,height=0; long long interval=0; std::wstring name; };
static bool readSettings(REFCLSID cls, Settings& s) {
    HKEY key; auto path=L"Software\\MultiCam\\Cameras\\"+multi::guidText(cls);
    if (RegOpenKeyExW(HKEY_CURRENT_USER,path.c_str(),0,KEY_READ,&key)!=ERROR_SUCCESS) return false;
    auto number=[&](const wchar_t* name) { DWORD data=0,bytes=sizeof(data); RegGetValueW(key,nullptr,name,RRF_RT_REG_DWORD,nullptr,&data,&bytes); return data; };
    s.width=int(number(L"Width")); s.height=int(number(L"Height")); s.interval=number(L"Interval");
    wchar_t name[256]{}; DWORD bytes=sizeof(name);
    RegGetValueW(key,nullptr,L"Name",RRF_RT_REG_SZ,nullptr,name,&bytes); s.name=name;
    RegCloseKey(key);
    return s.width>=2 && s.width<=4096 && s.width%2==0 && s.height>=2 && s.height<=2160 && s.interval>=10000 && s.interval<=10000000;
}
class Camera final : public DShow::OutputFilter {
    CLSID cls_; Settings settings_; std::wstring id_;
    multi::Channel channel_;
    HANDLE stop_=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<REFERENCE_TIME> graphStart_{0};
    // Frames go out when the source publishes them, not on a timer of our own: a timer
    // drifts against the card's clock and repeats one frame and skips the next (judder).
    // A client slower than the source gets every `step`-th source frame (30 of 60).
    void stream() {
        using Clock=std::chrono::steady_clock;
        CoInitializeEx(nullptr,COINIT_MULTITHREADED);
        timeBeginPeriod(1);
        REFERENCE_TIME timestamp=0, previousTimestamp=-1;
        const auto hold=std::chrono::milliseconds(500);
        Clock::time_point lastNew{}, lastOpen{}, lastSent=Clock::now();
        uint32_t sent=0;
        bool repeating=false;
        // While this thread delivers video the camera is in use: the panel keeps the card capturing.
        multi::DemandClient demand;
        if (!demand.begin(id_))
            OutputDebugStringW((L"MultiCam: não foi possível sinalizar uso da câmera " + id_ + L"; a placa só captura se outro cliente pedir.\n").c_str());
        while (WaitForSingleObject(stop_,0)!=WAIT_OBJECT_0) {
            demand.beat();
            const auto interval=GetInterval();
            const auto period=std::chrono::nanoseconds(interval*100);
            const int w=GetCX(), h=GetCY();
            const size_t bytes=size_t(w)*h*2;
            auto now=Clock::now();
            if (!channel_.valid() && now-lastOpen>=std::chrono::milliseconds(100)) { lastOpen=now; channel_.open(id_,false); }
            // A real card is not regular: it stalls for a few frames (50-100 ms) and then delivers
            // a burst 1-2 ms apart. Like the card itself, deliver every frame once, in order: the
            // one after the last delivered (or `step` after it), not the newest. Only a frame
            // already overwritten (3 slots: two or more behind) is skipped.
            uint32_t wanted=0;
            if (channel_.valid()) {
                const uint32_t latest=channel_.latest();
                const int64_t source=channel_.sourceInterval();
                const uint32_t step=source>0 ? uint32_t(std::max<long long>(1,std::llround(double(interval)/double(source)))) : 1;
                if (latest && latest!=sent) {
                    if (!sent) wanted=latest;
                    else if (latest-sent>=step) wanted=uint32_t(latest-(sent+step))>1 ? latest-1 : sent+step;
                    // A source slower than expected: take what there is rather than drop the rate further.
                    else if (now-lastSent>=period*3/2) wanted=latest;
                }
            }
            const bool fresh=wanted!=0;
            // Without new frames keep the stream alive at the client's rate: repeat the last
            // frame, and black once the source has been gone for a while. Only after a real stall
            // (a worker restart), never during the short stalls of a working card.
            const auto stall=std::chrono::milliseconds(150);
            if (!fresh && (repeating ? now-lastSent<period : now-lastSent<stall)) {
                if (WaitForSingleObject(stop_,1)==WAIT_OBJECT_0) break;
                continue;
            }
            repeating=!fresh;
            BYTE* target=nullptr;
            if (LockSampleData(&target)) {
                // Copied straight from shared memory into the sample: the wanted frame, or the
                // last one again (it stays in its slot while the source is stalled).
                bool received=false;
                if (fresh) {
                    uint32_t frame=wanted;
                    received=channel_.copyTo(target,uint32_t(w),uint32_t(h),frame);
                    if (received) { sent=frame; lastNew=now; }
                    else sent=channel_.latest();  // torn or resized: never retry this one
                }
                if (!received && sent && now-lastNew<=hold) { uint32_t again=sent; received=channel_.copyTo(target,uint32_t(w),uint32_t(h),again); }
                // Black YUY2 only after the source has been gone for a while, never keep exposing a stale private frame.
                if (!received) {
                    for (size_t i=0; i<bytes; i+=4) { target[i]=16; target[i+1]=128; target[i+2]=16; target[i+3]=128; }
                }
                if (running_ && clock) {
                    REFERENCE_TIME graphNow=0;
                    if (SUCCEEDED(clock->GetTime(&graphNow))) timestamp=std::max<REFERENCE_TIME>(0,graphNow-graphStart_);
                }
                // Preroll can precede Run's clock origin, and coarse clock
                // readings can repeat. Keep delivered timestamps increasing.
                timestamp=std::max(timestamp,previousTimestamp+1);
                previousTimestamp=timestamp;
                UnlockSampleData(timestamp,timestamp+interval);
                timestamp+=interval;
            }
            lastSent=now;
        }
        demand.end();
        timeEndPeriod(1); CoUninitialize();
    }
public:
    Camera(REFCLSID cls,const Settings& s) : OutputFilter(DShow::VideoFormat::YUY2,s.width,s.height,s.interval),cls_(cls),settings_(s),id_(multi::guidText(cls)) {
        ++objects;
        // A 30 FPS client may consume the same 60 FPS source independently.
        if (s.interval<333333) AddVideoFormat(DShow::VideoFormat::YUY2,s.width,s.height,333333);
        if (s.interval<666667) AddVideoFormat(DShow::VideoFormat::YUY2,s.width,s.height,666667);
    }
    ~Camera() override { Stop(); CloseHandle(stop_); --objects; }
    const wchar_t* FilterName() const override { return settings_.name.c_str(); }
    STDMETHODIMP GetClassID(CLSID* cls) override { if (!cls) return E_POINTER; *cls=cls_; return S_OK; }
    STDMETHODIMP Pause() override {
        auto hr=OutputFilter::Pause();
        if (SUCCEEDED(hr) && !thread_.joinable()) { ResetEvent(stop_); thread_=std::thread([this]{stream();}); }
        return hr;
    }
    STDMETHODIMP Run(REFERENCE_TIME start) override {
        graphStart_=start; running_=true;
        if (!thread_.joinable()) Pause();
        return OutputFilter::Run(start);
    }
    STDMETHODIMP Stop() override {
        running_=false; SetEvent(stop_);
        auto hr=OutputFilter::Stop();
        if (thread_.joinable()) thread_.join();
        return hr;
    }
};
class Factory final : public IClassFactory {
    std::atomic<ULONG> refs_{1}; CLSID cls_; Settings settings_;
public:
    Factory(REFCLSID cls, const Settings& s):cls_(cls),settings_(s) { ++objects; }
    ~Factory() { --objects; }
    STDMETHODIMP QueryInterface(REFIID iid,void** result) override {
        if (!result) return E_POINTER; *result=nullptr;
        if (iid!=IID_IUnknown && iid!=IID_IClassFactory) return E_NOINTERFACE;
        *result=static_cast<IClassFactory*>(this); AddRef(); return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++refs_; }
    STDMETHODIMP_(ULONG) Release() override { auto refs=--refs_; if (!refs) delete this; return refs; }
    STDMETHODIMP CreateInstance(IUnknown* outer,REFIID iid,void** result) override {
        if (!result) return E_POINTER; *result=nullptr;
        if (outer) return CLASS_E_NOAGGREGATION;
        try {
            auto camera=new Camera(cls_,settings_); camera->AddRef();
            auto hr=camera->QueryInterface(iid,result); camera->Release(); return hr;
        } catch (...) { return E_OUTOFMEMORY; }
    }
    STDMETHODIMP LockServer(BOOL locked) override { if (locked) ++objects; else --objects; return S_OK; }
};
extern "C" HRESULT __stdcall DllGetClassObject(REFCLSID cls,REFIID iid,void** result) {
    if (!result) return E_POINTER; *result=nullptr;
    Settings settings;
    if (!readSettings(cls,settings)) return CLASS_E_CLASSNOTAVAILABLE;
    try { auto factory=new Factory(cls,settings); auto hr=factory->QueryInterface(iid,result); factory->Release(); return hr; }
    catch (...) { return E_OUTOFMEMORY; }
}
extern "C" HRESULT __stdcall DllCanUnloadNow() { return objects.load()?S_FALSE:S_OK; }
