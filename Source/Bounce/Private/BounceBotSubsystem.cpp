#include "BounceBotSubsystem.h"

#include "BounceBallPawn.h"
#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Dom/JsonObject.h"
#include "EngineUtils.h"
#include "Engine/GameInstance.h"
#include "Engine/GameViewportClient.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "EnhancedInputSubsystems.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "ImageCore.h"
#include "ImageUtils.h"
#include "InputActionValue.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/OutputDevice.h"
#include "Misc/OutputDeviceRedirector.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UnrealClient.h"

DEFINE_LOG_CATEGORY(LogBounceBot);

/** Collects warnings / errors logged during a run (from any thread), deduplicated with counts. */
class FBounceBotLogCapture : public FOutputDevice
{
public:
	virtual void Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity, const FName& Category) override
	{
		const ELogVerbosity::Type Level = ELogVerbosity::Type(Verbosity & ELogVerbosity::VerbosityMask);
		if (Level > ELogVerbosity::Warning || Level == ELogVerbosity::NoLogging)
		{
			return;
		}
		const FString Line = FString::Printf(TEXT("%s: %s"), *Category.ToString(), V).Left(240);
		FScopeLock ScopeLock(&Lock);
		TMap<FString, int32>& Map = Level <= ELogVerbosity::Error ? Errors : Warnings;
		if (int32* Count = Map.Find(Line))
		{
			++*Count;
		}
		else if (Map.Num() < 200)
		{
			Map.Add(Line, 1);
		}
	}

	virtual bool CanBeUsedOnAnyThread() const override { return true; }
	virtual bool CanBeUsedOnMultipleThreads() const override { return true; }

	FCriticalSection Lock;
	TMap<FString, int32> Errors;
	TMap<FString, int32> Warnings;
};

namespace BounceBot
{
	double Num(const FJsonObject& S, const TCHAR* Field, double Default)
	{
		double Value = Default;
		return S.TryGetNumberField(Field, Value) ? Value : Default;
	}

	bool Bool(const FJsonObject& S, const TCHAR* Field, bool Default)
	{
		bool Value = Default;
		return S.TryGetBoolField(Field, Value) ? Value : Default;
	}

	FString Str(const FJsonObject& S, const TCHAR* Field, const FString& Default = FString())
	{
		FString Value;
		return S.TryGetStringField(Field, Value) ? Value : Default;
	}

	bool VecFromArray(const TArray<TSharedPtr<FJsonValue>>& Array, FVector& Out)
	{
		if (Array.Num() < 2)
		{
			return false;
		}
		Out = FVector(Array[0]->AsNumber(), Array[1]->AsNumber(), Array.Num() > 2 ? Array[2]->AsNumber() : 0.0);
		return true;
	}

	/** Reads [x,y] or [x,y,z]. */
	bool Vec(const FJsonObject& S, const TCHAR* Field, FVector& Out)
	{
		const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
		return S.TryGetArrayField(Field, Array) && VecFromArray(*Array, Out);
	}

	FString Fmt(const FVector& V)
	{
		return FString::Printf(TEXT("(%.0f,%.0f,%.0f)"), V.X, V.Y, V.Z);
	}

	FString TypeName(EBounceType Type)
	{
		return StaticEnum<EBounceType>()->GetNameStringByValue(int64(Type));
	}

	FString Sanitize(const FString& In)
	{
		FString Out;
		for (const TCHAR C : In)
		{
			Out.AppendChar(FChar::IsAlnum(C) || C == TEXT('-') || C == TEXT('_') ? C : TEXT('_'));
		}
		return Out.Left(40);
	}

	/** "wait 1.5" / "shot apex" / "slam" -> step object. */
	TSharedPtr<FJsonObject> StepFromString(const FString& Text)
	{
		TSharedPtr<FJsonObject> Step = MakeShared<FJsonObject>();
		FString Verb = Text.TrimStartAndEnd();
		FString Arg;
		Verb.Split(TEXT(" "), &Verb, &Arg);
		Arg.TrimStartAndEndInline();
		Step->SetStringField(TEXT("do"), Verb);
		if (!Arg.IsEmpty())
		{
			if (Verb == TEXT("wait")) { Step->SetNumberField(TEXT("seconds"), FCString::Atod(*Arg)); }
			else if (Verb == TEXT("jump")) { Step->SetNumberField(TEXT("hold"), FCString::Atod(*Arg)); }
			else if (Verb == TEXT("look")) { Step->SetNumberField(TEXT("yaw"), FCString::Atod(*Arg)); }
			else if (Verb == TEXT("slam")) { Step->SetBoolField(TEXT("atApex"), Arg == TEXT("apex")); }
			else if (Verb == TEXT("waitUntil")) { Step->SetStringField(TEXT("until"), Arg); }
			else if (Verb == TEXT("cmd")) { Step->SetStringField(TEXT("command"), Arg); }
			else if (Verb == TEXT("log")) { Step->SetStringField(TEXT("text"), Arg); }
			else { Step->SetStringField(TEXT("label"), Arg); }
		}
		return Step;
	}

	/** Flattens "repeat" blocks and string shorthands into plain step objects. */
	void Flatten(const TArray<TSharedPtr<FJsonValue>>& In, TArray<TSharedPtr<FJsonObject>>& Out, int32 Depth = 0)
	{
		for (const TSharedPtr<FJsonValue>& Value : In)
		{
			if (Value->Type == EJson::String)
			{
				Out.Add(StepFromString(Value->AsString()));
				continue;
			}
			const TSharedPtr<FJsonObject>* Object = nullptr;
			if (!Value->TryGetObject(Object))
			{
				continue;
			}
			const TArray<TSharedPtr<FJsonValue>>* Inner = nullptr;
			if (Str(**Object, TEXT("do")) == TEXT("repeat") && (*Object)->TryGetArrayField(TEXT("steps"), Inner) && Depth < 8)
			{
				const int32 Times = FMath::Clamp(int32(Num(**Object, TEXT("times"), 2)), 0, 100);
				for (int32 i = 0; i < Times; ++i)
				{
					Flatten(*Inner, Out, Depth + 1);
				}
				continue;
			}
			Out.Add(*Object);
		}
	}

	FString ResolveScriptPath(const FString& In)
	{
		if (FPaths::FileExists(In))
		{
			return In;
		}
		const FString Route = FPaths::ProjectDir() / TEXT("Tools/BounceBot/routes") / In + (In.EndsWith(TEXT(".json")) ? TEXT("") : TEXT(".json"));
		return FPaths::FileExists(Route) ? Route : FPaths::ProjectDir() / In;
	}
}

using namespace BounceBot;

static FAutoConsoleCommandWithWorldAndArgs GBounceBotRunCommand(
	TEXT("BounceBot.Run"),
	TEXT("BounceBot.Run <script.json | route name> [outDir] - run a BounceBot script on the local player."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
	{
		UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
		UBounceBotSubsystem* Bot = GameInstance ? GameInstance->GetSubsystem<UBounceBotSubsystem>() : nullptr;
		FString Script;
		if (!Bot || Args.Num() < 1 || !FFileHelper::LoadFileToString(Script, *ResolveScriptPath(Args[0])))
		{
			UE_LOG(LogBounceBot, Warning, TEXT("BounceBot.Run: need a running game and a readable script (got '%s')."), Args.Num() ? *Args[0] : TEXT(""));
			return;
		}
		FString Error;
		if (!Bot->StartRun(Script, Args.Num() > 1 ? Args[1] : FString(), Error))
		{
			UE_LOG(LogBounceBot, Warning, TEXT("BounceBot.Run: %s"), *Error);
		}
	}));

static FAutoConsoleCommandWithWorld GBounceBotStopCommand(
	TEXT("BounceBot.Stop"),
	TEXT("Stop the current BounceBot run."),
	FConsoleCommandWithWorldDelegate::CreateLambda([](UWorld* World)
	{
		UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
		if (UBounceBotSubsystem* Bot = GameInstance ? GameInstance->GetSubsystem<UBounceBotSubsystem>() : nullptr)
		{
			Bot->StopRun(TEXT("stopped from console"));
		}
	}));

// ---------------------------------------------------------------------------------------------------------------------
// Lifetime

void UBounceBotSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	FString ScriptPath;
	if (FParse::Value(FCommandLine::Get(), TEXT("-BounceBot="), ScriptPath))
	{
		bQuitWhenDone = true;
		FParse::Value(FCommandLine::Get(), TEXT("-BounceBotOut="), PendingOutDir);
		if (!FFileHelper::LoadFileToString(PendingScript, *ResolveScriptPath(ScriptPath)))
		{
			UE_LOG(LogBounceBot, Error, TEXT("Could not read BounceBot script '%s'."), *ScriptPath);
			FPlatformMisc::RequestExit(false, TEXT("BounceBot"));
		}
	}
}

