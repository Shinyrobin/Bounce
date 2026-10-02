// Procedural "sitting on a hopper ball" pose for the rider. No animation assets needed:
// each entry swings a bone so it points in a direction given in rider space.
// The active ragdoll uses this pose as the target its muscles pull toward.

#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimInstanceProxy.h"
#include "BounceRiderAnimInstance.generated.h"

USTRUCT(BlueprintType)
struct FBounceRiderBoneAim
{
	GENERATED_BODY()

	/** Bone to rotate. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rider Pose")
	FName Bone;

	/** A child bone; the Bone is swung so the line Bone -> Tip points along Direction. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rider Pose")
	FName Tip;

	/** Direction in rider space: X = forward, Y = right, Z = up. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rider Pose")
	FVector Direction = FVector::ForwardVector;
};

USTRUCT()
struct FBounceRiderAnimInstanceProxy : public FAnimInstanceProxy
{
	GENERATED_BODY()

	FBounceRiderAnimInstanceProxy() = default;
	explicit FBounceRiderAnimInstanceProxy(UAnimInstance* InAnimInstance) : FAnimInstanceProxy(InAnimInstance) {}

	virtual void PreUpdate(UAnimInstance* InAnimInstance, float DeltaSeconds) override;
	virtual bool Evaluate(FPoseContext& Output) override;

private:
	TArray<FBounceRiderBoneAim> Aims;
	/** Rotates rider-space directions (X forward) into the mesh's component space. */
	FQuat RiderToComponent = FQuat::Identity;
};

UCLASS(Transient, Blueprintable)
class BOUNCE_API UBounceRiderAnimInstance : public UAnimInstance
{
	GENERATED_BODY()

public:
	/** Limb directions applied on top of the reference pose, in order (parents before children). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rider Pose")
	TArray<FBounceRiderBoneAim> Aims;

	/** Yaw (degrees) from the mesh's component space to rider forward. UE mannequins face +Y, i.e. 90. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rider Pose")
	float MeshForwardYaw = 90.f;

protected:
	virtual FAnimInstanceProxy* CreateAnimInstanceProxy() override { return new FBounceRiderAnimInstanceProxy(this); }
	virtual void DestroyAnimInstanceProxy(FAnimInstanceProxy* InProxy) override { delete InProxy; }

	friend struct FBounceRiderAnimInstanceProxy;
};
