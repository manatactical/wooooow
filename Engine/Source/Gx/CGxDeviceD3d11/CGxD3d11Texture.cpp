#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <new>
#include "Engine/Source/Gx/CGxDeviceD3d11/CGxDeviceD3d11.h"
#include "Engine/Source/Gx/Blit.h"
#include "Engine/Source/Gx/CGxBuf.h"
#include "Engine/Source/Gx/Gx.h"
#include "Engine/Source/Storm/H/storm.h"

// The DXGI tables below are ordered by this client's EGxTexFormat, not cata's 24-entry one:
//
//   0 Unknown  1 Abgr8888  2 Argb8888  3 Argb4444  4 Argb1555  5 Rgb565   6 Dxt1    7 Dxt3
//   8 Dxt5     9 Uv88     10 Gr1616F  11 R32F     12 D24X8     13 R16     14 Abgr16F 15 R24X8
//   16 Abgr32F 17 D16      18 D32
//
// Entries 0..12 are cata's rows re-ordered (cata's Uv88/Dxt1/Dxt3/Dxt5 sit one slot later there); 13..18 are
// cata's R16/Abgr16161616F/R24X8/Abgr32323232F/D16/D32 rows.
const EGxTexFormat CGxDeviceD3d11::s_GxTexFormatToUse[GxTexFormats_Last] = { GxTex_Unknown, GxTex_Abgr8888, GxTex_Abgr8888, GxTex_Abgr8888, GxTex_Abgr8888, GxTex_Abgr8888, GxTex_Dxt1, GxTex_Dxt3, GxTex_Dxt5, GxTex_Uv88, GxTex_Gr1616F, GxTex_R32F, GxTex_D24X8, GxTex_R16, GxTex_Abgr16161616F, GxTex_R24X8, GxTex_Abgr32323232F, GxTex_D16, GxTex_D32 };

const DXGI_FORMAT CGxDeviceD3d11::s_GxTexFormatToViewFormat[GxTexFormats_Last] = { DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_BC1_UNORM, DXGI_FORMAT_BC2_UNORM, DXGI_FORMAT_BC3_UNORM, DXGI_FORMAT_R8G8_SNORM, DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_D24_UNORM_S8_UINT, DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R24_UNORM_X8_TYPELESS, DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_D32_FLOAT };

const DXGI_FORMAT CGxDeviceD3d11::s_GxTexFormatToDxgiFormat[GxTexFormats_Last] = { DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_BC1_TYPELESS, DXGI_FORMAT_BC2_TYPELESS, DXGI_FORMAT_BC3_TYPELESS, DXGI_FORMAT_R8G8_TYPELESS, DXGI_FORMAT_R16G16_TYPELESS, DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_R24G8_TYPELESS, DXGI_FORMAT_R16_TYPELESS, DXGI_FORMAT_R16G16B16A16_TYPELESS, DXGI_FORMAT_R24G8_TYPELESS, DXGI_FORMAT_R32G32B32A32_TYPELESS, DXGI_FORMAT_R16_TYPELESS, DXGI_FORMAT_R32_TYPELESS };

const uint32_t CGxDeviceD3d11::s_texFormatResolvable[GxTexFormats_Last] = { 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 1, 1, 0, 1, 1, 1 };

const DXGI_FORMAT CGxDeviceD3d11::s_GxTexFormatToSrvFormat[GxTexFormats_Last] = { DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_BC1_UNORM, DXGI_FORMAT_BC2_UNORM, DXGI_FORMAT_BC3_UNORM, DXGI_FORMAT_R8G8_SNORM, DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R24_UNORM_X8_TYPELESS, DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R24_UNORM_X8_TYPELESS, DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_R32_FLOAT };

// OFFSET: 0x00401000/0x1407D7E80
static void ITexNullUserFunc(EGxTexCommand command, uint32_t width, uint32_t height, uint32_t face, uint32_t level, void* userArg, uint32_t& texelStrideInBytes, const void*& texels) {
}

static uint32_t ScratchSizeClass(uint32_t size) {
    uint32_t sizeClass = 0;
    uint32_t bits = (size >> 3) - 1;

    if ((size >> 3) != 1) {
        do {
            sizeClass++;
            bits &= bits - 1;
        } while (bits);
    }

    return sizeClass;
}

// OFFSET: 0x0086B0A0/0x14059F050
void D3d11Error(const char* file, int32_t line, const char* format, ...) {
    va_list args;
    va_start(args, format);
    CGxDevice::LogV(format, args);
    va_end(args);
}

// NOT IN BINARY: volume-aware sibling of CGxDevice::ITexWHDStartEnd. Same mip rules as the base, plus
// the depth this client's base helper has no parameter for.
void CGxDeviceD3d11::ITexWHD(CGxTex* texId, uint32_t& width, uint32_t& height, uint32_t& depth, uint32_t& baseMip, uint32_t& mipCount) {
    width = texId->m_width;
    height = texId->m_height;
    depth = texId->m_depth ? texId->m_depth : 1;

    if ((texId->m_flags.m_filter == GxTex_Nearest || texId->m_flags.m_filter == GxTex_Linear || texId->m_flags.m_generateMipMaps) && !texId->m_flags.m_forceMipTracking) {
        baseMip = 0;
        mipCount = 1;
        return;
    }

    uint32_t size;

    if (texId->m_format == GxTex_Dxt1 || texId->m_format == GxTex_Dxt3 || texId->m_format == GxTex_Dxt5) {
        size = texId->m_width < texId->m_height ? texId->m_width : texId->m_height;
    } else {
        size = texId->m_width > texId->m_height ? texId->m_width : texId->m_height;
    }

    uint32_t limit = texId->m_target == GxTex_Volume ? 1 : (texId->m_format == GxTex_Dxt1 || texId->m_format == GxTex_Dxt3 || texId->m_format == GxTex_Dxt5 ? 4 : 1);
    mipCount = 1;

    while (size > limit) {
        size >>= 1;
        mipCount++;
    }

    baseMip = this->m_baseMipLevel < mipCount - 1 ? this->m_baseMipLevel : mipCount - 1;
    width >>= baseMip;
    height >>= baseMip;
    depth >>= baseMip;

    if (depth < 1) {
        depth = 1;
    }

    if (texId->m_flags.m_forceMipTracking) {
        mipCount = baseMip + 1;
    }
}

