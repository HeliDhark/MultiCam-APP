#include "source/dshow-base.hpp"
#include "source/dshow-enum.hpp"
#include "source/dshow-formats.hpp"
#include "channel.hpp"
#include "jpeg.hpp"
#include <setupapi.h>
#include <cfgmgr32.h>
#include <initguid.h>
#include <devpkey.h>
#include <iostream>
#include <atomic>
#include <set>
#include <algorithm>
#include <memory>

using namespace DShow;
static std::wstring prop(IPropertyBag* bag, const wchar_t* key) {
    VARIANT v; VariantInit(&v); std::wstring text;
    if (SUCCEEDED(bag->Read(key, &v, nullptr)) && v.vt == VT_BSTR) text = v.bstrVal;
    VariantClear(&v); return text;
}
struct Identity { std::wstring key, kind = L"porta USB", location; };
static Identity identity(const std::wstring& path) {
    Identity result; result.key = path;
    std::transform(result.key.begin(), result.key.end(), result.key.begin(), towlower);
    HDEVINFO set = SetupDiCreateDeviceInfoList(nullptr, nullptr);
    if (set == INVALID_HANDLE_VALUE) return result;
    SP_DEVICE_INTERFACE_DATA iface{sizeof(iface)};
    SP_DEVINFO_DATA dev{sizeof(dev)};
    if (SetupDiOpenDeviceInterfaceW(set, path.c_str(), 0, &iface)) {
        DWORD size = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &iface, nullptr, 0, &size, nullptr);
        std::vector<BYTE> buffer(size);
        if (size >= sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)) {
            auto detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buffer.data());
            detail->cbSize = sizeof(*detail);
            if (SetupDiGetDeviceInterfaceDetailW(set, &iface, detail, size, nullptr, &dev)) {
                wchar_t loc[1024]{}; DEVPROPTYPE type;
                if (SetupDiGetDevicePropertyW(set, &dev, &DEVPKEY_Device_LocationPaths, &type,
                    reinterpret_cast<BYTE*>(loc), sizeof(loc), nullptr, 0)) result.location = loc;
                DEVINST node = dev.DevInst;
                for (int i = 0; i < 6; ++i) {
                    wchar_t instance[MAX_DEVICE_ID_LEN]{}; ULONG caps = 0, bytes = sizeof(caps), regType = 0;
                    if (CM_Get_Device_IDW(node, instance, MAX_DEVICE_ID_LEN, 0) != CR_SUCCESS) break;
                    std::wstring value(instance);
                    if (value.rfind(L"USB\\VID_", 0) == 0 && value.find(L"&MI_") == std::wstring::npos &&
                        CM_Get_DevNode_Registry_PropertyW(node, CM_DRP_CAPABILITIES, &regType, &caps, &bytes, 0) == CR_SUCCESS &&
                        (caps & CM_DEVCAP_UNIQUEID)) {
                        // Preserve the function/interface suffix for multi-camera USB devices.
                        auto hash = path.find(L'#');
                        auto end = path.find(L'#', hash + 1);
                        auto function = hash != std::wstring::npos ? path.substr(hash + 1, end - hash - 1) : L"";
                        result.key = L"serial:" + value + L"|" + function;
                        std::transform(result.key.begin(), result.key.end(), result.key.begin(), towlower);
                        result.kind = L"serial do dispositivo"; break;
                    }
                    DEVINST parent;
                    if (CM_Get_Parent(&parent, node, 0) != CR_SUCCESS) break;
                    node = parent;
                }
            }
        }
    }
    SetupDiDestroyDeviceInfoList(set); return result;
}
static int listDevices(bool all) {
    ComPtr<ICreateDevEnum> enumerator; ComPtr<IEnumMoniker> monikers;
    HRESULT hr = CoCreateInstance(CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&enumerator));
    if (FAILED(hr)) throw std::runtime_error("Falha ao enumerar dispositivos DirectShow.");
    hr = enumerator->CreateClassEnumerator(CLSID_VideoInputDeviceCategory, &monikers, 0);
    if (FAILED(hr)) throw std::runtime_error("Falha na lista de cameras.");
    std::cout << "["; bool comma = false;
    if (monikers) {
        ComPtr<IMoniker> moniker;
        while (monikers->Next(1, &moniker, nullptr) == S_OK) {
            ComPtr<IPropertyBag> bag;
            if (FAILED(moniker->BindToStorage(nullptr, nullptr, IID_PPV_ARGS(&bag)))) continue;
            auto name = prop(bag, L"FriendlyName"), path = prop(bag, L"DevicePath");
            if (path.empty()) {
                if (!all) continue;
                LPOLESTR display = nullptr;
                if (SUCCEEDED(moniker->GetDisplayName(nullptr, nullptr, &display))) { path = display; CoTaskMemFree(display); }
            }
            // Only physical USB video devices are automatically shared. Never recapture our own virtual outputs.
            std::wstring lower = path;
            std::transform(lower.begin(), lower.end(), lower.begin(), towlower);
            if (!all && lower.find(L"usb#") == std::wstring::npos) continue;
            auto id = identity(path);
            if (comma) std::cout << ","; comma = true;
            std::cout << "{\"Name\":" << multi::json(name) << ",\"Path\":" << multi::json(path)
                << ",\"Identity\":" << multi::json(id.key) << ",\"IdentityKind\":" << multi::json(id.kind)
                << ",\"Location\":" << multi::json(id.location) << "}";
        }
    }
    std::cout << "]\n"; return 0;
}
static int modes(const wchar_t* path) {
    ComPtr<IBaseFilter> filter; ComPtr<IPin> pin; std::vector<VideoInfo> caps;
    if (!GetDeviceFilter(CLSID_VideoInputDeviceCategory, nullptr, path, &filter) ||
        !GetFilterPin(filter, MEDIATYPE_Video, PIN_CATEGORY_CAPTURE, PINDIR_OUTPUT, &pin) || !EnumVideoCaps(pin, caps))
        throw std::runtime_error("Nao foi possivel ler os modos. Feche programas que estejam usando a placa.");
    std::cout << "["; bool comma = false;
    std::set<std::tuple<int,int,long long,int>> seen;
    for (const auto& c : caps) {
        for (auto size : {std::pair<int,int>{c.minCX,c.minCY}, {c.maxCX,c.maxCY}, {1920,1080}, {1280,720}, {3840,2160}, {640,480}}) {
            auto [w,h] = size;
            if (w < c.minCX || w > c.maxCX || h < c.minCY || h > c.maxCY || w > 4096 || h > 2160 || w < 2 || h < 2 || w % 2) continue;
            if (c.granularityCX > 0 && (w-c.minCX)%c.granularityCX || c.granularityCY > 0 && (h-c.minCY)%c.granularityCY) continue;
            for (long long interval : {c.minInterval, c.maxInterval, 166667LL, 166833LL, 200000LL, 333333LL, 333667LL}) {
                if (interval <= 0 || interval < c.minInterval || interval > c.maxInterval || !seen.emplace(w,h,interval,int(c.format)).second) continue;
                if (comma) std::cout << ","; comma = true;
                std::cout << "{\"Width\":" << w << ",\"Height\":" << h << ",\"Interval\":" << interval
                    << ",\"Format\":" << int(c.format) << "}";
            }
        }
    }
    std::cout << "]\n"; return 0;
}
static int capture(int argc, wchar_t** argv) {
    if (argc != 10) throw std::runtime_error("capture: path id width height interval format parentPid name");
    std::wstring path = argv[2], id = argv[3];
    int w = std::stoi(argv[4]), h = std::stoi(argv[5]); long long interval = std::stoll(argv[6]);
    auto format = VideoFormat(std::stoi(argv[7])); DWORD parentPid = std::stoul(argv[8]);
    if (w < 2 || w > 4096 || w%2 || h < 2 || h > 2160 || interval < 10000 || interval > 10000000)
        throw std::runtime_error("Modo invalido.");
    multi::Channel channel;
    if (!channel.open(id, true)) throw std::runtime_error("Falha na memoria compartilhada.");
    channel.begin(uint32_t(w), uint32_t(h), interval);
    HANDLE stop = CreateEventW(nullptr, TRUE, FALSE, multi::objectName(id,L"-stop").c_str());
    HANDLE parent = OpenProcess(SYNCHRONIZE, FALSE, parentPid);
    if (!stop || !parent) throw std::runtime_error("Falha ao acompanhar o processo principal.");
    ResetEvent(stop);
    std::atomic<uint64_t> count{0}, dropped{0};
    std::unique_ptr<JpegDecoder> jpeg;
    if(format==VideoFormat::MJPEG)jpeg=std::make_unique<JpegDecoder>();
    Device device(InitGraph::True);
    VideoConfig config; config.path = path; config.name = argv[9]; config.useDefaultConfig = false;
    config.cx = w; config.cy_abs = h; config.frameInterval = interval;
    config.internalFormat = format; config.format = jpeg?VideoFormat::MJPEG:VideoFormat::YUY2;
    config.callback = [&](const VideoConfig& actual, unsigned char* data, size_t bytes, long long, long long, long) {
        if(actual.cx!=w||actual.cy_abs!=h){++dropped;return;}
        if(jpeg) {
            if(actual.format!=VideoFormat::MJPEG){++dropped;return;}
            data=jpeg->decode(data,bytes,w,h);bytes=size_t(w)*h*2;
            if(!data){++dropped;return;}
        }else if(actual.format!=VideoFormat::YUY2){++dropped;return;}
        if(bytes<size_t(w)*h*2||bytes%h){++dropped;return;}
        auto target = channel.writeSlot();
        size_t stride = bytes/h;
        if (stride == size_t(w)*2) memcpy(target, data, size_t(w)*h*2);
        else for (int y=0; y<h; ++y) memcpy(target+size_t(y)*w*2, data+size_t(y)*stride, size_t(w)*2);
        ++count; channel.publish();
    };
    if (!device.Valid() || !device.SetVideoConfig(&config) || !device.ConnectFilters())
        throw std::runtime_error("Nao foi possivel abrir este modo ou converter para YUY2. Tente outro formato.");
    VideoConfig actual;
    if (!device.GetVideoConfig(actual) || actual.cx != w || actual.cy_abs != h || std::llabs(actual.frameInterval-interval)>2)
        throw std::runtime_error("O driver negociou outro modo. Selecione uma resolucao/FPS suportada.");
    if (device.Start() != Result::Success) throw std::runtime_error("Placa ocupada ou falha ao iniciar captura.");
    HANDLE waits[]{stop,parent}; uint64_t previous=0, lastFrame=GetTickCount64();
    while (WaitForMultipleObjects(2,waits,FALSE,1000)==WAIT_TIMEOUT) {
        auto current=count.load();
        std::cout << "{\"Frames\":" << current << ",\"Fps\":" << current-previous << ",\"Dropped\":" << dropped.load() << "}" << std::endl;
        if (current != previous) lastFrame=GetTickCount64();
        if (GetTickCount64()-lastFrame>7000) { std::cerr << "Sem novos quadros por 7 segundos. Verifique conexao, modo e programas usando a placa.\n"; break; }
        previous=current;
    }
    device.Stop();
    channel.stop();
    CloseHandle(parent); CloseHandle(stop); return count ? 0 : 3;
}
int wmain(int argc, wchar_t** argv) {
    HRESULT hr=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    if (FAILED(hr)) return 2;
    SetLogCallback([](LogType type,const wchar_t* msg,void*) { if (type==LogType::Error || type==LogType::Warning) std::cerr << multi::utf8(msg) << '\n'; },nullptr);
    int result=0;
    try {
        if (argc==2 && std::wstring(argv[1])==L"--list") result=listDevices(false);
        else if (argc==2 && std::wstring(argv[1])==L"--list-all") result=listDevices(true);
        else if (argc==3 && std::wstring(argv[1])==L"--modes") result=modes(argv[2]);
        else if (argc>1 && std::wstring(argv[1])==L"--capture") result=capture(argc,argv);
        else { std::cerr << "Use --list, --list-all, --modes path, --capture ...\n"; result=1; }
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; result=2; }
    CoUninitialize(); return result;
}
