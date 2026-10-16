#ifndef ENGINE_SOURCE_GX_CGXDEVICE_CGXDEVICE_H
#define ENGINE_SOURCE_GX_CGXDEVICE_CGXDEVICE_H

#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include "Engine/Source/Gx/CGxBuf.h"
#include "Engine/Source/Gx/CGxDevice/CGxShader.h"
#include "Engine/Source/Gx/CGxFrameBuffer.h"
#include "Engine/Source/Gx/CGxDevice/EmergencyMem.h"
#include "Engine/Source/Gx/CGxDevice/Types.h"
#include "Engine/Source/Gx/CGxFormat.h"
#include "Engine/Source/Gx/CGxGammaRamp.h"
#include "Engine/Source/Gx/CGxLight.h"
#include "Engine/Source/Gx/CGxMatrixStack.h"
#include "Engine/Source/Gx/CGxTex.h"
#include "Engine/Source/Gx/Types.h"
#include "Engine/Source/Storm/H/stpl.h"
#include "Engine/Source/Storm/H/Types.h"
#include "Engine/Source/Tempest/C2iVector.h"
#include "Engine/Source/Tempest/C2Vector.h"
#include "Engine/Source/Tempest/C3Vector.h"
#include "Engine/Source/Tempest/C44Matrix.h"
#include "Engine/Source/Tempest/C4Plane.h"
#include "Engine/Source/Tempest/C4Vector.h"
#include "Engine/Source/Tempest/CBoundingBox.h"
#include "Engine/Source/Tempest/CImVector.h"
#include "Engine/Source/Tempest/CiRect.h"
#include "Engine/Source/Tempest/CRect.h"

struct CGxAppRenderState;
struct CGxBuf;
struct CGxPool;
struct tagRECT;
class CGxQuery;
class CGxStateBom;
class CGxTex;
class CImVector;

class ShadowConstants {
    public:
    /* 0000 */ float constants[256 * 4];
    /* 1000 */ uint32_t dirtyMax;
    /* 1004 */ uint32_t dirtyMin;
};

class CGxStateBom {
    public:
    /* 0000 */ union {
        int32_t i[3];
        uint32_t u[3];
        float f[3];
        void* p;
    } m_data;
    /* 000C */ int32_t m_filler;
};

struct CGxPushedRenderState {
    /* 0000 */ EGxRenderState m_which;
    /* 0004 */ CGxStateBom m_value;
    /* 0014 */ uint32_t m_stackDepth;
};

struct CGxAppRenderState {
    /* 0000 */ CGxStateBom m_value;
    /* 0010 */ uint32_t m_stackDepth;
    /* 0014 */ int32_t m_dirty;
};

typedef void (__fastcall* GxDeviceCallback)();

class CGxDevice {
    public:
    struct TextureTarget {
        /* 0000 */ CGxTex* m_texture;
        /* 0004 */ uint32_t m_plane;
        /* 0008 */ void* m_apiSpecific;
    };

