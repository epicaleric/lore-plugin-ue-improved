#include "LoreSourceControlProvider.h"
#include "LoreSourceControlCommand.h"
#include "LoreSourceControlModule.h"
#include "LoreSourceControlState.h"
#include "LoreSourceControlUtils.h"
#include "LoreSourceControlLog.h"
#include "ILoreSourceControlWorker.h"
#include "LorePathUtils.h"

#include "ISourceControlModule.h"
#include "ISourceControlOperation.h"
#include "SourceControlOperations.h"
#include "SourceControlHelpers.h"
#include "ScopedSourceControlProgress.h"
#include "Logging/MessageLog.h"
#include "Misc/MessageDialog.h"
#include "Misc/Paths.h"
#include "Misc/App.h"
#include "Misc/QueuedThreadPool.h"
#include "HAL/PlatformProcess.h"
#include "HAL/IConsoleManager.h"

/** Check out (and lock) files as soon as they are saved, so edits never race another user. */
static TAutoConsoleVariable<bool> CVarLoreAutoCheckOutOnSave(
	TEXT("Lore.AutoCheckOutOnSave"),
	true,
	TEXT("Automatically check out and lock a file in Lore when it is saved in the editor."));

/** Refuse to save files someone else holds the Lore lock on (Lore itself does not stop a push over a lock). */
static TAutoConsoleVariable<bool> CVarLoreBlockSaveOfLockedFiles(
	TEXT("Lore.BlockSaveOfLockedFiles"),
	true,
	TEXT("Refuse to save an asset while another user holds its Lore lock."));
#include "UObject/Package.h"
#include "UObject/ObjectSaveContext.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/Linker.h"
#include "Misc/CoreDelegates.h"
#include "Misc/PackageName.h"
#include "Misc/ScopedSlowTask.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Engine/Engine.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Widgets/Text/STextBlock.h"
#include "Dialog/SCustomDialog.h"
#include "Framework/Application/SlateApplication.h"

#if SOURCE_CONTROL_WITH_SLATE
#include "SLoreSourceControlSettingsWidget.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#endif

#define LOCTEXT_NAMESPACE "LoreSourceControl"

static FName ProviderName("Lore");

void FLoreSourceControlProvider::Init(bool bForceConnection)
{
	RefreshConnectionSettings();

	if (LoreBinaryPath.IsEmpty())
	{
		ConnectionState = ELoreConnectionState::Error;
		return;
	}

	ConnectionState = ELoreConnectionState::Offline;

	// The editor never re-queries status after a save, so hook package writes to
	// keep Content Browser overlays in sync. Init() is called repeatedly during
	// startup, so guard against registering more than once.
	if (!OnPackageSavedHandle.IsValid())
	{
		OnPackageSavedHandle = UPackage::PackageSavedWithContextEvent.AddRaw(this, &FLoreSourceControlProvider::HandlePackageSaved);
	}

	// The editor binds IsPackageOKToSave while the engine initializes; wrap it only after that.
	if (GIsEditor && !IsRunningCommandlet() && !bSaveGuardInstalled && !PostEngineInitHandle.IsValid())
	{
		if (GEngine != nullptr && GEngine->IsInitialized())
		{
			InstallSaveGuard();
		}
		else
		{
			PostEngineInitHandle = FCoreDelegates::OnPostEngineInit.AddRaw(this, &FLoreSourceControlProvider::InstallSaveGuard);
		}
	}

	if (bForceConnection)
	{
		// A Connect operation will refine the connection state asynchronously.
		Execute(ISourceControlOperation::Create<FConnect>(), TArray<FString>(), EConcurrency::Synchronous);
	}
}

void FLoreSourceControlProvider::Close()
{
	if (OnPackageSavedHandle.IsValid())
	{
		UPackage::PackageSavedWithContextEvent.Remove(OnPackageSavedHandle);
		OnPackageSavedHandle.Reset();
	}
	RemoveSaveGuard();

	PackagesToRefresh.Empty();
	StateCache.Empty();
	ConnectionState = ELoreConnectionState::Disconnected;
}

void FLoreSourceControlProvider::RefreshConnectionSettings()
{
	FLoreSourceControlModule& Module = FLoreSourceControlModule::Get();
	FLoreSourceControlSettings& Settings = Module.AccessSettings();

	const FString ConfiguredBinary = Settings.GetBinaryPath();
	LoreBinaryPath = LoreSourceControlUtils::FindLoreBinaryPath(ConfiguredBinary);

	// When the user hasn't pinned an explicit path, persist whatever we resolved
	// from PATH/bundled so the settings UI shows the detected `lore` executable.
	if (ConfiguredBinary.IsEmpty() && !LoreBinaryPath.IsEmpty())
	{
		Settings.SetBinaryPath(LoreBinaryPath);
		Settings.SaveSettings();
	}

	UserName = Settings.GetUserName();

	if (WorkingCopyRoot.IsEmpty())
	{
		WorkingCopyRoot = FLorePathUtils::NormalizeAbsolutePath(FPaths::ProjectDir());
	}
}

