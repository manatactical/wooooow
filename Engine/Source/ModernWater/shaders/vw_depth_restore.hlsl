#include "vf_common.hlsli"

sampler2D sDepth : register(s0);

struct Output
{
    float4 colour : COLOR0;
    float depth : DEPTH;
};

// Writes the raw depth copy back into the bound depth surface. Colour writes are masked off by the caller, so the
// frame is untouched and only the depth buffer changes.
Output main(float2 pixelIndex : VPOS)
{
    Output output;
    output.colour = 0;
    output.depth = SampleDepth(sDepth, pixelIndex + 0.5);
    return output;
}