    /* 0004 */ TSGrowableArray<CGxPushedRenderState> m_pushedStates;
    /* 0014 */ TSGrowableArray<uint32_t> m_stackOffsets;
    /* 0024 */ TSGrowableArray<EGxRenderState> m_dirtyStates;
    /* 0034 */ EGxPrim m_primType;
    /* 0038 */ int32_t m_indexLocked;
    /* 003C */ int32_t m_vertexLocked;
    /* 0040 */ int32_t m_inBeginEnd;
    /* 0044 */ C3Vector m_primVertex;
    /* 0050 */ C2Vector m_primTexCoord [8];
    /* 0090 */ C3Vector m_primNormal;
    /* 009C */ CImVector m_primColor;
    /* 00A0 */ TSGrowableArray<C3Vector> m_primVertexArray;
    /* 00B0 */ TSGrowableArray<C2Vector> m_primTexCoordArray [8];
    /* 0130 */ TSGrowableArray<C3Vector> m_primNormalArray;
    /* 0140 */ TSGrowableArray<CImVector> m_primColorArray;
    /* 0150 */ TSGrowableArray<uint16_t> m_primIndexArray;
    /* 0160 */ uint32_t m_primMask;
    /* 0164 */ CRect m_defWindowRect;
    /* 0174 */ CRect m_curWindowRect;
    /* 0184 */ TSGrowableArray<GxDeviceCallback> m_deviceRestoredCallbacks;
    /* 0194 */ TSGrowableArray<GxDeviceCallback> m_textureRecreationCallbacks;
    /* 01A4 */ TSGrowableArray<GxDeviceCallback> m_stereoChangedCallbacks;
    /* 01B4 */ EGxApi m_api;
    /* 01B8 */ uint32_t m_cpuFeatures;
    /* 01BC */ CGxFormat m_format;
    /* 0214 */ CGxCaps m_caps;
    // Every offset from here down to m_cursorDepth is the 12340 value + 0x1A8: HEAD's +0x40 from the
    // DepthCache depth formats, +0x160 from the 15595 shader/volume caps and the m_texTarget/m_texMaxSize
    // growth to GxTexTargets_Last, and +0x8 from unk_0300/unk_0314 (see CGxCaps, Types.h). Note these are the
    // 32-bit (Win32) frame, matching the original binary; the x64 layout carries a larger delta because the
    // TS containers grow, which is why the offsets are guarded by build rather than asserted absolutely.
    /* 04F8 */ uint32_t m_baseMipLevel;
    /* 04FC */ CGxGammaRamp m_gammaRamp;
    /* 0AFC */ CGxGammaRamp m_systemGammaRamp;
    /* 10FC */ GxWindowProc m_windowProc;
    /* 1100 */ int32_t m_context;
    /* 1104 */ int32_t m_intF5C;
    /* 1108 */ int32_t m_windowVisible;
    /* 110C */ int32_t m_windowFocus;
    /* 1110 */ int32_t m_frameCount;
    /* 1114 */ int32_t m_viewportDirty;
    /* 1118 */ CBoundingBox m_viewport;
    /* 1130 */ C44Matrix m_projection;
    /* 1170 */ C44Matrix m_projNative;
    /* 11B0 */ CGxMatrixStack m_xforms [11];
    /* 1DB8 */ CGxMatrixStack m_texGen [8];
    /* 2678 */ uint32_t m_clipPlaneMask;
    /* 267C */ C4Plane m_clipPlane [6];
    /* 26DC */ int32_t m_scissorRectDirty;
    /* 26E0 */ CRect m_scissorRect;
    /* 26F0 */ CGxApiLight m_lights [4];
    /* 2810 */ TSHashTable<CGxShader, HASHKEY_STRI> m_shaderList [6];
    /* 2900 */ uint32_t m_appMasterEnables;
    /* 2904 */ uint32_t m_hwMasterEnables;
    /* 2908 */ TSList<CGxPool> m_poolList;
    /* 2914 */ TSList<CGxBuf> m_bufList;
    /* 2920 */ CGxBuf* m_bufLocked [2];
    /* 2928 */ CGxPool* m_vertexPool;
    /* 292C */ CGxPool* m_indexPool;
    /* 2930 */ CGxBuf* m_streamBufs [2];
    /* 2938 */ CGxVertexAttrib m_primVertexFormatAttrib [14];
    /* 2A18 */ CGxBuf* m_primVertexFormatBuf [14];
    /* 2A50 */ uint32_t m_primVertexMask;
    /* 2A54 */ uint32_t m_primVertexDirty;
    /* 2A58 */ EGxVertexBufferFormat m_primVertexFormat;
    /* 2A5C */ CGxBuf* m_primVertexBuf;
    /* 2A60 */ uint32_t m_primVertexSize;
    /* 2A64 */ CGxBuf* m_primIndexBuf;
    /* 2A68 */ int32_t m_primIndexDirty;
    /* 2A6C */ EmergencyMem m_emergencyMem [2];
    /* 2A94 */ TSFixedArray<CGxAppRenderState> m_appRenderStates;
    /* 2AA0 */ TSFixedArray<CGxStateBom> m_hwRenderStates;
    /* 2AAC */ TSExplicitList<CGxTex, offsetof(CGxTex, m_link)> m_textures;
    /* 2AB8 */ CGxDevice::TextureTarget m_textureTarget [2];
    // Not in the original binary. Ported from 15595: the whole attachment set as one object, so a pass
    // can save and restore it. m_textureTarget above is still the live device state.
    /* 2AD0 */ CGxTex* m_backBufferColorTex;
    /* 2AD4 */ CGxTex* m_backBufferDepthTex;
    /* 2AD8 */ CGxFrameBuffer* m_backBuffer;
    /* 2ADC */ CGxFrameBuffer* m_frameBuffer;
    /* 2AE0 */ uint32_t m_frameBufferVersion;
    /* 2AE4 */ TSExplicitList<CGxQuery, offsetof(CGxQuery, m_link)> m_queryList;
    /* 2AF0 */ int32_t m_scrShotClick;
    /* 2AF4 */ uint32_t m_scrShotWidth;
    /* 2AF8 */ uint32_t m_scrShotHeight;
    /* 2AFC */ TSGrowableArray<CImVector> m_scrShotPixels;
    /* 2B0C */ int32_t m_cursorVisible;
    /* 2B10 */ int32_t m_hwCursor;
    /* 2B14 */ uint32_t m_cursorHotspotX;
    /* 2B18 */ uint32_t m_cursorHotspotY;
    /* 2B1C */ uint32_t m_cursor [1024];
    /* 3B1C */ CGxTex* m_cursorTexture;
    /* 3B20 */ float m_cursorDepth;

