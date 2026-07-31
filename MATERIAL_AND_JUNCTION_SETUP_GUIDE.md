# Beginner Step-by-Step Water Junction Material Guide

## What this guide will build

You already have a working ship mask in `M_OceanBase`. Keep it.

By the end of this guide you will have:

1. The same ship mask working on rivers.
2. An optional advanced deformation texture for bow rise, side push, and foam.
3. A junction material that blends river waves into ocean/lake waves.
4. A river spline endpoint connected to a large water body without overlapping Single Layer Water meshes.

Work through the stages in order. Compile and save after every stage. Do not build the entire graph before testing it.

## The simple mental model

There are three different jobs:

```text
Ship mask
  Keeps water away from the visible hull.
  Produces WaveFade: 0 inside the ship, 1 outside it.

Advanced deformation
  Raises water around the bow, pushes it away from the sides,
  and provides foam. It does not create a trailing wake.

River junction
  Owns the geometry between the river and ocean/lake,
  and blends their two wave definitions.
```

The mask always has final authority inside a hull. Advanced deformation must never lift water back through the ship.

---

# Stage 1 — Back up the material assets

In the Unreal Content Browser, find:

```text
Content/Level_Building/Water/
```

Duplicate these assets before editing them:

```text
M_OceanBase
M_RiverBase
M_ShipWaveMaskStamp
```

Give the backups obvious names such as:

```text
M_OceanBase_BACKUP
M_RiverBase_BACKUP
M_ShipWaveMaskStamp_BACKUP
```

Do not edit the backups.

---

# Stage 2 — Check the existing OceanBase mask

Open `M_OceanBase`.

Find the group from your screenshot containing:

```text
ShipWaveMaskTexture
ShipMaskWorldOrigin
ShipMaskWorldSize
ShipWaveMaskEnabled
Absolute World Position
the ship-mask Custom node
```

## Step 2.1 — Check World Position

Select the World Position node. Set its mode to:

```text
Absolute World Position (Excluding Material Shader Offsets)
```

This prevents the mask from moving when WPO moves the water vertices.

## Step 2.2 — Identify WaveFade

Your Custom node returns:

```text
0 inside the ship
1 outside the ship
```

Add a comment around this group and name it:

```text
SHIP MASK — OUTPUT IS WAVEFADE
```

The output must feed the `WaveFade` input of your Gerstner displacement code.

Keep your existing depression code:

```hlsl
float HullInfluence = 1.0 - saturate(WaveFade);
float DepressionShape = HullInfluence * HullInfluence;
Disp.z -= DepressionShape * max(HullDepressionDepth, 0.0);
```

Do not connect `WaveFade` to Single Layer Water opacity.

## Step 2.3 — Test

Click **Apply**, then **Save**.

Run your current ship test. Do not continue until:

- Waves disappear underneath the ship.
- Water remains depressed under the hull.
- The water does not clip visibly through the hull.

---

# Stage 3 — Put the same mask in RiverBase

Open both `M_OceanBase` and `M_RiverBase`.

In `M_OceanBase`, select the complete ship-mask group from Stage 2. Copy it and paste it into `M_RiverBase`.

Do not rename its parameters. They must remain exactly:

```text
ShipWaveMaskTexture
ShipMaskWorldOrigin
ShipMaskWorldSize
ShipWaveMaskEnabled
```

Connect its output to the river Gerstner Custom node's `WaveFade` input.

Add the same hull-depression code to the river WPO path if it is not already present.

Click **Apply** and **Save**.

At this point the river and ocean both understand the same mask. The connected-water subsystem will give them the same render target when they belong to one junction network.

---

# Stage 4 — Create the advanced deformation stamp

This stage adds bow rise, side push, and foam. Your existing mask continues to provide under-hull depression.

## Step 4.1 — Create the material

In `Content/Level_Building/Water/`:

1. Right-click in empty space.
2. Choose **Material**.
3. Name it `M_ShipWaveDeformationStamp`.
4. Open it.

In the material Details panel set:

```text
Material Domain: Surface
Blend Mode: Additive
Shading Model: Unlit
```

## Step 4.2 — Create the parameters

For each item below, right-click in the graph, create a **Vector Parameter**, and give it the exact name shown:

```text
MaskWorldOrigin
MaskWorldSize
ShipCenter
ShipForward
ShipRight
ShapeParams0
ShapeParams1
DeformationParams
```

The runtime subsystem fills these values. Their editor defaults are not important.