TSharedRef<FLoreSourceControlState, ESPMode::ThreadSafe> FLoreSourceControlProvider::GetStateInternal(const FString& Filename)
{
	const FString Key = FLorePathUtils::MakeCacheKey(Filename);
	if (TSharedRef<FLoreSourceControlState, ESPMode::ThreadSafe>* State = StateCache.Find(Key))
	{
		return *State;
	}

	TSharedRef<FLoreSourceControlState, ESPMode::ThreadSafe> NewState = MakeShared<FLoreSourceControlState, ESPMode::ThreadSafe>(Filename);
	StateCache.Add(Key, NewState);
	return NewState;
}

bool FLoreSourceControlProvider::RemoveFileFromCache(const FString& Filename)
{
	return StateCache.Remove(FLorePathUtils::MakeCacheKey(Filename)) > 0;
}

void FLoreSourceControlProvider::HandlePackageSaved(const FString& InPackageFilename, UPackage* InPackage, FObjectPostSaveContext InObjectSaveContext)
{
	// Ignore saves during cooking/automation; only interactive edits matter here.
	if (FApp::IsUnattended() || IsRunningCommandlet())
	{
		return;
	}

	const FString AbsoluteFilename = FLorePathUtils::NormalizeAbsolutePath(FPaths::ConvertRelativePathToFull(InPackageFilename));

	const FString Root = WorkingCopyRoot.IsEmpty()
		? FLorePathUtils::NormalizeAbsolutePath(FPaths::ProjectDir())
		: WorkingCopyRoot;
	if (!FLorePathUtils::IsUnderWorkspace(AbsoluteFilename, Root))
	{
		return;
	}

	// Optimistically reflect the edit so the overlay flips immediately. A file
	// that was clean (or never scanned) is now locally modified; leave any
	// existing pending add/delete/conflict state alone and let the confirming
	// status pass (queued below, drained in Tick) report the authoritative state.
	TSharedRef<FLoreSourceControlState, ESPMode::ThreadSafe> State = GetStateInternal(AbsoluteFilename);
	if (State->WorkingState == ELoreWorkingState::Clean || State->WorkingState == ELoreWorkingState::Unknown)
	{
		State->WorkingState = ELoreWorkingState::Modified;
		State->TimeStamp = FDateTime::Now();
	}

	// PackageSavedWithContextEvent fires once per asset (hundreds for OFPA maps),
	// so coalesce into a single batched refresh on the next Tick.
	PackagesToRefresh.AddUnique(AbsoluteFilename);

	// Queue an automatic checkout unless another user already holds the lock.
	// Files we already hold are skipped, so repeated saves do not re-request the lock.
	if (CVarLoreAutoCheckOutOnSave.GetValueOnGameThread()
		&& State->LockState != ELoreLockState::LockedByMe
		&& State->LockState != ELoreLockState::LockedByOther)
	{
		PackagesToCheckOut.Add(AbsoluteFilename);
		UE_LOG(LogLoreSourceControl, Log, TEXT("Auto checkout queued after save: %s"), *AbsoluteFilename);
	}
	else
	{
		UE_LOG(LogLoreSourceControl, Log, TEXT("Auto checkout skipped after save (enabled=%d, lock state=%d): %s"),
			CVarLoreAutoCheckOutOnSave.GetValueOnGameThread() ? 1 : 0, static_cast<int32>(State->LockState), *AbsoluteFilename);

		// Only reached when Lore.BlockSaveOfLockedFiles is off: the save went through, but say plainly that
		// this change collides with someone else's lock instead of skipping the checkout without a word.
		if (State->LockState == ELoreLockState::LockedByOther)
		{
			const FText Message = FText::Format(LOCTEXT("SavedOverForeignLock", "{0} is locked by {1} in Lore. Your saved change cannot be submitted until {1} submits and releases it; revert it or agree with {1} who keeps which version."),
				FText::FromString(FPaths::GetCleanFilename(AbsoluteFilename)), FText::FromString(State->LockUser));
			UE_LOG(LogLoreSourceControl, Warning, TEXT("%s"), *Message.ToString());
			FMessageLog("SourceControl").Warning(Message);
			FNotificationInfo Info(Message);
			Info.ExpireDuration = 10.0f;
			FSlateNotificationManager::Get().AddNotification(Info);
		}
	}
}

void FLoreSourceControlProvider::InstallSaveGuard()
{
	if (PostEngineInitHandle.IsValid())
	{
		FCoreDelegates::OnPostEngineInit.Remove(PostEngineInitHandle);
		PostEngineInitHandle.Reset();
	}
	if (bSaveGuardInstalled)
	{
		return;
	}
	PreviousIsPackageOKToSave = FCoreUObjectDelegates::IsPackageOKToSaveDelegate;
	FCoreUObjectDelegates::IsPackageOKToSaveDelegate.BindRaw(this, &FLoreSourceControlProvider::HandleIsPackageOKToSave);
	bSaveGuardInstalled = true;
	UE_LOG(LogLoreSourceControl, Log, TEXT("Save guard installed: assets locked by another user cannot be saved (Lore.BlockSaveOfLockedFiles)."));
}