    virtual void ITexMarkAsUpdated(CGxTex* texId);
    virtual void IRsSendToHw(EGxRenderState which) = 0;
    virtual void ICursorCreate(const CGxFormat& format);
    virtual void ICursorDestroy();
    virtual void ICursorDraw();
    virtual void NotifyOnDeviceRestored();
    virtual void NotifyOnTextureRecreation();
    virtual void NotifyOnStereoChanged();
    virtual ~CGxDevice();
    virtual int32_t DeviceCreate(GxWindowProc windowProc, const CGxFormat& format);
    virtual int32_t DeviceCreate(void* window, const CGxFormat& format);
    virtual void DeviceDestroy();
    virtual void DeviceEvictResources();
    virtual int32_t DeviceSetFormat(const CGxFormat& format);
    virtual void DeviceSetBaseMipLevel(uint32_t level);
    virtual void DeviceSetGamma(float gamma);
    virtual void DeviceSetGamma(const CGxGammaRamp& ramp);
    virtual void* DeviceWindow() = 0;
    virtual void DeviceTakeScreenShot();
    virtual void DeviceReadScreenShot(uint32_t& width, uint32_t& height, const CImVector*& pixels);
    virtual void DeviceReadPixels(CiRect& rect, TSGrowableArray<CImVector>& pixels) = 0;
    virtual void DeviceReadDepths(CiRect& rect, TSGrowableArray<float>& depths) = 0;
    virtual int32_t DeviceWM(EGxWM wm, uintptr_t param1, uintptr_t param2) = 0;
    virtual void DeviceSetRenderTarget(EGxBuffer buffer, CGxTex* texture, uint32_t plane);
    virtual void DeviceSaveDepthSurface();
    virtual void DeviceRestoreDepthSurface();
    virtual void DeviceResolveDepthBuffer(CGxTex* texture);
    virtual void DeviceCopyTex(CGxTex* source, uint32_t sourcePlane, CGxTex* dest, uint32_t destPlane);
    virtual void DeviceOverride(EGxOverride override, uint32_t value);
    virtual void AddDeviceRestoredCallback(GxDeviceCallback callback);
    virtual void RemoveDeviceRestoredCallback(GxDeviceCallback callback);
    virtual void AddTextureRecreationCallback(GxDeviceCallback callback);
    virtual void RemoveTextureRecreationCallback(GxDeviceCallback callback);
    virtual void AddStereoChangedCallback(GxDeviceCallback callback);
    virtual void RemoveStereoChangedCallback(GxDeviceCallback callback);
    virtual void CapsWindowSize(CRect& rect) = 0;
    virtual void CapsWindowSizeInScreenCoords(CRect& rect) = 0;
    virtual void LogCrashInfo(char* buffer, uint32_t size);
    virtual void ScenePresent();
    virtual void SceneClear(uint32_t mask, CImVector color);
    virtual void XformSetProjection(const C44Matrix& matrix);
    virtual void XformSetView(const C44Matrix& matrix);
    virtual void Draw(CGxBatch* batch, int32_t count);
    virtual void PrimBegin(EGxPrim primType);
    virtual void PrimDrawElements();
    virtual void PrimVertex(const C3Vector& v);
    virtual void PrimTexCoord(uint32_t tmu, const C2Vector& t);
    virtual void PrimNormal(const C3Vector& n);
    virtual void PrimColor(const CImVector& c);
    virtual void PrimPointSize(float size);
    virtual void PrimLineWidth(float width);
    virtual void MasterEnableSet(EGxMasterEnables state, int32_t enable);
    virtual void PoolSizeSet(CGxPool* pool, uint32_t size) = 0;
    virtual void PoolDestroy(CGxPool* pool);
    virtual char* BufLock(CGxBuf* buf);
    virtual int32_t BufUnlock(CGxBuf* buf, uint32_t size);
    virtual void BufData(CGxBuf* buf, const void* data, uint32_t size, uint32_t offset);
    virtual int32_t TexCreate(EGxTexTarget target, uint32_t width, uint32_t height, uint32_t depth, EGxTexFormat format, EGxTexFormat dataFormat, CGxTexFlags flags, void* userArg, GxTexUserFunc userFunc, const char* name, CGxTex*& texId);
    virtual void TexDestroy(CGxTex* texId);
    virtual int32_t TexCopy(CGxTex* dest, const C2iVector& destPos, const C2iVector& sourcePos, const C2iVector& size, uint32_t level, uint32_t plane) = 0;
    virtual int32_t TexStretch(CGxTex* dest, const CiRect* destRect, const CiRect* sourceRect, uint32_t level, uint32_t face) = 0;
    virtual void TexSetCacheSize(uint32_t size);
    virtual void QueryCreate(CGxQuery*& query, EGxQueryType type);
    virtual void QueryDestroy(CGxQuery*& query);
    virtual bool QueryBegin(CGxQuery* query) = 0;
    virtual bool QueryEnd(CGxQuery* query) = 0;
    virtual bool QueryGetParam(CGxQuery* query, EGxQueryParam param, uint32_t& data) = 0;
    virtual bool QueryGetData(CGxQuery* query, uint32_t& data) = 0;
    virtual void ShaderCreate(CGxShader** shaders, EGxShTarget target, const char* path, const char* name, uint32_t permutations);
    virtual void ShaderDestroy(CGxShader*& shader);
    virtual void ShaderConstantsSet(EGxShTarget target, uint32_t index, const float* constants, uint32_t count);
    virtual void IShaderReload(CGxShader* shader, const char* path, const char* name) = 0;
    virtual void IShaderCreate(CGxShader* shader) = 0;
    virtual void CursorSetVisible(int32_t visible);
    virtual uint32_t* CursorLock();
    virtual void CursorUnlock(uint32_t x, uint32_t y);
    virtual void StereoSetConvergence(float convergence) = 0;
    virtual float StereoGetConvergence() = 0;
    virtual void StereoSetSeparation(float separation) = 0;
    virtual float StereoGetSeparation() = 0;
    virtual bool StereoEnabled() = 0;
    virtual void DeviceUnk81(uint32_t a, uint32_t b) = 0;
    virtual void DeviceUnk82() = 0;
    virtual void DeviceUnk83(uint32_t a, uint32_t b) = 0;
    // Not in the original binary. Frame-buffer wrappers, ported from 15595. Appended after the last
    // 12340 virtual so no existing vtable slot moves. 12340 has two buffers (color, depth) rather than
    // 15595's five, so CGxFrameBuffer holds one target per EGxBuffer instead of a fixed array.
    virtual CGxFrameBuffer* FrameBufferCreate();
    virtual void FrameBufferDelete(CGxFrameBuffer*& frameBuffer);
    virtual int32_t FrameBufferAttach(CGxFrameBuffer* frameBuffer, EGxBuffer buffer, CGxTex* texture, uint32_t plane);
    virtual int32_t FrameBufferFinalize(CGxFrameBuffer* frameBuffer);
    virtual CGxFrameBuffer* FrameBufferGet();
    virtual void FrameBufferSet(CGxFrameBuffer* frameBuffer);
    virtual CGxFrameBuffer* BackBuffer();
    virtual CGxTex* BackBufferColorTex();
    virtual CGxTex* BackBufferDepthTex();
    // Not in the original binary. Ported from 15595, which needs it for DepthCache::Resolve. 15595 only
    // overrides it in the D3D11 backend; the D3D9 backend leaves the base returning 0 and resolves depth via
    // DeviceResolveDepthBuffer instead. Appended at the END of the virtual list so no 12340 vtable slot moves -
    // inserting it next to TexStretch instead shifted CursorSetVisible to IShaderCreate and crashed the client.
    virtual int32_t TexResolve(CGxTex* dest, CGxTex* source);

