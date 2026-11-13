#include "LoreSourceControlOperations.h"
#include "LoreSourceControlCommand.h"
#include "LoreSourceControlModule.h"
#include "LoreSourceControlProvider.h"
#include "LoreSourceControlUtils.h"
#include "LoreSourceControlLog.h"
#include "LorePathUtils.h"

#include "SourceControlOperations.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"

#define LOCTEXT_NAMESPACE "LoreSourceControl.Operations"

namespace
{
	/**
	 * Build the workspace-relative paths Lore expects for a set of absolute files.
	 * Files outside the working copy (engine content, instanced temp packages,
	 * etc.) are dropped: forwarding them would make `lore` fail with "invalid path".
	 */
	TArray<FString> ToRelativePaths(const TArray<FString>& InAbsolute, const FString& InRoot)
	{
		TArray<FString> Out;
		Out.Reserve(InAbsolute.Num());
		for (const FString& File : InAbsolute)
		{
			if (!FLorePathUtils::IsUnderWorkspace(File, InRoot))
			{
				UE_LOG(LogLoreSourceControl, Verbose, TEXT("Skipping out-of-workspace path: %s"), *File);
				continue;
			}
			FString Rel = FLorePathUtils::ToWorkspaceRelativePath(File, InRoot);
			if (!Rel.IsEmpty())
			{
				Out.Add(Rel);
			}
		}
		return Out;
	}