## Step 4.3 — Create the Custom node

1. Right-click and create a **Custom** node.
2. Select it.
3. Set **Output Type** to `CMOT Float 4`.
4. Add these Custom inputs with the exact names:

```text
UV
MaskWorldOrigin
MaskWorldSize
ShipCenter
ShipForward
ShipRight
ShapeParams0
ShapeParams1
DeformationParams
```

5. Open this plugin file in your text editor:

```text
Shaders/ShipWaveDeformationStamp_CustomNode.hlsl
```

6. Copy all of its code into the Custom node's **Code** box.
7. Create a **TextureCoordinate** node and connect it to `UV`.
8. Connect every matching Vector Parameter to the Custom input with the same name.

### Important: use the RGBA output pin

Vector Parameter nodes expose several output pins. Connect the bottom white **RGBA** pin, not the upper **RGB** pin, to the Custom node.

This is mandatory for:

```text
ShapeParams0
ShapeParams1
DeformationParams
```

The stamp HLSL reads `.w`/alpha from these values. If RGB is connected, Unreal infers a three-component `float3` and reports errors such as:

```text
vector swizzle 'w' is out of bounds
vector swizzle 'zw' is out of bounds
```

For simplicity, use the RGBA output for all eight Vector Parameters in this stamp graph. The code will take `.xy` where it only needs two components.

## Step 4.4 — Connect the material output

Create two Component Mask nodes from the Custom output:

```text
RGB mask -> Emissive Color
A mask   -> Opacity
```

Click **Apply** and **Save**.

## Important first setting

Your existing mask already depresses the water. On every `ShipWaveExclusionComponent`, start with:

```text
InteriorVerticalOffset = 0
```

Otherwise the mask and deformation texture can both depress the surface.

The stamp backup supports both **Rounded Box** and **Capsule** and uses the same `Shape` setting as the suppression mask. For a capsule, `Capsule Half Length` controls the straight centre section and `Capsule Radius` controls its width and rounded ends.

Bow rise, side push, and deformation foam are written in a band around the hull boundary. `Interior Fade Depth` is also used as that band's width. The anti-clipping gate in Stage 6 remains required: it removes positive and horizontal deformation wherever the suppression mask says the water is protected beneath the hull.

---

# Stage 5 — Tell the subsystem about the new stamp

Find the Blueprint or C++ initialization where you already configure `M_ShipWaveMaskStamp`.

You should already have the equivalent of:

```text
Get ShipWaveMaskSubsystem
  -> Configure
```

From the same subsystem reference, add:

```text
Configure Deformation Field
```

Set:

```text
Deformation Stamp Material: M_ShipWaveDeformationStamp
Field Resolution:           2048
Field World Size:           20000
```

`20000 cm` means a 200-metre square field.

Compile and save the Blueprint.

## Standalone water works without a junction

The subsystem treats every registered water body as a one-body network. A level containing only one tiled ocean or lake therefore receives `ShipWaveDeformationTexture`, `ShipFieldWorldOrigin`, `ShipFieldWorldSize`, and `ShipWaveFieldEnabled` as soon as the water body and deformation stamp material are configured.

A junction is not required to enable deformation or foam. Adding a river junction merges the river, junction MID, and target body into one connected network so those same fields remain continuous while the ship crosses the seam. Removing the junction splits the network back into standalone body networks.

---

# Stage 6 — Sample deformation in OceanBase

Open `M_OceanBase`.

## Step 6.1 — Create parameters

Create these nodes with exact names:

| Node | Name | Default |
|---|---|---|
| Texture Object Parameter | `ShipWaveDeformationTexture` | Any black texture |
| Vector Parameter | `ShipFieldWorldOrigin` | `(0,0,0,0)` |
| Vector Parameter | `ShipFieldWorldSize` | `(1,1,0,0)` |
| Scalar Parameter | `ShipWaveFieldEnabled` | `0` |

Use a **Texture Object Parameter**, not a Texture Sample Parameter.

## Step 6.2 — Create the sample Custom node

1. Create a Custom node.
2. Set its output to `CMOT Float 4`.
3. Add these inputs:

```text
ShipWaveDeformationTexture
WorldPos
ShipFieldWorldOrigin
ShipFieldWorldSize
ShipWaveFieldEnabled
```

4. In the Custom node Details panel, find **Include File Paths**.
5. Click its `+` button and enter this virtual shader path exactly:

```text
/OceanSystem/ShipWaveField.ush
```

6. Copy the code from:

```text
Shaders/ShipWaveDeformationSample_CustomNode.hlsl
```

The backup contains only the function call. Do not add a literal `#include` line to the Code box. Unreal emits paths from **Include File Paths** at global shader scope, where the `.ush` helper functions are legal; a literal include in the Code box is emitted inside Unreal's generated Custom function and creates illegal nested definitions.

7. Connect:

```text
ShipWaveDeformationTexture -> matching input
Absolute World Position (Excluding Material Shader Offsets) -> WorldPos
ShipFieldWorldOrigin RG -> matching input
ShipFieldWorldSize RG -> matching input
ShipWaveFieldEnabled -> matching input
```

The Custom output contains:

```text
R = vertical displacement
G = world-X displacement
B = world-Y displacement
A = foam
```

## Step 6.3 — Reorder it into a WPO vector

Use Component Mask and Append Vector nodes to build:

```text
ShipDeformation = float3(G, B, R)
```

In node form:

```text
Mask G ----\
            Append Vector -> float2(G,B) --\
Mask B ----/                               Append Vector -> float3(G,B,R)
Mask R -----------------------------------/
```

## Step 6.4 — Add the anti-clipping gate

Create another Custom node:

```text
Output Type: CMOT Float 3
Inputs: ShipDeformation, WaveFade
```

Paste this code:

```hlsl
float ExteriorGate = smoothstep(0.65, 0.95, WaveFade);

float3 SafeDeformation = ShipDeformation;

SafeDeformation.z =
    min(ShipDeformation.z, 0.0)
    + max(ShipDeformation.z, 0.0) * ExteriorGate;

SafeDeformation.xy *= ExteriorGate;

return SafeDeformation;
```

Connect your existing mask output to `WaveFade`.

This guarantees:

- Positive bow water cannot rise through a hull.
- Horizontal side movement cannot move water through a hull.
- One ship's bow deformation cannot rise through another ship's combined mask.

## Step 6.5 — Add it to WPO

Your existing Gerstner Custom node already returns waves plus the mask-derived depression.

If you built the three Material Functions shown in this guide, the master-material wiring is:

```text
Ship Mask Function.WaveFade -------------------------> Gerstner.WaveFade
                 |
                 +-----------------------------------> Safety Gate.WaveFade

Deformation Sample Function.ShipDeformation --------> Safety Gate.ShipDeformation

Gerstner/depression WPO ------------------------------\
                                                       Add -> World Position Offset
Safety Gate.SafeDeformation --------------------------/

Deformation Sample Function.ShipFoam ----------------> your foam logic
```

Add the safe deformation after that output:

```text
Existing Gerstner/depression WPO --\
                                  Add -> World Position Offset
SafeDeformation ------------------/
```

### Why this is Add, not Multiply

Both inputs are displacement vectors measured in world units:

```text
Existing WPO    = movement caused by Gerstner waves and hull depression
SafeDeformation = extra movement caused by bow/side interaction
```

The total vertex movement is therefore their sum:

```hlsl
FinalWPO = ExistingWPO + SafeDeformation;
```

Multiplying would perform component-by-component multiplication, for example `ExistingWPO.x * SafeDeformation.x`. That would erase components whenever either value is zero, produce incorrect directions, and multiply world units by world units.

The required scaling has already happened inside the safety Custom node, where `ExteriorGate` multiplies only the positive vertical and horizontal advanced deformation. After that gate, use Add to combine the two independent offsets.

This assumes the existing Gerstner node returns a displacement such as `Disp`, which your current code does. If a different graph returns an absolute world position instead of an offset, do not connect that absolute position directly to this Add.

## Step 6.6 — Use ShipFoam safely

`ShipFoam` is a scalar mask, not an emissive colour. In the current Material Function it passes through without the hull safety gate. Gate it before using it:

```text
WaveFade -> SmoothStep(Min=0.65, Max=0.95) -> ExteriorGate

ShipFoam --\
            Multiply -> Saturate -> SafeShipFoam
ExteriorGate /
```

This removes ship foam from the protected hull interior. If the water material already has a Gerstner/crest foam mask, combine the masks with Max:

```text
ExistingFoam --\
               Max -> CombinedFoam
SafeShipFoam --/
```

Use Add followed by Saturate instead of Max only if overlapping effects should deliberately make the foam denser.

For a simple coloured emissive contribution, create:

```text
ShipFoamColor             Vector Parameter
ShipFoamEmissiveStrength  Scalar Parameter
```

Then wire:

