#if defined(WOWDECOMP_ARCH_64)

#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <windows.h>
#include <d3d9.h>
#include "Engine/Source/Gx/CGxDevice/CGxDevice.h"
// The D3d11 adapter-inference path below calls into the backend.
#include "Engine/Source/Gx/CGxDeviceD3d11/CGxDeviceD3d11.h"

// STATIC: 0x00AD8CE8
static D3DMULTISAMPLE_TYPE s_d3dMultisampleTypes[9] = { D3DMULTISAMPLE_NONE, D3DMULTISAMPLE_2_SAMPLES, D3DMULTISAMPLE_4_SAMPLES, D3DMULTISAMPLE_6_SAMPLES, D3DMULTISAMPLE_8_SAMPLES, D3DMULTISAMPLE_10_SAMPLES, D3DMULTISAMPLE_12_SAMPLES, D3DMULTISAMPLE_14_SAMPLES, D3DMULTISAMPLE_16_SAMPLES };

// STATIC: 0x00AD8D0C
static D3DFORMAT s_d3dDepthFormats[4] = { D3DFMT_D16, D3DFMT_D24X8, D3DFMT_D24S8, D3DFMT_D32 };

// STATIC: 0x00AD8D1C
static D3DFORMAT s_d3dAdapterFormats[3] = { D3DFMT_R5G6B5, D3DFMT_X8R8G8B8, D3DFMT_A8R8G8B8 };

// STATIC: 0x00AD8D28
static D3DMULTISAMPLE_TYPE s_d3d9ExMultisampleTypes[9] = { D3DMULTISAMPLE_NONE, D3DMULTISAMPLE_2_SAMPLES, D3DMULTISAMPLE_4_SAMPLES, D3DMULTISAMPLE_6_SAMPLES, D3DMULTISAMPLE_8_SAMPLES, D3DMULTISAMPLE_10_SAMPLES, D3DMULTISAMPLE_12_SAMPLES, D3DMULTISAMPLE_14_SAMPLES, D3DMULTISAMPLE_16_SAMPLES };

// STATIC: 0x00AD8D4C
static D3DFORMAT s_d3d9ExDepthFormats[4] = { D3DFMT_D16, D3DFMT_D24X8, D3DFMT_D24S8, D3DFMT_D32 };

// STATIC: 0x00AD8D5C
static D3DFORMAT s_d3d9ExAdapterFormats[3] = { D3DFMT_R5G6B5, D3DFMT_X8R8G8B8, D3DFMT_A8R8G8B8 };

// OFFSET: 0x00682B00
int32_t CGxDevice::AdapterFormats(EGxApi api, TSGrowableArray<CGxFormat>& formats) {
    formats.SetCount(0);
    formats.m_chunk = 64;
    formats.Reserve(256, 0);

    switch (api) {
        // Only D3d11 is built; the D3d/D3d9Ex/OpenGl enumeration paths went with their backends.
        case GxApi_D3d11:
        default:
            CGxDevice::D3d11AdapterFormats(formats);
            break;
    }

    return formats.Count() != 0;
}