	/**
	 * Refresh per-file states by running `lore status [--scan]` plus
	 * `lore lock query`, then (optionally) `lore file history` per file.
	 * Files that exist but aren't reported by status are recorded as clean.
	 */
	bool RefreshFileStates(
		FLoreSourceControlCommand& Command,
		bool bScan,
		bool bWithHistory,
		TArray<FLoreSourceControlState>& OutStates,
		TMap<FString, TArray<TSharedRef<FLoreSourceControlRevision, ESPMode::ThreadSafe>>>* OutHistory)
	{
		const LoreSourceControlUtils::FLoreCommandContext Context = LoreSourceControlUtils::MakeContext(Command);

		// 1) Status.
		TArray<FString> StatusParams;
		if (bScan)
		{
			StatusParams.Add(TEXT("--scan"));
		}

		const TArray<FString> RelativeFiles = ToRelativePaths(Command.Files, Command.WorkingCopyRoot);

		TArray<FString> StatusResults;
		const bool bStatusOk = LoreSourceControlUtils::RunCommandBatched(
			TEXT("status"), StatusParams, RelativeFiles, Context, /*bUseTargetsFile=*/true,
			StatusResults, Command.ErrorMessages);
		if (!bStatusOk)
		{
			// The editor only says that checking failed, so the reason has to be findable in the log.
			UE_LOG(LogLoreSourceControl, Warning, TEXT("`lore status` failed: %s"), *FString::Join(Command.ErrorMessages, TEXT(" | ")));
		}

		FLoreRepoStatus RepoStatus;
		LoreSourceControlUtils::ParseStatusResults(StatusResults, Command.WorkingCopyRoot, RepoStatus, OutStates);

		// 2) Fill clean states for requested files not reported as changed. This
		// MUST happen before locks are applied: a file can be locked while
		// otherwise unmodified (status reports nothing), so it would not yet have
		// a state for the lock to attach to. Creating the clean state first lets
		// step 3 stamp the correct lock state onto it.
		{
			TSet<FString> Reported;
			for (const FLoreSourceControlState& State : OutStates)
			{
				Reported.Add(FLorePathUtils::MakeCacheKey(State.LocalFilename));
			}

			for (const FString& File : Command.Files)
			{
				if (!FLorePathUtils::IsUnderWorkspace(File, Command.WorkingCopyRoot))
				{
					continue;
				}
				const FString Key = FLorePathUtils::MakeCacheKey(File);
				if (!Reported.Contains(Key) && FPaths::FileExists(File))
				{
					FLoreSourceControlState Clean(File);
					Clean.DepotFilename = FLorePathUtils::ToWorkspaceRelativePath(File, Command.WorkingCopyRoot);
					Clean.WorkingState = ELoreWorkingState::Clean;
					Clean.LockState = ELoreLockState::NotLocked;
					Clean.LocalRevision = RepoStatus.LocalRevision;
					Clean.LocalRevisionNumber = RepoStatus.LocalRevisionNumber;
					Clean.bNewerVersionOnServer = RepoStatus.bRemoteAhead;
					Clean.TimeStamp = FDateTime::Now();
					OutStates.Add(MoveTemp(Clean));
				}
			}
		}

		// 3) Locks. Applied after clean states exist so locks on unmodified files
		// are reflected (LockedByMe / LockedByOther) rather than silently dropped.
		{
			TArray<FString> LockResults;
			int32 LockReturn = -1;
			const bool bLockQueryOk = LoreSourceControlUtils::RunCommand(TEXT("lock query"), TArray<FString>(), TArray<FString>(), Context, LockResults, Command.ErrorMessages, LockReturn);

			TArray<FLoreLockInfo> Locks;
			LoreSourceControlUtils::ParseLockQueryResults(LockResults, Locks);
			// Locks carry the authenticated user's name; fall back to it when no identity is configured.
			const FString LockUser = Command.UserName.IsEmpty() ? LoreSourceControlUtils::DetectAuthenticatedUser(Context) : Command.UserName;

			// The server keeps locks in memory, so a restart wipes them. Remember our own locks and
			// take back the ones that vanished while their file is still changed locally.
			if (bLockQueryOk && !LockUser.IsEmpty())
			{
				TSet<FString> Mine;
				for (const FLoreLockInfo& Lock : Locks)
				{
					if (Lock.Owner.Equals(LockUser, ESearchCase::IgnoreCase))
					{
						Mine.Add(Lock.Path);
					}
				}

				TSet<FString> Remembered;
				LoreSourceControlUtils::LoadRememberedLocks(Remembered);
				TArray<FString> Lost;
				for (const FString& Path : Remembered)
				{
					if (!Mine.Contains(Path))
					{
						Lost.Add(Path);
					}
				}

				if (!Lost.IsEmpty())
				{
					TSet<FString> Changed;
					TArray<FString> StatusLines;
					TArray<FString> StatusErrors;
					int32 StatusReturn = -1;
					if (LoreSourceControlUtils::RunCommand(TEXT("status"), TArray<FString>(), TArray<FString>(), Context, StatusLines, StatusErrors, StatusReturn))
					{
						for (const FString& Raw : StatusLines)
						{
							FString Line = LoreSourceControlUtils::StripAnsi(Raw).TrimStartAndEnd();
							if (Line.Len() > 2 && Line[1] == TEXT(' ') && FString(TEXT("MADRT")).Contains(Line.Left(1), ESearchCase::CaseSensitive))
							{
								Line = Line.Mid(2).TrimStartAndEnd();
								int32 Open = INDEX_NONE;
								if (Line.EndsWith(TEXT(")")) && Line.FindLastChar(TEXT('('), Open))
								{
									Line = Line.Left(Open).TrimEnd();
								}
								Changed.Add(Line);
							}
						}
					}

					for (const FString& Path : Lost)
					{
						if (!Changed.Contains(Path))
						{
							continue; // no longer being edited: nothing to protect
						}
						TArray<FString> AcquireResults;
						TArray<FString> AcquireErrors;
						int32 AcquireReturn = -1;
						if (LoreSourceControlUtils::RunCommand(TEXT("lock acquire"), TArray<FString>(), { Path }, Context, AcquireResults, AcquireErrors, AcquireReturn))
						{
							FLoreLockInfo Restored;
							Restored.Path = Path;
							Restored.Owner = LockUser;
							Locks.Add(Restored);
							Mine.Add(Path);
							UE_LOG(LogLoreSourceControl, Log, TEXT("Restored lock that vanished from the server: %s"), *Path);
						}
						else
						{
							UE_LOG(LogLoreSourceControl, Warning, TEXT("Could not restore lock (someone else may hold it now): %s"), *Path);
						}
					}
				}
				LoreSourceControlUtils::SaveRememberedLocks(Mine);
			}

			LoreSourceControlUtils::ApplyLocksToStates(Locks, LockUser, Command.WorkingCopyRoot, OutStates);
		}

		// Diagnostic: report what we computed for each requested file so we can see
		// whether modified/lock state is being detected and cached as expected.
		// Verbose by default; enable with `log LogLoreSourceControl Verbose`.
		UE_LOG(LogLoreSourceControl, Verbose, TEXT("RefreshFileStates (scan=%d): %d requested, %d states computed"),
			bScan ? 1 : 0, Command.Files.Num(), OutStates.Num());
		for (const FLoreSourceControlState& State : OutStates)
		{
			UE_LOG(LogLoreSourceControl, Verbose, TEXT("  state: '%s' working=%d lock=%d"),
				*State.DepotFilename, (int32)State.WorkingState, (int32)State.LockState);
		}

		// 4) History.
		if (bWithHistory && OutHistory != nullptr)
		{
			for (const FString& File : Command.Files)
			{
				if (!FLorePathUtils::IsUnderWorkspace(File, Command.WorkingCopyRoot))
				{
					continue;
				}
				const FString Rel = FLorePathUtils::ToWorkspaceRelativePath(File, Command.WorkingCopyRoot);
				if (Rel.IsEmpty())
				{
					continue;
				}

				TArray<FString> HistoryResults;
				int32 HistoryReturn = -1;
				TArray<FString> Params;
				Params.Add(FString::Printf(TEXT("\"%s\""), *Rel));
				LoreSourceControlUtils::RunCommand(TEXT("file history"), Params, TArray<FString>(), Context, HistoryResults, Command.ErrorMessages, HistoryReturn);

				TArray<TSharedRef<FLoreSourceControlRevision, ESPMode::ThreadSafe>> History;
				LoreSourceControlUtils::ParseHistoryResults(HistoryResults, File, Rel, History);
				if (History.Num() > 0)
				{
					OutHistory->Add(FLorePathUtils::MakeCacheKey(File), MoveTemp(History));
				}
			}
		}

		return bStatusOk;
	}

