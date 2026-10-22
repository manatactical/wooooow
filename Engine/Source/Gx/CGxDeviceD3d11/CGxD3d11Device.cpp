#include <cstdint>
#include <cstring>
#include <new>
#include <d3d9.h>
#include "Engine/Source/Gx/CGxDeviceD3d11/CGxDeviceD3d11.h"
#include "Engine/Source/Gx/CGxBuf.h"
#include "Engine/Source/Gx/CGxPool.h"
#include "Engine/Source/Gx/Gx.h"
#include "Engine/Source/Os/Error.h"
#include "Engine/Source/Storm/H/storm.h"

typedef HRESULT (WINAPI* D3D11CreateDeviceProc)(IDXGIAdapter* adapter, D3D_DRIVER_TYPE driverType, HMODULE software, UINT flags, const D3D_FEATURE_LEVEL* featureLevels, UINT numFeatureLevels, UINT sdkVersion, ID3D11Device** device, D3D_FEATURE_LEVEL* featureLevel, ID3D11DeviceContext** context);
typedef HRESULT (WINAPI* CreateDXGIFactory1Proc)(REFIID riid, void** factory);

// STATIC: 0x00BD530C/0x1409CA468
const DXGI_FORMAT CGxDeviceD3d11::s_GxFormatToDxgiFormat[CGxFormat::Formats_Last] = { DXGI_FORMAT_B5G6R5_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_D16_UNORM, DXGI_FORMAT_D24_UNORM_S8_UINT, DXGI_FORMAT_D24_UNORM_S8_UINT, DXGI_FORMAT_D32_FLOAT };

// STATIC: 0x00BD532C/0x1409CA488
const DXGI_FORMAT CGxDeviceD3d11::s_GxFormatToDxgiTypeless[CGxFormat::Formats_Last] = { DXGI_FORMAT_B5G6R5_UNORM, DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R10G10B10A2_TYPELESS, DXGI_FORMAT_R16_TYPELESS, DXGI_FORMAT_R24G8_TYPELESS, DXGI_FORMAT_R24G8_TYPELESS, DXGI_FORMAT_R32_TYPELESS };

const uint32_t CGxDeviceD3d11::s_maxIndex[6] = { 0xFFFF, 0xFFFFF, 0xFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF };

const uint32_t CGxDeviceD3d11::s_texMaxSize2d[6] = { 0x800, 0x800, 0x1000, 0x2000, 0x2000, 0x4000 };

const uint32_t CGxDeviceD3d11::s_texMaxSizeCube[6] = { 0x200, 0x200, 0x1000, 0x2000, 0x2000, 0x4000 };

const uint32_t CGxDeviceD3d11::s_texMaxSizeVolume[6] = { 0x100, 0x100, 0x100, 0x800, 0x800, 0x800 };

const uint32_t CGxDeviceD3d11::s_maxAnisotropy[6] = { 2, 16, 16, 16, 16, 16 };

const uint32_t CGxDeviceD3d11::s_numRenderTargets[6] = { 1, 1, 4, 8, 8, 8 };

// STATIC: 0x00BD53DC/0x1409CA538
const D3D11_QUERY CGxDeviceD3d11::s_queryTypes[1] = { D3D11_QUERY_OCCLUSION };

// STATIC: 0x00EC66C0/0x140DB8828
static int32_t s_d3dLibRefCount;

// STATIC: 0x00EC66C4/0x140DB8830
static HMODULE s_d3d11Lib;

// STATIC: 0x00EC66C8/0x140DB8838
static HMODULE s_dxgiLib;

// STATIC: 0x00EC66CC/0x140DB8840
static D3D11CreateDeviceProc s_D3D11CreateDevice;

// STATIC: 0x00EC66D0/0x140DB8848
static CreateDXGIFactory1Proc s_CreateDXGIFactory1;

static int32_t s_hideCursorOnCreate = 1;

// OFFSET: 0x00842700/0x1407FDD80
static int32_t D3d11DebugLevel() {
    return 0;
}

// OFFSET: 0x00851C40/0x1405815E0
void CGxDeviceD3d11::ISetCaps(const CGxFormat& format) {
    uint32_t featureLevel = this->m_featureLevel;

    this->m_caps.m_numTmus = 8;
    this->m_caps.m_numVertexTmus = 4;
    this->m_caps.m_numShaderTmus = 16;
    this->m_caps.m_pixelCenterOnEdge = 0;
    this->m_caps.m_texelCenterOnEdge = 0;
    this->m_caps.m_numRenderTargets = s_numRenderTargets[featureLevel];
    this->m_caps.m_texMaxSize[0] = s_texMaxSize2d[featureLevel];
    this->m_caps.m_texMaxSize[1] = s_texMaxSizeCube[featureLevel];
    this->m_caps.m_texMaxSize[2] = s_texMaxSize2d[featureLevel];
    this->m_caps.m_texMaxSize[3] = s_texMaxSize2d[featureLevel];
    this->m_caps.m_texMaxSize[4] = s_texMaxSizeVolume[featureLevel];
    this->m_caps.m_texMaxSize[5] = this->m_caps.m_texMaxSize[0];
    this->m_caps.m_texMaxSize[6] = 0;
    this->m_caps.m_maxIndex = s_maxIndex[featureLevel];
    this->m_caps.m_texFilterTrilinear = 1;
    this->m_caps.m_texFilterAnisotropic = 1;
    this->m_caps.m_maxTexAnisotropy = s_maxAnisotropy[featureLevel];

    int32_t sm40 = featureLevel >= 3;
    this->m_caps.m_shaderProfiles[GxShProfile_Vs40] = sm40;
    this->m_caps.m_shaderProfiles[GxShProfile_Gs40] = sm40;
    this->m_caps.m_shaderProfiles[GxShProfile_Ps40] = sm40;
    this->m_caps.m_shaderProfiles[GxShProfile_Cs40] = sm40;
    this->m_caps.m_depthBias = 1;
    this->m_caps.m_numStreams = 32;

    int32_t sm50 = featureLevel >= 5;
    this->m_caps.m_shaderProfiles[GxShProfile_Vs50] = sm50;
    this->m_caps.m_shaderProfiles[GxShProfile_Hs50] = sm50;
    this->m_caps.m_shaderProfiles[GxShProfile_Ds50] = sm50;
    this->m_caps.m_shaderProfiles[GxShProfile_Gs50] = sm50;
    this->m_caps.m_shaderProfiles[GxShProfile_Ps50] = sm50;
    this->m_caps.m_shaderProfiles[GxShProfile_Cs50] = sm50;

    this->ChooseShaderModel(format.m_shaderModel);

    for (uint32_t i = 0; i < GxShTargets_Last; i++) {
        this->m_caps.m_shaderConstants[i] = 256;
    }

    for (uint32_t i = 0; i < GxTexFormats_Last; i++) {
        this->m_caps.m_vertexTexFmt[i] = 0;
        this->m_caps.m_texFmt[i] = 0;
        this->m_caps.m_rttFormat[i] = 0;
        this->m_caps.m_pcfFormat[i] = 0;
    }

    int32_t fl91 = this->m_featureLevel >= 1;
    int32_t fl10 = this->m_featureLevel >= 3;

    this->m_caps.m_vertexTexFmt[GxTex_Abgr8888] = 1;
    this->m_caps.m_vertexTexFmt[GxTex_Dxt1] = 1;
    this->m_caps.m_vertexTexFmt[GxTex_Dxt3] = 1;
    this->m_caps.m_vertexTexFmt[GxTex_Dxt5] = 1;
    this->m_caps.m_vertexTexFmt[GxTex_Uv88] = 1;
    this->m_caps.m_texFmt[GxTex_Abgr8888] = 1;
    this->m_caps.m_texFmt[GxTex_Dxt1] = 1;
    this->m_caps.m_texFmt[GxTex_Dxt3] = 1;
    this->m_caps.m_texFmt[GxTex_Dxt5] = 1;
    this->m_caps.m_texFmt[GxTex_Uv88] = 1;
    this->m_caps.m_rttFormat[GxTex_Abgr8888] = 1;
    this->m_caps.m_rttFormat[GxTex_D24X8] = 1;
    this->m_caps.m_pcfFormat[GxTex_D24X8] = 1;
    this->m_caps.m_pcfFormat[GxTex_D32] = 1;
    this->m_caps.m_colorWrite = 1;
    this->m_caps.unk_02BC = 1;
    this->m_caps.m_hwCursor = 1;
    this->m_caps.m_texTarget[0] = 1;
    this->m_caps.m_texTarget[1] = 1;
    this->m_caps.m_texTarget[3] = 1;
    this->m_caps.m_texTarget[4] = 1;
    this->m_caps.m_texTarget[5] = 0;
    this->m_caps.m_texTarget[6] = 0;
    this->m_caps.m_blendFactor = 1;
    this->m_caps.unk_0300 = 1;
    this->m_caps.unk_0314 = this->m_featureLevel >= 4;
    this->m_caps.unk_0304 = 1;
    this->m_caps.m_maxClipPlanes = 0;
    this->m_caps.m_texTarget[2] = 0;
    this->m_caps.m_texNonPow2 = 0;
    this->m_caps.m_pointParameters = 0;
    this->m_caps.m_pointSprite = 0;
    this->m_caps.unk_02D8[1] = 0;
    this->m_caps.unk_02D8[0] = 0;
    this->m_caps.unk_02D8[2] = 0;
    this->m_caps.unk_02D8[3] = 0;
    this->m_caps.unk_02D8[4] = 0;
    this->m_caps.m_stereoAvailable = 0;
    this->m_caps.unk_02F4 = 0;
    this->m_caps.unk_02FC = 0;
    this->m_caps.m_vertexTexFmt[GxTex_R16] = fl91;
    this->m_caps.m_vertexTexFmt[GxTex_Gr1616F] = fl91;
    this->m_caps.m_vertexTexFmt[GxTex_Abgr16161616F] = fl91;
    this->m_caps.m_vertexTexFmt[GxTex_R24X8] = fl10;
    this->m_caps.m_vertexTexFmt[GxTex_R32F] = fl91;
    this->m_caps.m_vertexTexFmt[GxTex_Abgr32323232F] = fl10;
    this->m_caps.m_vertexTexFmt[GxTex_D24X8] = fl10;
    this->m_caps.m_vertexTexFmt[GxTex_D32] = fl10;
    this->m_caps.m_texFmt[GxTex_R16] = fl91;
    this->m_caps.m_texFmt[GxTex_Gr1616F] = fl91;
    this->m_caps.m_texFmt[GxTex_Abgr16161616F] = fl91;
    this->m_caps.m_texFmt[GxTex_R24X8] = fl10;
    this->m_caps.m_texFmt[GxTex_R32F] = fl91;
    this->m_caps.m_texFmt[GxTex_Abgr32323232F] = fl10;
    this->m_caps.m_texFmt[GxTex_D24X8] = fl10;
    this->m_caps.m_texFmt[GxTex_D32] = fl10;
    this->m_caps.m_rttFormat[GxTex_Uv88] = fl10;
    this->m_caps.m_rttFormat[GxTex_R16] = fl91;
    this->m_caps.m_rttFormat[GxTex_Gr1616F] = fl91;
    this->m_caps.m_rttFormat[GxTex_Abgr16161616F] = fl91;
    this->m_caps.m_rttFormat[GxTex_R24X8] = fl10;
    this->m_caps.m_rttFormat[GxTex_R32F] = fl91;
    this->m_caps.m_rttFormat[GxTex_Abgr32323232F] = fl10;
    this->m_caps.m_occlusionQuery = fl91;
    this->m_caps.m_pointScaleMax = 0.0f;
    this->m_caps.m_vendorId = 0;
    this->m_caps.m_deviceId = 0;

    DXGI_ADAPTER_DESC desc;

    if (FAILED(this->m_dxgiAdapter->GetDesc(&desc))) {
        CGxDevice::Log("Failed to get DXGI Adapter Description");
        return;
    }

    this->m_caps.m_vendorId = desc.VendorId;
    this->m_caps.m_deviceId = desc.DeviceId;
}

