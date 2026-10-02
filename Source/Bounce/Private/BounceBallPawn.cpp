#include "BounceBallPawn.h"

#include "AnimationRuntime.h"
#include "Camera/CameraComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/SphereComponent.h"
#include "Components/StaticMeshComponent.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "Engine/LocalPlayer.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/SpringArmComponent.h"
#include "InputActionValue.h"
#include "PhysicsEngine/PhysicsHandleComponent.h"
#include "UObject/ConstructorHelpers.h"

namespace BounceBallPawn
{
	constexpr float BallRadius = 38.f;
	/** Frames to let the seated pose settle before turning on rider physics. */
	constexpr int32 RiderWarmupFrames = 2;
	/** How bouncy the popped ball / empty skin is against walls and floors. */
	constexpr float DeflatingRestitution = 0.5f;
	constexpr float SkinRestitution = 0.25f;
	/** Shape of the empty ball skin lying on the floor (relative to DeflatedScale). */
	const FVector SkinSquash(1.4f, 1.4f, 0.3f);

	UStaticMeshComponent* MakeVisualMesh(AActor* Owner, FName Name, USceneComponent* Parent, UStaticMesh* Mesh)
	{
		UStaticMeshComponent* Component = Owner->CreateDefaultSubobject<UStaticMeshComponent>(Name);
		Component->SetupAttachment(Parent);
		Component->SetStaticMesh(Mesh);
		Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Component->SetGenerateOverlapEvents(false);
		Component->SetCanEverAffectNavigation(false);
		return Component;
	}

	// Handle springs are force-based (not mass-scaled), so soft settings can't hold up a whole body.
	// Pins are rigid by default; the wobble comes from the rider's muscles instead.
	UPhysicsHandleComponent* MakeHandle(AActor* Owner, FName Name)
	{
		UPhysicsHandleComponent* Handle = Owner->CreateDefaultSubobject<UPhysicsHandleComponent>(Name);
		Handle->bInterpolateTarget = false;
		Handle->bSoftLinearConstraint = false;
		Handle->bSoftAngularConstraint = false;
		return Handle;
	}
}

