#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include "Engine/Source/Gxu/GxuFontFont.h"
#include "Engine/Source/Base/ConvertUTF.h"
#include "Engine/Source/Gxu/GxuFontFace.h"
#include "Engine/Source/Gxu/GxuFont.h"
#include "Engine/Source/Gxu/GxuFontUtil.h"
#include "Engine/Source/Gxu/IGxuFontGlyph.h"
#include "Engine/Source/Services/Texture.h"
#include "Engine/Source/Storm/H/storm.h"

// INLINED
KERNINGHASHKEY& KERNINGHASHKEY::operator=(const KERNINGHASHKEY& key) {
    if (this != &key) {
        this->m_code = key.m_code;
    }

    return *this;
}

// INLINED
bool KERNINGHASHKEY::operator==(const KERNINGHASHKEY& key) {
    return this == &key || this->m_code == key.m_code;
}

// VERIFY: d974db0f auto
// OFFSET: 0x006C2260
const char* CGxFont::GetName() {
    return FontFaceGetFontName(this->m_fontFaceHandle);
}

// VERIFY: 637e266a auto
// OFFSET: 0x006C2270
uint32_t CGxFont::GetPixelSize() {
    // NOT IN BINARY behaviour change: returns the rasterized size, not the requested one. Every glyph
    // metric (advance, bearing, m_pixelsPerUnit kerning) is measured at the raster size, so the
    // "pixelHeight / GetPixelSize()" ratios in GxuFontString/GxuFontUtil only mean on-screen pixels
    // when they divide by the size the glyphs were sampled at. With a font that opted out of
    // supersampling this equals m_pixelSize and nothing changes.
    return this->m_rasterSize;
}

// VERIFY: 3c5dbf73 manual
// OFFSET: 0x006C2280
float CGxFont::FindNearestFixedSize(FT_Face face, float height) {
    if (face->num_fixed_sizes == 0) {
        return height;
    }

    int32_t smallest = 0x7FFFFFFF;

    for (int32_t i = 0; i < face->num_fixed_sizes; i++) {
        if (smallest >= face->available_sizes[i].height) {
            smallest = face->available_sizes[i].height;
        }
    }

    float fixedHeight = PixelToScreenHeight(smallest);

    return fixedHeight < height ? height : fixedHeight;
}

// VERIFY: d7c70ac7 manual
// OFFSET: 0x006C22F0
int32_t CGxFont::UpdateDimensions() {
    FT_Face face = FontFaceGetFace(this->m_fontFaceHandle);

    float height = this->FindNearestFixedSize(face, this->m_fontHeight);
    double minHeight = 2.0 / g_heightPixels;
    height = height > minHeight ? height : minHeight;

    uint32_t pixelSize = static_cast<uint32_t>(ScreenToPixelHeight(0, height));

    // NOT IN BINARY. A zero here used to only log and then proceed, which left m_pixelSize at 0 and made
    // every ratio downstream divide by zero. Floor it to 1 pixel so a bad font height degrades to
    // unreadable rather than undefined.
    if (pixelSize == 0) {
        DebugLog("Error, font %s being created with height %g at screen res of %dx%d which is %d pixels!", this->m_fontName, height, g_widthPixels, g_heightPixels, pixelSize);
        pixelSize = 1;
    } else if (pixelSize > GXU_FONT_MAX_PIXEL_SIZE) {
        pixelSize = GXU_FONT_MAX_PIXEL_SIZE;
    }

    // m_pixelSize is the size the caller asked for and the size one-to-one text is drawn at; the
    // extent/wrap ratios divide by GetPixelSize(), which is the raster size, so those stay in raster
    // units. Glyphs are rasterized at m_rasterSize and drawn at scale = screen / raster, which lands
    // near 1/GXU_FONT_SUPERSAMPLE: linear filtering minifies a high-resolution raster instead of
    // magnifying a low-resolution one.
    this->m_pixelSize = pixelSize;

    // Supersample only when this font opted in. Everything downstream keys off m_rasterSize, so a font
    // that opts out behaves exactly as the original did (raster == drawn size, scale == 1).
    const uint32_t supersample = (this->m_fontFlags & GXU_FONTOPT_SUPERSAMPLE) ? GXU_FONT_SUPERSAMPLE : 1;
    uint32_t rasterSize = pixelSize * supersample;

    // Clamp against the RASTER ceiling, not GXU_FONT_MAX_PIXEL_SIZE. Using the on-screen cap here made
    // supersampling a silent no-op: every font drawing at >= 32 px (all of them, on a high-res UI) had
    // its raster clamped straight back to 32, so nothing was ever oversampled.
    if (rasterSize > GXU_FONT_MAX_RASTER_SIZE) {
        rasterSize = GXU_FONT_MAX_RASTER_SIZE;
    }

    this->m_rasterSize = rasterSize;

    int32_t units = face->ascender + abs(face->descender);

    if (units == 0) {
        return 0;
    }

    this->m_cellHeight = rasterSize;

    float baseline = static_cast<float>(rasterSize * face->ascender) / units;
    this->m_baseline = static_cast<uint32_t>(baseline + 0.5);

    if (this->m_fontFlags & 8) {
        this->m_cellHeight = rasterSize + 4;
    } else if (this->m_fontFlags & 1) {
        this->m_cellHeight = rasterSize + 2;
    }

    int32_t result = FT_Set_Pixel_Sizes(face, rasterSize, 0) == 0;

    this->m_pixelsPerUnit = static_cast<float>(face->size->metrics.x_ppem) / static_cast<float>(face->units_per_EM);

    return result;
}