// OFFSET: 0x0086B0C0/0x14059F070
int32_t CGxDeviceD3d11::ITexCreate(CGxTex* texId) {    uint32_t width;
    uint32_t height;
    uint32_t depth;
    uint32_t baseMip;
    uint32_t mipCount;
    this->ITexWHD(texId, width, height, depth, baseMip, mipCount);

    EGxTexFormat format = s_GxTexFormatToUse[texId->m_format];
    uint32_t mips = mipCount - baseMip;
    uint32_t multisample = texId->m_flags.m_bit8;
    texId->m_format = format;

    int32_t isCube = texId->m_target == GxTex_CubeMap;
    int32_t isVolume = texId->m_target == GxTex_Volume;
    uint32_t viewType = 0;
    ID3D11Resource* resource;

    if (isVolume) {
        D3D11_TEXTURE3D_DESC desc;
        desc.Width = width;
        desc.Height = height;
        desc.Depth = depth;
        desc.MipLevels = mips;
        desc.Format = s_GxTexFormatToDxgiFormat[format];
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.CPUAccessFlags = 0;
        desc.MiscFlags = 0;

        if (texId->m_flags.m_renderTarget) {
            viewType = 1;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        }

        ID3D11Texture3D* texture;
        HRESULT hr = this->m_d3dDevice->CreateTexture3D(&desc, NULL, &texture);

        if (FAILED(hr) || !texture) {
            if (hr == DXGI_ERROR_DEVICE_REMOVED) {
                GxLog("Device removed error when trying to create 3D texture:  Reason = 0x%08X\n", this->m_d3dDevice->GetDeviceRemovedReason());
            } else {
                D3d11Error(__FILE__, __LINE__, "Failed to Create API 3D Texture:\nHRESULT hr = 0x%08X\nName = %s\nGx Target = %d\nGx Format = %d\nGx Flags  = 0x%08X\nWidth = %d\nHeight = %d\nDepth = %d\nMip Levels = %d\nFormat = 0x%X\nUsage = %d\nBind Flags = %d\n", hr, "<Unknown>", texId->m_target, texId->m_format, texId->m_flags, desc.Width, desc.Height, desc.Depth, desc.MipLevels, desc.Format, desc.Usage, desc.BindFlags);
            }

            return 0;
        }

        resource = texture;

        if (!resource) {
            return 0;
        }
    } else {
        D3D11_TEXTURE2D_DESC desc;
        desc.Width = width;
        desc.Height = height;
        desc.ArraySize = isCube ? 6 : 1;
        desc.Format = s_GxTexFormatToDxgiFormat[format];
        desc.MipLevels = mips;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.CPUAccessFlags = 0;
        desc.MiscFlags = isCube ? D3D11_RESOURCE_MISC_TEXTURECUBE : 0;

        if (texId->m_flags.m_renderTarget) {
            if (CGxDevice::s_texFormatIsDepth[format]) {
                viewType = 2;
                desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_DEPTH_STENCIL;
            } else {
                viewType = 1;
                desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
            }
        }

        if (multisample) {
            desc.SampleDesc.Count = this->m_format.m_multisampleCount;
            desc.SampleDesc.Quality = static_cast<UINT>(static_cast<int64_t>(this->m_format.m_multisampleQuality));
        } else {
            desc.SampleDesc.Count = 1;
            desc.SampleDesc.Quality = 0;
        }

        ID3D11Texture2D* texture;
        HRESULT hr = this->m_d3dDevice->CreateTexture2D(&desc, NULL, &texture);

        if (FAILED(hr) || !texture) {
            if (hr == DXGI_ERROR_DEVICE_REMOVED) {
                GxLog("Device removed error when trying to create 2D texture:  Reason = 0x%08X\n", this->m_d3dDevice->GetDeviceRemovedReason());
            } else {
                D3d11Error(__FILE__, __LINE__, "Failed to Create API 2D Texture:\nHRESULT hr = 0x%08X\nName = %s\nGx Target = %d\nGx Format = %d\nGx Flags  = 0x%08X\nWidth = %d\nHeight = %d\nMip Levels = %d\nFormat = 0x%X\nUsage = %d\nBind Flags = %d\n", hr, "<Unknown>", texId->m_target, texId->m_format, texId->m_flags, desc.Width, desc.Height, desc.MipLevels, desc.Format, desc.Usage, desc.BindFlags);
            }

            return 0;
        }

        resource = texture;

        if (!resource) {
            return 0;
        }
    }

    ID3D11ShaderResourceView* srv = NULL;
    ID3D11RenderTargetView* rtv = NULL;
    ID3D11DepthStencilView* dsv = NULL;

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc;
    memset(&srvDesc, 0, sizeof(srvDesc));
    srvDesc.Format = s_GxTexFormatToSrvFormat[texId->m_format];

    if (isVolume) {
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D;
        srvDesc.Texture3D.MostDetailedMip = 0;
        srvDesc.Texture3D.MipLevels = mips;
    } else if (isCube) {
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
        srvDesc.TextureCube.MostDetailedMip = 0;
        srvDesc.TextureCube.MipLevels = mips;
    } else if (multisample) {
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMS;
    } else {
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = mips;
    }

    HRESULT srvResult = this->m_d3dDevice->CreateShaderResourceView(resource, &srvDesc, &srv);

    if (FAILED(srvResult)) {
        if (srvResult == DXGI_ERROR_DEVICE_REMOVED) {
            GxLog("Device removed error when trying to create shader resource view:  Reason = 0x%08X\n", this->m_d3dDevice->GetDeviceRemovedReason());
        } else {
            D3d11Error(__FILE__, __LINE__, "Failed to Create Shader Resource View:\nHRESULT hr = 0x%08X\nName = %s\nViewDimension = %d\nFormat = 0x%X\n", srvResult, "<Unknown>", srvDesc.ViewDimension, srvDesc.Format);
        }

        if (resource) {
            resource->Release();
        }

        return 0;
    }

    if (viewType == 2) {
        D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc;
        memset(&dsvDesc, 0, sizeof(dsvDesc));
        dsvDesc.Format = s_GxTexFormatToViewFormat[texId->m_format];
        dsvDesc.Flags = 0;

        if (isCube) {
            dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
            dsvDesc.Texture2DArray.FirstArraySlice = 0;
            dsvDesc.Texture2DArray.ArraySize = 6;
            dsvDesc.Texture2DArray.MipSlice = 0;
        } else if (multisample) {
            dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DMS;
        } else {
            dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
            dsvDesc.Texture2D.MipSlice = 0;
        }

        if (FAILED(this->m_d3dDevice->CreateDepthStencilView(resource, &dsvDesc, &dsv))) {
            if (srv) {
                srv->Release();
                srv = NULL;
            }

            if (resource) {
                resource->Release();
            }

            return 0;
        }
    } else if (viewType == 1) {
        D3D11_RENDER_TARGET_VIEW_DESC rtvDesc;
        memset(&rtvDesc, 0, sizeof(rtvDesc));
        rtvDesc.Format = s_GxTexFormatToViewFormat[texId->m_format];

        if (isVolume) {
            rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE3D;
            rtvDesc.Texture3D.FirstWSlice = 0;
            rtvDesc.Texture3D.WSize = depth;
            rtvDesc.Texture3D.MipSlice = 0;
        } else if (isCube) {
            rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
            rtvDesc.Texture2DArray.FirstArraySlice = 0;
            rtvDesc.Texture2DArray.ArraySize = 6;
            rtvDesc.Texture2DArray.MipSlice = 0;
        } else if (multisample) {
            rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DMS;
        } else {
            rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
            rtvDesc.Texture2D.MipSlice = 0;
        }

        if (FAILED(this->m_d3dDevice->CreateRenderTargetView(resource, &rtvDesc, &rtv))) {
            if (srv) {
                srv->Release();
                srv = NULL;
            }

            if (resource) {
                resource->Release();
            }

            return 0;
        }
    }

    TexViews* views = NULL;

    if (viewType == 0) {
        views = static_cast<TexViews*>(SMemAlloc(sizeof(TexViews), __FILE__, __LINE__, 0));
        views->m_type = 0;
        views->m_srv = srv;
    } else if (viewType == 2) {
        TexTargetViews* targetViews = static_cast<TexTargetViews*>(SMemAlloc(sizeof(TexTargetViews), __FILE__, __LINE__, 0));
        targetViews->m_type = 2;
        targetViews->m_srv = srv;
        targetViews->m_targetView = dsv;
        views = targetViews;
    } else if (viewType == 1) {
        TexTargetViews* targetViews = static_cast<TexTargetViews*>(SMemAlloc(sizeof(TexTargetViews), __FILE__, __LINE__, 0));
        targetViews->m_type = 1;
        targetViews->m_srv = srv;
        targetViews->m_targetView = rtv;
        views = targetViews;
    }

    texId->m_apiSpecificData = views;
    texId->m_apiSpecificData2 = resource;
    texId->m_needsCreation = 0;
    this->IResourceTouch(texId);

    return 1;
}