// OFFSET: 0x00851FD0/0x140581A00
uint32_t CGxDeviceD3d11::IFeatureLevelIndex(D3D_FEATURE_LEVEL featureLevel) {
    switch (featureLevel) {
        case D3D_FEATURE_LEVEL_9_1:
            return 0;

        case D3D_FEATURE_LEVEL_9_2:
            return 1;

        case D3D_FEATURE_LEVEL_9_3:
            return 2;

        case D3D_FEATURE_LEVEL_10_0:
            return 3;

        case D3D_FEATURE_LEVEL_10_1:
            return 4;

        case D3D_FEATURE_LEVEL_11_0:
            return 5;

        default:
            return 0xFFFFFFFF;
    }
}

// OFFSET: 0x008538D0/0x140583460
int32_t CGxDeviceD3d11::ICreateTargets(const C2iVector& size) {
    DXGI_SWAP_CHAIN_DESC swapChainDesc;
    this->m_swapChain->GetDesc(&swapChainDesc);

    if (swapChainDesc.SampleDesc.Count > 1) {
        D3D11_TEXTURE2D_DESC resolveDesc;
        resolveDesc.Width = swapChainDesc.BufferDesc.Width;
        resolveDesc.Height = swapChainDesc.BufferDesc.Height;
        resolveDesc.MipLevels = 1;
        resolveDesc.ArraySize = 1;
        resolveDesc.Format = swapChainDesc.BufferDesc.Format;
        resolveDesc.SampleDesc.Count = 1;
        resolveDesc.SampleDesc.Quality = 0;
        resolveDesc.Usage = D3D11_USAGE_DEFAULT;
        resolveDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        resolveDesc.CPUAccessFlags = 0;
        resolveDesc.MiscFlags = 0;

        if (FAILED(this->m_d3dDevice->CreateTexture2D(&resolveDesc, NULL, &this->m_resolveTexture))) {
            return 0;
        }
    }

    this->m_backBufferViews = static_cast<TexTargetViews*>(SMemAlloc(sizeof(TexTargetViews), __FILE__, __LINE__, 0));
    this->m_backBufferViews->m_type = 1;
    this->m_backBufferViews->m_srv = NULL;
    this->m_backBufferViews->m_targetView = NULL;

    ID3D11Texture2D* backBuffer;

    if (FAILED(this->m_swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backBuffer)))) {
        return 0;
    }

    if (FAILED(this->m_d3dDevice->CreateRenderTargetView(backBuffer, NULL, reinterpret_cast<ID3D11RenderTargetView**>(&this->m_backBufferViews->m_targetView)))) {
        backBuffer->Release();
        return 0;
    }

    HRESULT result;

    if (this->m_resolveTexture) {
        result = this->m_d3dDevice->CreateShaderResourceView(this->m_resolveTexture, NULL, &this->m_backBufferViews->m_srv);
    } else {
        result = this->m_d3dDevice->CreateShaderResourceView(backBuffer, NULL, &this->m_backBufferViews->m_srv);
    }

    backBuffer->Release();

    if (FAILED(result)) {
        return 0;
    }

    D3D11_TEXTURE2D_DESC depthDesc;
    depthDesc.Width = swapChainDesc.BufferDesc.Width;
    depthDesc.Height = swapChainDesc.BufferDesc.Height;
    depthDesc.MipLevels = 1;
    depthDesc.ArraySize = 1;
    depthDesc.Format = s_GxFormatToDxgiTypeless[this->m_format.m_depthFormat];
    depthDesc.SampleDesc.Count = swapChainDesc.SampleDesc.Count;
    depthDesc.SampleDesc.Quality = swapChainDesc.SampleDesc.Quality;
    depthDesc.Usage = D3D11_USAGE_DEFAULT;
    depthDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    depthDesc.CPUAccessFlags = 0;
    depthDesc.MiscFlags = 0;

    if (swapChainDesc.SampleDesc.Count == 1) {
        depthDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    }

    if (FAILED(this->m_d3dDevice->CreateTexture2D(&depthDesc, NULL, &this->m_depthTexture))) {
        return 0;
    }

    this->m_depthViews = static_cast<TexTargetViews*>(SMemAlloc(sizeof(TexTargetViews), __FILE__, __LINE__, 0));
    this->m_depthViews->m_type = 2;
    this->m_depthViews->m_srv = NULL;

    D3D11_DEPTH_STENCIL_VIEW_DESC viewDesc;
    viewDesc.Format = s_GxFormatToDxgiFormat[this->m_format.m_depthFormat];
    viewDesc.Flags = 0;

    if (swapChainDesc.SampleDesc.Count > 1) {
        viewDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DMS;
    } else {
        viewDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        viewDesc.Texture2D.MipSlice = 0;
    }

    if (FAILED(this->m_d3dDevice->CreateDepthStencilView(this->m_depthTexture, &viewDesc, reinterpret_cast<ID3D11DepthStencilView**>(&this->m_depthViews->m_targetView)))) {
        return 0;
    }

    this->m_format.m_size.x = swapChainDesc.BufferDesc.Width;
    this->m_format.m_size.y = swapChainDesc.BufferDesc.Height;
    this->ICreateBackBufferFB(swapChainDesc.BufferDesc.Width, swapChainDesc.BufferDesc.Height, this->m_backBufferViews, this->m_depthViews);
    memset(this->m_textureTarget, 0, sizeof(this->m_textureTarget));
    GxFrameBufferSet(g_theGxDevicePtr->BackBuffer());

    return 1;
}

// OFFSET: 0x00851C10
CGxDeviceD3d11::RenderTargets::RenderTargets() {
    this->m_numRenderTargets = 0;

    for (uint32_t i = 0; i < 8; i++) {
        this->m_renderTargets[i] = NULL;
    }

    this->m_curDepthView = NULL;
    this->m_colorTargetFormat = GxTex_Unknown;
}

// OFFSET: 0x00852040
void CGxDeviceD3d11::IReleaseTargets() {
    this->m_targets = RenderTargets();

    if (this->m_d3dContext) {
        this->m_d3dContext->OMSetRenderTargets(0, NULL, NULL);
    }

    if (this->m_backBufferViews) {
        if (this->m_backBufferViews->m_targetView) {
            this->m_backBufferViews->m_targetView->Release();
            this->m_backBufferViews->m_targetView = NULL;
        }

        if (this->m_backBufferViews->m_srv) {
            this->m_backBufferViews->m_srv->Release();
            this->m_backBufferViews->m_srv = NULL;
        }

        if (this->m_backBufferViews) {
            SMemFree(this->m_backBufferViews, "delete", -1, 0);
        }

        this->m_backBufferViews = NULL;
    }

    if (this->m_depthViews) {
        if (this->m_depthViews->m_targetView) {
            this->m_depthViews->m_targetView->Release();
            this->m_depthViews->m_targetView = NULL;
        }

        if (this->m_depthViews->m_srv) {
            this->m_depthViews->m_srv->Release();
            this->m_depthViews->m_srv = NULL;
        }

        if (this->m_depthViews) {
            SMemFree(this->m_depthViews, "delete", -1, 0);
        }

        this->m_depthViews = NULL;
    }

    if (this->m_depthTexture) {
        this->m_depthTexture->Release();
        this->m_depthTexture = NULL;
    }

    if (this->m_resolveTexture) {
        this->m_resolveTexture->Release();
        this->m_resolveTexture = NULL;
    }
}

// OFFSET: 0x008521C0/0x140581C00
void CGxDeviceD3d11::IUnloadD3dLib() {
    if (--s_d3dLibRefCount) {
        return;
    }

    if (s_d3d11Lib) {
        FreeLibrary(s_d3d11Lib);
        s_d3d11Lib = NULL;
    }

    if (s_dxgiLib) {
        FreeLibrary(s_dxgiLib);
        s_dxgiLib = NULL;
    }

    s_D3D11CreateDevice = NULL;
    s_CreateDXGIFactory1 = NULL;
}

// OFFSET: 0x00852280/0x140581CF0
IDXGIOutput* CGxDeviceD3d11::IGetOutputForMonitor(IDXGIAdapter* adapter, uint32_t monitor) {
    HMONITOR handle = static_cast<HMONITOR>(CGxDevice::AdapterGetMonitorHandle(monitor));
    uint32_t index = 0;

    if (!handle) {
        return NULL;
    }

    IDXGIOutput* output = NULL;

    if (FAILED(adapter->EnumOutputs(0, &output))) {
        return NULL;
    }

    while (true) {
        DXGI_OUTPUT_DESC desc;
        output->GetDesc(&desc);

        if (desc.Monitor == handle) {
            return output;
        }

        if (output) {
            output->Release();
            output = NULL;
        }

        if (FAILED(adapter->EnumOutputs(++index, &output))) {
            return NULL;
        }
    }
}

// OFFSET: 0x00852310
void CGxDeviceD3d11::ISetWindowFocus(int32_t focus) {
    RECT rect;

    if (!this->m_format.m_window && focus) {
        GetWindowRect(this->m_hwnd, &rect);
        ClipCursor(&rect);
    }

    if (this->m_windowFocus == focus) {
        return;
    }

    this->m_windowFocus = focus;

    if (this->m_format.m_window) {
        return;
    }

    if (focus) {
        if (this->m_swapChain) {
            this->m_swapChain->SetFullscreenState(TRUE, NULL);
            this->DeviceSetGamma(this->m_gammaRamp);
        }

        GetWindowRect(this->m_hwnd, &rect);
        ClipCursor(&rect);
    } else {
        if (this->m_swapChain) {
            this->m_swapChain->SetFullscreenState(FALSE, NULL);
        }

        ShowWindow(this->m_hwnd, SW_MINIMIZE);
    }
}

// OFFSET: 0x008523D0/0x140581DF0
void CGxDeviceD3d11::DeviceSetBaseMipLevel(uint32_t level) {
    this->ITexForceRecreation(1);
    CGxDevice::DeviceSetBaseMipLevel(level);
}

// OFFSET: 0x008523F0/0x140581E20
void CGxDeviceD3d11::DeviceSetGamma(float gamma) {
    CGxDevice::DeviceSetGamma(gamma);

    if (!this->m_context) {
        return;
    }

    IDXGIOutput* output = NULL;

    if (FAILED(this->m_swapChain->GetContainingOutput(&output))) {
        return;
    }

    DXGI_GAMMA_CONTROL_CAPABILITIES caps;

    if (SUCCEEDED(output->GetGammaControlCapabilities(&caps))) {
        DXGI_GAMMA_CONTROL control;
        memset(&control, 0, sizeof(control));
        control.Scale.Red = 1.0f;
        control.Scale.Green = 1.0f;
        control.Scale.Blue = 1.0f;
        control.Offset.Red = 0.0f;
        control.Offset.Green = 0.0f;
        control.Offset.Blue = 0.0f;

        for (uint32_t i = 0; i < caps.NumGammaControlPoints; i++) {
            uint32_t index = (i * 256) / caps.NumGammaControlPoints;
            control.GammaCurve[i].Red = this->m_gammaRamp.m_red[index] * (1.0f / 65535.0f);
            control.GammaCurve[i].Green = this->m_gammaRamp.m_green[index] * (1.0f / 65535.0f);
            control.GammaCurve[i].Blue = this->m_gammaRamp.m_blue[index] * (1.0f / 65535.0f);
        }

        output->SetGammaControl(&control);
    }

    if (output) {
        output->Release();
    }
}

