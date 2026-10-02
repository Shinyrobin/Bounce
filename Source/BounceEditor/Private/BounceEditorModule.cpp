// BounceBot editor bridge: a tiny localhost HTTP server so tools (the bouncebot MCP server) can run a whole
// scripted playtest in one request. It starts Play-In-Editor if needed, hands the script to UBounceBotSubsystem,
// and answers with the run report once the run finishes.
//
//   POST /run     body = BounceBot script JSON (plus optional "map", "fresh", "endPie", "realTimeout")
//   GET  /status  -> {"pie":..,"busy":..,"map":..}
//   POST /stop    stop the current run (?pie=1 also ends PIE)
//
// Port 8765 by default; override with -BounceBotPort=<port> on the editor command line.

#include "BounceBotSubsystem.h"
#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Editor/EditorPerformanceSettings.h"
#include "Engine/GameInstance.h"
#include "HttpPath.h"
#include "HttpServerModule.h"
#include "HttpServerRequest.h"
#include "HttpServerResponse.h"
#include "IHttpRouter.h"
#include "Misc/CommandLine.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "PlayInEditorDataTypes.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Settings/LevelEditorPlaySettings.h"
#include "UObject/StrongObjectPtr.h"

class FBounceEditorModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		if (IsRunningCommandlet())
		{
			return;
		}
		uint32 Port = 8765;
		FParse::Value(FCommandLine::Get(), TEXT("-BounceBotPort="), Port);

		Router = FHttpServerModule::Get().GetHttpRouter(Port, /*bFailOnBindFailure*/ true);
		if (!Router.IsValid())
		{
			UE_LOG(LogBounceBot, Warning, TEXT("BounceBot bridge: could not listen on port %u."), Port);
			return;
		}
		Routes.Add(Router->BindRoute(FHttpPath(TEXT("/run")), EHttpServerRequestVerbs::VERB_POST,
			FHttpRequestHandler::CreateRaw(this, &FBounceEditorModule::HandleRun)));
		Routes.Add(Router->BindRoute(FHttpPath(TEXT("/status")), EHttpServerRequestVerbs::VERB_GET,
			FHttpRequestHandler::CreateRaw(this, &FBounceEditorModule::HandleStatus)));
		Routes.Add(Router->BindRoute(FHttpPath(TEXT("/stop")), EHttpServerRequestVerbs::VERB_POST,
			FHttpRequestHandler::CreateRaw(this, &FBounceEditorModule::HandleStop)));
		FHttpServerModule::Get().StartAllListeners();

		TickerHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateRaw(this, &FBounceEditorModule::Tick));
		UE_LOG(LogBounceBot, Log, TEXT("BounceBot bridge listening on http://127.0.0.1:%u"), Port);
	}

	virtual void ShutdownModule() override
	{
		FTSTicker::GetCoreTicker().RemoveTicker(TickerHandle);
		if (Router.IsValid())
		{
			for (const FHttpRouteHandle& Route : Routes)
			{
				Router->UnbindRoute(Route);
			}
		}
		Routes.Reset();
		Router.Reset();
		Fail(TEXT("editor shutting down"));
	}