void UBounceBotSubsystem::Deinitialize()
{
	if (bRunning)
	{
		Finish(TEXT("game ended during the run"));
	}
	Super::Deinitialize();
}

ETickableTickType UBounceBotSubsystem::GetTickableTickType() const
{
	return IsTemplate() ? ETickableTickType::Never : ETickableTickType::Conditional;
}

bool UBounceBotSubsystem::IsTickable() const
{
	return bRunning || !PendingScript.IsEmpty();
}

UWorld* UBounceBotSubsystem::GetTickableGameObjectWorld() const
{
	return GetGameInstance() ? GetGameInstance()->GetWorld() : nullptr;
}

TStatId UBounceBotSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UBounceBotSubsystem, STATGROUP_Tickables);
}

bool UBounceBotSubsystem::IsReady() const
{
	const UGameViewportClient* Viewport = GetGameInstance() ? GetGameInstance()->GetGameViewportClient() : nullptr;
	return GetPawn() != nullptr && Viewport && Viewport->Viewport;
}

// ---------------------------------------------------------------------------------------------------------------------
// Run flow

bool UBounceBotSubsystem::StartRun(const FString& ScriptJson, const FString& OutDir, FString& OutError)
{
	if (bRunning)
	{
		OutError = TEXT("a run is already in progress");
		return false;
	}
	if (!IsReady())
	{
		OutError = TEXT("no player ball / viewport yet");
		return false;
	}

	TSharedPtr<FJsonObject> Parsed;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ScriptJson);
	if (!FJsonSerializer::Deserialize(Reader, Parsed) || !Parsed.IsValid())
	{
		OutError = FString::Printf(TEXT("script is not valid JSON: %s"), *Reader->GetErrorMessage());
		return false;
	}
	const TArray<TSharedPtr<FJsonValue>>* StepValues = nullptr;
	if (!Parsed->TryGetArrayField(TEXT("steps"), StepValues))
	{
		OutError = TEXT("script has no \"steps\" array");
		return false;
	}

	// Reset all run state.
	Script = Parsed;
	Steps.Reset();
	Flatten(*StepValues, Steps);
	RunName = Str(*Script, TEXT("name"), TEXT("run"));
	StepIndex = 0;
	bStepStarted = false;
	RunTime = 0.f;
	RunFrames = 0;
	RealStart = FPlatformTime::Seconds();
	RunTimeout = Num(*Script, TEXT("timeout"), 120.0);
	ShotWidth = FMath::Clamp(int32(Num(*Script, TEXT("shotWidth"), 960)), 160, 3840);
	JpgQuality = FMath::Clamp(int32(Num(*Script, TEXT("jpgQuality"), 85)), 10, 100);
	TraceInterval = 1.f / FMath::Clamp(float(Num(*Script, TEXT("traceHz"), 10)), 1.f, 120.f);
	bFaceMove = Bool(*Script, TEXT("faceMove"), true);
	CurrentStick = FVector2D::ZeroVector;
	bJumpHeld = false;
	Bounces.Reset();
	PawnChanges = 0;
	Deaths = 0;
	bWasDead = false;
	MaxZ = -UE_BIG_NUMBER;
	MinZ = UE_BIG_NUMBER;
	Distance = 0.f;
	bHaveLastLocation = false;
	LastVelZ = 0.f;
	TraceAccumulator = 0.f;
	TraceRows.Reset();
	TraceRows.Add(TEXT("t,x,y,z,vx,vy,vz,step"));
	StepLines.Reset();
	Shots.Reset();
	Values.Reset();
	ChecksPassed = 0;
	ChecksFailed = 0;
	Restores.Reset();
	BurstFrames.Reset();
	bFinishedReport = false;

	RunDir = OutDir;
	if (RunDir.IsEmpty())
	{
		RunDir = FPaths::ProjectSavedDir() / TEXT("BounceBot/Runs") / (FDateTime::Now().ToString(TEXT("%Y%m%d-%H%M%S-")) + Sanitize(RunName));
	}
	RunDir = FPaths::ConvertRelativePathToFull(RunDir);
	IFileManager::Get().MakeDirectory(*RunDir, true);

	// Deterministic, faster-than-realtime stepping: every frame advances exactly 1/fixedFps of game time.
	const double FixedFps = Num(*Script, TEXT("fixedFps"), 60.0);
	bChangedFixedStep = FixedFps > 0.0;
	if (bChangedFixedStep)
	{
		bSavedFixedStep = FApp::UseFixedTimeStep();
		SavedFixedDelta = FApp::GetFixedDeltaTime();
		FApp::SetUseFixedTimeStep(true);
		FApp::SetFixedDeltaTime(1.0 / FixedFps);
	}
	UWorld* World = GetTickableGameObjectWorld();
	SavedTimeDilation = UGameplayStatics::GetGlobalTimeDilation(World);
	UGameplayStatics::SetGlobalTimeDilation(World, Num(*Script, TEXT("timeScale"), 1.0));

	LogCapture = MakeShared<FBounceBotLogCapture>();
	GLog->AddOutputDevice(LogCapture.Get());

	bRunning = true;
	BindPawn();

	// Start position: the PlayerStart by default so reused PIE sessions start from the same place.
	const TSharedPtr<FJsonValue> Start = Script->TryGetField(TEXT("start"));
	const TSharedPtr<FJsonObject>* StartObject = nullptr;
	if (Start.IsValid() && Start->TryGetObject(StartObject))
	{
		FVector Location;
		if (Vec(**StartObject, TEXT("to"), Location))
		{
			double Yaw = 0.0;
			Teleport(Location, (*StartObject)->TryGetNumberField(TEXT("yaw"), Yaw) ? TOptional<float>(Yaw) : TOptional<float>());
		}
	}
	else if (!Start.IsValid() || Start->AsString() != TEXT("here"))
	{
		AGameModeBase* GameMode = World ? World->GetAuthGameMode() : nullptr;
		if (AActor* PlayerStart = GameMode ? GameMode->FindPlayerStart(GetPC()) : nullptr)
		{
			Teleport(PlayerStart->GetActorLocation(), PlayerStart->GetActorRotation().Yaw);
		}
	}

	RunStartLocation = GetPawn() ? GetPawn()->GetActorLocation() : FVector::ZeroVector;

	UE_LOG(LogBounceBot, Log, TEXT("Run '%s' started: %d steps -> %s"), *RunName, Steps.Num(), *RunDir);
	return true;
}

void UBounceBotSubsystem::StopRun(const FString& Reason)
{
	if (bRunning)
	{
		Finish(Reason.IsEmpty() ? TEXT("stopped") : Reason);
	}
}

