#if defined(WOWDECOMP_ARCH_64)

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <windows.h>
#include "Engine/Source/Gx/CGxDevice/CGxDevice.h"
#include "Engine/Source/Gx/Gx.h"
#include "Engine/Source/Storm/H/storm.h"

LRESULT CALLBACK WindowProcGx(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);

// Platform layer for the D3d11 backend, lifted from woooowcata's Win32/CGxDevice.cpp.
//
// Only what the D3d11 backend actually calls: the monitor enumeration its adapter code walks, and
// the window class / window / cursor plumbing DeviceCreate uses when it creates its own window.
// 3.3.5a kept private copies of these inside CGxDeviceD3d.cpp, which is now retired.

static int CGxMonitorModeSort(const void* a, const void* b) {
    const CGxMonitorMode* modeA = static_cast<const CGxMonitorMode*>(a);
    const CGxMonitorMode* modeB = static_cast<const CGxMonitorMode*>(b);

    if (modeA->m_size.x < modeB->m_size.x) {
        return -1;
    }

    if (modeA->m_size.x > modeB->m_size.x) {
        return 1;
    }

    if (modeA->m_size.y < modeB->m_size.y) {
        return -1;
    }

    if (modeA->m_size.y > modeB->m_size.y) {
        return 1;
    }

    if (modeA->m_bpp < modeB->m_bpp) {
        return -1;
    }

    if (modeA->m_bpp > modeB->m_bpp) {
        return 1;
    }

    // Denom is file/monitor supplied; guard it rather than dividing by zero.
    uint32_t refreshB = modeB->m_refreshRateDenom ? modeB->m_refreshRate / modeB->m_refreshRateDenom : modeB->m_refreshRate;
    uint32_t refreshA = modeA->m_refreshRateDenom ? modeA->m_refreshRate / modeA->m_refreshRateDenom : modeA->m_refreshRate;

    if (refreshB > refreshA) {
        return -1;
    }

    return refreshB < refreshA;
}

struct MonitorEnumData {
    int32_t primary;
    TSGrowableArray<void*>* monitors;
};

// OFFSET: 0x0084AAB0/0x140578FC0
static BOOL CALLBACK MonitorEnumProc(HMONITOR monitor, HDC dc, LPRECT rect, LPARAM param) {
    MonitorEnumData* data = reinterpret_cast<MonitorEnumData*>(param);

    for (uint32_t i = 0; i < data->monitors->Count(); i++) {
        if ((*data->monitors)[i] == monitor) {
            return TRUE;
        }
    }

    if (data->primary == -1) {
        MONITORINFO info;
        info.cbSize = sizeof(info);

        if (GetMonitorInfoA(monitor, &info) && (info.dwFlags & MONITORINFOF_PRIMARY)) {
            data->primary = data->monitors->Count();
        }
    }

    void* handle = monitor;
    data->monitors->New(handle);

    return TRUE;
}


// OFFSET: 0x0084AAB0/0x140578FC0
// OFFSET: 0x0084ADE0/0x140579420
void CGxDevice::AdapterInitMonitors() {
    CGxDevice::s_monitors.Clear();

    void* primary = NULL;
    CGxDevice::s_monitors.New(primary);

    MonitorEnumData data;
    data.primary = -1;
    data.monitors = &CGxDevice::s_monitors;
    EnumDisplayMonitors(NULL, NULL, MonitorEnumProc, reinterpret_cast<LPARAM>(&data));

    if (data.primary != -1) {
        CGxDevice::s_monitors[0] = CGxDevice::s_monitors[data.primary];
    } else if (CGxDevice::s_monitors.Count() > 1) {
        CGxDevice::s_monitors[0] = CGxDevice::s_monitors[1];
    } else {
        CGxDevice::s_monitors.Clear();
    }

    CGxDevice::s_monitorsInit = 1;
}


