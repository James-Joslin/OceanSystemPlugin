float2 WorldXY =
MaskWorldOrigin.xy
+ UV.xy * MaskWorldSize.xy;

float2 Delta = WorldXY - ShipCenter.xy;

float2 Forward = normalize(ShipForward.xy);
float2 Right = normalize(ShipRight.xy);

float2 LocalP = float2(
    dot(Delta, Forward),
    dot(Delta, Right)
);

// ------------------------------------------------------------
// Rounded-box signed distance
// ShapeParams0:
//   X = capsule half length
//   Y = capsule radius
//   Z = rounded-box half extent X
//   W = rounded-box half extent Y
//
// ShapeParams1:
//   X = rounded-box corner radius
//   Y = interior fade depth
//   Z = shape type: 0 box, 1 capsule
//   W = enabled
// ------------------------------------------------------------

float CornerRadius = max(ShapeParams1.x, 0.0);

float2 BoxHalfExtents = max(
    ShapeParams0.zw,
    float2(1.0, 1.0)
);

float2 BoxQ =
abs(LocalP)
- BoxHalfExtents
+ CornerRadius;

float DBox =
length(max(BoxQ, 0.0))
+ min(max(BoxQ.x, BoxQ.y), 0.0)
- CornerRadius;

// ------------------------------------------------------------
// Capsule signed distance
// ------------------------------------------------------------

float CapsuleHalfLength = max(ShapeParams0.x, 0.0);
float CapsuleRadius = max(ShapeParams0.y, 1.0);

float2 CapsuleQ = LocalP;

CapsuleQ.x -= clamp(
    CapsuleQ.x,
    -CapsuleHalfLength,
    CapsuleHalfLength
);

float DCapsule =
length(CapsuleQ)
- CapsuleRadius;

// Select shape.
float ShapeSelection = step(0.5, ShapeParams1.z);
float D = lerp(DBox, DCapsule, ShapeSelection);

// Fade only toward the interior.
float FadeDepth = max(ShapeParams1.y, 1.0);

float Influence =
1.0
- smoothstep(-FadeDepth, 0.0, D);

return saturate(
    Influence * ShapeParams1.w
);