// OFFSET: 0x00852550/0x140581FB0
void CGxDeviceD3d11::DeviceSetGamma(const CGxGammaRamp& ramp) {
    CGxDevice::DeviceSetGamma(ramp);

    if (!this->m_context) {
        return;
    }

    IDXGIOutput* output = NULL;

    if (FAILED(this->m_swapChain->GetContainingOutput(&output))) {
        return;
    }

    DXGI_GAMMA_CONTROL_CAPABILITIES caps;

    if (SUCCEEDED(output->GetGammaControlCapabilities(&caps))) {
        DXGI_GAMMA_CONTROL control;
        memset(&control, 0, sizeof(control));
        control.Scale.Red = 1.0f;
        control.Scale.Green = 1.0f;
        control.Scale.Blue = 1.0f;
        control.Offset.Red = 0.0f;
        control.Offset.Green = 0.0f;
        control.Offset.Blue = 0.0f;

        for (uint32_t i = 0; i < caps.NumGammaControlPoints; i++) {
            uint32_t index = (i * 256) / caps.NumGammaControlPoints;
            control.GammaCurve[i].Red = this->m_gammaRamp.m_red[index] * (1.0f / 65535.0f);
            control.GammaCurve[i].Green = this->m_gammaRamp.m_green[index] * (1.0f / 65535.0f);
            control.GammaCurve[i].Blue = this->m_gammaRamp.m_blue[index] * (1.0f / 65535.0f);
        }

        output->SetGammaControl(&control);
    }

    if (output) {
        output->Release();
    }
}

// OFFSET: 0x008526B0/0x140582140
void* CGxDeviceD3d11::DeviceWindow() {
    return this->m_hwnd;
}

// OFFSET: 0x008526C0
void CGxDeviceD3d11::DeviceOverride(EGxOverride override, uint32_t value) {
    CGxDevice::DeviceOverride(override, value);
}

// OFFSET: 0x008526D0/0x140582160
int32_t CGxDeviceD3d11::CreatePoolAPI(CGxPool* pool) {
    D3D11_BUFFER_DESC desc;
    desc.ByteWidth = pool->m_size;
    desc.Usage = pool->m_usage ? D3D11_USAGE_DYNAMIC : D3D11_USAGE_DEFAULT;
    desc.CPUAccessFlags = pool->m_usage ? D3D11_CPU_ACCESS_WRITE : 0;
    desc.MiscFlags = 0;
    desc.StructureByteStride = 0;

    switch (pool->m_target) {
        case GxPoolTarget_Vertex:
            desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
            break;

        case GxPoolTarget_Index:
            desc.BindFlags = D3D11_BIND_INDEX_BUFFER;
            break;

        default:
            desc.BindFlags = 0;
            break;
    }

    ID3D11Buffer* buffer;

    if (SUCCEEDED(this->m_d3dDevice->CreateBuffer(&desc, NULL, &buffer))) {
        pool->m_apiSpecific = buffer;
    }

    return 1;
}

// OFFSET: 0x00852750/0x140582200
void CGxDeviceD3d11::PoolSizeSet(CGxPool* pool, uint32_t size) {
    if (pool->m_usage == GxPoolUsage_Stream) {
        pool->m_offset = 0;
    }

    pool->Invalidate();

    if (pool->m_apiSpecific) {
        static_cast<ID3D11Buffer*>(pool->m_apiSpecific)->Release();
        pool->m_apiSpecific = NULL;
    }

    pool->m_size = size;
    this->CreatePoolAPI(pool);
}

// OFFSET: 0x008527A0/0x140582270
void CGxDeviceD3d11::PoolDestroy(CGxPool* pool) {
    if (!pool) {
        return;
    }

    if (pool->m_usage == GxPoolUsage_Stream) {
        pool->m_offset = 0;
    }

    pool->Invalidate();

    if (pool->m_apiSpecific) {
        static_cast<ID3D11Buffer*>(pool->m_apiSpecific)->Release();
        pool->m_apiSpecific = NULL;
    }

    CGxDevice::PoolDestroy(pool);
}

// OFFSET: 0x008527F0/0x1405822D0
char* CGxDeviceD3d11::IBufLock(CGxBuf* buf) {
    CGxPool* pool = buf->m_pool;

    if (!this->m_context) {
        return static_cast<char*>(this->m_emergencyMem[pool->m_target].Lock(buf->m_size));
    }

    if (!pool->m_apiSpecific) {
        this->CreatePoolAPI(pool);
    }

    ID3D11Buffer* buffer = static_cast<ID3D11Buffer*>(pool->m_apiSpecific);

    if (!buffer) {
        return static_cast<char*>(this->m_emergencyMem[pool->m_target].Lock(buf->m_size));
    }

    D3D11_MAPPED_SUBRESOURCE mapped;

    switch (pool->m_usage) {
        case GxPoolUsage_Static:
            return static_cast<char*>(this->m_staticEmergencyMem[pool->m_target].Lock(buf->m_size));

        case GxPoolUsage_Dynamic:
            if (FAILED(this->m_d3dContext->Map(buffer, 0, D3D11_MAP_WRITE_NO_OVERWRITE, 0, &mapped))) {
                return static_cast<char*>(this->m_emergencyMem[pool->m_target].Lock(buf->m_size));
            }

            break;

        case GxPoolUsage_Stream: {
            uint32_t index = (buf->m_itemSize + pool->m_offset - 1) - (buf->m_itemSize + pool->m_offset - 1) % buf->m_itemSize;
            D3D11_MAP mapType;

            if (index + buf->m_size <= static_cast<uint32_t>(pool->m_size)) {
                mapType = D3D11_MAP_WRITE_NO_OVERWRITE;
            } else {
                mapType = D3D11_MAP_WRITE_DISCARD;
                pool->Discard();
                index = 0;
            }

            buf->m_index = index / buf->m_itemSize;
            pool->m_offset = index + buf->m_size;

            if (FAILED(this->m_d3dContext->Map(buffer, 0, mapType, 0, &mapped))) {
                return static_cast<char*>(this->m_emergencyMem[pool->m_target].Lock(buf->m_size));
            }

            break;
        }

        default:
            return NULL;
    }

    return static_cast<char*>(mapped.pData) + buf->m_itemSize * buf->m_index;
}

// OFFSET: 0x00852960/0x140582480
char* CGxDeviceD3d11::BufLock(CGxBuf* buf) {
    CGxDevice::BufLock(buf);
    return this->IBufLock(buf);
}

// OFFSET: 0x00852980/0x1405824F0
void CGxDeviceD3d11::ICursorCreate(const CGxFormat& format) {
    CGxDevice::ICursorCreate(format);

    if (this->m_hwCursor) {
        this->IWindowCursorCreate(this->m_hwnd, &this->m_cursor);
    } else {
        SetCursor(NULL);
    }
}

// OFFSET: 0x008529C0/0x140582530
void CGxDeviceD3d11::ICursorDestroy() {
    CGxDevice::ICursorDestroy();
    this->IWindowCursorDestroy(&this->m_cursor);
}

// OFFSET: 0x00852A10
void CGxDeviceD3d11::IQueryCreateApi(CGxQuery* query) {
    if (query->m_apiSpecific) {
        return;
    }

    D3D11_QUERY_DESC desc;
    desc.Query = s_queryTypes[query->m_type];
    desc.MiscFlags = 0;

    ID3D11Query* d3dQuery;

    if (SUCCEEDED(this->m_d3dDevice->CreateQuery(&desc, &d3dQuery))) {
        query->m_apiSpecific = d3dQuery;
    }
}

// OFFSET: 0x00852A60/0x140582590
void CGxDeviceD3d11::QueryCreate(CGxQuery*& query, EGxQueryType type) {
    CGxDevice::QueryCreate(query, type);

    if (query->m_apiSpecific) {
        return;
    }

    D3D11_QUERY_DESC desc;
    desc.Query = s_queryTypes[query->m_type];
    desc.MiscFlags = 0;

    ID3D11Query* d3dQuery;

    if (SUCCEEDED(this->m_d3dDevice->CreateQuery(&desc, &d3dQuery))) {
        query->m_apiSpecific = d3dQuery;
    }
}

// OFFSET: 0x00852AF0/0x140582650
bool CGxDeviceD3d11::QueryBegin(CGxQuery* query) {
    if (!this->m_context) {
        return 0;
    }

    this->IQueryCreateApi(query);

    if (!query->m_apiSpecific) {
        return 0;
    }

    if (!query->m_type) {
        this->m_d3dContext->Begin(static_cast<ID3D11Query*>(query->m_apiSpecific));
    }

    return 1;
}

// OFFSET: 0x00852B40
bool CGxDeviceD3d11::QueryEnd(CGxQuery* query) {
    if (!this->m_context || !query->m_apiSpecific) {
        return 0;
    }

    if (!query->m_type) {
        this->m_d3dContext->End(static_cast<ID3D11Query*>(query->m_apiSpecific));
    }

    return 1;
}

// OFFSET: 0x00852B80
bool CGxDeviceD3d11::QueryGetParam(CGxQuery* query, EGxQueryParam param, uint32_t& data) {
    if (!this->m_context) {
        return 0;
    }

    ID3D11Query* d3dQuery = static_cast<ID3D11Query*>(query->m_apiSpecific);

    if (!d3dQuery) {
        return 0;
    }

    if (param == 0) {
        data = d3dQuery->GetDataSize();
        return 1;
    }

    if (param == 1) {
        HRESULT result = this->m_d3dContext->GetData(d3dQuery, NULL, 0, 0);
        this->ICheckResult(result);
        data = result >= 0;
    }

    return 0;
}

// OFFSET: 0x00852C00/0x1405827D0
bool CGxDeviceD3d11::QueryGetData(CGxQuery* query, uint32_t& data) {
    if (!this->m_context) {
        return 0;
    }

    ID3D11Query* d3dQuery = static_cast<ID3D11Query*>(query->m_apiSpecific);

    if (!d3dQuery) {
        return 0;
    }

    UINT64 samples;

    while (FAILED(this->m_d3dContext->GetData(d3dQuery, &samples, sizeof(samples), D3D11_ASYNC_GETDATA_DONOTFLUSH))) {
    }

    data = static_cast<uint32_t>(samples);

    return 1;
}

// OFFSET: 0x00852AC0/0x140582600
void CGxDeviceD3d11::QueryDestroy(CGxQuery*& query) {
    CGxQuery* gxQuery = query;

    if (gxQuery->m_apiSpecific) {
        static_cast<IUnknown*>(gxQuery->m_apiSpecific)->Release();
    }

    gxQuery->m_apiSpecific = NULL;
    CGxDevice::QueryDestroy(query);
}

