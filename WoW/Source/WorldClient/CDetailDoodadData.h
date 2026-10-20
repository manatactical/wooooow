#ifndef WOW_SOURCE_WORLDCLIENT_CDETAILDOODADDATA_H
#define WOW_SOURCE_WORLDCLIENT_CDETAILDOODADDATA_H

#include <cstddef>
#include <cstdint>

class CM2Model;
class CTexture;
class GroundEffectDoodadRec;

class CDetailDoodadData {
    public:
    static void ModelLoadedCallback(CM2Model* model, void* arg);

    /* 0000 */ GroundEffectDoodadRec* m_record;
    /* 0004 */ CM2Model* m_model;
    /* 0008 */ CTexture* m_texture;

    // Not in the original binary. The wind's per-doodad geometry, filled in by CGrassMotion::AnalyzeDoodad
    // from the vertices the skin actually uses: the mean UV V of the lowest 12% of the model's height (the
    // blade's root) and of the highest 12% (its tip), and the reciprocal of the span between them. A doodad
    // too flat to bend over anything -- a pebble, a shell -- is left with m_grassSways clear and then never
    // moves, which is what keeps stock decoration from shivering.
    /* 000C */ float m_grassRootV;
    /* 0010 */ float m_grassInvSpan;
    /* 0014 */ int32_t m_grassSways;
    /* 0018 */ int32_t m_grassAnalyzed;

    int32_t Load();
};

#endif