ABounceBallPawn::ABounceBallPawn()
{
	PrimaryActorTick.bCanEverTick = true;
	bUseControllerRotationPitch = false;
	bUseControllerRotationYaw = false;
	bUseControllerRotationRoll = false;

	static ConstructorHelpers::FObjectFinder<UStaticMesh> SphereMesh(TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	static ConstructorHelpers::FObjectFinder<UStaticMesh> CylinderMesh(TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));

	// Ball
	Collision = CreateDefaultSubobject<USphereComponent>(TEXT("Collision"));
	Collision->InitSphereRadius(BounceBallPawn::BallRadius);
	Collision->SetCollisionProfileName(UCollisionProfile::Pawn_ProfileName);
	Collision->SetCollisionResponseToChannel(ECC_PhysicsBody, ECR_Block);
	Collision->SetCanEverAffectNavigation(false);
	RootComponent = Collision;

	BallPivot = CreateDefaultSubobject<USceneComponent>(TEXT("BallPivot"));
	BallPivot->SetupAttachment(Collision);

	// Engine sphere has a 50cm radius; scale it to the collision radius.
	BallMesh = BounceBallPawn::MakeVisualMesh(this, TEXT("BallMesh"), BallPivot, SphereMesh.Object);
	BallMesh->SetRelativeScale3D(FVector(BounceBallPawn::BallRadius / 50.f));

	// Placeholder handle: a post leaning back from the top-front of the ball, with a bar across it.
	HandleMesh = BounceBallPawn::MakeVisualMesh(this, TEXT("HandleMesh"), BallPivot, CylinderMesh.Object);
	HandleMesh->SetRelativeLocation(FVector(23.f, 0.f, 41.f));
	HandleMesh->SetRelativeRotation(FRotator(-12.f, 0.f, 0.f));
	HandleMesh->SetRelativeScale3D(FVector(0.07f, 0.07f, 0.26f));

	HandleBarMesh = BounceBallPawn::MakeVisualMesh(this, TEXT("HandleBarMesh"), BallPivot, CylinderMesh.Object);
	HandleBarMesh->SetRelativeLocation(FVector(26.f, 0.f, 54.f));
	HandleBarMesh->SetRelativeRotation(FRotator(0.f, 0.f, 90.f));
	HandleBarMesh->SetRelativeScale3D(FVector(0.05f, 0.05f, 0.36f));

	SeatPoint = CreateDefaultSubobject<USceneComponent>(TEXT("SeatPoint"));
	SeatPoint->SetupAttachment(BallPivot);
	SeatPoint->SetRelativeLocation(FVector(-4.f, 0.f, BounceBallPawn::BallRadius + 10.f));

	LeftHandPoint = CreateDefaultSubobject<USceneComponent>(TEXT("LeftHandPoint"));
	LeftHandPoint->SetupAttachment(BallPivot);
	LeftHandPoint->SetRelativeLocation(FVector(22.f, -13.f, 56.f));

	RightHandPoint = CreateDefaultSubobject<USceneComponent>(TEXT("RightHandPoint"));
	RightHandPoint->SetupAttachment(BallPivot);
	RightHandPoint->SetRelativeLocation(FVector(22.f, 13.f, 56.f));

	// Rider. Attached to the root (not the squashing pivot) so squash never scales the skeleton.
	RiderMesh = CreateDefaultSubobject<USkeletalMeshComponent>(TEXT("RiderMesh"));
	RiderMesh->SetupAttachment(Collision);
	RiderMesh->SetRelativeRotation(FRotator(0.f, -90.f, 0.f));
	RiderMesh->SetAnimationMode(EAnimationMode::AnimationBlueprint);
	RiderMesh->SetAnimInstanceClass(UBounceRiderAnimInstance::StaticClass());
	RiderMesh->SetCollisionObjectType(ECC_PhysicsBody);
	RiderMesh->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	RiderMesh->SetCollisionResponseToAllChannels(ECR_Block);
	RiderMesh->SetCollisionResponseToChannel(ECC_Camera, ECR_Ignore);
	RiderMesh->SetCollisionResponseToChannel(ECC_Visibility, ECR_Ignore);
	RiderMesh->SetGenerateOverlapEvents(false);
	RiderMesh->SetCanEverAffectNavigation(false);
	RiderMesh->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;

	RiderMuscles = CreateDefaultSubobject<UPhysicalAnimationComponent>(TEXT("RiderMuscles"));
	PelvisHandle = BounceBallPawn::MakeHandle(this, TEXT("PelvisHandle"));
	LeftHandHandle = BounceBallPawn::MakeHandle(this, TEXT("LeftHandHandle"));
	RightHandHandle = BounceBallPawn::MakeHandle(this, TEXT("RightHandHandle"));

	// Camera
	CameraAnchor = CreateDefaultSubobject<USceneComponent>(TEXT("CameraAnchor"));
	CameraAnchor->SetupAttachment(Collision);
	CameraAnchor->SetUsingAbsoluteLocation(true);
	CameraAnchor->SetUsingAbsoluteRotation(true);

	CameraBoom = CreateDefaultSubobject<USpringArmComponent>(TEXT("CameraBoom"));
	CameraBoom->SetupAttachment(CameraAnchor);
	CameraBoom->SetRelativeLocation(FVector(0.f, 0.f, 80.f));
	CameraBoom->TargetArmLength = 550.f;
	CameraBoom->bUsePawnControlRotation = true;

	Camera = CreateDefaultSubobject<UCameraComponent>(TEXT("Camera"));
	Camera->SetupAttachment(CameraBoom, USpringArmComponent::SocketName);
	Camera->bUsePawnControlRotation = false;

	BallMovement = CreateDefaultSubobject<UBounceBallMovementComponent>(TEXT("BallMovement"));
	BallMovement->UpdatedComponent = Collision;

	// Seated pose for the UE mannequin (rider space: X forward, Y right, Z up).
	RiderPose = {
		{ TEXT("spine_01"), TEXT("neck_01"), FVector(0.12f, 0.f, 1.f) },
		{ TEXT("thigh_l"), TEXT("calf_l"), FVector(0.6f, -0.65f, -0.35f) },
		{ TEXT("thigh_r"), TEXT("calf_r"), FVector(0.6f, 0.65f, -0.35f) },
		{ TEXT("calf_l"), TEXT("foot_l"), FVector(0.25f, -0.1f, -1.f) },
		{ TEXT("calf_r"), TEXT("foot_r"), FVector(0.25f, 0.1f, -1.f) },
		{ TEXT("upperarm_l"), TEXT("lowerarm_l"), FVector(0.4f, -0.25f, -0.88f) },
		{ TEXT("upperarm_r"), TEXT("lowerarm_r"), FVector(0.4f, 0.25f, -0.88f) },
		{ TEXT("lowerarm_l"), TEXT("hand_l"), FVector(0.8f, 0.15f, -0.5f) },
		{ TEXT("lowerarm_r"), TEXT("hand_r"), FVector(0.8f, -0.15f, -0.5f) },
	};

	RiderMuscleSettings = {
		{ TEXT("spine_01"), true, 2000.f, 150.f },
		{ TEXT("thigh_l"), true, 1000.f, 80.f },
		{ TEXT("thigh_r"), true, 1000.f, 80.f },
		{ TEXT("clavicle_l"), true, 400.f, 40.f },
		{ TEXT("clavicle_r"), true, 400.f, 40.f },
	};
}