// OFFSET: 0x00853EE0/0x140583C20
int32_t CGxDeviceD3d11::ICreateD3dDevice(const CGxFormat& format) {
    D3DPERF_SetOptions(1);

    if (FAILED(s_CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&this->m_dxgiFactory)))) {
        return 0;
    }

    if (FAILED(this->m_dxgiFactory->EnumAdapters1(0, &this->m_dxgiAdapter))) {
        return 0;
    }

    UINT flags = D3D11_CREATE_DEVICE_SINGLETHREADED;

    if (D3d11DebugLevel()) {
        flags = D3D11_CREATE_DEVICE_SINGLETHREADED | D3D11_CREATE_DEVICE_DEBUG;
    }

    D3D_FEATURE_LEVEL featureLevels[3] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };
    D3D_FEATURE_LEVEL featureLevel;

    if (FAILED(s_D3D11CreateDevice(this->m_dxgiAdapter, D3D_DRIVER_TYPE_UNKNOWN, NULL, flags, featureLevels, 3, D3D11_SDK_VERSION, &this->m_d3dDevice, &featureLevel, &this->m_d3dContext))) {
        return 0;
    }

    if (featureLevel == D3D_FEATURE_LEVEL_10_0 && format.unk_002C && format.m_multisampleCount > 1) {
        if (this->m_d3dDevice) {
            this->m_d3dDevice->Release();
            this->m_d3dDevice = NULL;
        }

        if (this->m_d3dContext) {
            this->m_d3dContext->Release();
            this->m_d3dContext = NULL;
        }

        return 0;
    }

    if (D3d11DebugLevel() >= 2) {
        ID3D11InfoQueue* infoQueue = NULL;
        this->m_d3dDevice->QueryInterface(__uuidof(ID3D11InfoQueue), reinterpret_cast<void**>(&infoQueue));

        if (infoQueue) {
            infoQueue->SetBreakOnSeverity(D3D11_MESSAGE_SEVERITY_CORRUPTION, TRUE);
            infoQueue->SetBreakOnSeverity(D3D11_MESSAGE_SEVERITY_ERROR, TRUE);
            infoQueue->SetBreakOnSeverity(D3D11_MESSAGE_SEVERITY_INFO, FALSE);

            if (D3d11DebugLevel() < 3) {
                infoQueue->SetBreakOnSeverity(D3D11_MESSAGE_SEVERITY_WARNING, FALSE);
            } else {
                infoQueue->SetBreakOnSeverity(D3D11_MESSAGE_SEVERITY_WARNING, TRUE);
            }

            if (infoQueue) {
                infoQueue->Release();
            }
        }
    }

    this->m_featureLevel = this->IFeatureLevelIndex(featureLevel);

    DXGI_SWAP_CHAIN_DESC desc;
    memset(&desc, 0, sizeof(desc));

    DXGI_FORMAT depthFormat = s_GxFormatToDxgiFormat[this->m_format.m_depthFormat];
    DXGI_FORMAT colorFormat = s_GxFormatToDxgiFormat[format.m_colorFormat];
    uint32_t sampleCount = 1;
    uint32_t sampleQuality = 0;

    if (format.m_multisampleCount > 1) {
        UINT colorQuality = 0;
        UINT depthQuality = 0;

        if (SUCCEEDED(this->m_d3dDevice->CheckMultisampleQualityLevels(colorFormat, format.m_multisampleCount, &colorQuality)) && SUCCEEDED(this->m_d3dDevice->CheckMultisampleQualityLevels(depthFormat, format.m_multisampleCount, &depthQuality)) && colorQuality && depthQuality) {
            uint32_t quality = depthQuality;

            if (colorQuality < depthQuality) {
                quality = colorQuality;
            }

            sampleCount = format.m_multisampleCount;
            sampleQuality = static_cast<uint32_t>(static_cast<double>(quality - 1) * format.m_multisampleQuality);
        }
    }

    desc.BufferDesc.Width = format.m_size.x;
    desc.BufferDesc.Height = format.m_size.y;
    desc.BufferDesc.RefreshRate.Numerator = format.m_refreshRate;
    desc.BufferDesc.RefreshRate.Denominator = format.m_refreshRateDenom;
    desc.BufferDesc.Format = colorFormat;
    desc.BufferDesc.ScanlineOrdering = static_cast<DXGI_MODE_SCANLINE_ORDER>(format.m_scanlineOrdering);
    desc.BufferDesc.Scaling = static_cast<DXGI_MODE_SCALING>(format.m_scaling);
    desc.SampleDesc.Count = sampleCount;
    desc.SampleDesc.Quality = sampleQuality;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;

    if (sampleCount == 1) {
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT | DXGI_USAGE_SHADER_INPUT;
    }

    desc.BufferCount = format.m_vsync ? format.m_backBufferCount : 1;
    desc.OutputWindow = this->m_hwnd;
    desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    if (!format.m_window) {
        desc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    }

    desc.Windowed = format.m_window != 0;

    if (FAILED(this->m_dxgiFactory->CreateSwapChain(this->m_d3dDevice, &desc, &this->m_swapChain))) {
        return 0;
    }

    if (format.m_fixLag && SUCCEEDED(this->m_d3dDevice->QueryInterface(__uuidof(IDXGIDevice1), reinterpret_cast<void**>(&this->m_dxgiDevice)))) {
        this->m_dxgiDevice->SetMaximumFrameLatency(1);
    }

    this->m_context = 1;
    this->ISetCaps(format);
    this->Log(this->m_caps);

    if (!this->ICreateTargets(format.m_size)) {
        return 0;
    }

    D3D11_BUFFER_DESC bufferDesc;
    bufferDesc.Usage = D3D11_USAGE_DYNAMIC;
    bufferDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bufferDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    bufferDesc.MiscFlags = 0;
    bufferDesc.StructureByteStride = 0;

    for (uint32_t i = 0; i < GxShTargets_Last; i++) {
        if (!this->m_caps.m_shaderTargets[i]) {
            continue;
        }

        bufferDesc.ByteWidth = 16 * this->m_caps.m_shaderConstants[i];

        ID3D11Buffer* buffer;

        if (FAILED(this->m_d3dDevice->CreateBuffer(&bufferDesc, NULL, &buffer))) {
            return 0;
        }

        this->m_constantBuffers[i] = buffer;
    }

    if (this->m_constantBuffers[GxSh_Vertex]) {
        this->m_d3dContext->VSSetConstantBuffers(0, 1, &this->m_constantBuffers[GxSh_Vertex]);
    }

    if (this->m_constantBuffers[GxSh_Pixel]) {
        this->m_d3dContext->PSSetConstantBuffers(0, 1, &this->m_constantBuffers[GxSh_Pixel]);
    }

    if (this->m_constantBuffers[GxSh_Geometry]) {
        this->m_d3dContext->GSSetConstantBuffers(0, 1, &this->m_constantBuffers[GxSh_Geometry]);
    }

    if (this->m_constantBuffers[GxSh_Hull]) {
        this->m_d3dContext->HSSetConstantBuffers(0, 1, &this->m_constantBuffers[GxSh_Hull]);
    }

    if (this->m_constantBuffers[GxSh_Domain]) {
        this->m_d3dContext->DSSetConstantBuffers(0, 1, &this->m_constantBuffers[GxSh_Domain]);
    }

    this->m_clearVertexShader = this->ShaderCreate(GxSh_Vertex, "Clear");
    this->m_clearPixelShader = this->ShaderCreate(GxSh_Pixel, "Clear");
    this->m_passThroughVertexShader = this->ShaderCreate(GxSh_Vertex, "PassThrough_PT");
    this->m_stretchRectPixelShader = this->ShaderCreate(GxSh_Pixel, "StretchRect");
    this->m_resolveVertexShader = this->ShaderCreate(GxSh_Vertex, "Resolve");
    this->m_resolvePixelShader = this->ShaderCreate(GxSh_Pixel, "Resolve");
    // This client keeps two UI vertex shaders (the second is the stereo permutation that the
    // interface shader set defines); only slot 0 is used by the D3d11 backend.
    CGxDevice::s_uiVertexShader[0] = this->ShaderCreate(GxSh_Vertex, "UI");
    CGxDevice::s_uiPixelShader = this->ShaderCreate(GxSh_Pixel, "UI_AlphaTest");

    D3D11_VIEWPORT viewport;
    viewport.TopLeftX = 0.0f;
    viewport.TopLeftY = 0.0f;
    viewport.Width = static_cast<float>(desc.BufferDesc.Width);
    viewport.Height = static_cast<float>(desc.BufferDesc.Height);
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    this->m_d3dContext->RSSetViewports(1, &viewport);

    if (!this->m_format.m_window) {
        this->DeviceSetGamma(this->m_gammaRamp);
    }

    this->m_dxgiFactory->MakeWindowAssociation(this->m_hwnd, DXGI_MWA_NO_WINDOW_CHANGES | DXGI_MWA_NO_ALT_ENTER);
    this->ICursorCreate(format);

    if (s_hideCursorOnCreate) {
        this->CursorSetVisible(0);
        s_hideCursorOnCreate = 0;
    }

    return 1;
}

// OFFSET: 0x00852C60/0x140582870
int32_t CGxDeviceD3d11::ILoadD3dLib() {
    if (s_d3dLibRefCount++) {
        return 1;
    }

    s_d3d11Lib = LoadLibraryA("d3d11.dll");

    if (!s_d3d11Lib) {
        CGxDevice::Log("CGxDeviceD3d11::ILoadD3dLib(): unable to LoadLibrary(\"d3d11.dll\")");
        CGxDeviceD3d11::IUnloadD3dLib();
        return 0;
    }

    s_dxgiLib = LoadLibraryA("dxgi.dll");

    if (!s_dxgiLib) {
        CGxDevice::Log("CGxDeviceD3d11::ILoadD3dLib(): unable to LoadLibrary(\"dxgi.dll\")");
        CGxDeviceD3d11::IUnloadD3dLib();
        return 0;
    }

    s_D3D11CreateDevice = reinterpret_cast<D3D11CreateDeviceProc>(GetProcAddress(s_d3d11Lib, "D3D11CreateDevice"));

    if (!s_D3D11CreateDevice) {
        CGxDevice::Log("CGxDeviceD3d11::ILoadD3dLib(): unable to GetProcAddress(\"D3D11CreateDevice\")");
        CGxDeviceD3d11::IUnloadD3dLib();
        return 0;
    }

    s_CreateDXGIFactory1 = reinterpret_cast<CreateDXGIFactory1Proc>(GetProcAddress(s_dxgiLib, "CreateDXGIFactory1"));

    if (!s_CreateDXGIFactory1) {
        CGxDevice::Log("CGxDeviceD3d11::ILoadD3dLib(): unable to GetProcAddress(\"CreateDXGIFactory\")");
        CGxDeviceD3d11::IUnloadD3dLib();
        return 0;
    }

    return 1;
}

// OFFSET: 0x00852F10/0x140582BF0
int32_t CGxDeviceD3d11::DeviceCreate(GxWindowProc windowProc, const CGxFormat& format) {
    this->m_ownhwnd = 1;

    HDC dc = GetDC(NULL);

    if (GetDeviceGammaRamp(dc, &this->m_systemGammaRamp) && &this->m_gammaRamp != &this->m_systemGammaRamp) {
        this->m_gammaRamp = this->m_systemGammaRamp;
    }

    ReleaseDC(NULL, dc);

    this->m_hwndClass = this->IWindowClassCreate();

    if (!this->m_hwndClass) {
        char* error = OsGetLastErrorStr();
        CGxDevice::Log("CGxDeviceD3d::DeviceCreate(): IWindowClassCreate() failed: %s", error);
        OsFreeLastErrorStr(error);
        this->DeviceDestroy();
        return 0;
    }

    if (CGxDeviceD3d11::ILoadD3dLib() && CGxDevice::DeviceCreate(windowProc, format)) {
        return 1;
    }

    this->DeviceDestroy();
    return 0;
}

// OFFSET: 0x00854540/0x140584430
int32_t CGxDeviceD3d11::DeviceCreate(void* window, const CGxFormat& format) {
    this->m_ownhwnd = 0;
    CGxDevice::DeviceCreate(window, format);
    this->m_hwnd = static_cast<HWND>(window);

    if (CGxDeviceD3d11::ILoadD3dLib() && this->ICreateD3dDevice(format) && CGxDevice::DeviceCreate(window, format)) {
        return 1;
    }

    this->DeviceDestroy();
    return 0;
}

