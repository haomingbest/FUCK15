// Fill out your copyright notice in the Description page of Project Settings.

#include "FUCK15.h"
#include "ServerEngine.h"

#include "GenericPlatform/GenericPlatformSurvey.h"
#include "Misc/CommandLine.h"
#include "Misc/TimeGuard.h"
#include "Misc/App.h"
#include "GameMapsSettings.h"
#include "EngineStats.h"
#include "EngineGlobals.h"
#include "RenderingThread.h"
#include "Engine/GameViewportClient.h"
#include "Engine/LevelStreaming.h"
#include "Engine/PlatformInterfaceBase.h"
#include "ContentStreaming.h"
#include "UnrealEngine.h"
#include "HAL/PlatformSplash.h"
#include "UObject/Package.h"
#include "GameFramework/GameModeBase.h"
#include "EngineUtils.h"
#include "Framework/Application/SlateApplication.h"
#include "AudioDeviceManager.h"
#include "Net/NetworkProfiler.h"
#include "RendererInterface.h"
#include "EngineModule.h"
#include "GeneralProjectSettings.h"
#include "Misc/PackageName.h"
#include "Net/DataChannel.h"

#include "Slate/SceneViewport.h"



#include "SynthBenchmark.h"

#include "Misc/HotReloadInterface.h"
#include "Engine/LocalPlayer.h"
#include "Slate/SGameLayerManager.h"
#include "Components/SkyLightComponent.h"
#include "Components/ReflectionCaptureComponent.h"
#include "GameFramework/GameUserSettings.h"
#include "GameDelegates.h"
#include "Engine/CoreSettings.h"
#include "EngineAnalytics.h"
#include "Engine/DemoNetDriver.h"

#include "Tickable.h"



FUCK15_API bool sGDisallowNetworkTravel = false;

// How slow must a frame be (in seconds) to be logged out (<= 0 to disable)
FUCK15_API float sGSlowFrameLoggingThreshold = 0.0f;


static int32 sGDoAsyncEndOfFrameTasks = 0;


UServerEngine::UServerEngine(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
}


void UServerEngine::Init(IEngineLoop* InEngineLoop)
{
	DECLARE_SCOPE_CYCLE_COUNTER(TEXT("UServerEngine Init"), STAT_GameEngineStartup, STATGROUP_LoadTime);

	// Call base.
	UEngine::Init(InEngineLoop);

#if USE_NETWORK_PROFILER
	/*FString NetworkProfilerTag;
	if (FParse::Value(FCommandLine::Get(), TEXT("NETWORKPROFILER="), NetworkProfilerTag))
	{
	GNetworkProfiler.EnableTracking(true);
	}*/
#endif

	// Load and apply user game settings
	GetGameUserSettings()->LoadSettings();
	GetGameUserSettings()->ApplyNonResolutionSettings();



	// Create game instance.  For GameEngine, this should be the only GameInstance that ever gets created.
	CreateServerGame();
	//CreateServerGame();


	//init net
	CreateServerNetDriver();

	/*IMovieSceneCaptureInterface* MovieSceneCaptureImpl = nullptr;
	#if WITH_EDITOR
	if (!IsRunningDedicatedServer() && !IsRunningCommandlet())
	{
	MovieSceneCaptureImpl = IMovieSceneCaptureModule::Get().InitializeFromCommandLine();
	if (MovieSceneCaptureImpl)
	{
	StartupMovieCaptureHandle = MovieSceneCaptureImpl->GetHandle();
	}
	}
	#endif*/

	LastTimeLogsFlushed = FPlatformTime::Seconds();

	UE_LOG(LogInit, Display, TEXT("Server Engine Initialized."));

	// for IsInitialized()
	bIsInitialized = true;
}

void UServerEngine::Start()
{
	UE_LOG(LogInit, Display, TEXT("Starting Server."));


	for (auto iter = AGameList.CreateIterator(); iter; iter++)
	{
		//(*iter)->StartGameInstance();

		FURL InURL;
		InURL.LoadURLConfig(TEXT("DefaultPlayer"), GGameIni);
		InURL.Map = L"/Game/Map/FUCK01";
		StartServerGameInstance(*iter, InURL);
	}

	//GameInstance->StartGameInstance();
}

void UServerEngine::PreExit()
{
	Super::PreExit();

	// Stop tracking, automatically flushes.
	//NETWORK_PROFILER(GNetworkProfiler.EnableTracking(false));

	CancelAllPending();

	// Clean up all worlds
	for (int32 WorldIndex = 0; WorldIndex < WorldList.Num(); ++WorldIndex)
	{
		UWorld* const World = WorldList[WorldIndex].World();
		if (World != NULL)
		{
			World->bIsTearingDown = true;

			// Cancel any pending connection to a server
			CancelPending(World);

			// Shut down any existing game connections
			ShutdownWorldNetDriver(World);

			for (FActorIterator ActorIt(World); ActorIt; ++ActorIt)
			{
				ActorIt->RouteEndPlay(EEndPlayReason::Quit);
			}

			if (World->GetGameInstance() != nullptr)
			{
				World->GetGameInstance()->Shutdown();
			}

			World->FlushLevelStreaming(EFlushLevelStreamingType::Visibility);
			World->CleanupWorld();
		}
	}
}

void UServerEngine::FinishDestroy()
{
	if (!HasAnyFlags(RF_ClassDefaultObject))
	{
		// Game exit.
		UE_LOG(LogExit, Log, TEXT("Game engine shut down"));
	}

	Super::FinishDestroy();
}

UGameInstance* UServerEngine::CreateServerGame()
{
	UGameInstance* _GameInstance;

	FStringClassReference GameInstanceClassName = GetDefault<UGameMapsSettings>()->GameInstanceClass;
	UClass* GameInstanceClass = (GameInstanceClassName.IsValid() ? LoadObject<UClass>(NULL, *GameInstanceClassName.ToString()) : UGameInstance::StaticClass());

	if (GameInstanceClass == nullptr)
	{
		UE_LOG(LogEngine, Error, TEXT("Unable to load GameInstance Class '%s'. Falling back to generic UGameInstance."), *GameInstanceClassName.ToString());
		GameInstanceClass = UGameInstance::StaticClass();
	}

	_GameInstance = NewObject<UGameInstance>(this, GameInstanceClass);

	_GameInstance->InitializeStandalone();

	AGameList.Push(_GameInstance);
	return _GameInstance;
}

