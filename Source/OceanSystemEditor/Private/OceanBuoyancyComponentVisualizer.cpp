// Copyright James Joslin. All Rights Reserved.

#include "OceanBuoyancyComponentVisualizer.h"

// NOTE: adjust this include to match the runtime module's folder layout,
// e.g. "Components/OceanBuoyancyComponent.h".
#include "Components/OceanBuoyancyComponent.h"

#include "ActorEditorUtils.h"
#include "CanvasTypes.h"
#include "Editor.h"
#include "EditorViewportClient.h"
#include "Engine/Engine.h"
#include "Engine/Font.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "GameFramework/Actor.h"
#include "SceneManagement.h"
#include "ScopedTransaction.h"

IMPLEMENT_HIT_PROXY(HBuoyancySamplePointProxy, HComponentVisProxy);

#define LOCTEXT_NAMESPACE "OceanBuoyancyVisualizer"

namespace OceanBuoyancyVis
{
	static const FLinearColor PointColor(0.15f, 0.75f, 1.0f);      // cyan
	static const FLinearColor SelectedColor(1.0f, 0.55f, 0.05f);   // orange
	static const FLinearColor BoxColor(0.2f, 1.0f, 0.4f);          // green
}

// ===================================================================
// Drawing
// ===================================================================

void FOceanBuoyancyComponentVisualizer::DrawVisualization(
	const UActorComponent* Component, const FSceneView* View,
	FPrimitiveDrawInterface* PDI)
{
	const UOceanBuoyancyComponent* Buoy =
		Cast<const UOceanBuoyancyComponent>(Component);
	const AActor* Owner = Buoy ? Buoy->GetOwner() : nullptr;
	if (!Buoy || !Owner)
	{
		return;
	}

	const FTransform ActorTransform = Owner->GetActorTransform();
	const bool bIsEditedComponent = (Buoy == GetEditedBuoyancyComponent());

	// --- Generator box (visual reference for lining up the grid) ---
	if (Buoy->bDrawGeneratorBox)
	{
		const FBox LocalBox = FBox::BuildAABB(Buoy->BoxCenter, Buoy->BoxHalfExtents);
		DrawWireBox(PDI, ActorTransform.ToMatrixWithScale(), LocalBox,
			OceanBuoyancyVis::BoxColor, SDPG_World, 0.6f);
	}

	// --- Sample points ---
	for (int32 Index = 0; Index < Buoy->SamplePoints.Num(); ++Index)
	{
		const FVector WorldPoint =
			ActorTransform.TransformPosition(Buoy->SamplePoints[Index]);
		const bool bSelected =
			bIsEditedComponent && (Index == SelectedPointIndex);
		const FLinearColor Color = bSelected
			? OceanBuoyancyVis::SelectedColor
			: OceanBuoyancyVis::PointColor;

		// Hit proxy makes the sphere clickable in the viewport.
		PDI->SetHitProxy(new HBuoyancySamplePointProxy(Component, Index));

		DrawWireSphere(PDI, WorldPoint, Color, Buoy->DebugPointRadius,
			12, SDPG_Foreground, bSelected ? 1.5f : 0.75f);
		PDI->DrawPoint(WorldPoint, Color, bSelected ? 12.0f : 8.0f,
			SDPG_Foreground);

		PDI->SetHitProxy(nullptr);
	}
}

void FOceanBuoyancyComponentVisualizer::DrawVisualizationHUD(
	const UActorComponent* Component, const FViewport* Viewport,
	const FSceneView* View, FCanvas* Canvas)
{
	const UOceanBuoyancyComponent* Buoy = GetEditedBuoyancyComponent();
	if (!Buoy || Buoy != Component
		|| !Buoy->SamplePoints.IsValidIndex(SelectedPointIndex))
	{
		return;
	}

	const FVector& Local = Buoy->SamplePoints[SelectedPointIndex];
	const FString Text = FString::Printf(
		TEXT("Buoyancy point %d / %d   local (X=%.1f  Y=%.1f  Z=%.1f)   ")
		TEXT("[Drag: move | Alt+Drag: duplicate | Del: remove]"),
		SelectedPointIndex, Buoy->SamplePoints.Num(),
		Local.X, Local.Y, Local.Z);

	Canvas->DrawShadowedString(
		60.0f, 60.0f, *Text, GEngine->GetSmallFont(),
		OceanBuoyancyVis::SelectedColor);
}

// ===================================================================
// Selection
// ===================================================================