    static HSLOG m_log;
    static C2iVector s_clientAdjust;
    static const char* s_pixelShaderTargetNames[14];
    static const char* s_vertexShaderTargetNames[12];
    // Not in the original binary. Ported from 15595 shape: the shader-model / shader-profile name tables
    // s_pixelShaderTargetNames/s_vertexShaderTargetNames above index 12340's per-target profile numbering,
    // while these index the 15595 EGxShaderProfile order that ChooseShaderModel and m_shaderProfiles use.
    // The two numberings differ on purpose; see the note on ChooseShaderModel.
    static const char* s_shaderModelNames[8];
    static const char* s_shaderProfileNames[GxShProfiles_Last];
    // Not in the original binary. Ported from 15595: 1 where the format is a depth/stencil format, so an
    // ISetCaps RTT loop can pick D3DUSAGE_DEPTHSTENCIL over D3DUSAGE_RENDERTARGET. Appended with the other
    // s_texFormat* tables rather than at 15595's offset, because it joins the 12340 EGxTexFormat ordering.
    // Not referenced yet: 12340's ISetCaps still lists its m_rttFormat checks individually.
    static const uint32_t s_texFormatIsDepth[GxTexFormats_Last];
    static C3Vector s_pointScaleAttenuation;
    static CGxShader* s_uiPixelShader;
    static CGxShader* s_uiVertexShader[2];
    static ShadowConstants s_shadowConstants[2];
    static const char* s_texFormatNames[GxTexFormats_Last];
    static uint32_t s_alphaRef[12];
    static const uint32_t s_texFormatBitDepth[GxTexFormats_Last];
    static const uint32_t s_texFormatBlockSize[GxTexFormats_Last];
    static const uint32_t s_texFormatBytesPerBlock[GxTexFormats_Last];
    static uint32_t m_logBytes;