void ABounceBallPawn::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);

	RiderMesh->SetCollisionResponseToChannel(ECC_Pawn, bRiderCollidesWithBall ? ECR_Block : ECR_Ignore);
	if (UBounceRiderAnimInstance* RiderAnim = Cast<UBounceRiderAnimInstance>(RiderMesh->GetAnimInstance()))
	{
		RiderAnim->Aims = RiderPose;
	}
	if (!bRiderPhysicsActive)
	{
		PlaceRiderOnSeat();
	}
}

void ABounceBallPawn::PostInitializeComponents()
{
	Super::PostInitializeComponents();
	BallMovement->OnBounced.AddUniqueDynamic(this, &ABounceBallPawn::HandleBounced);
}

void ABounceBallPawn::BeginPlay()
{
	Super::BeginPlay();

	// Order each frame: ball moves -> pawn updates targets -> handles push targets to physics.
	PrimaryActorTick.AddPrerequisite(BallMovement, BallMovement->PrimaryComponentTick);
	for (UPhysicsHandleComponent* Handle : { PelvisHandle.Get(), LeftHandHandle.Get(), RightHandHandle.Get() })
	{
		Handle->PrimaryComponentTick.AddPrerequisite(this, PrimaryActorTick);
	}

	BallRadius = Collision->GetUnscaledSphereRadius();
	RespawnTransform = GetActorTransform();

	UpdateCamera(0.f, true);
	PlaceRiderOnSeat();
	RiderWarmupFrames = BounceBallPawn::RiderWarmupFrames;
}

void ABounceBallPawn::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	switch (LifeState)
	{
	case EBounceLifeState::Alive:
		UpdateBallLook(DeltaSeconds);
		break;
	case EBounceLifeState::Deflating:
		UpdateDeflating(DeltaSeconds);
		break;
	case EBounceLifeState::Dead:
		UpdateDead(DeltaSeconds);
		break;
	}

	if (UBounceRiderAnimInstance* RiderAnim = Cast<UBounceRiderAnimInstance>(RiderMesh->GetAnimInstance()))
	{
		RiderAnim->Aims = RiderPose;
	}

	if (LifeState == EBounceLifeState::Alive && bRiderRagdoll && !bRiderPhysicsActive && --RiderWarmupFrames <= 0)
	{
		ActivateRiderPhysics();
	}
	if (LifeState != EBounceLifeState::Dead)
	{
		UpdateRider();
	}
	UpdateCamera(DeltaSeconds, false);
}

