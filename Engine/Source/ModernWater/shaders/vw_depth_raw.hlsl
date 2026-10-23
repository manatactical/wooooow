#include "vf_common.hlsli"

sampler2D sDepth : register(s0);

// The shared depth exactly as stored, unlinearised, so it can be written back after the water has drawn into it.
float4 main(float2 copyTexel : VPOS) : COLOR0
{
    float2 pixel = ViewportOrigin() + copyTexel + 0.5;
    return SampleDepth(sDepth, pixel);
}
