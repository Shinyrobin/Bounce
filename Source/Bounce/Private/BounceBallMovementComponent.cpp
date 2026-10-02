#include "BounceBallMovementComponent.h"

#include "GameFramework/Pawn.h"

namespace BounceBall
{
	constexpr int32 MaxMoveIterations = 4;
	/** How long (s) without floor contact before a resting ball counts as airborne again. */
	constexpr float RestingGraceTime = 0.1f;
	/** Impacts slower than this (cm/s) don't fire wall events. */
	constexpr float MinNotifySpeed = 50.f;
}

UBounceBallMovementComponent::UBounceBallMovementComponent()
{
	bConstrainToPlane = false;
}

float UBounceBallMovementComponent::GetGravityZ() const
{
	return Super::GetGravityZ() * GravityScale;
}

float UBounceBallMovementComponent::GetSpeedForHeight(float Height) const
{
	return FMath::Sqrt(2.f * FMath::Abs(GetGravityZ()) * FMath::Max(Height, 0.f));
}

bool UBounceBallMovementComponent::IsFloor(const FVector& Normal) const
{
	return Normal.Z >= FMath::Cos(FMath::DegreesToRadians(MaxFloorAngle));
}

void UBounceBallMovementComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	if (ShouldSkipUpdate(DeltaTime) || !UpdatedComponent || !PawnOwner || DeltaTime <= 0.f)
	{
		return;
	}

	FVector Input = ConsumeInputVector();
	Input.Z = 0.f;
	Input = Input.GetClampedToMaxSize(1.f);

	JumpBufferRemaining = FMath::Max(0.f, JumpBufferRemaining - DeltaTime);
	TimeSinceFloorBounce += DeltaTime;
	RestingGraceRemaining -= DeltaTime;

	ApplyRotation(Input, DeltaTime);
	ApplySteering(Input, DeltaTime);

	Velocity.Z = FMath::Max(Velocity.Z + GetGravityZ() * DeltaTime, -MaxFallSpeed);

	// Sweep along the velocity, bouncing off whatever we hit and spending the rest of the frame on the new velocity.
	float RemainingFraction = 1.f;
	for (int32 Iteration = 0; Iteration < BounceBall::MaxMoveIterations && RemainingFraction > UE_KINDA_SMALL_NUMBER; ++Iteration)
	{
		FHitResult Hit;
		SafeMoveUpdatedComponent(Velocity * DeltaTime * RemainingFraction, UpdatedComponent->GetComponentQuat(), true, Hit);
		if (!Hit.IsValidBlockingHit())
		{
			break;
		}
		RemainingFraction *= 1.f - Hit.Time;
		ResolveImpact(Hit, Input);
	}

	if (bResting && RestingGraceRemaining <= 0.f)
	{
		bResting = false;
	}

	UpdateComponentVelocity();
}

void UBounceBallMovementComponent::ApplySteering(const FVector& Input, float DeltaTime)
{
	if (bSlamming)
	{
		return;
	}

	FVector Horizontal(Velocity.X, Velocity.Y, 0.f);
	const float Control = bResting ? 1.f : AirControl;

	if (Input.IsNearlyZero())
	{
		const float Speed = FMath::Max(0.f, Horizontal.Size() - Deceleration * Control * DeltaTime);
		Horizontal = Horizontal.GetSafeNormal() * Speed;
	}
	else
	{
		// Above MaxSpeed (launch pads, slopes) bleed off gently instead of snapping back.
		const bool bOverspeed = Horizontal.SizeSquared() > FMath::Square(MaxSpeed);
		const float Rate = (bOverspeed ? Deceleration : Acceleration) * Control;
		Horizontal = FMath::VInterpConstantTo(Horizontal, Input * MaxSpeed, DeltaTime, Rate);
	}

	Velocity.X = Horizontal.X;
	Velocity.Y = Horizontal.Y;
}

void UBounceBallMovementComponent::ApplyRotation(const FVector& Input, float DeltaTime)
{
	if (!bOrientToMovement)
	{
		return;
	}

	FVector Facing = Input;
	if (Facing.IsNearlyZero())
	{
		Facing = FVector(Velocity.X, Velocity.Y, 0.f);
		if (Facing.SizeSquared() < FMath::Square(50.f))
		{
			return;
		}
	}

	const FRotator Current = UpdatedComponent->GetComponentRotation();
	const float NewYaw = FMath::FixedTurn(Current.Yaw, Facing.Rotation().Yaw, RotationRate * DeltaTime);
	if (!FMath::IsNearlyEqual(NewYaw, Current.Yaw))
	{
		MoveUpdatedComponent(FVector::ZeroVector, FRotator(0.f, NewYaw, 0.f), false);
	}
}