// ---------- Input ----------

void ABounceBallPawn::NotifyControllerChanged()
{
	Super::NotifyControllerChanged();

	if (const APlayerController* PC = Cast<APlayerController>(Controller))
	{
		if (UEnhancedInputLocalPlayerSubsystem* Subsystem = ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(PC->GetLocalPlayer()))
		{
			for (const UInputMappingContext* Context : InputMappingContexts)
			{
				if (Context)
				{
					Subsystem->AddMappingContext(Context, 0);
				}
			}
		}
	}
}

void ABounceBallPawn::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
	Super::SetupPlayerInputComponent(PlayerInputComponent);

	UEnhancedInputComponent* Input = Cast<UEnhancedInputComponent>(PlayerInputComponent);
	if (!Input)
	{
		return;
	}
	if (MoveAction)
	{
		Input->BindAction(MoveAction, ETriggerEvent::Triggered, this, &ABounceBallPawn::Move);
	}
	if (LookAction)
	{
		Input->BindAction(LookAction, ETriggerEvent::Triggered, this, &ABounceBallPawn::Look);
	}
	if (MouseLookAction)
	{
		Input->BindAction(MouseLookAction, ETriggerEvent::Triggered, this, &ABounceBallPawn::Look);
	}
	if (JumpAction)
	{
		Input->BindAction(JumpAction, ETriggerEvent::Started, this, &ABounceBallPawn::JumpStarted);
		Input->BindAction(JumpAction, ETriggerEvent::Completed, this, &ABounceBallPawn::JumpCompleted);
		Input->BindAction(JumpAction, ETriggerEvent::Canceled, this, &ABounceBallPawn::JumpCompleted);
	}
	if (SlamAction)
	{
		Input->BindAction(SlamAction, ETriggerEvent::Started, this, &ABounceBallPawn::SlamStarted);
	}
}

void ABounceBallPawn::Move(const FInputActionValue& Value)
{
	if (!Controller || IsDead())
	{
		return;
	}
	const FVector2D Axis = Value.Get<FVector2D>();
	const FRotationMatrix YawMatrix(FRotator(0.f, Controller->GetControlRotation().Yaw, 0.f));
	AddMovementInput(YawMatrix.GetUnitAxis(EAxis::X), Axis.Y);
	AddMovementInput(YawMatrix.GetUnitAxis(EAxis::Y), Axis.X);
}

void ABounceBallPawn::Look(const FInputActionValue& Value)
{
	const FVector2D Axis = Value.Get<FVector2D>();
	AddControllerYawInput(Axis.X);
	AddControllerPitchInput(Axis.Y);
}

void ABounceBallPawn::JumpStarted()
{
	if (IsDead())
	{
		return;
	}
	BallMovement->JumpPressed();
}

void ABounceBallPawn::JumpCompleted()
{
	BallMovement->JumpReleased();
}

void ABounceBallPawn::SlamStarted()
{
	if (IsDead())
	{
		return;
	}
	BallMovement->Slam();
}

// ---------- Bounce feedback ----------

void ABounceBallPawn::HandleBounced(const FBounceImpact& Impact)
{
	if (Impact.Type != EBounceType::Wall)
	{
		// Jumps off a resting ball have no impact speed; squash a bit from the push instead.
		const float Speed = FMath::Max(Impact.ImpactSpeed, Impact.BounceSpeed * 0.5f);
		SquashAmount = -FMath::Min(Speed * SquashPerImpactSpeed, MaxSquash);
		SquashVelocity = 0.f;
	}
	ReceiveBounce(Impact);
}

