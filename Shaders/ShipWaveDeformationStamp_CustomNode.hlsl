// Signed deformation stamp. Uses the same hull inputs as ShipWaveMaskStamp.
// DeformationParams: X=interior Z, Y=bow Z, Z=side XY, W=foam.
// ShapeParams0: X=capsule half length, Y=capsule radius,
//               ZW=rounded-box half extents.
// ShapeParams1: X=box corner radius, Y=fade/band width,
//               Z=shape (0 box, 1 capsule), W=enabled.
float2 WorldXY = MaskWorldOrigin.xy + UV.xy * MaskWorldSize.xy;
float2 Delta = WorldXY - ShipCenter.xy;
float2 Forward = normalize(ShipForward.xy);
float2 Right = normalize(ShipRight.xy);
float2 LocalP = float2(dot(Delta, Forward), dot(Delta, Right));

float CornerRadius = max(ShapeParams1.x, 0.0);
float2 HalfExtents = max(ShapeParams0.zw, float2(1.0, 1.0));
float2 Q = abs(LocalP) - HalfExtents + CornerRadius;
float BoxDistance =
    length(max(Q, 0.0)) + min(max(Q.x, Q.y), 0.0) - CornerRadius;

float CapsuleHalfLength = max(ShapeParams0.x, 0.0);
float CapsuleRadius = max(ShapeParams0.y, 1.0);
float2 CapsuleQ = LocalP;
CapsuleQ.x -= clamp(CapsuleQ.x, -CapsuleHalfLength, CapsuleHalfLength);
float CapsuleDistance = length(CapsuleQ) - CapsuleRadius;

float IsCapsule = step(0.5, ShapeParams1.z);
float Distance = lerp(BoxDistance, CapsuleDistance, IsCapsule);
float BandWidth = max(ShapeParams1.y, 1.0);

// Negative distance is inside the hull. Keep the optional interior offset
// inside, but put the bow, side push, and foam in a band around/outside the
// boundary. The water material's WaveFade safety gate remains the final
// anti-clipping guard.
float InteriorFalloff = 1.0 - smoothstep(-BandWidth, 0.0, Distance);
float BoundaryRise = smoothstep(-BandWidth * 0.25, 0.0, Distance);
float ExteriorDecay = 1.0 - smoothstep(0.0, BandWidth, Distance);
float ExteriorBand = BoundaryRise * ExteriorDecay;

float FrontExtent = lerp(
    HalfExtents.x,
    CapsuleHalfLength + CapsuleRadius,
    IsCapsule);
float SideExtent = lerp(HalfExtents.y, CapsuleRadius, IsCapsule);
float BowRegion = saturate(
    (LocalP.x - (FrontExtent - BandWidth)) / BandWidth);
float SideRegion = saturate(
    (abs(LocalP.y) - (SideExtent - BandWidth)) / BandWidth);
float Bow = BowRegion * ExteriorBand;
float Side = SideRegion * ExteriorBand;

float SideSign = sign(LocalP.y);
float3 Offset = float3(
    Right.x * SideSign * DeformationParams.z * Side,
    Right.y * SideSign * DeformationParams.z * Side,
    DeformationParams.x * InteriorFalloff + DeformationParams.y * Bow);

return float4(Offset.z, Offset.x, Offset.y, DeformationParams.w * max(Bow, Side))
    * ShapeParams1.w;
