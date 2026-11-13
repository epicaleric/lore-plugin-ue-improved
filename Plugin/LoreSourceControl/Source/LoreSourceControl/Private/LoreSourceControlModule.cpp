#include "LoreSourceControlModule.h"
#include "LoreSourceControlOperations.h"
#include "LoreSourceControlLog.h"
#include "LoreHistoryTab.h"

#include "Containers/Ticker.h"
#include "Features/IModularFeatures.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "ISourceControlModule.h"
#include "Modules/ModuleManager.h"
#include "Misc/App.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/CoreDelegates.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "ToolMenus.h"

#define LOCTEXT_NAMESPACE "LoreSourceControl"

/** Console commands `Lore.ShowHistory` / `Lore.ShowLocks`: open the Lore History window. */
static FAutoConsoleCommand GLoreShowHistoryCommand(
	TEXT("Lore.ShowHistory"),
	TEXT("Open the Lore History window (revisions, restore, locked files)."),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>&) { LoreHistoryTab::Open(); }));

static FAutoConsoleCommand GLoreShowLocksCommand(
	TEXT("Lore.ShowLocks"),
	TEXT("Open the Lore History window (revisions, restore, locked files)."),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>&) { LoreHistoryTab::Open(); }));

template<typename Type>
static TSharedRef<ILoreSourceControlWorker, ESPMode::ThreadSafe> CreateWorker()
{
	return MakeShared<Type, ESPMode::ThreadSafe>();
}

void FLoreSourceControlModule::StartupModule()
{
	LoreSettings.LoadSettings();

	RegisterWorkers();

	// Bind our provider to the editor's modular source-control feature.
	IModularFeatures::Get().RegisterModularFeature("SourceControl", &LoreProvider);

	LoreHistoryTab::Register();

	if (GIsEditor && !IsRunningCommandlet())
	{
		UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FLoreSourceControlModule::RegisterMenus));
		PostEngineInitHandle = FCoreDelegates::OnPostEngineInit.AddRaw(this, &FLoreSourceControlModule::ConfigureFromWorkingCopy);
	}

	UE_LOG(LogLoreSourceControl, Log, TEXT("Lore source control module started."));
}

void FLoreSourceControlModule::ShutdownModule()
{
	FCoreDelegates::OnPostEngineInit.Remove(PostEngineInitHandle);
	if (UObjectInitialized())
	{
		UToolMenus::UnRegisterStartupCallback(this);
		UToolMenus::UnregisterOwner(this);
	}

	LoreHistoryTab::Unregister();

	LoreProvider.Close();

	IModularFeatures::Get().UnregisterModularFeature("SourceControl", &LoreProvider);

	UE_LOG(LogLoreSourceControl, Log, TEXT("Lore source control module shut down."));
}

void FLoreSourceControlModule::SaveSettings()
{
	if (FApp::IsUnattended() || IsRunningCommandlet())
	{
		return;
	}
	LoreSettings.SaveSettings();
}

void FLoreSourceControlModule::RegisterMenus()
{
	FToolMenuOwnerScoped OwnerScoped(this);
	UToolMenu* Menu = UToolMenus::Get()->ExtendMenu("StatusBar.ToolBar.SourceControl");
	FToolMenuSection& Section = Menu->AddSection("Lore", LOCTEXT("LoreMenuSection", "Lore"));
	Section.AddMenuEntry(
		"OpenLoreRevisionControl",
		LOCTEXT("OpenLoreWindow", "Lore Revision Control"),
		LOCTEXT("OpenLoreWindowTooltip", "Open the Lore window: changes, history, locked files and settings."),
		FSlateIcon(),
		FUIAction(FExecuteAction::CreateStatic(&LoreHistoryTab::Open)));
}

