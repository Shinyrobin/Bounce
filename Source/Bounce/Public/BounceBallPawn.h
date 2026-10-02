// The player: a hopper ball with an active-ragdoll rider sitting on it and holding the handles.
//
// Artist / designer workflow: make a Blueprint child of this class (BP_BounceBall) and
//  - swap BallMesh / HandleMesh / RiderMesh for real art,
//  - drag SeatPoint / LeftHandPoint / RightHandPoint in the viewport to fit the art,
//  - tune bounce feel on the BallMovement component,
//  - hook sounds / VFX up in the "On Bounce" event.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Pawn.h"
#include "PhysicsEngine/PhysicalAnimationComponent.h"
#include "BounceBallMovementComponent.h"
#include "BounceRiderAnimInstance.h"
#include "BounceBallPawn.generated.h"

class UCameraComponent;
class UInputAction;
class UInputMappingContext;
class UPhysicsHandleComponent;
class USphereComponent;
class USpringArmComponent;
struct FInputActionValue;

/** Muscle strength for a group of rider bones (the bone and, optionally, everything below it). */
USTRUCT(BlueprintType)
struct FBounceRiderMuscle
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rider")
	FName Bone;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rider")
	bool bIncludeChildren = true;

	/** How hard this part pulls toward the seated pose. Higher = stiffer, lower = floppier. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rider")
	float Strength = 1000.f;

	/** Resists spinning; higher = less wobble. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rider")
	float Damping = 100.f;
};

UENUM(BlueprintType)
enum class EBounceLifeState : uint8
{
	Alive,
	/** Popped: the ball whizzes around like a let-go balloon while it shrinks. */
	Deflating,
	/** The rider has been thrown off as a limp ragdoll; waiting to respawn. */
	Dead
};

UCLASS()
class BOUNCE_API ABounceBallPawn : public APawn
{
	GENERATED_BODY()

public:
	ABounceBallPawn();

	// ---------- Components ----------

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<USphereComponent> Collision;

