#ifndef WOW_SOURCE_WORLDCLIENT_SKYCAPTURE_H
#define WOW_SOURCE_WORLDCLIENT_SKYCAPTURE_H

class CGxTex;
class CPortalView;

// Not in the original binary. A small picture of the sky as the player actually sees it -- the dome gradient, the
// custom skybox model, the stars and the sun and moon -- which the modern water reflects instead of the zone's light
// bands. The bands describe only the stock gradient; a custom skybox draws over it, so reflecting the bands gave
// orange water under a blue night.
//
// The picture is four narrow vertical strips side by side in a 64x64 target, one per compass direction, each looking
// 45 degrees up with a 90 degree vertical field of view, so a strip's rows run from the horizon (bottom) to the zenith
// (top). The water averages the four strips at the height of its reflected ray, which removes the sun and moon.
namespace SkyCapture {

constexpr int kTileCount = 4;
constexpr int kTileWidth = 16;
constexpr int kTileHeight = 64;

// Redraws the sky into the capture target every few frames. Called right after the world's own sky, so the sky state
// (skybox weights, animation time) is the one the frame is drawn with. Leaves every piece of device state it touches
// as it found it.
void Update(CPortalView* view);

// The capture as a gx texture, or null when the sky is not drawn this frame (indoors, no sky) or the target could not
// be made -- the caller then falls back to the light bands.
CGxTex* Texture();

}  // namespace SkyCapture

#endif