void UServerEngine::StartServerGameInstance(UGameInstance* _GameInstance, FURL URL)
{
	FWorldContext* _WorldContext = _GameInstance->GetWorldContext();
	UWorld* _World = _GameInstance->GetWorld();
	
	check(_WorldContext);
	check(_World);

	UPackage* WorldPackage = NULL;
	const FString URLTrueMapName = URL.Map;

	// make sure level streaming isn't frozen	
	_World->bIsLevelStreamingFrozen = false;

	const FName URLMapFName = FName(*URL.Map);
	UWorld::WorldTypePreLoadMap.FindOrAdd(URLMapFName) = _WorldContext->WorldType;

	// See if the level is already in memory
	WorldPackage = FindPackage(nullptr, *URL.Map);

	const bool bPackageAlreadyLoaded = (WorldPackage != nullptr);

	// If the level isn't already in memory, load level from disk
	if (WorldPackage == nullptr)
	{
		WorldPackage = LoadPackage(nullptr, *URL.Map, (_WorldContext->WorldType == EWorldType::PIE ? LOAD_PackageForPIE : LOAD_None));
	}

	// Clean up the world type list now that PostLoad has occurred
	UWorld::WorldTypePreLoadMap.Remove(URLMapFName);

	if (WorldPackage == nullptr)
	{
		// it is now the responsibility of the caller to deal with a NULL return value and alert the user if necessary
		UE_LOG(LogEngine, Error, TEXT("Failed to load package '%s'"), *URL.Map);
		return;
	}

	// Find the newly loaded world.
	_World = UWorld::FindWorldInPackage(WorldPackage);

	// If the world was not found, it could be a redirector to a world. If so, follow it to the destination world.
	if (!_World)
	{
		_World = UWorld::FollowWorldRedirectorInPackage(WorldPackage);
		if (_World)
		{
			WorldPackage = _World->GetOutermost();
		}
	}

	_World->PersistentLevel->HandleLegacyMapBuildData();

	FScopeCycleCounterUObject MapScope(WorldPackage);

	check(_WorldContext->WorldType == EWorldType::Game);

	_WorldContext->World()->AddToRoot();

	if (!_World->bIsWorldInitialized)
	{
		_World->InitWorld();
	}

	_World->SetGameInstance(_WorldContext->OwningGameInstance);
	_WorldContext->SetCurrentWorld(_World);

	_World->SetGameMode(URL);

	//blind netdirver
	//ClientNetDriver->SetWorld(_World);
	//_World->SetNetDriver(ClientNetDriver);

	///

	const TCHAR* MutatorString = URL.GetOption(TEXT("Mutator="), TEXT(""));
	if (MutatorString)
	{
		TArray<FString> Mutators;
		FString(MutatorString).ParseIntoArray(Mutators, TEXT(","), true);

		for (int32 MutatorIndex = 0; MutatorIndex < Mutators.Num(); MutatorIndex++)
		{
			LoadPackagesFully(_World, FULLYLOAD_Mutator, Mutators[MutatorIndex]);
		}
	}

	_World->CreateAISystem();
	_World->InitializeActorsForPlay(URL);
	UNavigationSystem::InitializeForWorld(_World, FNavigationSystemRunMode::GameMode);

	_WorldContext->LastURL = URL;
	_WorldContext->LastURL.Map = URLTrueMapName;

	_World->BeginPlay();
	_World->bWorldWasLoadedThisTick = true;
}

bool UServerEngine::CreateServerNetDriver()
{
	FURL InURL;
	InURL.LoadURLConfig(TEXT("DefaultPlayer"), GGameIni);
	UClass* NetDriverClass = StaticLoadClass(UNetDriver::StaticClass(), NULL, L"/Script/OnlineSubsystemUtils.IpNetDriver", NULL, LOAD_Quiet, NULL);
	ClientNetDriver = NewObject<UNetDriver>(GetTransientPackage(), NetDriverClass);
	FString Error;
	if (!ClientNetDriver->InitListen(this, InURL, false, Error))
	{
		UE_LOG(LogEngine, Log, TEXT("Failed to listen: %s"), *Error);
	}

	return true;
}


bool UServerEngine::NetworkRemapPath(UNetDriver* Driver, FString& Str, bool bReading /*= true*/)
{
	if (Driver == nullptr)
	{
		return false;
	}

	UWorld* const World = AGameList[0]->GetWorld();

	// If the driver is using a duplicate level ID, find the level collection using the driver
	// and see if any of its levels match the prefixed name. If so, remap Str to that level's
	// prefixed name.
	if (Driver->GetDuplicateLevelID() != INDEX_NONE && bReading)
	{
		const FName PrefixedName = *UWorld::ConvertToPIEPackageName(Str, Driver->GetDuplicateLevelID());

		for (const FLevelCollection& Collection : World->GetLevelCollections())
		{
			if (Collection.GetNetDriver() == Driver || Collection.GetDemoNetDriver() == Driver)
			{
				for (const ULevel* Level : Collection.GetLevels())
				{
					const UPackage* const CachedOutermost = Level ? Level->GetOutermost() : nullptr;
					if (CachedOutermost && CachedOutermost->GetFName() == PrefixedName)
					{
						Str = PrefixedName.ToString();
						return true;
					}
				}
			}
		}
	}

	// If the game has created multiple worlds, some of them may have prefixed package names,
	// so we need to remap the world package and streaming levels for replay playback to work correctly.
	FWorldContext& Context = GetWorldContextFromWorldChecked(World);
	if (Context.PIEInstance == INDEX_NONE || !bReading)
	{
		return false;
	}

	// If the prefixed path matches the world package name or the name of a streaming level,
	// return the prefixed name.
	const FString PackageNameOnly = FPackageName::PackageFromPath(*Str);

	const FString PrefixedFullName = UWorld::ConvertToPIEPackageName(Str, Context.PIEInstance);
	const FString PrefixedPackageName = UWorld::ConvertToPIEPackageName(PackageNameOnly, Context.PIEInstance);
	const FString WorldPackageName = World->GetOutermost()->GetName();

	if (WorldPackageName == PrefixedPackageName)
	{
		Str = PrefixedFullName;
		return true;
	}

	for (ULevelStreaming* StreamingLevel : World->StreamingLevels)
	{
		if (StreamingLevel != nullptr)
		{
			const FString StreamingLevelName = StreamingLevel->GetWorldAsset().GetLongPackageName();
			if (StreamingLevelName == PrefixedPackageName)
			{
				Str = PrefixedFullName;
				return true;
			}
		}
	}

	return false;
}

