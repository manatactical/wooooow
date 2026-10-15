sampler2D s_depth : register(s0);

// c0: x radius (view units), y bias, z intensity, w projection c2 (the z scale)
// c1: x projection d2 (the z bias), y projection a0, z projection b1
//
// These four come from the device's *native* projection (m_projNative), the one D3D rasterises with, not the
// engine-space m_projection: the depth buffer holds the native form, whose z runs 0..1 as D3D9 always stores,
// while m_projection keeps the -1..1 form. Mixing the two scales view z by two.
//
// a0/b1 are the view-to-clip scales (x_clip = a0*x, w_clip = z), so a world offset at depth z spans
// 0.5*a0/z in u (the 0.5 is clip -1..1 -> texture 0..1). That is what turns the world radius into a texture
// offset below. Feeding the texel size in instead -- which is what this pass did at first -- makes the radius
// about a thousand times too small at 1920 wide: every tap lands on the centre texel, the occlusion comes out a
// flat 1.0, and the effect runs while changing nothing on screen.
float4 g_ao   : register(c0);
float4 g_proj : register(c1);

static const float2 k_kernel[16] = {
    float2( 0.2032,  0.1174), float2( 0.6178,  0.1553),
    float2( 0.3501,  0.5586), float2(-0.1093,  0.4029),
    float2(-0.5058,  0.2530), float2(-0.7021, -0.1589),
    float2(-0.4084, -0.5017), float2(-0.0552, -0.7043),
    float2( 0.3018, -0.5561), float2( 0.6551, -0.4072),
    float2( 0.8061,  0.3502), float2( 0.1533,  0.8504),
    float2(-0.3052,  0.8073), float2(-0.8512,  0.3541),
    float2(-0.9024, -0.5017), float2( 0.5024,  0.7545),
};

// View z from the depth buffer. The native projection writes z_clip = c2*z + d2 with w = z, and D3D9 stores
// z_stored = z_clip/z in 0..1, so the buffer holds z_clip/z directly and z = d2 / (depth - c2). d2 is negative,
// so this is positive for depth in (0, c2).
float ViewZ(float d)
{
    return g_proj.x / (d - g_ao.w);
}

float4 main(float2 uv : TEXCOORD0) : COLOR
{
    float radius = g_ao.x;
    float bias = g_ao.y;
    float intensity = g_ao.z;

    float z0 = ViewZ(tex2D(s_depth, uv).x);

    // Per-pixel rotation so the 16 taps do not band into a fixed pattern.
    float2 rv = frac(uv * 71.37) * 2.0 - 1.0;
    rv /= max(length(rv), 0.03125);
    float2x2 rot = float2x2(rv.x, -rv.y, rv.y, rv.x);

    // A view-space sample at depth z0 moves this far on screen, so the kernel keeps a world-sized radius. The
    // cap keeps a large CVar (the radius is in world units, and a metre-scale one at close range would push the
    // taps right off the frame) from turning the pass into a blur of half the screen.
    float2 base = radius * g_proj.yz / (2.0 * z0);
    float baseLen = length(base);
    if (baseLen > 0.03)
    {
        base *= 0.03 / baseLen;
    }

    float occl = 0.0;

    for (int i = 0; i < 16; i++)
    {
        float2 k = mul(k_kernel[i], rot) * base;
        float zs = ViewZ(tex2D(s_depth, uv + k).x);
        // Only samples in front of the surface occlude it, and their weight fades with the distance so a
        // far surface cannot darken everything.
        float range = saturate(radius / max(abs(z0 - zs), 0.0001));
        occl += ((zs < z0 - bias) ? 1.0 : 0.0) * range;
    }

    float ao = saturate(1.0 - (occl / 16.0) * intensity);
    return float4(ao, ao, ao, 1.0);
}
