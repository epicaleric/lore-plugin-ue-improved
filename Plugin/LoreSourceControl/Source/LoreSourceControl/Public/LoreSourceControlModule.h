#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleInterface.h"
#include "Modules/ModuleManager.h"
#include "LoreSourceControlSettings.h"
#include "LoreSourceControlProvider.h"

class FLoreSourceControlModule : public IModuleInterface
{
public:
	/** IModuleInterface */
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	/** Access the Lore settings. */
	FLoreSourceControlSettings& AccessSettings() { return LoreSettings; }
	const FLoreSourceControlSettings& AccessSettings() const { return LoreSettings; }

	/** Persist the Lore settings. */
	void SaveSettings();

	/** The one and only Lore provider. */
	FLoreSourceControlProvider& GetProvider() { return LoreProvider; }
	const FLoreSourceControlProvider& GetProvider() const { return LoreProvider; }

	/** Loaded-module accessor (asserts the module is loaded). */
	static FLoreSourceControlModule& Get()
	{
		return FModuleManager::LoadModuleChecked<FLoreSourceControlModule>("LoreSourceControl");
	}

	/** Safe accessor usable off the game thread; returns nullptr if not loaded. */
	static FLoreSourceControlModule* GetPtr()
	{
		return FModuleManager::GetModulePtr<FLoreSourceControlModule>("LoreSourceControl");
	}

private:
	void RegisterWorkers();

	/** Adds the Lore window to the Revision Control menu in the status bar. */
	void RegisterMenus();

	/** A project that is a Lore working copy uses Lore without any setup: server and name come from its .lore folder. */
	void ConfigureFromWorkingCopy();

	FDelegateHandle PostEngineInitHandle;

	FLoreSourceControlProvider LoreProvider;
	FLoreSourceControlSettings LoreSettings;
};
