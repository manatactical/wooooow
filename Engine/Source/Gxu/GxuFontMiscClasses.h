#ifndef ENGINE_SOURCE_GXU_GXUFONTMISCCLASSES_H
#define ENGINE_SOURCE_GXU_GXUFONTMISCCLASSES_H

#include <cstddef>
#include <cstdint>
#include "Engine/Source/Gx/Types.h"
#include "Engine/Source/Gxu/GxuFontString.h"
#include "Engine/Source/Storm/H/stpl.h"
#include "Engine/Source/Tempest/C2Vector.h"
#include "Engine/Source/Tempest/C3Vector.h"
#include "Engine/Source/Tempest/CImVector.h"
#include "Engine/Source/Tempest/CRect.h"

class CGxVertexPCT;
class CTexture;
struct CGxBuf;
struct CGxFont;
struct CGxPool;

// NOT IN BINARY. Largest glyph raster height in pixels. The PasteGlyph* functions in
// GxuFontTextureCache.cpp index row-major scratch buffers by m_cellHeight, which is
// pixelSize + 4 for thick-outline fonts, so the two must move together. The original
// binary capped pixelSize at 32, which is exactly what kept those buffers at 36 rows.
//
// 32 is the original. Raising it does NOT just sharpen text -- pixelSize is the font's on-screen size,
// so a bigger cap makes text genuinely larger than vanilla on high resolutions. At 2160p a UI font asks
// for 0.0183 * 2160 = ~40 px; the old cap clamped that to 32 and text was 22% smaller than it "wanted"
// to be. Removing the clamp therefore RESIZES the whole UI, which is not what we want.
//
// Keep 32 to preserve vanilla sizes. Quality work belongs in GXU_FONT_SUPERSAMPLE below, which raises
// the raster resolution without changing the drawn size.
inline constexpr uint32_t GXU_FONT_MAX_PIXEL_SIZE = 32;
inline constexpr uint32_t GXU_FONT_MAX_CELL_HEIGHT = GXU_FONT_MAX_PIXEL_SIZE + 4;

// NOT IN BINARY. Glyphs rasterize at this multiple of their drawn size. The draw scale in
// CGxString::InitializeTextLine is fontPixelHeight / GetPixelSize(), so it lands at
// 1/GXU_FONT_SUPERSAMPLE: linear filtering then minifies a high-resolution raster instead of
// magnifying a low-resolution one. That is what actually looks HD; raising the cap alone does not.
inline constexpr uint32_t GXU_FONT_SUPERSAMPLE = 2;

// NOT IN BINARY. Ceiling for the RASTER size, which is separate from the on-screen cap above: a font
// may draw at 32 px while sampling at 64. Clamping the raster by GXU_FONT_MAX_PIXEL_SIZE would make
// supersampling a no-op for every font at or above the cap, which is most of them.
// Derived from the scratch grid: the paste routines cannot address a cell taller than it. This is the
// 32 * 2 + 4 the buffers were sized for at 2x.
inline constexpr uint32_t GXU_FONT_MAX_RASTER_SIZE = GXU_FONT_MAX_PIXEL_SIZE * GXU_FONT_SUPERSAMPLE;
inline constexpr uint32_t GXU_FONT_MAX_RASTER_CELL = GXU_FONT_MAX_RASTER_SIZE + 4;

// NOT IN BINARY. Side length of one TEXTURECACHE page, in texels. Was hardcoded as 256 in a dozen
// places: the row stride, the row-shift, GxTexUpdate's rectangle, TextureCreate, the row count and
// the UV divisor. 512 keeps a 2x-supersampled glyph set resident (512x512 ARGB4444 is 512 KB per
// page, 4 MB across the 8 pages).
inline constexpr uint32_t GXU_FONT_ATLAS_SIZE = 512;