bool UServerEngine::ShouldDoAsyncEndOfFrameTasks() const
{
	return FApp::ShouldUseThreadingForPerformance() && ENamedThreads::RenderThread != ENamedThreads::GameThread && !!sGDoAsyncEndOfFrameTasks;
}

/*-----------------------------------------------------------------------------
Command line executor.
-----------------------------------------------------------------------------*/

bool UServerEngine::Exec(UWorld* InWorld, const TCHAR* Cmd, FOutputDevice& Ar)
{
	if (FParse::Command(&Cmd, TEXT("REATTACHCOMPONENTS")) || FParse::Command(&Cmd, TEXT("REREGISTERCOMPONENTS")))
	{
		UE_LOG(LogConsoleResponse, Warning, TEXT("Deprectated command! Please use 'Reattach.Components' instead."));
		return true;
	}
	else if (FParse::Command(&Cmd, TEXT("EXIT")) || FParse::Command(&Cmd, TEXT("QUIT")))
	{
		FString CmdName = FParse::Token(Cmd, 0);
		bool Background = false;
		if (!CmdName.IsEmpty() && !FCString::Stricmp(*CmdName, TEXT("background")))
		{
			Background = true;
		}

		if (Background && FPlatformProperties::SupportsMinimize())
		{
			return HandleMinimizeCommand(Cmd, Ar);
		}
		else if (FPlatformProperties::SupportsQuit())
		{
			return HandleExitCommand(Cmd, Ar);
		}
		else
		{
			// ignore command on xbox one and ps4 as it will cause a crash
			// ttp:321126
			return true;
		}
	}
	else if (FParse::Command(&Cmd, TEXT("GETMAXTICKRATE")))
	{
		return HandleGetMaxTickRateCommand(Cmd, Ar);
	}
	else if (FParse::Command(&Cmd, TEXT("CANCEL")))
	{
		return HandleCancelCommand(Cmd, Ar, InWorld);
	}
	else if (FParse::Command(&Cmd, TEXT("TOGGLECVAR")))
	{
		FString CVarName;
		FParse::Token(Cmd, CVarName, false);

		bool bEnoughParamsSupplied = false;
		IConsoleVariable * CVar = nullptr;

		if (CVarName.Len() > 0)
		{
			CVar = IConsoleManager::Get().FindConsoleVariable(*CVarName);
		}

		if (CVar)
		{
			// values to toggle between
			FString StringVal1, StringVal2;

			if (FParse::Token(Cmd, StringVal1, false))
			{
				if (FParse::Token(Cmd, StringVal2, false))
				{
					bEnoughParamsSupplied = true;
					FString CurrentValue = CVar->GetString();

					FString Command(FString::Printf(TEXT("%s %s"), *CVarName, (CurrentValue == StringVal1) ? *StringVal2 : *StringVal1));
					GEngine->Exec(InWorld, *Command);
				}
			}
		}
		else
		{
			Ar.Log(*FString::Printf(TEXT("TOGGLECVAR: cvar '%s' was not found"), *CVarName));
			bEnoughParamsSupplied = true;	// cannot say anything about the rest of parameters
		}

		if (!bEnoughParamsSupplied)
		{
			Ar.Log(TEXT("Usage: TOGGLECVAR CVarName Value1 Value2"));
		}

		return true;
	}
#if !UE_BUILD_SHIPPING
	else if (FParse::Command(&Cmd, TEXT("ApplyUserSettings")))
	{
		return HandleApplyUserSettingsCommand(Cmd, Ar);
	}
#endif // !UE_BUILD_SHIPPING
#if WITH_EDITOR
	else if (FParse::Command(&Cmd, TEXT("STARTMOVIECAPTURE")) && GIsEditor)
	{
		/*	IMovieSceneCaptureInterface* CaptureInterface = IMovieSceneCaptureModule::Get().GetFirstActiveMovieSceneCapture();
		if (CaptureInterface)
		{
		CaptureInterface->StartCapturing();
		return true;
		}
		else if (SceneViewport.IsValid())
		{
		if (IMovieSceneCaptureModule::Get().CreateMovieSceneCapture(SceneViewport))
		{
		return true;
		}
		}*/
		return false;
	}
#endif
	else if (InWorld && InWorld->Exec(InWorld, Cmd, Ar))
	{
		return true;
	}
	else if (InWorld && InWorld->GetAuthGameMode() && InWorld->GetAuthGameMode()->ProcessConsoleExec(Cmd, Ar, NULL))
	{
		return true;
	}
	else
	{
#if UE_BUILD_SHIPPING
		// disallow set of actor properties if network game
		if ((FParse::Command(&Cmd, TEXT("SET")) || FParse::Command(&Cmd, TEXT("SETNOPEC"))))
		{
			FWorldContext &Context = GetWorldContextFromWorldChecked(InWorld);
			if (Context.PendingNetGame != NULL || InWorld->GetNetMode() != NM_Standalone)
			{
				return true;
			}
			// the effects of this cannot be easily reversed, so prevent the user from playing network games without restarting to avoid potential exploits
			sGDisallowNetworkTravel = true;
		}
#endif // UE_BUILD_SHIPPING
		if (UEngine::Exec(InWorld, Cmd, Ar))
		{
			return true;
		}
		/*else if (UPlatformInterfaceBase::StaticExec(Cmd, Ar))
		{
		return true;
		}*/

		return false;
	}
}

