#include "../third_party/libdshowcapture/source/dshow-base.hpp"
// MultiCam only requests generic SDR capture. Do not change vendor-wide HDR
// settings or issue HID commands to unrelated capture devices.
namespace DShow {
bool IsVendorVideoHDR(IKsPropertySet*) { return false; }
void SetVendorVideoFormat(IKsPropertySet*, bool) {}
void SetVendorTonemapperUsage(IBaseFilter*, bool) {}
}