void FLoreSourceControlModule::ConfigureFromWorkingCopy()
{
	const FString LoreFolder = FPaths::Combine(FPaths::ProjectDir(), TEXT(".lore"));
	FString Config;
	if (!IFileManager::Get().DirectoryExists(*LoreFolder) || !FFileHelper::LoadFileToString(Config, *FPaths::Combine(LoreFolder, TEXT("config.toml"))))
	{
		return;
	}
	// UEFN projects have their own revision control.
	if (Config.Contains(TEXT(".epicgames.com")))
	{
		return;
	}

	// Another provider the person chose stays as it is.
	ISourceControlModule& SourceControl = ISourceControlModule::Get();
	const bool bNoProvider = SourceControl.GetProvider().GetName() == FName("None");
	if (!bNoProvider && SourceControl.GetProvider().GetName() != LoreProvider.GetName())
	{
		return;
	}

	TArray<FString> Lines;
	Config.ParseIntoArrayLines(Lines);
	const auto ValueOf = [&Lines](const TCHAR* InKey)
	{
		for (const FString& Line : Lines)
		{
			FString Key, Value;
			if (Line.Split(TEXT("="), &Key, &Value) && Key.TrimStartAndEnd() == InKey)
			{
				return Value.TrimStartAndEnd().TrimQuotes();
			}
		}
		return FString();
	};

	bool bChanged = false;
	if (LoreSettings.GetRepositoryUrl().IsEmpty() && !ValueOf(TEXT("remote_url")).IsEmpty())
	{
		LoreSettings.SetRepositoryUrl(ValueOf(TEXT("remote_url")));
		bChanged = true;
	}
	if (LoreSettings.GetUserName().IsEmpty() && !ValueOf(TEXT("identity")).IsEmpty())
	{
		LoreSettings.SetUserName(ValueOf(TEXT("identity")));
		bChanged = true;
	}
	if (bChanged)
	{
		SaveSettings();
	}

	if (bNoProvider)
	{
		SourceControl.SetProvider(LoreProvider.GetName());
	}
	if (bNoProvider || bChanged)
	{
		LoreProvider.Init(false);
		LoreProvider.Login(FString(), EConcurrency::Asynchronous);
		UE_LOG(LogLoreSourceControl, Log, TEXT("Lore revision control was set up from the working copy."));
	}

	// The window opens by itself the first time, afterwards the editor's layout remembers it.
	const TCHAR* const Section = TEXT("LoreSourceControl.UI");
	bool bOpenedBefore = false;
	GConfig->GetBool(Section, TEXT("WindowOpenedOnce"), bOpenedBefore, GEditorPerProjectIni);
	if (!bOpenedBefore)
	{
		GConfig->SetBool(Section, TEXT("WindowOpenedOnce"), true, GEditorPerProjectIni);
		GConfig->Flush(false, GEditorPerProjectIni);
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([](float)
		{
			LoreHistoryTab::Open();
			return false;
		}), 5.0f);
	}
}

void FLoreSourceControlModule::RegisterWorkers()
{
	LoreProvider.RegisterWorker("Connect", FGetLoreSourceControlWorker::CreateStatic(&CreateWorker<FLoreConnectWorker>));
	LoreProvider.RegisterWorker("UpdateStatus", FGetLoreSourceControlWorker::CreateStatic(&CreateWorker<FLoreUpdateStatusWorker>));
	LoreProvider.RegisterWorker("CheckOut", FGetLoreSourceControlWorker::CreateStatic(&CreateWorker<FLoreCheckOutWorker>));
	LoreProvider.RegisterWorker("CheckIn", FGetLoreSourceControlWorker::CreateStatic(&CreateWorker<FLoreCheckInWorker>));
	LoreProvider.RegisterWorker("MarkForAdd", FGetLoreSourceControlWorker::CreateStatic(&CreateWorker<FLoreMarkForAddWorker>));
	LoreProvider.RegisterWorker("Delete", FGetLoreSourceControlWorker::CreateStatic(&CreateWorker<FLoreDeleteWorker>));
	LoreProvider.RegisterWorker("Revert", FGetLoreSourceControlWorker::CreateStatic(&CreateWorker<FLoreRevertWorker>));
	LoreProvider.RegisterWorker("Sync", FGetLoreSourceControlWorker::CreateStatic(&CreateWorker<FLoreSyncWorker>));
	LoreProvider.RegisterWorker("SyncPreview", FGetLoreSourceControlWorker::CreateStatic(&CreateWorker<FLoreSyncPreviewWorker>));
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FLoreSourceControlModule, LoreSourceControl);
