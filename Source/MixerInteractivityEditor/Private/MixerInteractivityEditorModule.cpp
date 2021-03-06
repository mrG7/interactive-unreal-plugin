//*********************************************************
//
// Copyright (c) Microsoft. All rights reserved.
// THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
// IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
// PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
//
//*********************************************************

#include "MixerInteractivityEditorModule.h"
#include "MixerInteractivityModule.h"
#include "MixerInteractivityTypes.h"
#include "MixerInteractivitySettings.h"
#include "PropertyEditorModule.h"
#include "ISettingsModule.h"
#include "EditorCategoryUtils.h"
#include "MixerInteractivitySettings.h"
#include "MixerInteractivityUserSettings.h"
#include "MixerInteractivitySettingsCustomization.h"
#include "MixerInteractivityPinFactory.h"
#include "MixerInteractiveGame.h"
#include "UObjectGlobals.h"
#include "IHttpRequest.h"
#include "IHttpResponse.h"
#include "HttpModule.h"
#include "JsonObject.h"
#include "JsonReader.h"
#include "JsonSerializer.h"

#define LOCTEXT_NAMESPACE "MixerInteractivityEditor"

class FMixerInteractivityEditorModule : public IMixerInteractivityEditorModule
{
public:
	virtual void StartupModule() override;

public:
	virtual bool RequestAvailableInteractiveGames(FOnMixerInteractiveGamesRequestFinished OnFinished);

	virtual bool RequestInteractiveControlsForGameVersion(const FMixerInteractiveGameVersion& Version, FOnMixerInteractiveControlsRequestFinished OnFinished);
	
	virtual const TArray<TSharedPtr<FName>>& GetDesignTimeButtons() { return DesignTimeButtons; }
	virtual const TArray<TSharedPtr<FName>>& GetDesignTimeSticks() { return DesignTimeSticks; }
	virtual const TArray<TSharedPtr<FName>>& GetDesignTimeScenes() { return DesignTimeScenes; }
	virtual const TArray<TSharedPtr<FName>>& GetDesignTimeGroups() { return DesignTimeGroups; }

	virtual void RefreshDesignTimeObjects();

	virtual FOnDesignTimeObjectsChanged& OnDesignTimeObjectsChanged() { return DesignTimeObjectsChanged; }

private:
	TArray<TSharedPtr<FName>> DesignTimeButtons;
	TArray<TSharedPtr<FName>> DesignTimeSticks;
	TArray<TSharedPtr<FName>> DesignTimeScenes;
	TArray<TSharedPtr<FName>> DesignTimeGroups;

	FOnDesignTimeObjectsChanged DesignTimeObjectsChanged;
};

IMPLEMENT_MODULE(FMixerInteractivityEditorModule, MixerInteractivityEditor)

void FMixerInteractivityEditorModule::StartupModule()
{
	FEditorCategoryUtils::RegisterCategoryKey("MixerInteractivity",
		LOCTEXT("MixerInteractivityCategory", "Mixer|Interactivity"),
		LOCTEXT("MixerInteractivityCategory_Tooltip", "Interactivity features provided by Mixer"));
	
	FPropertyEditorModule& PropertyModule = FModuleManager::LoadModuleChecked<FPropertyEditorModule>("PropertyEditor");
	PropertyModule.RegisterCustomClassLayout(
		"MixerInteractivitySettings",
		FOnGetDetailCustomizationInstance::CreateStatic(&FMixerInteractivitySettingsCustomization::MakeInstance)
	);

	ISettingsModule* SettingsModule = FModuleManager::GetModulePtr<ISettingsModule>("Settings");
	if (SettingsModule != nullptr)
	{
		SettingsModule->RegisterSettings("Project", "Plugins", "MixerInteractivity",
			LOCTEXT("MixerRuntimeSettingsName", "Mixer Interactivity"),
			LOCTEXT("MixerRuntimeSettingsDescription", "Configure the Mixer Interactivity plugin"),
			GetMutableDefault<UMixerInteractivitySettings>()
		);
	}

	TSharedPtr<FMixerInteractivityPinFactory> MixerInteractivityPinFactory = MakeShareable(new FMixerInteractivityPinFactory());
	FEdGraphUtilities::RegisterVisualPinFactory(MixerInteractivityPinFactory);

	RefreshDesignTimeObjects();
}

