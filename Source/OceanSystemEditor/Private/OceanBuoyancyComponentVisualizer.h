// Copyright James Joslin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ComponentVisualizer.h"

class UOceanBuoyancyComponent;
class FEditorViewportClient;
class SWidget;

/**
 * Hit proxy for a single buoyancy sample point sphere.
 * Lets the viewport map a mouse click back to a point index.
 */
struct HBuoyancySamplePointProxy : public HComponentVisProxy
{
	DECLARE_HIT_PROXY();

	HBuoyancySamplePointProxy(const UActorComponent* InComponent, int32 InPointIndex)
		: HComponentVisProxy(InComponent, HPP_Wireframe)
		, PointIndex(InPointIndex)
	{
	}

	int32 PointIndex;
};

/**
 * Viewport visualizer for UOceanBuoyancyComponent.
 *
 * Works in BOTH the level editor (select the actor) and the Blueprint
 * editor viewport (select the buoyancy component in the Components panel).
 *
 * Features:
 *   - Draws every sample point as a wire sphere, plus the generator box,
 *     so points can be lined up against the root mesh visually.
 *   - Click a point to select it — the standard translate gizmo appears
 *     at the point. Drag to move it (stored back in actor local space,
 *     so it respects actor rotation/scale).
 *   - Alt + drag duplicates the selected point, then moves the copy
 *     (same UX as spline points).
 *   - Delete / Backspace removes the selected point.
 *   - Right-click a selected point for Duplicate / Delete menu entries.
 *
 * All edits are transacted (undo/redo) and propagated to instances when
 * editing a Blueprint archetype via NotifyPropertyModified.
 */
class FOceanBuoyancyComponentVisualizer : public FComponentVisualizer
{
public:
	// ----- FComponentVisualizer interface -----
	virtual void DrawVisualization(const UActorComponent* Component,
		const FSceneView* View, FPrimitiveDrawInterface* PDI) override;
	virtual void DrawVisualizationHUD(const UActorComponent* Component,
		const FViewport* Viewport, const FSceneView* View, FCanvas* Canvas) override;

	virtual bool VisProxyHandleClick(FEditorViewportClient* InViewportClient,
		HComponentVisProxy* VisProxy, const FViewportClick& Click) override;

	virtual bool GetWidgetLocation(const FEditorViewportClient* ViewportClient,
		FVector& OutLocation) const override;
	virtual bool HandleInputDelta(FEditorViewportClient* ViewportClient,
		FViewport* Viewport, FVector& DeltaTranslate, FRotator& DeltaRotate,
		FVector& DeltaScale) override;
	virtual bool HandleInputKey(FEditorViewportClient* ViewportClient,
		FViewport* Viewport, FKey Key, EInputEvent Event) override;

	virtual void TrackingStarted(FEditorViewportClient* InViewportClient) override;
	virtual void TrackingStopped(FEditorViewportClient* InViewportClient,
		bool bInDidMove) override;

	virtual TSharedPtr<SWidget> GenerateContextMenu() const override;

	virtual void EndEditing() override;
	virtual UActorComponent* GetEditedComponent() const override;
	virtual bool IsVisualizingArchetype() const override;

private:
	/** Resolves the currently edited component (survives BP reconstruction). */
	UOceanBuoyancyComponent* GetEditedBuoyancyComponent() const;

	void DuplicateSelectedPoint();
	void DeleteSelectedPoint();

	/** Fires PostEditChange + archetype propagation for SamplePoints. */
	static void NotifySamplePointsModified(UOceanBuoyancyComponent* Component);

	/** Property path to the edited component instance. */
	FComponentPropertyPath ComponentPropertyPath;

	/** Index of the selected sample point, or INDEX_NONE. */
	int32 SelectedPointIndex = INDEX_NONE;

	/** Guards Alt-drag so one drag produces one duplicate. */
	bool bHasDuplicatedThisDrag = false;

	/** True while a gizmo-drag transaction is open. */
	bool bIsTransacting = false;
};
