// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "UObject/ObjectMacros.h"
#include "Widgets/SWindow.h"
#include "Widgets/SViewport.h"
#include "Engine/Engine.h"
#include "Runtime/MovieSceneCapture/Public/MovieSceneCaptureHandle.h"
#include "ServerEngine.generated.h"

/**
*
*/
class Error;
class FSceneViewport;
class UGameViewportClient;
class UNetDriver;


UCLASS(config = Engine, transient)
//UCLASS()
class FUCK15_API UServerEngine : public UEngine, public FNetworkNotify
{
	GENERATED_UCLASS_BODY()

		/** Maximium delta time the engine uses to populate FApp::DeltaTime. If 0, unbound. */
		UPROPERTY(config)
		float MaxDeltaTime;

	/** Maximium time (in seconds) between the flushes of the logs on the server (best effort). If 0, this will happen every tick. */
	UPROPERTY(config)
		float ServerFlushLogInterval;

	UPROPERTY(transient)
		//UGameInstance* GameInstance;
		TArray<UGameInstance*> AGameList;
	UNetDriver* ClientNetDriver;
	//UPendingNetGame* pendingNetGame;

public:

	// UObject interface
	UGameInstance* CreateServerGame();
	void StartServerGameInstance(UGameInstance* _GameInstance, FURL URL);
	bool CreateServerNetDriver();
	void WelcomePlayer(UNetConnection* Connection);

	virtual void FinishDestroy() override;

public:

	// UEngine interface

	virtual void Init(class IEngineLoop* InEngineLoop) override;
	virtual void Start() override;
	virtual void PreExit() override;
	virtual void Tick(float DeltaSeconds, bool bIdleMode) override;
	virtual float GetMaxTickRate(float DeltaTime, bool bAllowFrameRateSmoothing = true) const override;
	virtual void ProcessToggleFreezeCommand(UWorld* InWorld) override;
	virtual void ProcessToggleFreezeStreamingCommand(UWorld* InWorld) override;
	virtual bool NetworkRemapPath(UNetDriver* Driver, FString& Str, bool bReading = true) override;
	virtual bool ShouldDoAsyncEndOfFrameTasks() const override;

	//~ Begin FNetworkNotify Interface
	virtual EAcceptConnection::Type NotifyAcceptingConnection() override;
	virtual void NotifyAcceptedConnection(class UNetConnection* Connection) override;
	virtual bool NotifyAcceptingChannel(class UChannel* Channel) override;
	virtual void NotifyControlMessage(UNetConnection* Connection, uint8 MessageType, class FInBunch& Bunch) override;
	//~ End FNetworkNotify Interface
public:

	// FExec interface

	virtual bool Exec(UWorld* InWorld, const TCHAR* Cmd, FOutputDevice& Ar = *GLog) override;

public:

	// Exec command handlers
	bool HandleCommand(const TCHAR* Cmd, FOutputDevice& Ar);
	bool HandleExitCommand(const TCHAR* Cmd, FOutputDevice& Ar);
	bool HandleMinimizeCommand(const TCHAR *Cmd, FOutputDevice &Ar);
	bool HandleGetMaxTickRateCommand(const TCHAR* Cmd, FOutputDevice& Ar);
	bool HandleCancelCommand(const TCHAR* Cmd, FOutputDevice& Ar, UWorld* InWorld);

#if !UE_BUILD_SHIPPING
	bool HandleApplyUserSettingsCommand(const TCHAR* Cmd, FOutputDevice& Ar);
#endif // !UE_BUILD_SHIPPING

	/** Returns the GameViewport widget */
	/*virtual TSharedPtr<SViewport> GetGameViewportWidget() const override
	{
	return GameViewportWidget;
	}*/

	/**
	* This is a global, parameterless function used by the online subsystem modules.
	* It should never be used in gamecode - instead use the appropriate world context function
	* in order to properly support multiple concurrent UWorlds.
	*/
	UWorld* GetGameWorld();

protected:

	//const FSceneViewport* GetGameSceneViewport(UGameViewportClient* ViewportClient) const;

	/** Handle to a movie capture implementation to create on startup */
	//FMovieSceneCaptureHandle StartupMovieCaptureHandle;

	virtual void HandleBrowseToDefaultMapFailure(FWorldContext& Context, const FString& TextURL, const FString& Error) override;

private:

	virtual void HandleNetworkFailure_NotifyGameInstance(UWorld* World, UNetDriver* NetDriver, ENetworkFailure::Type FailureType) override;
	virtual void HandleTravelFailure_NotifyGameInstance(UWorld* World, ETravelFailure::Type FailureType) override;

	/** Last time the logs have been flushed. */
	double LastTimeLogsFlushed;

};