bool FOceanBuoyancyComponentVisualizer::VisProxyHandleClick(
	FEditorViewportClient* InViewportClient, HComponentVisProxy* VisProxy,
	const FViewportClick& Click)
{
	if (VisProxy && VisProxy->Component.IsValid()
		&& VisProxy->IsA(HBuoyancySamplePointProxy::StaticGetType()))
	{
		const UOceanBuoyancyComponent* Buoy =
			CastChecked<const UOceanBuoyancyComponent>(VisProxy->Component.Get());

		ComponentPropertyPath = FComponentPropertyPath(Buoy);
		if (ComponentPropertyPath.IsValid())
		{
			SelectedPointIndex =
				static_cast<HBuoyancySamplePointProxy*>(VisProxy)->PointIndex;
			bHasDuplicatedThisDrag = false;
			return true;
		}
	}

	SelectedPointIndex = INDEX_NONE;
	ComponentPropertyPath.Reset();
	return false;
}

// ===================================================================
// Widget (translate gizmo)
// ===================================================================

bool FOceanBuoyancyComponentVisualizer::GetWidgetLocation(
	const FEditorViewportClient* ViewportClient, FVector& OutLocation) const
{
	const UOceanBuoyancyComponent* Buoy = GetEditedBuoyancyComponent();
	if (Buoy && Buoy->GetOwner()
		&& Buoy->SamplePoints.IsValidIndex(SelectedPointIndex))
	{
		OutLocation = Buoy->GetOwner()->GetActorTransform()
			.TransformPosition(Buoy->SamplePoints[SelectedPointIndex]);
		return true;
	}
	return false;
}

bool FOceanBuoyancyComponentVisualizer::HandleInputDelta(
	FEditorViewportClient* ViewportClient, FViewport* Viewport,
	FVector& DeltaTranslate, FRotator& DeltaRotate, FVector& DeltaScale)
{
	UOceanBuoyancyComponent* Buoy = GetEditedBuoyancyComponent();
	if (!Buoy || !Buoy->GetOwner()
		|| !Buoy->SamplePoints.IsValidIndex(SelectedPointIndex))
	{
		return false;
	}

	// Allow a fresh Alt-drag duplication once Alt is released.
	if (!ViewportClient->IsAltPressed())
	{
		bHasDuplicatedThisDrag = false;
	}

	if (DeltaTranslate.IsZero())
	{
		return true; // consume — we're mid-drag on a point
	}

	Buoy->Modify();

	// Alt + drag: duplicate the point once, then move the copy.
	if (ViewportClient->IsAltPressed() && !bHasDuplicatedThisDrag)
	{
		Buoy->SamplePoints.Insert(
			Buoy->SamplePoints[SelectedPointIndex], SelectedPointIndex + 1);
		++SelectedPointIndex;
		bHasDuplicatedThisDrag = true;
	}

	// Widget delta is world-space; points are stored in actor local
	// space, so run it through the inverse actor transform (handles
	// rotation AND non-uniform scale).
	const FTransform ActorTransform = Buoy->GetOwner()->GetActorTransform();
	const FVector LocalDelta = ActorTransform.InverseTransformVector(DeltaTranslate);
	Buoy->SamplePoints[SelectedPointIndex] += LocalDelta;

	NotifySamplePointsModified(Buoy);
	return true;
}

// ===================================================================
// Undo-friendly drag transactions
// ===================================================================

void FOceanBuoyancyComponentVisualizer::TrackingStarted(
	FEditorViewportClient* InViewportClient)
{
	UOceanBuoyancyComponent* Buoy = GetEditedBuoyancyComponent();
	if (Buoy && Buoy->SamplePoints.IsValidIndex(SelectedPointIndex) && GEditor)
	{
		GEditor->BeginTransaction(
			LOCTEXT("MoveBuoyancySamplePoint", "Move Buoyancy Sample Point"));
		bIsTransacting = true;
	}
}

void FOceanBuoyancyComponentVisualizer::TrackingStopped(
	FEditorViewportClient* InViewportClient, bool bInDidMove)
{
	if (bIsTransacting && GEditor)
	{
		GEditor->EndTransaction();
	}
	bIsTransacting = false;
	bHasDuplicatedThisDrag = false;
}

// ===================================================================
// Keyboard
// ===================================================================

bool FOceanBuoyancyComponentVisualizer::HandleInputKey(
	FEditorViewportClient* ViewportClient, FViewport* Viewport,
	FKey Key, EInputEvent Event)
{
	if (Event == IE_Pressed
		&& (Key == EKeys::Delete || Key == EKeys::BackSpace)
		&& SelectedPointIndex != INDEX_NONE)
	{
		DeleteSelectedPoint();
		return true;
	}
	return false;
}

// ===================================================================
// Context menu (right-click on a selected point)
// ===================================================================

