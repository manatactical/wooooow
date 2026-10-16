#ifndef WOW_SOURCE_LIQUIDSYSTEM_LIQUID_H
#define WOW_SOURCE_LIQUIDSYSTEM_LIQUID_H

#include <cstddef>
#include <cstdint>
#include "Engine/Source/Base/CDataAllocator.h"
#include "Engine/Source/Gx/Types.h"
#include "Engine/Source/Storm/H/stpl.h"
#include "Engine/Source/Tempest/C2Vector.h"
#include "Engine/Source/Tempest/C3Vector.h"
#include "Engine/Source/Tempest/C44Matrix.h"
#include "Engine/Source/Tempest/C2iVector.h"
#include "Engine/Source/Tempest/C4Vector.h"
#include "Engine/Source/Tempest/CAaSphere.h"
#include "Engine/Source/Tempest/CImVector.h"
#include "WoW/Source/LiquidSystem/Types.h"

template <class T>
class IDatabase;
class CGxShader;
class CGxTex;
class CGxFrameBuffer;
class CM2Lighting;
class CTexture;
class LiquidMaterialRec;
class LiquidTypeRec;

namespace Liquid {
    class CInstance;
    class CMaterialSettings;
    class IWaveManager;
    class IEnvironment;
    class IGeomFactory;

    // Not in the original binary. Ported from 15595 (Cataclysm) - the displacement-map and reflection types the
    // new water system needs. See the D.1 block further down for the extent of what was ported.
    enum ERenderPass {
        RenderPass_Opaque = 0,
        RenderPass_Transparent = 1,
    };

    enum EPertSurface {
        PertSurface_Water = 0,
        PertSurface_Ocean = 1,
    };

    struct DisplMapPerturbation {
        /* 0000 */ C3Vector m_pos;
        /* 000C */ float m_radius;
        /* 0010 */ float m_amplitude;
        /* 0014 */ uint32_t m_period;
        /* 0018 */ uint32_t unk_0018;
    };

    struct DisplMapPerturbationMesh {
        /* 0000 */ C44Matrix m_transform;
        /* 0040 */ TSGrowableArray<C3Vector> m_vertices;
        /* 0050 */ TSGrowableArray<uint16_t> m_indices;
        /* 0060 */ float m_amplitude;
        /* 0064 */ uint32_t m_period;
        /* 0068 */ uint32_t unk_0068;

        DisplMapPerturbationMesh();
    };

    struct VSLight {
        /* 0000 */ C4Vector m_position;
        /* 0010 */ C4Vector m_color;
        /* 0020 */ C4Vector m_attenuation;
    };

    struct VSConstants {
        /* 0000 */ C44Matrix m_projection;
        /* 0040 */ C4Vector m_fog;
        /* 0050 */ C44Matrix m_modelView;
        /* 0090 */ C44Matrix m_texTransforms[6];
        /* 0210 */ C4Vector m_sunDir;
        /* 0220 */ C4Vector m_sunAmbient;
        /* 0230 */ C4Vector m_sunDiffuse;
        /* 0240 */ C4Vector m_sunSpecular;
        /* 0250 */ VSLight m_lights[3];
    };

    struct VSWaveConstants {
        /* 0000 */ float m_directionalPhase[4];
        /* 0010 */ float m_circularPhase[4];
        /* 0020 */ C4Vector m_directionalWaves[3];
        /* 0050 */ float unk_0050[4];
        /* 0060 */ float unk_0060[4];
        /* 0070 */ C4Vector m_circularWaves[3];
        /* 00A0 */ float unk_00A0[4];
        /* 00B0 */ float unk_00B0[4];
    };

    struct PSConstants {
        /* 0000 */ C44Matrix m_modelViewProj;
        /* 0040 */ C4Vector m_fogColor;
        /* 0050 */ C4Vector m_cameraPos;
    };

    struct PSProcWaterConstants {
        /* 0000 */ C4Vector m_sunDir;
        /* 0010 */ C4Vector m_sunAmbient;
        /* 0020 */ C4Vector m_sunDiffuse;
        /* 0030 */ C4Vector m_sunSpecular;
        /* 0040 */ float m_params[8];
    };

    class IEnvironment {
        public:
        virtual ~IEnvironment();
        virtual void Release() = 0;
        virtual void SetInteriorFog(int32_t interiorFog) = 0;
        virtual void SelectLights(CM2Lighting* lighting) = 0;
    };

    class IWaveManager {
        public:
        virtual ~IWaveManager();
        virtual void AddRef() = 0;
        virtual void Release() = 0;
        virtual void* GetData() = 0;
        virtual int32_t GetCount() = 0;
    };