// OFFSET: 0x00843DB0/0x140570310
void* CGxDevice::AdapterGetMonitorHandle(uint32_t monitor) {
    if (!CGxDevice::s_monitorsInit) {
        CGxDevice::AdapterInitMonitors();
    }

    return monitor < CGxDevice::s_monitors.Count() ? CGxDevice::s_monitors[monitor] : NULL;
}


// OFFSET: 0x00849960/0x140577A80
void CGxDevice::IWindowClassDestroy(uint16_t* windowClass) {
    UnregisterClassA(reinterpret_cast<LPCSTR>(*windowClass), GetModuleHandleA(NULL));
    *windowClass = 0;
}


// OFFSET: 0x00849990/0x140577AB0
void CGxDevice::IWindowDestroy(void* window) {
    HWND& hwnd = *static_cast<HWND*>(window);

    if (hwnd) {
        DestroyWindow(hwnd);
    }

    hwnd = NULL;
}


// OFFSET: 0x008499D0/0x140577AF0
void CGxDevice::IWindowCursorCreate(void* window, void* icon) {
    HWND hwnd = static_cast<HWND>(window);
    HICON& cursor = *static_cast<HICON*>(icon);

    if (cursor) {
        DestroyIcon(cursor);
        cursor = NULL;
    }

    BITMAPV5HEADER header;
    memset(&header, 0, sizeof(header));
    header.bV5Size = sizeof(header);
    header.bV5Width = 32;
    header.bV5Height = -32;
    header.bV5Planes = 1;
    header.bV5BitCount = 32;
    header.bV5Compression = BI_BITFIELDS;
    header.bV5RedMask = 0x00FF0000;
    header.bV5GreenMask = 0x0000FF00;
    header.bV5BlueMask = 0x000000FF;
    header.bV5AlphaMask = 0xFF000000;

    void* bits = NULL;
    HDC dc = GetDC(hwnd);
    HBITMAP color = CreateDIBSection(dc, reinterpret_cast<BITMAPINFO*>(&header), DIB_RGB_COLORS, &bits, NULL, 0);
    ReleaseDC(hwnd, dc);

    for (uint32_t i = 0; i < 1024; i += 32) {
        for (uint32_t j = 0; j < 32; j++) {
            static_cast<uint32_t*>(bits)[i + j] = CGxDevice::s_cursor[i + j];
        }
    }

    HBITMAP mask = CreateBitmap(32, 32, 1, 1, NULL);

    ICONINFO info;
    info.fIcon = FALSE;
    info.xHotspot = CGxDevice::s_cursorHotspotX;
    info.yHotspot = CGxDevice::s_cursorHotspotY;
    info.hbmMask = mask;
    info.hbmColor = color;
    cursor = CreateIconIndirect(&info);

    SetClassLongA(hwnd, GCLP_HCURSOR, reinterpret_cast<LONG>(cursor));
    SetCursor(cursor);
    DeleteObject(mask);
    DeleteObject(color);
}


// OFFSET: 0x00849B50/0x140577CD0
void CGxDevice::IWindowCursorDestroy(void* icon) {
    HICON& cursor = *static_cast<HICON*>(icon);

    if (cursor) {
        DestroyIcon(cursor);
        cursor = NULL;
    }
}


// OFFSET: 0x0084A6B0/0x140578470
uint16_t CGxDevice::IWindowClassCreate() {
    HINSTANCE instance = GetModuleHandleA(NULL);

    WNDCLASSEXA windowClass;
    memset(&windowClass, 0, sizeof(windowClass));
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_OWNDC;
    windowClass.lpfnWndProc = WindowProcGx;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = "GxWindowClass";
    windowClass.hIcon = static_cast<HICON>(LoadImageA(instance, "BlizzardIcon.ico", IMAGE_ICON, 0, 0, LR_DEFAULTSIZE));
    windowClass.hCursor = NULL;

    return RegisterClassExA(&windowClass);
}