void FLoreSourceControlProvider::RemoveSaveGuard()
{
	if (PostEngineInitHandle.IsValid())
	{
		FCoreDelegates::OnPostEngineInit.Remove(PostEngineInitHandle);
		PostEngineInitHandle.Reset();
	}
	if (!bSaveGuardInstalled)
	{
		return;
	}
	FCoreUObjectDelegates::IsPackageOKToSaveDelegate = PreviousIsPackageOKToSave;
	PreviousIsPackageOKToSave.Unbind();
	bSaveGuardInstalled = false;
}

bool FLoreSourceControlProvider::IsLockedByOtherUser(const FString& InAbsoluteFilename, FString& OutOwner)
{
	const FLoreSourceControlState* Known = nullptr;
	if (const TSharedRef<FLoreSourceControlState, ESPMode::ThreadSafe>* Cached = StateCache.Find(FLorePathUtils::MakeCacheKey(InAbsoluteFilename)))
	{
		Known = &Cached->Get();
	}

	// A lock can be taken at any moment, so read the server's lock list instead of trusting the cache.
	// One query covers every file; saving a level with hundreds of actor packages reuses it.
	const double Now = FPlatformTime::Seconds();
	if (SaveGuardLocksTime < 0.0 || Now - SaveGuardLocksTime > 5.0)
	{
		LoreSourceControlUtils::FLoreCommandContext Context = LoreSourceControlUtils::GetCommandContextFromModule();
		Context.TimeoutSeconds = 10.0; // the save waits on this; an unreachable server must not freeze it
		TArray<FString> Lines;
		TArray<FString> Errors;
		int32 ReturnCode = -1;
		SaveGuardLocks.Reset();
		bSaveGuardLocksValid = LoreSourceControlUtils::RunCommand(TEXT("lock query"), TArray<FString>(), TArray<FString>(), Context, Lines, Errors, ReturnCode);
		if (bSaveGuardLocksValid)
		{
			LoreSourceControlUtils::ParseLockQueryResults(Lines, SaveGuardLocks);
		}
		SaveGuardLocksTime = Now;
	}

	if (!bSaveGuardLocksValid)
	{
		// Offline or the server did not answer: fall back to what the last status pass knew.
		if (Known != nullptr && Known->LockState == ELoreLockState::LockedByOther)
		{
			OutOwner = Known->LockUser;
			return true;
		}
		return false;
	}

	const LoreSourceControlUtils::FLoreCommandContext Context = LoreSourceControlUtils::GetCommandContextFromModule();
	const FString CurrentUser = UserName.IsEmpty() ? LoreSourceControlUtils::DetectAuthenticatedUser(Context) : UserName;
	TArray<FLoreSourceControlState> Probe;
	Probe.Emplace(InAbsoluteFilename);
	LoreSourceControlUtils::ApplyLocksToStates(SaveGuardLocks, CurrentUser, WorkingCopyRoot, Probe);

	// Keep the cache in step, so overlays and the checkout prompt show the same holder.
	if (Probe[0].LockState == ELoreLockState::LockedByOther || (Known != nullptr && Known->LockState == ELoreLockState::LockedByOther))
	{
		TSharedRef<FLoreSourceControlState, ESPMode::ThreadSafe> State = GetStateInternal(InAbsoluteFilename);
		State->LockState = Probe[0].LockState == ELoreLockState::Unknown ? ELoreLockState::NotLocked : Probe[0].LockState;
		State->LockUser = Probe[0].LockUser;
		State->LockBranch = Probe[0].LockBranch;
	}

	if (Probe[0].LockState == ELoreLockState::LockedByOther)
	{
		OutOwner = Probe[0].LockUser;
		return true;
	}
	return false;
}