// OFFSET: 0x0086B6C0/0x14059F850
void CGxDeviceD3d11::ITexScratchClear() {
    for (uint32_t i = 0; i < GxTexFormats_Last; i++) {
        this->m_scratchCount[i] = 0;
    }
}

// OFFSET: 0x0086B760
void CGxDeviceD3d11::ITexCalcOffset(EGxTexFormat format, uint32_t pitch, uint32_t height, const RECT& rect, uint32_t& offset, uint32_t& size) {
    if (static_cast<uint32_t>(format - GxTex_Dxt1) > 2) {
        offset = pitch * rect.top + ((rect.left * CGxDevice::s_texFormatBitDepth[format]) >> 3);
        size = height * pitch;
    } else {
        offset = pitch * (rect.top >> 2) + CGxDevice::s_texFormatBytesPerBlock[format] * (rect.left >> 2);
        size = pitch * (height >> 2);
    }
}

// OFFSET: 0x0086B7D0
void CGxDeviceD3d11::ITexRectToBox(EGxTexFormat format, const RECT& rect, D3D11_BOX& box) {
    box.left = 0;
    box.top = 0;
    box.right = rect.right - rect.left;
    box.bottom = rect.bottom - rect.top;
    box.front = 0;
    box.back = 1;

    if (format >= GxTex_Dxt1 && format <= GxTex_Dxt5) {
        box.right = (box.right + 3) & 0xFFFFFFFC;
        box.bottom = (box.bottom + 3) & 0xFFFFFFFC;
    }
}

