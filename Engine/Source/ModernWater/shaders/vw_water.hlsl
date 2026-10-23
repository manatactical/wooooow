#include "vf_common.hlsli"
#include "vw_constants.hlsli"
#include "vw_surface.hlsli"
#include "vw_optics.hlsli"
#include "vw_reflection.hlsli"

static const float kDebugNormal = 1;
static const float kDebugFoamCoverage = 2;
static const float kDebugTransmittance = 3;
static const float kDebugReflection = 4;
static const float kMinSceneVisibility = 0.05;
static const float kDebugLiquidClass = 5;
static const float kDebugRippleHeightGain = 4;
static const float kDebugRippleWindowShade = 0.25;

float3 ClassColour(float waterClass)
{
    float3 primaries = saturate(1 - abs(waterClass - float3(1, 2, 3)));
    return primaries + saturate(1 - abs(waterClass - 4)) * float3(1, 1, 0);
}

float3 RippleColour(RippleSample ripples)
{
    float height = 0.5 + 0.5 * clamp(ripples.height * kDebugRippleHeightGain, -1, 1);
    return (height * lerp(1 - kDebugRippleWindowShade, 1, ripples.coverage)).xxx;
}

float3 DebugColour(float3 N, float4 foamAlbedo, float3 transmittance, float3 reflected, RippleSample ripples)
{
    float view = WaterDebugView();
    if (view < kDebugNormal + 0.5)
        return N * 0.5 + 0.5;
    if (view < kDebugFoamCoverage + 0.5)
        return foamAlbedo.aaa;
    if (view < kDebugTransmittance + 0.5)
        return transmittance;
    if (view < kDebugReflection + 0.5)
        return LinearToGamma(reflected);
    if (view < kDebugLiquidClass + 0.5)
        return ClassColour(WaterClassIndex());
    return RippleColour(ripples);
}

float4 main(float2 pixelIndex : VPOS) : COLOR0
{
    float2 pixel = pixelIndex + 0.5;
    WaterPixel w = ReconstructWaterPixel(pixel);
    RippleSample ripples = SampleRipples(w);
    WaveState waves = SampleWaves(w, ripples.slope);
    float4 foamAlbedo = FoamAlbedo(w, waves);

    float3 N = normalize(float3(-waves.moments.xy, 1));
    float3 V = w.toCamera;
    float3 L = ToLight();
    float NoV = clamp(dot(N, V), kMinCosine, 1);
    float variance = SlopeVariance(waves.moments);
    float envRoughness = PerceptualRoughness(lerp(EnvironmentRoughnessParameter(), kFoamRoughness, foamAlbedo.a),
                                             variance);
    float sunRoughness = PerceptualRoughness(lerp(SunRoughnessParameter(), kFoamRoughness, foamAlbedo.a), variance);

    Refraction refraction = RefractScene(w, waves.moments.xy);
    float3 transmittance = Transmittance(refraction.pathDepth);
    float3 inscatter = InScattering(V, NoV, CrestTilt(waves.moments), waves.foam.y, transmittance);
    // The fog is at most 90% opaque: however deep the water, a tenth of what is under it still shows through.
    float3 sceneVisibility = lerp(kMinSceneVisibility, 1, transmittance);
    float3 water = sceneVisibility * refraction.scene + inscatter;
    float3 foam = FoamLight(N, waves.foam.x) * foamAlbedo.rgb + inscatter;
    float3 colour = lerp(water, foam, foamAlbedo.a);

    float3 R = ReflectedDirection(V, N);
    float2 hitUv;
    float4 screenReflection = TraceScreenReflection(w, pixel, R, hitUv);
    float3 sky = 0;
    [branch] if (screenReflection.a < 1)
        sky = FoggedSkyReflection(w, R, SkyColour(R.z));
    [branch] if (screenReflection.a > 0)
        screenReflection.rgb = FoggedScreenReflection(w, R, screenReflection.rgb, hitUv);
    float3 reflected = lerp(sky, screenReflection.rgb, screenReflection.a);
    float3 environment = reflected * EnvironmentBrdf(envRoughness, NoV) * ReflectionStrength();
    float3 H = normalize(V + L);
    float environmentAttenuation = 1 - FresnelSchlick(dot(H, V)) * SunVisibility();
    float3 sunSpecular = SunVisibility() * SunColour() * SpecularStrength() * SunGlint(sunRoughness, N, V, L);
    colour += sunSpecular + environmentAttenuation * environment;

    // Self-lit surfaces. Zero for every water class, so the line above is all that water does. Lava is drawn with
    // it at full strength and takes its glow from its own scattering colours modulated by the wave normals, which
    // is the only per-pixel variation this shader has for a surface with no foam masks and no transmittance.
    [branch] if (EmissiveStrength() > 0.5)
        colour = lerp(TroughScatteringColour(), CrestScatteringColour(), saturate(N.z)) * EmissiveStrength();

    // Uncovered pixels leave the frame alone by blending to zero rather than by painting the scene copy back.
    //
    // Returning sSceneColour here looked equivalent and is not: that copy was taken before the liquid pass, so it is
    // the frame *without* the client's water. Painting it back erased the client's own water on every pixel this pass
    // failed to claim, which is most of them while the coverage tolerance is still being tuned. Blending with coverage
    // as the alpha leaves whatever is on the target untouched and replaces only the pixels actually shaded.
    float3 encoded = LinearToGamma(colour);
    [branch] if (StockFogApplies())
        encoded = lerp(StockFogColour(), encoded, StockFogVisibility(w.waterZ));
    encoded = lerp(refraction.copied, encoded, saturate(kShoreFadePerYard * refraction.pathDepth));

    // Pixels this pass does not own are discarded rather than blended with zero alpha, so an occlusion query around the
    // draw counts exactly the pixels that were shaded.
    clip(w.coverage - 0.003);
    return float4(encoded, w.coverage);
}