    class IGeomFactory {
        public:
        virtual ~IGeomFactory();
        virtual void Release() = 0;
        virtual void GetBuffers(EGxVertexBufferFormat format, CGxBuf** vertexBuf, CGxBuf** indexBuf, CGxBatch* batch) = 0;
        virtual void Purge() = 0;
        virtual bool SetTransform(C44Matrix* transform) = 0;
    };

    class IMaterial {
        public:
        virtual ~IMaterial();
        virtual void Release() = 0;
        virtual void Render(IEnvironment* environment, IGeomFactory* geomFactory, IWaveManager* waveManager, const C3Vector& cameraPos, const C44Matrix* transform, const CAaSphere* bounds, CMaterialSettings* settings) = 0;
        // Not in the original binary. Ported from 15595; the material bank calls Reset() when the water
        // settings change. Added at the END of the virtual list so no existing 12340 slot moves - inserting
        // one mid-list is what broke CursorSetVisible earlier in this port. The base is empty, like 15595's;
        // the concrete materials in 12340 do not override it yet, which is the C.2 material stage.
        virtual void Reset();
    };

    class CMaterialProcWater : public IMaterial {
        public:
        static int32_t s_refCount;
        static CGxShader* s_vertexShaders[4];
        static CGxShader* s_pixelShader;

        static CMaterialProcWater* Create();

        virtual ~CMaterialProcWater();
        virtual void Release();
        virtual void Render(IEnvironment* environment, IGeomFactory* geomFactory, IWaveManager* waveManager, const C3Vector& cameraPos, const C44Matrix* transform, const CAaSphere* bounds, CMaterialSettings* settings);

        CMaterialProcWater();
    };

    class CMaterialProcWaterFFP : public IMaterial {
        public:
        static CMaterialProcWaterFFP* Create();

        virtual void Release();
        virtual void Render(IEnvironment* environment, IGeomFactory* geomFactory, IWaveManager* waveManager, const C3Vector& cameraPos, const C44Matrix* transform, const CAaSphere* bounds, CMaterialSettings* settings);
    };

    class CMaterialWater : public IMaterial {
        public:
        static int32_t s_refCount;
        static CGxShader* s_vertexShaders[4];
        static CGxShader* s_pixelShader;

        static CMaterialWater* Create();

        virtual ~CMaterialWater();
        virtual void Release();
        virtual void Render(IEnvironment* environment, IGeomFactory* geomFactory, IWaveManager* waveManager, const C3Vector& cameraPos, const C44Matrix* transform, const CAaSphere* bounds, CMaterialSettings* settings);

        CMaterialWater();
    };

    class CMaterialWaterNoSpec : public IMaterial {
        public:
        static int32_t s_refCount;
        static CGxShader* s_vertexShaders[4];
        static CGxShader* s_pixelShader;

        static CMaterialWaterNoSpec* Create();

        virtual ~CMaterialWaterNoSpec();
        virtual void Release();
        virtual void Render(IEnvironment* environment, IGeomFactory* geomFactory, IWaveManager* waveManager, const C3Vector& cameraPos, const C44Matrix* transform, const CAaSphere* bounds, CMaterialSettings* settings);

        CMaterialWaterNoSpec();
    };

    class CMaterialWaterFFP : public IMaterial {
        public:
        static CMaterialWaterFFP* Create();

        virtual void Release();
        virtual void Render(IEnvironment* environment, IGeomFactory* geomFactory, IWaveManager* waveManager, const C3Vector& cameraPos, const C44Matrix* transform, const CAaSphere* bounds, CMaterialSettings* settings);
    };

    class CMaterialMagma : public IMaterial {
        public:
        static int32_t s_refCount;
        static CGxShader* s_vertexShader;
        static CGxShader* s_pixelShader;

        static CMaterialMagma* Create();

        virtual ~CMaterialMagma();
        virtual void Release();
        virtual void Render(IEnvironment* environment, IGeomFactory* geomFactory, IWaveManager* waveManager, const C3Vector& cameraPos, const C44Matrix* transform, const CAaSphere* bounds, CMaterialSettings* settings);

        CMaterialMagma();
    };

    class CMaterialMagmaFFP : public IMaterial {
        public:
        static CMaterialMagmaFFP* Create();

        virtual void Release();
        virtual void Render(IEnvironment* environment, IGeomFactory* geomFactory, IWaveManager* waveManager, const C3Vector& cameraPos, const C44Matrix* transform, const CAaSphere* bounds, CMaterialSettings* settings);
    };

