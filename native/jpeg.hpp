#pragma once
#include "../third_party/turbojpeg/turbojpeg.h"
#include <windows.h>
#include <vector>
#include <string>
#include <stdexcept>
#include <algorithm>

class JpegDecoder {
    HMODULE module_=nullptr; tjhandle decoder_=nullptr;
    decltype(&tjInitDecompress) init_{};
    decltype(&tjDecompressHeader3) header_{};
    decltype(&tjDecompressToYUV2) decode_{};
    decltype(&tjBufSizeYUV2) size_{};
    decltype(&tjPlaneWidth) width_{};
    decltype(&tjPlaneHeight) height_{};
    decltype(&tjDestroy) destroy_{};
    std::vector<unsigned char> planes_,packed_;
    // JPEG YCbCr is full range; DirectShow YUY2 uses studio range. Tables avoid the
    // multiply/divide per byte (about twice as fast at 1080p).
    unsigned char luma_[256],chroma_[256];
    template<class T> void get(T& func,const char* name) {
        func=reinterpret_cast<T>(GetProcAddress(module_,name));
        if(!func)throw std::runtime_error("Decodificador JPEG incompativel.");
    }
    template<int HS> void pack(int w,int h,int yw,int yh,int cw,int ch,int vs) {
        auto yplane=planes_.data();auto uplane=yplane+size_t(yw)*yh;auto vplane=uplane+size_t(cw)*ch;
        for(int y=0;y<h;++y){
            auto source=yplane+size_t(y)*yw;auto out=packed_.data()+size_t(y)*w*2;
            // Vertical indexing is constant for the whole row. Horizontal
            // subsampling is a template constant, avoiding division per pixel.
            size_t row=HS==0?0:size_t(y/vs)*cw;
            for(int x=0;x<w;x+=2){
                int u=128,v=128;
                if constexpr(HS==1){
                    size_t a=row+x;
                    u=(int(uplane[a])+uplane[a+1]+1)/2;v=(int(vplane[a])+vplane[a+1]+1)/2;
                }else if constexpr(HS>1){
                    size_t a=row+x/HS;
                    u=uplane[a];v=vplane[a];
                }
                out[x*2]=luma_[source[x]];
                out[x*2+1]=chroma_[u];
                out[x*2+2]=luma_[source[x+1]];
                out[x*2+3]=chroma_[v];
            }
        }
    }
public:
    JpegDecoder() {
        for(int i=0;i<256;++i){
            luma_[i]=static_cast<unsigned char>(16+(i*219+127)/255);
            chroma_[i]=static_cast<unsigned char>(16+(i*224+127)/255);
        }
        wchar_t exe[32768]{};GetModuleFileNameW(nullptr,exe,32768);
        std::wstring path=exe;path=path.substr(0,path.find_last_of(L"\\/"))+L"\\turbojpeg.dll";
        module_=LoadLibraryExW(path.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if(!module_)throw std::runtime_error("turbojpeg.dll ausente na pasta do worker de captura.");
        try {
            get(init_,"tjInitDecompress");get(header_,"tjDecompressHeader3");get(decode_,"tjDecompressToYUV2");
            get(size_,"tjBufSizeYUV2");get(width_,"tjPlaneWidth");get(height_,"tjPlaneHeight");get(destroy_,"tjDestroy");
            decoder_=init_();if(!decoder_)throw std::runtime_error("Falha ao criar decodificador JPEG.");
        }catch(...){FreeLibrary(module_);module_=nullptr;throw;}
    }
    ~JpegDecoder(){if(decoder_)destroy_(decoder_);if(module_)FreeLibrary(module_);}
    unsigned char* decode(const unsigned char* jpeg,size_t bytes,int expectedW,int expectedH) {
        if(bytes>ULONG_MAX)return nullptr;
        int w=0,h=0,subsamp=0,colorspace=0;
        if(header_(decoder_,jpeg,static_cast<unsigned long>(bytes),&w,&h,&subsamp,&colorspace)<0 ||
            w!=expectedW || h!=expectedH || (colorspace!=TJCS_YCbCr && colorspace!=TJCS_GRAY))return nullptr;
        auto size=size_(w,1,h,subsamp);if(size==static_cast<unsigned long>(-1)||size>128*1024*1024)return nullptr;
        planes_.resize(size);packed_.resize(size_t(w)*h*2);
        if(decode_(decoder_,jpeg,static_cast<unsigned long>(bytes),planes_.data(),w,1,h,0)<0)return nullptr;
        int yw=width_(0,w,subsamp),yh=height_(0,h,subsamp);
        bool gray=subsamp==TJSAMP_GRAY;
        int cw=gray?0:width_(1,w,subsamp),ch=gray?0:height_(1,h,subsamp);
        if(yw<=0||yh<=0||(!gray&&(cw<=0||ch<=0)))return nullptr;
        int hs=gray?1:(yw+cw-1)/cw,vs=gray?1:(yh+ch-1)/ch;
        if(gray)pack<0>(w,h,yw,yh,cw,ch,vs);
        else if(hs==1)pack<1>(w,h,yw,yh,cw,ch,vs);
        else if(hs==2)pack<2>(w,h,yw,yh,cw,ch,vs);
        else if(hs==4)pack<4>(w,h,yw,yh,cw,ch,vs);
        else return nullptr;
        return packed_.data();
    }
};