// OFFSET: 0x0086B830/0x14059F870
int32_t CGxDeviceD3d11::TexCopy(CGxTex* dest, const C2iVector& destPos, const C2iVector& sourcePos, const C2iVector& size, uint32_t level, uint32_t plane) {
    // This client's TexCopy takes only the destination; the source is the bound render target.
    if (!this->m_textureTarget[GxBuffers_Color].m_texture) {
        return 0;
    }

    CGxTex* source = this->m_textureTarget[GxBuffers_Color].m_texture;
    if (!this->m_context) {
        return 0;
    }

    if (!dest || ((dest->m_needsCreation || (!dest->m_apiSpecificData && !dest->m_apiSpecificData2)) && !this->ITexCreate(dest))) {
        return 0;
    }

    DXGI_SWAP_CHAIN_DESC desc;

    if (FAILED(this->m_swapChain->GetDesc(&desc))) {
        return 0;
    }

    ID3D11Texture2D* backBuffer = NULL;

    if (FAILED(this->m_swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backBuffer)))) {
        return 0;
    }

    ID3D11Resource* destRes = static_cast<ID3D11Resource*>(dest->m_apiSpecificData2);
    ID3D11Resource* sourceRes;

    if (this->m_format.m_multisampleCount == 1) {
        sourceRes = backBuffer;
    } else {
        this->m_d3dContext->ResolveSubresource(this->m_resolveTexture, 0, backBuffer, 0, s_GxFormatToDxgiFormat[this->m_format.m_colorFormat]);
        sourceRes = this->m_resolveTexture;
    }

    int32_t dx = destPos.x - sourcePos.x;
    int32_t dy = destPos.y - sourcePos.y;

    if (level || plane || dest->m_depth != 1 || dx || dy || sourcePos.x || sourcePos.y || size.x != desc.BufferDesc.Width || size.y != desc.BufferDesc.Height || dest->m_width != desc.BufferDesc.Width || dest->m_height != desc.BufferDesc.Height) {
        D3D11_BOX box;
        box.left = sourcePos.x;
        box.top = sourcePos.y;
        box.front = 0;
        box.right = sourcePos.x + size.x;
        box.bottom = sourcePos.y + size.y;
        box.back = 1;
        this->m_d3dContext->CopySubresourceRegion(destRes, level, dx, dy, plane, sourceRes, 0, &box);
    } else {
        this->m_d3dContext->CopyResource(destRes, sourceRes);
    }

    if (backBuffer) {
        backBuffer->Release();
    }

    return 1;
}

// OFFSET: 0x0086BA90/0x14059FAB0
void CGxDeviceD3d11::ITexReleaseD3dTexture(CGxTex* texId) {
    TexViews* views = static_cast<TexViews*>(texId->m_apiSpecificData);

    if (views) {
        switch (views->m_type) {
            case 0:
                if (views->m_srv) {
                    views->m_srv->Release();
                    views->m_srv = NULL;
                }

                break;

            case 1:
            case 2: {
                TexTargetViews* targetViews = static_cast<TexTargetViews*>(views);

                if (targetViews->m_srv) {
                    targetViews->m_srv->Release();
                    targetViews->m_srv = NULL;
                }

                if (targetViews->m_targetView) {
                    targetViews->m_targetView->Release();
                    targetViews->m_targetView = NULL;
                }

                break;
            }

            default:
                break;
        }

        SMemFree(views, "delete", -1, 0);
        texId->m_apiSpecificData = NULL;
    }

    if (texId->m_apiSpecificData2) {
        static_cast<ID3D11Resource*>(texId->m_apiSpecificData2)->Release();
        texId->m_apiSpecificData2 = NULL;
    }

    texId->m_link2.Unlink();
    this->IResourceTouch(texId);
}

// OFFSET: 0x0086BB60/0x14059FBC0
void CGxDeviceD3d11::TexDestroy(CGxTex* texId) {
    if (!texId->m_needsCreation && (texId->m_apiSpecificData || texId->m_apiSpecificData2)) {
        this->ITexReleaseD3dTexture(texId);
    }

    CGxDevice::TexDestroy(texId);
}

// OFFSET: 0x0086BB90
void CGxDeviceD3d11::ITexScratchRelease(ScratchTex* scratch) {
    uint32_t sizeClass = ScratchSizeClass(scratch->m_size);
    this->m_scratchTextures[13 * sizeClass + scratch->m_format - 1].LinkToTail(scratch);
}

// OFFSET: 0x0086BBE0
void CGxDeviceD3d11::ITexScratchAlloc(EGxTexFormat format, uint32_t count) {
    DXGI_FORMAT dxgiFormat = s_GxTexFormatToDxgiFormat[format];

    for (uint32_t i = 0; i < count; i++) {
        for (uint32_t size = 256; size; size >>= 1) {
            uint32_t texSize = size < 8 ? 8 : size;
            uint32_t sizeClass = ScratchSizeClass(texSize);

            D3D11_TEXTURE2D_DESC desc;
            desc.Width = texSize;
            desc.Height = texSize;
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = dxgiFormat;
            desc.SampleDesc.Count = 1;
            desc.SampleDesc.Quality = 0;
            desc.Usage = D3D11_USAGE_STAGING;
            desc.BindFlags = 0;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            desc.MiscFlags = 0;

            ID3D11Texture2D* texture = NULL;

            if (FAILED(this->m_d3dDevice->CreateTexture2D(&desc, NULL, &texture))) {
                return;
            }

            ScratchTex* scratch = new ScratchTex;
            scratch->m_format = format;
            scratch->m_dxgiFormat = dxgiFormat;
            scratch->m_frame = 0;
            scratch->m_size = texSize;
            scratch->m_texture = texture;
            this->m_scratchTextures[format - 1 + 13 * sizeClass].LinkToTail(scratch);
        }
    }
}

// OFFSET: 0x0086BD60
CGxDeviceD3d11::ScratchTex* CGxDeviceD3d11::ITexScratchGet(uint32_t frame, uint32_t size, EGxTexFormat format, int32_t& reused) {
    ScratchTex* found;

    while (true) {
        uint32_t texSize = size < 8 ? 8 : size;
        uint32_t sizeClass = ScratchSizeClass(texSize);
        TSExplicitList<ScratchTex, offsetof(ScratchTex, m_link)>& list = this->m_scratchTextures[format - 1 + 13 * sizeClass];

        found = NULL;

        for (ScratchTex* scratch = list.Head(); scratch; scratch = list.Next(scratch)) {
            if (scratch->m_size == texSize && scratch->m_dxgiFormat == s_GxTexFormatToDxgiFormat[format]) {
                found = scratch;

                if (scratch->m_frame != frame) {
                    break;
                }
            }
        }

        if (found && found->m_frame != frame) {
            break;
        }

        uint32_t count = this->m_scratchCount[format];

        if (count == 32) {
            break;
        }

        count *= 2;

        if (!count) {
            count = 1;
        }

        this->m_scratchCount[format] = count;
        this->ITexScratchAlloc(format, count);
    }

    found->m_link.Unlink();
    reused = found->m_frame == frame;
    found->m_frame = frame;

    return found;
}