	/** Merge worker history into the cached states (game thread). */
	void MergeHistoryIntoCache(const TMap<FString, TArray<TSharedRef<FLoreSourceControlRevision, ESPMode::ThreadSafe>>>& InHistory)
	{
		if (InHistory.Num() == 0)
		{
			return;
		}
		FLoreSourceControlModule* Module = FLoreSourceControlModule::GetPtr();
		if (Module == nullptr)
		{
			return;
		}
		FLoreSourceControlProvider& Provider = Module->GetProvider();
		for (const auto& Pair : InHistory)
		{
			TSharedRef<FLoreSourceControlState, ESPMode::ThreadSafe> State = Provider.GetStateInternal(Pair.Key);
			State->History = Pair.Value;
		}
	}
}

// -------------------------------------------------------------------------- Connect

FName FLoreConnectWorker::GetName() const
{
	return "Connect";
}

bool FLoreConnectWorker::Execute(FLoreSourceControlCommand& InCommand)
{
	const LoreSourceControlUtils::FLoreCommandContext Context = LoreSourceControlUtils::MakeContext(InCommand);

	if (Context.BinaryPath.IsEmpty())
	{
		InCommand.ErrorMessages.Add(TEXT("Could not locate the `lore` executable. Set its path in the Lore source control settings."));
		return false;
	}

	TArray<FString> Results;
	int32 ReturnCode = -1;
	const bool bOk = LoreSourceControlUtils::RunCommand(TEXT("status"), TArray<FString>(), TArray<FString>(), Context, Results, InCommand.ErrorMessages, ReturnCode);

	if (!bOk)
	{
		InCommand.ErrorMessages.Add(TEXT("`lore status` failed: the working copy may not be a Lore repository, or the server is unreachable."));
	}
	else
	{
		InCommand.InfoMessages.Add(TEXT("Connected to Lore repository."));
	}

	InCommand.ReturnCode = ReturnCode;
	return bOk;
}

bool FLoreConnectWorker::UpdateStates() const
{
	return false;
}

// --------------------------------------------------------------------- UpdateStatus

FName FLoreUpdateStatusWorker::GetName() const
{
	return "UpdateStatus";
}

bool FLoreUpdateStatusWorker::Execute(FLoreSourceControlCommand& InCommand)
{
	bool bWithHistory = false;
	if (InCommand.Operation->GetName() == "UpdateStatus")
	{
		const TSharedRef<FUpdateStatus, ESPMode::ThreadSafe> Operation = StaticCastSharedRef<FUpdateStatus>(InCommand.Operation);
		bWithHistory = Operation->ShouldUpdateHistory();
	}

	const bool bOk = RefreshFileStates(InCommand, /*bScan=*/true, bWithHistory, OutStates, &OutHistory);

	// Submit Content asks for a status pass over the whole project and lists whatever is cached as submittable
	// afterwards. Log what that pass found, so an empty submit list can be traced to its cause.
	int32 Pending = 0;
	for (const FLoreSourceControlState& State : OutStates)
	{
		Pending += State.CanCheckIn() ? 1 : 0;
	}
	UE_LOG(LogLoreSourceControl, Log, TEXT("UpdateStatus: %d path(s) requested, %d state(s) returned, %d submittable (ok=%d)"),
		InCommand.Files.Num(), OutStates.Num(), Pending, bOk ? 1 : 0);
	return bOk;
}