void UBounceBallMovementComponent::ResolveImpact(const FHitResult& Hit, const FVector& Input)
{
	const FVector Normal = Hit.Normal;
	const float NormalVelocity = Velocity | Normal;
	if (NormalVelocity >= 0.f)
	{
		return; // Already moving away from the surface.
	}

	const float InSpeed = -NormalVelocity;
	FVector Tangent = Velocity - NormalVelocity * Normal;

	if (!IsFloor(Normal))
	{
		const float OutSpeed = InSpeed * WallRestitution;
		Velocity = Tangent + Normal * OutSpeed;
		if (InSpeed > BounceBall::MinNotifySpeed)
		{
			Notify(EBounceType::Wall, InSpeed, OutSpeed, Hit);
		}
		return;
	}

	EBounceType Type = EBounceType::Hop;
	float OutSpeed = InSpeed * Restitution;
	if (bAutoHop)
	{
		OutSpeed = FMath::Max(OutSpeed, GetSpeedForHeight(HopHeight));
	}
	if (bSlamming)
	{
		OutSpeed = FMath::Max3(OutSpeed, GetSpeedForHeight(SlamBounceHeight), InSpeed * SlamRestitution);
		Type = EBounceType::Slam;
		bSlamming = false;
	}
	if (JumpBufferRemaining > 0.f || (bHoldJumpForBigBounces && bJumpHeld))
	{
		OutSpeed = FMath::Max3(OutSpeed, GetSpeedForHeight(JumpHeight), InSpeed * JumpRestitution);
		if (Type != EBounceType::Slam)
		{
			Type = EBounceType::Jump;
		}
		JumpBufferRemaining = 0.f;
	}
	OutSpeed = FMath::Min(OutSpeed, GetSpeedForHeight(MaxBounceHeight));

	if (OutSpeed < MinBounceSpeed)
	{
		// Too soft to bounce: drop the into-floor speed and roll along it.
		Velocity = Tangent;
		const bool bWasResting = bResting;
		bResting = true;
		RestingGraceRemaining = BounceBall::RestingGraceTime;
		if (!bWasResting)
		{
			Notify(EBounceType::Settle, InSpeed, 0.f, Hit);
		}
		return;
	}

	Tangent *= 1.f - BounceFriction;
	if (!Input.IsNearlyZero())
	{
		// Keep the horizontal speed but swing its direction toward the stick.
		const FVector Horizontal(Tangent.X, Tangent.Y, 0.f);
		const FVector Turned = FMath::Lerp(Horizontal, Input.GetSafeNormal() * Horizontal.Size(), BounceTurnAssist);
		Tangent.X = Turned.X;
		Tangent.Y = Turned.Y;
	}

	const FVector BounceDir = FMath::Lerp(Normal, FVector::UpVector, UprightBias).GetSafeNormal();
	Velocity = Tangent + BounceDir * OutSpeed;

	bResting = false;
	TimeSinceFloorBounce = 0.f;
	bLastBounceUpgradable = Type == EBounceType::Hop;
	Notify(Type, InSpeed, OutSpeed, Hit);
}

void UBounceBallMovementComponent::Notify(EBounceType Type, float InSpeed, float OutSpeed, const FHitResult& Hit)
{
	FBounceImpact Impact;
	Impact.Type = Type;
	Impact.ImpactSpeed = InSpeed;
	Impact.BounceSpeed = OutSpeed;
	Impact.ImpactPoint = Hit.ImpactPoint;
	Impact.ImpactNormal = Hit.Normal;
	Impact.Hit = Hit;
	OnBounced.Broadcast(Impact);
}

void UBounceBallMovementComponent::JumpPressed()
{
	bJumpHeld = true;

	if (bResting && UpdatedComponent)
	{
		// Resting on the floor: hop off right away.
		const float OutSpeed = GetSpeedForHeight(JumpHeight);
		Velocity.Z = FMath::Max(Velocity.Z, OutSpeed);
		bResting = false;
		RestingGraceRemaining = 0.f;

		FHitResult Hit;
		Hit.ImpactPoint = UpdatedComponent->GetComponentLocation() - FVector(0.f, 0.f, UpdatedComponent->Bounds.SphereRadius);
		Hit.Normal = FVector::UpVector;
		Notify(EBounceType::Jump, 0.f, OutSpeed, Hit);
		return;
	}

	if (bLastBounceUpgradable && TimeSinceFloorBounce <= JumpLateTime && UpdatedComponent)
	{
		// Pressed just after a plain hop: upgrade it.
		const float OutSpeed = GetSpeedForHeight(JumpHeight);
		if (Velocity.Z < OutSpeed)
		{
			Velocity.Z = OutSpeed;
			bLastBounceUpgradable = false;

			FHitResult Hit;
			Hit.ImpactPoint = UpdatedComponent->GetComponentLocation() - FVector(0.f, 0.f, UpdatedComponent->Bounds.SphereRadius);
			Hit.Normal = FVector::UpVector;
			Notify(EBounceType::Jump, 0.f, OutSpeed, Hit);
			return;
		}
	}

	JumpBufferRemaining = JumpBufferTime;
}

void UBounceBallMovementComponent::JumpReleased()
{
	bJumpHeld = false;
}

bool UBounceBallMovementComponent::Slam()
{
	if (!bCanSlam || bSlamming || bResting)
	{
		return false;
	}

	Velocity.X *= SlamHorizontalKeep;
	Velocity.Y *= SlamHorizontalKeep;
	Velocity.Z = -SlamSpeed;
	bSlamming = true;
	return true;
}

void UBounceBallMovementComponent::Launch(FVector LaunchVelocity, bool bOverrideHorizontal, bool bOverrideVertical)
{
	FVector NewVelocity = LaunchVelocity;
	if (!bOverrideHorizontal)
	{
		NewVelocity.X += Velocity.X;
		NewVelocity.Y += Velocity.Y;
	}
	if (!bOverrideVertical)
	{
		NewVelocity.Z += Velocity.Z;
	}
	Velocity = NewVelocity;
	bSlamming = false;
	bResting = false;
	RestingGraceRemaining = 0.f;
}