bool UServerEngine::HandleExitCommand(const TCHAR* Cmd, FOutputDevice& Ar)
{
	Ar.Log(TEXT("Closing by request"));

	FGameDelegates::Get().GetExitCommandDelegate().Broadcast();

	FPlatformMisc::RequestExit(0);
	return true;
}

bool UServerEngine::HandleMinimizeCommand(const TCHAR *Cmd, FOutputDevice &Ar)
{
	Ar.Log(TEXT("Minimize by request"));
	FPlatformMisc::RequestMinimize();

	return true;
}

bool UServerEngine::HandleGetMaxTickRateCommand(const TCHAR* Cmd, FOutputDevice& Ar)
{
	Ar.Logf(TEXT("%f"), GetMaxTickRate(0, false));
	return true;
}

bool UServerEngine::HandleCancelCommand(const TCHAR* Cmd, FOutputDevice& Ar, UWorld* InWorld)
{
	CancelPending(GetWorldContextFromWorldChecked(InWorld));
	return true;
}

#if !UE_BUILD_SHIPPING
bool UServerEngine::HandleApplyUserSettingsCommand(const TCHAR* Cmd, FOutputDevice& Ar)
{
	GetGameUserSettings()->ApplySettings(false);
	return true;
}
#endif // !UE_BUILD_SHIPPING

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

float UServerEngine::GetMaxTickRate(float DeltaTime, bool bAllowFrameRateSmoothing) const
{
	return 60;
	float MaxTickRate = 0.f;

	if (FPlatformProperties::SupportsWindowedMode() == false && !IsRunningDedicatedServer())
	{
		static const auto CVar = IConsoleManager::Get().FindTConsoleVariableDataInt(TEXT("r.VSync"));
		// Limit framerate on console if VSYNC is enabled to avoid jumps from 30 to 60 and back.
		if (CVar->GetValueOnGameThread() != 0)
		{
			if (SmoothedFrameRateRange.HasUpperBound())
			{
				MaxTickRate = SmoothedFrameRateRange.GetUpperBoundValue();
			}
		}
	}
	else
	{
		UWorld* World = NULL;

		for (int32 WorldIndex = 0; WorldIndex < WorldList.Num(); ++WorldIndex)
		{
			if (WorldList[WorldIndex].WorldType == EWorldType::Game)
			{
				World = WorldList[WorldIndex].World();
				break;
			}
		}

		if (World)
		{
			UNetDriver* NetDriver = World->GetNetDriver();
			// In network games, limit framerate to not saturate bandwidth.
			if (NetDriver && (NetDriver->GetNetMode() == NM_DedicatedServer || (NetDriver->GetNetMode() == NM_ListenServer && NetDriver->bClampListenServerTickRate)))
			{
				// We're a dedicated server, use the LAN or Net tick rate.
				MaxTickRate = FMath::Clamp(NetDriver->NetServerMaxTickRate, 1, 1000);
			}
			/*else if( NetDriver && NetDriver->ServerConnection )
			{
			if( NetDriver->ServerConnection->CurrentNetSpeed <= 10000 )
			{
			MaxTickRate = FMath::Clamp( MaxTickRate, 10.f, 90.f );
			}
			}*/
		}
	}

	// See if the code in the base class wants to replace this
	float SuperTickRate = Super::GetMaxTickRate(DeltaTime, bAllowFrameRateSmoothing);
	if (SuperTickRate != 0.0)
	{
		MaxTickRate = SuperTickRate;
	}

	return MaxTickRate;
}


