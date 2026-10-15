sampler2D s_ao : register(s0);

float4 g_db : register(c1);

// Diagnostics. All three steps are mathematically identical to returning the occlusion, so anything odd on
// screen can only come from the reconstruction, not from this shader.
//  1 the occlusion, with a stripe of the unblurred value across the middle and a bright band near the top
//  2 a constant, to check the multiply itself
//  3 the reconstruction divided into measured bands, for reading g_proj/g_ao off the screen
float4 main(float2 uv0 : TEXCOORD0, float2 uv1 : TEXCOORD1, float2 uv2 : TEXCOORD2, float2 uv3 : TEXCOORD3) : COLOR
{
    int mode = (int)g_db.x;

    float a0 = tex2D(s_ao, uv0).x;
    float a1 = tex2D(s_ao, uv1).x;
    float a2 = tex2D(s_ao, uv2).x;
    float a3 = tex2D(s_ao, uv3).x;

    float ao = (a0 + a1 + a2 + a3) * 0.25;

    if (mode == 2)
    {
        ao = 1.0;
    }
    else if (mode == 1)
    {
        ao = a0;
        if (uv0.y > 0.46 && uv0.y < 0.54)
        {
            ao = 0.0;
        }
        if (uv0.y > 0.70 && uv0.y < 0.72)
        {
            ao = 1.0;
        }
    }
    else if (mode == 3)
    {
        ao = g_db.w;
    }

    return float4(ao, ao, ao, 1.0);
}
