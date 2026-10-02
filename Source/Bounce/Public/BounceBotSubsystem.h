// BounceBot: a scripted playtester. Runs a JSON route (move / jump / slam / look / shots / checks ...)
// against the player's ball, records telemetry and screenshots, and writes a compact report.
//
// Ways to start a run:
//  - Editor: the BounceEditor module serves http://127.0.0.1:8765/run (used by the bouncebot MCP server).
//  - Standalone: UnrealEditor.exe Bounce.uproject <Map> -game -BounceBot=<script.json> [-BounceBotOut=<dir>]
//    (quits when the run ends).
//  - Console, while playing: BounceBot.Run <script.json>
//
// Script reference: Tools/BounceBot/README.md

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Tickable.h"
#include "BounceBallMovementComponent.h"
#include "BounceBotSubsystem.generated.h"

class ABounceBallPawn;
class ACameraActor;
class APlayerController;
class FJsonObject;
class FJsonValue;
class UEnhancedInputLocalPlayerSubsystem;
class FBounceBotLogCapture;

DECLARE_MULTICAST_DELEGATE_OneParam(FOnBounceBotFinished, const FString& /*ReportJson*/);

BOUNCE_API DECLARE_LOG_CATEGORY_EXTERN(LogBounceBot, Log, All);

UCLASS()
class BOUNCE_API UBounceBotSubsystem : public UGameInstanceSubsystem, public FTickableGameObject
{
	GENERATED_BODY()

public:
	/** Starts a run. OutDir receives report.json, trace.csv and the shots (empty = Saved/BounceBot/Runs/<time>). */
	bool StartRun(const FString& ScriptJson, const FString& OutDir, FString& OutError);

	/** Ends the current run early; the report is still written and OnFinished still fires. */
	void StopRun(const FString& Reason);

	bool IsRunning() const { return bRunning; }

	/** True once the player has a ball pawn, i.e. a run can start. */
	bool IsReady() const;

	/** Fired once per run with the report (JSON). */
	FOnBounceBotFinished OnFinished;

	//~ USubsystem
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	//~ FTickableGameObject
	virtual void Tick(float DeltaTime) override;
	virtual ETickableTickType GetTickableTickType() const override;
	virtual bool IsTickable() const override;
	virtual UWorld* GetTickableGameObjectWorld() const override;
	virtual TStatId GetStatId() const override;

private:
	struct FShot
	{
		FString File;
		FString Label;
		float Time = 0.f;
		int32 Step = 0;
		int32 Frames = 1;
	};

	struct FBounceRecord
	{
		float Time = 0.f;
		EBounceType Type = EBounceType::Hop;
		float In = 0.f;
		float Out = 0.f;
		FVector Location = FVector::ZeroVector;
	};

	struct FPropertyRestore
	{
		TWeakObjectPtr<UObject> Object;
		FString Path;
		FString OldValue;
	};

	UFUNCTION()
	void HandleBounce(const FBounceImpact& Impact);

	// Run flow
	void BeginStep();
	bool TickStep(float DeltaTime);
	void Finish(const FString& Error);
	void StepLog(const FString& Text);
	FString StepName() const;

	// Steps (return true when done)
	bool StepWait(const FJsonObject& S);
	bool StepShot(const FJsonObject& S);
	bool StepBurst(const FJsonObject& S);
	bool StepMove(const FJsonObject& S, float DeltaTime);
	bool StepMoveTo(const FJsonObject& S, float DeltaTime);
	bool StepJump(const FJsonObject& S);
	bool StepSlam(const FJsonObject& S);
	bool StepWaitUntil(const FJsonObject& S);
	/** True once S's "until" condition holds (bounce|land|apex|rest|dead|alive|zAbove|zBelow, bounces counted from step start). */
	bool CheckUntil(const FJsonObject& S, const FString& Until, bool& bOutValid, FString& OutDetail) const;
	bool StepLook(const FJsonObject& S);
	bool StepTeleport(const FJsonObject& S);
	bool StepSet(const FJsonObject& S);
	bool StepGet(const FJsonObject& S);
	bool StepCheck(const FJsonObject& S);
	bool StepCmd(const FJsonObject& S);

