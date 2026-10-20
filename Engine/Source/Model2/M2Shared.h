#ifndef ENGINE_SOURCE_MODEL2_M2SHARED_H
#define ENGINE_SOURCE_MODEL2_M2SHARED_H

#include <cstddef>
#include <cstdint>
#include "Engine/Source/Model2/M2Data.h"
#include "Engine/Source/Tempest/CAaBox.h"
#include "Engine/Source/Storm/H/stpl.h"

class CAsyncObject;
struct CGxBuf;
struct CGxPool;
class CM2Cache;
class CM2Model;
class CM2SequencePlayBack;
class CM2Shared;
class CShaderEffect;
class CTexture;
class SFileImpl;

class TextureComboList {
    public:
    /* 0000 */ uint32_t m_count;
    /* 0004 */ uint16_t* m_data;
};

class CM2SequenceLoad : public TSLinkedNode<CM2SequenceLoad> {
    public:
    /* 0008 */ CAsyncObject* m_asyncObject;
    /* 000C */ CM2Shared* m_shared;
    /* 0010 */ uint16_t m_sequenceIndex;
    /* 0012 */ uint16_t m_sequenceBufferIndex;
    /* 0014 */ TSList<CM2SequencePlayBack> m_playbacks;

    CM2SequenceLoad();
    ~CM2SequenceLoad();
};

class CM2Shared {
    public:
    /* 0000 */ uint32_t m_refCount;
    /* 0004 */ CM2Cache* m_cache;
    /* 0008 */ uint32_t m_flags;
    /* 000C */ CAsyncObject* m_asyncObject;
    /* 0010 */ CM2Model* m_callbackList;
    /* 0014 */ CM2Model** m_callbackListTail;
    /* 0018 */ TSList<CM2SequenceLoad> m_sequenceLoads;
    /* 0024 */ uint32_t m_lowPrioritySequenceCapacity;
    /* 0028 */ uint8_t** m_lowPrioritySequenceBuffers;
    /* 002C */ uint32_t m_lowPrioritySequenceCount;
    /* 0030 */ CM2Shared** m_freePrev;
    /* 0034 */ CM2Shared* m_freeNext;
    /* 0038 */ uint32_t m_freeTime;
    /* 003C */ char m_fileName [260];
    /* 0140 */ char* m_fileNameWithoutPath;
    /* 0144 */ CM2Shared** m_previous;
    /* 0148 */ CM2Shared* m_next;
    /* 014C */ uint32_t m_fileNameHash;
    /* 0150 */ M2Data* m_data;
    /* 0154 */ CAaBox m_boundingBox;
    /* 016C */ uint32_t m_fileSize;
#if defined(WOWDECOMP_MODERN_ASSETS)
    // NOT IN BINARY, build-only: writable bytes at m_data. A modern model is repacked in place and
    // appends its resolved texture paths past the body, so the allocation carries a reserve the body
    // length alone does not show. Stock models leave this equal to m_fileSize.
    /* ---- */ uint32_t m_allocSize;
#endif
    /* 0170 */ M2SkinProfile* m_skinData;
    /* 0174 */ CTexture** m_textures;
    /* 0178 */ CGxPool* m_indexPool;
    /* 017C */ CGxBuf* m_indexBuf;
    /* 0180 */ CGxPool* m_vertexPool;
    /* 0184 */ CGxBuf* m_vertexBuf;
#if defined(WOWDECOMP_ARCH_64)
    // NOT IN BINARY: the expanded vertex buffer, when it had to be reallocated instead of overwritten in
    // place. M2Array::m_offset is a signed 32-bit delta from m_data, so a runtime allocation more than
    // 2 GiB away would truncate; holding the pointer here keeps the format field 32-bit and the pointer
    // 64-bit. Null means the vertices still live in the loaded body.
    /* ---- */ M2Vertex* m_vertexData;
#endif
    /* 0188 */ CShaderEffect** m_effects;
    /* 018C */ M2SkinSection* m_skinSections;
    /* 0190 */ uint32_t m_instances;
    /* 0194 */ uint32_t m_maxInstances;
    /* 0198 */ uint16_t m_billboardBoneCount;
    /* 019C */ uint32_t m_lastUsedTime;
    /* 01A0 */ TSLink<CM2Shared> m_updateLink;