// OFFSET: 0x0084A720/0x140578510
int32_t CGxDevice::IWindowCreate(void* window, CGxFormat& format, int32_t show) {
    HWND& hwnd = *static_cast<HWND*>(window);
    HINSTANCE instance = GetModuleHandleA(NULL);
    DWORD style;

    if (!format.m_window) {
        style = WS_POPUP | WS_CLIPSIBLINGS | WS_CLIPCHILDREN | WS_SYSMENU;
    } else if (format.m_maximize == 1) {
        style = WS_POPUP | WS_VISIBLE;
    } else if (format.m_maximize == 2) {
        style = WS_POPUP;
    } else {
        style = WS_OVERLAPPEDWINDOW | WS_CLIPSIBLINGS | WS_CLIPCHILDREN;
    }

    float aspectRatio = 0.0f;

    if (format.m_aspect) {
        int32_t width = format.m_size.x;

        if (width) {
            int32_t height = format.m_size.y;

            if (height) {
                aspectRatio = static_cast<float>(width) / static_cast<float>(height);
            }
        }
    }

    HMONITOR monitor = static_cast<HMONITOR>(CGxDevice::AdapterGetMonitorHandle(format.m_monitor));

    if (monitor) {
        LONG right = format.m_size.x;
        LONG bottom = format.m_size.y;
        LONG left = 0;
        LONG top = 0;

        MONITORINFO info;
        info.cbSize = sizeof(info);

        if (GetMonitorInfoA(monitor, &info)) {
            if (format.m_window && format.m_maximize != 1) {
                left = info.rcWork.left;
                top = info.rcWork.top;
                right = info.rcWork.right;
                bottom = info.rcWork.bottom;
            } else {
                left = info.rcMonitor.left;
                top = info.rcMonitor.top;
                right = info.rcMonitor.right;
                bottom = info.rcMonitor.bottom;
            }
        }

        if (format.m_window && format.m_maximize == 1) {
            format.m_size.y = bottom - top;
            format.m_pos.x = left;
            format.m_pos.y = top;
            format.m_size.x = right - left;
        } else {
            format.m_pos.x += left;
            format.m_pos.y += top;
        }
    } else if (format.m_window && format.m_maximize == 1) {
        format.m_pos.x = 0;
        format.m_pos.y = 0;
        format.m_size.x = GetSystemMetrics(SM_CXSCREEN);
        format.m_size.y = GetSystemMetrics(SM_CYSCREEN);
    }

    if (format.m_window && format.m_maximize != 1) {
        RECT rect;
        rect.left = format.m_pos.x;
        rect.top = format.m_pos.y;
        rect.right = format.m_pos.x + format.m_size.x;
        rect.bottom = format.m_pos.y + format.m_size.y;
        AdjustWindowRectEx(&rect, style, FALSE, 0);

        CGxDevice::s_clientAdjust.x = rect.right - format.m_size.x - rect.left;
        CGxDevice::s_clientAdjust.y = rect.bottom - format.m_size.y - rect.top;

        rect.left = format.m_pos.x;
        rect.top = format.m_pos.y;
        rect.right = format.m_pos.x + format.m_size.x;
        rect.bottom = format.m_pos.y + format.m_size.y;

        if (CGxDevice::AdjustWindowRectToWorkArea(aspectRatio, &CGxDevice::s_clientAdjust, &rect)) {
            format.m_pos.x = rect.left;
            format.m_pos.y = rect.top;
            format.m_size.x = rect.right - rect.left;
            format.m_size.y = rect.bottom - rect.top;
        }
    }

    int32_t width = format.m_size.x;
    int32_t height = format.m_size.y;

    if (format.m_window && format.m_maximize != 1 && width && height) {
        width += CGxDevice::s_clientAdjust.x;
        height += CGxDevice::s_clientAdjust.y;
    } else {
        if (!width) {
            width = CW_USEDEFAULT;
        }

        if (!height) {
            height = CW_USEDEFAULT;
        }
    }

    hwnd = CreateWindowExA(WS_EX_APPWINDOW, "GxWindowClass", "World of Warcraft", style, format.m_pos.x, format.m_pos.y, width, height, NULL, NULL, instance, this);

    if (show && hwnd && format.m_maximize != 2) {
        ShowWindow(hwnd, SW_SHOWNORMAL);
    }

    return hwnd != NULL;
}


