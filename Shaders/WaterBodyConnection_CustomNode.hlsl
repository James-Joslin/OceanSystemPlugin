// Inputs mirror EvaluateConnectedWaterWPO in WaterBodyConnection.ush.
// Output type: float3 (World Position Offset).
// Custom node Details -> Include File Paths must contain:
//     /OceanSystem/WaterBodyConnection.ush
// Do not put a literal #include in the Custom node Code box.

float Alpha = ComputeWaterConnectionAlpha(
    PreWPOWorldPos,
    ConnectionWorldStart,
    ConnectionWorldDirection,
    ConnectionBlendLength);
Alpha *= saturate(ConnectionEnabled);

float3 Result;
EvaluateConnectedWaterWPO(
    PreWPOWorldPos, Alpha, WaveFade, ShipDeformation,
    SourceWaveTime, SourceWaveCount, SourceWaveDataTexture,
    SourceDomainWarpFrequency, SourceDomainWarpAmount, SourceCrestSharpness,
    TargetWaveTime, TargetWaveCount, TargetWaveDataTexture, TargetBaseZ,
    TargetDomainWarpFrequency, TargetDomainWarpAmount, TargetCrestSharpness,
    Choppiness, SmallWaveChop, MaxHorizontal, Result);
return Result;