    static int32_t AdapterDesktopMode(CGxMonitorMode& mode);
    static int32_t AdapterFormats(EGxApi api, TSGrowableArray<CGxFormat>& formats);
    static int32_t AdapterMonitorModes(TSGrowableArray<CGxMonitorMode>& modes);
    static int32_t AdjustWindowRectToWorkArea(float aspectRatio, C2iVector* adjust, tagRECT* rect);
    static int32_t D3d9ExAdapterFormats(TSGrowableArray<CGxFormat>& formats);
    static int32_t D3dAdapterFormats(TSGrowableArray<CGxFormat>& formats);
    static int32_t DeviceAdapterID(uint16_t& vendorID, uint16_t& deviceID, uint32_t& driverVersionHi, uint32_t& driverVersionLo);
    static int32_t DeviceAdapterInfer(uint16_t& deviceID);
    static int32_t FormatExists(TSGrowableArray<CGxFormat>& formats, const CGxFormat& format);
    static int32_t OpenGlAdapterFormats(TSGrowableArray<CGxFormat>& formats);
    static void ClampRectBottom(CiRect& windowRect);
    static void ClampRectBottomByAspect(CiRect& windowRect);
    static void ClampRectLeftByAspect(CiRect& windowRect);
    static void ClampRectRightByAspect(CiRect& windowRect);
    static void ClampRectTopByAspect(CiRect& windowRect);
    static void Log(const char* format, ...);
    static void Log(const char* format, va_list args);
    static void LogClose();
    static void LogOpen();
    static void ClampRectLeft(CiRect& windowRect);
    static void ClampRectRight(CiRect& windowRect);
    static void ClampRectTop(CiRect& windowRect);
    static void ICursorUpdate(EGxTexCommand command, uint32_t width, uint32_t height, uint32_t face, uint32_t level, void* userArg, uint32_t& texelStrideInBytes, const void*& texels);

