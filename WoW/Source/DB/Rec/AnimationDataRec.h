#ifndef WOW_SOURCE_DB_REC_ANIMATIONDATAREC_H
#define WOW_SOURCE_DB_REC_ANIMATIONDATAREC_H

#include <cstddef>
#include <cstdint>
#include <vector>

class SFileImpl;

class AnimationDataRec {
    public:
    static const int32_t s_numColumns = 8;
    static const int32_t s_rowSize = 32;
    static const bool s_generatedID = false;

    /* 0000 */ int32_t m_id;
    /* 0004 */ const char* m_name;
    /* 0008 */ int32_t m_weaponflags;
    /* 000C */ int32_t m_bodyflags;
    /* 0010 */ int32_t m_flags;
    /* 0014 */ int32_t m_fallback;
    /* 0018 */ int32_t m_behaviorID;
    /* 001C */ int32_t m_behaviorTier;

    AnimationDataRec();
    ~AnimationDataRec();
    static const char* GetFilename();
    bool Read(SFileImpl* file, const char* stringBuffer);

    /**
     * @brief Builds this table from modern content when the stock .dbc describes the wrong era.
     *
     * The stock AnimationData.dbc stops at id 505. Modern content -- the retail mounts in this pack among
     * it -- uses ids far past that, and GetRecord() is a minId/maxId range check, so every one of those ids
     * returns null: no behaviour id, no fallback chain, no animation. AnimationData.db2 is the same table at
     * a later revision covering ids 0..1865, so it is the authority here.
     *
     * @param out receives the complete WDBC image.
     * @return true when the image was built.
     */
    static bool BuildModernTable(std::vector<uint8_t>& out);
};

#endif