bool FLoreUpdateStatusWorker::UpdateStates() const
{
	const bool bUpdated = LoreSourceControlUtils::UpdateCachedStates(OutStates);
	MergeHistoryIntoCache(OutHistory);
	return bUpdated || OutHistory.Num() > 0;
}

// ------------------------------------------------------------------------- CheckOut

FName FLoreCheckOutWorker::GetName() const
{
	return "CheckOut";
}

bool FLoreCheckOutWorker::Execute(FLoreSourceControlCommand& InCommand)
{
	const LoreSourceControlUtils::FLoreCommandContext Context = LoreSourceControlUtils::MakeContext(InCommand);
	const TArray<FString> RelativeFiles = ToRelativePaths(InCommand.Files, InCommand.WorkingCopyRoot);
	UE_LOG(LogLoreSourceControl, Log, TEXT("CheckOut: %d file(s) received, %d inside the workspace: %s"),
		InCommand.Files.Num(), RelativeFiles.Num(), *FString::Join(RelativeFiles, TEXT(", ")));

	TArray<FString> Results;
	const bool bOk = LoreSourceControlUtils::RunCommandBatched(
		TEXT("lock acquire"), TArray<FString>(), RelativeFiles, Context, /*bUseTargetsFile=*/false,
		Results, InCommand.ErrorMessages);
	UE_LOG(LogLoreSourceControl, Log, TEXT("CheckOut: lock acquire %s"), bOk ? TEXT("succeeded") : TEXT("FAILED"));

	RefreshFileStates(InCommand, /*bScan=*/false, /*bWithHistory=*/false, OutStates, nullptr);

	if (!bOk)
	{
		// Say who holds each lock, so the editor's "could not be checked out" is not a dead end.
		bool bNamedOwner = false;
		for (const FLoreSourceControlState& State : OutStates)
		{
			if (State.LockState == ELoreLockState::LockedByOther)
			{
				InCommand.ErrorMessages.Add(FString::Printf(TEXT("Cannot check out '%s': %s holds its lock in Lore. Ask %s to submit and release it, then sync before you change it."),
					*State.DepotFilename, *State.LockUser, *State.LockUser));
				bNamedOwner = true;
			}
		}
		if (!bNamedOwner)
		{
			InCommand.ErrorMessages.Add(TEXT("Failed to acquire one or more locks (a file may be locked by another user)."));
		}
	}
	return bOk;
}

bool FLoreCheckOutWorker::UpdateStates() const
{
	return LoreSourceControlUtils::UpdateCachedStates(OutStates);
}

// ----------------------------------------------------------------------- MarkForAdd

FName FLoreMarkForAddWorker::GetName() const
{
	return "MarkForAdd";
}

bool FLoreMarkForAddWorker::Execute(FLoreSourceControlCommand& InCommand)
{
	const LoreSourceControlUtils::FLoreCommandContext Context = LoreSourceControlUtils::MakeContext(InCommand);
	const TArray<FString> RelativeFiles = ToRelativePaths(InCommand.Files, InCommand.WorkingCopyRoot);

	TArray<FString> Results;
	const bool bOk = LoreSourceControlUtils::RunCommandBatched(
		TEXT("stage"), TArray<FString>(), RelativeFiles, Context, /*bUseTargetsFile=*/true,
		Results, InCommand.ErrorMessages);

	RefreshFileStates(InCommand, /*bScan=*/true, /*bWithHistory=*/false, OutStates, nullptr);
	return bOk;
}

bool FLoreMarkForAddWorker::UpdateStates() const
{
	return LoreSourceControlUtils::UpdateCachedStates(OutStates);
}

// --------------------------------------------------------------------------- Delete

FName FLoreDeleteWorker::GetName() const
{
	return "Delete";
}

bool FLoreDeleteWorker::Execute(FLoreSourceControlCommand& InCommand)
{
	const LoreSourceControlUtils::FLoreCommandContext Context = LoreSourceControlUtils::MakeContext(InCommand);

	// Remove the files on disk, then stage so Lore records the deletion.
	for (const FString& File : InCommand.Files)
	{
		if (FPaths::FileExists(File))
		{
			IFileManager::Get().Delete(*File, /*RequireExists=*/false, /*EvenReadOnly=*/true, /*Quiet=*/true);
		}
	}

	const TArray<FString> RelativeFiles = ToRelativePaths(InCommand.Files, InCommand.WorkingCopyRoot);
	TArray<FString> Results;
	const bool bOk = LoreSourceControlUtils::RunCommandBatched(
		TEXT("stage"), TArray<FString>(), RelativeFiles, Context, /*bUseTargetsFile=*/true,
		Results, InCommand.ErrorMessages);

	RefreshFileStates(InCommand, /*bScan=*/true, /*bWithHistory=*/false, OutStates, nullptr);
	return bOk;
}