// OFFSET: 0x00853B80/0x1405837F0
int32_t CGxDeviceD3d11::DeviceWM(EGxWM wm, uintptr_t param1, uintptr_t param2) {
    switch (wm) {
        case GxWM_Size:
            if (param2 == 1 || param2 == 2) {
                this->m_windowVisible = 0;
                break;
            }

            this->m_windowVisible = 1;
            this->DeviceSetDefWindow(*reinterpret_cast<const CRect*>(param1));

            if (this->m_d3dDevice && this->m_context) {
                const CRect& rect = *reinterpret_cast<const CRect*>(param1);

                this->IReleaseTargets();

                DXGI_SWAP_CHAIN_DESC desc;
                this->m_swapChain->GetDesc(&desc);

                UINT width = static_cast<UINT>(static_cast<int64_t>(rect.maxX - rect.minX));
                UINT height = static_cast<UINT>(static_cast<int64_t>(rect.maxY - rect.minY));
                this->m_swapChain->ResizeBuffers(desc.BufferCount, width, height, desc.BufferDesc.Format, desc.Flags);

                C2iVector size;
                this->ICreateTargets(size);
                this->NotifyOnDeviceRestored();
            }

            this->m_viewportDirty = 1;
            break;

        case GxWM_DisplayChange:
            if (this->m_windowVisible) {
                this->DeviceSetDefWindow(*reinterpret_cast<const CRect*>(param1));
                this->m_viewportDirty = 1;
            }

            break;

        case GxWM_Destroy:
        case GxWM_KillFocus:
            this->ISetWindowFocus(0);
            break;

        case GxWM_SetFocus:
            this->ISetWindowFocus(1);
            break;

        default:
            break;
    }

    return 0;
}

// OFFSET: 0x008529E0/0x140582560
int32_t CGxDeviceD3d11::ISetCursor() {
    if (!this->m_d3dDevice || this->m_hwCursor) {
        return 0;
    }

    SetCursor(NULL);
    return 1;
}

// OFFSET: 0x00852FF0/0x140582CE0
void CGxDeviceD3d11::DeviceSetRenderTarget(EGxBuffer buffer, CGxTex* texture, uint32_t plane) {
    if (!this->m_context) {
        return;
    }

    CGxDevice::TextureTarget& target = this->m_textureTarget[buffer];

    if (target.m_texture == texture && target.m_plane == plane) {
        return;
    }

    CGxDevice::DeviceSetRenderTarget(buffer, texture, plane);

    if (texture) {
        if (texture->m_needsCreation || (!texture->m_apiSpecificData && !texture->m_apiSpecificData2)) {
            this->ITexCreate(texture);
        }

        if (!texture->m_needsCreation && (texture->m_apiSpecificData || texture->m_apiSpecificData2)) {
            target.m_apiSpecific = texture->m_apiSpecificData;
        }
    } else {
        target.m_apiSpecific = NULL;
    }

    // Colour targets only: m_renderTargets[] is indexed from GxBuffers_Color, and the Depth slot is
    // tracked separately in m_curDepthView. (EGxBuffer numbers Depth 0 / Color 1 in this client.)
    for (uint32_t i = GxBuffers_Color; i <= GxBuffers_Color3; i++) {
        TexTargetViews* views = static_cast<TexTargetViews*>(this->m_textureTarget[i].m_apiSpecific);

        // An unbound slot must also clear its RTV: leaving the previous view in place would keep
        // OMSetRenderTargets bound to a released target.
        if (!views) {
            this->m_targets.m_renderTargets[i - GxBuffers_Color] = nullptr;
            continue;
        }

        this->m_targets.m_renderTargets[i - GxBuffers_Color] = static_cast<ID3D11RenderTargetView*>(views->m_targetView);
        this->m_targets.m_numRenderTargets = (i - GxBuffers_Color) + 1;

        if (i == GxBuffers_Color) {
            this->m_targets.m_colorTargetFormat = this->m_textureTarget[i].m_texture->m_format;
        }

        CGxTex* targetTex = this->m_textureTarget[i].m_texture;

        if (targetTex->m_width != this->m_format.m_size.x || targetTex->m_height != this->m_format.m_size.y || (!targetTex->m_flags.m_bit8 && this->m_format.m_multisampleCount > 1) || (targetTex->m_flags.m_bit8 && this->m_format.m_multisampleCount == 1)) {
            this->m_targets.m_curDepthView = NULL;
        }
    }

    TexTargetViews* depthViews = static_cast<TexTargetViews*>(this->m_textureTarget[GxBuffers_Depth].m_apiSpecific);

    if (depthViews) {
        if (this->m_textureTarget[GxBuffers_Depth].m_texture &&
                this->m_textureTarget[GxBuffers_Depth].m_texture->m_width == this->m_format.m_size.x &&
                this->m_textureTarget[GxBuffers_Depth].m_texture->m_height == this->m_format.m_size.y) {
            this->m_targets.m_curDepthView = static_cast<ID3D11DepthStencilView*>(depthViews->m_targetView);
        } else {
            this->m_targets.m_curDepthView = NULL;
        }
    }

    this->m_d3dContext->OMSetRenderTargets(this->m_targets.m_numRenderTargets, this->m_targets.m_renderTargets, this->m_targets.m_curDepthView);
    GxXformSetViewport(this->m_viewport.m_x.m_l, this->m_viewport.m_x.m_h, this->m_viewport.m_y.m_l, this->m_viewport.m_y.m_h, this->m_viewport.m_z.m_l, this->m_viewport.m_z.m_h);
    this->m_viewportDirty = 1;
    this->m_scissorRectDirty = 1;
}

// OFFSET: 0x00853280/0x140582ED0
// NOT IN BINARY: cata's CGxDevice::ICreateBackBufferFB. It wraps the swap chain's colour/depth views in
// CGxTex objects and builds the back-buffer CGxFrameBuffer the engine binds. Kept on the backend rather than
// the base because this client's base has no back-buffer subsystem at all.
void CGxDeviceD3d11::ICreateBackBufferFB(uint32_t width, uint32_t height, void* colorApiData, void* depthApiData) {
    CGxTexFlags flags;
    flags.m_renderTarget = 1;

    // The two CGxTex wrappers and the CGxFrameBuffer are allocated once and freed in IDestroyD3d. They are
    // only descriptors here -- the underlying D3D views live in m_backBufferViews/m_depthViews and are
    // rebuilt on every device (re)creation, which is why the apiSpecificData pointers are refreshed each
    // time even when the wrappers already exist.
    if (!this->m_backBufferColorTex) {
        void* mem = SMemAlloc(sizeof(CGxTex), __FILE__, __LINE__, 0);

        if (!mem) {
            return;
        }

        this->m_backBufferColorTex = new (mem) CGxTex(GxTex_2d, width, height, 1, GxTex_Abgr8888, GxTex_Abgr8888, flags, NULL, NULL, "backBufferColorTex");
    }

    this->m_backBufferColorTex->m_width = width;
    this->m_backBufferColorTex->m_height = height;
    this->m_backBufferColorTex->m_apiSpecificData = colorApiData;
    this->m_backBufferColorTex->m_needsCreation = 0;

    if (!this->m_backBufferDepthTex) {
        void* mem = SMemAlloc(sizeof(CGxTex), __FILE__, __LINE__, 0);

        if (!mem) {
            return;
        }

        this->m_backBufferDepthTex = new (mem) CGxTex(GxTex_2d, width, height, 1, GxTex_D24X8, GxTex_D24X8, flags, NULL, NULL, "backBufferDepthTex");
    }

    this->m_backBufferDepthTex->m_width = width;
    this->m_backBufferDepthTex->m_height = height;
    this->m_backBufferDepthTex->m_apiSpecificData = depthApiData;
    this->m_backBufferDepthTex->m_needsCreation = 0;

    if (!this->m_frameBuffer) {
        void* mem = SMemAlloc(sizeof(CGxFrameBuffer), __FILE__, __LINE__, 0);

        if (!mem) {
            return;
        }

        CGxFrameBuffer* frameBuffer = new (mem) CGxFrameBuffer();
        frameBuffer->Attach(GxBuffers_Depth, this->m_backBufferDepthTex, 0);
        frameBuffer->Attach(GxBuffers_Color, this->m_backBufferColorTex, 0);
        frameBuffer->Finalize();
        this->m_frameBuffer = frameBuffer;
    } else {
        // Re-finalize so the frame buffer gets a fresh version. FrameBufferSet early-outs on an unchanged
        // (pointer, version) pair, and the caller has just cleared m_textureTarget, so without a new version
        // the render targets would never be bound again after a reset or resize.
        this->m_frameBuffer->Finalize();
    }

    // Deliberately NOT storing m_frameBuffer->m_version here: FrameBufferSet's early-out compares the stored
    // version against the frame buffer's, and the caller always clears m_textureTarget before calling it. If
    // this matched, the first (and every post-reset) bind of the back buffer would be skipped and no render
    // targets would ever be set. Leaving it stale forces the bind; FrameBufferSet updates it on the way out.
    this->m_frameBufferVersion = 0;
}

// NOT IN BINARY: the bound back buffer. The engine calls this to decide whether it is rendering to the
// screen (see IXformSetViewport and ScenePresent).
CGxFrameBuffer* CGxDeviceD3d11::BackBuffer() {
    return this->m_frameBuffer;
}

void CGxDeviceD3d11::FrameBufferSet(CGxFrameBuffer* frameBuffer) {
    if (!this->m_context) {
        return;
    }

    // GxFrameBufferSet(nullptr) is the documented way to unbind; fall back to the back buffer.
    if (!frameBuffer) {
        frameBuffer = this->m_frameBuffer;
    }

    if (!frameBuffer) {
        return;
    }

    if (this->m_frameBuffer == frameBuffer && this->m_frameBufferVersion == frameBuffer->m_version) {
        return;
    }

    int32_t count = frameBuffer->m_count;
    int32_t changed = 0;

    for (int32_t buffer = 0; buffer < count; buffer++) {
        CGxDevice::TextureTarget& target = this->m_textureTarget[buffer];
        CGxTex* texture = frameBuffer->m_targets[buffer].m_texture;

        if (target.m_texture == texture && target.m_plane == frameBuffer->m_targets[buffer].m_plane) {
            continue;
        }

        changed = 1;
        CGxDevice::DeviceSetRenderTarget(static_cast<EGxBuffer>(buffer), texture, frameBuffer->m_targets[buffer].m_plane);

        if (texture) {
            if (texture->m_needsCreation || (!texture->m_apiSpecificData && !texture->m_apiSpecificData2)) {
                this->ITexCreate(texture);
            }

            if (!texture->m_needsCreation && (texture->m_apiSpecificData || texture->m_apiSpecificData2)) {
                target.m_apiSpecific = texture->m_apiSpecificData;
            }
        } else {
            target.m_apiSpecific = NULL;
        }
    }

    if (!changed) {
        return;
    }

    // Colour targets only: m_renderTargets[] is indexed from GxBuffers_Color, and the Depth slot is
    // tracked separately in m_curDepthView. (EGxBuffer numbers Depth 0 / Color 1 in this client.)
    for (uint32_t i = GxBuffers_Color; i <= GxBuffers_Color3; i++) {
        TexTargetViews* views = static_cast<TexTargetViews*>(this->m_textureTarget[i].m_apiSpecific);

        // An unbound slot must also clear its RTV: leaving the previous view in place would keep
        // OMSetRenderTargets bound to a released target.
        if (!views) {
            this->m_targets.m_renderTargets[i - GxBuffers_Color] = nullptr;
            continue;
        }

        this->m_targets.m_renderTargets[i - GxBuffers_Color] = static_cast<ID3D11RenderTargetView*>(views->m_targetView);
        this->m_targets.m_numRenderTargets = (i - GxBuffers_Color) + 1;

        if (i == GxBuffers_Color) {
            this->m_targets.m_colorTargetFormat = this->m_textureTarget[i].m_texture->m_format;
        }

        CGxTex* targetTex = this->m_textureTarget[i].m_texture;

        if (targetTex->m_width != this->m_format.m_size.x || targetTex->m_height != this->m_format.m_size.y || (!targetTex->m_flags.m_bit8 && this->m_format.m_multisampleCount > 1) || (targetTex->m_flags.m_bit8 && this->m_format.m_multisampleCount == 1)) {
            this->m_targets.m_curDepthView = NULL;
        }
    }

    TexTargetViews* depthViews = static_cast<TexTargetViews*>(this->m_textureTarget[GxBuffers_Depth].m_apiSpecific);

    if (depthViews) {
        if (this->m_textureTarget[GxBuffers_Depth].m_texture &&
                this->m_textureTarget[GxBuffers_Depth].m_texture->m_width == this->m_format.m_size.x &&
                this->m_textureTarget[GxBuffers_Depth].m_texture->m_height == this->m_format.m_size.y) {
            this->m_targets.m_curDepthView = static_cast<ID3D11DepthStencilView*>(depthViews->m_targetView);
        } else {
            this->m_targets.m_curDepthView = NULL;
        }
    }

    this->m_d3dContext->OMSetRenderTargets(this->m_targets.m_numRenderTargets, this->m_targets.m_renderTargets, this->m_targets.m_curDepthView);
    GxXformSetViewport(this->m_viewport.m_x.m_l, this->m_viewport.m_x.m_h, this->m_viewport.m_y.m_l, this->m_viewport.m_y.m_h, this->m_viewport.m_z.m_l, this->m_viewport.m_z.m_h);
    this->m_viewportDirty = 1;
    this->m_scissorRectDirty = 1;

    this->m_frameBuffer->m_bound = 0;
    this->m_frameBuffer = frameBuffer;
    frameBuffer->m_bound = 1;
    this->m_frameBufferVersion = this->m_frameBuffer->m_version;
}