// VERIFY: c831afcc auto
// OFFSET: 0x006C2480
bool CGxFont::GetGlyphData(GLYPHBITMAPDATA* data, uint32_t code) {
    FT_Face face = FontFaceGetFace(this->m_fontFaceHandle);
    FT_Set_Pixel_Sizes(face, this->m_rasterSize, 0);

    uint32_t padding = 0;

    if (this->m_fontFlags & 8) {
        padding = 4;
    } else if (this->m_fontFlags & 1) {
        padding = 2;
    }

    return IGxuFontGlyphRenderGlyph(face, this->m_rasterSize, code, this->m_baseline, data, this->m_fontFlags & 2, padding);
}

// VERIFY: 3597e899 auto
// OFFSET: 0x006C24F0
float CGxFont::GetGlyphBearing(const CHARCODEDESC* desc, bool scaled, float height) {
    // m_glyphBearing is measured in raster pixels, so the "unscaled" branch has to convert it the same
    // way the scaled branch does. It returns ceil() of a raw bitmap bearing, which used to equal screen
    // pixels because glyphs were rasterized at their drawn size; with supersampling it would be 2x too
    // far right. The unscaled path is the billboarded/world-text one (m_flags & 0x80).
    if (!scaled) {
        float bearing = desc->m_bitmapData.m_glyphBearing;
        float rasterSize = static_cast<float>(this->m_rasterSize);

        if (rasterSize > 0.0f) {
            bearing *= static_cast<float>(this->m_pixelSize) / rasterSize;
        }

        return ceil(bearing);
    }

    float pixelHeight = ScreenToPixelHeight(1, height);
    float scale = pixelHeight / this->m_rasterSize;

    return scale * desc->m_bitmapData.m_glyphBearing;
}

// VERIFY: 29412482 manual
// OFFSET: 0x006C2600
void CGxFont::RegisterEvictNotice(uint32_t texturePage) {
    for (CGxString* string = this->m_strings.Head(); string; string = this->m_strings.Next(string)) {
        string->TexturePageEvicted(texturePage);
    }
}

// VERIFY: 22997850 auto
// OFFSET: 0x006C29A0
int32_t CGxFont::CheckStringGlyphs(const char* string) {
    while (string && *string) {
        int32_t advance;
        uint32_t code = sgetu8(reinterpret_cast<const uint8_t*>(string), &advance);
        string += advance;

        if (code != '\r' && code != '\n' && !this->m_activeCharacters.Ptr(code, HASHKEY_NONE())) {
            return 0;
        }
    }

    return 1;
}