bool FLoreDeleteWorker::UpdateStates() const
{
	return LoreSourceControlUtils::UpdateCachedStates(OutStates);
}

// --------------------------------------------------------------------------- Revert

FName FLoreRevertWorker::GetName() const
{
	return "Revert";
}

bool FLoreRevertWorker::Execute(FLoreSourceControlCommand& InCommand)
{
	const LoreSourceControlUtils::FLoreCommandContext Context = LoreSourceControlUtils::MakeContext(InCommand);
	const TArray<FString> RelativeFiles = ToRelativePaths(InCommand.Files, InCommand.WorkingCopyRoot);

	// Discard local changes (best effort). Reverting an unmodified/unstaged file
	// is a legitimate no-op in the editor (e.g. "Revert Unchanged"), but `lore
	// reset` errors when there is nothing staged for a path, so we do not treat a
	// reset failure as a hard error: the post-op status refresh reflects reality.
	TArray<FString> ResetResults;
	LoreSourceControlUtils::RunCommandBatched(
		TEXT("reset"), TArray<FString>(), RelativeFiles, Context, /*bUseTargetsFile=*/true,
		ResetResults, InCommand.ErrorMessages);

	// Release any locks we hold (best effort; not having a lock is not an error).
	// One file per call: Lore stops at the first file that is not locked.
	for (const FString& RelativeFile : RelativeFiles)
	{
		TArray<FString> ReleaseResults;
		TArray<FString> ReleaseErrors;
		int32 ReleaseCode = -1;
		LoreSourceControlUtils::RunCommand(TEXT("lock release"), TArray<FString>(), { RelativeFile }, Context, ReleaseResults, ReleaseErrors, ReleaseCode);
	}

	RefreshFileStates(InCommand, /*bScan=*/true, /*bWithHistory=*/false, OutStates, nullptr);
	return true;
}

bool FLoreRevertWorker::UpdateStates() const
{
	return LoreSourceControlUtils::UpdateCachedStates(OutStates);
}

// ----------------------------------------------------------------------------- Sync

FName FLoreSyncWorker::GetName() const
{
	return "Sync";
}