// OFFSET: 0x0084A180/0x140577EB0
LRESULT CALLBACK WindowProcGx(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    CGxDevice* device = reinterpret_cast<CGxDevice*>(GetWindowLongPtrA(hwnd, GWLP_USERDATA));

    // Anything Windows sends before WM_NCCREATE has no device attached yet (and a window created without
    // lpCreateParams never will). Hand those to DefWindowProc instead of dereferencing null.
    if (!device && message != WM_NCCREATE) {
        return DefWindowProcA(hwnd, message, wparam, lparam);
    }

    switch (message) {
        case WM_NCCREATE:
            // The device pointer arrives with NCCREATE, which Windows sends before WM_CREATE. Storing it in
            // both places means every message after this one can dereference it, including the
            // GETMINMAXINFO/WM_SIZE traffic Windows generates while CreateWindowExA is still running.
            SetWindowLongPtrA(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTA*>(lparam)->lpCreateParams));
            return TRUE;

        case WM_CREATE:
            SetWindowLongPtrA(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTA*>(lparam)->lpCreateParams));
            return 0;

        case WM_DESTROY:
            device->DeviceWM(GxWM_Destroy, 0, 0);
            return 0;

        case WM_SIZE: {
            CRect rect;
            rect.minY = 0.0f;
            rect.minX = 0.0f;
            rect.maxY = HIWORD(lparam);
            rect.maxX = LOWORD(lparam);

            uint32_t resizeType = 0;

            if (wparam == SIZE_MINIMIZED) {
                resizeType = 1;
            } else if (wparam == SIZE_MAXHIDE) {
                resizeType = 2;
            }

            device->DeviceWM(GxWM_Size, reinterpret_cast<uintptr_t>(&rect), resizeType);
            break;
        }

        case WM_ACTIVATE:
            if (LOWORD(wparam) == WA_INACTIVE) {
                if (!device->IDevIsWindowed()) {
                    CRect rect;
                    rect.minY = 0.0f;
                    rect.minX = 0.0f;
                    rect.maxY = 0.0f;
                    rect.maxX = 0.0f;
                    device->DeviceWM(GxWM_Size, reinterpret_cast<uintptr_t>(&rect), 1);
                }
            } else if (LOWORD(wparam) == WA_ACTIVE && !device->IDevIsWindowed()) {
                CRect rect;
                rect.minY = 0.0f;
                rect.minX = 0.0f;
                rect.maxY = 0.0f;
                rect.maxX = 0.0f;
                GxCapsWindowSizeInScreenCoords(rect);
                device->DeviceWM(GxWM_Size, reinterpret_cast<uintptr_t>(&rect), 3);
            }

            break;

        case WM_SETFOCUS:
            device->DeviceWM(GxWM_SetFocus, 0, 0);
            return 0;

        case WM_KILLFOCUS:
            device->DeviceWM(GxWM_KillFocus, 0, 0);
            return 0;

        case WM_PAINT: {
            PAINTSTRUCT paint;
            BeginPaint(hwnd, &paint);
            EndPaint(hwnd, &paint);
            return 0;
        }

        case WM_ERASEBKGND:
            return 0;

        case WM_SETCURSOR:
            if (!device) {
                return DefWindowProcA(hwnd, message, wparam, lparam);
            }

            if (LOWORD(lparam) == HTCLIENT && device->ISetCursor()) {
                return 1;
            }

            break;

        case WM_DISPLAYCHANGE:
            CGxDevice::AdapterResetMonitors();

            if (device->m_format.m_monitor >= static_cast<int32_t>(CGxDevice::AdapterMonitorCount())) {
                device->m_format.m_monitor = 0;
            }

            device->NotifyOnDisplayChange(device->m_format.m_monitor);

            if (!device->IDevIsWindowed()) {
                CRect rect;
                rect.minY = 0.0f;
                rect.minX = 0.0f;
                rect.maxY = HIWORD(lparam);
                rect.maxX = LOWORD(lparam);

                MONITORINFO info;
                info.cbSize = sizeof(info);

                if (GetMonitorInfoA(static_cast<HMONITOR>(CGxDevice::AdapterGetMonitorHandle(device->m_format.m_monitor)), &info)) {
                    rect.maxX = static_cast<float>(info.rcMonitor.right) - static_cast<float>(info.rcMonitor.left);
                    rect.maxY = static_cast<float>(info.rcMonitor.bottom) - static_cast<float>(info.rcMonitor.top);
                }

                device->DeviceWM(GxWM_DisplayChange, reinterpret_cast<uintptr_t>(&rect), 0);
            }

            break;

        case WM_SYSCOMMAND:
            if (wparam == SC_SCREENSAVE || wparam == SC_MONITORPOWER) {
                return 0;
            }

            break;

        case WM_SIZING: {
            RECT* sizingRect = reinterpret_cast<RECT*>(lparam);

            CiRect rect;
            rect.minX = sizingRect->left;
            rect.minY = sizingRect->top;
            rect.maxX = sizingRect->right;
            rect.maxY = sizingRect->bottom;

            if (rect.maxX - rect.minX <= 0 || rect.maxY - rect.minY <= 0) {
                return 0;
            }

            switch (wparam) {
                case WMSZ_LEFT:
                    CGxDevice::ClampRectLeft(rect);
                    CGxDevice::ClampRectTopByAspect(rect);
                    break;

                case WMSZ_RIGHT:
                    CGxDevice::ClampRectRight(rect);
                    CGxDevice::ClampRectBottomByAspect(rect);
                    break;

                case WMSZ_TOP:
                    CGxDevice::ClampRectTop(rect);
                    CGxDevice::ClampRectLeftByAspect(rect);
                    break;

                case WMSZ_TOPLEFT:
                    CGxDevice::ClampRectTop(rect);
                    CGxDevice::ClampRectLeft(rect);
                    CGxDevice::ClampRectTopByAspect(rect);
                    CGxDevice::ClampRectLeftByAspect(rect);
                    break;

                case WMSZ_TOPRIGHT:
                    CGxDevice::ClampRectTop(rect);
                    CGxDevice::ClampRectRight(rect);
                    CGxDevice::ClampRectTopByAspect(rect);
                    CGxDevice::ClampRectRightByAspect(rect);
                    break;

                case WMSZ_BOTTOM:
                    CGxDevice::ClampRectBottom(rect);
                    CGxDevice::ClampRectRightByAspect(rect);
                    break;

                case WMSZ_BOTTOMLEFT:
                    CGxDevice::ClampRectBottom(rect);
                    CGxDevice::ClampRectLeft(rect);
                    CGxDevice::ClampRectLeftByAspect(rect);
                    CGxDevice::ClampRectBottomByAspect(rect);
                    break;

                case WMSZ_BOTTOMRIGHT:
                    CGxDevice::ClampRectBottom(rect);
                    CGxDevice::ClampRectRight(rect);
                    CGxDevice::ClampRectBottomByAspect(rect);
                    CGxDevice::ClampRectRightByAspect(rect);
                    break;

                default:
                    break;
            }

            sizingRect->left = rect.minX;
            sizingRect->top = rect.minY;
            sizingRect->right = rect.maxX;
            sizingRect->bottom = rect.maxY;
            return 1;
        }

        default:
            break;
    }

    if (device && device->m_windowProc) {
        return device->m_windowProc(hwnd, message, wparam, lparam);
    }

    return DefWindowProcA(hwnd, message, wparam, lparam);
}

#endif