void UServerEngine::Tick(float DeltaSeconds, bool bIdleMode)
{
	//	SCOPE_TIME_GUARD(TEXT("UServerEngine::Tick"));

	//	SCOPE_CYCLE_COUNTER(STAT_ServerEngineTick);
	NETWORK_PROFILER(GNetworkProfiler.TrackFrameBegin());

	int32 LocalTickCycles = 0;
	CLOCK_CYCLES(LocalTickCycles);

	// -----------------------------------------------------
	// Non-World related stuff
	// -----------------------------------------------------

	if (DeltaSeconds < 0.0f)
	{
#if (UE_BUILD_SHIPPING && WITH_EDITOR)
		// End users don't have access to the secure parts of UDN.  Regardless, they won't
		// need the warning because the game ships with AMD drivers that address the issue.
		UE_LOG(LogEngine, Fatal, TEXT("Negative delta time!"));
#else
		// Send developers to the support list thread.
		UE_LOG(LogEngine, Fatal, TEXT("Negative delta time! Please see https://udn.epicgames.com/lists/showpost.php?list=ue3bugs&id=4364"));
#endif
	}

	if ((sGSlowFrameLoggingThreshold > 0.0f) && (DeltaSeconds > sGSlowFrameLoggingThreshold))
	{
		UE_LOG(LogEngine, Log, TEXT("Slow GT frame detected (GT frame %u, delta time %f s)"), GFrameCounter - 1, DeltaSeconds);
	}

	// Tick the module manager
	IHotReloadInterface* HotReload = IHotReloadInterface::GetPtr();
	if (HotReload != nullptr)
	{
		HotReload->Tick();
	}

	if (IsRunningDedicatedServer())
	{
		double CurrentTime = FPlatformTime::Seconds();
		if (CurrentTime - LastTimeLogsFlushed > static_cast<double>(ServerFlushLogInterval))
		{
			GLog->Flush();

			LastTimeLogsFlushed = FPlatformTime::Seconds();
		}
	}

	// Update subsystems.
	{
		// This assumes that UObject::StaticTick only calls ProcessAsyncLoading.
		StaticTick(DeltaSeconds, !!GAsyncLoadingUseFullTimeLimit, GAsyncLoadingTimeLimit / 1000.f);
	}

	FEngineAnalytics::Tick(DeltaSeconds);

	// -----------------------------------------------------
	// Begin ticking NetDirver
	// -----------------------------------------------------

	/*if (ClientNetDriver)
	{
		for (int32 WorldIdx = 0; WorldIdx < WorldList.Num(); ++WorldIdx)
		{
			FWorldContext &Context = WorldList[WorldIdx];
			Context.World()->NetDriver = ClientNetDriver;
		}
		ClientNetDriver->TickDispatch(DeltaSeconds);

		for (int32 WorldIdx = 0; WorldIdx < WorldList.Num(); ++WorldIdx)
		{
			FWorldContext &Context = WorldList[WorldIdx];
			Context.World()->NetDriver = NULL;
		}
	}*/



	// -----------------------------------------------------
	// Begin ticking worlds
	// -----------------------------------------------------

	bool bIsAnyNonPreviewWorldUnpaused = false;

	FName OriginalGWorldContext = NAME_None;
	for (int32 i = 0; i < WorldList.Num(); ++i)
	{
		if (WorldList[i].World() == GWorld)
		{
			OriginalGWorldContext = WorldList[i].ContextHandle;
			break;
		}
	}

	for (int32 WorldIdx = 0; WorldIdx < WorldList.Num(); ++WorldIdx)
	{
		FWorldContext &Context = WorldList[WorldIdx];
		if (Context.World() == NULL || !Context.World()->ShouldTick())
		{
			continue;
		}

		GWorld = Context.World();

		// Tick all travel and Pending NetGames (Seamless, server, client)
		{
			QUICK_SCOPE_CYCLE_COUNTER(STAT_UGameEngine_Tick_TickWorldTravel);
			TickWorldTravel(Context, DeltaSeconds);
		}

		if (!IsRunningDedicatedServer() && !IsRunningCommandlet())
		{
			QUICK_SCOPE_CYCLE_COUNTER(STAT_UGameEngine_Tick_CheckCaptures);
			// Only update reflection captures in game once all 'always loaded' levels have been loaded
			// This won't work with actual level streaming though
			if (Context.World()->AreAlwaysLoadedLevelsLoaded())
			{
				// Update sky light first because it's considered direct lighting, sky diffuse will be visible in reflection capture indirect specular
				USkyLightComponent::UpdateSkyCaptureContents(Context.World());
				UReflectionCaptureComponent::UpdateReflectionCaptureContents(Context.World());
			}
		}

		if (!bIdleMode)
		{
			// Tick the world.
			GameCycles = 0;
			CLOCK_CYCLES(GameCycles);
			if (ClientNetDriver)
			{
				ClientNetDriver->SetWorld(Context.World());
				Context.World()->NetDriver = ClientNetDriver;
			}

			Context.World()->Tick(LEVELTICK_All, DeltaSeconds);

			if (ClientNetDriver)
			{
				ClientNetDriver->TickFlush(DeltaSeconds);
				ClientNetDriver->SetWorld(NULL);
				ClientNetDriver->World = NULL;
				Context.World()->NetDriver = NULL;
			}

			UNCLOCK_CYCLES(GameCycles);
		}

		// Issue cause event after first tick to provide a chance for the game to spawn the player and such.
		if (Context.World()->bWorldWasLoadedThisTick)
		{
			Context.World()->bWorldWasLoadedThisTick = false;

			const TCHAR* InitialExec = Context.LastURL.GetOption(TEXT("causeevent="), NULL);
			ULocalPlayer* GamePlayer = Context.OwningGameInstance ? Context.OwningGameInstance->GetFirstGamePlayer() : NULL;
			if (InitialExec && GamePlayer)
			{
				UE_LOG(LogEngine, Log, TEXT("Issuing initial cause event passed from URL: %s"), InitialExec);
				GamePlayer->Exec(GamePlayer->GetWorld(), *(FString("CAUSEEVENT ") + InitialExec), *GLog);
			}

			Context.World()->bTriggerPostLoadMap = true;
		}

		UpdateTransitionType(Context.World());

		// Block on async loading if requested.
		if (Context.World()->bRequestedBlockOnAsyncLoading)
		{
			BlockTillLevelStreamingCompleted(Context.World());
			Context.World()->bRequestedBlockOnAsyncLoading = false;
		}

		// streamingServer
		if (GIsServer == true)
		{
			SCOPE_CYCLE_COUNTER(STAT_UpdateLevelStreaming);
			Context.World()->UpdateLevelStreaming();
		}

		UNCLOCK_CYCLES(LocalTickCycles);
		TickCycles = LocalTickCycles;

		// See whether any map changes are pending and we requested them to be committed.
		QUICK_SCOPE_CYCLE_COUNTER(STAT_UGameEngine_Tick_ConditionalCommitMapChange);
		ConditionalCommitMapChange(Context);

		if (Context.WorldType != EWorldType::EditorPreview && !Context.World()->IsPaused())
		{
			bIsAnyNonPreviewWorldUnpaused = true;
		}
	}

	// ----------------------------
	//	End per-world ticking
	// ----------------------------

	FTickableGameObject::TickObjects(nullptr, LEVELTICK_All, false, DeltaSeconds);

	// Restore original GWorld*. This will go away one day.
	if (OriginalGWorldContext != NAME_None)
	{
		QUICK_SCOPE_CYCLE_COUNTER(STAT_UGameEngine_Tick_GetWorldContextFromHandleChecked);
		GWorld = GetWorldContextFromHandleChecked(OriginalGWorldContext).World();
	}

}


void UServerEngine::ProcessToggleFreezeCommand(UWorld* InWorld)
{
	if (GameViewport)
	{
		GameViewport->Viewport->ProcessToggleFreezeCommand();
	}
}


void UServerEngine::ProcessToggleFreezeStreamingCommand(UWorld* InWorld)
{
	// if not already frozen, then flush async loading before we freeze so that we don't mess up any in-process streaming
	if (!InWorld->bIsLevelStreamingFrozen)
	{
		FlushAsyncLoading();
	}

	// toggle the frozen state
	InWorld->bIsLevelStreamingFrozen = !InWorld->bIsLevelStreamingFrozen;
}


UWorld* UServerEngine::GetGameWorld()
{
	for (auto It = WorldList.CreateConstIterator(); It; ++It)
	{
		const FWorldContext& Context = *It;
		// Explicitly not checking for PIE worlds here, this should only 
		// be called outside of editor (and thus is in UServerEngine
		if (Context.WorldType == EWorldType::Game && Context.World())
		{
			return Context.World();
		}
	}

	return NULL;
}