// OFFSET: 0x0086BE90/0x14059FF80
void CGxDeviceD3d11::ITexScratchDestroy() {
    for (uint32_t i = 0; i < 78; i++) {
        TSExplicitList<ScratchTex, offsetof(ScratchTex, m_link)>& list = this->m_scratchTextures[i];
        ScratchTex* next;

        for (ScratchTex* scratch = list.Head(); scratch; scratch = next) {
            next = list.Next(scratch);

            if (scratch->m_texture) {
                scratch->m_texture->Release();
                scratch->m_texture = NULL;
            }

            scratch->m_link.Unlink();
            delete scratch;
        }

        list.UnlinkAll();
    }

    for (uint32_t i = 0; i < GxTexFormats_Last; i++) {
        this->m_scratchCount[i] = 0;
    }
}

// OFFSET: 0x0086BFF0/0x1405A00F0
void CGxDeviceD3d11::ITexUploadRect(uint32_t size, CGxTex* texId, const void* bits, uint32_t pitch, uint32_t height, const RECT& rect, uint32_t plane, uint32_t subresource) {
    uint32_t frame = g_theGxDevicePtr->m_frameCount;
    uint32_t offset = 0;
    uint32_t planeSize = 0;

    if (!texId->m_flags.m_bit15) {
        CGxDeviceD3d11::ITexCalcOffset(texId->m_dataFormat, pitch, height, rect, offset, planeSize);
    }

    const uint8_t* src = static_cast<const uint8_t*>(bits) + offset + plane * planeSize;
    int32_t reused = 0;
    ScratchTex* scratch;
    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT result;

    for (scratch = this->ITexScratchGet(frame, size, texId->m_format, reused); ; scratch = this->ITexScratchGet(frame, size, texId->m_format, reused)) {
        result = this->m_d3dContext->Map(scratch->m_texture, 0, D3D11_MAP_WRITE, reused ? 0 : D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);

        if (result != DXGI_ERROR_WAS_STILL_DRAWING) {
            break;
        }

        this->ITexScratchRelease(scratch);
    }

    if (SUCCEEDED(result)) {
        C2iVector blitSize;
        blitSize.x = rect.right - rect.left;
        blitSize.y = rect.bottom - rect.top;
        Blit(blitSize, BlitAlpha_0, src, pitch, GxGetBlitFormat(texId->m_dataFormat), mapped.pData, mapped.RowPitch, GxGetBlitFormat(texId->m_format));

        ID3D11Resource* dest = static_cast<ID3D11Resource*>(texId->m_apiSpecificData2);
        D3D11_BOX box;
        CGxDeviceD3d11::ITexRectToBox(texId->m_format, rect, box);
        this->m_d3dContext->Unmap(scratch->m_texture, 0);
        this->m_d3dContext->CopySubresourceRegion(dest, subresource, rect.left, rect.top, plane, scratch->m_texture, 0, &box);
    }

    this->ITexScratchRelease(scratch);
}

// OFFSET: 0x0086C210/0x1405A0450
void CGxDeviceD3d11::ITexUpload(CGxTex* texId) {
    uint32_t texelStrideInBytes;
    const void* texels = NULL;
    texId->m_userFunc(GxTex_Lock, texId->m_width, texId->m_height, 0, 0, texId->m_userArg, texelStrideInBytes, texels);

    uint32_t width;
    uint32_t height;
    uint32_t depth;
    uint32_t baseMip;
    uint32_t mipCount;
    this->ITexWHD(texId, width, height, depth, baseMip, mipCount);

    int32_t isVolume = texId->m_target == GxTex_Volume;
    uint32_t faceCount = texId->m_target != GxTex_CubeMap ? 1 : 6;
    uint32_t mipRange = mipCount - baseMip;
    uint32_t subresourceBase = 0;

    for (uint32_t face = 0; face < faceCount; face++) {
        uint32_t mipWidth = width;
        uint32_t mipHeight = height;
        uint32_t tile = 256;

        while (true) {
            uint32_t largest = width;

            if (largest <= height) {
                largest = height;
            }

            if ((tile >> 1) < largest) {
                break;
            }

            tile >>= 1;

            if (tile <= 8) {
                break;
            }
        }

        for (uint32_t mip = baseMip; mip != mipCount; mip++) {
            uint32_t plane = isVolume ? depth : face;
            texels = NULL;
            texId->m_userFunc(GxTex_Latch, mipWidth, mipHeight, plane, mip, texId->m_userArg, texelStrideInBytes, texels);

            if (texels && !texId->m_flags.m_renderTarget) {
                int32_t y0 = texId->m_updateRect.minY >> mip;
                int32_t x0 = texId->m_updateRect.minX >> mip;
                int32_t y1 = ((texId->m_updateRect.maxY - 1) >> mip) + 1;
                int32_t x1 = ((texId->m_updateRect.maxX - 1) >> mip) + 1;

                if (y1 >= static_cast<int32_t>(mipHeight)) {
                    y1 = mipHeight;
                }

                if (x1 >= static_cast<int32_t>(mipWidth)) {
                    x1 = mipWidth;
                }

                int32_t rows = y1 - y0;

                if (rows > 0) {
                    int32_t cols = x1 - x0;

                    do {
                        int32_t x = x0;

                        if (cols > 0) {
                            int32_t xEnd = x0 + tile;
                            int32_t yEnd = y0 + tile;
                            int32_t remaining = cols;

                            while (true) {
                                RECT rect;
                                rect.left = x;
                                rect.top = y0;
                                rect.right = xEnd >= x1 ? x1 : xEnd;
                                rect.bottom = yEnd >= y1 ? y1 : yEnd;

                                uint32_t subresource = mip + subresourceBase - baseMip;
                                uint32_t planeStart = texId->m_updatePlaneMin >> mip;
                                uint32_t planeEnd = texId->m_updatePlaneMax >> mip;

                                if (planeStart == planeEnd) {
                                    planeEnd++;
                                }

                                for (uint32_t p = planeStart; p < planeEnd; p++) {
                                    this->ITexUploadRect(tile, texId, texels, texelStrideInBytes, mipHeight, rect, p, subresource);
                                }

                                x += tile;
                                xEnd += tile;
                                remaining -= tile;

                                if (remaining <= 0) {
                                    break;
                                }
                            }
                        }

                        y0 += tile;
                        rows -= tile;
                    } while (rows > 0);
                }
            }

            tile >>= 1;

            if (tile < 8) {
                tile = 8;
            }

            mipHeight >>= 1;
            mipWidth >>= 1;

            if (!mipWidth) {
                mipWidth = 1;
            }

            if (!mipHeight) {
                mipHeight = 1;
            }
        }

        subresourceBase += mipRange;
    }

    texels = NULL;
    texId->m_userFunc(GxTex_Unlock, texId->m_width, texId->m_height, 0, 0, texId->m_userArg, texelStrideInBytes, texels);
}