void UBounceBotSubsystem::Tick(float DeltaTime)
{
	if (!bRunning)
	{
		// Standalone auto run (-BounceBot=): wait for the level and pawn, then go.
		if (!PendingScript.IsEmpty())
		{
			PendingWait += DeltaTime;
			FString Error;
			if (IsReady() && PendingWait > 0.5f)
			{
				const FString ScriptToRun = MoveTemp(PendingScript);
				PendingScript.Reset();
				if (!StartRun(ScriptToRun, PendingOutDir, Error))
				{
					UE_LOG(LogBounceBot, Error, TEXT("BounceBot could not start: %s"), *Error);
					FPlatformMisc::RequestExit(false, TEXT("BounceBot"));
				}
			}
			else if (PendingWait > 60.f)
			{
				PendingScript.Reset();
				UE_LOG(LogBounceBot, Error, TEXT("BounceBot gave up waiting for the player ball."));
				FPlatformMisc::RequestExit(false, TEXT("BounceBot"));
			}
		}
		return;
	}

	RunTime += DeltaTime;
	++RunFrames;

	ABounceBallPawn* Pawn = GetPawn();
	if (Pawn != BoundPawn.Get())
	{
		BindPawn();
	}

	// Telemetry.
	bApexThisFrame = false;
	if (Pawn)
	{
		const FVector Location = Pawn->GetActorLocation();
		const FVector Velocity = Pawn->BallMovement->Velocity;
		MaxZ = FMath::Max(MaxZ, Location.Z);
		MinZ = FMath::Min(MinZ, Location.Z);
		if (bHaveLastLocation)
		{
			Distance += FVector::Dist2D(Location, LastLocation);
		}
		LastLocation = Location;
		bHaveLastLocation = true;
		bApexThisFrame = LastVelZ > 0.f && Velocity.Z <= 0.f;

		const bool bDead = Pawn->IsDead();
		if (bDead != bWasDead)
		{
			Deaths += bDead ? 1 : 0;
			StepLines.Add(FString::Printf(TEXT("   t=%.2f ** %s at %s"), RunTime, bDead ? TEXT("BALL DIED") : TEXT("respawned"), *Fmt(Location)));
			bWasDead = bDead;
		}
		LastVelZ = Velocity.Z;

		TraceAccumulator += DeltaTime;
		if (TraceAccumulator >= TraceInterval || RunFrames == 1)
		{
			TraceAccumulator = 0.f;
			TraceRows.Add(FString::Printf(TEXT("%.3f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%d"),
				RunTime, Location.X, Location.Y, Location.Z, Velocity.X, Velocity.Y, Velocity.Z, StepIndex + 1));
		}

		// Fallback steering when the pawn has no Move action asset.
		if (!Pawn->MoveAction && !CurrentStick.IsNearlyZero() && GetPC())
		{
			const FRotationMatrix Yaw(FRotator(0.f, GetPC()->GetControlRotation().Yaw, 0.f));
			Pawn->AddMovementInput(Yaw.GetUnitAxis(EAxis::X), CurrentStick.Y);
			Pawn->AddMovementInput(Yaw.GetUnitAxis(EAxis::Y), CurrentStick.X);
		}
	}

	if (RunTime > RunTimeout)
	{
		Finish(FString::Printf(TEXT("run timed out after %.0fs of game time (step %s)"), RunTimeout, *StepName()));
		return;
	}

	// Run steps; instant steps (look, set, check ...) chain within one frame.
	for (int32 Guard = 0; bRunning && Guard < 64; ++Guard)
	{
		if (StepIndex >= Steps.Num())
		{
			Finish(FString());
			return;
		}
		if (!bStepStarted)
		{
			BeginStep();
		}
		else if (Guard == 0)
		{
			StepTime += DeltaTime;
			++StepFrames;
		}
		if (!TickStep(DeltaTime))
		{
			break;
		}
		++StepIndex;
		bStepStarted = false;
	}
}

void UBounceBotSubsystem::BeginStep()
{
	bStepStarted = true;
	StepTime = 0.f;
	StepFrames = 0;
	StepPhase = 0;
	StepCounter = 0;
	StepFloat = 0.f;
	StepVector = FVector::ZeroVector;
	BouncesAtStepStart = Bounces.Num();
}

FString UBounceBotSubsystem::StepName() const
{
	return Steps.IsValidIndex(StepIndex) ? FString::Printf(TEXT("%d %s"), StepIndex + 1, *Str(*Steps[StepIndex], TEXT("do"))) : TEXT("-");
}

void UBounceBotSubsystem::StepLog(const FString& Text)
{
	StepLines.Add(FString::Printf(TEXT("%2d t=%.2f %s"), StepIndex + 1, RunTime, *Text));
}

bool UBounceBotSubsystem::TickStep(float DeltaTime)
{
	const FJsonObject& S = *Steps[StepIndex];
	const FString Do = Str(S, TEXT("do"));

	if (!GetPawn() && Do != TEXT("wait") && Do != TEXT("cmd") && Do != TEXT("log"))
	{
		// Pawn missing (respawning?) - give it a moment before failing the step.
		if (StepTime > 3.f)
		{
			StepLog(TEXT("FAIL no player ball"));
			++ChecksFailed;
			return true;
		}
		return false;
	}

	if (Do == TEXT("wait")) { return StepWait(S); }
	if (Do == TEXT("shot")) { return StepShot(S); }
	if (Do == TEXT("burst")) { return StepBurst(S); }
	if (Do == TEXT("move")) { return StepMove(S, DeltaTime); }
	if (Do == TEXT("moveTo") || Do == TEXT("path")) { return StepMoveTo(S, DeltaTime); }
	if (Do == TEXT("jump")) { return StepJump(S); }
	if (Do == TEXT("slam")) { return StepSlam(S); }
	if (Do == TEXT("waitUntil")) { return StepWaitUntil(S); }
	if (Do == TEXT("look")) { return StepLook(S); }
	if (Do == TEXT("teleport")) { return StepTeleport(S); }
	if (Do == TEXT("set")) { return StepSet(S); }
	if (Do == TEXT("get")) { return StepGet(S); }
	if (Do == TEXT("check")) { return StepCheck(S); }
	if (Do == TEXT("cmd")) { return StepCmd(S); }
	if (Do == TEXT("stop"))
	{
		ReleaseInputs();
		StepLog(TEXT("stop inputs ") + StateString());
		return true;
	}
	if (Do == TEXT("log"))
	{
		StepLog(TEXT("note: ") + Str(S, TEXT("text")));
		return true;
	}
	if (Do == TEXT("timeScale"))
	{
		UGameplayStatics::SetGlobalTimeDilation(GetTickableGameObjectWorld(), Num(S, TEXT("value"), 1.0));
		StepLog(FString::Printf(TEXT("timeScale %.2f"), Num(S, TEXT("value"), 1.0)));
		return true;
	}

	StepLog(FString::Printf(TEXT("FAIL unknown step '%s'"), *Do));
	++ChecksFailed;
	return true;
}

void UBounceBotSubsystem::Finish(const FString& Error)
{
	if (!bRunning)
	{
		return;
	}
	bRunning = false;

	ReleaseInputs();
	EndCameraOverride();

	for (int32 i = Restores.Num() - 1; i >= 0; --i)
	{
		UObject* Object = Restores[i].Object.Get();
		FProperty* Property = nullptr;
		FString Ignored;
		if (Object && ResolveProperty(Restores[i].Path, Object, Property, Ignored))
		{
			Property->ImportText_Direct(*Restores[i].OldValue, Property->ContainerPtrToValuePtr<void>(Object), Object, PPF_None);
		}
	}

	if (bChangedFixedStep)
	{
		FApp::SetUseFixedTimeStep(bSavedFixedStep);
		FApp::SetFixedDeltaTime(SavedFixedDelta);
		bChangedFixedStep = false;
	}
	if (UWorld* World = GetTickableGameObjectWorld())
	{
		UGameplayStatics::SetGlobalTimeDilation(World, SavedTimeDilation);
	}
	if (ABounceBallPawn* Pawn = BoundPawn.Get())
	{
		Pawn->BallMovement->OnBounced.RemoveDynamic(this, &UBounceBotSubsystem::HandleBounce);
	}
	BoundPawn.Reset();

	if (LogCapture.IsValid())
	{
		GLog->RemoveOutputDevice(LogCapture.Get());
	}

	WriteReport(Error);
	LogCapture.Reset();

	if (bQuitWhenDone)
	{
		FPlatformMisc::RequestExit(false, TEXT("BounceBot"));
	}
}

// ---------------------------------------------------------------------------------------------------------------------
// Steps

bool UBounceBotSubsystem::StepWait(const FJsonObject& S)
{
	const float Seconds = Num(S, TEXT("seconds"), 1.0);
	if (StepTime + KINDA_SMALL_NUMBER < Seconds)
	{
		return false;
	}
	StepLog(FString::Printf(TEXT("wait %.2fs -> %s"), Seconds, *StateString()));
	return true;
}

bool UBounceBotSubsystem::StepShot(const FJsonObject& S)
{
	const bool bCamera = S.HasField(TEXT("from")) || S.HasField(TEXT("offset"));
	if (StepPhase == 0 && bCamera)
	{
		if (!BeginCameraOverride(S))
		{
			StepLog(TEXT("FAIL shot: could not place camera"));
			++ChecksFailed;
			return true;
		}
		StepPhase = 1;
	}
	// A camera cut needs a few frames to render (and for temporal AA to settle).
	if (bCamera && StepFrames < 3)
	{
		return false;
	}
	const int32 Settle = int32(Num(S, TEXT("settleFrames"), 0));
	if (StepFrames < Settle)
	{
		return false;
	}

	TArray<FColor> Pixels;
	FIntPoint Size;
	const FString Label = Str(S, TEXT("label"), FString::Printf(TEXT("shot%d"), Shots.Num() + 1));
	if (ReadViewport(Pixels, Size))
	{
		const FString File = SaveImage(Pixels, Size, Label);
		Shots.Add({File, Label, RunTime, StepIndex + 1, 1});
		StepLog(FString::Printf(TEXT("shot '%s' %s"), *Label, *StateString()));
	}
	else
	{
		StepLog(TEXT("FAIL shot: could not read the viewport"));
		++ChecksFailed;
	}
	EndCameraOverride();
	return true;
}

