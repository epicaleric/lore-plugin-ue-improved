#pragma once

#include "CoreMinimal.h"
#include "ILoreSourceControlWorker.h"
#include "LoreSourceControlState.h"
#include "LoreSourceControlRevision.h"

/** Connect: locate the binary and confirm the working copy is a Lore repo. */
class FLoreConnectWorker : public ILoreSourceControlWorker
{
public:
	virtual ~FLoreConnectWorker() = default;
	virtual FName GetName() const override;
	virtual bool Execute(FLoreSourceControlCommand& InCommand) override;
	virtual bool UpdateStates() const override;
};

/** UpdateStatus: `lore status --scan` + `lore lock query` (+ history on request). */
class FLoreUpdateStatusWorker : public ILoreSourceControlWorker
{
public:
	virtual ~FLoreUpdateStatusWorker() = default;
	virtual FName GetName() const override;
	virtual bool Execute(FLoreSourceControlCommand& InCommand) override;
	virtual bool UpdateStates() const override;

public:
	TArray<FLoreSourceControlState> OutStates;
	TMap<FString, TArray<TSharedRef<FLoreSourceControlRevision, ESPMode::ThreadSafe>>> OutHistory;
};

/** CheckOut: `lore lock acquire`. */
class FLoreCheckOutWorker : public ILoreSourceControlWorker
{
public:
	virtual ~FLoreCheckOutWorker() = default;
	virtual FName GetName() const override;
	virtual bool Execute(FLoreSourceControlCommand& InCommand) override;
	virtual bool UpdateStates() const override;

public:
	TArray<FLoreSourceControlState> OutStates;
};

/** MarkForAdd: `lore stage`. */
class FLoreMarkForAddWorker : public ILoreSourceControlWorker
{
public:
	virtual ~FLoreMarkForAddWorker() = default;
	virtual FName GetName() const override;
	virtual bool Execute(FLoreSourceControlCommand& InCommand) override;
	virtual bool UpdateStates() const override;

public:
	TArray<FLoreSourceControlState> OutStates;
};

/** Delete: remove on disk then `lore stage` to record the deletion. */
class FLoreDeleteWorker : public ILoreSourceControlWorker
{
public:
	virtual ~FLoreDeleteWorker() = default;
	virtual FName GetName() const override;
	virtual bool Execute(FLoreSourceControlCommand& InCommand) override;
	virtual bool UpdateStates() const override;

public:
	TArray<FLoreSourceControlState> OutStates;
};

/** Revert: `lore reset` (+ release any locks we hold). */
class FLoreRevertWorker : public ILoreSourceControlWorker
{
public:
	virtual ~FLoreRevertWorker() = default;
	virtual FName GetName() const override;
	virtual bool Execute(FLoreSourceControlCommand& InCommand) override;
	virtual bool UpdateStates() const override;

public:
	TArray<FLoreSourceControlState> OutStates;
};

/** Sync: `lore sync`. */
class FLoreSyncWorker : public ILoreSourceControlWorker
{
public:
	virtual ~FLoreSyncWorker() = default;
	virtual FName GetName() const override;
	virtual bool Execute(FLoreSourceControlCommand& InCommand) override;
	virtual bool UpdateStates() const override;

public:
	TArray<FLoreSourceControlState> OutStates;

	/** Workspace-relative files the sync refused to overwrite because they were also changed on this PC. */
	TArray<FString> Conflicts;
};

/** CheckIn: fail-closed verify, `lore stage`, `lore commit`, `lore push`. */
/**
 * Lists the files a sync would change. Without it the editor's Sync Content treats every package under
 * every content root (engine plugins included) as changed and reloads all of them afterwards, which takes
 * minutes and can crash the editor.
 */
class FLoreSyncPreviewWorker : public ILoreSourceControlWorker
{
public:
	virtual ~FLoreSyncPreviewWorker() = default;
	virtual FName GetName() const override;
	virtual bool Execute(FLoreSourceControlCommand& InCommand) override;
	virtual bool UpdateStates() const override;
};

class FLoreCheckInWorker : public ILoreSourceControlWorker
{
public:
	virtual ~FLoreCheckInWorker() = default;
	virtual FName GetName() const override;
	virtual bool Execute(FLoreSourceControlCommand& InCommand) override;
	virtual bool UpdateStates() const override;

public:
	TArray<FLoreSourceControlState> OutStates;
};