// Width of the local scratch rows the PasteGlyph* functions build before writing into the atlas.
// This is deliberately NOT the atlas size: the neighbor tests are written against a 256-wide
// scratch grid (cell[-257] .. cell[257]) and only the destination stride follows the atlas. 256
// still exceeds GXU_FONT_MAX_CELL_HEIGHT, so no glyph cell can overrun it.
inline constexpr uint32_t GXU_FONT_SCRATCH_WIDTH = 256;

// The PasteGlyph* routines address the scratch grid with a hardcoded << 8 and +/-255/256/257
// neighbours, which only hold while the scratch width is exactly 256. The cell must also fit a row.
static_assert(GXU_FONT_SCRATCH_WIDTH == 256, "PasteGlyph* neighbour offsets assume a 256-wide scratch grid");
static_assert(GXU_FONT_MAX_RASTER_CELL <= GXU_FONT_SCRATCH_WIDTH, "raster cell is wider than a scratch row");
static_assert(GXU_FONT_MAX_RASTER_CELL <= GXU_FONT_ATLAS_SIZE, "raster cell is taller than an atlas page");

// NOT IN BINARY. Transparent texels reserved after every glyph in the atlas. Glyphs used to be packed edge
// to edge, so with GXU_FONT_SUPERSAMPLE > 1 the linear sampler (which reaches a texel past the quad) read
// the neighbouring glyph -- that is the intermittent artifacting on some words. 2 texels is the bilinear
// footprint at 2x downscale.
inline constexpr uint32_t GXU_FONT_GLYPH_GUTTER = 2;

// NOT IN BINARY. Per-font supersampling control. Glyphs are rasterized at GXU_FONT_SUPERSAMPLE times their
// drawn size only when the font asks for it.
//
// Supersampling is a clear win for large text (combat numbers, zone banners, name plates) and a clear loss
// for small text: a 1px stem rasterized at 2x and sampled back down spreads across two device pixels at
// half alpha, which reads as thin and blurry rather than sharp. Vanilla drew 1:1 and was crisp by
// construction, so small UI text opts out and keeps that.
//
// Set by OR-ing GXU_FONTOPT_SUPERSAMPLE into the flags passed to GxuFontCreateFont / TextBlockGenerateFont.
// Bit 0x10 is free in the gxFlags space (1 outline, 2 monochrome, 4 outline padding, 8 thick outline).
inline constexpr uint32_t GXU_FONTOPT_SUPERSAMPLE = 0x10;

struct GLYPHBITMAPDATA {
    /* 0000 */ void* m_data;
    /* 0004 */ uint32_t m_dataSize;
    /* 0008 */ uint32_t m_glyphWidth;
    /* 000C */ uint32_t m_glyphHeight;
    /* 0010 */ uint32_t m_glyphCellWidth;
    /* 0014 */ float m_glyphAdvance;
    /* 0018 */ float m_glyphBearing;
    /* 001C */ uint32_t m_glyphPitch;
    /* 0020 */ int32_t m_yOffset;
    /* 0024 */ uint32_t m_yStart;
    /* 0028 */ CRect m_textureCoords;

    GLYPHBITMAPDATA();
    ~GLYPHBITMAPDATA();
    void CopyFrom(GLYPHBITMAPDATA* source);
};

struct CHARCODEDESC : public TSHashObject<CHARCODEDESC, HASHKEY_NONE> {
    /* 0018 */ TSLink<CHARCODEDESC> m_textureRowLink;
    /* 0020 */ TSLink<CHARCODEDESC> m_fontGlyphLink;
    /* 0028 */ uint32_t m_textureNumber;
    /* 002C */ uint32_t m_rowNumber;
    /* 0030 */ uint32_t m_glyphStartPixel;
    /* 0034 */ uint32_t m_glyphEndPixel;
    /* 0038 */ GLYPHBITMAPDATA m_bitmapData;

    CHARCODEDESC();
    void GenerateTextureCoords(uint32_t rowNumber, uint32_t cellHeight);
    uint32_t GapToPrevious();
};