// OFFSET: 0x00853590/0x140583170
void CGxDeviceD3d11::DeviceResolveDepthBuffer(CGxTex* texture) {
    if ((texture->m_needsCreation || (!texture->m_apiSpecificData && !texture->m_apiSpecificData2)) && !this->ITexCreate(texture)) {
        return;
    }

    this->m_d3dContext->CopyResource(static_cast<ID3D11Resource*>(texture->m_apiSpecificData2), this->m_depthTexture);
}

// OFFSET: 0x008535E0/0x1405831D0
void CGxDeviceD3d11::DeviceCopyTex(CGxTex* source, uint32_t sourcePlane, CGxTex* dest, uint32_t destPlane, uint32_t level) {
    if (!source || !dest) {
        return;
    }

    if ((source->m_needsCreation || (!source->m_apiSpecificData && !source->m_apiSpecificData2)) && !this->ITexCreate(source)) {
        return;
    }

    if ((dest->m_needsCreation || (!dest->m_apiSpecificData && !dest->m_apiSpecificData2)) && !this->ITexCreate(dest)) {
        return;
    }

    ID3D11Resource* sourceRes = static_cast<ID3D11Resource*>(source->m_apiSpecificData2);
    ID3D11Resource* destRes = static_cast<ID3D11Resource*>(dest->m_apiSpecificData2);

    if (sourcePlane || destPlane || level != source->m_depth || level != dest->m_depth || CGxDevice::s_texFormatIsDepth[source->m_format]) {
        D3D11_BOX box;
        box.left = 0;
        box.top = 0;
        box.front = sourcePlane;
        box.right = source->m_width;
        box.bottom = source->m_height;
        box.back = level + sourcePlane;
        this->m_d3dContext->CopySubresourceRegion(destRes, 0, 0, 0, destPlane, sourceRes, 0, &box);
    } else {
        this->m_d3dContext->CopyResource(destRes, sourceRes);
    }
}

// OFFSET: 0x008536F0/0x140583310
void CGxDeviceD3d11::CursorUnlock(uint32_t x, uint32_t y) {
    CGxDevice::CursorUnlock(x, y);

    if (this->m_hwCursor) {
        this->IWindowCursorCreate(this->m_hwnd, &this->m_cursor);
    } else {
        SetCursor(NULL);
    }
}

// OFFSET: 0x00853740
void CGxDeviceD3d11::ICheckResult(HRESULT result) {
}

// OFFSET: 0x008537B0
void CGxDeviceD3d11::IReleaseD3dQueries() {
    for (CGxQuery* query = this->m_queryList.Head(); query; query = this->m_queryList.Next(query)) {
        if (query->m_apiSpecific) {
            static_cast<IUnknown*>(query->m_apiSpecific)->Release();
        }

        query->m_apiSpecific = NULL;
    }
}

// OFFSET: 0x008537F0/0x140583350
int32_t CGxDevice::D3d11AdapterInfer(uint16_t& deviceID) {
    deviceID = 0;
    int32_t found = 0;

    if (!CGxDeviceD3d11::ILoadD3dLib()) {
        return 0;
    }

    IDXGIFactory* factory = NULL;
    s_CreateDXGIFactory1(__uuidof(IDXGIFactory), reinterpret_cast<void**>(&factory));

    if (factory) {
        uint32_t index = 0;
        HRESULT result;

        do {
            IDXGIAdapter* adapter;
            result = factory->EnumAdapters(index, &adapter);

            if (SUCCEEDED(result) && adapter) {
                if (SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(ID3D10Device), NULL))) {
                    deviceID = 3;
                    found = 1;
                }

                if (adapter) {
                    adapter->Release();
                    adapter = NULL;
                }
            }

            index++;
        } while (SUCCEEDED(result) && !found);

        if (factory) {
            factory->Release();
            factory = NULL;
        }
    }

    CGxDeviceD3d11::IUnloadD3dLib();
    CGxDevice::Log("CGxDevice::D3d11AdapterInfer(): D3D11 - RET: %d, DID: %x", found, deviceID);

    return found;
}

// OFFSET: 0x00853D70/0x140583A30
void CGxDeviceD3d11::IBufUnlock(CGxBuf* buf, uint32_t size) {
    CGxPool* pool = buf->m_pool;
    EmergencyMem& emergencyMem = this->m_emergencyMem[pool->m_target];

    if (emergencyMem.m_lock) {
        emergencyMem.Unlock();
        buf->m_valid = 0;
        this->m_intF5C = 1;
        return;
    }

    ID3D11Buffer* buffer = static_cast<ID3D11Buffer*>(pool->m_apiSpecific);

    switch (pool->m_usage) {
        case GxPoolUsage_Static: {
            EmergencyMem& staticMem = this->m_staticEmergencyMem[pool->m_target];
            staticMem.Unlock();

            if (!size) {
                size = buf->m_size;
            }

            D3D11_BOX box;
            box.left = buf->m_itemSize * buf->m_index;
            box.top = 0;
            box.front = 0;
            box.right = size + box.left;
            box.bottom = 1;
            box.back = 1;
            this->m_d3dContext->UpdateSubresource(buffer, 0, &box, staticMem.m_data.m_data, pool->m_size, 0);
            break;
        }

        case GxPoolUsage_Dynamic:
        case GxPoolUsage_Stream:
            this->m_d3dContext->Unmap(buffer, 0);
            break;

        default:
            break;
    }

    buf->m_valid = 1;
}

// OFFSET: 0x00853E60/0x140583B60
int32_t CGxDeviceD3d11::BufUnlock(CGxBuf* buf, uint32_t size) {
    CGxDevice::BufUnlock(buf, size);
    this->IBufUnlock(buf, size);
    return 1;
}

// OFFSET: 0x00853E90/0x140583BA0
void CGxDeviceD3d11::BufData(CGxBuf* buf, const void* data, uint32_t size, uint32_t offset) {
    CGxDevice::BufData(buf, data, size, offset);

    char* bufData = this->IBufLock(buf);
    memcpy(&bufData[offset], data, size);
    this->IBufUnlock(buf, size);
}

// OFFSET: 0x00852210/0x140581C60
int32_t CGxDeviceD3d11::IHardwareSupported() {
    UINT flags = D3D11_CREATE_DEVICE_SINGLETHREADED;

    if (D3d11DebugLevel()) {
        flags = D3D11_CREATE_DEVICE_SINGLETHREADED | D3D11_CREATE_DEVICE_DEBUG;
    }

    D3D_FEATURE_LEVEL featureLevels[3] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };
    D3D_FEATURE_LEVEL featureLevel;

    if (FAILED(s_D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, flags, featureLevels, 3, D3D11_SDK_VERSION, NULL, &featureLevel, NULL))) {
        return 0;
    }

    return featureLevel >= D3D_FEATURE_LEVEL_10_0;
}

// OFFSET: 0x00850270/0x14057F530
void CGxDevice::D3d11ApiQuery(int32_t& supported, int32_t& selectable) {
    if (!CGxDeviceD3d11::ILoadD3dLib()) {
        return;
    }

    supported = 1;
    selectable = CGxDeviceD3d11::IHardwareSupported();
    CGxDeviceD3d11::IUnloadD3dLib();
}

// OFFSET: 0x00852D30/0x140582970
int32_t CGxDevice::D3d11AdapterDesktopMode(uint32_t monitor, CGxMonitorMode& mode) {
    if (!CGxDeviceD3d11::ILoadD3dLib()) {
        return 0;
    }

    IDXGIFactory* factory = NULL;
    IDXGIAdapter* adapter = NULL;
    s_CreateDXGIFactory1(__uuidof(IDXGIFactory), reinterpret_cast<void**>(&factory));

    if (factory && SUCCEEDED(factory->EnumAdapters(0, &adapter))) {
        IDXGIOutput* output = CGxDeviceD3d11::IGetOutputForMonitor(adapter, monitor);

        if (output) {
            HMONITOR hmonitor = static_cast<HMONITOR>(CGxDevice::AdapterGetMonitorHandle(monitor));
            MONITORINFOEXA info;
            DEVMODEA devMode;
            DXGI_MODE_DESC desc;
            DXGI_MODE_DESC closest;

            info.cbSize = sizeof(info);

            if (hmonitor && GetMonitorInfoA(hmonitor, &info)) {
                memset(&devMode, 0, sizeof(devMode));
                devMode.dmSize = sizeof(devMode);

                if (EnumDisplaySettingsA(info.szDevice, ENUM_CURRENT_SETTINGS, &devMode)) {
                    desc.Width = devMode.dmPelsWidth;
                    desc.Height = devMode.dmPelsHeight;
                    desc.RefreshRate.Numerator = 0;
                    desc.RefreshRate.Denominator = 0;
                    desc.Format = devMode.dmBitsPerPel == 16 ? DXGI_FORMAT_B5G6R5_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
                    desc.ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED;
                    desc.Scaling = DXGI_MODE_SCALING_UNSPECIFIED;

                    if (SUCCEEDED(output->FindClosestMatchingMode(&desc, &closest, NULL))) {
                        mode.m_size.x = closest.Width;
                        mode.m_size.y = closest.Height;
                        mode.m_bpp = closest.Format == DXGI_FORMAT_B5G6R5_UNORM ? 16 : 32;
                        mode.m_refreshRate = closest.RefreshRate.Numerator;
                        mode.m_refreshRateDenom = closest.RefreshRate.Denominator;
                        mode.m_scanlineOrdering = closest.ScanlineOrdering;
                        mode.m_scaling = closest.Scaling;
                        output->Release();

                        if (adapter) {
                            adapter->Release();
                            adapter = NULL;
                        }

                        if (factory) {
                            factory->Release();
                            factory = NULL;
                        }

                        CGxDeviceD3d11::IUnloadD3dLib();
                        return 1;
                    }
                }
            }

            output->Release();
        }
    }

    if (adapter) {
        adapter->Release();
        adapter = NULL;
    }

    if (factory) {
        factory->Release();
        factory = NULL;
    }

    CGxDeviceD3d11::IUnloadD3dLib();
    return 0;
}