    class IVertexData {
        public:
        /* 0004 */ uint32_t unused_0004;

        virtual ~IVertexData();
        virtual float GetHeight(uint32_t index) = 0;
        virtual uint8_t GetDepth(uint32_t index) = 0;
        virtual const uint16_t* GetTexCoord(uint32_t index) = 0;
    };

    class CChunkVertexData : public IVertexData {
        public:
        /* 0008 */ const uint8_t* m_data;
        /* 000C */ uint32_t m_format;

        virtual float GetHeight(uint32_t index);
        virtual uint8_t GetDepth(uint32_t index);
        virtual const uint16_t* GetTexCoord(uint32_t index);

        CChunkVertexData(const uint8_t* data, uint32_t format);
    };

    class CAreaData;

    class CInstanceVertexData : public IVertexData {
        public:
        /* 0008 */ const uint8_t* m_data;
        /* 000C */ const SMLiquidInstance* m_instance;
        /* 0010 */ uint32_t m_vertexCount;

        virtual float GetHeight(uint32_t index);
        virtual uint8_t GetDepth(uint32_t index);
        virtual const uint16_t* GetTexCoord(uint32_t index);

        CInstanceVertexData(const CAreaData* areaData, const SMLiquidInstance* instance);
    };

    class CAreaData {
        public:
        static uint8_t s_emptyTable[8];
        static uint8_t s_fullTable[8];
        static uint16_t s_defaultTexCoord[2];

        /* 0000 */ uint32_t unused_0000;
        /* 0004 */ const uint8_t* m_data;

        CAreaData(const void* data);
        void SetData(const void* data);
        bool HasData() const;
        const SMLiquidChunkDesc* GetChunkDesc(uint32_t x, uint32_t y) const;
        const SMLiquidInstance* GetInstanceDesc(const SMLiquidChunkDesc* chunk, uint32_t index) const;
        const uint8_t* GetFishableTable(const SMLiquidChunkDesc* chunk) const;
        const uint8_t* GetDeepTable(const SMLiquidChunkDesc* chunk) const;
        const uint8_t* GetExistsTable(const SMLiquidInstance* instance) const;
    };

    class CMaterialSettings {
        public:
        /* 0000 */ char m_textureNames[6][128];
        /* 0300 */ CImVector m_colors[2];
        /* 0308 */ int32_t m_ints[4];
        /* 0318 */ float m_floats[18];
        /* 0360 */ int32_t m_transparent;
        /* 0364 */ int32_t m_texturesReady[6];
        /* 037C */ TSGrowableArray<CTexture*> m_textures[6];
        /* 03DC */ TSGrowableArray<CTexture*> m_pendingTextures[6];

        CMaterialSettings();
        ~CMaterialSettings();
        void Destroy();
        float GetFloat(uint32_t index) const;
        int32_t GetInt(uint32_t index) const;
        CGxTex* GetTexture(uint32_t index, uint32_t period);
        void InitTexture();
    };

    class CInstance {
        public:
        static CInstance* Create();

        /* 0000 */ IMaterial* m_material;
        /* 0004 */ CMaterialSettings* m_settings;
        /* 0008 */ IGeomFactory* m_geomFactory;
        /* 000C */ IWaveManager* m_waveManager;
        /* 0010 */ IEnvironment* m_environment;
        /* 0014 */ C44Matrix m_transform;
        /* 0054 */ CAaSphere m_bounds;
        /* 0064 */ int32_t m_queued;
        /* 0068 */ int32_t m_refCount;

        CInstance();
        void AddRef();
        void Purge();
        void Release();
    };

    class CRenderer {
        public:
        typedef int32_t (*DrawListSorter)(const void* a, const void* b);

        static DrawListSorter s_drawListSorter[2];

        static int32_t CompareOpaque(const void* a, const void* b);
        static int32_t CompareTransparent(const void* a, const void* b);

        /* 0000 */ TSGrowableArray<CInstance*> m_instances[2];

        ~CRenderer();
        void Draw(CInstance* instance);
        void Flush(const C3Vector& cameraPos, uint32_t pass);
        void Validate();
    };

    class CSettingsBank {
        public:
        static TSGrowableArray<CMaterialSettings*> s_settings;

        static void Destroy();
        static CMaterialSettings* GetSettings(uint32_t liquidType);
        // Not in the original binary. Ported from 15595.
        static void Purge();
        static void ResetTextures();
        static bool UpdateRecord(CMaterialSettings* settings, uint32_t liquidType);
    };

    class CMaterialBank {
        public:
        static TSGrowableArray<IMaterial*> s_materials;