bool UBounceBotSubsystem::StepBurst(const FJsonObject& S)
{
	const bool bCamera = S.HasField(TEXT("from")) || S.HasField(TEXT("offset"));
	const int32 Count = FMath::Clamp(int32(Num(S, TEXT("count"), 6)), 2, 16);
	const float Interval = FMath::Max(Num(S, TEXT("interval"), 0.1), 0.0);
	const FString Label = Str(S, TEXT("label"), FString::Printf(TEXT("burst%d"), Shots.Num() + 1));

	if (StepPhase == 0)
	{
		BurstFrames.Reset();
		if (bCamera && !BeginCameraOverride(S))
		{
			StepLog(TEXT("FAIL burst: could not place camera"));
			++ChecksFailed;
			return true;
		}
		StepPhase = 1;
	}
	if (StepPhase == 1)
	{
		if (bCamera && StepFrames < 3)
		{
			return false;
		}
		StepPhase = 2;
		StepFloat = StepTime;
	}
	if (bCamera && Bool(S, TEXT("follow"), false))
	{
		BeginCameraOverride(S);
	}

	if (StepTime - StepFloat + KINDA_SMALL_NUMBER >= StepCounter * Interval)
	{
		TArray<FColor> Pixels;
		FIntPoint Size;
		if (!ReadViewport(Pixels, Size) || (BurstFrames.Num() && Size != BurstSize))
		{
			StepLog(TEXT("FAIL burst: could not read the viewport"));
			++ChecksFailed;
			EndCameraOverride();
			return true;
		}
		if (BurstFrames.IsEmpty())
		{
			BurstFirstTime = RunTime;
		}
		BurstSize = Size;
		BurstFrames.Add(MoveTemp(Pixels));
		++StepCounter;
	}
	if (StepCounter < Count)
	{
		return false;
	}
	EndCameraOverride();

	// Contact sheet: frames left-to-right, top-to-bottom.
	const int32 Cols = Count <= 3 ? Count : (Count == 4 ? 2 : (Count <= 9 ? 3 : 4));
	const int32 Rows = FMath::DivideAndRoundUp(Count, Cols);
	const int32 Gap = 4;
	const int32 SheetW = FMath::Max(ShotWidth * 3 / 2, 640);
	const int32 CellW = (SheetW - Gap * (Cols - 1)) / Cols;
	const int32 CellH = FMath::Max(1, CellW * BurstSize.Y / FMath::Max(1, BurstSize.X));
	const FIntPoint SheetSize(CellW * Cols + Gap * (Cols - 1), CellH * Rows + Gap * (Rows - 1));
	TArray<FColor> Sheet;
	Sheet.Init(FColor(24, 24, 24, 255), SheetSize.X * SheetSize.Y);
	TArray<FColor> Cell;
	Cell.SetNumUninitialized(CellW * CellH);
	for (int32 i = 0; i < BurstFrames.Num(); ++i)
	{
		FImageUtils::ImageResize(BurstSize.X, BurstSize.Y, BurstFrames[i], CellW, CellH, Cell, false, true);
		const int32 X0 = (i % Cols) * (CellW + Gap);
		const int32 Y0 = (i / Cols) * (CellH + Gap);
		for (int32 y = 0; y < CellH; ++y)
		{
			FMemory::Memcpy(&Sheet[(Y0 + y) * SheetSize.X + X0], &Cell[y * CellW], CellW * sizeof(FColor));
		}
	}
	BurstFrames.Reset();

	const FString File = SaveImage(Sheet, SheetSize, Label);
	Shots.Add({File, Label, BurstFirstTime, StepIndex + 1, Count});
	StepLog(FString::Printf(TEXT("burst '%s' %d frames every %.2fs (%dx%d grid) -> %s"), *Label, Count, Interval, Cols, Rows, *StateString()));
	return true;
}

bool UBounceBotSubsystem::StepMove(const FJsonObject& S, float DeltaTime)
{
	const FString Until = Str(S, TEXT("until"));
	const float Seconds = Num(S, TEXT("seconds"), Until.IsEmpty() ? 1.0 : 10.0);
	FVector Stick3;
	FVector WorldDir = FVector::ZeroVector;
	double Heading = 0.0;
	if (S.TryGetNumberField(TEXT("heading"), Heading))
	{
		WorldDir = FRotator(0.f, Heading, 0.f).Vector();
	}
	else if (Vec(S, TEXT("dir"), WorldDir))
	{
		WorldDir.Z = 0.f;
		WorldDir = WorldDir.GetSafeNormal();
	}

	if (!WorldDir.IsZero())
	{
		if (bFaceMove)
		{
			FaceDirection(WorldDir, DeltaTime);
		}
		SetStick(StickForWorldDirection(WorldDir).GetSafeNormal() * Num(S, TEXT("strength"), 1.0));
	}
	else if (Vec(S, TEXT("stick"), Stick3))
	{
		SetStick(FVector2D(Stick3.X, Stick3.Y));
	}
	if (StepFrames == 0 && Bool(S, TEXT("jump"), false))
	{
		SetJumpHeld(true);
	}

	// "until" ends the move early (e.g. "dead", "bounce"); "seconds" is then the maximum.
	bool bValid = true;
	FString Detail;
	const bool bStopped = !Until.IsEmpty() && CheckUntil(S, Until, bValid, Detail);
	if (bValid && !bStopped && StepTime + KINDA_SMALL_NUMBER < Seconds)
	{
		return false;
	}
	if (!Bool(S, TEXT("keep"), false) || !bValid)
	{
		SetStick(FVector2D::ZeroVector);
		if (Bool(S, TEXT("jump"), false))
		{
			SetJumpHeld(false);
		}
	}
	if (!bValid)
	{
		StepLog(FString::Printf(TEXT("FAIL move: unknown until '%s'"), *Until));
		++ChecksFailed;
	}
	else if (bStopped)
	{
		StepLog(FString::Printf(TEXT("move stopped by %s after %.2fs%s -> %s"), *Until, StepTime, *Detail, *StateString()));
	}
	else
	{
		StepLog(FString::Printf(TEXT("move %.2fs%s -> %s"), Seconds, Until.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" (no %s)"), *Until), *StateString()));
	}
	return true;
}

bool UBounceBotSubsystem::StepMoveTo(const FJsonObject& S, float DeltaTime)
{
	ABounceBallPawn* Pawn = GetPawn();
	const float Tolerance = Num(S, TEXT("tolerance"), 100.0);
	const float Timeout = Num(S, TEXT("timeout"), 20.0);
	const bool bJump = Bool(S, TEXT("jump"), false);

	// Waypoints: "points" (path), "to", or "actor".
	TArray<FVector> Points;
	const TArray<TSharedPtr<FJsonValue>>* PointValues = nullptr;
	if (S.TryGetArrayField(TEXT("points"), PointValues))
	{
		for (const TSharedPtr<FJsonValue>& Value : *PointValues)
		{
			FVector Point;
			if (VecFromArray(Value->AsArray(), Point))
			{
				Points.Add(Point);
			}
		}
	}
	FVector To;
	if (Vec(S, TEXT("to"), To))
	{
		Points.Add(To);
	}
	const FString ActorName = Str(S, TEXT("actor"));
	if (!ActorName.IsEmpty())
	{
		for (TActorIterator<AActor> It(GetTickableGameObjectWorld()); It; ++It)
		{
#if WITH_EDITOR
			const bool bMatch = It->GetActorLabel().Contains(ActorName) || It->GetName().Contains(ActorName);
#else
			const bool bMatch = It->GetName().Contains(ActorName);
#endif
			if (bMatch && *It != Pawn)
			{
				Points.Add(It->GetActorLocation());
				break;
			}
		}
	}
	if (Bool(S, TEXT("relative"), false))
	{
		// Offsets from where the run started (z ignored for arrival anyway).
		for (FVector& Point : Points)
		{
			Point += RunStartLocation;
		}
	}
	if (Points.IsEmpty())
	{
		StepLog(TEXT("FAIL moveTo: no target (use \"to\", \"points\" or \"actor\")"));
		++ChecksFailed;
		return true;
	}

	if (StepFrames == 0)
	{
		StepCounter = 0;            // current waypoint
		StepFloat = UE_BIG_NUMBER;  // best distance so far
		StepVector.X = 0.f;         // time of last progress
		if (bJump)
		{
			SetJumpHeld(true);
		}
	}

	const FString Until = Str(S, TEXT("until"));
	if (!Until.IsEmpty())
	{
		bool bValid = true;
		FString Detail;
		const bool bStopped = CheckUntil(S, Until, bValid, Detail);
		if (bStopped || !bValid)
		{
			SetStick(FVector2D::ZeroVector);
			if (bJump)
			{
				SetJumpHeld(false);
			}
			ChecksFailed += bValid ? 0 : 1;
			StepLog(bValid
				? FString::Printf(TEXT("moveTo %s stopped by %s after %.2fs%s -> %s"), *Fmt(Points.Last()), *Until, StepTime, *Detail, *StateString())
				: FString::Printf(TEXT("FAIL moveTo: unknown until '%s'"), *Until));
			return true;
		}
	}

	const FVector Location = Pawn->GetActorLocation();
	const FVector Target = Points[StepCounter];
	const float Dist = FVector::Dist2D(Location, Target);
	const bool bLast = StepCounter == Points.Num() - 1;

	if (Dist <= Tolerance)
	{
		if (!bLast)
		{
			++StepCounter;
			StepFloat = UE_BIG_NUMBER;
			StepVector.X = StepTime;
			return false;
		}
		SetStick(FVector2D::ZeroVector);
		if (bJump)
		{
			SetJumpHeld(false);
		}
		StepLog(FString::Printf(TEXT("moveTo %s reached in %.2fs (%d pts, err %.0fcm) -> %s"),
			*Fmt(Target), StepTime, Points.Num(), Dist, *StateString()));
		return true;
	}

	if (Dist < StepFloat - 50.f)
	{
		StepFloat = Dist;
		StepVector.X = StepTime;
	}
	const bool bStuck = StepTime - StepVector.X > Num(S, TEXT("stuckTime"), 4.0);
	if (StepTime > Timeout || bStuck)
	{
		SetStick(FVector2D::ZeroVector);
		if (bJump)
		{
			SetJumpHeld(false);
		}
		const bool bRequired = Bool(S, TEXT("required"), true);
		ChecksFailed += bRequired ? 1 : 0;
		StepLog(FString::Printf(TEXT("%s moveTo %s %s at %.2fs, point %d/%d still %.0fcm away -> %s"),
			bRequired ? TEXT("FAIL") : TEXT("WARN"), *Fmt(Target), bStuck ? TEXT("STUCK") : TEXT("TIMEOUT"),
			StepTime, StepCounter + 1, Points.Num(), Dist, *StateString()));
		return true;
	}

	if (bFaceMove)
	{
		FaceDirection(Target - Location, DeltaTime);
	}
	SetStick(ArriveStick(Target, bLast && Bool(S, TEXT("brake"), true)));
	return false;
}