void UServerEngine::HandleNetworkFailure_NotifyGameInstance(UWorld* World, UNetDriver* NetDriver, ENetworkFailure::Type FailureType)
{
	/*if (GameInstance != nullptr)
	{
	bool bIsServer = true;
	if (NetDriver != nullptr)
	{
	bIsServer = NetDriver->GetNetMode() != NM_Client;
	}
	GameInstance->HandleNetworkError(FailureType, bIsServer);
	}*/
}

void UServerEngine::HandleTravelFailure_NotifyGameInstance(UWorld* World, ETravelFailure::Type FailureType)
{
	//if (GameInstance != nullptr)
	//{
	//	GameInstance->HandleTravelError(FailureType);
	//}
}

void UServerEngine::HandleBrowseToDefaultMapFailure(FWorldContext& Context, const FString& TextURL, const FString& Error)
{
	Super::HandleBrowseToDefaultMapFailure(Context, TextURL, Error);
	FPlatformMisc::RequestExit(false);
}

EAcceptConnection::Type UServerEngine::NotifyAcceptingConnection()
{
	check(ClientNetDriver);
	return EAcceptConnection::Accept;
}

void  UServerEngine::NotifyAcceptedConnection(UNetConnection* Connection)
{
	check(ClientNetDriver != NULL);
	check(ClientNetDriver->ServerConnection == NULL);
	UE_LOG(LogNet, Log, TEXT("NotifyAcceptedConnection: Name: %s, TimeStamp: %s, %s"), *GetName(), FPlatformTime::StrTimestamp(), *Connection->Describe());
	return;
}