TSharedPtr<SWidget> FOceanBuoyancyComponentVisualizer::GenerateContextMenu() const
{
	FMenuBuilder MenuBuilder(/*bShouldCloseAfterSelection=*/true, nullptr);

	MenuBuilder.BeginSection("BuoyancySamplePoint",
		LOCTEXT("SamplePointSection", "Buoyancy Sample Point"));
	{
		MenuBuilder.AddMenuEntry(
			LOCTEXT("DuplicatePoint", "Duplicate Point"),
			LOCTEXT("DuplicatePointTooltip",
				"Duplicate the selected sample point (Alt + drag also duplicates)."),
			FSlateIcon(),
			FUIAction(FExecuteAction::CreateLambda([this]()
			{
				const_cast<FOceanBuoyancyComponentVisualizer*>(this)
					->DuplicateSelectedPoint();
			})));

		MenuBuilder.AddMenuEntry(
			LOCTEXT("DeletePoint", "Delete Point"),
			LOCTEXT("DeletePointTooltip",
				"Delete the selected sample point (Delete key)."),
			FSlateIcon(),
			FUIAction(FExecuteAction::CreateLambda([this]()
			{
				const_cast<FOceanBuoyancyComponentVisualizer*>(this)
					->DeleteSelectedPoint();
			})));
	}
	MenuBuilder.EndSection();

	return MenuBuilder.MakeWidget();
}

// ===================================================================
// Point operations
// ===================================================================

void FOceanBuoyancyComponentVisualizer::DuplicateSelectedPoint()
{
	UOceanBuoyancyComponent* Buoy = GetEditedBuoyancyComponent();
	if (!Buoy || !Buoy->SamplePoints.IsValidIndex(SelectedPointIndex))
	{
		return;
	}

	const FScopedTransaction Transaction(
		LOCTEXT("DuplicateBuoyancySamplePoint", "Duplicate Buoyancy Sample Point"));
	Buoy->Modify();

	// Offset the copy slightly so it's visibly a new point.
	FVector NewPoint = Buoy->SamplePoints[SelectedPointIndex];
	NewPoint.X += Buoy->DebugPointRadius * 2.0f;

	Buoy->SamplePoints.Insert(NewPoint, SelectedPointIndex + 1);
	++SelectedPointIndex;

	NotifySamplePointsModified(Buoy);
}

void FOceanBuoyancyComponentVisualizer::DeleteSelectedPoint()
{
	UOceanBuoyancyComponent* Buoy = GetEditedBuoyancyComponent();
	if (!Buoy || !Buoy->SamplePoints.IsValidIndex(SelectedPointIndex))
	{
		return;
	}

	const FScopedTransaction Transaction(
		LOCTEXT("DeleteBuoyancySamplePoint", "Delete Buoyancy Sample Point"));
	Buoy->Modify();

	Buoy->SamplePoints.RemoveAt(SelectedPointIndex);
	SelectedPointIndex = INDEX_NONE;

	NotifySamplePointsModified(Buoy);
}

void FOceanBuoyancyComponentVisualizer::NotifySamplePointsModified(
	UOceanBuoyancyComponent* Component)
{
	static FProperty* SamplePointsProperty = FindFProperty<FProperty>(
		UOceanBuoyancyComponent::StaticClass(),
		GET_MEMBER_NAME_CHECKED(UOceanBuoyancyComponent, SamplePoints));

	// Fires PostEditChange and, when editing a Blueprint archetype in the
	// BP editor viewport, propagates the change to instances correctly.
	NotifyPropertyModified(Component, SamplePointsProperty);
}

// ===================================================================
// Bookkeeping
// ===================================================================

void FOceanBuoyancyComponentVisualizer::EndEditing()
{
	SelectedPointIndex = INDEX_NONE;
	ComponentPropertyPath.Reset();
	bHasDuplicatedThisDrag = false;
}

UActorComponent* FOceanBuoyancyComponentVisualizer::GetEditedComponent() const
{
	return ComponentPropertyPath.GetComponent();
}

UOceanBuoyancyComponent*
FOceanBuoyancyComponentVisualizer::GetEditedBuoyancyComponent() const
{
	return Cast<UOceanBuoyancyComponent>(ComponentPropertyPath.GetComponent());
}

bool FOceanBuoyancyComponentVisualizer::IsVisualizingArchetype() const
{
	const UOceanBuoyancyComponent* Buoy = GetEditedBuoyancyComponent();
	return Buoy && Buoy->GetOwner()
		&& FActorEditorUtils::IsAPreviewOrInactiveActor(Buoy->GetOwner());
}

#undef LOCTEXT_NAMESPACE
