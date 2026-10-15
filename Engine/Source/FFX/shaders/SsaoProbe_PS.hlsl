sampler2D s_depth : register(s0);

// Debug view of the reconstruction: the recovered view depth, quantised into steps so it can be read.
// c0: w = projection c2 (the z scale). c1: x = 2 steps left unused, y = projection d2 (the z bias).
float4 g_ao   : register(c0);
float4 g_proj : register(c1);

float4 main(float2 uv : TEXCOORD0) : COLOR
{
    float d = tex2D(s_depth, uv).x;
    float z = g_proj.y / (d - g_ao.w);

    // 16 steps over [0, 300) world units. Near geometry is dark, far is light.
    float s = saturate(z / 300.0);
    s = floor(s * 16.0) / 16.0;

    return float4(s, s, s, 1.0);
}
