// Movement for the hopper ball: gravity, steering, and the bounce rules (hop / jump / slam / walls).
// All tuning lives in UPROPERTYs so designers can tweak it on the Blueprint without touching code.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PawnMovementComponent.h"
#include "BounceBallMovementComponent.generated.h"

UENUM(BlueprintType)
enum class EBounceType : uint8
{
	/** Plain automatic hop / restitution bounce off the floor. */
	Hop,
	/** Bounce boosted by the Jump input. */
	Jump,
	/** Bounce coming out of a Slam (ground pound). */
	Slam,
	/** Hit a wall or ceiling. */
	Wall,
	/** Touched the floor too softly to bounce; the ball is now resting / rolling. */
	Settle
};

USTRUCT(BlueprintType)
struct FBounceImpact
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Bounce")
	EBounceType Type = EBounceType::Hop;

	/** Speed into the surface (cm/s) right before the impact. */
	UPROPERTY(BlueprintReadOnly, Category = "Bounce")
	float ImpactSpeed = 0.f;

	/** Speed away from the surface (cm/s) right after the impact. */
	UPROPERTY(BlueprintReadOnly, Category = "Bounce")
	float BounceSpeed = 0.f;

	UPROPERTY(BlueprintReadOnly, Category = "Bounce")
	FVector ImpactPoint = FVector::ZeroVector;

	UPROPERTY(BlueprintReadOnly, Category = "Bounce")
	FVector ImpactNormal = FVector::UpVector;

	UPROPERTY(BlueprintReadOnly, Category = "Bounce")
	FHitResult Hit;
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnBallBounced, const FBounceImpact&, Impact);

UCLASS(ClassGroup = Movement, meta = (BlueprintSpawnableComponent))
class BOUNCE_API UBounceBallMovementComponent : public UPawnMovementComponent
{
	GENERATED_BODY()

public:
	UBounceBallMovementComponent();

	// ---------- Steering ----------

	/** Top horizontal speed reached by steering alone (cm/s). Launches/slopes can exceed it. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Bounce|Steering", meta = (ClampMin = "0", Units = "cm/s"))
	float MaxSpeed = 900.f;

	/** How fast steering builds horizontal speed (cm/s^2). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Bounce|Steering", meta = (ClampMin = "0"))
	float Acceleration = 2200.f;

	/** How fast the ball slows when there is no input (cm/s^2). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Bounce|Steering", meta = (ClampMin = "0"))
	float Deceleration = 900.f;

	/** Multiplier on Acceleration/Deceleration while in the air (0 = no air control, 1 = same as ground). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Bounce|Steering", meta = (ClampMin = "0", ClampMax = "2"))
	float AirControl = 0.75f;

	/** On each floor bounce, how much of the horizontal velocity snaps toward the stick direction (0..1). Makes turning feel snappy. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Bounce|Steering", meta = (ClampMin = "0", ClampMax = "1"))
	float BounceTurnAssist = 0.35f;

	/** Rotate the ball (and rider) to face the direction it is steering. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Bounce|Steering")
	bool bOrientToMovement = true;

	/** Turn speed when orienting to movement (degrees/s). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Bounce|Steering", meta = (ClampMin = "0", EditCondition = "bOrientToMovement"))
	float RotationRate = 540.f;

	// ---------- Gravity ----------

	/** Multiplier on world gravity. Platformers usually feel better a bit heavier than real life. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Bounce|Gravity", meta = (ClampMin = "0"))
	float GravityScale = 1.6f;

	/** Terminal fall speed (cm/s). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Bounce|Gravity", meta = (ClampMin = "0", Units = "cm/s"))
	float MaxFallSpeed = 4000.f;

	// ---------- Bouncing ----------

	/** Always hop when touching the floor, like a real hopper ball. If off, the ball can come to rest and roll. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Bounce|Bouncing")
	bool bAutoHop = true;

	/** Minimum height (cm) of the automatic hop. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Bounce|Bouncing", meta = (ClampMin = "0", Units = "cm", EditCondition = "bAutoHop"))
	float HopHeight = 90.f;

	/** Fraction of impact speed kept on a normal floor bounce (0 = dead, 1 = perfectly elastic). Lets big falls carry into big bounces. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Bounce|Bouncing", meta = (ClampMin = "0", ClampMax = "1"))
	float Restitution = 0.55f;

	/** Bounces slower than this (cm/s) settle onto the floor instead (only matters when Auto Hop is off). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Bounce|Bouncing", meta = (ClampMin = "0", Units = "cm/s"))
	float MinBounceSpeed = 150.f;

	/** Fraction of along-surface speed lost on each floor bounce. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Bounce|Bouncing", meta = (ClampMin = "0", ClampMax = "1"))
	float BounceFriction = 0.05f;

	/** 0 = bounce straight off the surface normal (slopes push you downhill), 1 = always bounce straight up. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Bounce|Bouncing", meta = (ClampMin = "0", ClampMax = "1"))
	float UprightBias = 0.6f;

	/** Surfaces at most this steep count as floor; steeper ones are walls. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Bounce|Bouncing", meta = (ClampMin = "0", ClampMax = "89", Units = "deg"))
	float MaxFloorAngle = 50.f;

	/** Fraction of impact speed kept when hitting a wall or ceiling. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Bounce|Bouncing", meta = (ClampMin = "0", ClampMax = "1"))
	float WallRestitution = 0.6f;

	/** No bounce will ever go higher than this (cm). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Bounce|Bouncing", meta = (ClampMin = "0", Units = "cm"))
	float MaxBounceHeight = 1600.f;

	// ---------- Jump ----------

	/** Height (cm) of a bounce boosted by Jump. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Bounce|Jump", meta = (ClampMin = "0", Units = "cm"))
	float JumpHeight = 380.f;

	/** Fraction of impact speed kept on a Jump bounce. Timing a jump after a big fall keeps the momentum ("carry through"). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Bounce|Jump", meta = (ClampMin = "0", ClampMax = "1.5"))
	float JumpRestitution = 0.9f;

	/** Holding Jump makes every floor bounce a Jump bounce. If off, Jump must be pressed close to landing. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Bounce|Jump")
	bool bHoldJumpForBigBounces = true;

	/** Pressing Jump this long (s) before touching down still counts. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Bounce|Jump", meta = (ClampMin = "0", Units = "s"))
	float JumpBufferTime = 0.2f;

	/** Pressing Jump this long (s) after a normal hop upgrades it to a Jump bounce. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Bounce|Jump", meta = (ClampMin = "0", Units = "s"))
	float JumpLateTime = 0.12f;

	// ---------- Slam ----------

	/** Allow the Slam (ground pound) move. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Bounce|Slam")
	bool bCanSlam = true;

	/** Downward speed (cm/s) when slamming. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Bounce|Slam", meta = (ClampMin = "0", Units = "cm/s", EditCondition = "bCanSlam"))
	float SlamSpeed = 2600.f;

	/** Fraction of horizontal speed kept when starting a slam. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Bounce|Slam", meta = (ClampMin = "0", ClampMax = "1", EditCondition = "bCanSlam"))
	float SlamHorizontalKeep = 0.15f;

	/** Minimum height (cm) of the bounce coming out of a slam. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Bounce|Slam", meta = (ClampMin = "0", Units = "cm", EditCondition = "bCanSlam"))
	float SlamBounceHeight = 650.f;

	/** Fraction of impact speed kept on the slam bounce. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Bounce|Slam", meta = (ClampMin = "0", ClampMax = "1.5", EditCondition = "bCanSlam"))
	float SlamRestitution = 0.8f;

	// ---------- Events ----------

	/** Fired on every impact (floor bounces, walls, settling). */
	UPROPERTY(BlueprintAssignable, Category = "Bounce")
	FOnBallBounced OnBounced;