// OFFSET: 0x00684D70
int32_t CGxDevice::AdjustWindowRectToWorkArea(float aspectRatio, C2iVector* adjust, tagRECT* rect) {
    POINT origin = { 0, 0 };
    HMONITOR monitor = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);

    MONITORINFO info;
    info.cbSize = sizeof(info);

    if (!GetMonitorInfoA(monitor, &info)) {
        return 0;
    }

    int32_t height = rect->bottom - rect->top;
    int32_t width = rect->right - rect->left;

    rect->top = info.rcWork.top;
    rect->left = info.rcWork.left;
    rect->right = width + info.rcWork.left + adjust->x;
    rect->bottom = height + info.rcWork.top + adjust->y;

    if (rect->right > info.rcWork.right) {
        rect->right = info.rcWork.right;

        if (aspectRatio != 0.0f) {
            int32_t workWidth = info.rcWork.right - info.rcWork.left;
            int32_t maxWidth = workWidth - adjust->x;
            int32_t newWidth = maxWidth;
            float invAspect = 1.0f / aspectRatio;

            for (; newWidth >= width / 2; newWidth--) {
                float newHeight = newWidth * invAspect;

                if (newHeight - floor(newHeight) < 0.01f) {
                    break;
                }
            }

            double newHeight;

            if (newWidth < width / 2) {
                newHeight = maxWidth / aspectRatio + 0.5f;
            } else {
                workWidth = newWidth + adjust->x;
                newHeight = newWidth * invAspect;
            }

            rect->right = info.rcWork.left + workWidth;
            rect->bottom = info.rcWork.top + static_cast<int32_t>(adjust->y + floor(newHeight));
        }
    }

    if (rect->bottom > info.rcWork.bottom) {
        rect->bottom = info.rcWork.bottom;

        if (aspectRatio != 0.0f) {
            int32_t workHeight = info.rcWork.bottom - info.rcWork.top;
            int32_t maxHeight = workHeight - adjust->y;
            int32_t newHeight = maxHeight;

            for (; newHeight >= height / 2; newHeight--) {
                float newWidth = newHeight * aspectRatio;

                if (newWidth - floor(newWidth) < 0.01f) {
                    break;
                }
            }

            int32_t newWidth;

            if (newHeight < height / 2) {
                newWidth = static_cast<int32_t>(floor(maxHeight * aspectRatio + 0.5f));
            } else {
                workHeight = adjust->y + newHeight;
                newWidth = static_cast<int32_t>(floor(newHeight * aspectRatio));
            }

            rect->right = info.rcWork.left + adjust->x + newWidth;
            rect->bottom = info.rcWork.top + workHeight;
        }
    }

    int32_t spaceY = info.rcWork.bottom - rect->bottom;
    int32_t spaceX = info.rcWork.right - rect->right;

    rect->right += spaceX / 2;
    rect->left = spaceX / 2 + info.rcWork.left;
    rect->bottom += spaceY / 2;
    rect->top = spaceY / 2 + info.rcWork.top;
    rect->right -= adjust->x;
    rect->bottom -= adjust->y;

    return 1;
}

// OFFSET: 0x00689B90
static uint16_t HToI(const char* h, int32_t count) {
    uint16_t value = 0;

    while (count) {
        char c = *h;
        count--;
        value *= 16;
        h++;

        if (isxdigit(c)) {
            if (isdigit(c)) {
                value = c - '0' + value;
            } else {
                value = value + toupper(c) - 55;
            }
        }
    }

    return value;
}

// OFFSET: 0x00689C10
int32_t CGxDevice::DeviceAdapterID(uint16_t& vendorID, uint16_t& deviceID, uint32_t& driverVersionHi, uint32_t& driverVersionLo) {
    int32_t result = 0;
    vendorID = 0xFFFF;
    deviceID = 0xFFFF;
    driverVersionHi = 0;
    driverVersionLo = 0;

    DISPLAY_DEVICEA device;
    memset(&device, 0, sizeof(device));
    device.cb = sizeof(device);

    for (DWORD i = 0; EnumDisplayDevicesA(nullptr, i, &device, 0); i++) {
        if (device.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) {
            if (strlen(device.DeviceID) >= 21) {
                uint16_t vendor = HToI(device.DeviceID + 8, 4);
                uint16_t id = HToI(device.DeviceID + 17, 4);

                if (vendor && id) {
                    vendorID = vendor;
                    deviceID = id;
                    result = 1;
                }
            }

            break;
        }
    }

    if (!result) {
        // The D3d9 adapter-identifier fallback went with its backend. The DISPLAY_DEVICEA pass above already
        // recovers vendor/device for any DXGI-capable adapter, so there is nothing left to fall back to.
    }

    CGxDevice::Log("CGxDevice::DeviceAdapterID(): RET: %d, VID: %x, DID: %x, DVER: %x.%x", result, vendorID, deviceID, driverVersionHi, driverVersionLo);

    return result;
}

