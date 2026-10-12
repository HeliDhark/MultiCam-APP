#include "source/dshow-base.hpp"
#include "source/capture-filter.hpp"
#include "source/dshow-media-type.hpp"
#include "channel.hpp"
#include "demand.hpp"
#include <atomic>
#include <iostream>
#include <thread>
#include <chrono>
#include <stdexcept>
#include <fstream>
#include <vector>
#include <algorithm>
#include <cmath>
using namespace DShow;
static void check(HRESULT hr,const char* where) {if(FAILED(hr)) {std::cerr<<where<<" HRESULT 0x"<<std::hex<<hr<<"\n";throw std::runtime_error(where);}}
// solid: when non-null, every frame is YUY2 {y,u,y,v} instead of the numbered test pattern.
// real: behave like a capture card. Its clock runs 0.1% slower than the advertised rate
// (59.94 instead of 60) and every frame arrives 0..4 ms late (USB, driver, JPEG decoding).
static int produce(const std::wstring& id,int w,int h,long long interval,int seconds,const int* solid=nullptr,bool real=false) {
    multi::Channel c;if(!c.open(id,true))return 2;
    c.begin(uint32_t(w),uint32_t(h),interval);
    auto end=std::chrono::steady_clock::now()+std::chrono::seconds(seconds),next=std::chrono::steady_clock::now();
    uint32_t sequence=0;timeBeginPeriod(1);
    while(std::chrono::steady_clock::now()<end) {
        ++sequence;auto slot=c.writeSlot();
        if(solid){for(size_t i=0;i<size_t(w)*h*2;i+=4){slot[i]=uint8_t(solid[0]);slot[i+1]=uint8_t(solid[1]);slot[i+2]=uint8_t(solid[0]);slot[i+3]=uint8_t(solid[2]);}}
        else{memset(slot,sequence%200+20,size_t(w)*h*2);
        memcpy(slot,&sequence,4);memcpy(slot+size_t(w)*h*2-4,&sequence,4);}
        c.publish();
        next+=std::chrono::nanoseconds(real?interval*1001/10:interval*100);
        const auto late=real?std::chrono::microseconds((sequence*7919u)%4000u):std::chrono::microseconds(0);
        std::this_thread::sleep_until(next+late);
    }
    c.stop();timeEndPeriod(1);
    std::cout<<"{\"Produced\":"<<sequence<<"}\n";return 0;
}
static int consume(const std::wstring& id,long long requested,int seconds,bool physical=false) {
    CLSID cls;check(CLSIDFromString(id.c_str(),&cls),"CLSID");
    ComPtr<IBaseFilter> source;check(CoCreateInstance(cls,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&source)),"activate camera");
    ComPtr<IGraphBuilder> graph;ComPtr<ICaptureGraphBuilder2> builder;ComPtr<IMediaControl> control;
    if(!CreateFilterGraph(&graph,&builder,&control))return 3;
    ComPtr<IPin> pin;check(source->FindPin(L"Output Pin",&pin),"output pin");
    ComQIPtr<IAMStreamConfig> format(pin);if(!format)return 4;
    MediaTypePtr type;check(format->GetFormat(&type),"get format");
    auto info=reinterpret_cast<VIDEOINFOHEADER*>(type->pbFormat);
    int w=info->bmiHeader.biWidth,h=info->bmiHeader.biHeight;
    // The first format is the source rate; a 30 FPS client of a 60 FPS source should get every 2nd frame.
    const long long step=std::max(1LL,std::llround(double(requested)/double(info->AvgTimePerFrame)));
    info->bmiHeader.biWidth+=2;
    if(SUCCEEDED(format->SetFormat(type)))throw std::runtime_error("accepted invalid dimensions");
    info->bmiHeader.biWidth=w;info->AvgTimePerFrame=requested;check(format->SetFormat(type),"set FPS");
    std::atomic<int> samples{0},unique{0},torn{0},black{0},badTime{0},repeats{0},irregular{0},skipped{0};uint32_t last=0;REFERENCE_TIME prev=-1;
    // Physical source only: identical consecutive samples, arrival intervals, frames published meanwhile.
    int same=0,late=0;uint64_t lastHash=0;double minMs=1e9,maxMs=0;std::chrono::steady_clock::time_point lastArrival{};
    multi::Channel channel;if(physical)channel.open(id,false);
    PinCaptureInfo sinkInfo;sinkInfo.expectedMajorType=MEDIATYPE_Video;sinkInfo.expectedSubType=MEDIASUBTYPE_YUY2;
    sinkInfo.callback=[&](IMediaSample* sample){BYTE* data; if(FAILED(sample->GetPointer(&data)))return;
        if(sample->GetActualDataLength()!=w*h*2){++torn;return;}
        uint32_t a,b;memcpy(&a,data,4);memcpy(&b,data+size_t(w)*h*2-4,4);
        if(!physical && a==0x80108010){++black;return;}
        if(!physical && a!=b)++torn;
        // Pacing: the same frame twice (repeat), a jump other than `step` (irregular), and a
        // jump larger than `step` (skipped: a source frame, i.e. motion, was lost).
        if(!physical && last){if(a==last)++repeats;else if(a-last!=uint32_t(step)){++irregular;if(a-last>uint32_t(step))++skipped;}}
        if(a!=last){++unique;last=a;}++samples;
        REFERENCE_TIME t,end;if(SUCCEEDED(sample->GetTime(&t,&end))){if(t<=prev)++badTime;prev=t;}
        if(physical){
            // Live video: a sample identical to the previous one is a repeat (sparse FNV-1a hash).
            uint64_t hash=1469598103934665603ULL;
            for(size_t i=0;i<size_t(w)*h*2;i+=61){hash^=data[i];hash*=1099511628211ULL;}
            if(hash==lastHash)++same;lastHash=hash;
            const auto now=std::chrono::steady_clock::now();
            if(lastArrival.time_since_epoch().count()){
                const double ms=std::chrono::duration<double,std::milli>(now-lastArrival).count();
                minMs=std::min(minMs,ms);maxMs=std::max(maxMs,ms);if(ms>25)++late;
            }
            lastArrival=now;
        }
    };
    ComPtr<IBaseFilter> sink=new CaptureFilter(sinkInfo);
    check(graph->AddFilter(source,L"MultiCam test source"),"add source");check(graph->AddFilter(sink,L"MultiCam test sink"),"add sink");
    check(builder->RenderStream(&PIN_CATEGORY_CAPTURE,&MEDIATYPE_Video,source,nullptr,sink),"connect");
    // Exercise the preroll-to-Run clock transition, not just a direct Run.
    if(!physical){check(control->Pause(),"pause");std::this_thread::sleep_for(std::chrono::milliseconds(100));}
    check(control->Run(),"run");
    std::this_thread::sleep_for(std::chrono::seconds(1));  // let the stream settle
    const int samples0=samples;const uint32_t published0=channel.valid()?channel.latest():0;
    std::this_thread::sleep_for(std::chrono::seconds(seconds));
    const int delivered=samples-samples0;const uint32_t published=channel.valid()?channel.latest()-published0:0;
    check(control->Stop(),"stop");
    std::cout<<"{\"Samples\":"<<samples<<",\"Unique\":"<<unique<<",\"Torn\":"<<torn<<",\"Black\":"<<black<<",\"BadTime\":"<<badTime<<",\"Repeats\":"<<repeats<<",\"Irregular\":"<<irregular<<",\"Skipped\":"<<skipped;
    if(physical)std::cout<<",\"Delivered\":"<<delivered<<",\"Published\":"<<published<<",\"Same\":"<<same<<",\"MinMs\":"<<minMs<<",\"MaxMs\":"<<maxMs<<",\"Late25\":"<<late;
    std::cout<<"}\n";
    return samples>int(seconds*10000000.0/requested*0.8)&&torn==0&&badTime==0?0:5;
}
// Replays a recorded sequence of frame intervals (ms, one per line), looping it: a real card
// is not regular, it stalls for a few frames and then delivers a burst.
static int producePattern(const std::wstring& id,int w,int h,long long interval,int seconds,const std::wstring& file) {
    std::vector<double> gaps; std::ifstream in(file.c_str());
    for(double g;in>>g;)gaps.push_back(g); if(gaps.empty())return 3;
    multi::Channel c;if(!c.open(id,true))return 2;
    c.begin(uint32_t(w),uint32_t(h),interval);timeBeginPeriod(1);
    auto start=std::chrono::steady_clock::now(),end=start+std::chrono::seconds(seconds),next=start;
    uint32_t sequence=0;
    while(next<end) {
        next+=std::chrono::microseconds(llround(gaps[sequence%gaps.size()]*1000));
        std::this_thread::sleep_until(next);
        ++sequence;auto slot=c.writeSlot();
        memset(slot,sequence%200+20,size_t(w)*h*2);memcpy(slot,&sequence,4);memcpy(slot+size_t(w)*h*2-4,&sequence,4);
        c.publish();
    }
    c.stop();timeEndPeriod(1);
    std::cout<<"{\"Produced\":"<<sequence<<"}\n";return 0;
}
// Holds a demand slot for a while, like an app using the camera (panel tests).
static int demand(const std::wstring& id,int seconds) {
    multi::DemandClient client;if(!client.begin(id))return 2;
    auto end=std::chrono::steady_clock::now()+std::chrono::seconds(seconds);
    while(std::chrono::steady_clock::now()<end){client.beat();std::this_thread::sleep_for(std::chrono::milliseconds(200));}
    client.end();std::cout<<"{\"Demand\":1}\n";return 0;
}
int wmain(int argc,wchar_t** argv){CoInitializeEx(nullptr,COINIT_MULTITHREADED);int result=1;
    try { if(argc==7&&std::wstring(argv[1])==L"--produce")result=produce(argv[2],std::stoi(argv[3]),std::stoi(argv[4]),std::stoll(argv[5]),std::stoi(argv[6]));
        else if(argc==10&&std::wstring(argv[1])==L"--produce-solid"){const int yuv[3]={std::stoi(argv[7]),std::stoi(argv[8]),std::stoi(argv[9])};
            result=produce(argv[2],std::stoi(argv[3]),std::stoi(argv[4]),std::stoll(argv[5]),std::stoi(argv[6]),yuv);}
        else if(argc==7&&std::wstring(argv[1])==L"--produce-real")result=produce(argv[2],std::stoi(argv[3]),std::stoi(argv[4]),std::stoll(argv[5]),std::stoi(argv[6]),nullptr,true);
        else if(argc==8&&std::wstring(argv[1])==L"--produce-pattern")result=producePattern(argv[2],std::stoi(argv[3]),std::stoi(argv[4]),std::stoll(argv[5]),std::stoi(argv[6]),argv[7]);
        else if(argc==4&&std::wstring(argv[1])==L"--demand")result=demand(argv[2],std::stoi(argv[3]));
        else if(argc==5&&std::wstring(argv[1])==L"--consume")result=consume(argv[2],std::stoll(argv[3]),std::stoi(argv[4]));
        else if(argc==5&&std::wstring(argv[1])==L"--observe")result=consume(argv[2],std::stoll(argv[3]),std::stoi(argv[4]),true);
    }catch(const std::exception&e){std::cerr<<e.what()<<"\n";result=10;}CoUninitialize();return result;
}