bool FMixerInteractivityEditorModule::RequestAvailableInteractiveGames(FOnMixerInteractiveGamesRequestFinished OnFinished)
{
	IMixerInteractivityModule& MixerRuntimeModule = IMixerInteractivityModule::Get();
	if (MixerRuntimeModule.GetLoginState() != EMixerLoginState::Logged_In)
	{
		return false;
	}

	const UMixerInteractivityUserSettings* UserSettings = GetDefault<UMixerInteractivityUserSettings>();
	TSharedPtr<const FMixerLocalUser> MixerUser = MixerRuntimeModule.GetCurrentUser();
	check(MixerUser.IsValid());
	FString GamesListUrl = FString::Printf(TEXT("https://mixer.com/api/v1/interactive/games/owned?user=%d"), MixerUser->Id);
	FString AuthZHeaderValue = FString::Printf(TEXT("Bearer %s"), *UserSettings->AccessToken);

	TSharedRef<IHttpRequest> GamesRequest = FHttpModule::Get().CreateRequest();
	GamesRequest->SetVerb(TEXT("GET"));
	GamesRequest->SetURL(GamesListUrl);
	GamesRequest->SetHeader(TEXT("Authorization"), AuthZHeaderValue);
	GamesRequest->OnProcessRequestComplete().BindLambda(
		[OnFinished](FHttpRequestPtr HttpRequest, FHttpResponsePtr HttpResponse, bool bSucceeded)
	{
		bool Success = false;
		TArray<FMixerInteractiveGame> GameCollection;
		if (bSucceeded && HttpResponse.IsValid())
		{
			if (EHttpResponseCodes::IsOk(HttpResponse->GetResponseCode()))
			{
				TArray<TSharedPtr<FJsonValue>> GameCollectionJson;
				TSharedRef<TJsonReader<> > JsonReader = TJsonReaderFactory<>::Create(HttpResponse->GetContentAsString());
				if (FJsonSerializer::Deserialize(JsonReader, GameCollectionJson))
				{
					Success = true;
					GameCollection.Reserve(GameCollectionJson.Num());
					for (TSharedPtr<FJsonValue>& GameJson : GameCollectionJson)
					{
						FMixerInteractiveGame Game;
						Game.FromJson(GameJson->AsObject());
						GameCollection.Add(Game);
					}
				}
			}
		}
		OnFinished.ExecuteIfBound(Success, GameCollection);
	});
	if (!GamesRequest->ProcessRequest())
	{
		return false;
	}

	return true;
}

bool FMixerInteractivityEditorModule::RequestInteractiveControlsForGameVersion(const FMixerInteractiveGameVersion& Version, FOnMixerInteractiveControlsRequestFinished OnFinished)
{
	FString ControlsForVersionUrl = FString::Printf(TEXT("https://mixer.com/api/v1/interactive/versions/%d"), Version.Id);

	TSharedRef<IHttpRequest> ControlsRequest = FHttpModule::Get().CreateRequest();
	ControlsRequest->SetVerb(TEXT("GET"));
	ControlsRequest->SetURL(ControlsForVersionUrl);
	ControlsRequest->OnProcessRequestComplete().BindLambda(
		[OnFinished](FHttpRequestPtr HttpRequest, FHttpResponsePtr HttpResponse, bool bSucceeded)
	{
		bool Success = false;
		FMixerInteractiveGameVersion VersionWithControls;
		if (bSucceeded && HttpResponse.IsValid())
		{
			if (EHttpResponseCodes::IsOk(HttpResponse->GetResponseCode()))
			{
				Success = VersionWithControls.FromJson(HttpResponse->GetContentAsString());
			}
		}

		OnFinished.ExecuteIfBound(Success, VersionWithControls);
	});

	if (!ControlsRequest->ProcessRequest())
	{
		return false;
	}

	return true;
}

void FMixerInteractivityEditorModule::RefreshDesignTimeObjects()
{
	const UMixerInteractivitySettings* Settings = GetDefault<UMixerInteractivitySettings>();
	DesignTimeButtons.Empty(Settings->CachedButtons.Num());
	for (FName Name : Settings->CachedButtons)
	{
		DesignTimeButtons.Add(MakeShareable(new FName(Name)));
	}

	DesignTimeSticks.Empty(Settings->CachedSticks.Num());
	for (FName Name : Settings->CachedSticks)
	{
		DesignTimeSticks.Add(MakeShareable(new FName(Name)));
	}

	DesignTimeScenes.Empty(Settings->CachedScenes.Num());
	for (FName Name : Settings->CachedScenes)
	{
		DesignTimeScenes.Add(MakeShareable(new FName(Name)));
	}

	DesignTimeGroups.Empty(Settings->DesignTimeGroups.Num() + 1);
	DesignTimeGroups.Add(MakeShareable(new FName("default")));
	for (const FMixerPredefinedGroup& Group : Settings->DesignTimeGroups)
	{
		DesignTimeGroups.Add(MakeShareable(new FName(Group.Name)));
	}

	DesignTimeObjectsChanged.Broadcast();
}

#undef LOCTEXT_NAMESPACE