void ABounceBallPawn::UpdateBallLook(float DeltaSeconds)
{
	// Damped spring back to round after each bounce.
	const float SpringAccel = -SquashStiffness * SquashAmount - SquashDamping * SquashVelocity;
	SquashVelocity += SpringAccel * DeltaSeconds;
	SquashAmount = FMath::Clamp(SquashAmount + SquashVelocity * DeltaSeconds, -MaxSquash, MaxSquash);

	const float Stretch = BallMovement->IsResting() ? 0.f : FMath::Min(FMath::Abs(BallMovement->Velocity.Z) * StretchPerSpeed, MaxStretch);
	const float Height = FMath::Max(1.f + SquashAmount + Stretch, 0.1f);
	const float Width = 1.f / FMath::Sqrt(Height);

	// Tilt toward where the player steers.
	const FVector LocalInput = GetActorQuat().UnrotateVector(GetLastMovementInputVector());
	const FRotator TargetLean(-LocalInput.X * MaxLeanAngle, 0.f, -LocalInput.Y * MaxLeanAngle);
	CurrentLean = FMath::RInterpTo(CurrentLean, TargetLean, DeltaSeconds, LeanSpeed);

	// Keep the bottom of the ball on the floor while squashed.
	const float Radius = Collision->GetScaledSphereRadius();
	BallPivot->SetRelativeLocationAndRotation(FVector(0.f, 0.f, Radius * FMath::Min(Height - 1.f, 0.f)), CurrentLean);
	BallPivot->SetRelativeScale3D(FVector(Width, Width, Height));
}

void ABounceBallPawn::UpdateCamera(float DeltaSeconds, bool bSnap)
{
	// Once the rider is thrown off, watch them instead of the empty ball.
	const bool bWatchRider = LifeState == EBounceLifeState::Dead && bRiderPhysicsActive;
	const FVector Location = bWatchRider ? RiderMesh->GetSocketLocation(PelvisBone) : GetActorLocation();
	CameraHeight = (bSnap || CameraHeightFollowSpeed <= 0.f)
		? Location.Z
		: FMath::FInterpTo(CameraHeight, Location.Z, DeltaSeconds, CameraHeightFollowSpeed);
	CameraHeight = FMath::Clamp(CameraHeight, Location.Z - CameraMaxHeightLag, Location.Z + CameraMaxHeightLag);
	CameraAnchor->SetWorldLocation(FVector(Location.X, Location.Y, CameraHeight));
}

// ---------- Rider ----------

void ABounceBallPawn::PlaceRiderOnSeat()
{
	const USkeletalMesh* Mesh = RiderMesh ? RiderMesh->GetSkeletalMeshAsset() : nullptr;
	if (!Mesh)
	{
		return;
	}
	const FReferenceSkeleton& RefSkeleton = Mesh->GetRefSkeleton();
	const int32 PelvisIndex = RefSkeleton.FindBoneIndex(PelvisBone);
	if (PelvisIndex == INDEX_NONE)
	{
		return;
	}

	// Put the mesh so its pelvis lands exactly on the seat.
	const FVector PelvisInMesh = FAnimationRuntime::GetComponentSpaceTransformRefPose(RefSkeleton, PelvisIndex).GetLocation();
	const FTransform SeatInRoot = SeatPoint->GetRelativeTransform() * BallPivot->GetRelativeTransform();
	const FVector Offset = RiderMesh->GetRelativeRotation().RotateVector(PelvisInMesh * RiderMesh->GetRelativeScale3D());
	RiderMesh->SetRelativeLocation(SeatInRoot.GetLocation() - Offset, false, nullptr, ETeleportType::TeleportPhysics);
}