    static void LoadFailedCallback(void* param);
    static void LoadSucceededCallback(void* param);
    static void LowPrioritySequenceFailedCallback(void* param);
    static void LowPrioritySequenceLoadedCallback(void* param);
    static void SkinProfileLoadedCallback(void* param);

    CM2Shared(CM2Cache* cache);
    ~CM2Shared();
    uint32_t AddRef();
    uint32_t AllocInstances(uint32_t count);
    void AssignBatchTextureComboIndices();
    int32_t CallbackWhenLoaded(CM2Model* model);
    void CancelAllDeferredSequences();
    void ConvertTextureCombosToValues();
    void ConvertTextureValuesToCombos();
    void DestroyBuffers();
    int32_t FinishLoading();
    int32_t FinishLoadingLowPrioritySequence(uint32_t sequenceIndex, uint8_t* buffer, uint32_t size);
    int32_t FinishLoadingLowPrioritySequence(uint32_t sequenceIndex, CAsyncObject* object);
    int32_t FinishLoadingSkinProfile(uint32_t size);
    uint32_t GetSkinProfileIndex(uint32_t maxBones);
    // NOT IN BINARY: the vertex array's runtime base. M2Array::m_offset is a signed 32-bit delta from
    // m_data and cannot hold a pointer that landed elsewhere on x64, so the expanded buffer is tracked
    // here instead. Falls back to the in-body array.
    // Defined inline on purpose: called once per vertex in every transform and hit-test loop, and an
    // out-of-line call there is measurably worse.
    inline M2Vertex* Vertices() const {
#if defined(WOWDECOMP_ARCH_64)
        if (this->m_vertexData) {
            return this->m_vertexData;
        }
#endif
        return this->m_data->m_vertices.Data();
    }
    void SetVerticesArray(M2Vertex* data, uint32_t count);
    CShaderEffect* GetEffect(M2Batch* batch);
    CShaderEffect* GetEffect(uint32_t textureCount, uint16_t shader, uint16_t textureCoordComboIndex);
    int32_t Initialize();
    int32_t InitializeSkinProfile();
    int32_t Load(SFileImpl* file, int32_t flags, CAaBox* boundingBox);
#if defined(WOWDECOMP_MODERN_ASSETS)
    // NOT IN BINARY, build-only: repacks an MD21 container in place as a v264 body (see M2Modern.h).
    void ConvertModernModel();
    void ConvertModernSkin();
#endif
    CM2SequenceLoad* LoadLowPrioritySequence(uint16_t sequenceIndex);
    int32_t LoadSkinProfile(uint32_t profile);
    void Release();
    int32_t SetIndices();
    int32_t SetVertices(uint32_t texCoordIndex);
    void SubstituteSimpleShaders();
    void SubstituteSpecializedShaders();
};

template <class T>
int32_t M2Init(uint8_t* base, uint32_t size, const M2Data& data, M2Array<T>& array);

template <class T>
int32_t M2Init(uint8_t* base, uint32_t size, const M2Data& data, M2Array<M2Array<T> >& array);

template <class T>
int32_t M2Init(uint8_t* base, uint32_t size, const M2Data& data, M2Track<T>& track);

int32_t M2Init(uint8_t* base, uint32_t size, const M2Data& data, M2TrackBase& track);
int32_t M2Init(uint8_t* base, uint32_t size, const M2Data& data, M2Particle& particle);
int32_t M2Init(uint8_t* base, uint32_t size, M2Data& data);
void M2HeapSort(int32_t (*compare)(uint32_t, uint32_t, const void*), uint32_t* array, uint32_t count, const void* param);

#endif