bool FLoreSourceControlProvider::HandleIsPackageOKToSave(UPackage* InPackage, const FString& InFilename, FOutputDevice* InError)
{
	if (CVarLoreBlockSaveOfLockedFiles.GetValueOnGameThread() && IsInGameThread() && !IsRunningCommandlet() && IsAvailable()
		&& ISourceControlModule::Get().IsEnabled() && &ISourceControlModule::Get().GetProvider() == this)
	{
		const FString AbsoluteFilename = FLorePathUtils::NormalizeAbsolutePath(FPaths::ConvertRelativePathToFull(InFilename));
		const FString Root = WorkingCopyRoot.IsEmpty() ? FLorePathUtils::NormalizeAbsolutePath(FPaths::ProjectDir()) : WorkingCopyRoot;
		FString Owner;
		if (FLorePathUtils::IsUnderWorkspace(AbsoluteFilename, Root) && IsLockedByOtherUser(AbsoluteFilename, Owner))
		{
			const FText Message = FText::Format(LOCTEXT("SaveBlockedByLock", "Cannot save {0}: {1} holds its lock in Lore. Ask {1} to submit and release it, sync, and then make your change. (Console: Lore.BlockSaveOfLockedFiles 0 turns this check off.)"),
				FText::FromString(FLorePathUtils::ToWorkspaceRelativePath(AbsoluteFilename, Root)), FText::FromString(Owner));
			UE_LOG(LogLoreSourceControl, Warning, TEXT("%s"), *Message.ToString());
			if (InError != nullptr)
			{
				InError->Logf(ELogVerbosity::Warning, TEXT("%s"), *Message.ToString());
			}
			FMessageLog("SourceControl").Error(Message);
			if (!FApp::IsUnattended())
			{
				FNotificationInfo Info(Message);
				Info.ExpireDuration = 10.0f;
				FSlateNotificationManager::Get().AddNotification(Info);
			}
			return false;
		}
	}

	return PreviousIsPackageOKToSave.IsBound() ? PreviousIsPackageOKToSave.Execute(InPackage, InFilename, InError) : true;
}

FText FLoreSourceControlProvider::GetStatusText() const
{
	FFormatNamedArguments Args;
	Args.Add(TEXT("IsEnabled"), IsEnabled() ? LOCTEXT("Yes", "Yes") : LOCTEXT("No", "No"));
	Args.Add(TEXT("IsAvailable"), IsAvailable() ? LOCTEXT("Yes", "Yes") : LOCTEXT("No", "No"));
	Args.Add(TEXT("BinaryPath"), FText::FromString(LoreBinaryPath));
	Args.Add(TEXT("WorkingCopy"), FText::FromString(WorkingCopyRoot));
	Args.Add(TEXT("UserName"), FText::FromString(UserName));

	return FText::Format(LOCTEXT("ProviderStatusText", "Provider: Lore\nEnabled: {IsEnabled}\nAvailable: {IsAvailable}\nlore binary: {BinaryPath}\nWorking copy: {WorkingCopy}\nUser: {UserName}"), Args);
}

TMap<ISourceControlProvider::EStatus, FString> FLoreSourceControlProvider::GetStatus() const
{
	TMap<EStatus, FString> Result;
	Result.Add(EStatus::Enabled, IsEnabled() ? TEXT("Yes") : TEXT("No"));
	Result.Add(EStatus::Connected, (IsEnabled() && IsAvailable()) ? TEXT("Yes") : TEXT("No"));
	Result.Add(EStatus::Repository, WorkingCopyRoot);
	Result.Add(EStatus::User, UserName);
	return Result;
}

bool FLoreSourceControlProvider::IsEnabled() const
{
	return true;
}

bool FLoreSourceControlProvider::IsAvailable() const
{
	return !LoreBinaryPath.IsEmpty() && ConnectionState != ELoreConnectionState::Error;
}

const FName& FLoreSourceControlProvider::GetName() const
{
	return ProviderName;
}

ECommandResult::Type FLoreSourceControlProvider::GetState(const TArray<FString>& InFiles, TArray<FSourceControlStateRef>& OutState, EStateCacheUsage::Type InStateCacheUsage)
{
	if (!IsEnabled())
	{
		return ECommandResult::Failed;
	}

	const TArray<FString> AbsoluteFiles = SourceControlHelpers::AbsoluteFilenames(InFiles);

	if (InStateCacheUsage == EStateCacheUsage::ForceUpdate)
	{
		Execute(ISourceControlOperation::Create<FUpdateStatus>(), AbsoluteFiles);
	}

	for (const FString& File : AbsoluteFiles)
	{
		OutState.Add(GetStateInternal(File));
	}

	return ECommandResult::Succeeded;
}

ECommandResult::Type FLoreSourceControlProvider::GetState(const TArray<FSourceControlChangelistRef>& InChangelists, TArray<FSourceControlChangelistStateRef>& OutState, EStateCacheUsage::Type InStateCacheUsage)
{
	return ECommandResult::Failed;
}

TArray<FSourceControlStateRef> FLoreSourceControlProvider::GetCachedStateByPredicate(TFunctionRef<bool(const FSourceControlStateRef&)> Predicate) const
{
	TArray<FSourceControlStateRef> Result;
	for (const auto& CacheItem : StateCache)
	{
		FSourceControlStateRef State = CacheItem.Value;
		if (Predicate(State))
		{
			Result.Add(State);
		}
	}
	return Result;
}

FDelegateHandle FLoreSourceControlProvider::RegisterSourceControlStateChanged_Handle(const FSourceControlStateChanged::FDelegate& SourceControlStateChanged)
{
	return OnSourceControlStateChanged.Add(SourceControlStateChanged);
}

void FLoreSourceControlProvider::UnregisterSourceControlStateChanged_Handle(FDelegateHandle Handle)
{
	OnSourceControlStateChanged.Remove(Handle);
}