void ABounceBallPawn::ActivateRiderPhysics()
{
	if (!RiderMesh->GetSkeletalMeshAsset() || !RiderMesh->GetPhysicsAsset())
	{
		return;
	}

	// A simulated mesh must not be dragged around by its parent; the handles carry it instead.
	RiderMesh->DetachFromComponent(FDetachmentTransformRules::KeepWorldTransform);
	RiderMesh->SetAllBodiesSimulatePhysics(true);
	RiderMesh->SetPhysicsBlendWeight(1.f);
	RiderMesh->WakeAllRigidBodies();

	RiderMuscles->SetSkeletalMeshComponent(RiderMesh);
	RiderMuscles->SetStrengthMultiplyer(1.f);
	for (const FBounceRiderMuscle& Muscle : RiderMuscleSettings)
	{
		FPhysicalAnimationData Data;
		Data.bIsLocalSimulation = true;
		Data.OrientationStrength = Muscle.Strength;
		Data.AngularVelocityStrength = Muscle.Damping;
		Data.PositionStrength = 0.f;
		Data.VelocityStrength = 0.f;
		if (Muscle.bIncludeChildren)
		{
			RiderMuscles->ApplyPhysicalAnimationSettingsBelow(Muscle.Bone, Data, true);
		}
		else
		{
			RiderMuscles->ApplyPhysicalAnimationSettings(Muscle.Bone, Data);
		}
	}

	// Grab in the seat's frame so the target is simply the seat transform each frame.
	PelvisHandle->GrabComponentAtLocationWithRotation(RiderMesh, PelvisBone, RiderMesh->GetSocketLocation(PelvisBone), SeatPoint->GetComponentRotation());
	LeftHandHandle->GrabComponentAtLocation(RiderMesh, LeftHandBone, RiderMesh->GetSocketLocation(LeftHandBone));
	RightHandHandle->GrabComponentAtLocation(RiderMesh, RightHandBone, RiderMesh->GetSocketLocation(RightHandBone));

	bRiderPhysicsActive = true;
}

void ABounceBallPawn::DeactivateRiderPhysics()
{
	PelvisHandle->ReleaseComponent();
	LeftHandHandle->ReleaseComponent();
	RightHandHandle->ReleaseComponent();

	RiderMesh->SetAllBodiesSimulatePhysics(false);
	RiderMesh->SetPhysicsBlendWeight(0.f);
	if (RiderMesh->GetAttachParent() != Collision)
	{
		RiderMesh->AttachToComponent(Collision, FAttachmentTransformRules::KeepWorldTransform);
	}
	RiderMesh->SetRelativeRotation(FRotator(0.f, -90.f, 0.f), false, nullptr, ETeleportType::TeleportPhysics);
	bRiderPhysicsActive = false;
}

void ABounceBallPawn::UpdateRider()
{
	if (!bRiderPhysicsActive)
	{
		return;
	}

	PelvisHandle->SetTargetLocationAndRotation(SeatPoint->GetComponentLocation(), SeatPoint->GetComponentRotation());
	LeftHandHandle->SetTargetLocation(LeftHandPoint->GetComponentLocation());
	RightHandHandle->SetTargetLocation(RightHandPoint->GetComponentLocation());

	if (LifeState == EBounceLifeState::Alive && RiderResetDistance > 0.f &&
		FVector::DistSquared(RiderMesh->GetSocketLocation(PelvisBone), SeatPoint->GetComponentLocation()) > FMath::Square(RiderResetDistance))
	{
		ResetRider();
	}
}

void ABounceBallPawn::ResetRider()
{
	DeactivateRiderPhysics();
	PlaceRiderOnSeat();
	RiderWarmupFrames = BounceBallPawn::RiderWarmupFrames;
}

void ABounceBallPawn::SetRiderRagdoll(bool bEnable)
{
	bRiderRagdoll = bEnable;
	if (!bEnable && bRiderPhysicsActive)
	{
		DeactivateRiderPhysics();
		PlaceRiderOnSeat();
	}
	RiderWarmupFrames = BounceBallPawn::RiderWarmupFrames;
}

// ---------- Death ----------