// OFFSET: 0x0086C530/0x1405A0980
void CGxDeviceD3d11::ITexMarkAsUpdated(CGxTex* texId) {
    if (!texId->m_needsUpdate || !this->m_context) {
        return;
    }

    if (texId->m_needsCreation || (!texId->m_apiSpecificData && !texId->m_apiSpecificData2)) {
        this->ITexCreate(texId);
    }

    if (texId->m_needsCreation || (!texId->m_apiSpecificData && !texId->m_apiSpecificData2)) {
        return;
    }

    if (texId->m_userFunc) {
        this->ITexUpload(texId);
    }

    CGxDevice::ITexMarkAsUpdated(texId);
}

// OFFSET: 0x0086C590/0x1405A0A00
void CGxDeviceD3d11::ITexForceRecreation(int32_t all) {
    this->ITexScratchDestroy();

    for (CGxTex* texId = this->m_textures.Head(); texId; texId = texId->m_link.Next()) {
        if (!texId->m_needsCreation && (texId->m_apiSpecificData || texId->m_apiSpecificData2)) {
            this->ITexReleaseD3dTexture(texId);
        }

        if (all || texId->m_flags.m_renderTarget) {
            texId->m_needsCreation = 1;

            static CiRect s_updateRect;

            this->TexMarkForUpdate(texId, s_updateRect, 0);

            uint32_t texelStrideInBytes;
            const void* texels;
            texId->m_userFunc(GxTex_3, texId->m_width, texId->m_height, 0, 0, texId->m_userArg, texelStrideInBytes, texels);
        }
    }

    if (this->m_hwnd && this->m_d3dDevice) {
        this->NotifyOnTextureRecreation();
    }
}

static void StretchQuad(CGxBuf*& vertexBuf, CGxBuf*& indexBuf, float u, float v) {
    float vertices[20] = {
        -1.0f, 1.0f, 1.0f, 0.0f, 0.0f,
        1.0f, 1.0f, 1.0f, u, 0.0f,
        -1.0f, -1.0f, 1.0f, 0.0f, v,
        1.0f, -1.0f, 1.0f, u, v
    };

    uint16_t indices[4] = { 0, 2, 1, 3 };

    vertexBuf = g_theGxDevicePtr->BufStream(GxPoolTarget_Vertex, 20, 4);
    memcpy(GxBufLock(vertexBuf), vertices, sizeof(vertices));
    GxBufUnlock(vertexBuf, 0);

    indexBuf = g_theGxDevicePtr->BufStream(GxPoolTarget_Index, 2, 4);
    memcpy(GxBufLock(indexBuf), indices, sizeof(indices));
    GxBufUnlock(indexBuf, 0);
}