        static void Destroy();
        static IMaterial* GetMaterial(uint32_t liquidType);
        // Not in the original binary. Ported from 15595.
        static void Purge();
        static void ResetMaterials();
    };

    extern CDataAllocator s_settingsAllocator;
    extern CDataAllocator s_instanceAllocator;
    extern CDataAllocator s_procWaterAllocator;
    extern CDataAllocator s_procWaterFFPAllocator;
    extern CDataAllocator s_waterAllocator;
    extern CDataAllocator s_waterNoSpecAllocator;
    extern CDataAllocator s_waterFFPAllocator;
    extern CDataAllocator s_magmaAllocator;
    extern CDataAllocator s_magmaFFPAllocator;
    extern float s_worldCoordScale;
    extern int32_t s_specularEnabled;
    extern IDatabase<LiquidMaterialRec>* s_liquidMaterialDB;
    extern IDatabase<LiquidTypeRec>* s_liquidTypeDB;
    extern char s_shaderPath[256];
    extern int32_t s_shadersEnabled;
    extern int32_t s_useCameraRelativeCoords;
    extern int32_t s_settingsBankRefCount;
    extern int32_t s_materialBankRefCount;
    // Not in the original binary. Ported from 15595 (Cataclysm). 12340's s_unk_D43B48/s_unk_D43B4C/s_unk_D43B50
    // were never written anywhere in this tree - the caches below replace them, which is what 15595 did too.
    extern uint8_t s_oceanDepthTexValid;
    extern uint8_t s_riverDepthTexValid;
    extern uint8_t s_wmoWaterTexValid;
    extern uint8_t s_displMapDirty;
    extern uint32_t s_displMapCount;
    extern TSGrowableArray<DisplMapPerturbation*> s_eventPerturbations[2];
    extern TSGrowableArray<DisplMapPerturbation*> s_perturbations[2];
    extern TSGrowableArray<DisplMapPerturbationMesh*> s_eventPerturbationMeshes[2];
    extern TSGrowableArray<DisplMapPerturbationMesh*> s_perturbationMeshes[2];
    extern CGxFrameBuffer* s_displMapFBs[4];
    extern CTexture* s_displMaps[4];
    extern CGxShader* s_clearDisplMapPS;
    extern CGxShader* s_clearDisplMapVS;
    extern CGxShader* s_fourTapBlurPS;
    extern CGxShader* s_fourTapBlurVS;
    extern uint32_t s_kawasePingPong;
    extern CGxShader* s_perturbDisplMapMeshPS;
    extern CGxShader* s_perturbDisplMapMeshVS;
    extern CGxShader* s_perturbDisplMapPS;
    extern CGxShader* s_perturbDisplMapVS;
    extern CTexture* s_perturbTex;
    extern CGxShader* s_propagateDisplMapPS;
    extern CGxShader* s_propagateDisplMapVS;
    extern CTexture* s_wmoWaterTex;
    extern CTexture* s_oceanDepthTex;
    extern CTexture* s_riverDepthTex;
    extern VSConstants s_vsConstants;
    extern VSWaveConstants s_vsWaveConstants;
    extern PSConstants s_psConstants;
    extern PSProcWaterConstants s_psProcWaterConstants;

    void CacheProceduralTextures();
    void ClearMaterials();
    // Not in the original binary. 12340 took no parameter; 15595 keys the work off whether the material bank
    // is also being rebuilt, so the parameter was added to match.
    void CreateProceduralTextures(int32_t materials);
    void DestroyProceduralTextures(int32_t materials);
    void ExtWaterDepthTexCallback(EGxTexCommand command, uint32_t width, uint32_t height, uint32_t depth, uint32_t mipLevel, void* userArg, uint32_t& texelStrideInBytes, const void*& texels);
    const char* GetShaderSuffix();
    C44Matrix* GetTexScrollMtx(const C2Vector& speed);
    int32_t Initialize(int32_t a1, float a2, int32_t a3, const char* a4, IDatabase<LiquidTypeRec>* liquidTypeDB, IDatabase<LiquidMaterialRec>* liquidMaterialDB);
    void InvalidateProceduralTextures();
    void SetCircularWave(uint32_t index, const C2Vector& a2, const C2Vector& a3, float a4, float a5, float a6, float speed);
    void SetDirectionalWave(uint32_t index, const C2Vector& direction, float a3, float a4, float a5, float speed);
    void SetProcWaterLighting(const CM2Lighting& lighting);
    void SetProcWaterParams(float a1, float a2, float a3, float a4, float a5, float a6, float a7);
    void SetShaderConstants();
    void SetShaderLighting(const CM2Lighting& lighting);
    void SetShaderTransforms(const C3Vector& cameraPos, const C44Matrix& transform);
    void Shutdown();
    void UpdateProceduralTextures();
    void SetShadersEnabled(int32_t enabled);
    void SetSpecularEnabled(int32_t enabled);
    int32_t UseCameraRelativeCoords();
    void WmoWaterTexCallback(EGxTexCommand command, uint32_t width, uint32_t height, uint32_t depth, uint32_t mipLevel, void* userArg, uint32_t& texelStrideInBytes, const void*& texels);
    float WorldCoordScale();