bool FLoreSyncWorker::Execute(FLoreSourceControlCommand& InCommand)
{
	const LoreSourceControlUtils::FLoreCommandContext Context = LoreSourceControlUtils::MakeContext(InCommand);

	TArray<FString> Params;
	if (InCommand.Operation->GetName() == "Sync")
	{
		const TSharedRef<FSync, ESPMode::ThreadSafe> Operation = StaticCastSharedRef<FSync>(InCommand.Operation);
		if (!Operation->GetRevision().IsEmpty())
		{
			Params.Add(FString::Printf(TEXT("\"%s\""), *Operation->GetRevision()));
		}
	}

	TArray<FString> Results;
	TArray<FString> Errors;
	int32 ReturnCode = -1;
	bool bOk = LoreSourceControlUtils::RunCommand(TEXT("sync"), Params, TArray<FString>(), Context, Results, Errors, ReturnCode);

	// Lore does not sync while files are staged for the next commit (for example after a submit that
	// stopped halfway). Set them back to unstaged for the sync and stage them again afterwards, as
	// Lore+ does; nothing in the files changes.
	auto Mentions = [&Results, &Errors](const TCHAR* InText)
	{
		for (const TArray<FString>* Lines : { &Results, &Errors })
		{
			for (const FString& Line : *Lines)
			{
				if (Line.Contains(InText, ESearchCase::IgnoreCase))
				{
					return true;
				}
			}
		}
		return false;
	};
	if (!bOk && Mentions(TEXT("staged state")))
	{
		TArray<FString> StatusLines;
		TArray<FString> StatusErrors;
		int32 StatusReturn = -1;
		LoreSourceControlUtils::RunCommand(TEXT("status"), TArray<FString>(), TArray<FString>(), Context, StatusLines, StatusErrors, StatusReturn);
		const TArray<FString> Staged = LoreSourceControlUtils::ParseStagedFiles(StatusLines);

		TArray<FString> UnstageLines;
		int32 UnstageReturn = -1;
		if (LoreSourceControlUtils::RunCommand(TEXT("unstage"), TArray<FString>(), { TEXT(".") }, Context, UnstageLines, Errors, UnstageReturn))
		{
			UE_LOG(LogLoreSourceControl, Log, TEXT("Sync: unstaged %d staged file(s) to sync, staging them again afterwards"), Staged.Num());
			Results.Reset();
			Errors.Reset();
			bOk = LoreSourceControlUtils::RunCommand(TEXT("sync"), Params, TArray<FString>(), Context, Results, Errors, ReturnCode);

			TArray<FString> StageLines;
			TArray<FString> StageErrors;
			if (!Staged.IsEmpty() && !LoreSourceControlUtils::RunCommandBatched(TEXT("stage"), TArray<FString>(), Staged, Context, /*bUseTargetsFile=*/true, StageLines, StageErrors))
			{
				Errors.Add(FString::Printf(TEXT("Could not stage these files again: %s"), *FString::Join(Staged, TEXT(", "))));
			}
		}
	}

	if (!bOk)
	{
		TArray<FString> AllLines = Results;
		AllLines.Append(Errors);
		Conflicts = LoreSourceControlUtils::ParseSyncConflicts(AllLines);
		if (!Conflicts.IsEmpty())
		{
			// Lead with what happened and what to do; Lore's own lines follow for detail.
			InCommand.ErrorMessages.Add(FString::Printf(
				TEXT("Sync stopped, nothing was downloaded: %d file(s) changed on this PC are also changed in the newer version on the server: %s. ")
				TEXT("Choose \"Take theirs\" (your version is kept as a copy in Saved/Lore/Conflicts) or \"Keep mine\" in the dialog, or revert or submit these files and sync again."),
				Conflicts.Num(), *FString::Join(Conflicts, TEXT(", "))));
		}
		UE_LOG(LogLoreSourceControl, Warning, TEXT("Sync failed (exit %d): %s"), ReturnCode, *FString::Join(Errors.IsEmpty() ? Results : Errors, TEXT(" | ")));
	}
	else
	{
		InCommand.InfoMessages.Add(TEXT("Synced with Lore."));
	}
	InCommand.ErrorMessages.Append(Errors);
	if (!bOk && Errors.IsEmpty())
	{
		// Lore writes some refusals to stdout; without them the editor would only say "failed".
		for (const FString& Line : Results)
		{
			const FString Clean = Line.TrimStartAndEnd();
			if (Clean.StartsWith(TEXT("[Error]")))
			{
				InCommand.ErrorMessages.Add(Clean);
			}
		}
	}
	InCommand.ReturnCode = ReturnCode;

	RefreshFileStates(InCommand, /*bScan=*/true, /*bWithHistory=*/false, OutStates, nullptr);
	return bOk;
}

bool FLoreSyncWorker::UpdateStates() const
{
	if (FLoreSourceControlModule* Module = FLoreSourceControlModule::GetPtr())
	{
		Module->GetProvider().SetLastSyncConflicts(Conflicts);
	}
	return LoreSourceControlUtils::UpdateCachedStates(OutStates);
}

// ---------------------------------------------------------------------- SyncPreview

FName FLoreSyncPreviewWorker::GetName() const
{
	return "SyncPreview";
}

