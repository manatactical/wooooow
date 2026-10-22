#ifndef ENGINE_SOURCE_GX_CGXDEVICED3D11_CGXDEVICED3D11_H
#define ENGINE_SOURCE_GX_CGXDEVICED3D11_CGXDEVICED3D11_H

#include <cstddef>
#include <cstdint>
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include "Engine/Source/Gx/CGxDevice/CGxDevice.h"
#include "Engine/Source/Gx/CGxDevice/EmergencyMem.h"
#include "Engine/Source/Gx/CGxTex.h"
#include "Engine/Source/Storm/H/stpl.h"

class CGxFrameBuffer;

class CGxDeviceD3d11 : public CGxDevice {
    public:
    struct TexViews {
        /* 0000 */ uint32_t m_type;
        /* 0004 */ ID3D11ShaderResourceView* m_srv;
    };

    struct TexTargetViews : public TexViews {
        /* 0008 */ ID3D11View* m_targetView;
    };

    struct RenderTargets {
        /* 0000 */ uint32_t m_numRenderTargets;
        /* 0004 */ ID3D11RenderTargetView* m_renderTargets [8];
        /* 0024 */ ID3D11DepthStencilView* m_curDepthView;
        /* 0028 */ EGxTexFormat m_colorTargetFormat;

        RenderTargets();
    };

    struct ScratchTex {
        /* 0000 */ uint32_t m_frame;
        /* 0004 */ uint32_t m_size;
        /* 0008 */ EGxTexFormat m_format;
        /* 000C */ DXGI_FORMAT m_dxgiFormat;
        /* 0010 */ ID3D11Texture2D* m_texture;
        /* 0014 */ TSLink<ScratchTex> m_link;
    };

    struct InputLayoutKey {
        /* 0000 */ uint32_t m_format;
        /* 0004 */ uint32_t m_mask;

        bool operator==(const InputLayoutKey& key);
    };

    class InputLayoutRec : public TSHashObject<InputLayoutRec, InputLayoutKey> {
        public:
        /* 001C */ ID3D11InputLayout* m_layout;
    };

    class RasterizerStateRec : public TSHashObject<RasterizerStateRec, HASHKEY_NONE> {
        public:
        /* 0018 */ ID3D11RasterizerState* m_state;
    };

    class BlendStateRec : public TSHashObject<BlendStateRec, HASHKEY_NONE> {
        public:
        /* 0018 */ ID3D11BlendState* m_state;
    };

    class DepthStateRec : public TSHashObject<DepthStateRec, HASHKEY_NONE> {
        public:
        /* 0018 */ ID3D11DepthStencilState* m_state;
    };

    class SamplerStateRec : public TSHashObject<SamplerStateRec, HASHKEY_NONE> {
        public:
        /* 0018 */ ID3D11SamplerState* m_state;
    };

    enum {
        StateDirty_Blend = 0x1,
        StateDirty_BlendApply = 0x2,
        StateDirty_Depth = 0x4,
        StateDirty_DepthApply = 0x8,
        StateDirty_Rasterizer = 0x10,
        StateDirty_RasterizerApply = 0x20,
        StateDirty_InputLayout = 0x40,
        StateDirty_InputLayoutApply = 0x80,
    };