// VERIFY: f8f5bfc8 auto
// OFFSET: 0x006C2A00
float CGxFont::GetGlyphAdvance(uint32_t code) {
    CHARCODEDESC* desc = this->m_activeCharacters.Ptr(code, HASHKEY_NONE());

    if (!desc) {
        return 0.0f;
    }

    float advance = desc->m_bitmapData.m_glyphAdvance;

    if (this->m_fontFlags & 8) {
        advance += 1.0;
    }

    return advance;
}

// VERIFY: a8a8859d manual
// OFFSET: 0x006C3710
void CGxFont::ClearGlyphs() {
    for (uint32_t i = 0; i < 8; i++) {
        this->m_textureCache[i].Destroy();
    }

    this->m_activeCharacters.Clear();
    this->m_activeCharacterCache.Clear();
    this->m_kernInfo.Clear();
}

// VERIFY: 9be95581 manual
// OFFSET: 0x006C3960
void CGxFont::HandleScreenSizeChange() {
    this->ClearGlyphs();
    this->UpdateDimensions();

    for (CGxString* string = this->m_strings.Head(); string; string = this->m_strings.Next(string)) {
        string->HandleScreenSizeChange();
    }
}

// VERIFY: 4ab2e948 manual
// OFFSET: 0x006C39A0
CGxFont::CGxFont() : m_cellHeight(0), m_pixelsPerUnit(0.0f), m_pixelSize(0), m_rasterSize(0) {
}

// VERIFY: b72018df manual
// OFFSET: 0x006C3B20
CGxFont::~CGxFont() {
    if (this->m_fontFaceHandle) {
        FontFaceCloseHandle(this->m_fontFaceHandle);
    }

    this->m_fontFaceHandle = nullptr;
    this->ClearGlyphs();
}

// VERIFY: 4d729130 auto
// OFFSET: 0x006C3CB0
int32_t CGxFont::Initialize(const char* name, uint32_t fontFlags, float fontHeight) {
    SStrPrintf(this->m_fontName, sizeof(this->m_fontName), "%s", name);
    this->m_fontHeight = fontHeight;

    if (this->m_fontFaceHandle) {
        FontFaceCloseHandle(this->m_fontFaceHandle);
    }

    this->m_fontFaceHandle = nullptr;
    this->ClearGlyphs();
    this->m_fontFlags = fontFlags;

    if (this->m_fontFaceHandle) {
        FontFaceCloseHandle(this->m_fontFaceHandle);
    }

    this->m_fontFaceHandle = FontFaceGetHandle(name, g_FTLibrary);

    if (!this->m_fontFaceHandle) {
        return 0;
    }

    return this->UpdateDimensions();
}

// VERIFY: 249ead28 manual
// OFFSET: 0x006C3D40
float CGxFont::ComputeStep(uint32_t code, uint32_t nextCode) {
    KERNINGHASHKEY key;
    key.m_code = (code << 16) | (nextCode & 0xFFFF);

    KERNNODE* node = this->m_kernInfo.Ptr(code, key);

    if (node && (node->m_flags & 2)) {
        return node->m_proportionalSpacing;
    }

    FT_Face face = FontFaceGetFace(this->m_fontFaceHandle);
    FT_UInt leftIndex = FT_Get_Char_Index(face, code);
    FT_UInt rightIndex = FT_Get_Char_Index(face, nextCode);

    FT_Vector kerning;
    kerning.x = 0;

    if (FT_HAS_KERNING(face)) {
        FT_Get_Kerning(face, leftIndex, rightIndex, ft_kerning_unscaled, &kerning);

        if (kerning.x >= 0) {
            kerning.x = 0;
        }
    }

    float step = this->GetGlyphAdvance(code);
    step = this->m_pixelsPerUnit * kerning.x + step;

    if (!node) {
        node = this->m_kernInfo.New(code, key, 0, 0);
    }

    node->m_flags |= 2;

    // NOT IN BINARY behaviour change: no ceil() when supersampling. The cached spacing is in RASTER
    // pixels but is later multiplied by scale (~0.5) to reach screen pixels, so ceil() there rounded every
    // advance up to a whole raster pixel -- two whole screen pixels at 2x. Each glyph then jumped that far
    // and the string visibly drifted against the unhinted bearing/width. Keeping the exact advance lets the
    // fractional part ride along in pos.x, which is what subpixel positioning is supposed to do.
    node->m_proportionalSpacing = (this->m_rasterSize > this->m_pixelSize) ? step : ceil(step);

    return node->m_proportionalSpacing;
}