bool FLoreSyncPreviewWorker::Execute(FLoreSourceControlCommand& InCommand)
{
	const LoreSourceControlUtils::FLoreCommandContext Context = LoreSourceControlUtils::MakeContext(InCommand);
	const TSharedRef<FSyncPreview, ESPMode::ThreadSafe> Operation = StaticCastSharedRef<FSyncPreview>(InCommand.Operation);

	// Where we are and where the sync would go.
	TArray<FString> StatusLines;
	int32 StatusReturn = -1;
	if (!LoreSourceControlUtils::RunCommand(TEXT("status"), TArray<FString>(), TArray<FString>(), Context, StatusLines, InCommand.ErrorMessages, StatusReturn))
	{
		return false;
	}
	FLoreRepoStatus RepoStatus;
	TArray<FLoreSourceControlState> IgnoredStates;
	LoreSourceControlUtils::ParseStatusResults(StatusLines, InCommand.WorkingCopyRoot, RepoStatus, IgnoredStates);

	const FString Target = Operation->GetRevision().IsEmpty() ? RepoStatus.RemoteRevision : Operation->GetRevision();
	TArray<FString> Affected;
	if (Target.IsEmpty() || RepoStatus.LocalRevision.IsEmpty() || Target == RepoStatus.LocalRevision)
	{
		Operation->SetAffectedFiles(MoveTemp(Affected)); // nothing incoming
		return true;
	}

	TArray<FString> DiffLines;
	int32 DiffReturn = -1;
	if (!LoreSourceControlUtils::RunCommand(TEXT("revision diff"), { RepoStatus.LocalRevision, TEXT("--target"), FString::Printf(TEXT("\"%s\""), *Target) },
		TArray<FString>(), Context, DiffLines, InCommand.ErrorMessages, DiffReturn))
	{
		return false;
	}
	for (const FString& Line : DiffLines)
	{
		// "<code> <path>"; directories come without a trailing slash and are simply not packages.
		const FString Clean = LoreSourceControlUtils::StripAnsi(Line).TrimStartAndEnd();
		if (Clean.Len() > 2 && Clean[1] == TEXT(' ') && FString(TEXT("MADVC")).Contains(Clean.Left(1), ESearchCase::CaseSensitive))
		{
			FString Path = Clean.Mid(2).TrimStartAndEnd();
			const int32 Arrow = Path.Find(TEXT(" -> "), ESearchCase::CaseSensitive);
			if (Arrow != INDEX_NONE)
			{
				Affected.Add(FLorePathUtils::ToAbsolutePath(Path.Left(Arrow).TrimStartAndEnd(), InCommand.WorkingCopyRoot));
				Path = Path.Mid(Arrow + 4).TrimStartAndEnd();
			}
			Affected.Add(FLorePathUtils::ToAbsolutePath(Path, InCommand.WorkingCopyRoot));
		}
	}
	UE_LOG(LogLoreSourceControl, Log, TEXT("SyncPreview: %d path(s) change between revision %d and %s"), Affected.Num(), RepoStatus.LocalRevisionNumber, *Target);
	Operation->SetAffectedFiles(MoveTemp(Affected));
	return true;
}

bool FLoreSyncPreviewWorker::UpdateStates() const
{
	return false;
}

// -------------------------------------------------------------------------- CheckIn

FName FLoreCheckInWorker::GetName() const
{
	return "CheckIn";
}