// OFFSET: 0x008545B0/0x140584550
int32_t CGxDevice::D3d11EnumMonitorModes(uint32_t monitor, TSGrowableArray<CGxMonitorMode>& modes) {
    if (!CGxDeviceD3d11::ILoadD3dLib()) {
        return 0;
    }

    IDXGIFactory* factory = NULL;
    s_CreateDXGIFactory1(__uuidof(IDXGIFactory), reinterpret_cast<void**>(&factory));

    if (!factory) {
        return 0;
    }

    IDXGIAdapter* adapter = NULL;
    int32_t result = 0;

    if (SUCCEEDED(factory->EnumAdapters(0, &adapter))) {
        IDXGIOutput* output = CGxDeviceD3d11::IGetOutputForMonitor(adapter, monitor);

        if (output) {
            static const CGxFormat::Format formats[3] = { CGxFormat::Fmt_Rgb565, CGxFormat::Fmt_Argb8888, CGxFormat::Fmt_Argb2101010 };
            static const uint32_t bpp[3] = { 16, 32, 32 };

            DXGI_MODE_DESC* descs = NULL;
            uint32_t i;

            for (i = 0; i < 3; i++) {
                DXGI_FORMAT format = CGxDeviceD3d11::s_GxFormatToDxgiFormat[formats[i]];
                UINT count = 0;

                if (FAILED(output->GetDisplayModeList(format, 0, &count, NULL))) {
                    break;
                }

                descs = static_cast<DXGI_MODE_DESC*>(SMemAlloc(sizeof(DXGI_MODE_DESC) * count, __FILE__, __LINE__, 0));

                if (FAILED(output->GetDisplayModeList(format, 0, &count, descs))) {
                    break;
                }

                for (uint32_t j = 0; j < count; j++) {
                    const DXGI_MODE_DESC& desc = descs[j];

                    if (desc.Width >= 640 && desc.Height >= 480 && desc.Scaling != DXGI_MODE_SCALING_CENTERED) {
                        CGxMonitorMode* mode = modes.New();
                        mode->m_size.x = desc.Width;
                        mode->m_size.y = desc.Height;
                        mode->m_bpp = bpp[i];
                        mode->m_refreshRate = desc.RefreshRate.Numerator;
                        mode->m_refreshRateDenom = desc.RefreshRate.Denominator;
                        mode->m_scanlineOrdering = desc.ScanlineOrdering;
                        mode->m_scaling = desc.Scaling;
                    }
                }

                if (descs) {
                    SMemFree(descs, "delete[]", -1, 0);
                    descs = NULL;
                }
            }

            if (i >= 3) {
                result = modes.Count() != 0;
            } else if (descs) {
                SMemFree(descs, "delete[]", -1, 0);
            }

            output->Release();
        }
    }

    if (adapter) {
        adapter->Release();
        adapter = NULL;
    }

    if (factory) {
        factory->Release();
        factory = NULL;
    }

    CGxDeviceD3d11::IUnloadD3dLib();

    return result;
}

// OFFSET: 0x00854810/0x1405848A0
void CGxDeviceD3d11::DeviceReadPixels(CiRect& rect, TSGrowableArray<CImVector>& pixels) {
    pixels.Clear();

    if (!this->m_context) {
        return;
    }

    D3D11_TEXTURE2D_DESC desc;
    desc.Width = this->m_format.m_size.x;
    desc.Height = this->m_format.m_size.y;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE;
    desc.MiscFlags = 0;

    ID3D11Texture2D* staging = NULL;

    if (FAILED(this->m_d3dDevice->CreateTexture2D(&desc, NULL, &staging))) {
        return;
    }

    ID3D11Texture2D* backBuffer;

    if (SUCCEEDED(this->m_swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backBuffer)))) {
        ID3D11Texture2D* source;

        if (this->m_format.m_multisampleCount == 1) {
            source = backBuffer;
        } else {
            this->m_d3dContext->ResolveSubresource(this->m_resolveTexture, 0, backBuffer, 0, s_GxFormatToDxgiFormat[this->m_format.m_colorFormat]);
            source = this->m_resolveTexture;
        }

        D3D11_BOX box;
        box.left = 0;
        box.top = 0;
        box.front = 0;
        box.right = this->m_format.m_size.x;
        box.bottom = this->m_format.m_size.y;
        box.back = 1;
        this->m_d3dContext->CopySubresourceRegion(staging, 0, 0, 0, 0, source, 0, &box);

        if (backBuffer) {
            backBuffer->Release();
            backBuffer = NULL;
        }

        D3D11_MAPPED_SUBRESOURCE mapped;

        if (SUCCEEDED(this->m_d3dContext->Map(staging, 0, D3D11_MAP_READ, 0, &mapped))) {
            uint32_t rows = rect.maxY - rect.minY;
            uint32_t cols = rect.maxX - rect.minX;
            pixels.SetCount(cols * rows);

            const uint32_t* src = static_cast<const uint32_t*>(mapped.pData) + rect.minX + ((mapped.RowPitch * rect.minY) >> 2);
            uint32_t* dest = reinterpret_cast<uint32_t*>(pixels.m_data);

            for (uint32_t row = 0; row < rows; row++) {
                for (uint32_t col = 0; col < cols; col++) {
                    uint32_t color = *src;
                    *dest = color ^ ((color ^ ((color << 16) | (color >> 16))) & 0xFF00FF);
                    src++;
                    dest++;
                }

                src += (mapped.RowPitch >> 2) - cols;
            }

            this->m_d3dContext->Unmap(staging, 0);
        }
    }

    staging->Release();
}

// OFFSET: 0x00854A20/0x140584B90
void CGxDeviceD3d11::DeviceReadDepths(CiRect& rect, TSGrowableArray<float>& depths) {
    depths.Clear();

    if (!this->m_context) {
        return;
    }

    if (this->m_format.m_multisampleCount > 1) {
        this->ClampRectToWindow(rect);
        depths.SetCount((rect.maxY - rect.minY) * (rect.maxX - rect.minX));
        memset(depths.m_data, 0, sizeof(float) * depths.Count());
        return;
    }

    D3D11_TEXTURE2D_DESC desc;
    desc.Width = this->m_format.m_size.x;
    desc.Height = this->m_format.m_size.y;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R32_TYPELESS;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE;
    desc.MiscFlags = 0;

    ID3D11Texture2D* staging = NULL;

    if (FAILED(this->m_d3dDevice->CreateTexture2D(&desc, NULL, &staging))) {
        return;
    }

    this->m_d3dContext->CopyResource(staging, this->m_depthTexture);

    D3D11_MAPPED_SUBRESOURCE mapped;

    if (SUCCEEDED(this->m_d3dContext->Map(staging, 0, D3D11_MAP_READ, 0, &mapped))) {
        uint32_t cols = rect.maxX - rect.minX;
        uint32_t rows = rect.maxY - rect.minY;
        uint32_t size = sizeof(float) * cols * rows;
        depths.SetCount(cols * rows);

        if (this->m_format.m_depthFormat == CGxFormat::Fmt_Ds320) {
            if (rect.minX || rect.minY || rows * mapped.RowPitch != size) {
                uint32_t srcOffset = mapped.RowPitch * rect.minY + sizeof(float) * rect.minX;
                uint32_t destIndex = 0;

                for (uint32_t row = 0; row < rows; row++) {
                    memcpy(&depths.m_data[destIndex], static_cast<const uint8_t*>(mapped.pData) + srcOffset, sizeof(float) * cols);
                    destIndex += mapped.RowPitch;
                    srcOffset += cols;
                }
            } else {
                memcpy(depths.m_data, mapped.pData, size);
            }
        }

        this->m_d3dContext->Unmap(staging, 0);
    }

    staging->Release();
}

// OFFSET: 0x00856020/0x140586BA0
CGxDeviceD3d11::CGxDeviceD3d11() {
    this->m_api = GxApi_D3d11;
    this->m_ownhwnd = 0;
    this->m_hwnd = NULL;
    this->m_d3dDevice = NULL;
    this->m_dxgiFactory = NULL;
    this->m_dxgiAdapter = NULL;
    this->m_dxgiDevice = NULL;
    this->m_swapChain = NULL;
    this->m_d3dContext = NULL;
    this->m_backBufferViews = NULL;
    this->m_depthViews = NULL;
    this->m_depthTexture = NULL;
    this->m_resolveTexture = NULL;
    this->m_hwndClass = 0;

    for (uint32_t i = 0; i < GxShTargets_Last; i++) {
        this->m_constantBuffers[i] = NULL;
    }

    this->m_stateDirty = (this->m_stateDirty & 0xFFFFFF00) | 0x3F;
    this->m_curInputLayout = NULL;
    this->m_curIndexBuffer = NULL;
    this->m_curVertexBuffers[0] = NULL;
    this->m_vertexStrides[0] = 0;
    this->m_curVertexBuffers[1] = NULL;
    this->m_vertexStrides[1] = 0;
    this->m_instanceCount = 1;
    this->m_curRasterizerState = NULL;
    this->m_curBlendState = NULL;
    this->m_curDepthState = NULL;
    this->m_cursor = NULL;
    this->m_clearVertexShader = NULL;
    this->m_clearPixelShader = NULL;
    this->m_passThroughVertexShader = NULL;
    this->m_stretchRectPixelShader = NULL;
    this->m_resolveVertexShader = NULL;
    this->m_resolvePixelShader = NULL;
    this->m_caps.m_colorFormat = GxCF_rgba;
    // cata sets CGxCaps::unk_001C here; this client's CGxCaps has no such field.

    // NOT IN BINARY: these three back-buffer wrappers do not exist in cata, so its constructor never
    // initialises them. IDestroyD3d() runs on a fresh device (DeviceSetFormat calls it before creating
    // anything), so leaving them uninitialised makes it free garbage pointers.
    this->m_frameBuffer = nullptr;
    this->m_backBufferColorTex = nullptr;
    this->m_backBufferDepthTex = nullptr;
    this->m_frameBufferVersion = 0;

    this->ITexScratchClear();
    this->DeviceCreatePools();
    this->DeviceCreateStreamBufs();
}

// OFFSET: [0x00856240/0x140586E20, 0x00856320/0x140586FB0]
CGxDeviceD3d11::~CGxDeviceD3d11() {
}

// OFFSET: 0x008544D0/0x1405843A0
void CGxDeviceD3d11::IReleaseD3dPools(int32_t freePools) {
    for (CGxPool* pool = CGxDevice::s_poolList.Head(), * next; !CGxDevice::s_poolList.IsEnd(pool); pool = next) {
        next = CGxDevice::s_poolList.RawNext(pool);

        if (!freePools && pool->m_usage != GxPoolUsage_Stream && pool->m_usage != GxPoolUsage_Dynamic) {
            continue;
        }

        if (pool->m_usage == GxPoolUsage_Stream) {
            pool->m_offset = 0;
        }

        pool->Invalidate();

        if (pool->m_apiSpecific) {
            static_cast<ID3D11Buffer*>(pool->m_apiSpecific)->Release();
            pool->m_apiSpecific = NULL;
        }
    }
}