    CGxDevice();
    CGxBuf* BufCreate(CGxPool* pool, uint32_t itemSize, uint32_t itemCount, uint32_t index);
    void BufDestroy(CGxBuf*& buf);
    void DeviceCreatePools();
    void DeviceCreateStreamBufs();
    void IShaderLoad(CGxShader** shaders, EGxShTarget target, const char* path, const char* name, uint32_t permutations);
    CGxPool* PoolCreate(EGxPoolTarget target, EGxPoolUsage usage, int32_t size, EGxPoolHintBits hint, const char* name);
    CGxBuf* BufStream(EGxPoolTarget target, uint32_t itemSize, uint32_t itemCount);
    void BufSizeSet(CGxBuf* buf, uint32_t itemSize, uint32_t itemCount);
    CGxCaps& Caps();
    int32_t CapsHasContext(int32_t context);
    int32_t CapsIsWindowVisible(int32_t visible);
    int32_t CapsWindowHasFocus(int32_t focus);
    void ClampRectToWindow(CiRect& rect);
    void Log(const CGxCaps& caps) const;
    void Log(const CGxFormat& format) const;
    void IRsDirty(EGxRenderState which);
    void IRsForceUpdate();
    // Not in the original binary. Ported from 15595. Picks the highest shader model whose whole profile set
    // the device reports in m_caps.m_shaderProfiles, then writes the winning profiles into
    // m_caps.m_shaderTargets. NOTE: m_shaderTargets is written in 15595's EGxShaderProfile numbering, which
    // is NOT the numbering 12340's ShaderCreate/IShaderLoad uses to build the .bls path (that one indexes
    // s_pixelShaderTargetNames/s_vertexShaderTargetNames). Keep the two separate or every shader load moves.
    void ChooseShaderModel(int32_t shaderModel);
    void IRsForceUpdate(EGxRenderState which);
    void IRsInit();
    void IRsSync(int32_t force);
    void RsPop();
    void RsInit();
    void RsPush();
    void RsSet(EGxRenderState which, void* value);
    void RsSet(EGxRenderState which, int32_t value);
    void RsSet(EGxRenderState which, float value);
    void RsSet(EGxRenderState which, const C3Vector& value);
    void TexSetFlags(CGxTex* texId, CGxTexFlags flags);
    void TexSetWrap(CGxTex* texId, EGxTexWrapMode wrapU, EGxTexWrapMode wrapV);
    void ClipPlaneSet(uint32_t index, const C4Plane& plane);
    void CursorSetDepth(float depth);
    EGxApi DeviceApi();
    uint32_t DeviceBaseMipLevel();
    void DeviceClearScreenShot();
    const CRect& DeviceCurWindow();
    const CRect& DeviceDefWindow();
    // Not in the original binary. Ported from 15595; DepthCache::IsSupported and CreateTextures read the active
    // format's depth format and multisample count to pick a sample-able depth texture format.
    const CGxFormat& DeviceFormat();
    void DeviceGamma(CGxGammaRamp& ramp);
    void DeviceGetRenderTarget(EGxBuffer buffer, CGxTex*& texture);
    void DeviceScreenShot();
    void DeviceSetCurWindow(const CRect& rect);
    void DeviceSetDefWindow(const CRect& rect);
    void GetSurveyModeInfo(uint32_t& desktopWidth, uint32_t& desktopHeight, uint32_t& desktopBpp, uint32_t& width, uint32_t& height, uint32_t& bpp);
    int32_t IDevIsWindowed();
    void InitLights();
    uint32_t ITexBitsSize(CGxTex* texId, uint32_t width, uint32_t height);
    void ITexWHDStartEnd(CGxTex* texId, uint32_t& width, uint32_t& height, uint32_t& baseMip, uint32_t& mipCount);
    void Light(uint32_t index, CGxLight& light);
    void LightEnable(uint32_t index, int32_t enable);
    void LightSet(uint32_t index, const CGxLight& light, const C3Vector& pos);
    int32_t MasterEnable(EGxMasterEnables state);
    int32_t NeedsUpdate(uint32_t app, uint32_t hw, uint32_t appDisables, uint32_t hwDisables, uint32_t flagPos, int32_t& enable);
    uint32_t PrimCalcCount(EGxPrim primType, uint32_t indexCount);
    void PrimIndexPtr(CGxBuf* buf);
    void PrimVertexFormat(CGxBuf* buf, const CGxVertexAttrib* attribs, uint32_t count);
    void PrimVertexMask(uint32_t mask);
    void PrimVertexPtr(CGxBuf* buf, EGxVertexBufferFormat format);
    void SetScissor(const CRect& rect);
    void ShaderConstantsClear();
    float* ShaderConstantsLock(EGxShTarget target);
    void ShaderConstantsUnlock(EGxShTarget target, uint32_t index, uint32_t count);
    int32_t TexCanUpdate(CGxTex* texId);
    int32_t TexIsReadyNow(CGxTex* texId);
    void TexMarkForUpdate(CGxTex* texId, const CiRect& updateRect, int32_t immediate);
    void TexParameters(const CGxTex* texId, CGxTexParms& parms);
    void TexSetCannotUpdate(CGxTex* texId);
    void TexSetDataFormat(CGxTex* texId, EGxTexFormat dataFormat);
    void TexSetUserData(CGxTex* texId, GxTexUserFunc userFunc, void* userArg);
    void WaitForFPSCap();
    void XformPush(EGxXform xf);
    void XformPushAndSet(EGxXform xf, const C44Matrix& matrix);
    void XformSetViewport(float minX, float maxX, float minY, float maxY, float minZ, float maxZ);
};