struct TEXTLINETEXTURE : public TSLinkedNode<TEXTLINETEXTURE> {
    static int32_t s_recycledBytes;
    static TSList<TEXTLINETEXTURE, TSGetLink<TEXTLINETEXTURE> > s_freeTextLineTextures;

    static TEXTLINETEXTURE* NewTextLineTexture();
    static void Recycle(TEXTLINETEXTURE* ptr);
    static void Shutdown();

    /* 0008 */ TSGrowableArray<VERT> m_verts;
    /* 0018 */ TSGrowableArray<CImVector> m_colors;

    TEXTLINETEXTURE();
    ~TEXTLINETEXTURE();
    void WriteGeometry(CGxVertexPCT* buf, const CImVector& fontColor, const C2Vector& shadowOffset, const CImVector& shadowColor, const C3Vector& viewTranslation, bool hasShadow, bool hasGradient, int32_t vertIndex, int32_t numVerts) const;
};

struct BATCHEDRENDERFONTDESC : public TSHashObject<BATCHEDRENDERFONTDESC, HASHKEY_PTR> {
    static CGxPool* s_indexPool;
    static CGxBuf* s_indexBuf;
    static uint8_t s_billboarded;

    static void Initialize();
    static void InitializeIndexBuff();
    static void Shutdown();
    static CGxVertexPCT* UnlockVertexPtrAndRender(CGxBuf*& buf, int32_t count);

    /* 0018 */ CGxFont* m_face;
    /* 001C */ TSExplicitList<CGxString, offsetof(CGxString, m_batchedStringLink)> m_strings;

    ~BATCHEDRENDERFONTDESC();
    void RenderBatch();
};

struct CGxStringBatch : public TSLinkedNode<CGxStringBatch> {
    /* 0008 */ uint32_t m_flags;
    /* 000C */ TSHashTable<BATCHEDRENDERFONTDESC, HASHKEY_PTR> m_fontBatch;

    CGxStringBatch();
    void AddString(CGxString* string);
    void RenderBatch();
};

struct TEXTURECACHEROW {
    /* 0000 */ uint32_t m_widestFreeSlot;
    /* 0004 */ TSExplicitList<CHARCODEDESC, offsetof(CHARCODEDESC, m_textureRowLink)> m_glyphlist;

    TEXTURECACHEROW();
    CHARCODEDESC* CreateNewDesc(GLYPHBITMAPDATA* data, uint32_t rowNumber, uint32_t cellHeight);
    void EvictGlyph(CHARCODEDESC* desc);
};

struct TEXTURECACHE {
    static void TextureCallback(EGxTexCommand command, uint32_t width, uint32_t height, uint32_t face, uint32_t level, void* userArg, uint32_t& texelStrideInBytes, const void*& texels);

    /* 0000 */ CTexture* m_texture;
    /* 0004 */ CGxFont* m_theFace;
    /* 0008 */ uint8_t m_anyDirtyGlyphs;
    /* 000C */ TSFixedArray<TEXTURECACHEROW> m_textureRows;

    TEXTURECACHE();
    ~TEXTURECACHE();
    CHARCODEDESC* AllocateNewGlyph(GLYPHBITMAPDATA* data);
    void CreateTexture(int32_t filter);
    void Destroy();
    void Initialize(CGxFont* face, uint32_t cellHeight);
    void PasteGlyphNonOutlinedAA(const GLYPHBITMAPDATA& data, uint16_t* dest);
    void PasteGlyphNonOutlinedMonochrome(const GLYPHBITMAPDATA& data, uint16_t* dest);
    void PasteGlyphOutlinedAA(const GLYPHBITMAPDATA& data, uint16_t* dest);
    void PasteGlyphOutlinedMonochrome(const GLYPHBITMAPDATA& data, uint16_t* dest);
    void UpdateDirty();
};

#endif