	// Player access
	APlayerController* GetPC() const;
	ABounceBallPawn* GetPawn() const;
	UEnhancedInputLocalPlayerSubsystem* GetInput() const;
	void BindPawn();

	// Inputs (go through Enhanced Input so bindings/triggers are exercised; direct calls as fallback)
	void SetStick(const FVector2D& Stick);
	void SetJumpHeld(bool bHeld);
	void PressSlam();
	void ReleaseInputs();
	FVector2D StickForWorldDirection(const FVector& WorldDir) const;
	FVector2D ArriveStick(const FVector& Target, bool bBrake) const;
	void FaceDirection(const FVector& WorldDir, float DeltaTime);
	void Teleport(const FVector& Location, TOptional<float> Yaw);

	// Shots
	bool ReadViewport(TArray<FColor>& OutPixels, FIntPoint& OutSize) const;
	FString SaveImage(const TArray<FColor>& Pixels, FIntPoint Size, const FString& Label);
	bool BeginCameraOverride(const FJsonObject& S);
	void EndCameraOverride();

	// Properties
	bool ResolveProperty(const FString& Path, UObject*& OutObject, FProperty*& OutProperty, FString& OutError) const;
	static FString JsonToPropertyText(const TSharedPtr<FJsonValue>& Value);

	// Telemetry / report
	float ReadMetric(const FString& Name, bool& bOk) const;
	FString StateString() const;
	void WriteReport(const FString& Error);

	// ----- run state -----
	TSharedPtr<FJsonObject> Script;
	TArray<TSharedPtr<FJsonObject>> Steps;
	FString RunName;
	FString RunDir;
	bool bRunning = false;
	bool bFinishedReport = false;
	int32 StepIndex = -1;
	bool bStepStarted = false;
	float StepTime = 0.f;
	int32 StepFrames = 0;
	int32 StepPhase = 0;
	int32 StepCounter = 0;
	float StepFloat = 0.f;
	FVector StepVector = FVector::ZeroVector;
	float RunTime = 0.f;
	int32 RunFrames = 0;
	double RealStart = 0.0;
	float RunTimeout = 120.f;
	FVector RunStartLocation = FVector::ZeroVector;

	// Options
	int32 ShotWidth = 960;
	int32 JpgQuality = 85;
	float TraceInterval = 0.1f;
	bool bSavedFixedStep = false;
	double SavedFixedDelta = 0.0;
	bool bChangedFixedStep = false;
	float SavedTimeDilation = 1.f;

	// Input state
	FVector2D CurrentStick = FVector2D::ZeroVector;
	bool bJumpHeld = false;
	bool bFaceMove = true;

	// Telemetry
	TWeakObjectPtr<ABounceBallPawn> BoundPawn;
	int32 PawnChanges = 0;
	int32 Deaths = 0;
	bool bWasDead = false;
	TArray<FBounceRecord> Bounces;
	int32 BouncesAtStepStart = 0;
	float LastVelZ = 0.f;
	bool bApexThisFrame = false;
	float MaxZ = -UE_BIG_NUMBER;
	float MinZ = UE_BIG_NUMBER;
	float Distance = 0.f;
	FVector LastLocation = FVector::ZeroVector;
	bool bHaveLastLocation = false;
	float TraceAccumulator = 0.f;
	TArray<FString> TraceRows;
	TArray<FString> StepLines;
	TArray<FShot> Shots;
	TArray<TPair<FString, FString>> Values;
	int32 ChecksPassed = 0;
	int32 ChecksFailed = 0;
	TArray<FPropertyRestore> Restores;
	TSharedPtr<FBounceBotLogCapture> LogCapture;

	// Shot state
	TWeakObjectPtr<ACameraActor> ShotCamera;
	TArray<TArray<FColor>> BurstFrames;
	FIntPoint BurstSize = FIntPoint::ZeroValue;
	float BurstFirstTime = 0.f;

	// Standalone (-BounceBot=) auto run
	FString PendingScript;
	FString PendingOutDir;
	bool bQuitWhenDone = false;
	float PendingWait = 0.f;
};