// VERIFY: 960b7c8a manual
// OFFSET: 0x006C3E80
float CGxFont::ComputeStepFixedWidth(uint32_t code, uint32_t nextCode) {
    KERNINGHASHKEY key;
    key.m_code = (code << 16) | (nextCode & 0xFFFF);

    KERNNODE* node = this->m_kernInfo.Ptr(code, key);

    if (node && (node->m_flags & 1)) {
        return node->m_fixedWidthSpacing;
    }

    if (!this->m_activeCharacters.Ptr(code, HASHKEY_NONE())) {
        if (!node) {
            node = this->m_kernInfo.New(code, key, 0, 0);
        }

        node->m_flags |= 1;
        node->m_fixedWidthSpacing = 0.0f;

        return 0.0f;
    }

    uint32_t advance = static_cast<uint32_t>(this->GetGlyphAdvance(code));

    if (advance >= this->m_cellHeight) {
        return this->m_cellHeight;
    }

    uint32_t padding = this->m_cellHeight - advance;

    if (padding & 1) {
        padding--;
    }

    uint32_t step = this->m_cellHeight - padding / 2;

    uint32_t nextAdvance = static_cast<uint32_t>(this->GetGlyphAdvance(nextCode));

    if (nextAdvance >= this->m_cellHeight) {
        return step;
    }

    uint32_t nextPadding = this->m_cellHeight - nextAdvance;

    if (nextPadding & 1) {
        nextPadding--;
    }

    return step + nextPadding / 2;
}

// VERIFY: 141d931e manual
// OFFSET: 0x006C3FC0
CHARCODEDESC* CGxFont::NewCodeDesc(uint32_t code) {
    CHARCODEDESC* desc = this->m_activeCharacters.Ptr(code, HASHKEY_NONE());

    if (desc) {
        this->m_activeCharacterCache.LinkToHead(desc);
        return desc;
    }

    GLYPHBITMAPDATA data;

    if (!this->GetGlyphData(&data, code)) {
        return nullptr;
    }

    TEXTURECACHE* cache = nullptr;
    uint32_t textureNumber;

    for (textureNumber = 0; textureNumber < 8; textureNumber++) {
        TEXTURECACHE* current = &this->m_textureCache[textureNumber];

        if (!current->m_texture || !TextureGetGxTex(current->m_texture, 1, nullptr)) {
            cache = current;
            // Initialize first: CreateTexture picks its filter from m_theFace's raster/drawn sizes, which
            // Initialize is what assigns.
            cache->Initialize(this, this->m_cellHeight);
            cache->CreateTexture(this->m_fontFlags & 4);
            desc = cache->AllocateNewGlyph(&data);
            break;
        }

        desc = current->AllocateNewGlyph(&data);

        if (desc) {
            cache = current;
            break;
        }
    }

    if (desc) {
        desc->m_textureNumber = textureNumber;
    } else {
        CHARCODEDESC* oldest = this->m_activeCharacterCache.Tail();

        if (oldest) {
            textureNumber = oldest->m_textureNumber;
            uint32_t rowNumber = oldest->m_rowNumber;
            cache = &this->m_textureCache[textureNumber];
            TEXTURECACHEROW* row = &cache->m_textureRows.m_data[rowNumber];

            row->EvictGlyph(oldest);
            this->RegisterEvictNotice(textureNumber);

            desc = row->CreateNewDesc(&data, rowNumber, this->m_cellHeight);

            if (desc) {
                desc->m_rowNumber = rowNumber;
                desc->m_textureNumber = textureNumber;
            }
        }
    }

    if (desc) {
        this->m_activeCharacters.Insert(desc, code, HASHKEY_NONE());
        this->m_activeCharacterCache.LinkToHead(desc);

        if (cache) {
            cache->m_anyDirtyGlyphs = 1;
        }
    }

    return desc;
}
