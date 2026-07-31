// Output: float4 packed as (signed Z, signed X, signed Y, foam).
// Custom node Details -> Include File Paths must contain:
//     /OceanSystem/ShipWaveField.ush
// Do not put a literal #include in the Custom node Code box.
return SampleShipDeformationField(
    ShipWaveDeformationTexture,
    ShipWaveDeformationTextureSampler,
    WorldPos,
    ShipFieldWorldOrigin,
    ShipFieldWorldSize,
    ShipWaveFieldEnabled);