	/** Squash / lean pivot. Everything that should squash with the ball goes under this. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<USceneComponent> BallPivot;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<UStaticMeshComponent> BallMesh;

	/** Placeholder handle post. Replace or hide when the ball art has its own handle. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<UStaticMeshComponent> HandleMesh;

	/** Placeholder handle bar the hands grip. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<UStaticMeshComponent> HandleBarMesh;

	/** Where the rider's pelvis sits. Move it to fit the art; the rider follows. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<USceneComponent> SeatPoint;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<USceneComponent> LeftHandPoint;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<USceneComponent> RightHandPoint;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<USkeletalMeshComponent> RiderMesh;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<UPhysicalAnimationComponent> RiderMuscles;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<UPhysicsHandleComponent> PelvisHandle;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<UPhysicsHandleComponent> LeftHandHandle;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<UPhysicsHandleComponent> RightHandHandle;

	/** Follows the ball with smoothed height so the camera doesn't bob on every bounce. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<USceneComponent> CameraAnchor;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<USpringArmComponent> CameraBoom;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<UCameraComponent> Camera;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<UBounceBallMovementComponent> BallMovement;

	// ---------- Input ----------

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Input")
	TArray<TObjectPtr<UInputMappingContext>> InputMappingContexts;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Input")
	TObjectPtr<UInputAction> MoveAction;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Input")
	TObjectPtr<UInputAction> LookAction;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Input")
	TObjectPtr<UInputAction> MouseLookAction;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Input")
	TObjectPtr<UInputAction> JumpAction;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Input")
	TObjectPtr<UInputAction> SlamAction;

	// ---------- Camera ----------

	/** How quickly the camera catches up to the ball's height. Lower = calmer camera while bouncing. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera", meta = (ClampMin = "0"))
	float CameraHeightFollowSpeed = 3.f;

	/** The camera never lags further than this (cm) above or below the ball. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera", meta = (ClampMin = "0", Units = "cm"))
	float CameraMaxHeightLag = 250.f;

	// ---------- Ball look ----------

	/** Squash amount per cm/s of impact speed. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ball Look", meta = (ClampMin = "0"))
	float SquashPerImpactSpeed = 0.00025f;

	/** Maximum squash (0.3 = 30% flatter). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ball Look", meta = (ClampMin = "0", ClampMax = "0.9"))
	float MaxSquash = 0.35f;

	/** Springiness of the jiggle after a bounce. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ball Look", meta = (ClampMin = "0"))
	float SquashStiffness = 300.f;

	/** How quickly the jiggle dies out. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ball Look", meta = (ClampMin = "0"))
	float SquashDamping = 9.f;

	/** Stretch amount per cm/s of vertical speed while flying. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ball Look", meta = (ClampMin = "0"))
	float StretchPerSpeed = 0.00005f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ball Look", meta = (ClampMin = "0", ClampMax = "0.9"))
	float MaxStretch = 0.12f;

	/** How far (degrees) the ball tilts toward the steering direction. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ball Look", meta = (ClampMin = "0", ClampMax = "45", Units = "deg"))
	float MaxLeanAngle = 14.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ball Look", meta = (ClampMin = "0"))
	float LeanSpeed = 6.f;

	// ---------- Rider ----------

	/** Physically simulate the rider. Off = rider just holds the seated pose rigidly. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Rider")
	bool bRiderRagdoll = true;

	/** Rider limbs collide with the ball. Off by default: the thighs start overlapping it and get kicked away. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Rider")
	bool bRiderCollidesWithBall = false;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Rider|Bones")
	FName PelvisBone = TEXT("pelvis");

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Rider|Bones")
	FName LeftHandBone = TEXT("hand_l");

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Rider|Bones")
	FName RightHandBone = TEXT("hand_r");

	/** Seated pose the rider's muscles pull toward (used when the rider has no custom Anim Blueprint). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rider")
	TArray<FBounceRiderBoneAim> RiderPose;

	/** Muscle strength per body part, applied in order (later entries override earlier ones).
	 *  The pelvis is held by PelvisHandle, so it needs no muscle. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rider")
	TArray<FBounceRiderMuscle> RiderMuscleSettings;

	/** If the rider gets pulled this far (cm) from the seat, snap them back on. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rider", meta = (ClampMin = "0", Units = "cm"))
	float RiderResetDistance = 150.f;

	// ---------- Death ----------

	/** How long (s) the popped ball flies around deflating before the rider is thrown off. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Death", meta = (ClampMin = "0.1", Units = "s"))
	float DeflateDuration = 1.6f;

	/** Push of the escaping air (cm/s^2). Fades out as the ball empties. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Death", meta = (ClampMin = "0"))
	float DeflateThrust = 3000.f;

	/** How sideways the air jet points (0 = straight up, 1 = 45 degrees). Higher = wider corkscrew. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Death", meta = (ClampMin = "0", ClampMax = "3"))
	float DeflateWildness = 1.1f;

	/** Corkscrew turns per second of the flight path. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Death", meta = (ClampMin = "0"))
	float DeflateSpiralRate = 0.9f;

	/** Air drag while deflating. Higher = tighter, slower flight. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Death", meta = (ClampMin = "0"))
	float DeflateDrag = 1.6f;

	/** Fraction of the ball's gravity while it still has air in it. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Death", meta = (ClampMin = "0", ClampMax = "1"))
	float DeflateGravityScale = 0.6f;

	/** Spin (degrees/s) right after the pop... */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Death", meta = (ClampMin = "0"))
	float TornadoSpinStart = 600.f;

	/** ...building up to this (degrees/s) as the ball empties. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Death", meta = (ClampMin = "0"))
	float TornadoSpinMax = 2200.f;

	/** How far (degrees) the spinning ball wobbles off upright. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Death", meta = (ClampMin = "0", ClampMax = "80", Units = "deg"))
	float TornadoWobble = 28.f;

	/** Size of the empty ball skin (fraction of full size). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Death", meta = (ClampMin = "0.05", ClampMax = "1"))
	float DeflatedScale = 0.22f;

	/** Rider muscle strength while spinning (fraction). Low = limbs fly out. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Death", meta = (ClampMin = "0", ClampMax = "1"))
	float SpinRiderMuscle = 0.15f;

	/** Point in the deflation (0..1) where the rider's hands lose their grip. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Death", meta = (ClampMin = "0", ClampMax = "1"))
	float HandsLetGoAt = 0.6f;

	/** Extra speed (cm/s) the rider is flung with when thrown off. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Death", meta = (ClampMin = "0", Units = "cm/s"))
	float RiderFlingSpeed = 700.f;

	/** Respawn automatically this long (s) after the rider is thrown off. 0 = never (call Respawn yourself). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Death", meta = (ClampMin = "0", Units = "s"))
	float RespawnDelay = 2.5f;

	// ---------- Events / API ----------

	/** Called on every impact. Hook up sounds, particles, camera shakes here. */
	UFUNCTION(BlueprintImplementableEvent, Category = "Bounce", meta = (DisplayName = "On Bounce"))
	void ReceiveBounce(const FBounceImpact& Impact);