```text
CombinedFoam
  * ShipFoamColor
  * ShipFoamEmissiveStrength
  + ExistingEmissive
  -> Emissive Color
```

Do not add the raw scalar directly unless plain white foam is specifically wanted. Start with a low emissive strength such as `0.1` and increase it gradually. Large values make foam look self-illuminated rather than reflective.

For less glowing and more physically readable foam, also use `CombinedFoam` to blend material properties:

```text
Base Color = Lerp(WaterColor, FoamColor, CombinedFoam)
Roughness  = Lerp(WaterRoughness, 0.85, CombinedFoam)
```

Keep the small Emissive contribution for artistic visibility if desired. Do not connect the foam mask to SLW opacity to hide or own the surface.

Click **Apply** and **Save**.

### Optional graph-only wiring test

To verify that the Add and WPO wiring works, temporarily disconnect `SafeDeformation` and connect a Constant3Vector equivalent to `(0, 0, 50)` to that Add input. The water should rise by 50 cm. Reconnect `SafeDeformation` immediately after this check.

The real sampled field also works on a standalone tiled body. Test with very small values first:

```text
BowVerticalOffset:    5 cm
SideHorizontalOffset: 2 cm
DeformationFoam:      0.25
```

Increase them only after confirming that the hull remains clear.

---

# Stage 7 — Copy deformation sampling to RiverBase

Copy the complete deformation sampling group from Stage 6 into `M_RiverBase`:

```text
deformation texture parameters
deformation sample Custom node
G/B/R reorder nodes
anti-clipping Custom node
final Add node
```

Connect the river mask output to the anti-clipping node's `WaveFade` input.

Click **Apply** and **Save**.

Before continuing, confirm that both materials compile and use the temporary constant test if needed. A standalone river or tiled body can already sample its own field; after the junction network is created in Stages 8–10, both connected surfaces should:

- Suppress waves under the hull.
- Retain the existing depression.
- Show small bow/side deformation.
- Remain clear of the visible hull.

---

# Stage 8 — Add the junction WPO path to RiverBase

This is the largest graph stage. Complete Stages 1–7 first.

## Step 8.1 — Create the Static Switch

In `M_RiverBase`:

1. Create a **Static Switch Parameter**.
2. Name it exactly `ConnectionMode`.
3. Leave its default value `false`.

Eventually it will select:

```text
False -> normal river WPO
True  -> junction WPO
```

Do not connect it yet.

## Step 8.2 — Create connection geometry parameters

Create:

| Type | Exact name |
|---|---|
| Vector Parameter | `ConnectionWorldStart` |
| Vector Parameter | `ConnectionWorldDirection` |
| Vector Parameter | `ConnectionWorldRight` |
| Scalar Parameter | `ConnectionBlendLength` |
| Scalar Parameter | `ConnectionEnabled` |
| Scalar Parameter | `ConnectionEndpoint` |

## Step 8.3 — Create source wave parameters

Create Scalar Parameters:

```text
SourceWaveCount
SourceWaveTime
SourceBaseZ
SourceCrestSharpness
SourceDomainWarpFrequency
SourceDomainWarpAmount
SourceDetailWaveCount
```

Create Texture Object Parameters:

```text
SourceWaveDataTexture
SourceDetailWaveDataTexture
```

## Step 8.4 — Create target wave parameters

Create Scalar Parameters:

```text
TargetWaveCount
TargetWaveTime
TargetBaseZ
TargetCrestSharpness
TargetDomainWarpFrequency
TargetDomainWarpAmount
TargetDetailWaveCount
```

Create Texture Object Parameters:

```text
TargetWaveDataTexture
TargetDetailWaveDataTexture
```

Give all four Texture Object Parameters a black/default texture so the material can compile before runtime binding.

## Step 8.5 — Recreate the mask-derived depression outside the existing node

The junction helper needs the depression as an input. Build this small node chain from `WaveFade`:

```text
WaveFade
  -> OneMinus
  -> Multiply by itself
  -> Multiply by HullDepressionDepth
  -> Multiply by -1
  -> append with X=0 and Y=0
  -> MaskDerivedHullDepression float3
```

Then add:

```text
MaskDerivedHullDepression + SafeDeformation = CommonShipOffset
```

## Step 8.6 — Create the connected WPO Custom node

1. Create a Custom node.
2. Set output to `CMOT Float 3`.
3. In the Custom node Details panel, expand **Include File Paths**, add an entry, and set it to:

```text
/OceanSystem/WaterBodyConnection.ush
```

This property places the helper functions at global shader scope. Do not paste a literal `#include` line into the Custom node Code box.

4. Copy the code from:

```text
Shaders/WaterBodyConnection_CustomNode.hlsl
```

5. Add these inputs exactly:

```text
PreWPOWorldPos
ConnectionWorldStart
ConnectionWorldDirection
ConnectionBlendLength
ConnectionEnabled

WaveFade
ShipDeformation

SourceWaveTime
SourceWaveCount
SourceWaveDataTexture
SourceDomainWarpFrequency
SourceDomainWarpAmount
SourceCrestSharpness

TargetWaveTime
TargetWaveCount
TargetWaveDataTexture
TargetBaseZ
TargetDomainWarpFrequency
TargetDomainWarpAmount
TargetCrestSharpness

Choppiness
SmallWaveChop
MaxHorizontal
```

The current backup already multiplies its alpha by `ConnectionEnabled`, providing the safe source-only fallback when a connection is invalid.

6. Connect:

```text
Absolute World Position (Excluding Material Shader Offsets) -> PreWPOWorldPos
your existing mask output -> WaveFade
CommonShipOffset -> ShipDeformation
all matching source/target parameters -> matching inputs
existing Choppiness -> Choppiness
existing SmallWaveChop -> SmallWaveChop
existing MaxHoriz/MaxHorizontal value -> MaxHorizontal
```

## Step 8.7 — Connect the Static Switch

Connect:

```text
False: existing normal river final WPO
True:  connected WPO Custom output
```

Connect the Static Switch output to **World Position Offset**.

Click **Apply** and **Save**.

The ordinary river keeps the false branch, so it does not pay for two wave evaluations.

---

# Stage 9 — Create MI_WaterJunction

In the Content Browser:

1. Right-click `M_RiverBase`.
2. Choose **Create Material Instance**.
3. Name it `MI_WaterJunction`.
4. Open it.
5. Find the Static Switch Parameter `ConnectionMode`.
6. Enable its override checkbox.
7. Set it to `true`.
8. Save.

This asset is assigned only to generated junction meshes.

The runtime also writes a scalar called `ConnectionMode`, but that scalar cannot enable a Static Switch. The Material Instance override above is required.

---

# Stage 10 — Connect a river endpoint in the level

## Step 10.1 — Prepare the actors

The target must be an Ocean or Lake actor containing:

```text
OceanBodyComponent
TiledWaterMeshComponent
```

The river spline must be open, not closed.

## Step 10.2 — Position the endpoint

For an end connection:

- Move the final spline point to the river mouth.
- Point its tangent outward into the ocean/lake.

For a start connection:

- Move spline point 0 to the river mouth.
- The system uses the opposite of its forward tangent as the outward direction.

For automatic detection, place the endpoint XY just inside the target water body's bounds.

Keep the endpoint at the intended river water height. The junction slopes its base mesh toward the target body's base Z.

## Step 10.3 — Detect the target

1. Select the `RiverWaterBodyActor`.
2. In Details, click **Detect Water Connections**.
3. Expand `StartConnection` and `EndConnection`.
4. Check which connection was enabled and which `TargetBody` was selected.

Detection only writes a suggestion into the actor. Runtime will use the stored target and will not silently select another body.

## Step 10.4 — Set the junction properties

On the connected StartConnection or EndConnection set:

```text
TargetBody:          target actor's OceanBody component
JunctionMaterial:    MI_WaterJunction
bEnabled:            true
BlendLength:         1500 (good first test)
MouthWidthScale:     1.5
LengthSubdivisions:  24
WidthSubdivisions:   match the river mesh if possible
```

Do not manually alter `ConnectionId` or `NetworkId`.

Click **Refresh Junctions**.

## What should happen

The plugin should:

1. Read the spline endpoint and outward tangent.
2. Generate a flared junction apron.
3. Cut the apron shape out of the target water tiles.
4. Retriangulate the target tiles around the opening.
5. Assign `MI_WaterJunction` to the apron.
6. Bind the river and target wave textures to the junction MID.
7. Put the river, junction, and target into one ship-field network.

The components share boundary positions but are not welded into one Unreal mesh asset.

---

# Stage 11 — Check the river source mesh

The source mesh used by the spline must follow this convention:

```text
Local X = river length
Local Y = river width
Mesh width = exactly 100 Unreal units
Mesh centred on Y = 0
```

The actor scales that 100-unit width to `RiverWidth`.