// OFFSET: 0x0086C680
int32_t CGxDeviceD3d11::TexStretch(CGxTex* dest, const CiRect* destRect, const CiRect* sourceRect, uint32_t level, uint32_t face) {
    // This client's TexStretch takes only the destination; the source is the bound render target.
    if (!this->m_textureTarget[GxBuffers_Color].m_texture) {
        return 0;
    }

    CGxTex* source = this->m_textureTarget[GxBuffers_Color].m_texture;
    if (!this->m_context) {
        return 0;
    }

    if (!dest || ((dest->m_needsCreation || (!dest->m_apiSpecificData && !dest->m_apiSpecificData2)) && !this->ITexCreate(dest))) {
        return 0;
    }

    ID3D11Resource* sourceRes;
    uint32_t sourceWidth;
    uint32_t sourceHeight;
    DXGI_FORMAT sourceFormat;

    if (source) {
        if ((source->m_needsCreation || (!source->m_apiSpecificData && !source->m_apiSpecificData2)) && !this->ITexCreate(source)) {
            return 0;
        }

        sourceRes = static_cast<ID3D11Resource*>(source->m_apiSpecificData2);
        sourceRes->AddRef();

        D3D11_TEXTURE2D_DESC desc;
        static_cast<ID3D11Texture2D*>(sourceRes)->GetDesc(&desc);
        sourceWidth = desc.Width;
        sourceHeight = desc.Height;
        sourceFormat = desc.Format;
    } else {
        ID3D11Texture2D* backBuffer = NULL;

        if (FAILED(this->m_swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backBuffer)))) {
            return 0;
        }

        if (this->m_format.m_multisampleCount == 1) {
            sourceRes = backBuffer;
        } else {
            this->m_d3dContext->ResolveSubresource(this->m_resolveTexture, 0, backBuffer, 0, s_GxFormatToDxgiFormat[this->m_format.m_colorFormat]);
            sourceRes = this->m_resolveTexture;
            sourceRes->AddRef();

            if (backBuffer) {
                backBuffer->Release();
                backBuffer = NULL;
            }
        }

        DXGI_SWAP_CHAIN_DESC desc;

        if (FAILED(this->m_swapChain->GetDesc(&desc))) {
            return 0;
        }

        sourceFormat = s_GxFormatToDxgiTypeless[this->m_format.m_colorFormat];
        sourceWidth = desc.BufferDesc.Width;
        sourceHeight = desc.BufferDesc.Height;
    }

    ID3D11Resource* destRes = static_cast<ID3D11Resource*>(dest->m_apiSpecificData2);
    int32_t sameFormat = dest->m_format < CGxFormat::Formats_Last && sourceFormat == s_GxFormatToDxgiTypeless[dest->m_format];
    int32_t fullCopy = sameFormat && !level && !face && dest->m_depth == 1 && dest->m_width == sourceWidth && dest->m_height == sourceHeight;
    int32_t regionCopy;

    if (destRect && sourceRect) {
        int32_t dx = destRect->minX - sourceRect->minX;
        int32_t dy = destRect->minY - sourceRect->minY;

        regionCopy = 0;

        if (sameFormat && sourceRect->maxX - sourceRect->minX == destRect->maxX - destRect->minX && sourceRect->maxY - sourceRect->minY == destRect->maxY - destRect->minY) {
            regionCopy = 1;

            if (fullCopy && !dx && !dy && !sourceRect->minX && !sourceRect->minY && sourceRect->maxX == sourceWidth && sourceRect->maxY == sourceHeight && destRect->maxX == sourceWidth && destRect->maxY == sourceHeight) {
                this->m_d3dContext->CopyResource(destRes, sourceRes);
                regionCopy = 0;
            } else {
                fullCopy = 0;
            }
        } else {
            fullCopy = 0;
        }

        if (regionCopy) {
            D3D11_BOX box;
            box.left = sourceRect->minX;
            box.top = sourceRect->minY;
            box.front = 0;
            box.right = sourceRect->maxX;
            box.bottom = sourceRect->maxY;
            box.back = destRect->maxY;
            this->m_d3dContext->CopySubresourceRegion(destRes, level, dx, dy, face, sourceRes, 0, &box);
        }
    } else {
        regionCopy = sameFormat && dest->m_width == sourceWidth && dest->m_height == sourceHeight;

        if (fullCopy) {
            this->m_d3dContext->CopyResource(destRes, sourceRes);
        } else if (regionCopy) {
            D3D11_BOX box;

            if (sourceRect) {
                box.left = sourceRect->minX;
                box.top = sourceRect->minY;
                box.right = sourceRect->maxX;
                box.bottom = sourceRect->maxY;
            } else {
                box.left = 0;
                box.top = 0;
                box.right = sourceWidth;
                box.bottom = sourceHeight;
            }

            box.front = 0;
            box.back = 1;
            this->m_d3dContext->CopySubresourceRegion(destRes, level, 0, 0, face, sourceRes, 0, &box);
        }
    }

    if (sourceRes) {
        sourceRes->Release();
    }

    if (fullCopy || regionCopy) {
        return 1;
    }

    if (level || face || dest->m_depth != 1) {
        return 0;
    }

    TexViews* views = static_cast<TexViews*>(dest->m_apiSpecificData);
    ID3D11RenderTargetView* rtv = NULL;

    if (views->m_type == 1) {
        rtv = static_cast<ID3D11RenderTargetView*>(static_cast<TexTargetViews*>(views)->m_targetView);
    }

    if (!rtv) {
        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc;
        views->m_srv->GetDesc(&srvDesc);

        D3D11_RENDER_TARGET_VIEW_DESC rtvDesc;
        memset(&rtvDesc, 0, sizeof(rtvDesc));
        rtvDesc.Format = srvDesc.Format;
        rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;

        if (FAILED(this->m_d3dDevice->CreateRenderTargetView(destRes, &rtvDesc, &rtv))) {
            return 0;
        }

        TexTargetViews* targetViews = static_cast<TexTargetViews*>(SMemAlloc(sizeof(TexTargetViews), __FILE__, __LINE__, 0));
        targetViews->m_type = 1;
        targetViews->m_srv = views->m_srv;
        targetViews->m_targetView = rtv;
        dest->m_apiSpecificData = targetViews;
        delete views;
    }

    RenderTargets targets;
    targets.m_numRenderTargets = 1;
    targets.m_renderTargets[0] = rtv;
    this->m_d3dContext->OMSetRenderTargets(1, targets.m_renderTargets, targets.m_curDepthView);

    D3D11_VIEWPORT viewport;
    viewport.TopLeftX = 0.0f;
    viewport.TopLeftY = 0.0f;
    viewport.Width = static_cast<float>(dest->m_width);
    viewport.Height = static_cast<float>(dest->m_height);
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    this->m_viewportDirty = 0;
    this->m_d3dContext->RSSetViewports(1, &viewport);

    g_theGxDevicePtr->RsPush();

    int32_t polygonFill = GxMasterEnable(GxMasterEnable_PolygonFill);
    GxMasterEnableSet(GxMasterEnable_PolygonFill, 1);
    GxRsSet(GxRs_ColorWrite, 15);
    GxRsSet(GxRs_DepthWrite, 0);
    GxRsSet(GxRs_DepthTest, 0);
    GxRsSet(GxRs_BlendingMode, 0);
    GxRsSet(GxRs_Lighting, 0);
    GxRsSet(GxRs_Culling, 0);
    GxRsSet(GxRs_ScissorTest, 0);
    GxRsSet(GxRs_VertexShader, this->m_passThroughVertexShader, 0);
    GxRsSet(GxRs_PixelShader, this->m_stretchRectPixelShader, 0);

    CGxBuf* vertexBuf;
    CGxBuf* indexBuf;
    StretchQuad(vertexBuf, indexBuf, 1.0f, 1.0f);
    GxPrimVertexPtr(vertexBuf, GxVBF_PT);
    GxPrimIndexPtr(indexBuf);

    if (source) {
        GxRsSet(GxRs_Texture0, source);
    } else {
        // STATIC: 0x00EC69B0
        CGxTexFlags flags;
        static CGxTex s_backBufferTex(GxTex_2d, sourceWidth, sourceHeight, 1, GxTex_Abgr8888, GxTex_Abgr8888, flags, NULL, ITexNullUserFunc, NULL);

        s_backBufferTex = CGxTex(GxTex_2d, sourceWidth, sourceHeight, 1, GxTex_Abgr8888, GxTex_Abgr8888, flags, NULL, ITexNullUserFunc, NULL);
        s_backBufferTex.m_apiSpecificData = this->m_backBufferViews;
        s_backBufferTex.m_needsCreation = 0;
        s_backBufferTex.m_needsUpdate = 0;
        g_theGxDevicePtr->RsSet(GxRs_Texture0, &s_backBufferTex);
    }

    CGxBatch batch;
    batch.m_primType = GxPrim_TriangleStrip;
    batch.m_start = 0;
    batch.m_count = 4;
    batch.m_minIndex = 0;
    batch.m_maxIndex = 3;
    g_theGxDevicePtr->Draw(&batch, 1);
    g_theGxDevicePtr->MasterEnableSet(GxMasterEnable_PolygonFill, polygonFill);
    g_theGxDevicePtr->RsPop();

    this->m_viewportDirty = 1;
    this->m_d3dContext->OMSetRenderTargets(this->m_targets.m_numRenderTargets, this->m_targets.m_renderTargets, this->m_targets.m_curDepthView);

    return 1;
}