bool UBounceBotSubsystem::StepJump(const FJsonObject& S)
{
	const float Hold = Num(S, TEXT("hold"), 0.15);
	if (StepFrames == 0)
	{
		SetJumpHeld(true);
	}
	if (StepTime + KINDA_SMALL_NUMBER < Hold)
	{
		return false;
	}
	SetJumpHeld(false);
	StepLog(FString::Printf(TEXT("jump held %.2fs -> %s"), Hold, *StateString()));
	return true;
}

bool UBounceBotSubsystem::StepSlam(const FJsonObject& S)
{
	if (Bool(S, TEXT("atApex"), false) && !bApexThisFrame)
	{
		if (StepTime > Num(S, TEXT("timeout"), 3.0))
		{
			StepLog(TEXT("FAIL slam: no apex reached"));
			++ChecksFailed;
			return true;
		}
		return false;
	}
	PressSlam();
	StepLog(TEXT("slam at ") + StateString());
	return true;
}

bool UBounceBotSubsystem::CheckUntil(const FJsonObject& S, const FString& Until, bool& bOutValid, FString& OutDetail) const
{
	bOutValid = true;
	const FString Type = Str(S, TEXT("type"));
	const ABounceBallPawn* Pawn = GetPawn();
	if (!Pawn)
	{
		return false;
	}
	const float Z = Pawn->GetActorLocation().Z;

	if (Until == TEXT("bounce") || Until == TEXT("land"))
	{
		for (int32 i = BouncesAtStepStart; i < Bounces.Num(); ++i)
		{
			const FBounceRecord& B = Bounces[i];
			const bool bFloor = B.Type != EBounceType::Wall;
			if ((Until == TEXT("bounce") || bFloor) && (Type.IsEmpty() || TypeName(B.Type) == Type))
			{
				OutDetail = FString::Printf(TEXT(" (%s in %.0f out %.0f)"), *TypeName(B.Type), B.In, B.Out);
				return true;
			}
		}
		return false;
	}
	if (Until == TEXT("apex")) { return bApexThisFrame; }
	if (Until == TEXT("rest")) { return Pawn->BallMovement->IsResting(); }
	if (Until == TEXT("dead")) { return Pawn->IsDead(); }
	if (Until == TEXT("alive")) { return !Pawn->IsDead(); }
	if (Until == TEXT("zAbove")) { return Z > Num(S, TEXT("value"), 0.0); }
	if (Until == TEXT("zBelow")) { return Z < Num(S, TEXT("value"), 0.0); }
	bOutValid = false;
	return false;
}

bool UBounceBotSubsystem::StepWaitUntil(const FJsonObject& S)
{
	const FString Until = Str(S, TEXT("until"), TEXT("bounce"));
	const FString Type = Str(S, TEXT("type"));
	const float Timeout = Num(S, TEXT("timeout"), 5.0);

	bool bValid = true;
	FString Detail;
	const bool bDone = CheckUntil(S, Until, bValid, Detail);
	if (!bValid)
	{
		StepLog(FString::Printf(TEXT("FAIL waitUntil: unknown condition '%s'"), *Until));
		++ChecksFailed;
		return true;
	}

	const FString What = Until + (Type.IsEmpty() ? FString() : TEXT("(") + Type + TEXT(")"));
	if (bDone)
	{
		StepLog(FString::Printf(TEXT("waitUntil %s after %.2fs%s -> %s"), *What, StepTime, *Detail, *StateString()));
		return true;
	}
	if (StepTime > Timeout)
	{
		const bool bRequired = Bool(S, TEXT("required"), true);
		ChecksFailed += bRequired ? 1 : 0;
		StepLog(FString::Printf(TEXT("%s waitUntil %s TIMEOUT %.1fs -> %s"), bRequired ? TEXT("FAIL") : TEXT("WARN"), *What, Timeout, *StateString()));
		return true;
	}
	return false;
}

bool UBounceBotSubsystem::StepLook(const FJsonObject& S)
{
	APlayerController* PC = GetPC();
	FRotator Rotation = PC->GetControlRotation();
	FVector LookAt;
	double Value = 0.0;
	if (Vec(S, TEXT("lookAt"), LookAt))
	{
		Rotation = (LookAt - GetPawn()->GetActorLocation()).Rotation();
	}
	if (S.TryGetNumberField(TEXT("yaw"), Value)) { Rotation.Yaw = Value; }
	if (S.TryGetNumberField(TEXT("turn"), Value)) { Rotation.Yaw += Value; }
	if (S.TryGetNumberField(TEXT("pitch"), Value)) { Rotation.Pitch = Value; }
	Rotation.Roll = 0.f;
	PC->SetControlRotation(Rotation);
	StepLog(FString::Printf(TEXT("look yaw %.0f pitch %.0f"), Rotation.Yaw, Rotation.Pitch));
	return true;
}

bool UBounceBotSubsystem::StepTeleport(const FJsonObject& S)
{
	FVector To;
	if (Str(S, TEXT("to")) == TEXT("start"))
	{
		To = RunStartLocation;
	}
	else if (!Vec(S, TEXT("to"), To))
	{
		StepLog(TEXT("FAIL teleport: needs \"to\": [x,y,z]"));
		++ChecksFailed;
		return true;
	}
	double Yaw = 0.0;
	Teleport(To, S.TryGetNumberField(TEXT("yaw"), Yaw) ? TOptional<float>(Yaw) : TOptional<float>());
	StepLog(TEXT("teleport -> ") + StateString());
	return true;
}

bool UBounceBotSubsystem::StepSet(const FJsonObject& S)
{
	const FString Path = Str(S, TEXT("path"));
	UObject* Object = nullptr;
	FProperty* Property = nullptr;
	FString Error;
	if (!ResolveProperty(Path, Object, Property, Error))
	{
		StepLog(FString::Printf(TEXT("FAIL set %s: %s"), *Path, *Error));
		++ChecksFailed;
		return true;
	}
	void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Object);
	FString Old;
	Property->ExportTextItem_Direct(Old, ValuePtr, nullptr, Object, PPF_None);
	const FString New = JsonToPropertyText(S.TryGetField(TEXT("value")));
	if (!Property->ImportText_Direct(*New, ValuePtr, Object, PPF_None))
	{
		StepLog(FString::Printf(TEXT("FAIL set %s: could not parse '%s'"), *Path, *New));
		++ChecksFailed;
		return true;
	}
	if (!Bool(S, TEXT("keep"), false))
	{
		Restores.Add({Object, Path, Old});
	}
	FString Now;
	Property->ExportTextItem_Direct(Now, ValuePtr, nullptr, Object, PPF_None);
	StepLog(FString::Printf(TEXT("set %s %s -> %s"), *Path, *Old, *Now));
	return true;
}

