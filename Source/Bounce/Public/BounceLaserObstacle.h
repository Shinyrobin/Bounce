// A laser beam strung between two posts. If the ball touches the beam it pops (see ABounceBallPawn::Pop).
//
// Designer workflow: drop BP_LaserObstacle (or this class) into the level with its origin on the floor, then
//  - set Length / Beam Height (the default height is too high for a normal hop: you must Jump over it),
//  - optionally make the beam slide (Move Offset / Move Period) or blink (Blink / On Time / Off Time),
//  - hook sounds / sparks up in the "On Zapped" event.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "BounceLaserObstacle.generated.h"

class ABounceBallPawn;
class UBoxComponent;
class UMaterialInstanceDynamic;
class UMaterialInterface;
class UPointLightComponent;

UCLASS()
class BOUNCE_API ABounceLaserObstacle : public AActor
{
	GENERATED_BODY()

public:
	ABounceLaserObstacle();

	// ---------- Components ----------

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<USceneComponent> Root;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<UStaticMeshComponent> LeftPost;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<UStaticMeshComponent> RightPost;

	/** Everything that moves with the beam (Move Offset) lives under this. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<USceneComponent> BeamPivot;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<UStaticMeshComponent> Beam;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<UStaticMeshComponent> LeftEmitter;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<UStaticMeshComponent> RightEmitter;

	/** Touching this pops the ball. Sized from Length / Kill Thickness. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<UBoxComponent> KillZone;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<UPointLightComponent> Glow;

	// ---------- Laser ----------

	/** Distance between the two posts (cm). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Laser", meta = (ClampMin = "50", Units = "cm"))
	float Length = 500.f;

	/** Height of the beam above the actor's origin (cm). A plain hop reaches ~90cm, a Jump ~380cm. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Laser", meta = (ClampMin = "0", Units = "cm"))
	float BeamHeight = 100.f;

	/** Visual thickness of the beam (cm). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Laser", meta = (ClampMin = "0.5", Units = "cm"))
	float BeamThickness = 5.f;

	/** Thickness of the deadly zone around the beam (cm). A bit thicker than the visual is fairer at speed. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Laser", meta = (ClampMin = "1", Units = "cm"))
	float KillThickness = 12.f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Laser")
	FLinearColor Color = FLinearColor(1.f, 0.03f, 0.02f);

	/** Emissive brightness of the beam. Higher = more bloom. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Laser", meta = (ClampMin = "0"))
	float GlowStrength = 40.f;

	/** Material for the beam and emitters. Needs a "Color" vector and "Glow" scalar parameter. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Laser")
	TObjectPtr<UMaterialInterface> BeamMaterial;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Laser")
	bool bShowPosts = true;

	/** Cast coloured light on the floor around the beam. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Laser")
	bool bGlowLight = true;

	// ---------- Movement ----------

	/** The beam slides back and forth between its start and start + this offset (cm). (0,0,0) = still.
	 *  Z moves it up and down; X sweeps it across the floor. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Laser|Movement")
	FVector MoveOffset = FVector::ZeroVector;

	/** Seconds for one full back-and-forth. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Laser|Movement", meta = (ClampMin = "0.1", Units = "s"))
	float MovePeriod = 3.f;

	/** Where in the cycle it starts (0..1). Use different values to desync several lasers. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Laser|Movement", meta = (ClampMin = "0", ClampMax = "1"))
	float MovePhase = 0.f;

	// ---------- Blink ----------

	/** Turn the beam on and off on a timer. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Laser|Blink")
	bool bBlink = false;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Laser|Blink", meta = (ClampMin = "0.05", Units = "s", EditCondition = "bBlink"))
	float OnTime = 2.f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Laser|Blink", meta = (ClampMin = "0.05", Units = "s", EditCondition = "bBlink"))
	float OffTime = 1.5f;

	/** Before switching on, a faint harmless beam flickers for this long as a warning. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Laser|Blink", meta = (ClampMin = "0", Units = "s", EditCondition = "bBlink"))
	float WarningTime = 0.5f;

	/** Seconds into the on/off cycle at the start. Use different values to desync several lasers. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Laser|Blink", meta = (ClampMin = "0", Units = "s", EditCondition = "bBlink"))
	float BlinkOffset = 0.f;

	// ---------- Events / API ----------

	/** Called when this laser pops the player. Hook up a zap sound / sparks here. */
	UFUNCTION(BlueprintImplementableEvent, Category = "Laser", meta = (DisplayName = "On Zapped"))
	void ReceiveZapped(ABounceBallPawn* Player, FVector ZapLocation);

	/** Force the beam on or off (overrides blinking until called with bOn again). */
	UFUNCTION(BlueprintCallable, Category = "Laser")
	void SetLaserEnabled(bool bEnabled);

	UFUNCTION(BlueprintPure, Category = "Laser")
	bool IsLaserOn() const { return bLaserOn; }

	//~ AActor
	virtual void OnConstruction(const FTransform& Transform) override;
	virtual void Tick(float DeltaSeconds) override;

protected:
	virtual void BeginPlay() override;

	UFUNCTION()
	void HandleKillZoneOverlap(UPrimitiveComponent* OverlappedComponent, AActor* OtherActor, UPrimitiveComponent* OtherComp,
		int32 OtherBodyIndex, bool bFromSweep, const FHitResult& SweepResult);

private:
	void SetupMaterial();
	void BuildLayout();
	void ApplyBeamState(bool bOn, float Brightness);
	void Zap(AActor* OtherActor);

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> BeamMID;

	float Age = 0.f;
	bool bLaserOn = true;
	bool bEnabled = true;
	float LightIntensity = 0.f;
};
