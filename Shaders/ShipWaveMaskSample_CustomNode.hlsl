// ShipWaveMaskSample Custom node body
// Output type: CMOT Float 1
//
// Inputs:
//   WorldPos             float3
//   ShipWaveMaskTexture  Texture2D
//   ShipMaskWorldOrigin  float2
//   ShipMaskWorldSize    float2
//   ShipWaveMaskEnabled   float
//
// Add a texture-object input named ShipWaveMaskTexture.
// The generated sampler name is ShipWaveMaskTextureSampler.

float2 UV = (WorldPos.xy - ShipMaskWorldOrigin) / max(ShipMaskWorldSize, float2(1.0, 1.0));

float Inside =
    step(0.0, UV.x) * step(UV.x, 1.0) *
    step(0.0, UV.y) * step(UV.y, 1.0);

float Influence = ShipWaveMaskTexture.SampleLevel(
    ShipWaveMaskTextureSampler,
    saturate(UV),
    0).r;

// Full waves outside the mask window. Inside, fade toward zero under ships.
float MaskedFade = lerp(1.0, 1.0 - saturate(Influence), Inside);
return lerp(1.0, MaskedFade, saturate(ShipWaveMaskEnabled));