// OFFSET: 0x00689DB0
//
// The "is this machine good enough, and if so which tier" probe. 3.3.5a answered it with D3D9 device caps;
// this client renders through D3d11, so it asks the D3d11 backend instead (cata's CGxDevice::AdapterInfer
// does the same). ConsoleDetectDetectHardware needs a non-zero answer here when the exact vendor/device pair
// is not in VideoHardware.dbc: it sets vendorID to 0xFFFF and calls this to pick a generic tier, and exits
// with "Failed to find a suitable display device" if nothing comes back.
int32_t CGxDevice::DeviceAdapterInfer(uint16_t& deviceID) {
    if (CGxDevice::D3d11AdapterInfer(deviceID)) {
        return 1;
    }

    // Second chance. D3d11AdapterInfer only accepts an adapter that answers CheckInterfaceSupport for
    // ID3D10Device, which some drivers refuse even though they create a D3D11 device happily. Being able to
    // create one is the real question, and IHardwareSupported asks exactly that.
    if (CGxDeviceD3d11::ILoadD3dLib()) {
        int32_t creatable = CGxDeviceD3d11::IHardwareSupported();
        CGxDeviceD3d11::IUnloadD3dLib();

        if (creatable) {
            deviceID = 3;
            CGxDevice::Log("CGxDevice::DeviceAdapterInfer(): D3D11 device creatable, DID: 3");
            return 1;
        }
    }

    CGxDevice::Log("CGxDevice::DeviceAdapterInfer(): no usable D3D11 adapter");
    return 0;
}

// OFFSET: 0x00689E80
static int CGxMonitorModeSort(const void* a, const void* b) {
    const CGxMonitorMode* modeA = static_cast<const CGxMonitorMode*>(a);
    const CGxMonitorMode* modeB = static_cast<const CGxMonitorMode*>(b);

    return modeA->m_size.x * modeA->m_size.y - modeB->m_size.x * modeB->m_size.y;
}

// OFFSET: 0x00689EA0
static int32_t FindDisplayDevice(PDISPLAY_DEVICEA device, uint32_t flag) {
    DWORD i = 0;
    device->cb = sizeof(DISPLAY_DEVICEA);

    if (!EnumDisplayDevicesA(nullptr, 0, device, 0)) {
        return 0;
    }

    while ((flag & device->StateFlags) != flag) {
        if (!EnumDisplayDevicesA(nullptr, ++i, device, 0)) {
            return 0;
        }
    }

    return 1;
}

// OFFSET: 0x00689F20
int32_t CGxDevice::AdapterDesktopMode(CGxMonitorMode& mode) {
    DISPLAY_DEVICEA device;

    if (!FindDisplayDevice(&device, DISPLAY_DEVICE_ATTACHED_TO_DESKTOP | DISPLAY_DEVICE_PRIMARY_DEVICE)) {
        return 0;
    }

    DEVMODEA devMode;
    devMode.dmSize = sizeof(devMode);

    if (!EnumDisplaySettingsA(device.DeviceName, ENUM_CURRENT_SETTINGS, &devMode)) {
        return 0;
    }

    mode.m_size.x = devMode.dmPelsWidth;
    mode.m_size.y = devMode.dmPelsHeight;
    mode.m_refreshRate = devMode.dmDisplayFrequency;
    mode.m_bpp = devMode.dmBitsPerPel;

    return 1;
}

// OFFSET: 0x0068A4C0
int32_t CGxDevice::AdapterMonitorModes(TSGrowableArray<CGxMonitorMode>& modes) {
    modes.SetCount(0);

    DISPLAY_DEVICEA device;

    if (!FindDisplayDevice(&device, DISPLAY_DEVICE_PRIMARY_DEVICE)) {
        return 0;
    }

    DEVMODEA devMode;
    devMode.dmSize = sizeof(devMode);

    for (DWORD i = 0; EnumDisplaySettingsA(device.DeviceName, i, &devMode); i++) {
        if (devMode.dmPelsWidth >= 640 && devMode.dmPelsHeight >= 480 && devMode.dmBitsPerPel >= 16) {
            CGxMonitorMode* mode = modes.New();
            mode->m_size.x = devMode.dmPelsWidth;
            mode->m_size.y = devMode.dmPelsHeight;
            mode->m_bpp = devMode.dmBitsPerPel;
            mode->m_refreshRate = devMode.dmDisplayFrequency;
        }
    }

    qsort(modes.Ptr(), modes.Count(), sizeof(CGxMonitorMode), CGxMonitorModeSort);

    return modes.Count() != 0;
}

#endif