bool UBounceBotSubsystem::StepGet(const FJsonObject& S)
{
	const FString Path = Str(S, TEXT("path"));
	UObject* Object = nullptr;
	FProperty* Property = nullptr;
	FString Error;
	if (!ResolveProperty(Path, Object, Property, Error))
	{
		StepLog(FString::Printf(TEXT("FAIL get %s: %s"), *Path, *Error));
		++ChecksFailed;
		return true;
	}
	FString Value;
	Property->ExportTextItem_Direct(Value, Property->ContainerPtrToValuePtr<void>(Object), nullptr, Object, PPF_None);
	Values.Add({Path, Value});
	StepLog(FString::Printf(TEXT("get %s = %s"), *Path, *Value));
	return true;
}

bool UBounceBotSubsystem::StepCheck(const FJsonObject& S)
{
	const FString What = Str(S, TEXT("what"), TEXT("z"));
	bool bOk = false;
	const float Value = ReadMetric(What, bOk);
	if (!bOk)
	{
		StepLog(FString::Printf(TEXT("FAIL check: unknown metric '%s'"), *What));
		++ChecksFailed;
		return true;
	}

	bool bPass = true;
	FString Cond;
	double Bound = 0.0;
	if (S.TryGetNumberField(TEXT("gt"), Bound)) { bPass &= Value > Bound; Cond += FString::Printf(TEXT(" >%g"), Bound); }
	if (S.TryGetNumberField(TEXT("lt"), Bound)) { bPass &= Value < Bound; Cond += FString::Printf(TEXT(" <%g"), Bound); }
	if (S.TryGetNumberField(TEXT("eq"), Bound))
	{
		const double Tol = Num(S, TEXT("tol"), 1.0);
		bPass &= FMath::Abs(Value - Bound) <= Tol;
		Cond += FString::Printf(TEXT(" =%g±%g"), Bound, Tol);
	}
	(bPass ? ChecksPassed : ChecksFailed) += 1;
	const FString Label = Str(S, TEXT("label"));
	StepLog(FString::Printf(TEXT("%s check %s%s (got %.1f)%s"), bPass ? TEXT("PASS") : TEXT("FAIL"), *What, *Cond, Value,
		Label.IsEmpty() ? TEXT("") : *(TEXT(" - ") + Label)));
	return true;
}