    // Not in the original binary. Ported from 15595 (Cataclysm) - the settings/config cluster of the new water
    // system, added in one block so the C.1 additions stay visible as a unit. These are what tie the water
    // detail level to DepthCache (SetWaterDetail -> DepthCache::AddRef/Release), which is what gives the
    // Stage B port an owner. The material/renderer half of 15595's Liquid is a separate stage.
    typedef void (*CONFIG_CHANGED_CALLBACK)();

    void AddConfigChangedCallback(CONFIG_CHANGED_CALLBACK callback);
    void RemoveConfigChangedCallback(CONFIG_CHANGED_CALLBACK callback);
    void SignalConfigChanged(int32_t materials);
    uint32_t GetWaterDetail();
    uint32_t GetRippleDetail();
    uint32_t GetReflectionMode();
    uint32_t GetReflectionDownscale();
    int32_t WaterDetailNeedsDepthCache(uint32_t detail);
    int32_t CameraSubmerged();
    void SetCameraSubmerged(int32_t submerged);
    int32_t NeedBackBuffer();
    void SetNeedBackBuffer(int32_t need);
    int32_t NeedDynDisplMap();
    void SetNeedDynDisplMap(int32_t need);
    int32_t GetShadersEnabled();
    uint32_t ValidateWaterDetail(uint32_t detail);
    int32_t SetWaterDetail(uint32_t detail);
    uint32_t ValidateRippleDetail(uint32_t detail);
    int32_t SetRippleDetail(uint32_t detail);
    uint32_t ValidateReflectionMode(uint32_t mode);
    int32_t SetReflectionMode(uint32_t mode);
    uint32_t ValidateReflectionDownscale(uint32_t downscale);
    int32_t SetReflectionDownscale(uint32_t downscale);
    void SetUnderWaterFog(float start, float end, float density, const CImVector& color);
    void GetUnderWaterFog(float& start, float& end, float& density, CImVector& color);
    void SetAboveWaterFog(float start, float end, float density, const CImVector& color);
    void GetAboveWaterFog(float& start, float& end, float& density, CImVector& color);

    // Not in the original binary. Ported from 15595 (Cataclysm) - Stage D.1: the reflection / back / blur
    // buffer set and the displacement-map textures, plus the Update() loop that creates them. The perturbator
    // simulation (CreatePerturbator/PerturbDisplMap/PropagateDisplMap/KawaseBlur) and the render path were left
    // out: both need waterDetail 2+, which ValidateWaterDetail rejects until the D3D11 backend makes DepthCache
    // supported. The buffer plumbing below is what has to exist first either way.
    void Update();
    void GrabBackBuffer();
    void OnTexturesDirty();
    void UnCacheProceduralTextures();
    void CreateDynamicDisplacementMap();
    void SetDisplMapInfo(const C3Vector& pos, const C2Vector& size);
    void GetDisplMapInfoRaw(uint32_t index, C3Vector& pos, C2Vector& size);
    void ClearPerturbatorCache();
    void ClearEventPerturbations();
    int32_t HasDynamicDisplMap();
    uint32_t GetDynamicDisplMapCnt();
    CGxTex* GetDynamicDisplMap(uint32_t index);
    float GetDynamicDisplMapInterpolation(uint32_t index);
    C2iVector GetDisplMapTexSize(uint32_t index);
    CTexture* GetBackBuffer();
    CTexture* GetBlurredBackBuffer();
    CTexture* GetRealReflectionBuffer();
    CTexture* GetBlurredReflectionBuffer();
    CTexture* GetFakeReflectionBuffer();
    CGxFrameBuffer* GetBlurredBackBufferFB();
    CGxFrameBuffer* GetBlurredReflectionBufferFB();
    CGxFrameBuffer* GetRealReflectionFB();
    CGxFrameBuffer* GetFakeReflectionFB();
}

// INLINED
inline Liquid::IVertexData::~IVertexData() {
}

#endif