// Load-bearing: CGxCaps growth moves every CGxDevice member below m_caps, and a silent drift is what
// produced the m_d3dVertexDecl array overrun that crashed the client on launch. Fail the build, not the run.
//
// The /* 0XXX */ comments above stay in the 32-bit (Win32) frame so they stay diffable against the original
// binary, but the asserts cover the build actually being compiled: x64 is the shipped target and its offsets
// differ because the TS containers grow. Measured, not derived - re-measure with the probe template if the
// assert fires. sizeof(CGxCaps) is arch-independent (int32/float arrays only) and guards both.
static_assert(sizeof(CGxCaps) == 0x2E4, "CGxCaps size changed - re-anchor CGxDevice.h and every caps-indexed table");
#if defined(WOWDECOMP_ARCH_64)
static_assert(offsetof(CGxDevice, m_caps) == 0x02B0, "m_caps moved - CGxFormat or a member above it changed size");
static_assert(offsetof(CGxDevice, m_baseMipLevel) == 0x0594, "CGxDevice layout changed - re-anchor the offsets in CGxDevice.h");
static_assert(offsetof(CGxDevice, m_cursorDepth) == 0x3D70, "CGxDevice layout changed - re-anchor the offsets in CGxDevice.h");
#else
static_assert(offsetof(CGxDevice, m_caps) == 0x0214, "m_caps moved - CGxFormat or a member above it changed size");
static_assert(offsetof(CGxDevice, m_baseMipLevel) == 0x04F8, "CGxDevice layout changed - re-anchor the offsets in CGxDevice.h");
static_assert(offsetof(CGxDevice, m_cursorDepth) == 0x3B20, "CGxDevice layout changed - re-anchor the offsets in CGxDevice.h");
#endif

extern CGxDevice* g_theGxDevicePtr;
extern const char* s_formatToInt[CGxFormat::Formats_Last];

#endif