bool UBounceBotSubsystem::StepCmd(const FJsonObject& S)
{
	const FString Command = Str(S, TEXT("command"));
	if (APlayerController* PC = GetPC())
	{
		PC->ConsoleCommand(Command);
	}
	StepLog(TEXT("cmd ") + Command);
	return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// Player + input

APlayerController* UBounceBotSubsystem::GetPC() const
{
	return GetGameInstance() ? GetGameInstance()->GetFirstLocalPlayerController() : nullptr;
}

ABounceBallPawn* UBounceBotSubsystem::GetPawn() const
{
	const APlayerController* PC = GetPC();
	return PC ? Cast<ABounceBallPawn>(PC->GetPawn()) : nullptr;
}

UEnhancedInputLocalPlayerSubsystem* UBounceBotSubsystem::GetInput() const
{
	const APlayerController* PC = GetPC();
	return PC ? ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(PC->GetLocalPlayer()) : nullptr;
}

void UBounceBotSubsystem::BindPawn()
{
	if (ABounceBallPawn* Old = BoundPawn.Get())
	{
		Old->BallMovement->OnBounced.RemoveDynamic(this, &UBounceBotSubsystem::HandleBounce);
	}
	ABounceBallPawn* Pawn = GetPawn();
	if (BoundPawn.IsValid() || (Pawn && BoundPawn.IsStale()))
	{
		++PawnChanges;
	}
	BoundPawn = Pawn;
	if (Pawn)
	{
		Pawn->BallMovement->OnBounced.AddUniqueDynamic(this, &UBounceBotSubsystem::HandleBounce);
		bHaveLastLocation = false;
		// A respawned pawn doesn't carry the injected inputs over; re-apply them.
		const FVector2D Stick = CurrentStick;
		CurrentStick = FVector2D::ZeroVector;
		SetStick(Stick);
	}
}

void UBounceBotSubsystem::HandleBounce(const FBounceImpact& Impact)
{
	Bounces.Add({RunTime, Impact.Type, Impact.ImpactSpeed, Impact.BounceSpeed, Impact.ImpactPoint});
}

void UBounceBotSubsystem::SetStick(const FVector2D& InStick)
{
	const FVector2D Stick = InStick.Size() > 1.f ? InStick.GetSafeNormal() : InStick;
	CurrentStick = Stick;
	ABounceBallPawn* Pawn = GetPawn();
	UEnhancedInputLocalPlayerSubsystem* Input = GetInput();
	if (!Pawn || !Pawn->MoveAction || !Input)
	{
		return; // Tick() steers directly.
	}
	const UInputAction* Action = Pawn->MoveAction;
	if (Stick.IsNearlyZero(0.01))
	{
		if (Input->HasContinuousInputInjectionForAction(Action))
		{
			Input->StopContinuousInputInjectionForAction(Action);
		}
	}
	else if (Input->HasContinuousInputInjectionForAction(Action))
	{
		Input->UpdateValueOfContinuousInputInjectionForAction(Action, FInputActionValue(Stick));
	}
	else
	{
		Input->StartContinuousInputInjectionForAction(Action, FInputActionValue(Stick), {}, {});
	}
}

void UBounceBotSubsystem::SetJumpHeld(bool bHeld)
{
	bJumpHeld = bHeld;
	ABounceBallPawn* Pawn = GetPawn();
	if (!Pawn)
	{
		return;
	}
	UEnhancedInputLocalPlayerSubsystem* Input = GetInput();
	if (Pawn->JumpAction && Input)
	{
		if (bHeld && !Input->HasContinuousInputInjectionForAction(Pawn->JumpAction))
		{
			Input->StartContinuousInputInjectionForAction(Pawn->JumpAction, FInputActionValue(true), {}, {});
		}
		else if (!bHeld && Input->HasContinuousInputInjectionForAction(Pawn->JumpAction))
		{
			Input->StopContinuousInputInjectionForAction(Pawn->JumpAction);
		}
	}
	else if (bHeld)
	{
		Pawn->BallMovement->JumpPressed();
	}
	else
	{
		Pawn->BallMovement->JumpReleased();
	}
}

void UBounceBotSubsystem::PressSlam()
{
	ABounceBallPawn* Pawn = GetPawn();
	UEnhancedInputLocalPlayerSubsystem* Input = GetInput();
	if (Pawn && Pawn->SlamAction && Input)
	{
		Input->InjectInputForAction(Pawn->SlamAction, FInputActionValue(true), {}, {});
	}
	else if (Pawn)
	{
		Pawn->BallMovement->Slam();
	}
}

void UBounceBotSubsystem::ReleaseInputs()
{
	SetStick(FVector2D::ZeroVector);
	if (bJumpHeld)
	{
		SetJumpHeld(false);
	}
}

FVector2D UBounceBotSubsystem::StickForWorldDirection(const FVector& WorldDir) const
{
	const APlayerController* PC = GetPC();
	const FRotationMatrix Yaw(FRotator(0.f, PC ? PC->GetControlRotation().Yaw : 0.f, 0.f));
	return FVector2D(FVector::DotProduct(WorldDir, Yaw.GetUnitAxis(EAxis::Y)), FVector::DotProduct(WorldDir, Yaw.GetUnitAxis(EAxis::X)));
}

FVector2D UBounceBotSubsystem::ArriveStick(const FVector& Target, bool bBrake) const
{
	const ABounceBallPawn* Pawn = GetPawn();
	const UBounceBallMovementComponent* Move = Pawn->BallMovement;
	FVector ToTarget = Target - Pawn->GetActorLocation();
	ToTarget.Z = 0.f;
	const float Dist = ToTarget.Size();
	const FVector Dir = ToTarget.GetSafeNormal();

	// Steer toward a desired velocity that slows down in time to stop on the target.
	const float BrakeAccel = Move->Acceleration * FMath::Max(Move->AirControl, 0.2f);
	const float DesiredSpeed = bBrake ? FMath::Min(Move->MaxSpeed, FMath::Sqrt(2.f * BrakeAccel * 0.6f * Dist)) : Move->MaxSpeed;
	FVector Velocity = Move->Velocity;
	Velocity.Z = 0.f;
	FVector Command = (Dir * DesiredSpeed - Velocity) / FMath::Max(Move->MaxSpeed * 0.3f, 1.f);
	if (Command.Size() > 1.f)
	{
		Command.Normalize();
	}
	return StickForWorldDirection(Command);
}

void UBounceBotSubsystem::FaceDirection(const FVector& WorldDir, float DeltaTime)
{
	APlayerController* PC = GetPC();
	if (!PC || WorldDir.IsNearlyZero())
	{
		return;
	}
	FRotator Rotation = PC->GetControlRotation();
	Rotation.Yaw = FMath::FixedTurn(Rotation.Yaw, WorldDir.Rotation().Yaw, 270.f * DeltaTime);
	PC->SetControlRotation(Rotation);
}

void UBounceBotSubsystem::Teleport(const FVector& Location, TOptional<float> Yaw)
{
	ABounceBallPawn* Pawn = GetPawn();
	if (!Pawn)
	{
		return;
	}
	Pawn->SetActorLocation(Location, false, nullptr, ETeleportType::TeleportPhysics);
	if (Yaw.IsSet())
	{
		Pawn->SetActorRotation(FRotator(0.f, *Yaw, 0.f), ETeleportType::TeleportPhysics);
		if (APlayerController* PC = GetPC())
		{
			PC->SetControlRotation(FRotator(PC->GetControlRotation().Pitch, *Yaw, 0.f));
		}
	}
	Pawn->BallMovement->StopMovementImmediately();
	Pawn->ResetRider();
	bHaveLastLocation = false;
}

// ---------------------------------------------------------------------------------------------------------------------
// Shots

bool UBounceBotSubsystem::ReadViewport(TArray<FColor>& OutPixels, FIntPoint& OutSize) const
{
	UGameViewportClient* Client = GetGameInstance() ? GetGameInstance()->GetGameViewportClient() : nullptr;
	FViewport* Viewport = Client ? Client->Viewport : nullptr;
	if (!Viewport)
	{
		return false;
	}
	OutSize = Viewport->GetSizeXY();
	return OutSize.X > 0 && OutSize.Y > 0 && Viewport->ReadPixels(OutPixels) && OutPixels.Num() == OutSize.X * OutSize.Y;
}

FString UBounceBotSubsystem::SaveImage(const TArray<FColor>& Pixels, FIntPoint Size, const FString& Label)
{
	TArray<FColor> Scaled;
	const TArray<FColor>* Source = &Pixels;
	if (Size.X > ShotWidth)
	{
		const FIntPoint NewSize(ShotWidth, FMath::Max(1, Size.Y * ShotWidth / Size.X));
		Scaled.SetNumUninitialized(NewSize.X * NewSize.Y);
		FImageUtils::ImageResize(Size.X, Size.Y, Pixels, NewSize.X, NewSize.Y, Scaled, false, true);
		Source = &Scaled;
		Size = NewSize;
	}
	const FString File = RunDir / FString::Printf(TEXT("%02d_%s.jpg"), Shots.Num() + 1, *Sanitize(Label));
	if (!FImageUtils::SaveImageByExtension(*File, FImageView(Source->GetData(), Size.X, Size.Y), JpgQuality))
	{
		UE_LOG(LogBounceBot, Warning, TEXT("Could not save %s"), *File);
	}
	return File;
}

bool UBounceBotSubsystem::BeginCameraOverride(const FJsonObject& S)
{
	APlayerController* PC = GetPC();
	const ABounceBallPawn* Pawn = GetPawn();
	if (!PC || !Pawn)
	{
		return false;
	}

	const FVector PawnLocation = Pawn->GetActorLocation();
	FVector From;
	FVector Offset;
	if (!Vec(S, TEXT("from"), From))
	{
		if (!Vec(S, TEXT("offset"), Offset))
		{
			return false;
		}
		From = PawnLocation + Offset;
	}
	FVector LookAt = PawnLocation;
	Vec(S, TEXT("lookAt"), LookAt);
	const FRotator Rotation = (LookAt - From).Rotation();

	ACameraActor* Camera = ShotCamera.Get();
	if (!Camera)
	{
		FActorSpawnParameters Params;
		Params.ObjectFlags |= RF_Transient;
		Camera = GetTickableGameObjectWorld()->SpawnActor<ACameraActor>(From, Rotation, Params);
		if (!Camera)
		{
			return false;
		}
		Camera->GetCameraComponent()->SetConstraintAspectRatio(false);
		ShotCamera = Camera;
		PC->SetViewTarget(Camera);
	}
	Camera->SetActorLocationAndRotation(From, Rotation);
	Camera->GetCameraComponent()->SetFieldOfView(Num(S, TEXT("fov"), 90.0));
	return true;
}

void UBounceBotSubsystem::EndCameraOverride()
{
	if (ACameraActor* Camera = ShotCamera.Get())
	{
		if (APlayerController* PC = GetPC())
		{
			PC->SetViewTarget(PC->GetPawn());
		}
		Camera->Destroy();
	}
	ShotCamera.Reset();
}

// ---------------------------------------------------------------------------------------------------------------------
// Properties

bool UBounceBotSubsystem::ResolveProperty(const FString& Path, UObject*& OutObject, FProperty*& OutProperty, FString& OutError) const
{
	ABounceBallPawn* Pawn = GetPawn();
	if (!Pawn)
	{
		OutError = TEXT("no player ball");
		return false;
	}

	auto FindOwner = [this, Pawn](const FString& Name) -> UObject*
	{
		if (Name == TEXT("Pawn")) { return Pawn; }
		if (Name == TEXT("Controller") || Name == TEXT("PC")) { return GetPC(); }
		if (const FObjectProperty* Property = FindFProperty<FObjectProperty>(Pawn->GetClass(), *Name))
		{
			return Property->GetObjectPropertyValue_InContainer(Pawn);
		}
		for (UActorComponent* Component : Pawn->GetComponents())
		{
			if (Component && Component->GetName() == Name)
			{
				return Component;
			}
		}
		return nullptr;
	};

	FString OwnerName;
	FString PropertyName = Path;
	if (Path.Split(TEXT("."), &OwnerName, &PropertyName))
	{
		OutObject = FindOwner(OwnerName);
		if (!OutObject)
		{
			OutError = FString::Printf(TEXT("no component/object '%s' on the ball"), *OwnerName);
			return false;
		}
		OutProperty = FindFProperty<FProperty>(OutObject->GetClass(), *PropertyName);
	}
	else
	{
		// Bare name: the pawn, then its components.
		OutObject = Pawn;
		OutProperty = FindFProperty<FProperty>(Pawn->GetClass(), *PropertyName);
		for (UActorComponent* Component : Pawn->GetComponents())
		{
			if (OutProperty)
			{
				break;
			}
			if (Component && (OutProperty = FindFProperty<FProperty>(Component->GetClass(), *PropertyName)) != nullptr)
			{
				OutObject = Component;
			}
		}
	}
	if (!OutProperty)
	{
		OutError = FString::Printf(TEXT("no property '%s'"), *PropertyName);
		return false;
	}
	return true;
}

FString UBounceBotSubsystem::JsonToPropertyText(const TSharedPtr<FJsonValue>& Value)
{
	if (!Value.IsValid())
	{
		return FString();
	}
	switch (Value->Type)
	{
	case EJson::Number:
		return FString::SanitizeFloat(Value->AsNumber());
	case EJson::Boolean:
		return Value->AsBool() ? TEXT("True") : TEXT("False");
	case EJson::Array:
	{
		const TArray<TSharedPtr<FJsonValue>>& Array = Value->AsArray();
		static const TCHAR* Axes[] = {TEXT("X"), TEXT("Y"), TEXT("Z"), TEXT("W")};
		TArray<FString> Parts;
		for (int32 i = 0; i < Array.Num() && i < 4; ++i)
		{
			Parts.Add(FString::Printf(TEXT("%s=%s"), Axes[i], *FString::SanitizeFloat(Array[i]->AsNumber())));
		}
		return TEXT("(") + FString::Join(Parts, TEXT(",")) + TEXT(")");
	}
	default:
		return Value->AsString();
	}
}

// ---------------------------------------------------------------------------------------------------------------------
// Telemetry + report

float UBounceBotSubsystem::ReadMetric(const FString& Name, bool& bOk) const
{
	bOk = true;
	const ABounceBallPawn* Pawn = GetPawn();
	const FVector L = Pawn ? Pawn->GetActorLocation() : FVector::ZeroVector;
	const FVector V = Pawn ? Pawn->BallMovement->Velocity : FVector::ZeroVector;
	if (Name == TEXT("x")) { return L.X; }
	if (Name == TEXT("y")) { return L.Y; }
	if (Name == TEXT("z")) { return L.Z; }
	if (Name == TEXT("vx")) { return V.X; }
	if (Name == TEXT("vy")) { return V.Y; }
	if (Name == TEXT("vz")) { return V.Z; }
	if (Name == TEXT("speed")) { return V.Size(); }
	if (Name == TEXT("hspeed")) { return V.Size2D(); }
	if (Name == TEXT("maxZ")) { return MaxZ; }
	if (Name == TEXT("minZ")) { return MinZ; }
	if (Name == TEXT("distance")) { return Distance; }
	if (Name == TEXT("bounces")) { return Bounces.Num(); }
	if (Name == TEXT("deaths")) { return Deaths; }
	if (Name == TEXT("dead")) { return Pawn && Pawn->IsDead() ? 1.f : 0.f; }
	if (Name == TEXT("time")) { return RunTime; }
	if (Name == TEXT("yaw")) { return Pawn ? Pawn->GetActorRotation().Yaw : 0.f; }
	if (Name == TEXT("lastBounceOut")) { return Bounces.Num() ? Bounces.Last().Out : 0.f; }
	if (Name == TEXT("lastBounceIn")) { return Bounces.Num() ? Bounces.Last().In : 0.f; }
	bOk = false;
	return 0.f;
}

FString UBounceBotSubsystem::StateString() const
{
	const ABounceBallPawn* Pawn = GetPawn();
	if (!Pawn)
	{
		return TEXT("(no ball)");
	}
	const FVector V = Pawn->BallMovement->Velocity;
	return FString::Printf(TEXT("pos%s vel%s%s"), *Fmt(Pawn->GetActorLocation()), *Fmt(V),
		Pawn->IsDead() ? TEXT(" DEAD") : Pawn->BallMovement->IsSlamming() ? TEXT(" SLAM") : (Pawn->BallMovement->IsResting() ? TEXT(" REST") : TEXT("")));
}

void UBounceBotSubsystem::WriteReport(const FString& Error)
{
	const TSharedRef<FJsonObject> Report = MakeShared<FJsonObject>();
	const UWorld* World = GetTickableGameObjectWorld();
	const double FixedFps = Script.IsValid() ? Num(*Script, TEXT("fixedFps"), 60.0) : 0.0;

	Report->SetBoolField(TEXT("ok"), Error.IsEmpty() && ChecksFailed == 0);
	Report->SetStringField(TEXT("error"), Error);
	Report->SetStringField(TEXT("name"), RunName);
	Report->SetStringField(TEXT("dir"), RunDir);
	Report->SetStringField(TEXT("map"), World ? UWorld::RemovePIEPrefix(World->GetMapName()) : FString());
	Report->SetNumberField(TEXT("gameTime"), FMath::RoundToFloat(RunTime * 100.f) / 100.f);
	Report->SetNumberField(TEXT("realTime"), FMath::RoundToDouble((FPlatformTime::Seconds() - RealStart) * 100.0) / 100.0);
	Report->SetNumberField(TEXT("frames"), RunFrames);
	Report->SetNumberField(TEXT("fixedFps"), FixedFps);
	Report->SetNumberField(TEXT("stepsRun"), FMath::Min(StepIndex + (bStepStarted ? 1 : 0), Steps.Num()));
	Report->SetNumberField(TEXT("stepsTotal"), Steps.Num());

	TArray<TSharedPtr<FJsonValue>> StepArray;
	for (const FString& Line : StepLines)
	{
		StepArray.Add(MakeShared<FJsonValueString>(Line));
	}
	Report->SetArrayField(TEXT("steps"), StepArray);

	TArray<TSharedPtr<FJsonValue>> ShotArray;
	for (const FShot& Shot : Shots)
	{
		const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("file"), Shot.File);
		Object->SetStringField(TEXT("label"), Shot.Label);
		Object->SetNumberField(TEXT("t"), FMath::RoundToFloat(Shot.Time * 100.f) / 100.f);
		Object->SetNumberField(TEXT("step"), Shot.Step);
		Object->SetNumberField(TEXT("frames"), Shot.Frames);
		ShotArray.Add(MakeShared<FJsonValueObject>(Object));
	}
	Report->SetArrayField(TEXT("shots"), ShotArray);

	// Bounces: totals by type plus the first 40 as one-liners.
	const TSharedRef<FJsonObject> BounceObject = MakeShared<FJsonObject>();
	const TSharedRef<FJsonObject> ByType = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> BounceList;
	float MaxIn = 0.f;
	float MaxOut = 0.f;
	for (const FBounceRecord& B : Bounces)
	{
		const FString Type = TypeName(B.Type);
		ByType->SetNumberField(Type, ByType->HasField(Type) ? ByType->GetNumberField(Type) + 1 : 1);
		MaxIn = FMath::Max(MaxIn, B.In);
		MaxOut = FMath::Max(MaxOut, B.Out);
		if (BounceList.Num() < 40)
		{
			BounceList.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("t=%.2f %s in %.0f out %.0f at %s"), B.Time, *Type, B.In, B.Out, *Fmt(B.Location))));
		}
	}
	BounceObject->SetNumberField(TEXT("count"), Bounces.Num());
	BounceObject->SetObjectField(TEXT("byType"), ByType);
	BounceObject->SetNumberField(TEXT("maxIn"), FMath::RoundToFloat(MaxIn));
	BounceObject->SetNumberField(TEXT("maxOut"), FMath::RoundToFloat(MaxOut));
	BounceObject->SetArrayField(TEXT("list"), BounceList);
	Report->SetObjectField(TEXT("bounces"), BounceObject);

	Report->SetNumberField(TEXT("maxZ"), Bounces.Num() || MaxZ > -UE_BIG_NUMBER ? FMath::RoundToFloat(MaxZ) : 0.f);
	Report->SetNumberField(TEXT("minZ"), MinZ < UE_BIG_NUMBER ? FMath::RoundToFloat(MinZ) : 0.f);
	Report->SetNumberField(TEXT("distance"), FMath::RoundToFloat(Distance));
	Report->SetStringField(TEXT("end"), StateString());
	Report->SetNumberField(TEXT("pawnChanges"), PawnChanges);
	Report->SetNumberField(TEXT("deaths"), Deaths);
	Report->SetNumberField(TEXT("checksPassed"), ChecksPassed);
	Report->SetNumberField(TEXT("checksFailed"), ChecksFailed);

	const TSharedRef<FJsonObject> ValueObject = MakeShared<FJsonObject>();
	for (const TPair<FString, FString>& Value : Values)
	{
		ValueObject->SetStringField(Value.Key, Value.Value);
	}
	Report->SetObjectField(TEXT("values"), ValueObject);

	// Log: errors and warnings seen during the run, most frequent first.
	const TSharedRef<FJsonObject> LogObject = MakeShared<FJsonObject>();
	if (LogCapture.IsValid())
	{
		FScopeLock ScopeLock(&LogCapture->Lock);
		auto AddLines = [&LogObject](const TCHAR* Field, TMap<FString, int32> Map)
		{
			Map.ValueSort([](int32 A, int32 B) { return A > B; });
			TArray<TSharedPtr<FJsonValue>> Lines;
			for (const TPair<FString, int32>& Pair : Map)
			{
				if (Lines.Num() >= 12)
				{
					break;
				}
				Lines.Add(MakeShared<FJsonValueString>(Pair.Value > 1 ? FString::Printf(TEXT("[%dx] %s"), Pair.Value, *Pair.Key) : Pair.Key));
			}
			LogObject->SetArrayField(Field, Lines);
			LogObject->SetNumberField(FString(Field) + TEXT("Distinct"), Map.Num());
		};
		AddLines(TEXT("errors"), LogCapture->Errors);
		AddLines(TEXT("warnings"), LogCapture->Warnings);
	}
	Report->SetObjectField(TEXT("log"), LogObject);

	const FString TraceFile = RunDir / TEXT("trace.csv");
	FFileHelper::SaveStringToFile(FString::Join(TraceRows, TEXT("\n")), *TraceFile);
	Report->SetStringField(TEXT("trace"), TraceFile);

	FString Json;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Json);
	FJsonSerializer::Serialize(Report, Writer);
	FFileHelper::SaveStringToFile(Json, *(RunDir / TEXT("report.json")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);

	UE_LOG(LogBounceBot, Log, TEXT("Run '%s' finished (%s): %d steps, %d shots, %d bounces, checks %d/%d -> %s"),
		*RunName, Error.IsEmpty() ? TEXT("ok") : *Error, StepLines.Num(), Shots.Num(), Bounces.Num(), ChecksPassed, ChecksPassed + ChecksFailed, *RunDir);
	OnFinished.Broadcast(Json);
}