	/** Called the moment the ball pops. Hook up the bang + air hiss here. */
	UFUNCTION(BlueprintImplementableEvent, Category = "Death", meta = (DisplayName = "On Popped"))
	void ReceivePopped(FVector PopLocation);

	/** Called when the rider is thrown off and goes limp. */
	UFUNCTION(BlueprintImplementableEvent, Category = "Death", meta = (DisplayName = "On Died"))
	void ReceiveDied();

	UFUNCTION(BlueprintImplementableEvent, Category = "Death", meta = (DisplayName = "On Respawned"))
	void ReceiveRespawned();

	/** Pop the ball (e.g. touched a laser). PopLocation is where it was hit; the ball shoots away from it. */
	UFUNCTION(BlueprintCallable, Category = "Death")
	void Pop(FVector PopLocation);

	/** Put a fresh ball and rider back at the respawn point. */
	UFUNCTION(BlueprintCallable, Category = "Death")
	void Respawn();

	/** Where Respawn puts the player. Defaults to where the pawn started; call this for checkpoints. */
	UFUNCTION(BlueprintCallable, Category = "Death")
	void SetRespawnTransform(const FTransform& NewRespawnTransform) { RespawnTransform = NewRespawnTransform; }

	/** True from the moment the ball pops until it respawns. */
	UFUNCTION(BlueprintPure, Category = "Death")
	bool IsDead() const { return LifeState != EBounceLifeState::Alive; }

	UFUNCTION(BlueprintPure, Category = "Death")
	EBounceLifeState GetLifeState() const { return LifeState; }

	/** Snap the rider back onto the seat (e.g. after a respawn). */
	UFUNCTION(BlueprintCallable, Category = "Rider")
	void ResetRider();

	UFUNCTION(BlueprintCallable, Category = "Rider")
	void SetRiderRagdoll(bool bEnable);

	//~ AActor / APawn
	virtual void OnConstruction(const FTransform& Transform) override;
	virtual void PostInitializeComponents() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void SetupPlayerInputComponent(UInputComponent* PlayerInputComponent) override;
	virtual void NotifyControllerChanged() override;
	virtual UPawnMovementComponent* GetMovementComponent() const override { return BallMovement; }

protected:
	virtual void BeginPlay() override;

	void Move(const FInputActionValue& Value);
	void Look(const FInputActionValue& Value);
	void JumpStarted();
	void JumpCompleted();
	void SlamStarted();

	UFUNCTION()
	void HandleBounced(const FBounceImpact& Impact);

private:
	void PlaceRiderOnSeat();
	void ActivateRiderPhysics();
	void DeactivateRiderPhysics();
	void UpdateRider();
	void UpdateBallLook(float DeltaSeconds);
	void UpdateCamera(float DeltaSeconds, bool bSnap);
	void UpdateDeflating(float DeltaSeconds);
	void UpdateDead(float DeltaSeconds);
	void Die();
	/** Moves the ball by Velocity with a sweep, bouncing off whatever it hits. */
	void MoveDeadBall(FVector& InOutVelocity, const FQuat& Rotation, float Restitution, float DeltaSeconds);
	void SetBallSize(float Scale, const FVector& Squash, const FRotator& Tilt);

	float SquashAmount = 0.f;
	float SquashVelocity = 0.f;
	FRotator CurrentLean = FRotator::ZeroRotator;
	float CameraHeight = 0.f;
	int32 RiderWarmupFrames = 0;
	bool bRiderPhysicsActive = false;

	EBounceLifeState LifeState = EBounceLifeState::Alive;
	FTransform RespawnTransform = FTransform::Identity;
	float BallRadius = 0.f;
	float DeathTime = 0.f;
	float SpinRate = 0.f;
	float SpiralAngle = 0.f;
	FVector DeathVelocity = FVector::ZeroVector;
	bool bHandsReleased = false;
};