ECommandResult::Type FLoreSourceControlProvider::Execute(const FSourceControlOperationRef& InOperation, FSourceControlChangelistPtr InChangelist, const TArray<FString>& InFiles, EConcurrency::Type InConcurrency, const FSourceControlOperationComplete& InOperationCompleteDelegate)
{
	if (!IsEnabled())
	{
		InOperationCompleteDelegate.ExecuteIfBound(InOperation, ECommandResult::Failed);
		return ECommandResult::Failed;
	}

	const TArray<FString> AbsoluteFiles = SourceControlHelpers::AbsoluteFilenames(InFiles);

	TSharedPtr<ILoreSourceControlWorker, ESPMode::ThreadSafe> Worker = CreateWorker(InOperation->GetName());
	if (!Worker.IsValid())
	{
		FFormatNamedArguments Arguments;
		Arguments.Add(TEXT("OperationName"), FText::FromName(InOperation->GetName()));
		Arguments.Add(TEXT("ProviderName"), FText::FromName(GetName()));
		const FText Message(FText::Format(LOCTEXT("UnsupportedOperation", "Operation '{OperationName}' not supported by revision control provider '{ProviderName}'"), Arguments));
		FMessageLog("SourceControl").Error(Message);
		InOperation->AddErrorMessge(Message);
		InOperationCompleteDelegate.ExecuteIfBound(InOperation, ECommandResult::Failed);
		return ECommandResult::Failed;
	}

	FLoreSourceControlCommand* Command = new FLoreSourceControlCommand(InOperation, Worker.ToSharedRef(), InOperationCompleteDelegate);
	Command->Files = AbsoluteFiles;

	if (InConcurrency == EConcurrency::Synchronous)
	{
		Command->bAutoDelete = false;
		const bool bIsSync = InOperation->GetName() == "Sync";
		if (bIsSync)
		{
			LastSyncConflicts.Reset();
		}
		ECommandResult::Type Result = ExecuteSynchronousCommand(*Command, InOperation->GetInProgressString());
		if (bIsSync && Result != ECommandResult::Succeeded && !LastSyncConflicts.IsEmpty() && !bResolvingSyncConflicts)
		{
			Result = ResolveSyncConflicts(InOperation, AbsoluteFiles, MoveTemp(LastSyncConflicts));
		}
		return Result;
	}

	Command->bAutoDelete = true;
	return IssueCommand(*Command, false);
}

bool FLoreSourceControlProvider::CanExecuteOperation(const FSourceControlOperationRef& InOperation) const
{
	return WorkersMap.Find(InOperation->GetName()) != nullptr;
}

bool FLoreSourceControlProvider::CanCancelOperation(const FSourceControlOperationRef& InOperation) const
{
	// Cancelling kills the running `lore` process. That is harmless for reads and for a sync (the next
	// sync finishes the job); a commit or push is left to run to the end.
	const FName Name = InOperation->GetName();
	return Name == "Sync" || Name == "UpdateStatus" || Name == "Connect";
}

void FLoreSourceControlProvider::CancelOperation(const FSourceControlOperationRef& InOperation)
{
	for (FLoreSourceControlCommand* Command : CommandQueue)
	{
		if (Command->Operation == InOperation && CanCancelOperation(InOperation))
		{
			Command->bCancelRequested = true;
		}
	}
}

bool FLoreSourceControlProvider::UsesLocalReadOnlyState() const
{
	// Lore does not manage the read-only bit on the working copy.
	return false;
}

bool FLoreSourceControlProvider::UsesChangelists() const
{
	return false;
}

bool FLoreSourceControlProvider::UsesUncontrolledChangelists() const
{
	return true;
}

bool FLoreSourceControlProvider::UsesCheckout() const
{
	return true;
}

bool FLoreSourceControlProvider::UsesFileRevisions() const
{
	return true;
}

bool FLoreSourceControlProvider::UsesSnapshots() const
{
	return false;
}

bool FLoreSourceControlProvider::AllowsDiffAgainstDepot() const
{
	// TryToDownloadFileFromBackgroundThread is not yet implemented.
	// FLoreSourceControlRevision::Get() materializes revisions for
	// explicit history-view diffs, but the background download hook
	// that this flag gates is unimplemented. Return false to avoid
	// triggering the unimplemented path.
	// Set to true once TryToDownloadFileFromBackgroundThread is wired up.
	return false;
}

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION < 8
TOptional<bool> FLoreSourceControlProvider::IsAtLatestRevision() const
{
	return TOptional<bool>();
}

TOptional<int> FLoreSourceControlProvider::GetNumLocalChanges() const
{
	return TOptional<int>();
}
#endif
TSharedPtr<ILoreSourceControlWorker, ESPMode::ThreadSafe> FLoreSourceControlProvider::CreateWorker(const FName& InOperationName) const
{
	if (const FGetLoreSourceControlWorker* Operation = WorkersMap.Find(InOperationName))
	{
		return Operation->Execute();
	}
	return nullptr;
}