	// ---------- API ----------

	UFUNCTION(BlueprintCallable, Category = "Bounce")
	void JumpPressed();

	UFUNCTION(BlueprintCallable, Category = "Bounce")
	void JumpReleased();

	/** Start a ground pound (only while airborne). Returns true if it started. */
	UFUNCTION(BlueprintCallable, Category = "Bounce")
	bool Slam();

	/** Launch the ball, e.g. from a bounce pad. */
	UFUNCTION(BlueprintCallable, Category = "Bounce")
	void Launch(FVector LaunchVelocity, bool bOverrideHorizontal = false, bool bOverrideVertical = true);

	UFUNCTION(BlueprintPure, Category = "Bounce")
	bool IsSlamming() const { return bSlamming; }

	UFUNCTION(BlueprintPure, Category = "Bounce")
	bool IsJumpHeld() const { return bJumpHeld; }

	/** True while the ball is resting / rolling on the floor (not bouncing). */
	UFUNCTION(BlueprintPure, Category = "Bounce")
	bool IsResting() const { return bResting; }

	/** Launch speed (cm/s) needed to reach a height with the current gravity. */
	UFUNCTION(BlueprintPure, Category = "Bounce")
	float GetSpeedForHeight(float Height) const;

	//~ UMovementComponent
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;
	virtual float GetGravityZ() const override;
	virtual float GetMaxSpeed() const override { return MaxSpeed; }
	virtual bool IsFalling() const override { return !bResting; }
	virtual bool IsMovingOnGround() const override { return bResting; }

private:
	void ApplySteering(const FVector& Input, float DeltaTime);
	void ResolveImpact(const FHitResult& Hit, const FVector& Input);
	void ApplyRotation(const FVector& Input, float DeltaTime);
	bool IsFloor(const FVector& Normal) const;
	void Notify(EBounceType Type, float InSpeed, float OutSpeed, const FHitResult& Hit);

	float JumpBufferRemaining = 0.f;
	float TimeSinceFloorBounce = BIG_NUMBER;
	float RestingGraceRemaining = 0.f;
	bool bJumpHeld = false;
	bool bSlamming = false;
	bool bResting = false;
	bool bLastBounceUpgradable = false;
};