private:
	enum class EPhase : uint8 { Idle, EndingPIE, StartingPIE, Running };

	static FString BodyText(const FHttpServerRequest& Request)
	{
		const FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Request.Body.GetData()), Request.Body.Num());
		return FString(Converted.Length(), Converted.Get());
	}

	static FString ErrorJson(const FString& Error)
	{
		const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetBoolField(TEXT("ok"), false);
		Object->SetStringField(TEXT("error"), Error);
		FString Json;
		FJsonSerializer::Serialize(Object, TJsonWriterFactory<>::Create(&Json));
		return Json;
	}

	static void Reply(const FHttpResultCallback& OnComplete, const FString& Json)
	{
		OnComplete(FHttpServerResponse::Create(Json, TEXT("application/json")));
	}

	static UBounceBotSubsystem* GetPIEBot()
	{
		UWorld* World = GEditor ? GEditor->PlayWorld.Get() : nullptr;
		UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
		return GameInstance ? GameInstance->GetSubsystem<UBounceBotSubsystem>() : nullptr;
	}

	static FString CurrentPIEMap()
	{
		const UWorld* World = GEditor ? GEditor->PlayWorld.Get() : nullptr;
		return World ? UWorld::RemovePIEPrefix(World->GetMapName()) : FString();
	}

	bool HandleRun(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
	{
		if (Phase != EPhase::Idle)
		{
			Reply(OnComplete, ErrorJson(TEXT("busy: a BounceBot run is already in progress")));
			return true;
		}
		if (!GEditor)
		{
			Reply(OnComplete, ErrorJson(TEXT("editor not ready")));
			return true;
		}

		PendingScript = BodyText(Request);
		TSharedPtr<FJsonObject> Script;
		if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(PendingScript), Script) || !Script.IsValid())
		{
			Reply(OnComplete, ErrorJson(TEXT("body is not valid JSON")));
			return true;
		}
		PendingMap = Script->HasField(TEXT("map")) ? Script->GetStringField(TEXT("map")) : FString();
		bEndPIEAfter = Script->HasField(TEXT("endPie")) && Script->GetBoolField(TEXT("endPie"));
		RealTimeout = Script->HasField(TEXT("realTimeout")) ? Script->GetNumberField(TEXT("realTimeout")) : 300.0;
		const bool bFresh = Script->HasField(TEXT("fresh")) && Script->GetBoolField(TEXT("fresh"));
		PendingComplete = OnComplete;
		PhaseStart = FPlatformTime::Seconds();

		const bool bPIE = GEditor->IsPlaySessionInProgress();
		const bool bWrongMap = !PendingMap.IsEmpty() && FPackageName::GetShortName(PendingMap) != CurrentPIEMap();
		if (bPIE && (bFresh || bWrongMap))
		{
			GEditor->RequestEndPlayMap();
			Phase = EPhase::EndingPIE;
		}
		else if (bPIE)
		{
			Phase = EPhase::StartingPIE; // already up; Tick starts the run once the ball exists
		}
		else
		{
			StartPIE();
		}
		return true;
	}

	bool HandleStatus(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
	{
		const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		const UBounceBotSubsystem* Bot = GetPIEBot();
		Object->SetBoolField(TEXT("ok"), true);
		Object->SetBoolField(TEXT("pie"), GEditor && GEditor->IsPlaySessionInProgress());
		Object->SetStringField(TEXT("map"), CurrentPIEMap());
		Object->SetBoolField(TEXT("ready"), Bot && Bot->IsReady());
		Object->SetBoolField(TEXT("busy"), Phase != EPhase::Idle);
		FString Json;
		FJsonSerializer::Serialize(Object, TJsonWriterFactory<>::Create(&Json));
		Reply(OnComplete, Json);
		return true;
	}

	bool HandleStop(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
	{
		if (UBounceBotSubsystem* Bot = RunningBot.Get())
		{
			Bot->StopRun(TEXT("stopped by request"));
		}
		else
		{
			Fail(TEXT("stopped by request"));
		}
		if (Request.QueryParams.Contains(TEXT("pie")) && GEditor && GEditor->IsPlaySessionInProgress())
		{
			GEditor->RequestEndPlayMap();
		}
		Reply(OnComplete, TEXT("{\"ok\":true}"));
		return true;
	}

	void StartPIE()
	{
		if (!PlaySettings.IsValid())
		{
			PlaySettings.Reset(DuplicateObject(GetDefault<ULevelEditorPlaySettings>(), GetTransientPackage()));
		}
		// Own 1280x720 window: consistent screenshots, and don't grab the user's mouse.
		PlaySettings->NewWindowWidth = 1280;
		PlaySettings->NewWindowHeight = 720;
		PlaySettings->GameGetsMouseControl = false;

		FRequestPlaySessionParams Params;
		Params.EditorPlaySettings = PlaySettings.Get();
		if (!PendingMap.IsEmpty())
		{
			Params.GlobalMapOverride = PendingMap;
		}
		GEditor->RequestPlaySession(Params);
		Phase = EPhase::StartingPIE;
		PhaseStart = FPlatformTime::Seconds();
		ReadySince = 0.0;
	}

	bool Tick(float DeltaTime)
	{
		const double Elapsed = FPlatformTime::Seconds() - PhaseStart;
		switch (Phase)
		{
		case EPhase::Idle:
			break;

		case EPhase::EndingPIE:
			if (!GEditor->IsPlaySessionInProgress())
			{
				StartPIE();
			}
			else if (Elapsed > 30.0)
			{
				Fail(TEXT("timed out waiting for the previous PIE session to end"));
			}
			break;

		case EPhase::StartingPIE:
		{
			UBounceBotSubsystem* Bot = GetPIEBot();
			if (Bot && Bot->IsReady())
			{
				// Let the freshly spawned ball settle a moment before driving it.
				const double Now = FPlatformTime::Seconds();
				ReadySince = ReadySince > 0.0 ? ReadySince : Now;
				if (Now - ReadySince > 0.3)
				{
					BeginRun(Bot);
				}
			}
			else if (Elapsed > 3.0 && !GEditor->IsPlaySessionInProgress())
			{
				Fail(TEXT("PIE did not start (check the editor for Blueprint compile errors or a missing map)"));
			}
			else if (Elapsed > 120.0)
			{
				Fail(TEXT("timed out waiting for PIE and the player ball"));
			}
			break;
		}

		case EPhase::Running:
			if (!RunningBot.IsValid())
			{
				Fail(TEXT("PIE ended during the run"));
			}
			else if (Elapsed > RealTimeout)
			{
				RunningBot->StopRun(FString::Printf(TEXT("real-time timeout (%.0fs)"), RealTimeout));
			}
			break;
		}
		return true;
	}

	void BeginRun(UBounceBotSubsystem* Bot)
	{
		RunningBot = Bot;
		FinishedHandle = Bot->OnFinished.AddRaw(this, &FBounceEditorModule::OnRunFinished);

		// A backgrounded editor throttles to a few fps; keep it at full speed while the bot drives it.
		UEditorPerformanceSettings* Performance = GetMutableDefault<UEditorPerformanceSettings>();
		bSavedThrottle = Performance->bThrottleCPUWhenNotForeground;
		Performance->bThrottleCPUWhenNotForeground = false;
		bThrottleChanged = true;

		Phase = EPhase::Running;
		PhaseStart = FPlatformTime::Seconds();
		FString Error;
		if (!Bot->StartRun(PendingScript, FString(), Error))
		{
			Fail(Error);
		}
	}

	void OnRunFinished(const FString& ReportJson)
	{
		Cleanup();
		if (PendingComplete)
		{
			FHttpResultCallback OnComplete = MoveTemp(PendingComplete);
			PendingComplete = nullptr;
			Reply(OnComplete, ReportJson);
		}
		if (bEndPIEAfter && GEditor && GEditor->IsPlaySessionInProgress())
		{
			GEditor->RequestEndPlayMap();
		}
	}

	void Fail(const FString& Error)
	{
		Cleanup();
		if (PendingComplete)
		{
			FHttpResultCallback OnComplete = MoveTemp(PendingComplete);
			PendingComplete = nullptr;
			Reply(OnComplete, ErrorJson(Error));
		}
	}

	void Cleanup()
	{
		if (UBounceBotSubsystem* Bot = RunningBot.Get())
		{
			Bot->OnFinished.Remove(FinishedHandle);
		}
		RunningBot.Reset();
		FinishedHandle.Reset();
		if (bThrottleChanged)
		{
			GetMutableDefault<UEditorPerformanceSettings>()->bThrottleCPUWhenNotForeground = bSavedThrottle;
			bThrottleChanged = false;
		}
		Phase = EPhase::Idle;
	}

	TSharedPtr<IHttpRouter> Router;
	TArray<FHttpRouteHandle> Routes;
	FTSTicker::FDelegateHandle TickerHandle;
	TStrongObjectPtr<ULevelEditorPlaySettings> PlaySettings;

	EPhase Phase = EPhase::Idle;
	FHttpResultCallback PendingComplete;
	FString PendingScript;
	FString PendingMap;
	bool bEndPIEAfter = false;
	double RealTimeout = 300.0;
	double PhaseStart = 0.0;
	double ReadySince = 0.0;
	TWeakObjectPtr<UBounceBotSubsystem> RunningBot;
	FDelegateHandle FinishedHandle;
	bool bSavedThrottle = true;
	bool bThrottleChanged = false;
};

IMPLEMENT_MODULE(FBounceEditorModule, BounceEditor)