void FLoreSourceControlProvider::RegisterWorker(const FName& InName, const FGetLoreSourceControlWorker& InDelegate)
{
	WorkersMap.Add(InName, InDelegate);
}

void FLoreSourceControlProvider::OutputCommandMessages(const FLoreSourceControlCommand& InCommand) const
{
	FMessageLog SourceControlLog("SourceControl");

	for (const FString& Error : InCommand.ErrorMessages)
	{
		SourceControlLog.Error(FText::FromString(Error));
	}
	for (const FString& Info : InCommand.InfoMessages)
	{
		SourceControlLog.Info(FText::FromString(Info));
	}
}

void FLoreSourceControlProvider::Tick()
{
	// Send queued auto-checkouts as one batch, once no other command is running.
	if (!PackagesToCheckOut.IsEmpty() && CommandQueue.IsEmpty())
	{
		TArray<FString> Files = PackagesToCheckOut.Array();
		PackagesToCheckOut.Empty();
		Execute(ISourceControlOperation::Create<FCheckOut>(), nullptr, Files, EConcurrency::Asynchronous);
	}

	bool bStatesUpdated = false;
	for (int32 CommandIndex = 0; CommandIndex < CommandQueue.Num(); ++CommandIndex)
	{
		FLoreSourceControlCommand& Command = *CommandQueue[CommandIndex];
		if (Command.bExecuteProcessed)
		{
			CommandQueue.RemoveAt(CommandIndex);

			bStatesUpdated |= Command.Worker->UpdateStates();

			// Refine the connection state from the result.
			if (Command.Operation->GetName() == "Connect" && !Command.bCommandSuccessful)
			{
				ConnectionState = ELoreConnectionState::Offline;
			}
			else if (Command.bCommandSuccessful)
			{
				ConnectionState = ELoreConnectionState::Connected;
			}

			OutputCommandMessages(Command);

			Command.ReturnResults();

			if (Command.bAutoDelete)
			{
				delete &Command;
			}

			// Only process one command per tick to avoid concurrent queue modification.
			break;
		}
	}

	if (bStatesUpdated)
	{
		OnSourceControlStateChanged.Broadcast();
	}

	// Drain any files saved since the last tick. Broadcasting now repaints the
	// optimistic "modified" overlay immediately; the async status pass then
	// confirms or corrects each file's true state (and broadcasts again when it
	// completes via the command-drain path above).
	if (PackagesToRefresh.Num() > 0)
	{
		TArray<FString> Files = MoveTemp(PackagesToRefresh);
		PackagesToRefresh.Reset();

		OnSourceControlStateChanged.Broadcast();

		if (IsAvailable())
		{
			Execute(ISourceControlOperation::Create<FUpdateStatus>(), Files, EConcurrency::Asynchronous);
		}
	}
}

TArray<TSharedRef<ISourceControlLabel>> FLoreSourceControlProvider::GetLabels(const FString& InMatchingSpec) const
{
	// Lore branches are not surfaced as labels in v1.
	return TArray<TSharedRef<ISourceControlLabel>>();
}

TArray<FSourceControlChangelistRef> FLoreSourceControlProvider::GetChangelists(EStateCacheUsage::Type InStateCacheUsage)
{
	return TArray<FSourceControlChangelistRef>();
}

#if SOURCE_CONTROL_WITH_SLATE
TSharedRef<class SWidget> FLoreSourceControlProvider::MakeSettingsWidget() const
{
	return SNew(SLoreSourceControlSettings);
}
#endif

ECommandResult::Type FLoreSourceControlProvider::ExecuteSynchronousCommand(FLoreSourceControlCommand& InCommand, const FText& Task)
{
	ECommandResult::Type Result = ECommandResult::Failed;

	{
		// A Cancel button on the progress window, so a long or stuck sync never traps the user.
		FSimpleDelegate OnCancelled;
		if (CanCancelOperation(InCommand.Operation))
		{
			OnCancelled = FSimpleDelegate::CreateLambda([&InCommand]() { InCommand.bCancelRequested = true; });
		}
		FScopedSourceControlProgress Progress(Task, OnCancelled);

		IssueCommand(InCommand, false);

		while (!InCommand.bExecuteProcessed)
		{
			Tick();
			Progress.Tick();
			FPlatformProcess::Sleep(0.01f);
		}

		// One more Tick to drain the completed command.
		Tick();

		if (InCommand.bCommandSuccessful)
		{
			Result = ECommandResult::Succeeded;
		}
	}

	check(!InCommand.bAutoDelete);

	if (CommandQueue.Contains(&InCommand))
	{
		CommandQueue.Remove(&InCommand);
	}

	delete &InCommand;

	return Result;
}

ECommandResult::Type FLoreSourceControlProvider::IssueCommand(FLoreSourceControlCommand& InCommand, const bool bSynchronous)
{
	if (!bSynchronous && GThreadPool != nullptr)
	{
		GThreadPool->AddQueuedWork(&InCommand);
		CommandQueue.Add(&InCommand);
		return ECommandResult::Succeeded;
	}

	InCommand.bCommandSuccessful = InCommand.DoWork();

	InCommand.Worker->UpdateStates();

	OutputCommandMessages(InCommand);

	return InCommand.ReturnResults();
}