    /* 27EC */ int32_t m_ownhwnd;
    /* 27F0 */ HWND m_hwnd;
    /* 27F4 */ ATOM m_hwndClass;
    /* 27F8 */ uint32_t m_featureLevel;
    /* 27FC */ ID3D11Device* m_d3dDevice;
    /* 2800 */ IDXGIDevice1* m_dxgiDevice;
    /* 2804 */ IDXGIFactory1* m_dxgiFactory;
    /* 2808 */ IDXGIAdapter1* m_dxgiAdapter;
    /* 280C */ IDXGISwapChain* m_swapChain;
    /* 2810 */ ID3D11DeviceContext* m_d3dContext;
    /* 2814 */ TexTargetViews* m_backBufferViews;
    /* 2818 */ TexTargetViews* m_depthViews;
    /* 281C */ ID3D11Texture2D* m_depthTexture;
    /* 2820 */ ID3D11Texture2D* m_resolveTexture;
    /* 2824 */ ID3D11Buffer* m_constantBuffers [GxShTargets_Last];
    /* 283C */ RenderTargets m_targets;
    /* 2868 */ ID3D11InputLayout* m_curInputLayout;
    /* 286C */ ID3D11Buffer* m_curIndexBuffer;
    /* 2870 */ ID3D11Buffer* m_curVertexBuffers [2];
    /* 2878 */ uint32_t m_vertexStrides [2];
    // NOT IN BINARY: the byte offsets currently bound. A pool holds many CGxBufs, each at its own
    // m_index, so the cached buffer pointer alone is not enough to know whether the binding is current.
    /* ---- */ uint32_t m_curIndexOffset;
    /* ---- */ uint32_t m_curVertexOffsets [2];
    /* 2880 */ uint32_t m_instanceCount;
    /* 2884 */ ID3D11RasterizerState* m_curRasterizerState;
    /* 2888 */ ID3D11BlendState* m_curBlendState;
    /* 288C */ ID3D11DepthStencilState* m_curDepthState;
    /* 2890 */ EmergencyMem m_staticEmergencyMem [2];
    /* 28B8 */ HICON m_cursor;
    /* 28BC */ TSExplicitList<ScratchTex, offsetof(ScratchTex, m_link)> m_scratchTextures [78];
    /* 2C64 */ uint32_t m_scratchCount [GxTexFormats_Last];
    /* 2CC4 */ CGxShader* m_clearVertexShader;
    /* 2CC8 */ CGxShader* m_clearPixelShader;
    /* 2CCC */ CGxShader* m_passThroughVertexShader;
    /* 2CD0 */ CGxShader* m_stretchRectPixelShader;
    /* 2CD4 */ CGxShader* m_resolveVertexShader;
    /* 2CD8 */ CGxShader* m_resolvePixelShader;
    /* 2CDC */ uint32_t m_stateDirty;
    /* 2CE0 */ TSHashTable<InputLayoutRec, InputLayoutKey> m_inputLayouts;
    /* 2D0C */ TSHashTable<RasterizerStateRec, HASHKEY_NONE> m_rasterizerStates;
    /* 2D38 */ TSHashTable<BlendStateRec, HASHKEY_NONE> m_blendStates;
    /* 2D64 */ TSHashTable<DepthStateRec, HASHKEY_NONE> m_depthStates;
    /* 2D90 */ TSHashTable<SamplerStateRec, HASHKEY_NONE> m_samplerStates;

    // NOT IN BINARY: the frame buffer bound through this device. Cata's base exposes BackBuffer(); this
    // client's does not, and ScenePresent only compares against the currently bound one.
    CGxFrameBuffer* m_frameBuffer;
    // NOT IN BINARY: the version of the frame buffer last bound, so FrameBufferSet can skip re-binding an
    // unchanged target list (cata keeps it in its base; this client's base has no frame buffers).
    uint32_t m_frameBufferVersion;

