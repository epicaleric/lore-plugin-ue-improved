#pragma once

#include "CoreMinimal.h"
#include "LoreTypes.h"
#include "Templates/SharedPointer.h"
#include "Templates/Atomic.h"

class FLoreSourceControlState;
class FLoreSourceControlRevision;
class FLoreSourceControlCommand;

namespace LoreSourceControlUtils
{
	/** Parameters needed to invoke the `lore` CLI off the game thread. */
	struct FLoreCommandContext
	{
		/** Absolute path to the `lore` executable. */
		FString BinaryPath;

		/** Working copy root; used as the process working directory and `--repository`. */
		FString WorkingCopyRoot;

		/** Optional identity passed via `--identity`. */
		FString UserName;

		/** Force `--offline`. */
		bool bOffline = false;

		/** When set to true from another thread, a running command is stopped (its process is killed). */
		const TAtomic<bool>* CancelFlag = nullptr;

		/** When above zero, replaces the command's usual time limit (for calls made while the editor waits). */
		double TimeoutSeconds = 0.0;
	};

	/**
	 * Resolve the `lore` executable: explicit settings override, then PATH, then
	 * a binary bundled alongside the plugin. Returns an empty string if none found.
	 */
	FString FindLoreBinaryPath(const FString& InSettingsOverride);

	/** Run `lore --version` to confirm the binary is usable. */
	bool CheckLoreAvailable(const FString& InBinaryPath, FString& OutVersion);

	/** Build a context from the active module/provider (game thread). */
	FLoreCommandContext GetCommandContextFromModule();

	/** Build a context from a snapshotted command (worker thread safe). */
	FLoreCommandContext MakeContext(const FLoreSourceControlCommand& InCommand);

	/**
	 * Run a single `lore` invocation. It never waits for input (`--non-interactive`, no stdin) and is
	 * stopped when it runs longer than its command's time limit or when the context's CancelFlag is set.
	 * @param InCommand     The subcommand, possibly multi-word (e.g. "file write").
	 * @param InParameters  Extra flags/arguments, pre-quoted as needed.
	 * @param InFiles       Absolute file paths (quoted automatically).
	 */
	bool RunCommand(
		const FString& InCommand,
		const TArray<FString>& InParameters,
		const TArray<FString>& InFiles,
		const FLoreCommandContext& InContext,
		TArray<FString>& OutResults,
		TArray<FString>& OutErrorMessages,
		int32& OutReturnCode);

	/**
	 * Run a command across a batched file list (chunked, or via a temp
	 * `--targets` file when bUseTargetsFile is set). Aggregates results/errors.
	 */
	bool RunCommandBatched(
		const FString& InCommand,
		const TArray<FString>& InParameters,
		const TArray<FString>& InFiles,
		const FLoreCommandContext& InContext,
		bool bUseTargetsFile,
		TArray<FString>& OutResults,
		TArray<FString>& OutErrorMessages);

	/** Remove ANSI/VT escape sequences from a line. */
	FString StripAnsi(const FString& InLine);

	/**
	 * Workspace-relative paths `lore sync` refused to overwrite because they were also changed on this PC
	 * ("File has local changes: <path> (incoming ...)" and the deleted-file variants).
	 */
	TArray<FString> ParseSyncConflicts(const TArray<FString>& InLines);

	/** Workspace-relative paths in the "Changes staged for commit" section of `lore status` output. */
	TArray<FString> ParseStagedFiles(const TArray<FString>& InLines);

	/** Parse `lore status [--scan]` output into a repo header and per-file states. */
	bool ParseStatusResults(
		const TArray<FString>& InResults,
		const FString& InWorkingCopyRoot,
		FLoreRepoStatus& OutRepoStatus,
		TArray<FLoreSourceControlState>& OutStates);

	/**
	 * The name of the user the `lore` CLI is logged in as (from `lore auth info`),
	 * cached. Locks are reported under this name once the auth server is enabled.
	 * Returns an empty string if it cannot be determined.
	 */
	FString DetectAuthenticatedUser(const FLoreCommandContext& InContext);

	/**
	 * The user's own locks, remembered in Saved/Lore/MyLocks.txt. The Lore server keeps locks in
	 * memory only, so a server restart wipes them; the plugin uses this list to take back the
	 * ones whose files are still changed locally.
	 */
	void LoadRememberedLocks(TSet<FString>& OutPaths);
	void SaveRememberedLocks(const TSet<FString>& InPaths);
	void ForgetRememberedLock(const FString& InPath);

	/** Parse `lore lock query` output into lock records. */
	void ParseLockQueryResults(const TArray<FString>& InResults, TArray<FLoreLockInfo>& OutLocks);

	/** Parse `lore file history <path>` output into revisions (newest first). */
	void ParseHistoryResults(
		const TArray<FString>& InResults,
		const FString& InAbsoluteFilename,
		const FString& InRepoFilename,
		TArray<TSharedRef<FLoreSourceControlRevision, ESPMode::ThreadSafe>>& OutHistory);

	/**
	 * Merge lock records onto states, classifying each as LockedByMe/LockedByOther
	 * relative to CurrentUser. Paths are matched workspace-relative.
	 */
	void ApplyLocksToStates(
		const TArray<FLoreLockInfo>& InLocks,
		const FString& InCurrentUser,
		const FString& InWorkingCopyRoot,
		TArray<FLoreSourceControlState>& InOutStates);

	/** Copy worker-produced states into the provider's cache (game thread only). */
	bool UpdateCachedStates(const TArray<FLoreSourceControlState>& InStates);
}