ECommandResult::Type FLoreSourceControlProvider::ResolveSyncConflicts(const FSourceControlOperationRef& InOperation, const TArray<FString>& InFiles, const TArray<FString>& InConflicts)
{
	check(IsInGameThread());
	FMessageLog SourceControlLog("SourceControl");

	const FString BackupDir = FPaths::Combine(FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()), TEXT("Lore"), TEXT("Conflicts"), FDateTime::Now().ToString(TEXT("%Y.%m.%d-%H.%M.%S")));

	// One line per file, naming who holds its lock where that is known.
	FString List;
	for (const FString& Rel : InConflicts)
	{
		List += TEXT("    ") + Rel;
		const FString Abs = FLorePathUtils::ToAbsolutePath(Rel, WorkingCopyRoot);
		if (const TSharedRef<FLoreSourceControlState, ESPMode::ThreadSafe>* Cached = StateCache.Find(FLorePathUtils::MakeCacheKey(Abs)))
		{
			if ((*Cached)->LockState == ELoreLockState::LockedByOther)
			{
				List += FString::Printf(TEXT("   (locked by %s)"), *(*Cached)->LockUser);
			}
		}
		List += TEXT("\n");
	}

	if (FApp::IsUnattended() || !FSlateApplication::IsInitialized())
	{
		UE_LOG(LogLoreSourceControl, Warning, TEXT("Sync stopped on files changed on both sides; nobody to ask, so nothing was changed:\n%s"), *List);
		return ECommandResult::Failed;
	}

	const FText Message = FText::Format(LOCTEXT("SyncConflictMessage",
		"Sync stopped before downloading anything. These files were changed on this PC and are also changed in the newer version on the server:\n\n{0}\n"
		"Take theirs: download the new version. Your version is kept as a copy in\n    {1}\n\n"
		"Keep mine: download the new version, then put your version back on top of it, so you can submit it. "
		"Submitting replaces their change to these files, so agree on it with whoever made it. A copy is kept as well.\n\n"
		"Cancel: change nothing. You can also revert or submit these files yourself and sync again."),
		FText::FromString(List), FText::FromString(BackupDir));

	const TSharedRef<SCustomDialog> Dialog = SNew(SCustomDialog)
		.Title(LOCTEXT("SyncConflictTitle", "Lore: files changed on both sides"))
		.Content()
		[
			SNew(STextBlock)
			.Text(Message)
			.WrapTextAt(720.0f)
		]
		.Buttons({
			SCustomDialog::FButton(LOCTEXT("SyncConflictTakeTheirs", "Take theirs (keep mine as a copy)")),
			SCustomDialog::FButton(LOCTEXT("SyncConflictKeepMine", "Keep mine")),
			SCustomDialog::FButton(LOCTEXT("SyncConflictCancel", "Cancel")),
		});
	const int32 Choice = Dialog->ShowModal();
	const bool bKeepMine = Choice == 1;
	if (Choice != 0 && !bKeepMine)
	{
		SourceControlLog.Info(LOCTEXT("SyncConflictCancelled", "Sync cancelled; nothing was changed."));
		return ECommandResult::Failed;
	}

	FScopedSlowTask SlowTask(3.0f, LOCTEXT("SyncConflictResolving", "Resolving the files changed on both sides..."));
	SlowTask.MakeDialog();

	// 1) Copy every conflicting file aside first: from here on nothing may be lost.
	SlowTask.EnterProgressFrame(1.0f);
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	TArray<UObject*> LoadedPackages;
	for (const FString& Rel : InConflicts)
	{
		const FString Abs = FLorePathUtils::ToAbsolutePath(Rel, WorkingCopyRoot);
		const FString Backup = FPaths::Combine(BackupDir, Rel);
		bool bCopied = true;
		if (PlatformFile.DirectoryExists(*Abs))
		{
			bCopied = PlatformFile.CreateDirectoryTree(*Backup) && PlatformFile.CopyDirectoryTree(*Backup, *Abs, /*bOverwriteAllExisting=*/true);
		}
		else if (PlatformFile.FileExists(*Abs))
		{
			bCopied = PlatformFile.CreateDirectoryTree(*FPaths::GetPath(Backup)) && PlatformFile.CopyFile(*Backup, *Abs);
		}
		if (!bCopied)
		{
			SourceControlLog.Error(FText::Format(LOCTEXT("SyncConflictBackupFailed", "Could not copy {0} to {1}, so nothing was changed. Sync stopped."), FText::FromString(Abs), FText::FromString(Backup)));
			SourceControlLog.Notify(LOCTEXT("SyncConflictFailed", "Sync stopped: files changed on both sides could not be resolved."));
			return ECommandResult::Failed;
		}

		FString PackageName;
		if (FPackageName::TryConvertFilenameToLongPackageName(Abs, PackageName))
		{
			if (UPackage* Package = FindPackage(nullptr, *PackageName))
			{
				LoadedPackages.Add(Package);
			}
		}
	}
	UE_LOG(LogLoreSourceControl, Log, TEXT("Sync conflict: copied %d file(s) to %s (%s)"), InConflicts.Num(), *BackupDir, bKeepMine ? TEXT("keep mine") : TEXT("take theirs"));

	// 2) Let go of files the editor still has open, then drop the local changes so the sync can write.
	// The editor's Sync Content reloads these packages once the sync returns.
	SlowTask.EnterProgressFrame(1.0f);
	for (UObject* Package : LoadedPackages)
	{
		ResetLoaders(Package);
	}
	{
		const LoreSourceControlUtils::FLoreCommandContext Context = LoreSourceControlUtils::GetCommandContextFromModule();
		TArray<FString> Lines;
		TArray<FString> Errors;
		if (!LoreSourceControlUtils::RunCommandBatched(TEXT("reset"), TArray<FString>(), InConflicts, Context, /*bUseTargetsFile=*/true, Lines, Errors))
		{
			SourceControlLog.Error(FText::Format(LOCTEXT("SyncConflictResetFailed", "Could not set the files back before syncing: {0}. Your versions are in {1}."),
				FText::FromString(FString::Join(Errors, TEXT(" "))), FText::FromString(BackupDir)));
			SourceControlLog.Notify(LOCTEXT("SyncConflictFailed", "Sync stopped: files changed on both sides could not be resolved."));
			return ECommandResult::Failed;
		}
	}

	// 3) Sync again (no second dialog if something else collides now).
	SlowTask.EnterProgressFrame(1.0f);
	ECommandResult::Type Result = ECommandResult::Failed;
	{
		TGuardValue<bool> Guard(bResolvingSyncConflicts, true);
		Result = Execute(InOperation, nullptr, InFiles, EConcurrency::Synchronous);
	}
	if (Result != ECommandResult::Succeeded)
	{
		SourceControlLog.Error(FText::Format(LOCTEXT("SyncConflictSyncFailed", "Sync still did not complete. Your versions of the files are in {0}."), FText::FromString(BackupDir)));
		return Result;
	}

	TArray<FString> AbsoluteConflicts;
	for (const FString& Rel : InConflicts)
	{
		AbsoluteConflicts.Add(FLorePathUtils::ToAbsolutePath(Rel, WorkingCopyRoot));
	}

	FText Done;
	if (bKeepMine)
	{
		TArray<FString> RestoredFiles;
		for (const FString& Rel : InConflicts)
		{
			const FString Abs = FLorePathUtils::ToAbsolutePath(Rel, WorkingCopyRoot);
			const FString Backup = FPaths::Combine(BackupDir, Rel);
			if (PlatformFile.DirectoryExists(*Backup))
			{
				PlatformFile.CreateDirectoryTree(*Abs);
				PlatformFile.CopyDirectoryTree(*Abs, *Backup, /*bOverwriteAllExisting=*/true);
			}
			else if (PlatformFile.FileExists(*Backup))
			{
				PlatformFile.CreateDirectoryTree(*FPaths::GetPath(Abs));
				if (PlatformFile.CopyFile(*Abs, *Backup))
				{
					RestoredFiles.Add(Abs);
				}
				else
				{
					SourceControlLog.Error(FText::Format(LOCTEXT("SyncConflictRestoreFailed", "Could not put your version of {0} back. It is in {1}."), FText::FromString(Rel), FText::FromString(Backup)));
				}
			}
		}
		// Lock them, so nobody else edits them before you submit (reports the holder if someone already does).
		if (!RestoredFiles.IsEmpty())
		{
			Execute(ISourceControlOperation::Create<FCheckOut>(), nullptr, RestoredFiles, EConcurrency::Asynchronous);
		}
		Done = FText::Format(LOCTEXT("SyncConflictKeptMine", "Synced. Your version of {0} file(s) is back on top of the new one; submit it to share it. A copy is in {1}."),
			FText::AsNumber(InConflicts.Num()), FText::FromString(BackupDir));
	}
	else
	{
		Done = FText::Format(LOCTEXT("SyncConflictTookTheirs", "Synced. {0} file(s) now have the new version; your versions are kept in {1}."),
			FText::AsNumber(InConflicts.Num()), FText::FromString(BackupDir));
	}

	Execute(ISourceControlOperation::Create<FUpdateStatus>(), nullptr, AbsoluteConflicts, EConcurrency::Asynchronous);
	SourceControlLog.Info(Done);
	UE_LOG(LogLoreSourceControl, Log, TEXT("%s"), *Done.ToString());
	FNotificationInfo Info(Done);
	Info.ExpireDuration = 10.0f;
	FSlateNotificationManager::Get().AddNotification(Info);
	return Result;
}

#undef LOCTEXT_NAMESPACE