    virtual void ITexMarkAsUpdated(CGxTex* texId);
    virtual void IRsSendToHw(EGxRenderState which);
    virtual void ICursorCreate(const CGxFormat& format);
    virtual void ICursorDestroy();
    virtual ~CGxDeviceD3d11();
    virtual int32_t DeviceCreate(GxWindowProc windowProc, const CGxFormat& format);
    virtual int32_t DeviceCreate(void* window, const CGxFormat& format);
    virtual void DeviceDestroy();
    virtual void DeviceEvictResources();
    virtual int32_t DeviceSetFormat(const CGxFormat& format);
    virtual void DeviceSetBaseMipLevel(uint32_t level);
    virtual void DeviceSetGamma(float gamma);
    virtual void DeviceSetGamma(const CGxGammaRamp& ramp);
    virtual void* DeviceWindow();
    virtual void DeviceReadPixels(CiRect& rect, TSGrowableArray<CImVector>& pixels);
    virtual void DeviceReadDepths(CiRect& rect, TSGrowableArray<float>& depths);
    virtual int32_t DeviceWM(EGxWM wm, uintptr_t param1, uintptr_t param2);
    virtual void DeviceSetRenderTarget(EGxBuffer buffer, CGxTex* texture, uint32_t plane);
    virtual void FrameBufferSet(CGxFrameBuffer* frameBuffer);
    // NOT IN BINARY: cata keeps these on CGxDevice; this client's base has no back-buffer subsystem, and
    // only the D3d11 backend needs one (it wraps the swap chain's views in CGxTex objects so the engine's
    // render-target code can address them).
    void ICreateBackBufferFB(uint32_t width, uint32_t height, void* colorApiData, void* depthApiData);
    CGxTex* m_backBufferColorTex;
    CGxTex* m_backBufferDepthTex;
    virtual void DeviceResolveDepthBuffer(CGxTex* texture);
    virtual void DeviceCopyTex(CGxTex* source, uint32_t sourcePlane, CGxTex* dest, uint32_t destPlane, uint32_t level);
    virtual void DeviceOverride(EGxOverride override, uint32_t value);
    virtual void CapsWindowSize(CRect& rect);
    virtual void CapsWindowSizeInScreenCoords(CRect& rect);
    virtual void ScenePresent();
    // NOT IN BINARY: cata's accessor for the bound frame buffer.
    virtual CGxFrameBuffer* BackBuffer();
    virtual void SceneClear(uint32_t mask, CImVector color);
    virtual void SceneClearToViewport(uint32_t mask, CImVector color);
    virtual int32_t XformIsValidViewport(float minX, float maxX, float minY, float maxY, float minZ, float maxZ);
    virtual void XformSetProjection(const C44Matrix& matrix);
    virtual void XformSetView(const C44Matrix& matrix);
    virtual void PrimVertexPtr(CGxBuf* buf, EGxVertexBufferFormat format);
    virtual void PrimVertexPtrs(CGxBuf** bufs, const EGxVertexBufferFormat* formats, EGxVertexBufferFormat format);
    virtual void DrawNonIndexed(const CGxBatch& batch);
    virtual void Draw(const CGxBatch& batch);
    virtual void Draw(CGxBatch* batch, int32_t count);
    virtual void DrawInstancedNonIndexed(const CGxBatch& batch);
    virtual void DrawInstanced(const CGxBatch& batch);
    virtual void SetInstanceCount(uint32_t count);
    virtual void ResetInstancing();
    virtual void PoolSizeSet(CGxPool* pool, uint32_t size);
    virtual void PoolDestroy(CGxPool* pool);
    virtual char* BufLock(CGxBuf* buf);
    virtual int32_t BufUnlock(CGxBuf* buf, uint32_t size);
    virtual void BufData(CGxBuf* buf, const void* data, uint32_t size, uint32_t offset);
    virtual void TexDestroy(CGxTex* texId);
    virtual int32_t TexCopy(CGxTex* dest, const C2iVector& destPos, const C2iVector& sourcePos, const C2iVector& size, uint32_t level, uint32_t plane);
    virtual int32_t TexStretch(CGxTex* dest, const CiRect* destRect, const CiRect* sourceRect, uint32_t level, uint32_t face);
    virtual int32_t TexResolve(CGxTex* source, CGxTex* dest);
    virtual void QueryCreate(CGxQuery*& query, EGxQueryType type);
    virtual void QueryDestroy(CGxQuery*& query);
    virtual bool QueryBegin(CGxQuery* query);
    virtual bool QueryEnd(CGxQuery* query);
    virtual bool QueryGetParam(CGxQuery* query, EGxQueryParam param, uint32_t& data);
    virtual bool QueryGetData(CGxQuery* query, uint32_t& data);
    virtual void IShaderCreate(CGxShaderPermute* permute);
    virtual void ShaderCreateCompiled(CGxShaderPermute* permute, const void* bytecode, uint32_t bytecodeSize);
    virtual void IShaderDestroy(CGxShaderPermute* permute);
    virtual void CursorSetVisible(int32_t visible);
    virtual void CursorUnlock(uint32_t x, uint32_t y);
    virtual int32_t ISetCursor();
    virtual void StereoSetConvergence(float convergence);
    virtual float StereoGetConvergence();
    virtual void StereoSetSeparation(float separation);
    virtual float StereoGetSeparation();
    virtual bool StereoEnabled();
    virtual void DebugGroupBegin(const char* name, char* buffer);
    virtual void DebugGroupEnd();
    virtual void DebugEvent(const char* name, char* buffer);
    virtual void ICheckResult(HRESULT result);
    virtual void DeviceUnk81(uint32_t a, uint32_t b);
    virtual void DeviceUnk82();
    virtual void DeviceUnk83(uint32_t a, uint32_t b);

    static const DXGI_FORMAT s_GxFormatToDxgiFormat[CGxFormat::Formats_Last];
    static const DXGI_FORMAT s_GxFormatToDxgiTypeless[CGxFormat::Formats_Last];
    static const D3D11_QUERY s_queryTypes[1];
    static const uint32_t s_maxIndex[6];
    static const uint32_t s_texMaxSize2d[6];
    static const uint32_t s_texMaxSizeCube[6];
    static const uint32_t s_texMaxSizeVolume[6];
    static const uint32_t s_maxAnisotropy[6];
    static const uint32_t s_numRenderTargets[6];
    static const DXGI_FORMAT s_gxAttribToDxgiFormat[8];
    static const uint32_t s_gxAttribToSemanticIndex[GxVAs_Last];
    static const char* s_gxAttribToSemanticName[GxVAs_Last];
    static const EGxTexFormat s_GxTexFormatToUse[GxTexFormats_Last];
    static const DXGI_FORMAT s_GxTexFormatToViewFormat[GxTexFormats_Last];
    static const DXGI_FORMAT s_GxTexFormatToDxgiFormat[GxTexFormats_Last];
    static const uint32_t s_texFormatResolvable[GxTexFormats_Last];
    static const DXGI_FORMAT s_GxTexFormatToSrvFormat[GxTexFormats_Last];
    static const D3D11_BLEND s_srcBlend[13];
    static const D3D11_BLEND s_srcBlendAlpha[13];
    static const D3D11_BLEND s_dstBlend[13];
    static const D3D11_BLEND s_dstBlendAlpha[13];
    static const D3D11_COMPARISON_FUNC s_cmpFunc[4];
    static const D3D11_FILTER s_filterModes[GxTexFilters_Last];
    static const D3D11_FILTER s_compareFilterModes[GxTexFilters_Last];
    static const D3D11_TEXTURE_ADDRESS_MODE s_wrapModes[3];
    static const D3D11_PRIMITIVE_TOPOLOGY s_primitiveTopology[5];
    static const D3D11_PRIMITIVE_TOPOLOGY s_patchTopology[5];