// OFFSET: 0x0086CEB0/0x1405A16C0
int32_t CGxDeviceD3d11::TexResolve(CGxTex* source, CGxTex* dest) {
    if ((source->m_needsCreation || (!source->m_apiSpecificData && !source->m_apiSpecificData2)) && !this->ITexCreate(source)) {
        return 0;
    }

    ID3D11Resource* sourceRes = static_cast<ID3D11Resource*>(source->m_apiSpecificData2);

    if ((dest->m_needsCreation || (!dest->m_apiSpecificData && !dest->m_apiSpecificData2)) && !this->ITexCreate(dest)) {
        return 0;
    }

    if (s_texFormatResolvable[source->m_format] && s_texFormatResolvable[dest->m_format]) {
        this->m_d3dContext->ResolveSubresource(static_cast<ID3D11Resource*>(dest->m_apiSpecificData2), 0, sourceRes, 0, s_GxTexFormatToViewFormat[source->m_format]);
        return 1;
    }

    ID3D11RenderTargetView* targets[10];
    memset(targets, 0, sizeof(targets));
    targets[0] = static_cast<ID3D11RenderTargetView*>(static_cast<TexTargetViews*>(dest->m_apiSpecificData)->m_targetView);
    this->m_d3dContext->OMSetRenderTargets(1, targets, NULL);

    float minX = 0.0f;
    float minY = 0.0f;
    float minZ = 0.0f;
    float maxX = 0.0f;
    float maxY = 0.0f;
    float maxZ = 0.0f;
    GxXformViewport(minX, maxX, minY, maxY, minZ, maxZ);
    GxXformSetViewport(0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f);

    g_theGxDevicePtr->RsPush();

    int32_t polygonFill = g_theGxDevicePtr->MasterEnable(GxMasterEnable_PolygonFill);
    g_theGxDevicePtr->MasterEnableSet(GxMasterEnable_PolygonFill, 1);
    GxRsSet(GxRs_ColorWrite, 15);
    GxRsSet(GxRs_DepthWrite, 0);
    GxRsSet(GxRs_DepthTest, 0);
    GxRsSet(GxRs_BlendingMode, 0);
    GxRsSet(GxRs_Lighting, 0);
    GxRsSet(GxRs_Culling, 0);
    GxRsSet(GxRs_ScissorTest, 0);

    uint32_t permute = 0;

    switch (g_theGxDevicePtr->DeviceFormat().m_multisampleCount) {
        case 2:
            permute = 0;
            break;

        case 4:
            permute = 1;
            break;

        case 8:
            permute = 2;
            break;

        case 16:
            permute = 3;
            break;

        case 32:
            permute = 4;
            break;

        default:
            break;
    }

    g_theGxDevicePtr->RsSet(GxRs_VertexShader, &this->m_resolveVertexShader->m_permutes[0]);
    g_theGxDevicePtr->RsSet(GxRs_PixelShader, &this->m_resolvePixelShader->m_permutes[permute]);

    CGxBuf* vertexBuf;
    CGxBuf* indexBuf;
    StretchQuad(vertexBuf, indexBuf, static_cast<float>(source->m_width), static_cast<float>(source->m_height));
    g_theGxDevicePtr->PrimVertexPtr(vertexBuf, GxVBF_PT);
    g_theGxDevicePtr->PrimIndexPtr(indexBuf);
    g_theGxDevicePtr->RsSet(GxRs_Texture0, source);

    CGxBatch batch;
    batch.m_primType = GxPrim_TriangleStrip;
    batch.m_start = 0;
    batch.m_count = 4;
    batch.m_minIndex = 0;
    batch.m_maxIndex = 3;
    g_theGxDevicePtr->Draw(&batch, 1);
    g_theGxDevicePtr->MasterEnableSet(GxMasterEnable_PolygonFill, polygonFill);
    g_theGxDevicePtr->RsPop();

    GxXformSetViewport(minX, maxX, minY, maxY, minZ, maxZ);
    this->m_d3dContext->OMSetRenderTargets(this->m_targets.m_numRenderTargets, this->m_targets.m_renderTargets, this->m_targets.m_curDepthView);

    return 1;
}

// OFFSET: 0x00847950/0x1407D7E80
void CGxDeviceD3d11::IResourceTouch(const void* resource) {
}