void ABounceBallPawn::Pop(FVector PopLocation)
{
	if (IsDead())
	{
		return;
	}

	LifeState = EBounceLifeState::Deflating;
	DeathTime = 0.f;
	SpinRate = TornadoSpinStart;
	SpiralAngle = FMath::FRandRange(0.f, UE_TWO_PI);
	bHandsReleased = false;

	// The pawn flies itself from here on.
	ConsumeMovementInputVector();
	BallMovement->JumpReleased();
	BallMovement->SetComponentTickEnabled(false);

	// Shoot up and away from whatever popped it, keeping a bit of the old speed.
	const FVector Away = (GetActorLocation() - PopLocation).GetSafeNormal2D();
	DeathVelocity = BallMovement->Velocity * 0.3f + Away * 300.f + FVector(0.f, 0.f, 500.f);

	// Go floppy so the limbs fling out while spinning.
	if (!bRiderPhysicsActive)
	{
		ActivateRiderPhysics();
	}
	if (bRiderPhysicsActive)
	{
		RiderMuscles->SetStrengthMultiplyer(SpinRiderMuscle);
	}

	ReceivePopped(PopLocation);
}

void ABounceBallPawn::UpdateDeflating(float DeltaSeconds)
{
	DeathTime += DeltaSeconds;
	const float Alpha = FMath::Clamp(DeathTime / DeflateDuration, 0.f, 1.f);
	const float Air = 1.f - Alpha;

	// Escaping air is a jet that corkscrews around, mostly upward, weakening as the ball empties.
	SpiralAngle += UE_TWO_PI * DeflateSpiralRate * DeltaSeconds;
	const FVector JetDir = FVector(FMath::Cos(SpiralAngle) * DeflateWildness, FMath::Sin(SpiralAngle) * DeflateWildness, 1.f).GetSafeNormal();
	const float Gravity = BallMovement->GetGravityZ() * FMath::Lerp(DeflateGravityScale, 1.f, Alpha);
	DeathVelocity += (JetDir * DeflateThrust * FMath::Sqrt(Air) + FVector(0.f, 0.f, Gravity)) * DeltaSeconds;
	DeathVelocity *= FMath::Exp(-DeflateDrag * DeltaSeconds);

	// Tornado spin that winds up as the air runs out.
	SpinRate = FMath::Lerp(TornadoSpinStart, TornadoSpinMax, FMath::Sin(Alpha * UE_HALF_PI));
	const FRotator Rotation(0.f, GetActorRotation().Yaw + SpinRate * DeltaSeconds, 0.f);
	MoveDeadBall(DeathVelocity, Rotation.Quaternion(), BounceBallPawn::DeflatingRestitution, DeltaSeconds);

	// Shrink with a rubbery flutter, wobbling off upright like a spinning top.
	const float Flutter = 1.f + 0.07f * Air * FMath::Sin(DeathTime * 45.f);
	const float Scale = FMath::Lerp(1.f, DeflatedScale, FMath::Pow(Alpha, 1.3f)) * Flutter;
	const float WobbleIn = FMath::Min(Alpha * 4.f, 1.f);
	const FRotator Tilt(TornadoWobble * WobbleIn * FMath::Sin(DeathTime * 7.f), 0.f, TornadoWobble * WobbleIn * FMath::Cos(DeathTime * 5.f));
	SetBallSize(Scale, FVector::OneVector, Tilt);

	if (!bHandsReleased && Alpha >= HandsLetGoAt)
	{
		LeftHandHandle->ReleaseComponent();
		RightHandHandle->ReleaseComponent();
		bHandsReleased = true;
	}

	if (Alpha >= 1.f)
	{
		Die();
	}
}

void ABounceBallPawn::Die()
{
	LifeState = EBounceLifeState::Dead;
	DeathTime = 0.f;

	PelvisHandle->ReleaseComponent();
	LeftHandHandle->ReleaseComponent();
	RightHandHandle->ReleaseComponent();

	if (bRiderPhysicsActive)
	{
		// Fully limp, flung off along the spin.
		RiderMuscles->SetStrengthMultiplyer(0.f);

		FVector Radial = RiderMesh->GetSocketLocation(PelvisBone) - GetActorLocation();
		Radial.Z = 0.f;
		Radial = Radial.SizeSquared() > 1.f ? Radial.GetSafeNormal() : GetActorForwardVector();
		const FVector Tangent = FVector::CrossProduct(FVector::UpVector, Radial);
		const FVector Fling = (Tangent + Radial * 0.5f).GetSafeNormal() * RiderFlingSpeed + FVector(0.f, 0.f, RiderFlingSpeed * 0.5f);
		RiderMesh->SetAllPhysicsLinearVelocity(Fling, true);
		RiderMesh->SetAllPhysicsAngularVelocityInDegrees(FVector(0.f, 0.f, SpinRate * 0.3f), true);
	}

	ReceiveDied();
}