    static int32_t IHardwareSupported();
    static int32_t ILoadD3dLib();
    static void IUnloadD3dLib();
    static IDXGIOutput* IGetOutputForMonitor(IDXGIAdapter* adapter, uint32_t monitor);
    static void ITexCalcOffset(EGxTexFormat format, uint32_t pitch, uint32_t height, const RECT& rect, uint32_t& offset, uint32_t& size);
    static void ITexRectToBox(EGxTexFormat format, const RECT& rect, D3D11_BOX& box);
    void ITexReleaseD3dTexture(CGxTex* texId);
    // Volume-aware sibling of CGxDevice::ITexWHDStartEnd: cata's takes depth too, this client's base
    // does not (it has no volume targets). Only the baseMip/width/height logic is shared; this adds the
    // depth and keeps the same mip-count rules.
    void ITexWHD(CGxTex* texId, uint32_t& width, uint32_t& height, uint32_t& depth, uint32_t& baseMip, uint32_t& mipCount);
    void IResourceTouch(const void* resource);

    CGxDeviceD3d11();
    void ISetCaps(const CGxFormat& format);
    uint32_t IFeatureLevelIndex(D3D_FEATURE_LEVEL featureLevel);
    int32_t ICreateTargets(const C2iVector& size);
    void IReleaseTargets();
    void ISetWindowFocus(int32_t focus);
    int32_t CreatePoolAPI(CGxPool* pool);
    char* IBufLock(CGxBuf* buf);
    void IBufUnlock(CGxBuf* buf, uint32_t size);
    void IQueryCreateApi(CGxQuery* query);
    int32_t ICreateD3dDevice(const CGxFormat& format);
    void IReleaseD3dPools(int32_t freePools);
    void IReleaseD3dQueries();
    void IReleaseD3dShaders(int32_t freeShaders);
    void IDestroyD3d();
    int32_t ITexCreate(CGxTex* texId);
    void ITexScratchClear();
    void ITexScratchRelease(ScratchTex* scratch);
    void ITexScratchAlloc(EGxTexFormat format, uint32_t count);
    ScratchTex* ITexScratchGet(uint32_t frame, uint32_t size, EGxTexFormat format, int32_t& reused);
    void ITexScratchDestroy();
    void ITexUploadRect(uint32_t size, CGxTex* texId, const void* bits, uint32_t pitch, uint32_t height, const RECT& rect, uint32_t plane, uint32_t subresource);
    void ITexUpload(CGxTex* texId);
    void ITexForceRecreation(int32_t all);
    void IStateReset();
    void ISetIndexBuffer();
    void ISetVertexBuffer();
    void ISetVertexBuffers();
    void IStateSyncScissorRect();
    void IUpdateInputLayout();
    void ISetInputLayout();
    void ISetInputLayoutInstanced();
    void IStateSyncRasterizer();
    void IStateSyncBlend();
    void IStateSyncDepth();
    ID3D11SamplerState* IGetSamplerState(CGxTexFlags flags);
    void ISetVertexTexture(uint32_t slot, CGxTex* texId);
    void ISetDomainTexture(uint32_t slot, CGxTex* texId);
    void ISetTexture(uint32_t slot, CGxTex* texId);
    void IStateSync();
    void IXformSetViewport();
    void IBindVertexShader(CGxShaderPermute* permute);
    void IBindHullShader(CGxShaderPermute* permute);
    void IBindDomainShader(CGxShaderPermute* permute);
    void IBindGeometryShader(CGxShaderPermute* permute);
    void IBindPixelShader(CGxShaderPermute* permute);
    void IShaderConstantsFlush();
};

CGxDevice* CreateCGxDeviceD3d11();

#endif
