# River–Large Water Junction and Ship-Wave Field Plan

## Goal

Replace overlapping Single Layer Water surfaces with geometry-owned river junctions. A river, junction, and connected lake/ocean must share exact boundary loops, while the junction alone evaluates and blends both bodies' absolute WPO, normals, and material properties. Connected surfaces also share world-space ship suppression and signed deformation fields.

Existing unconnected water actors remain backward compatible.

## Phase 0 — Baseline and diagnostics

- Record current actor defaults, material parameter names, mesh subdivision settings, and ship-mask behavior.
- Visualize body footprints, endpoint candidates, connection footprints, boundary loops, and ship-field coverage.
- Identify the active Single Layer Water master and stamp materials before editing material assets.

## Phase 1 — Connection model

- Add explicit, serialized start/end river connection settings: target body, endpoint, blend length, mouth scale, stable connection ID, network ID, and enabled state.
- Provide editor target detection based on endpoint containment, vertical proximity, and body priority. Detection writes the explicit target; it does not silently drive runtime behavior.
- Rebuild a connection when either body, spline, width, material, tile layout, transform, or wave configuration changes.
- Compute mouth blending from distance along the spline, not lateral bank distance.

## Phase 2 — Unified CPU evaluation

- Route physics height/data, full height, velocity, and fold intensity through one connection-aware evaluator.
- Evaluate source and target independently, including their base height, spline height, time scale, warp, sharpening, and layers.
- Blend absolute surface height and normalized normals using the same quintic alpha used by the GPU.
- Keep ship suppression visual-only by default; any later physical parity must use the analytical proxy SDF rather than GPU readback.

## Phase 3 — Snapped geometry

- Generate a tessellated junction strip/apron from the river endpoint into the target body.
- Generate river, junction, and target boundaries from common world-space loops.
- Cut the junction footprint from affected target tiles and retriangulate them; never use SLW opacity for ownership.
- Keep a fixed, LOD-invariant boundary ring and stitch surrounding LODs to it.
- Rebuild only affected junctions and tiles.

## Phase 4 — Dual-wave junction shader

- Add reusable connection shader helpers for quintic alpha, absolute-surface WPO, and normal blending.
- Evaluate from world position excluding material offsets.
- Bind source and target wave/detail textures, counts, times, shaping parameters, and target base Z to the junction MID.
- Blend WPO, analytical/detail normals, foam, absorption, scattering, roughness, and other water properties with the same alpha.
- Restrict dual evaluation to junction MIDs; normal water keeps the single-wave path.

## Phase 5 — Shared ship fields

- Treat connected water bodies and junctions as a water-surface network.
- Replace single-body proxy assignment with network intersection.
- Allocate a rolling `R8` suppression field and `RGBA16F` signed deformation field per active network.
- Default to `2048 x 2048` over a configurable 200 m square, with a texel-snapped world origin.
- Stamp every eligible vessel deterministically. Suppression uses union/max semantics; deformation uses signed additive semantics.
- Suppress both Gerstner evaluations before blending, then apply common hull deformation exactly once.
- Include deformation slope in the final normal.

## Phase 6 — Single Layer Water materials

- Add a static `ConnectionMode` to the active SLW master.
- Add source/target wave bindings and shared ship-field bindings.
- Add/update suppression and signed-deformation stamp materials.
- Keep custom-node backup HLSL synchronized with active nodes.
- Missing target data falls back to source waves; missing ship fields fall back to full waves and zero deformation; invalid cutout geometry disables the junction to avoid overlapping SLW surfaces.

## Phase 7 — Bounds, LOD, compatibility, and performance

- Recompute bounds on existing tiles and junctions when amplitudes, base heights, or deformation limits change.
- Fix tile rebuild handling for subdivision and LOD array edits.
- Cache MIDs, textures, triangulation, and RT resources; update only dynamic time/field parameters each frame.
- Retain existing public subsystem/component names for Blueprint compatibility.
- Preserve legacy generic overlap behavior for unrelated bodies while explicit river connections supersede it at mouths.

## Acceptance criteria

- Shared boundary positions differ by no more than `0.1 cm` before WPO.
- Source and target boundaries exactly reproduce their neighboring WPO and normals.
- No overlapping SLW triangles, holes, colorless ghost layer, or doubled absorption.
- All CPU queries resolve the same connected surface.
- Stationary/moving vessels suppress and deform the surface continuously while crossing a junction.
- Multiple ship stamps accumulate without erasing each other.
- RT recentering does not visibly pop or crawl.
- Normal water does not pay for the junction's dual-wave shader path.
- No synchronous GPU readback is introduced.

## Defaults and compatibility

- Connections are explicit; editor detection is an authoring helper.
- Geometry ownership is absolute: river, junction, and target triangles do not overlap.
- V1 uses a rolling network field for one local active gameplay region per network. A paged atlas is a future multiplayer scalability step.
- Ship suppression remains visual-only for buoyancy by default.
- Active material assets outside this plugin require graph updates after their exact assets are confirmed.