bool FLoreCheckInWorker::Execute(FLoreSourceControlCommand& InCommand)
{
	const LoreSourceControlUtils::FLoreCommandContext Context = LoreSourceControlUtils::MakeContext(InCommand);

	// Fail-closed pre-flight: refresh status/locks and verify every file is
	// submittable (not foreign-locked, not conflicted, at head) before mutating.
	{
		TArray<FLoreSourceControlState> PreflightStates;
		RefreshFileStates(InCommand, /*bScan=*/true, /*bWithHistory=*/false, PreflightStates, nullptr);

		TMap<FString, const FLoreSourceControlState*> ByKey;
		for (const FLoreSourceControlState& State : PreflightStates)
		{
			ByKey.Add(FLorePathUtils::MakeCacheKey(State.LocalFilename), &State);
		}

		for (const FString& File : InCommand.Files)
		{
			const FLoreSourceControlState* const* Found = ByKey.Find(FLorePathUtils::MakeCacheKey(File));
			if (Found == nullptr)
			{
				continue; // Unchanged files are skipped by stage/commit.
			}
			const FLoreSourceControlState* State = *Found;

			if (State->LockState == ELoreLockState::LockedByOther)
			{
				InCommand.ErrorMessages.Add(FString::Printf(TEXT("Cannot submit '%s': locked by %s."), *State->DepotFilename, *State->LockUser));
				return false;
			}
			if (State->WorkingState == ELoreWorkingState::Conflicted)
			{
				InCommand.ErrorMessages.Add(FString::Printf(TEXT("Cannot submit '%s': the file is in conflict. Resolve it first."), *State->DepotFilename));
				return false;
			}
			if (State->bNewerVersionOnServer)
			{
				InCommand.ErrorMessages.Add(FString::Printf(TEXT("Cannot submit '%s': the repository has newer changes. Sync first."), *State->DepotFilename));
				return false;
			}
		}
	}

	const TArray<FString> RelativeFiles = ToRelativePaths(InCommand.Files, InCommand.WorkingCopyRoot);
	UE_LOG(LogLoreSourceControl, Log, TEXT("CheckIn: %d file(s) received, %d inside the workspace: %s"),
		InCommand.Files.Num(), RelativeFiles.Num(), *FString::Join(RelativeFiles, TEXT(", ")));

	// Stage.
	{
		TArray<FString> Results;
		if (!LoreSourceControlUtils::RunCommandBatched(TEXT("stage"), TArray<FString>(), RelativeFiles, Context, /*bUseTargetsFile=*/true, Results, InCommand.ErrorMessages))
		{
			InCommand.ErrorMessages.Add(TEXT("Failed to stage changes for commit."));
			return false;
		}
	}

	// Commit.
	FString Description;
	if (InCommand.Operation->GetName() == "CheckIn")
	{
		const TSharedRef<FCheckIn, ESPMode::ThreadSafe> Operation = StaticCastSharedRef<FCheckIn>(InCommand.Operation);
		Description = Operation->GetDescription().ToString();
	}
	if (Description.IsEmpty())
	{
		Description = TEXT("Submitted from Unreal Editor");
	}
	// Escape embedded quotes so the message survives the command line.
	Description.ReplaceInline(TEXT("\""), TEXT("\\\""));

	// `lore commit` writes the new revision but does not move the branch head,
	// and leaves the staged state behind. Read the new revision's signature,
	// clear the staged state, move the head onto that revision, then push.
	FString Signature;
	{
		TArray<FString> Results;
		int32 ReturnCode = -1;
		TArray<FString> Params;
		Params.Add(FString::Printf(TEXT("\"%s\""), *Description));
		if (!LoreSourceControlUtils::RunCommand(TEXT("commit"), Params, TArray<FString>(), Context, Results, InCommand.ErrorMessages, ReturnCode))
		{
			InCommand.ErrorMessages.Add(TEXT("`lore commit` failed."));
			return false;
		}

		for (const FString& Line : Results)
		{
			const FString Clean = LoreSourceControlUtils::StripAnsi(Line).TrimStartAndEnd();
			int32 Colon = INDEX_NONE;
			if (Clean.StartsWith(TEXT("Signature")) && Clean.FindChar(TEXT(':'), Colon))
			{
				Signature = Clean.Mid(Colon + 1).TrimStartAndEnd();
			}
		}

		if (Signature.IsEmpty())
		{
			InCommand.ErrorMessages.Add(TEXT("Commit completed but no revision signature was reported."));
			return false;
		}
	}

	// Clear the staged state so the branch head can move.
	{
		TArray<FString> Results;
		LoreSourceControlUtils::RunCommandBatched(TEXT("unstage"), TArray<FString>(), RelativeFiles, Context, /*bUseTargetsFile=*/true, Results, InCommand.ErrorMessages);
	}

	// Move the local branch head onto the new revision.
	{
		TArray<FString> Results;
		int32 ReturnCode = -1;
		TArray<FString> Params;
		Params.Add(Signature);
		if (!LoreSourceControlUtils::RunCommand(TEXT("branch reset"), Params, TArray<FString>(), Context, Results, InCommand.ErrorMessages, ReturnCode))
		{
			InCommand.ErrorMessages.Add(TEXT("`lore branch reset` failed. The revision is committed locally but not pushed."));
			return false;
		}
	}

	// Push.
	{
		TArray<FString> Results;
		int32 ReturnCode = -1;
		if (!LoreSourceControlUtils::RunCommand(TEXT("push"), TArray<FString>(), TArray<FString>(), Context, Results, InCommand.ErrorMessages, ReturnCode))
		{
			InCommand.ErrorMessages.Add(TEXT("`lore push` failed (the remote branch may have moved; sync and retry)."));
			return false;
		}
	}

	// Release locks for the submitted files now they are at head.
	{
		TArray<FString> Results;
		// One file per call: Lore stops at the first file that is not locked (for example a source file).
		bool bReleased = true;
		for (const FString& RelativeFile : RelativeFiles)
		{
			TArray<FString> ReleaseResults;
			TArray<FString> ReleaseErrors;
			int32 ReleaseCode = -1;
			bReleased &= LoreSourceControlUtils::RunCommand(TEXT("lock release"), TArray<FString>(), { RelativeFile }, Context, ReleaseResults, ReleaseErrors, ReleaseCode);
		}
		UE_LOG(LogLoreSourceControl, Log, TEXT("CheckIn: lock release %s for %d file(s)"), bReleased ? TEXT("succeeded") : TEXT("FAILED"), RelativeFiles.Num());
	}

	if (InCommand.Operation->GetName() == "CheckIn")
	{
		const TSharedRef<FCheckIn, ESPMode::ThreadSafe> Operation = StaticCastSharedRef<FCheckIn>(InCommand.Operation);
		Operation->SetSuccessMessage(LOCTEXT("CheckInSuccess", "Submitted revision to Lore."));
	}

	RefreshFileStates(InCommand, /*bScan=*/true, /*bWithHistory=*/false, OutStates, nullptr);
	return true;
}

bool FLoreCheckInWorker::UpdateStates() const
{
	return LoreSourceControlUtils::UpdateCachedStates(OutStates);
}

#undef LOCTEXT_NAMESPACE