// OFFSET: 0x00854C20/0x140584DF0
void CGxDeviceD3d11::IDestroyD3d() {
    if (this->m_d3dContext) {
        this->m_d3dContext->ClearState();
        this->m_d3dContext->Flush();
    }

    if (!this->m_format.m_window && this->m_swapChain) {
        this->m_swapChain->SetFullscreenState(FALSE, NULL);
    }

    for (uint32_t i = 0; i < 2; i++) {
        if (CGxDevice::s_uiVertexShader[i]) {
            this->ShaderRelease(CGxDevice::s_uiVertexShader[i]);
            CGxDevice::s_uiVertexShader[i] = NULL;
        }
    }

    if (CGxDevice::s_uiPixelShader) {
        this->ShaderRelease(CGxDevice::s_uiPixelShader);
        CGxDevice::s_uiPixelShader = NULL;
    }

    if (this->m_resolveVertexShader) {
        this->ShaderRelease(this->m_resolveVertexShader);
        this->m_resolveVertexShader = NULL;
    }

    if (this->m_resolvePixelShader) {
        this->ShaderRelease(this->m_resolvePixelShader);
        this->m_resolvePixelShader = NULL;
    }

    if (this->m_passThroughVertexShader) {
        this->ShaderRelease(this->m_passThroughVertexShader);
        this->m_passThroughVertexShader = NULL;
    }

    if (this->m_stretchRectPixelShader) {
        this->ShaderRelease(this->m_stretchRectPixelShader);
        this->m_stretchRectPixelShader = NULL;
    }

    if (this->m_clearVertexShader) {
        this->ShaderRelease(this->m_clearVertexShader);
        this->m_clearVertexShader = NULL;
    }

    if (this->m_clearPixelShader) {
        this->ShaderRelease(this->m_clearPixelShader);
        this->m_clearPixelShader = NULL;
    }

    // Free the back-buffer descriptors ICreateBackBufferFB allocated. Their m_apiSpecificData points at
    // the TexTargetViews owned by IReleaseTargets/IReleaseTargets below, so they go stale here either way;
    // dropping the wrappers avoids leaking one set per device recreation.
    if (this->m_frameBuffer) {
        this->m_frameBuffer->~CGxFrameBuffer();
        SMemFree(this->m_frameBuffer, __FILE__, __LINE__, 0);
        this->m_frameBuffer = nullptr;
    }

    if (this->m_backBufferColorTex) {
        this->m_backBufferColorTex->~CGxTex();
        SMemFree(this->m_backBufferColorTex, __FILE__, __LINE__, 0);
        this->m_backBufferColorTex = nullptr;
    }

    if (this->m_backBufferDepthTex) {
        this->m_backBufferDepthTex->~CGxTex();
        SMemFree(this->m_backBufferDepthTex, __FILE__, __LINE__, 0);
        this->m_backBufferDepthTex = nullptr;
    }

    this->m_frameBufferVersion = 0;

    this->ITexScratchDestroy();
    this->ICursorDestroy();
    this->ITexForceRecreation(1);
    this->IReleaseD3dShaders(1);
    this->ShaderConstantsClear();
    this->IReleaseD3dPools(1);
    this->IReleaseD3dQueries();
    this->IReleaseTargets();
    this->IStateReset();

    for (InputLayoutRec* rec = this->m_inputLayouts.Head(); rec; rec = this->m_inputLayouts.Next(rec)) {
        if (rec->m_layout) {
            rec->m_layout->Release();
            rec->m_layout = NULL;
        }
    }

    this->m_inputLayouts.Clear();

    for (RasterizerStateRec* rec = this->m_rasterizerStates.Head(); rec; rec = this->m_rasterizerStates.Next(rec)) {
        if (rec->m_state) {
            rec->m_state->Release();
            rec->m_state = NULL;
        }
    }

    this->m_rasterizerStates.Clear();

    for (BlendStateRec* rec = this->m_blendStates.Head(); rec; rec = this->m_blendStates.Next(rec)) {
        if (rec->m_state) {
            rec->m_state->Release();
            rec->m_state = NULL;
        }
    }

    this->m_blendStates.Clear();

    for (DepthStateRec* rec = this->m_depthStates.Head(); rec; rec = this->m_depthStates.Next(rec)) {
        if (rec->m_state) {
            rec->m_state->Release();
            rec->m_state = NULL;
        }
    }

    this->m_depthStates.Clear();

    for (SamplerStateRec* rec = this->m_samplerStates.Head(); rec; rec = this->m_samplerStates.Next(rec)) {
        if (rec->m_state) {
            rec->m_state->Release();
            rec->m_state = NULL;
        }
    }

    this->m_samplerStates.Clear();

    for (uint32_t i = 0; i < GxShTargets_Last; i++) {
        if (this->m_constantBuffers[i]) {
            this->m_constantBuffers[i]->Release();
            this->m_constantBuffers[i] = NULL;
        }
    }

    if (this->m_d3dContext) {
        this->m_d3dContext->Release();
        this->m_d3dContext = NULL;
    }

    if (this->m_swapChain) {
        this->m_swapChain->Release();
        this->m_swapChain = NULL;
    }

    if (this->m_dxgiFactory) {
        this->m_dxgiFactory->Release();
        this->m_dxgiFactory = NULL;
    }

    if (this->m_dxgiAdapter) {
        this->m_dxgiAdapter->Release();
        this->m_dxgiAdapter = NULL;
    }

    if (this->m_dxgiDevice) {
        this->m_dxgiDevice->Release();
        this->m_dxgiDevice = NULL;
    }

    if (this->m_d3dDevice) {
        this->m_d3dDevice->Release();
        this->m_d3dDevice = NULL;
    }

    this->m_context = 0;
}

// OFFSET: 0x00855150/0x140585530
void CGxDeviceD3d11::DeviceDestroy() {
    CGxDevice::DeviceDestroy();

    if (this->m_hwnd) {
        this->IWindowDestroy(&this->m_hwnd);
    }

    if (this->m_hwndClass) {
        this->IWindowClassDestroy(&this->m_hwndClass);
    }

    this->IDestroyD3d();
}

// OFFSET: 0x00855190/0x140585580
int32_t CGxDeviceD3d11::DeviceSetFormat(const CGxFormat& format) {
    CGxDevice::Log("CGxDeviceD3d11::DeviceSetFormat():");
    this->Log(format);
    this->IDestroyD3d();
    this->IWindowDestroy(&this->m_hwnd);
    this->m_format = format;

    CGxFormat createFormat = format;

    if (this->IWindowCreate(&this->m_hwnd, createFormat, 1) && this->ICreateD3dDevice(createFormat) && CGxDevice::DeviceSetFormat(createFormat)) {
        this->ISetWindowFocus(1);
        return 1;
    }

    CGxDevice::Log("CGxDeviceD3d11::DeviceSetFormat(): unable to set format!");
    this->IDestroyD3d();
    this->IWindowDestroy(&this->m_hwnd);

    return 0;
}

// OFFSET: 0x00850C60/0x1405824B0
void CGxDeviceD3d11::CursorSetVisible(int32_t visible) {
    if (visible == CGxDevice::s_cursorVisible) {
        return;
    }

    CGxDevice::CursorSetVisible(visible);

    if (this->m_hwCursor) {
        ShowCursor(CGxDevice::s_cursorVisible);
    }
}

// OFFSET: 0x00871FE0/0x140581DE0
void CGxDeviceD3d11::DeviceEvictResources() {
    CGxDevice::DeviceEvictResources();
}

// OFFSET: 0x00847950/0x1407D7E80
void CGxDeviceD3d11::StereoSetConvergence(float convergence) {
}

// OFFSET: 0x00599E10/0x1402082B0
float CGxDeviceD3d11::StereoGetConvergence() {
    return 1.0f;
}

// OFFSET: 0x00847950/0x1407D7E80
void CGxDeviceD3d11::StereoSetSeparation(float separation) {
}

// OFFSET: 0x0076CA20/0x140586E10
float CGxDeviceD3d11::StereoGetSeparation() {
    return 0.0f;
}

// OFFSET: 0x00B06D20/0x140317F90
bool CGxDeviceD3d11::StereoEnabled() {
    return false;
}

// OFFSET: 0x00841AE0/0x140211B90
void CGxDeviceD3d11::DebugGroupBegin(const char* name, char* buffer) {
}

// OFFSET: 0x00401000/0x1407D7E80
void CGxDeviceD3d11::DebugGroupEnd() {
}

// OFFSET: 0x00841AE0/0x140211B90
void CGxDeviceD3d11::DebugEvent(const char* name, char* buffer) {
}

// OFFSET: 0x008502D0/0x14057F5B0
int32_t CGxDevice::D3d11AdapterFormats(TSGrowableArray<CGxFormat>& formats) {
    if (!CGxDeviceD3d11::ILoadD3dLib()) {
        return 0;
    }

    ID3D11Device* device = NULL;
    D3D_FEATURE_LEVEL featureLevel;
    ID3D11DeviceContext* context;

    if (SUCCEEDED(s_D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &device, &featureLevel, &context))) {
        static const CGxFormat::Format colorFormats[3] = { CGxFormat::Fmt_Rgb565, CGxFormat::Fmt_Argb8888, CGxFormat::Fmt_Argb2101010 };
        static const CGxFormat::Format depthFormats[3] = { CGxFormat::Fmt_Ds160, CGxFormat::Fmt_Ds248, CGxFormat::Fmt_Ds320 };
        static const uint32_t sampleCounts[9] = { 1, 2, 4, 6, 8, 10, 12, 14, 16 };

        for (uint32_t i = 0; i < 3; i++) {
            CGxFormat::Format colorFormat = colorFormats[i];
            DXGI_FORMAT colorDxgi = CGxDeviceD3d11::s_GxFormatToDxgiFormat[colorFormat];
            UINT support;

            if (device->CheckFormatSupport(colorDxgi, &support) == E_FAIL || !(support & D3D11_FORMAT_SUPPORT_RENDER_TARGET)) {
                continue;
            }

            CGxFormat format;
            memset(&format, 0, sizeof(format));
            format.m_colorFormat = colorFormat;

            for (uint32_t j = 0; j < 3; j++) {
                CGxFormat::Format depthFormat = depthFormats[j];
                DXGI_FORMAT depthDxgi = CGxDeviceD3d11::s_GxFormatToDxgiFormat[depthFormat];

                if (device->CheckFormatSupport(depthDxgi, &support) == E_FAIL || !(support & D3D11_FORMAT_SUPPORT_DEPTH_STENCIL)) {
                    continue;
                }

                format.m_depthFormat = depthFormat;

                for (uint32_t k = 0; k < 9; k++) {
                    uint32_t sampleCount = sampleCounts[k];
                    UINT colorQuality;
                    UINT depthQuality;

                    if (FAILED(device->CheckMultisampleQualityLevels(colorDxgi, sampleCount, &colorQuality)) || FAILED(device->CheckMultisampleQualityLevels(depthDxgi, sampleCount, &depthQuality))) {
                        break;
                    }

                    if (colorQuality && depthQuality) {
                        format.m_multisampleCount = sampleCount;

                        if (!CGxDevice::FormatExists(formats, format)) {
                            *formats.New() = format;
                        }
                    }
                }
            }
        }

        if (device) {
            device->Release();
            device = NULL;
        }

        CGxDeviceD3d11::IUnloadD3dLib();

        return formats.Count() != 0;
    }

    if (device) {
        device->Release();
        device = NULL;
    }

    CGxDeviceD3d11::IUnloadD3dLib();

    return 0;
}