For the cleanest seam, match junction `WidthSubdivisions` to the number of quads across the river mesh endpoint.

Example:

```text
River endpoint has 9 vertices = 8 quads
Set WidthSubdivisions to 8
```

After changing `RiverWidth`, click both:

```text
Rebuild River Mesh
Refresh Junctions
```

---

# Stage 12 — First junction test

Temporarily make the river and target waves visibly different. Use different amplitudes or speeds so the blend is easy to see.

Check:

- At the river edge, the junction exactly follows river WPO.
- At the target edge, the junction exactly follows target WPO.
- No target surface remains underneath the apron.
- No colourless SLW layer remains.
- No crack opens during large waves.

Then drive a ship slowly across the connection and check:

- The same mask follows it across river, junction, and target.
- Gerstner waves remain suppressed underneath it.
- The existing depression keeps the hull clear.
- Positive bow and horizontal side deformation remain outside the hull.
- Deformation is not applied twice on the junction.

---

# Stage 13 — Normals and water appearance

Get the geometry and WPO working before doing this stage.

The junction eventually needs two normal evaluations:

```text
Source normal using Source wave parameters
Target normal using Target wave parameters
```

Blend them using the same connection alpha as WPO:

```text
FinalBodyNormal = normalize(lerp(SourceNormal, TargetNormal, ConnectionAlpha))
```

Do the same for:

```text
detail normals
foam
absorption
scattering
roughness
specular
water tint
refraction/distortion
```

Do not fade either surface out with SLW opacity. The target cutout controls ownership.

The subsystem automatically supplies the source and target wave parameters. It does not currently copy optical values out of the two body MIDs. For the first version, create manually controlled junction parameters such as:

```text
SourceAbsorption
TargetAbsorption
SourceScattering
TargetScattering
SourceRoughness
TargetRoughness
```

Set these values in `MI_WaterJunction`.

---

# Troubleshooting

## The mask moves or swims with the waves

Check that every mask and field sample uses:

```text
Absolute World Position (Excluding Material Shader Offsets)
```

## The junction does not appear

Check:

- The connection is enabled.
- `TargetBody` is assigned.
- `JunctionMaterial` is `MI_WaterJunction`.
- The target is an Ocean/Lake, not a river.
- The target has `TiledWaterMeshComponent`.
- The river spline is open.
- `MI_WaterJunction` compiles.

## The junction only shows river waves

Open `MI_WaterJunction` and confirm:

```text
ConnectionMode override checked
ConnectionMode = true
```

Also check all source/target parameter spelling.

## Water clips through the ship after adding deformation

Check:

- The existing mask depression is still connected.
- `InteriorVerticalOffset` starts at zero.
- `SafeDeformation`, rather than raw `ShipDeformation`, reaches WPO.
- The anti-clipping node receives the combined `WaveFade`.
- Bow and side values are initially small.

## The ship deformation is missing

Check:

- `Configure Deformation Field` is called.
- `M_ShipWaveDeformationStamp` is Additive and Unlit.
- The material uses `ShipWaveDeformationTexture`.
- `ShipWaveFieldEnabled` is connected to the sample Custom node.

## There is a crack at the river edge

Check:

- The source river mesh is exactly 100 units wide.
- It is centred on local Y=0.
- Its X axis follows the spline.
- `WidthSubdivisions` matches its endpoint quads.
- River and junction use the same pre-WPO Gerstner definition.
- You rebuilt the river mesh after changing width.

## A colourless or doubled water layer remains

- Do not use opacity to remove connected water.
- Confirm the target cutout was created.
- Confirm only the junction owns triangles inside the apron footprint.

---

# Files supplied by the plugin

You copy code from these files into Unreal Custom nodes:

```text
Shaders/WaterBodyConnection_CustomNode.hlsl
Shaders/ShipWaveDeformationSample_CustomNode.hlsl
Shaders/ShipWaveDeformationStamp_CustomNode.hlsl
```

The connection snippet uses the reusable plugin shader files:

```text
/OceanSystem/GerstnerWave.ush
/OceanSystem/WaterBodyConnection.ush
```

`ShipWaveDeformationSample_CustomNode.hlsl` calls the helpers from `/OceanSystem/ShipWaveField.ush`. Add that virtual path through the Custom node's **Include File Paths** array. Never paste a literal `#include` for it into the Code box.

The `.ush` files remain in the plugin. Use the provided `_CustomNode.hlsl` backup for each beginner material node.