void ABounceBallPawn::UpdateDead(float DeltaSeconds)
{
	DeathTime += DeltaSeconds;

	// The empty skin flops down and lies flat.
	SpinRate *= FMath::Exp(-2.f * DeltaSeconds);
	DeathVelocity.Z = FMath::Max(DeathVelocity.Z + BallMovement->GetGravityZ() * DeltaSeconds, -BallMovement->MaxFallSpeed);
	DeathVelocity.X *= FMath::Exp(-1.5f * DeltaSeconds);
	DeathVelocity.Y *= FMath::Exp(-1.5f * DeltaSeconds);
	const FRotator Rotation(0.f, GetActorRotation().Yaw + SpinRate * DeltaSeconds, 0.f);
	MoveDeadBall(DeathVelocity, Rotation.Quaternion(), BounceBallPawn::SkinRestitution, DeltaSeconds);
	SetBallSize(DeflatedScale, BounceBallPawn::SkinSquash, FRotator::ZeroRotator);

	if (RespawnDelay > 0.f && DeathTime >= RespawnDelay)
	{
		Respawn();
	}
}

void ABounceBallPawn::MoveDeadBall(FVector& InOutVelocity, const FQuat& Rotation, float Restitution, float DeltaSeconds)
{
	FVector Delta = InOutVelocity * DeltaSeconds;
	for (int32 Iteration = 0; Iteration < 3 && !Delta.IsNearlyZero(); ++Iteration)
	{
		FHitResult Hit;
		BallMovement->SafeMoveUpdatedComponent(Delta, Rotation, true, Hit);
		if (!Hit.IsValidBlockingHit())
		{
			break;
		}
		const float IntoSurface = InOutVelocity | Hit.Normal;
		if (IntoSurface < 0.f)
		{
			InOutVelocity -= (1.f + Restitution) * IntoSurface * Hit.Normal;
		}
		Delta = InOutVelocity * DeltaSeconds * (1.f - Hit.Time);
	}
	// Keep Velocity meaningful for anything reading it (camera, telemetry).
	BallMovement->Velocity = InOutVelocity;
}

void ABounceBallPawn::SetBallSize(float Scale, const FVector& Squash, const FRotator& Tilt)
{
	Collision->SetSphereRadius(BallRadius * Scale * Squash.Z);
	BallPivot->SetRelativeLocationAndRotation(FVector::ZeroVector, Tilt);
	BallPivot->SetRelativeScale3D(Squash * Scale);
}

void ABounceBallPawn::Respawn()
{
	LifeState = EBounceLifeState::Alive;
	DeathTime = 0.f;
	DeathVelocity = FVector::ZeroVector;
	SquashAmount = 0.f;
	SquashVelocity = 0.f;
	CurrentLean = FRotator::ZeroRotator;

	SetBallSize(1.f, FVector::OneVector, FRotator::ZeroRotator);
	const float SpawnYaw = RespawnTransform.Rotator().Yaw;
	SetActorLocationAndRotation(RespawnTransform.GetLocation(), FRotator(0.f, SpawnYaw, 0.f), false, nullptr, ETeleportType::TeleportPhysics);

	ConsumeMovementInputVector();
	BallMovement->Launch(FVector::ZeroVector, true, true);
	BallMovement->SetComponentTickEnabled(true);
	if (Controller)
	{
		Controller->SetControlRotation(FRotator(Controller->GetControlRotation().Pitch, SpawnYaw, 0.f));
	}

	ResetRider();
	UpdateCamera(0.f, true);
	ReceiveRespawned();
}
