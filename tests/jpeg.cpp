// Compare the optimized conversion with the original per-pixel formula using
// synthetic JPEGs. No camera is opened and no video is recorded.
#include "../native/jpeg.hpp"
#include <chrono>
#include <cstring>
#include <iostream>

template<class T> T jpegApi(const char* name) {
    auto fn=reinterpret_cast<T>(GetProcAddress(GetModuleHandleW(L"turbojpeg.dll"),name));
    if(!fn)throw std::runtime_error(name);
    return fn;
}
#define JPEG_API(name) auto name=jpegApi<decltype(&::name)>(#name)

int main() {
    try {
        JpegDecoder decoder;
        JPEG_API(tjInitCompress);JPEG_API(tjInitDecompress);JPEG_API(tjCompress2);
        JPEG_API(tjDestroy);JPEG_API(tjFree);JPEG_API(tjBufSizeYUV2);
        JPEG_API(tjPlaneWidth);JPEG_API(tjPlaneHeight);JPEG_API(tjDecompressToYUV2);
        JPEG_API(tjGetErrorStr2);
        auto compressor=tjInitCompress(),reference=tjInitDecompress();
        if(!compressor||!reference)throw std::runtime_error("JPEG initialization");
        unsigned cases=0;double benchmarkMs=0;
        for(auto dimensions:{std::pair<int,int>{18,9},{1280,720},{1920,1080}}){
            auto [w,h]=dimensions;
            std::vector<unsigned char> rgb(size_t(w)*h*3);
            for(size_t i=0;i<rgb.size();++i)rgb[i]=static_cast<unsigned char>((i*17+(i/97)*31)%256);
            // Formats supported by tjCompress2 in the bundled runtime.
            for(int sub:{TJSAMP_444,TJSAMP_422,TJSAMP_420,TJSAMP_GRAY,TJSAMP_440,TJSAMP_411,TJSAMP_441}){
                unsigned char* jpeg=nullptr;unsigned long bytes=0;
                if(tjCompress2(compressor,rgb.data(),w,0,h,TJPF_RGB,&jpeg,&bytes,sub,85,0)<0)
                    throw std::runtime_error(std::string("JPEG compression, subsampling ")+std::to_string(sub)+": "+tjGetErrorStr2(compressor));
                auto actual=decoder.decode(jpeg,bytes,w,h);
                if(!actual)throw std::runtime_error("JPEG decode");
                std::vector<unsigned char> planes(tjBufSizeYUV2(w,1,h,sub));
                if(tjDecompressToYUV2(reference,jpeg,bytes,planes.data(),w,1,h,0)<0)
                    throw std::runtime_error("Reference decode");
                int yw=tjPlaneWidth(0,w,sub),yh=tjPlaneHeight(0,h,sub);
                bool gray=sub==TJSAMP_GRAY;
                int cw=gray?0:tjPlaneWidth(1,w,sub),ch=gray?0:tjPlaneHeight(1,h,sub);
                int hs=gray?1:(yw+cw-1)/cw,vs=gray?1:(yh+ch-1)/ch;
                auto yp=planes.data(),up=yp+size_t(yw)*yh,vp=up+size_t(cw)*ch;
                for(int y=0;y<h;++y)for(int x=0;x<w;x+=2){
                    int u=128,v=128;
                    if(!gray){size_t a=size_t(y/vs)*cw+x/hs,b=size_t(y/vs)*cw+(x+1)/hs;
                        u=(int(up[a])+up[b]+1)/2;v=(int(vp[a])+vp[b]+1)/2;}
                    unsigned char expected[]{
                        static_cast<unsigned char>(16+(int(yp[size_t(y)*yw+x])*219+127)/255),
                        static_cast<unsigned char>(16+(u*224+127)/255),
                        static_cast<unsigned char>(16+(int(yp[size_t(y)*yw+x+1])*219+127)/255),
                        static_cast<unsigned char>(16+(v*224+127)/255)};
                    if(std::memcmp(actual+(size_t(y)*w+x)*2,expected,4))
                        throw std::runtime_error("YUY2 pixel mismatch");
                }
                if(decoder.decode(jpeg,bytes,w+2,h))throw std::runtime_error("Accepted wrong dimensions");
                if(w==1920&&sub==TJSAMP_422){
                    auto start=std::chrono::steady_clock::now();
                    for(int n=0;n<30;++n)if(!decoder.decode(jpeg,bytes,w,h))throw std::runtime_error("Benchmark decode");
                    benchmarkMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count()/30;
                }
                tjFree(jpeg);++cases;
            }
        }
        unsigned char invalid[16]{};
        if(decoder.decode(invalid,sizeof(invalid),1920,1080))throw std::runtime_error("Accepted invalid JPEG");
        tjDestroy(compressor);tjDestroy(reference);
        std::cout<<"{\"Cases\":"<<cases<<",\"PixelMismatches\":0,\"FullHd422DecodeMs\":"<<benchmarkMs<<"}\n";
        return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