bool  UServerEngine::NotifyAcceptingChannel(UChannel* Channel)
{
	check(Channel);
	check(Channel->Connection);
	check(Channel->Connection->Driver);
	// We are the server.
	if (Channel->ChIndex == 0 && Channel->ChType == CHTYPE_Control)
	{
		// The client has opened initial channel.
		UE_LOG(LogNet, Log, TEXT("NotifyAcceptingChannel Control %i server %s: Accepted"), Channel->ChIndex, *GetFullName());
		return 1;
	}
	else if (Channel->ChType == CHTYPE_File)
	{
		// The client is going to request a file.
		UE_LOG(LogNet, Log, TEXT("NotifyAcceptingChannel File %i server %s: Accepted"), Channel->ChIndex, *GetFullName());
		return 1;
	}
	else
	{
		// Client can't open any other kinds of channels.
		UE_LOG(LogNet, Log, TEXT("NotifyAcceptingChannel %i %i server %s: Refused"), (uint8)Channel->ChType, Channel->ChIndex, *GetFullName());
		return 0;
	}
}
void  UServerEngine::NotifyControlMessage(UNetConnection* Connection, uint8 MessageType, FInBunch& Bunch)
{
	/*FString st = FNetControlMessageInfo::GetName(MessageType);
	UKismetSystemLibrary::PrintString(GetWorld(), st);*/
	// We are the server.
	UE_LOG(LogNet, Error, TEXT("Level server received: %s"), FNetControlMessageInfo::GetName(MessageType));
#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
	
#endif
	if (!Connection->IsClientMsgTypeValid(MessageType))
	{
		// If we get here, either code is mismatched on the client side, or someone could be spoofing the client address
		UE_LOG(LogNet, Error, TEXT("IsClientMsgTypeValid FAILED (%i): Remote Address = %s"), (int)MessageType, *Connection->LowLevelGetRemoteAddress());
		Bunch.SetError();
		return;
	}

	switch (MessageType)
	{
	case NMT_Hello:
	{
		uint8 IsLittleEndian = 0;
		uint32 RemoteNetworkVersion = 0;
		uint32 LocalNetworkVersion = FNetworkVersion::GetLocalNetworkVersion();

		FNetControlMessage<NMT_Hello>::Receive(Bunch, IsLittleEndian, RemoteNetworkVersion);

		if (!FNetworkVersion::IsNetworkCompatible(LocalNetworkVersion, RemoteNetworkVersion))
		{
			UE_LOG(LogNet, Log, TEXT("NotifyControlMessage: Client connecting with invalid version. LocalNetworkVersion: %i, RemoteNetworkVersion: %i"), LocalNetworkVersion, RemoteNetworkVersion);
			FNetControlMessage<NMT_Upgrade>::Send(Connection, LocalNetworkVersion);
			Connection->FlushNet(true);
			Connection->Close();

			//PerfCountersIncrement(TEXT("ClosedConnectionsDueToIncompatibleVersion"));
		}
		else
		{
			Connection->Challenge = FString::Printf(TEXT("%08X"), FPlatformTime::Cycles());
			Connection->SetExpectedClientLoginMsgType(NMT_Login);
			FNetControlMessage<NMT_Challenge>::Send(Connection, Connection->Challenge);
			Connection->FlushNet();
		}
		break;
	}

	case NMT_Netspeed:
	{
		int32 Rate;
		FNetControlMessage<NMT_Netspeed>::Receive(Bunch, Rate);
		Connection->CurrentNetSpeed = FMath::Clamp(Rate, 1800, ClientNetDriver->MaxClientRate);
		UE_LOG(LogNet, Log, TEXT("Client netspeed is %i"), Connection->CurrentNetSpeed);

		break;
	}
	case NMT_Abort:
	{
		break;
	}
	case NMT_Skip:
	{
		break;
	}
	case NMT_Login:
	{
		FUniqueNetIdRepl UniqueIdRepl;

		// Admit or deny the player here.
		FNetControlMessage<NMT_Login>::Receive(Bunch, Connection->ClientResponse, Connection->RequestURL, UniqueIdRepl);
		UE_LOG(LogNet, Log, TEXT("Login request: %s userId: %s"), *Connection->RequestURL, UniqueIdRepl.IsValid() ? *UniqueIdRepl->ToString() : TEXT("Invalid"));


		// Compromise for passing splitscreen playercount through to gameplay login code,
		// without adding a lot of extra unnecessary complexity throughout the login code.
		// NOTE: This code differs from NMT_JoinSplit, by counting + 1 for SplitscreenCount
		//			(since this is the primary connection, not counted in Children)
		FURL InURL(NULL, *Connection->RequestURL, TRAVEL_Absolute);

		if (!InURL.Valid)
		{
			UE_LOG(LogNet, Error, TEXT("NMT_Login: Invalid URL %s"), *Connection->RequestURL);
			Bunch.SetError();
			break;
		}

		uint8 SplitscreenCount = FMath::Min(Connection->Children.Num() + 1, 255);

		// Don't allow clients to specify this value
		InURL.RemoveOption(TEXT("SplitscreenCount"));
		InURL.AddOption(*FString::Printf(TEXT("SplitscreenCount=%i"), SplitscreenCount));

		Connection->RequestURL = InURL.ToString();

		// skip to the first option in the URL
		const TCHAR* Tmp = *Connection->RequestURL;
		for (; *Tmp && *Tmp != '?'; Tmp++);

		// keep track of net id for player associated with remote connection
		Connection->PlayerId = UniqueIdRepl;

		// ask the game code if this player can join
		FString ErrorMsg;
		AGameModeBase* GameMode = AGameList[0]->GetWorld()->GetAuthGameMode();

		if (GameMode)
		{
			GameMode->PreLogin(Tmp, Connection->LowLevelGetRemoteAddress(), Connection->PlayerId, ErrorMsg);
		}
		if (!ErrorMsg.IsEmpty())
		{
			UE_LOG(LogNet, Log, TEXT("PreLogin failure: %s"), *ErrorMsg);
			//NETWORK_PROFILER(GNetworkProfiler.TrackEvent(TEXT("PRELOGIN FAILURE"), *ErrorMsg, Connection));
			FNetControlMessage<NMT_Failure>::Send(Connection, ErrorMsg);
			Connection->FlushNet(true);
			//@todo sz - can't close the connection here since it will leave the failure message 
			// in the send buffer and just close the socket. 
			//Connection->Close();
		}
		else
		{
			WelcomePlayer(Connection);
		}
		break;
	}
	case NMT_Join:
	{
		if (Connection->PlayerController == NULL)
		{
			// Spawn the player-actor for this network player.
			FString ErrorMsg;
			UE_LOG(LogNet, Log, TEXT("Join request: %s"), *Connection->RequestURL);

			FURL InURL(NULL, *Connection->RequestURL, TRAVEL_Absolute);

			if (!InURL.Valid)
			{
				UE_LOG(LogNet, Error, TEXT("NMT_Login: Invalid URL %s"), *Connection->RequestURL);
				Bunch.SetError();
				break;
			}

			Connection->PlayerController = AGameList[0]->GetWorld()->SpawnPlayActor(Connection, ROLE_AutonomousProxy, InURL, Connection->PlayerId, ErrorMsg);
			if (Connection->PlayerController == NULL)
			{
				// Failed to connect.
				UE_LOG(LogNet, Log, TEXT("Join failure: %s"), *ErrorMsg);
				//NETWORK_PROFILER(GNetworkProfiler.TrackEvent(TEXT("JOIN FAILURE"), *ErrorMsg, Connection));
				FNetControlMessage<NMT_Failure>::Send(Connection, ErrorMsg);
				Connection->FlushNet(true);
				//@todo sz - can't close the connection here since it will leave the failure message 
				// in the send buffer and just close the socket. 
				//Connection->Close();
			}
			else
			{
				// Successfully in game.
				UE_LOG(LogNet, Log, TEXT("Join succeeded"));// : %s"), *Connection->PlayerController->PlayerState->PlayerName);
															//NETWORK_PROFILER(GNetworkProfiler.TrackEvent(TEXT("JOIN"), *Connection->PlayerController->PlayerState->PlayerName, Connection));
															// if we're in the middle of a transition or the client is in the wrong world, tell it to travel
				FString LevelName;
				FSeamlessTravelHandler &SeamlessTravelHandler = GEngine->SeamlessTravelHandlerForWorld(AGameList[0]->GetWorld());
				
				if (SeamlessTravelHandler.IsInTransition())
				{
					// tell the client to go to the destination map
					LevelName = SeamlessTravelHandler.GetDestinationMapName();
				}
				else if (!Connection->PlayerController->HasClientLoadedCurrentWorld())
				{
					// tell the client to go to our current map
					FString NewLevelName = GetOutermost()->GetName();
					UE_LOG(LogNet, Log, TEXT("Client joined but was sent to another level. Asking client to travel to: '%s'"), *NewLevelName);
					LevelName = NewLevelName;
				}
				if (LevelName != TEXT(""))
				{
					Connection->PlayerController->ClientTravel(LevelName, TRAVEL_Relative, true);
				}

				// @TODO FIXME - TEMP HACK? - clear queue on join
				Connection->QueuedBits = 0;
			}
		}
		break;
	}
	case NMT_JoinSplit:
	{
		// Handle server-side request for spawning a new controller using a child connection.
		FString SplitRequestURL;
		FUniqueNetIdRepl UniqueIdRepl;
		FNetControlMessage<NMT_JoinSplit>::Receive(Bunch, SplitRequestURL, UniqueIdRepl);

		// Compromise for passing splitscreen playercount through to gameplay login code,
		// without adding a lot of extra unnecessary complexity throughout the login code.
		// NOTE: This code differs from NMT_Login, by counting + 2 for SplitscreenCount
		//			(once for pending child connection, once for primary non-child connection)
		FURL InURL(NULL, *SplitRequestURL, TRAVEL_Absolute);

		if (!InURL.Valid)
		{
			UE_LOG(LogNet, Error, TEXT("NMT_JoinSplit: Invalid URL %s"), *SplitRequestURL);
			Bunch.SetError();
			break;
		}

		uint8 SplitscreenCount = FMath::Min(Connection->Children.Num() + 2, 255);

		// Don't allow clients to specify this value
		InURL.RemoveOption(TEXT("SplitscreenCount"));
		InURL.AddOption(*FString::Printf(TEXT("SplitscreenCount=%i"), SplitscreenCount));

		SplitRequestURL = InURL.ToString();

		// skip to the first option in the URL
		const TCHAR* Tmp = *SplitRequestURL;
		for (; *Tmp && *Tmp != '?'; Tmp++);

		// keep track of net id for player associated with remote connection
		Connection->PlayerId = UniqueIdRepl;

		// go through the same full login process for the split player even though it's all in the same frame
		FString ErrorMsg;
		AGameModeBase* GameMode = AGameList[0]->GetWorld()->GetAuthGameMode();
		if (GameMode)
		{
			GameMode->PreLogin(Tmp, Connection->LowLevelGetRemoteAddress(), Connection->PlayerId, ErrorMsg);
		}
		if (!ErrorMsg.IsEmpty())
		{
			// if any splitscreen viewport fails to join, all viewports on that client also fail
			UE_LOG(LogNet, Log, TEXT("PreLogin failure: %s"), *ErrorMsg);
			//NETWORK_PROFILER(GNetworkProfiler.TrackEvent(TEXT("PRELOGIN FAILURE"), *ErrorMsg, Connection));
			FNetControlMessage<NMT_Failure>::Send(Connection, ErrorMsg);
			Connection->FlushNet(true);
			//@todo sz - can't close the connection here since it will leave the failure message 
			// in the send buffer and just close the socket. 
			//Connection->Close();
		}
		else
		{
			ULevel*	CurrentLevel;
			CurrentLevel = AGameList[0]->GetWorld()->GetCurrentLevel();
			// create a child network connection using the existing connection for its parent
			check(Connection->GetUChildConnection() == NULL);
			check(CurrentLevel);

			UChildConnection* ChildConn = ClientNetDriver->CreateChild(Connection);
			ChildConn->PlayerId = Connection->PlayerId;
			ChildConn->RequestURL = SplitRequestURL;
			ChildConn->ClientWorldPackageName = CurrentLevel->GetOutermost()->GetFName();

			// create URL from string
			FURL JoinSplitURL(NULL, *SplitRequestURL, TRAVEL_Absolute);

			UE_LOG(LogNet, Log, TEXT("JOINSPLIT: Join request: URL=%s"), *JoinSplitURL.ToString());
			APlayerController* PC = AGameList[0]->GetWorld()->SpawnPlayActor(ChildConn, ROLE_AutonomousProxy, JoinSplitURL, ChildConn->PlayerId, ErrorMsg, uint8(Connection->Children.Num()));
			if (PC == NULL)
			{
				// Failed to connect.
				UE_LOG(LogNet, Log, TEXT("JOINSPLIT: Join failure: %s"), *ErrorMsg);
				//NETWORK_PROFILER(GNetworkProfiler.TrackEvent(TEXT("JOINSPLIT FAILURE"), *ErrorMsg, Connection));
				// remove the child connection
				Connection->Children.Remove(ChildConn);
				// if any splitscreen viewport fails to join, all viewports on that client also fail
				FNetControlMessage<NMT_Failure>::Send(Connection, ErrorMsg);
				Connection->FlushNet(true);
				//@todo sz - can't close the connection here since it will leave the failure message 
				// in the send buffer and just close the socket. 
				//Connection->Close();
			}
			else
			{
				// Successfully spawned in game.
				UE_LOG(LogNet, Log, TEXT("JOINSPLIT: Succeeded"));
				//: %s PlayerId : %s"),
				//	*ChildConn->PlayerController->PlayerState->PlayerName,
				//	*ChildConn->PlayerController->PlayerState->UniqueId.ToDebugString());
			}
		}
		break;
	}
	case NMT_PCSwap:
	{
		UNetConnection* SwapConnection = Connection;
		int32 ChildIndex;
		FNetControlMessage<NMT_PCSwap>::Receive(Bunch, ChildIndex);
		if (ChildIndex >= 0)
		{
			SwapConnection = Connection->Children.IsValidIndex(ChildIndex) ? Connection->Children[ChildIndex] : NULL;
		}
		bool bSuccess = false;
		if (SwapConnection != NULL)
		{
			bSuccess = AGameList[0]->GetWorld()->DestroySwappedPC(SwapConnection);
		}

		if (!bSuccess)
		{
			UE_LOG(LogNet, Log, TEXT("Received invalid swap message with child index %i"), ChildIndex);
		}
		break;
	}
	case NMT_DebugText:
	{
		// debug text message
		FString Text;
		FNetControlMessage<NMT_DebugText>::Receive(Bunch, Text);

		UE_LOG(LogNet, Log, TEXT("%s received NMT_DebugText Text=[%s] Desc=%s DescRemote=%s"),
			*Connection->Driver->GetDescription(), *Text, *Connection->LowLevelDescribe(), *Connection->LowLevelGetRemoteAddress());
		break;
	}
	}

}

