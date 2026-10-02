#include "BounceRiderAnimInstance.h"

#include "Animation/AnimNodeBase.h"
#include "BonePose.h"

void FBounceRiderAnimInstanceProxy::PreUpdate(UAnimInstance* InAnimInstance, float DeltaSeconds)
{
	FAnimInstanceProxy::PreUpdate(InAnimInstance, DeltaSeconds);

	if (const UBounceRiderAnimInstance* Rider = Cast<UBounceRiderAnimInstance>(InAnimInstance))
	{
		Aims = Rider->Aims;
		RiderToComponent = FRotator(0.f, Rider->MeshForwardYaw, 0.f).Quaternion();
	}
}

bool FBounceRiderAnimInstanceProxy::Evaluate(FPoseContext& Output)
{
	Output.ResetToRefPose();

	const FBoneContainer& Bones = Output.Pose.GetBoneContainer();
	FCSPose<FCompactPose> ComponentPose;
	ComponentPose.InitPose(Output.Pose);

	for (const FBounceRiderBoneAim& Aim : Aims)
	{
		const int32 BoneMeshIndex = Bones.GetPoseBoneIndexForBoneName(Aim.Bone);
		const int32 TipMeshIndex = Bones.GetPoseBoneIndexForBoneName(Aim.Tip);
		if (BoneMeshIndex == INDEX_NONE || TipMeshIndex == INDEX_NONE)
		{
			continue;
		}
		const FCompactPoseBoneIndex BoneIndex = Bones.MakeCompactPoseIndex(FMeshPoseBoneIndex(BoneMeshIndex));
		const FCompactPoseBoneIndex TipIndex = Bones.MakeCompactPoseIndex(FMeshPoseBoneIndex(TipMeshIndex));
		if (!BoneIndex.IsValid() || !TipIndex.IsValid())
		{
			continue;
		}

		FTransform BoneTransform = ComponentPose.GetComponentSpaceTransform(BoneIndex);
		const FVector TipLocation = ComponentPose.GetComponentSpaceTransform(TipIndex).GetLocation();
		const FVector Current = (TipLocation - BoneTransform.GetLocation()).GetSafeNormal();
		const FVector Target = RiderToComponent.RotateVector(Aim.Direction).GetSafeNormal();
		if (Current.IsNearlyZero() || Target.IsNearlyZero())
		{
			continue;
		}

		BoneTransform.SetRotation(FQuat::FindBetweenNormals(Current, Target) * BoneTransform.GetRotation());
		const FBoneTransform NewTransform(BoneIndex, BoneTransform);
		ComponentPose.LocalBlendCSBoneTransforms(MakeArrayView(&NewTransform, 1), 1.f);
	}

	FCSPose<FCompactPose>::ConvertComponentPosesToLocalPosesSafe(ComponentPose, Output.Pose);
	return true;
}