void UServerEngine::WelcomePlayer(UNetConnection* Connection)
{
	ULevel*	CurrentLevel;
	CurrentLevel = AGameList[0]->GetWorld()->GetCurrentLevel();
	check(CurrentLevel);
	//empty function???
	//Connection->SendPackageMap();

	FString LevelName = CurrentLevel->GetOutermost()->GetName();
	Connection->ClientWorldPackageName = CurrentLevel->GetOutermost()->GetFName();

	FString GameName;
	FString RedirectURL;
	if (AGameList[0]->GetWorld()->GetAuthGameMode() != NULL)
	{
		GameName = AGameList[0]->GetWorld()->GetAuthGameMode()->GetClass()->GetPathName();
		AGameList[0]->GetWorld()->GetAuthGameMode()->GameWelcomePlayer(Connection, RedirectURL);
	}

	FNetControlMessage<NMT_Welcome>::Send(Connection, LevelName, GameName, RedirectURL);
	Connection->FlushNet();
	// don't count initial join data for netspeed throttling
	// as it's unnecessary, since connection won't be fully open until it all gets received, and this prevents later gameplay data from being delayed to "catch up"
	Connection->QueuedBits = 0;
	Connection->SetClientLoginState(EClientLoginState::Welcomed);		// Client is fully logged in
}


