#include "LoreHistoryTab.h"
#include "LoreSourceControlUtils.h"
#include "LoreSourceControlLog.h"
#include "LoreTypes.h"

#include "Brushes/SlateRoundedBoxBrush.h"
#include "Framework/Docking/TabManager.h"
#include "Dom/JsonObject.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformApplicationMisc.h"
#include "InputCoreTypes.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "ISourceControlModule.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "ISourceControlProvider.h"
#include "Misc/MessageDialog.h"
#include "Misc/Paths.h"
#include "Misc/ScopedSlowTask.h"
#include "SourceControlOperations.h"
#include "Styling/AppStyle.h"
#include "WorkspaceMenuStructure.h"
#include "WorkspaceMenuStructureModule.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SExpandableArea.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Layout/SSpacer.h"
#include "Widgets/Layout/SWidgetSwitcher.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/SListView.h"
#include "Widgets/Views/STableRow.h"

#define LOCTEXT_NAMESPACE "LoreHistoryTab"

namespace
{
	const FName LoreHistoryTabName(TEXT("LoreHistory"));
	constexpr int32 HistoryCount = 40;

	using FContext = LoreSourceControlUtils::FLoreCommandContext;

	struct FLoreFileChange
	{
		FString Status; // "A", "M" or "D"
		FString Path;   // workspace-relative, forward slashes
	};

	struct FLoreRevisionItem
	{
		int32 Number = 0;
		FString Signature;
		FString Parent;
		FString Author;
		FString Date;
		FString Description;
		bool bFilesLoaded = false;
		TArray<TSharedPtr<FLoreFileChange>> Files;
		TWeakPtr<SVerticalBox> BodyBox;
	};

	/** A file with local changes, as listed in the Changes tab. */
	struct FLoreLocalChange
	{
		FString Status; // "A", "M", "D", ...
		FString Path;
		bool bSelected = true;
	};

	const TCHAR* const AiSection = TEXT("LoreSourceControl.AI");
	const TCHAR* const DefaultGeminiModel = TEXT("gemini-3.5-flash-lite");

	/** AI settings live in the per-user editor config (Saved/Config), never in the repository. */
	FString LoadAiSetting(const TCHAR* InKey, const FString& InDefault)
	{
		FString Value;
		GConfig->GetString(AiSection, InKey, Value, GEditorPerProjectIni);
		return Value.IsEmpty() ? InDefault : Value;
	}

	void SaveAiSetting(const TCHAR* InKey, const FString& InValue)
	{
		GConfig->SetString(AiSection, InKey, *InValue, GEditorPerProjectIni);
		GConfig->Flush(false, GEditorPerProjectIni);
	}

	// --------------------------------------------------------------------- CLI helpers

	TArray<FString> CleanLines(const TArray<FString>& InLines)
	{
		TArray<FString> Out;
		for (const FString& Line : InLines)
		{
			Out.Add(LoreSourceControlUtils::StripAnsi(Line));
		}
		return Out;
	}

	/** Run one lore command; on failure OutError holds the CLI's error text. */
	bool RunLore(const FContext& Context, const FString& Command, const TArray<FString>& Params, const TArray<FString>& Files, TArray<FString>& OutLines, FString& OutError)
	{
		TArray<FString> Errors;
		int32 ReturnCode = -1;
		const bool bOk = LoreSourceControlUtils::RunCommand(Command, Params, Files, Context, OutLines, Errors, ReturnCode);
		OutLines = CleanLines(OutLines);
		OutError = FString::Join(CleanLines(Errors), TEXT(" "));
		UE_LOG(LogLoreSourceControl, Log, TEXT("LoreHistory: `%s` -> %s%s%s"), *Command, bOk ? TEXT("ok") : TEXT("FAILED"), OutError.IsEmpty() ? TEXT("") : TEXT(": "), *OutError);
		return bOk;
	}

	/** Same, over a list of files (batched through a targets file when requested). */
	bool RunLoreBatched(const FContext& Context, const FString& Command, const TArray<FString>& Files, bool bUseTargetsFile, FString& OutError)
	{
		TArray<FString> Lines;
		TArray<FString> Errors;
		const bool bOk = LoreSourceControlUtils::RunCommandBatched(Command, TArray<FString>(), Files, Context, bUseTargetsFile, Lines, Errors);
		OutError = FString::Join(CleanLines(Errors), TEXT(" "));
		UE_LOG(LogLoreSourceControl, Log, TEXT("LoreHistory: `%s` (%d file(s)) -> %s%s%s"), *Command, Files.Num(), bOk ? TEXT("ok") : TEXT("FAILED"), OutError.IsEmpty() ? TEXT("") : TEXT(": "), *OutError);
		return bOk;
	}

	/** Release locks one file at a time: Lore stops at the first file that is not locked, so a batch can miss the rest. */
	bool ReleaseLocks(const FContext& Context, const TArray<FString>& Files, FString& OutError)
	{
		bool bAll = true;
		for (const FString& File : Files)
		{
			TArray<FString> Lines;
			FString Error;
			if (!RunLore(Context, TEXT("lock release"), TArray<FString>(), { File }, Lines, Error))
			{
				bAll = false;
				OutError = Error;
			}
		}
		return bAll;
	}
	FString ValueAfterColon(const FString& InLine)
	{
		int32 Colon = INDEX_NONE;
		return InLine.FindChar(TEXT(':'), Colon) ? InLine.Mid(Colon + 1).TrimStartAndEnd() : FString();
	}

	FString QuoteMessage(FString InMessage)
	{
		InMessage.ReplaceInline(TEXT("\r"), TEXT(" "));
		InMessage.ReplaceInline(TEXT("\n"), TEXT(" "));
		InMessage.ReplaceInline(TEXT("\""), TEXT("\\\""));
		return FString::Printf(TEXT("\"%s\""), *InMessage);
	}

	FString ShortDescription(const FString& InDescription)
	{
		return InDescription.Len() > 80 ? InDescription.Left(77) + TEXT("...") : InDescription;
	}

	/** "Wed, 4 Nov 2026 21:17:14 +0000" -> local time "2026-11-04 22:17". Falls back to the raw text. */
	FString FormatDate(const FString& InRaw)
	{
		TArray<FString> Tokens;
		InRaw.ParseIntoArrayWS(Tokens);
		if (Tokens.Num() >= 5)
		{
			static const TCHAR* Months[] = { TEXT("Jan"), TEXT("Feb"), TEXT("Mar"), TEXT("Apr"), TEXT("May"), TEXT("Jun"), TEXT("Jul"), TEXT("Aug"), TEXT("Sep"), TEXT("Oct"), TEXT("Nov"), TEXT("Dec") };
			int32 Month = 0;
			for (int32 Index = 0; Index < 12; ++Index)
			{
				if (Tokens[2].StartsWith(Months[Index]))
				{
					Month = Index + 1;
				}
			}
			TArray<FString> Time;
			Tokens[4].ParseIntoArray(Time, TEXT(":"));
			if (Month > 0 && Time.Num() == 3)
			{
				const FDateTime Utc(FCString::Atoi(*Tokens[3]), Month, FCString::Atoi(*Tokens[1]), FCString::Atoi(*Time[0]), FCString::Atoi(*Time[1]), FCString::Atoi(*Time[2]));
				const FDateTime Local = Utc + (FDateTime::Now() - FDateTime::UtcNow());
				return Local.ToString(TEXT("%Y-%m-%d %H:%M"));
			}
		}
		return InRaw;
	}

	FSlateColor StatusColor(const FString& InStatus)
	{
		if (InStatus == TEXT("A")) { return FSlateColor(FLinearColor(0.2f, 0.8f, 0.3f)); }
		if (InStatus == TEXT("D")) { return FSlateColor(FLinearColor(0.9f, 0.3f, 0.3f)); }
		return FSlateColor(FLinearColor(0.95f, 0.75f, 0.2f));
	}

	// --------------------------------------------------------------------- data loading

	void ParseHistory(const TArray<FString>& InLines, TArray<TSharedPtr<FLoreRevisionItem>>& OutItems)
	{
		TSharedPtr<FLoreRevisionItem> Current;
		for (const FString& Line : InLines)
		{
			if (Line.TrimStartAndEnd().IsEmpty())
			{
				continue;
			}
			const bool bIndented = FChar::IsWhitespace(Line[0]);
			if (bIndented)
			{
				if (Current.IsValid())
				{
					if (!Current->Description.IsEmpty())
					{
						Current->Description += TEXT(" ");
					}
					Current->Description += Line.TrimStartAndEnd();
				}
				continue;
			}

			FString Key;
			FString Value;
			if (!Line.Split(TEXT(":"), &Key, &Value))
			{
				continue;
			}
			Key.TrimStartAndEndInline();
			Value.TrimStartAndEndInline();
			if (Key == TEXT("Revision"))
			{
				Current = MakeShared<FLoreRevisionItem>();
				Current->Number = FCString::Atoi(*Value);
				OutItems.Add(Current);
			}
			else if (Current.IsValid())
			{
				if (Key == TEXT("Signature")) { Current->Signature = Value; }
				else if (Key == TEXT("Date")) { Current->Date = Value; }
				else if (Key == TEXT("Creator")) { Current->Author = Value; }
			}
		}

		// Each revision's parent is the next older one in the (single-branch) list.
		for (int32 Index = 0; Index + 1 < OutItems.Num(); ++Index)
		{
			OutItems[Index]->Parent = OutItems[Index + 1]->Signature;
		}
	}

	void LoadRevisionFiles(const FContext& Context, FLoreRevisionItem& InOutItem)
	{
		InOutItem.bFilesLoaded = true;
		InOutItem.Files.Reset();

		FString Error;
		TArray<FString> Lines;
		if (InOutItem.Parent.IsEmpty())
		{
			if (RunLore(Context, TEXT("revision info"), { InOutItem.Signature }, TArray<FString>(), Lines, Error))
			{
				for (const FString& Line : Lines)
				{
					if (Line.TrimStart().StartsWith(TEXT("Parent")))
					{
						InOutItem.Parent = ValueAfterColon(Line);
					}
				}
			}
		}
		if (InOutItem.Parent.IsEmpty() || InOutItem.Parent.Replace(TEXT("0"), TEXT("")).IsEmpty())
		{
			return; // first revision: nothing to compare against
		}

		Lines.Reset();
		if (!RunLore(Context, TEXT("revision diff"), { InOutItem.Parent, TEXT("--target"), InOutItem.Signature }, TArray<FString>(), Lines, Error))
		{
			return;
		}

		TArray<TSharedPtr<FLoreFileChange>> All;
		for (const FString& Raw : Lines)
		{
			const FString Line = Raw.TrimStartAndEnd();
			if (Line.Len() > 2 && Line[1] == TEXT(' ') && (Line[0] == TEXT('A') || Line[0] == TEXT('M') || Line[0] == TEXT('D')))
			{
				TSharedPtr<FLoreFileChange> Change = MakeShared<FLoreFileChange>();
				Change->Status = Line.Left(1);
				Change->Path = Line.Mid(2).TrimStartAndEnd();
				All.Add(Change);
			}
		}

		// The diff also lists the directories it touched; keep only files.
		for (const TSharedPtr<FLoreFileChange>& Change : All)
		{
			bool bIsDirectory = false;
			for (const TSharedPtr<FLoreFileChange>& Other : All)
			{
				if (Other != Change && Other->Path.StartsWith(Change->Path + TEXT("/")))
				{
					bIsDirectory = true;
					break;
				}
			}
			if (!bIsDirectory)
			{
				InOutItem.Files.Add(Change);
			}
		}
	}

	/** Workspace-relative paths with local (staged or unstaged) changes, from `lore status`. */
	bool CollectLocalChanges(const FContext& Context, TSet<FString>& OutPaths, FString& OutError)
	{
		TArray<FString> Lines;
		if (!RunLore(Context, TEXT("status"), TArray<FString>(), TArray<FString>(), Lines, OutError))
		{
			return false;
		}
		for (const FString& Raw : Lines)
		{
			FString Line = Raw.TrimStartAndEnd();
			if (Line.Len() > 2 && Line[1] == TEXT(' ') && FString(TEXT("MADRT")).Contains(Line.Left(1), ESearchCase::CaseSensitive))
			{
				Line = Line.Mid(2).TrimStartAndEnd();
				if (Line.EndsWith(TEXT(")")))
				{
					int32 Open = INDEX_NONE;
					if (Line.FindLastChar(TEXT('('), Open))
					{
						Line = Line.Left(Open).TrimEnd();
					}
				}
				OutPaths.Add(Line);
			}
		}
		return true;
	}

	// --------------------------------------------------------------------- restore operations

	void RefreshSourceControlStates(const FContext& Context, const TArray<FString>& InRelativeFiles)
	{
		ISourceControlProvider& Provider = ISourceControlModule::Get().GetProvider();
		TArray<FString> Absolute;
		for (const FString& Relative : InRelativeFiles)
		{
			Absolute.Add(FPaths::ConvertRelativePathToFull(Context.WorkingCopyRoot, Relative));
		}
		Provider.Execute(ISourceControlOperation::Create<FUpdateStatus>(), Absolute, EConcurrency::Asynchronous);
	}

	/** Files with local (staged or unstaged) changes, with their status letter, from `lore status`. */
	bool CollectLocalChangeList(const FContext& Context, TArray<TSharedPtr<FLoreLocalChange>>& OutChanges, FString& OutError)
	{
		TArray<FString> Lines;
		if (!RunLore(Context, TEXT("status"), TArray<FString>(), TArray<FString>(), Lines, OutError))
		{
			return false;
		}
		TSet<FString> Seen;
		for (const FString& Raw : Lines)
		{
			FString Line = Raw.TrimStartAndEnd();
			if (Line.Len() > 2 && Line[1] == TEXT(' ') && FString(TEXT("MADRT")).Contains(Line.Left(1), ESearchCase::CaseSensitive))
			{
				const FString Status = Line.Left(1);
				Line = Line.Mid(2).TrimStartAndEnd();
				if (Line.EndsWith(TEXT(")")))
				{
					int32 Open = INDEX_NONE;
					if (Line.FindLastChar(TEXT('('), Open))
					{
						Line = Line.Left(Open).TrimEnd();
					}
				}
				if (!Seen.Contains(Line))
				{
					Seen.Add(Line);
					TSharedPtr<FLoreLocalChange> Change = MakeShared<FLoreLocalChange>();
					Change->Status = Status;
					Change->Path = Line;
					OutChanges.Add(Change);
				}
			}
		}
		return true;
	}

	/** Commit what is staged, move the branch head onto it and push (the plugin's Submit sequence). */
	bool CommitStagedAndPush(const FContext& Context, const FString& InMessage, FString& OutError, const TArray<FString>& InFilesToUnstage = TArray<FString>())
	{
		TArray<FString> Lines;
		if (!RunLore(Context, TEXT("commit"), { QuoteMessage(InMessage) }, TArray<FString>(), Lines, OutError))
		{
			return false;
		}
		FString Signature;
		for (const FString& Line : Lines)
		{
			if (Line.TrimStart().StartsWith(TEXT("Signature")))
			{
				Signature = ValueAfterColon(Line);
			}
		}
		if (Signature.IsEmpty())
		{
			OutError = TEXT("Commit finished but reported no revision signature.");
			return false;
		}

		// A plain commit leaves the staged state behind, which blocks moving the branch head.
		if (!InFilesToUnstage.IsEmpty())
		{
			FString Ignored;
			RunLoreBatched(Context, TEXT("unstage"), InFilesToUnstage, true, Ignored);
		}

		if (!RunLore(Context, TEXT("branch reset"), { Signature }, TArray<FString>(), Lines, OutError))
		{
			OutError = TEXT("The revision is committed locally but the branch could not be moved: ") + OutError;
			return false;
		}
		if (!RunLore(Context, TEXT("push"), TArray<FString>(), TArray<FString>(), Lines, OutError))
		{
			OutError = TEXT("The restore is committed locally but could not be pushed: ") + OutError;
			return false;
		}
		return true;
	}

	/** Submit the given local changes as one new revision and push it, then release their locks. */
	bool SubmitLocalChanges(const TArray<FString>& InFiles, const FString& InDescription, FString& OutError, bool bKeepLocked = false)
	{
		const FContext Context = LoreSourceControlUtils::GetCommandContextFromModule();
		UE_LOG(LogLoreSourceControl, Log, TEXT("LoreHistory: submitting %d file(s): %s"), InFiles.Num(), *FString::Join(InFiles, TEXT(", ")));

		if (!RunLoreBatched(Context, TEXT("stage"), InFiles, true, OutError))
		{
			OutError = TEXT("Could not stage the changes: ") + OutError;
			return false;
		}
		if (!CommitStagedAndPush(Context, InDescription, OutError, InFiles))
		{
			return false;
		}

		if (!bKeepLocked)
		{
			FString Ignored;
			ReleaseLocks(Context, InFiles, Ignored);
		}
		RefreshSourceControlStates(Context, InFiles);
		return true;
	}

	/** Undo what one revision changed, for all of its files or only InSelectedFiles. Creates a new revision. */
	bool UndoRevision(const FLoreRevisionItem& Item, const TArray<FString>& InSelectedFiles, FString& OutError)
	{
		const FContext Context = LoreSourceControlUtils::GetCommandContextFromModule();

		TArray<FString> AllFiles;
		for (const TSharedPtr<FLoreFileChange>& Change : Item.Files)
		{
			AllFiles.Add(Change->Path);
		}
		const TArray<FString>& Selected = InSelectedFiles.IsEmpty() ? AllFiles : InSelectedFiles;
		TArray<FString> Others;
		for (const FString& Path : AllFiles)
		{
			if (!Selected.Contains(Path))
			{
				Others.Add(Path);
			}
		}

		TSet<FString> Local;
		if (!CollectLocalChanges(Context, Local, OutError))
		{
			return false;
		}
		for (const FString& Path : Selected)
		{
			if (Local.Contains(Path))
			{
				OutError = FString::Printf(TEXT("'%s' has local changes. Submit or revert them first."), *Path);
				return false;
			}
		}

		// Lock first: makes the files writable and keeps others out while we change them.
		if (!RunLoreBatched(Context, TEXT("lock acquire"), Selected, false, OutError))
		{
			OutError = TEXT("Could not lock the files (another user may have them locked). ") + OutError;
			return false;
		}

		TArray<FString> Lines;
		if (!RunLore(Context, TEXT("revision revert"), { Item.Signature, TEXT("--no-commit") }, TArray<FString>(), Lines, OutError))
		{
			FString Ignored;
			RunLore(Context, TEXT("revision revert abort"), TArray<FString>(), TArray<FString>(), Lines, Ignored);
			ReleaseLocks(Context, Selected, Ignored);
			return false;
		}
		const FString Joined = FString::Join(Lines, TEXT(" "));
		if (Joined.Contains(TEXT("conflicted")) && !Joined.Contains(TEXT(" 0 conflicted")))
		{
			FString Ignored;
			RunLore(Context, TEXT("revision revert abort"), TArray<FString>(), TArray<FString>(), Lines, Ignored);
			ReleaseLocks(Context, Selected, Ignored);
			OutError = TEXT("Later revisions changed the same files, so this revision cannot be undone automatically. Nothing was changed.");
			return false;
		}

		// Per-file restore: drop the reverted state of the files we do not want to touch.
		if (!Others.IsEmpty())
		{
			RunLoreBatched(Context, TEXT("unstage"), Others, true, OutError);
			RunLoreBatched(Context, TEXT("reset"), Others, true, OutError);
		}

		const FString Message = InSelectedFiles.IsEmpty()
			? FString::Printf(TEXT("Undo revision %d: %s"), Item.Number, *ShortDescription(Item.Description))
			: FString::Printf(TEXT("Undo %d file(s) of revision %d: %s"), Selected.Num(), Item.Number, *ShortDescription(Item.Description));
		const bool bOk = CommitStagedAndPush(Context, Message, OutError);

		FString Ignored;
		ReleaseLocks(Context, Selected, Ignored);
		RefreshSourceControlStates(Context, Selected);
		return bOk;
	}

	/** Put the whole project back to how it was at a revision, as a new revision. */
	bool RestoreProjectToRevision(const FLoreRevisionItem& Item, FString& OutError)
	{
		const FContext Context = LoreSourceControlUtils::GetCommandContextFromModule();

		TSet<FString> Local;
		if (!CollectLocalChanges(Context, Local, OutError))
		{
			return false;
		}
		if (!Local.IsEmpty())
		{
			OutError = FString::Printf(TEXT("Your working copy has %d uncommitted change(s). Submit or revert them first."), Local.Num());
			return false;
		}

		TArray<FString> Lines;
		if (!RunLore(Context, TEXT("sync"), { Item.Signature }, TArray<FString>(), Lines, OutError))
		{
			return false;
		}
		const FString Message = FString::Printf(TEXT("Restore project to revision %d: %s"), Item.Number, *ShortDescription(Item.Description));
		if (!RunLore(Context, TEXT("revision restore"), { QuoteMessage(Message) }, TArray<FString>(), Lines, OutError))
		{
			return false;
		}

		FString Ignored;
		RunLore(Context, TEXT("push"), TArray<FString>(), TArray<FString>(), Lines, Ignored); // already pushed by restore; harmless
		RefreshSourceControlStates(Context, TArray<FString>());
		return true;
	}

	bool UnlockFile(const FLoreLockInfo& Lock, FString& OutError)
	{
		const FContext Context = LoreSourceControlUtils::GetCommandContextFromModule();
		TArray<FString> Lines;
		TArray<FString> Params;
		if (!Lock.Branch.IsEmpty())
		{
			Params.Add(TEXT("--branch"));
			Params.Add(Lock.Branch);
		}
		if (RunLore(Context, TEXT("lock release"), Params, { Lock.Path }, Lines, OutError))
		{
			return true;
		}

		// A lock held by someone else needs the owner named and the operation forced.
		Params.Add(TEXT("--owner"));
		Params.Add(Lock.Owner);
		Params.Add(TEXT("--force"));
		return RunLore(Context, TEXT("lock release"), Params, { Lock.Path }, Lines, OutError);
	}

	bool Confirm(const FText& InMessage)
	{
		return FMessageDialog::Open(EAppMsgType::YesNo, InMessage) == EAppReturnType::Yes;
	}

	void Notify(const FText& InMessage)
	{
		FMessageDialog::Open(EAppMsgType::Ok, InMessage);
	}
}

// ------------------------------------------------------------------------- panel helpers

namespace
{
	const FLinearColor Purple(0.42f, 0.22f, 0.62f, 1.0f);
	// Brush colors are linear, so these small values display as a dark grey.
	const FSlateRoundedBoxBrush CardBrush(FLinearColor(0.008f, 0.008f, 0.010f, 1.0f), 6.0f);
	const FSlateRoundedBoxBrush AvatarBrush(Purple, 14.0f);
	const FSlateRoundedBoxBrush DotBrush(Purple, 4.0f);
	const FSlateRoundedBoxBrush HeadBrush(Purple, 10.0f);

	TSharedRef<SWidget> MakeIcon(const FName& InBrushName, const FLinearColor& InColor, float InSize = 16.0f)
	{
		return SNew(SBox).WidthOverride(InSize).HeightOverride(InSize).HAlign(HAlign_Center).VAlign(VAlign_Center)
			[
				SNew(SImage).Image(FAppStyle::GetBrush(InBrushName)).ColorAndOpacity(FSlateColor(InColor))
			];
	}

	TSharedRef<SWidget> MakeStateIcon(const FString& InStatus)
	{
		if (InStatus == TEXT("A")) { return MakeIcon("Icons.Plus", FLinearColor(0.25f, 0.6f, 1.0f)); }
		if (InStatus == TEXT("D")) { return MakeIcon("Icons.Delete", FLinearColor(0.9f, 0.3f, 0.3f)); }
		return MakeIcon("Icons.Edit", FLinearColor(0.95f, 0.75f, 0.2f));
	}

	FText StateWord(const FString& InStatus)
	{
		if (InStatus == TEXT("A")) { return LOCTEXT("StateAdded", "Added"); }
		if (InStatus == TEXT("D")) { return LOCTEXT("StateDeleted", "Deleted"); }
		return LOCTEXT("StateModified", "Modified");
	}

	bool ParseRevisionNumber(const FString& InLine, const FString& InPrefix, int32& OutNumber)
	{
		const int32 Start = InLine.Find(InPrefix, ESearchCase::CaseSensitive);
		if (Start == INDEX_NONE)
		{
			return false;
		}
		OutNumber = FCString::Atoi(*InLine.Mid(Start + InPrefix.Len()).TrimStart());
		return true;
	}

	/** `lore status`: local changes plus the local and remote revision numbers. */
	bool LoadStatus(const FContext& Context, TArray<TSharedPtr<FLoreLocalChange>>& OutChanges, int32& OutLocalRevision, int32& OutRemoteRevision, FString& OutError)
	{
		TArray<FString> Lines;
		if (!RunLore(Context, TEXT("status"), TArray<FString>(), TArray<FString>(), Lines, OutError))
		{
			return false;
		}
		TSet<FString> Seen;
		for (const FString& Raw : Lines)
		{
			FString Line = Raw.TrimStartAndEnd();
			if (Line.StartsWith(TEXT("On branch")))
			{
				ParseRevisionNumber(Line, TEXT(" revision "), OutLocalRevision);
				continue;
			}
			if (Line.StartsWith(TEXT("Remote revision")))
			{
				ParseRevisionNumber(Line, TEXT("Remote revision "), OutRemoteRevision);
				continue;
			}
			if (Line.Len() > 2 && Line[1] == TEXT(' ') && FString(TEXT("MADRT")).Contains(Line.Left(1), ESearchCase::CaseSensitive))
			{
				const FString Status = Line.Left(1);
				Line = Line.Mid(2).TrimStartAndEnd();
				if (Line.EndsWith(TEXT(")")))
				{
					int32 Open = INDEX_NONE;
					if (Line.FindLastChar(TEXT('('), Open))
					{
						Line = Line.Left(Open).TrimEnd();
					}
				}
				if (!Seen.Contains(Line))
				{
					Seen.Add(Line);
					TSharedPtr<FLoreLocalChange> Change = MakeShared<FLoreLocalChange>();
					Change->Status = Status;
					Change->Path = Line;
					OutChanges.Add(Change);
				}
			}
		}
		return true;
	}

	FString GetBranchName(const FContext& Context)
	{
		TArray<FString> Lines;
		FString Error;
		if (RunLore(Context, TEXT("branch info"), TArray<FString>(), TArray<FString>(), Lines, Error))
		{
			for (const FString& Line : Lines)
			{
				const FString Trimmed = Line.TrimStart();
				if (Trimmed.StartsWith(TEXT("Branch ")))
				{
					return Trimmed.Mid(7).TrimStartAndEnd();
				}
			}
		}
		return TEXT("main");
	}

	FString FileHistoryText(const FString& InPath, FString& OutError)
	{
		const FContext Context = LoreSourceControlUtils::GetCommandContextFromModule();
		TArray<FString> Lines;
		if (!RunLore(Context, TEXT("file history"), TArray<FString>(), { InPath }, Lines, OutError))
		{
			return FString();
		}
		TArray<TSharedPtr<FLoreRevisionItem>> Items;
		ParseHistory(Lines, Items);
		FString Text = FString::Printf(TEXT("%s\n\n"), *InPath);
		for (const TSharedPtr<FLoreRevisionItem>& Item : Items)
		{
			Text += FString::Printf(TEXT("%d   %s   %s\n      %s\n"), Item->Number, *Item->Author, *FormatDate(Item->Date), *Item->Description);
		}
		return Items.IsEmpty() ? FString(TEXT("No history found.")) : Text;
	}

	/** Release one of our own locks (no forcing, so another user's lock is never touched). */
	bool UnlockOwn(const FLoreLockInfo& Lock, FString& OutError)
	{
		const FContext Context = LoreSourceControlUtils::GetCommandContextFromModule();
		TArray<FString> Params;
		if (!Lock.Branch.IsEmpty())
		{
			Params.Add(TEXT("--branch"));
			Params.Add(Lock.Branch);
		}
		TArray<FString> Lines;
		return RunLore(Context, TEXT("lock release"), Params, { Lock.Path }, Lines, OutError);
	}

	bool SyncToLatest(FString& OutError)
	{
		const FContext Context = LoreSourceControlUtils::GetCommandContextFromModule();
		TArray<FString> Lines;
		if (!RunLore(Context, TEXT("sync"), TArray<FString>(), TArray<FString>(), Lines, OutError))
		{
			return false;
		}
		RefreshSourceControlStates(Context, TArray<FString>());
		return true;
	}
}

// ------------------------------------------------------------------------- the panel

class SLoreHistoryPanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SLoreHistoryPanel) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs)
	{
		Language = LoadAiSetting(TEXT("Language"), TEXT("en"));

		ChildSlot
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(6.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 4.0f, 0.0f)
				[
					MakeTabButton(LOCTEXT("TabHistory", "History"), 0)
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 4.0f, 0.0f)
				[
					MakeTabButton(LOCTEXT("TabChanges", "Changes"), 1)
				]
				+ SHorizontalBox::Slot().AutoWidth()
				[
					MakeTabButton(LOCTEXT("TabLocks", "Locks"), 2)
				]
				+ SHorizontalBox::Slot().FillWidth(1.0f)
				[
					SNew(SSpacer)
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 8.0f, 0.0f)
				[
					SNew(SBorder).BorderImage(&CardBrush).Padding(FMargin(8.0f, 3.0f))
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 6.0f, 0.0f)
						[
							MakeIcon("Icons.Check", FLinearColor(0.35f, 0.8f, 0.45f), 14.0f)
						]
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
						[
							SAssignNew(BranchBlock, STextBlock).Text(LOCTEXT("BranchDefault", "main"))
						]
					]
				]
				+ SHorizontalBox::Slot().AutoWidth()
				[
					SNew(SButton).OnClicked(this, &SLoreHistoryPanel::OnRefresh).ToolTipText(LOCTEXT("RefreshTip", "Reload revisions, changes and locks"))
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 4.0f, 0.0f)
						[
							MakeIcon("Icons.Refresh", FLinearColor::White, 14.0f)
						]
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
						[
							SNew(STextBlock).Text(LOCTEXT("Refresh", "Refresh"))
						]
					]
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(8.0f, 0.0f, 8.0f, 4.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1.0f).Padding(0.0f, 0.0f, 6.0f, 0.0f)
				[
					SAssignNew(FilterBox, SEditableTextBox)
					.HintText(LOCTEXT("FilterHint", "Filter revisions by author, text, date or number"))
					.OnTextChanged(this, &SLoreHistoryPanel::OnFilterChanged)
				]
				+ SHorizontalBox::Slot().AutoWidth()
				[
					SNew(SButton).Text(LOCTEXT("ClearFilter", "Clear Filter")).OnClicked(this, &SLoreHistoryPanel::OnClearFilter)
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(8.0f, 0.0f, 8.0f, 6.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
				[
					SAssignNew(StatusBlock, STextBlock).ColorAndOpacity(FSlateColor::UseSubduedForeground())
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(6.0f, 0.0f, 0.0f, 0.0f)
				[
					SNew(SButton)
					.Visibility_Lambda([this]() { return BehindCount > 0 ? EVisibility::Visible : EVisibility::Collapsed; })
					.OnClicked(this, &SLoreHistoryPanel::OnSyncLatest)
					.ToolTipText(LOCTEXT("SyncLatestTip", "Download the newest revisions from the server into your project"))
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 4.0f, 0.0f)
						[
							MakeIcon("Icons.Download", FLinearColor(0.95f, 0.75f, 0.2f), 14.0f)
						]
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
						[
							SNew(STextBlock).Text_Lambda([this]() { return FText::Format(LOCTEXT("SyncLatest", "Sync Latest ({0} new)"), FText::AsNumber(BehindCount)); })
						]
					]
				]
			]
			+ SVerticalBox::Slot().FillHeight(1.0f)
			[
				SAssignNew(Switcher, SWidgetSwitcher)
				.WidgetIndex(0)
				+ SWidgetSwitcher::Slot()
				[
					SAssignNew(HistoryList, SListView<TSharedPtr<FLoreRevisionItem>>)
					.ListItemsSource(&Filtered)
					.OnGenerateRow(this, &SLoreHistoryPanel::GenerateRevisionRow)
					.SelectionMode(ESelectionMode::None)
				]
				+ SWidgetSwitcher::Slot()
				[
					BuildChangesTab()
				]
				+ SWidgetSwitcher::Slot()
				[
					BuildLocksTab()
				]
			]
		];

		Reload();
	}

private:
	// ----- chrome

	TSharedRef<SWidget> MakeTabButton(const FText& InLabel, int32 InIndex)
	{
		return SNew(SButton)
			.Text(InLabel)
			.ButtonColorAndOpacity_Lambda([this, InIndex]() { return ActiveTab == InIndex ? Purple : FLinearColor::White; })
			.OnClicked(this, &SLoreHistoryPanel::OnShowTab, InIndex);
	}

	FReply OnShowTab(int32 InIndex)
	{
		ActiveTab = InIndex;
		Switcher->SetActiveWidgetIndex(InIndex);
		return FReply::Handled();
	}

	FReply OnRefresh()
	{
		Reload();
		return FReply::Handled();
	}

	void OnFilterChanged(const FText& InText)
	{
		FilterText = InText.ToString().TrimStartAndEnd();
		ApplyFilter();
	}

	FReply OnClearFilter()
	{
		FilterBox->SetText(FText::GetEmpty());
		FilterText.Reset();
		ApplyFilter();
		return FReply::Handled();
	}

	void ApplyFilter()
	{
		Filtered.Reset();
		for (const TSharedPtr<FLoreRevisionItem>& Revision : Revisions)
		{
			if (FilterText.IsEmpty()
				|| Revision->Author.Contains(FilterText, ESearchCase::IgnoreCase)
				|| Revision->Description.Contains(FilterText, ESearchCase::IgnoreCase)
				|| FormatDate(Revision->Date).Contains(FilterText)
				|| FString::FromInt(Revision->Number) == FilterText)
			{
				Filtered.Add(Revision);
			}
		}
		HistoryList->RequestListRefresh();
	}

	void Reload()
	{
		const FContext Context = LoreSourceControlUtils::GetCommandContextFromModule();

		Revisions.Reset();
		FString Error;
		TArray<FString> Lines;
		if (RunLore(Context, TEXT("history"), { FString::FromInt(HistoryCount) }, TArray<FString>(), Lines, Error))
		{
			ParseHistory(Lines, Revisions);
		}

		Locks.Reset();
		FString LockError;
		TArray<FString> LockLines;
		if (RunLore(Context, TEXT("lock query"), TArray<FString>(), TArray<FString>(), LockLines, LockError))
		{
			TArray<FLoreLockInfo> Parsed;
			LoreSourceControlUtils::ParseLockQueryResults(LockLines, Parsed);
			for (const FLoreLockInfo& Lock : Parsed)
			{
				Locks.Add(MakeShared<FLoreLockInfo>(Lock));
			}
		}

		Changes.Reset();
		int32 LocalRevision = 0;
		int32 RemoteRevision = 0;
		FString ChangesError;
		LoadStatus(Context, Changes, LocalRevision, RemoteRevision, ChangesError);
		BehindCount = FMath::Max(0, RemoteRevision - LocalRevision);

		BranchBlock->SetText(FText::FromString(GetBranchName(Context)));
		SetStatus(FString::Printf(TEXT("%d revision(s)  |  %d changed file(s)  |  %d locked file(s)%s"), Revisions.Num(), Changes.Num(), Locks.Num(),
			Error.IsEmpty() ? TEXT("") : *(TEXT("  |  History error: ") + Error)));

		ApplyFilter();
		ChangeList->RequestListRefresh();
		LockList->RequestListRefresh();
	}

	void SetStatus(const FString& InText)
	{
		StatusBlock->SetText(FText::FromString(InText));
	}

	FReply OnSyncLatest()
	{
		if (!Confirm(FText::Format(LOCTEXT("ConfirmSync", "Download {0} new revision(s) from the server into your project?\n\nFiles open in the editor may need to be reloaded afterwards."), FText::AsNumber(BehindCount))))
		{
			return FReply::Handled();
		}
		FString Error;
		bool bOk = false;
		{
			FScopedSlowTask SlowTask(1.0f, LOCTEXT("Syncing", "Syncing with Lore..."));
			SlowTask.MakeDialog();
			SlowTask.EnterProgressFrame(1.0f);
			bOk = SyncToLatest(Error);
		}
		Notify(bOk
			? LOCTEXT("SyncDone", "Done. Your project is up to date. Reload the level (or restart the editor) so changed assets are read again.")
			: FText::Format(LOCTEXT("SyncFailed", "Sync did not work:\n{0}"), FText::FromString(Error)));
		Reload();
		return FReply::Handled();
	}

	// ----- history

	TSharedRef<ITableRow> GenerateRevisionRow(TSharedPtr<FLoreRevisionItem> Item, const TSharedRef<STableViewBase>& OwnerTable)
	{
		const TSharedRef<SVerticalBox> Body = SNew(SVerticalBox);
		Item->BodyBox = Body;
		if (Item->bFilesLoaded)
		{
			PopulateBody(Item);
		}

		const bool bIsHead = !Revisions.IsEmpty() && Revisions[0] == Item;
		const FString Initial = Item->Author.IsEmpty() ? FString(TEXT("?")) : Item->Author.Left(1).ToUpper();

		return SNew(STableRow<TSharedPtr<FLoreRevisionItem>>, OwnerTable)
			.Padding(FMargin(0.0f))
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth()
				[
					SNew(SBox).WidthOverride(36.0f)
					[
						SNew(SOverlay)
						+ SOverlay::Slot().HAlign(HAlign_Center)
						[
							SNew(SBox).WidthOverride(2.0f)
							[
								SNew(SBorder).BorderImage(FAppStyle::GetBrush("WhiteBrush")).BorderBackgroundColor(Purple)
							]
						]
						+ SOverlay::Slot().HAlign(HAlign_Center).VAlign(VAlign_Top).Padding(0.0f, 16.0f, 0.0f, 0.0f)
						[
							bIsHead
								? StaticCastSharedRef<SWidget>(SNew(SBox).WidthOverride(20.0f).HeightOverride(20.0f)
									[
										SNew(SOverlay)
										+ SOverlay::Slot()[ SNew(SBorder).BorderImage(&HeadBrush) ]
										+ SOverlay::Slot().HAlign(HAlign_Center).VAlign(VAlign_Center)[ MakeIcon("Icons.Check", FLinearColor::White, 12.0f) ]
									])
								: StaticCastSharedRef<SWidget>(SNew(SBox).WidthOverride(8.0f).HeightOverride(8.0f)
									[
										SNew(SBorder).BorderImage(&DotBrush)
									])
						]
					]
				]
				+ SHorizontalBox::Slot().FillWidth(1.0f).Padding(0.0f, 3.0f, 6.0f, 3.0f)
				[
					SNew(SExpandableArea)
					.InitiallyCollapsed(true)
					.OnAreaExpansionChanged(FOnBooleanValueChanged::CreateSP(this, &SLoreHistoryPanel::OnExpansionChanged, Item))
					.HeaderContent()
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(4.0f, 2.0f, 10.0f, 2.0f)
						[
							SNew(SBox).WidthOverride(28.0f).HeightOverride(28.0f)
							[
								SNew(SBorder).BorderImage(&AvatarBrush).HAlign(HAlign_Center).VAlign(VAlign_Center).Padding(0.0f)
								[
									SNew(STextBlock).Font(FAppStyle::GetFontStyle("BoldFont")).Text(FText::FromString(Initial))
								]
							]
						]
						+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
						[
							SNew(SVerticalBox)
							+ SVerticalBox::Slot().AutoHeight()
							[
								SNew(SHorizontalBox)
								+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 8.0f, 0.0f)
								[
									SNew(STextBlock).Font(FAppStyle::GetFontStyle("BoldFont")).Text(FText::FromString(Item->Author))
								]
								+ SHorizontalBox::Slot().AutoWidth()
								[
									SNew(STextBlock).ColorAndOpacity(FSlateColor::UseSubduedForeground()).Text(FText::FromString(FString::Printf(TEXT("#%d"), Item->Number)))
								]
							]
							+ SVerticalBox::Slot().AutoHeight()
							[
								SNew(STextBlock).Text(FText::FromString(Item->Description))
							]
						]
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8.0f, 0.0f)
						[
							SNew(STextBlock).ColorAndOpacity(FSlateColor::UseSubduedForeground()).Text(FText::FromString(FormatDate(Item->Date)))
						]
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(2.0f, 0.0f)
						[
							SNew(SButton)
							.ToolTipText(LOCTEXT("UndoRevisionTip", "Undo everything this revision changed (creates a new revision)"))
							.OnClicked(this, &SLoreHistoryPanel::OnUndoRevision, Item)
							[
								SNew(STextBlock).Text(LOCTEXT("UndoRevision", "Undo"))
							]
						]
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(2.0f, 0.0f, 4.0f, 0.0f)
						[
							SNew(SButton)
							.IsEnabled(!bIsHead)
							.ToolTipText(FText::Format(LOCTEXT("RestoreTip", "Sync your local copy of the project to revision {0}"), FText::AsNumber(Item->Number)))
							.OnClicked(this, &SLoreHistoryPanel::OnRestoreToRevision, Item)
							[
								MakeIcon("Icons.Download", bIsHead ? FLinearColor(0.4f, 0.4f, 0.4f) : FLinearColor(0.55f, 0.9f, 0.3f), 16.0f)
							]
						]
					]
					.BodyContent()
					[
						Body
					]
				]
			];
	}

	void OnExpansionChanged(bool bExpanded, TSharedPtr<FLoreRevisionItem> Item)
	{
		if (bExpanded && !Item->bFilesLoaded)
		{
			LoadRevisionFiles(LoreSourceControlUtils::GetCommandContextFromModule(), *Item);
			PopulateBody(Item);
		}
	}

	void PopulateBody(const TSharedPtr<FLoreRevisionItem>& Item)
	{
		const TSharedPtr<SVerticalBox> Body = Item->BodyBox.Pin();
		if (!Body.IsValid())
		{
			return;
		}
		Body->ClearChildren();

		const TSharedRef<SVerticalBox> Card = SNew(SVerticalBox);
		Card->AddSlot().AutoHeight()
		[
			SNew(STextBlock).ColorAndOpacity(FSlateColor::UseSubduedForeground())
				.Text(FText::Format(LOCTEXT("CardDetails", "{0}   |   {1} file(s)"), FText::FromString(FormatDate(Item->Date)), FText::AsNumber(Item->Files.Num())))
		];
		Card->AddSlot().AutoHeight().Padding(0.0f, 4.0f, 0.0f, 8.0f)
		[
			SNew(STextBlock).AutoWrapText(true).Text(FText::FromString(Item->Description))
		];

		if (Item->Files.IsEmpty())
		{
			Card->AddSlot().AutoHeight()
			[
				SNew(STextBlock).ColorAndOpacity(FSlateColor::UseSubduedForeground()).Text(LOCTEXT("NoFiles", "No file changes (or this is the first revision)."))
			];
		}
		else
		{
			Card->AddSlot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 2.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1.0f).Padding(24.0f, 0.0f)
				[
					SNew(STextBlock).ColorAndOpacity(FSlateColor::UseSubduedForeground()).Text(LOCTEXT("ColAsset", "Asset"))
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 8.0f, 0.0f)
				[
					SNew(STextBlock).ColorAndOpacity(FSlateColor::UseSubduedForeground()).Text(LOCTEXT("ColState", "State"))
				]
			];
		}

		for (const TSharedPtr<FLoreFileChange>& File : Item->Files)
		{
			Card->AddSlot().AutoHeight()
			[
				SNew(SBorder).BorderImage(FAppStyle::GetBrush("NoBorder")).Padding(FMargin(2.0f, 3.0f))
				.ToolTipText(FText::FromString(File->Path + TEXT("\n(right-click for more)")))
				.OnMouseButtonUp(this, &SLoreHistoryPanel::OnFileRowMouseUp, Item, File)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 8.0f, 0.0f)
					[
						MakeStateIcon(File->Status)
					]
					+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
					[
						SNew(STextBlock).Text(FText::FromString(FPaths::GetCleanFilename(File->Path)))
					]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8.0f, 0.0f)
					[
						SNew(STextBlock).ColorAndOpacity(StatusColor(File->Status)).Text(StateWord(File->Status))
					]
				]
			];
		}

		Body->AddSlot().AutoHeight().Padding(4.0f, 2.0f, 4.0f, 6.0f)
		[
			SNew(SBorder).BorderImage(&CardBrush).Padding(FMargin(12.0f))
			[
				Card
			]
		];
	}

	FReply OnFileRowMouseUp(const FGeometry&, const FPointerEvent& InMouseEvent, TSharedPtr<FLoreRevisionItem> Item, TSharedPtr<FLoreFileChange> File)
	{
		if (InMouseEvent.GetEffectingButton() != EKeys::RightMouseButton)
		{
			return FReply::Unhandled();
		}

		FMenuBuilder Menu(true, nullptr);
		Menu.AddMenuEntry(LOCTEXT("MenuHistory", "History"), LOCTEXT("MenuHistoryTip", "Show every revision that changed this file"), FSlateIcon(),
			FUIAction(FExecuteAction::CreateSP(this, &SLoreHistoryPanel::ShowFileHistory, File->Path)));
		Menu.AddMenuEntry(LOCTEXT("MenuUndoFile", "Undo this change"), LOCTEXT("MenuUndoFileTip", "Put this file back to how it was before this revision (creates a new revision)"), FSlateIcon(),
			FUIAction(FExecuteAction::CreateSP(this, &SLoreHistoryPanel::RunUndo, Item, TArray<FString>({ File->Path }))));
		Menu.AddMenuEntry(LOCTEXT("MenuCopyPath", "Copy path"), LOCTEXT("MenuCopyPathTip", "Copy the file path to the clipboard"), FSlateIcon(),
			FUIAction(FExecuteAction::CreateSP(this, &SLoreHistoryPanel::CopyPath, File->Path)));

		FSlateApplication::Get().PushMenu(AsShared(), InMouseEvent.GetEventPath() ? *InMouseEvent.GetEventPath() : FWidgetPath(), Menu.MakeWidget(),
			InMouseEvent.GetScreenSpacePosition(), FPopupTransitionEffect(FPopupTransitionEffect::ContextMenu));
		return FReply::Handled();
	}

	void ShowFileHistory(FString InPath)
	{
		FString Error;
		const FString Text = FileHistoryText(InPath, Error);
		Notify(Text.IsEmpty() ? FText::Format(LOCTEXT("FileHistoryFailed", "Could not read the history:\n{0}"), FText::FromString(Error)) : FText::FromString(Text));
	}

	void CopyPath(FString InPath)
	{
		FPlatformApplicationMisc::ClipboardCopy(*InPath);
	}

	FReply OnUndoRevision(TSharedPtr<FLoreRevisionItem> Item)
	{
		if (!Item->bFilesLoaded)
		{
			LoadRevisionFiles(LoreSourceControlUtils::GetCommandContextFromModule(), *Item);
		}
		RunUndo(Item, TArray<FString>());
		return FReply::Handled();
	}

	void RunUndo(TSharedPtr<FLoreRevisionItem> Item, TArray<FString> InFiles)
	{
		const FText Question = InFiles.IsEmpty()
			? FText::Format(LOCTEXT("ConfirmUndoRevision", "Undo everything revision {0} changed ({1} file(s))?\n\nThis creates a new revision and pushes it to the server. The history is kept."), FText::AsNumber(Item->Number), FText::AsNumber(Item->Files.Num()))
			: FText::Format(LOCTEXT("ConfirmUndoFile", "Undo the change of revision {0} to:\n{1}\n\nThis creates a new revision and pushes it to the server. The history is kept."), FText::AsNumber(Item->Number), FText::FromString(InFiles[0]));
		if (!Confirm(Question))
		{
			return;
		}

		FString Error;
		bool bOk = false;
		{
			FScopedSlowTask SlowTask(1.0f, LOCTEXT("Undoing", "Undoing changes in Lore..."));
			SlowTask.MakeDialog();
			SlowTask.EnterProgressFrame(1.0f);
			bOk = UndoRevision(*Item, InFiles, Error);
		}
		Notify(bOk
			? LOCTEXT("UndoDone", "Done. A new revision was created. Reload the level (or restart the editor) so changed assets are read again.")
			: FText::Format(LOCTEXT("UndoFailed", "That did not work:\n{0}"), FText::FromString(Error)));
		Reload();
	}

	FReply OnRestoreToRevision(TSharedPtr<FLoreRevisionItem> Item)
	{
		if (!Confirm(FText::Format(LOCTEXT("ConfirmRestore", "Put the whole project back to revision {0}?\n\nThis creates a new revision with that state and pushes it. The history is kept. Close the level first if its files are open."), FText::AsNumber(Item->Number))))
		{
			return FReply::Handled();
		}

		FString Error;
		bool bOk = false;
		{
			FScopedSlowTask SlowTask(1.0f, LOCTEXT("Restoring", "Restoring project in Lore..."));
			SlowTask.MakeDialog();
			SlowTask.EnterProgressFrame(1.0f);
			bOk = RestoreProjectToRevision(*Item, Error);
		}
		Notify(bOk
			? LOCTEXT("RestoreDone", "Done. The project is back at that version as a new revision. Restart the editor so everything is read again.")
			: FText::Format(LOCTEXT("RestoreFailed", "That did not work:\n{0}"), FText::FromString(Error)));
		Reload();
		return FReply::Handled();
	}

	// ----- changes (check-in)

	TSharedRef<SWidget> BuildChangesTab()
	{
		return SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(8.0f, 0.0f, 8.0f, 4.0f)
			[
				SNew(STextBlock).Font(FAppStyle::GetFontStyle("BoldFont")).Text(LOCTEXT("DescriptionLabel", "Changelist Description"))
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(8.0f, 0.0f)
			[
				SNew(SBox).MinDesiredHeight(90.0f)
				[
					SAssignNew(DescriptionBox, SMultiLineEditableTextBox)
					.HintText(LOCTEXT("DescriptionHint", "What did you change? Or press Generate."))
					.AutoWrapText(true)
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(8.0f, 6.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 8.0f, 0.0f)
				[
					SNew(SButton).Text(LOCTEXT("Generate", "Generate with Gemini")).OnClicked(this, &SLoreHistoryPanel::OnGenerate)
				]
				+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
				[
					SAssignNew(GenerateStatus, STextBlock).ColorAndOpacity(FSlateColor::UseSubduedForeground())
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8.0f, 0.0f)
				[
					SNew(SCheckBox)
					.IsChecked_Lambda([this]() { return bKeepLocked ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
					.OnCheckStateChanged_Lambda([this](ECheckBoxState InState) { bKeepLocked = (InState == ECheckBoxState::Checked); })
					[
						SNew(STextBlock).Text(LOCTEXT("KeepLocked", "Keep Files Checked Out"))
					]
				]
				+ SHorizontalBox::Slot().AutoWidth()
				[
					SNew(SButton).Text(LOCTEXT("Submit", "Submit"))
						.IsEnabled(this, &SLoreHistoryPanel::CanSubmit)
						.OnClicked(this, &SLoreHistoryPanel::OnSubmit)
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(8.0f, 4.0f, 8.0f, 2.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 8.0f, 0.0f)
				[
					SNew(SCheckBox)
					.IsChecked_Lambda([this]() { return AllSelected() ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
					.OnCheckStateChanged_Lambda([this](ECheckBoxState InState)
					{
						for (const TSharedPtr<FLoreLocalChange>& Change : Changes)
						{
							Change->bSelected = (InState == ECheckBoxState::Checked);
						}
					})
				]
				+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center).Padding(24.0f, 0.0f)
				[
					SNew(STextBlock).ColorAndOpacity(FSlateColor::UseSubduedForeground()).Text(LOCTEXT("ColAsset", "Asset"))
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 8.0f, 0.0f)
				[
					SNew(STextBlock).ColorAndOpacity(FSlateColor::UseSubduedForeground()).Text(LOCTEXT("ColState", "State"))
				]
			]
			+ SVerticalBox::Slot().FillHeight(1.0f)
			[
				SAssignNew(ChangeList, SListView<TSharedPtr<FLoreLocalChange>>)
				.ListItemsSource(&Changes)
				.OnGenerateRow(this, &SLoreHistoryPanel::GenerateChangeRow)
				.SelectionMode(ESelectionMode::None)
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(8.0f, 6.0f, 8.0f, 2.0f)
			[
				SNew(SExpandableArea)
				.InitiallyCollapsed(true)
				.AreaTitle(LOCTEXT("AiSettings", "Gemini settings (stored only on this computer)"))
				.BodyContent()
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight().Padding(4.0f)
					[
						SNew(STextBlock).AutoWrapText(true).ColorAndOpacity(FSlateColor::UseSubduedForeground())
							.Text(LOCTEXT("AiPrivacy", "Generate sends only the file names and change types, plus the last few descriptions, to Google. On the free tier Google may use prompts to improve its products. The key is saved in this project's Saved/Config folder, not in the repository."))
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(4.0f)
					[
						SAssignNew(ApiKeyBox, SEditableTextBox)
						.IsPassword(true)
						.HintText(LOCTEXT("ApiKeyHint", "Gemini API key"))
						.Text(FText::FromString(LoadAiSetting(TEXT("GeminiApiKey"), FString())))
						.OnTextCommitted(this, &SLoreHistoryPanel::OnAiSettingCommitted, FString(TEXT("GeminiApiKey")))
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(4.0f)
					[
						SAssignNew(ModelBox, SEditableTextBox)
						.HintText(LOCTEXT("ModelHint", "Model"))
						.Text(FText::FromString(LoadAiSetting(TEXT("GeminiModel"), DefaultGeminiModel)))
						.OnTextCommitted(this, &SLoreHistoryPanel::OnAiSettingCommitted, FString(TEXT("GeminiModel")))
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(4.0f)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 12.0f, 0.0f)
						[
							SNew(STextBlock).Text(LOCTEXT("LanguageLabel", "Description language"))
						]
						+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 12.0f, 0.0f)
						[
							MakeLanguageRadio(TEXT("nl"), LOCTEXT("LanguageDutch", "Nederlands"))
						]
						+ SHorizontalBox::Slot().AutoWidth()
						[
							MakeLanguageRadio(TEXT("en"), LOCTEXT("LanguageEnglish", "English"))
						]
					]
				]
			];
	}

	TSharedRef<SWidget> MakeLanguageRadio(const FString& InCode, const FText& InLabel)
	{
		return SNew(SCheckBox)
			.Style(&FAppStyle::Get().GetWidgetStyle<FCheckBoxStyle>("RadioButton"))
			.IsChecked_Lambda([this, InCode]() { return Language == InCode ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
			.OnCheckStateChanged_Lambda([this, InCode](ECheckBoxState InState)
			{
				if (InState == ECheckBoxState::Checked)
				{
					Language = InCode;
					SaveAiSetting(TEXT("Language"), InCode);
				}
			})
			[
				SNew(STextBlock).Text(InLabel)
			];
	}

	void OnAiSettingCommitted(const FText& InText, ETextCommit::Type, FString InKey)
	{
		SaveAiSetting(*InKey, InText.ToString().TrimStartAndEnd());
	}

	bool AllSelected() const
	{
		for (const TSharedPtr<FLoreLocalChange>& Change : Changes)
		{
			if (!Change->bSelected)
			{
				return false;
			}
		}
		return !Changes.IsEmpty();
	}

	TSharedRef<ITableRow> GenerateChangeRow(TSharedPtr<FLoreLocalChange> Change, const TSharedRef<STableViewBase>& OwnerTable)
	{
		return SNew(STableRow<TSharedPtr<FLoreLocalChange>>, OwnerTable)
			.Padding(FMargin(8.0f, 2.0f))
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 8.0f, 0.0f)
				[
					SNew(SCheckBox)
					.IsChecked_Lambda([Change]() { return Change->bSelected ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
					.OnCheckStateChanged_Lambda([Change](ECheckBoxState InState) { Change->bSelected = (InState == ECheckBoxState::Checked); })
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 8.0f, 0.0f)
				[
					MakeStateIcon(Change->Status)
				]
				+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
				[
					SNew(SVerticalBox)
					.ToolTipText(FText::FromString(Change->Path))
					+ SVerticalBox::Slot().AutoHeight()
					[
						SNew(STextBlock).Text(FText::FromString(FPaths::GetCleanFilename(Change->Path)))
					]
					+ SVerticalBox::Slot().AutoHeight()
					[
						SNew(STextBlock).ColorAndOpacity(FSlateColor::UseSubduedForeground()).Text(FText::FromString(FPaths::GetPath(Change->Path)))
					]
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8.0f, 0.0f)
				[
					SNew(STextBlock).ColorAndOpacity(StatusColor(Change->Status)).Text(StateWord(Change->Status))
				]
			];
	}

	TArray<FString> SelectedChangePaths() const
	{
		TArray<FString> Paths;
		for (const TSharedPtr<FLoreLocalChange>& Change : Changes)
		{
			if (Change->bSelected)
			{
				Paths.Add(Change->Path);
			}
		}
		return Paths;
	}

	bool CanSubmit() const
	{
		return !DescriptionBox->GetText().IsEmptyOrWhitespace() && !SelectedChangePaths().IsEmpty();
	}

	FReply OnSubmit()
	{
		const TArray<FString> Paths = SelectedChangePaths();
		const FString Description = DescriptionBox->GetText().ToString().TrimStartAndEnd();

		FString Error;
		bool bOk = false;
		{
			FScopedSlowTask SlowTask(1.0f, LOCTEXT("Submitting", "Submitting to Lore..."));
			SlowTask.MakeDialog();
			SlowTask.EnterProgressFrame(1.0f);
			bOk = SubmitLocalChanges(Paths, Description, Error, bKeepLocked);
		}
		if (bOk)
		{
			DescriptionBox->SetText(FText::GetEmpty());
			GenerateStatus->SetText(FText::Format(LOCTEXT("Submitted", "Submitted {0} file(s)."), FText::AsNumber(Paths.Num())));
		}
		else
		{
			Notify(FText::Format(LOCTEXT("SubmitFailed", "Submit did not work:\n{0}"), FText::FromString(Error)));
		}
		Reload();
		return FReply::Handled();
	}

	FReply OnGenerate()
	{
		if (bGenerating)
		{
			return FReply::Handled();
		}
		const FString Key = ApiKeyBox->GetText().ToString().TrimStartAndEnd();
		if (Key.IsEmpty())
		{
			GenerateStatus->SetText(LOCTEXT("NeedKey", "Enter your Gemini API key under 'Gemini settings' first."));
			return FReply::Handled();
		}
		SaveAiSetting(TEXT("GeminiApiKey"), Key);
		const FString Model = ModelBox->GetText().ToString().TrimStartAndEnd().IsEmpty() ? FString(DefaultGeminiModel) : ModelBox->GetText().ToString().TrimStartAndEnd();

		FString UserText = TEXT("Changed files (A = added, M = modified, D = deleted):\n");
		int32 Listed = 0;
		for (const TSharedPtr<FLoreLocalChange>& Change : Changes)
		{
			if (Change->bSelected && Listed < 150)
			{
				UserText += Change->Status + TEXT(" ") + Change->Path + TEXT("\n");
				++Listed;
			}
		}
		if (Listed == 0)
		{
			GenerateStatus->SetText(LOCTEXT("NoSelection", "Select at least one changed file first."));
			return FReply::Handled();
		}
		if (Revisions.Num() > 0)
		{
			UserText += TEXT("\nRecent descriptions in this project (for style only):\n");
			for (int32 Index = 0; Index < FMath::Min(5, Revisions.Num()); ++Index)
			{
				UserText += TEXT("- ") + Revisions[Index]->Description + TEXT("\n");
			}
		}

		const FString LanguageRule = Language == TEXT("nl")
			? TEXT("Write in Dutch. Use the past participle, e.g. 'toegevoegd', 'aangepast', 'bijgewerkt', 'verwijderd', so each line reads as 'X toegevoegd'.")
			: TEXT("Write in English. Use the past tense, e.g. 'Added', 'Updated', 'Removed', 'Fixed'.");
		const FString SystemText = TEXT("You write version control descriptions for an Unreal Engine game project. The description is a record of what was done in this version, never an instruction or a to-do: describe changes that have been made, not changes to make (so 'Added X', never 'Add X'). Reply with one short summary line (maximum 72 characters) and, only if useful, up to three short lines starting with '- '. Use only what the file paths and change types show; do not invent details. Plain text only: no markdown, no quotes, no preamble. ") + LanguageRule;

		const auto MakeTextParts = [](const FString& InText)
		{
			TSharedPtr<FJsonObject> Part = MakeShared<FJsonObject>();
			Part->SetStringField(TEXT("text"), InText);
			TArray<TSharedPtr<FJsonValue>> Parts;
			Parts.Add(MakeShared<FJsonValueObject>(Part));
			return Parts;
		};

		TSharedPtr<FJsonObject> SystemInstruction = MakeShared<FJsonObject>();
		SystemInstruction->SetArrayField(TEXT("parts"), MakeTextParts(SystemText));

		TSharedPtr<FJsonObject> UserContent = MakeShared<FJsonObject>();
		UserContent->SetStringField(TEXT("role"), TEXT("user"));
		UserContent->SetArrayField(TEXT("parts"), MakeTextParts(UserText));
		TArray<TSharedPtr<FJsonValue>> Contents;
		Contents.Add(MakeShared<FJsonValueObject>(UserContent));

		TSharedPtr<FJsonObject> GenerationConfig = MakeShared<FJsonObject>();
		GenerationConfig->SetNumberField(TEXT("maxOutputTokens"), 800);

		TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
		Body->SetObjectField(TEXT("systemInstruction"), SystemInstruction);
		Body->SetArrayField(TEXT("contents"), Contents);
		Body->SetObjectField(TEXT("generationConfig"), GenerationConfig);

		FString BodyText;
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&BodyText);
		FJsonSerializer::Serialize(Body.ToSharedRef(), Writer);

		const TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
		Request->SetURL(FString::Printf(TEXT("https://generativelanguage.googleapis.com/v1beta/models/%s:generateContent"), *Model));
		Request->SetVerb(TEXT("POST"));
		Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
		Request->SetHeader(TEXT("x-goog-api-key"), Key);
		Request->SetContentAsString(BodyText);
		Request->OnProcessRequestComplete().BindSP(this, &SLoreHistoryPanel::OnGenerated);

		bGenerating = true;
		GenerateStatus->SetText(LOCTEXT("Generating", "Generating..."));
		UE_LOG(LogLoreSourceControl, Log, TEXT("LoreHistory: asking Gemini (%s) to describe %d file(s)"), *Model, Listed);
		Request->ProcessRequest();
		return FReply::Handled();
	}

	void OnGenerated(FHttpRequestPtr, FHttpResponsePtr InResponse, bool bSucceeded)
	{
		bGenerating = false;
		if (!bSucceeded || !InResponse.IsValid())
		{
			GenerateStatus->SetText(LOCTEXT("GenerateNoConnection", "Could not reach Gemini. Check your internet connection."));
			return;
		}

		TSharedPtr<FJsonObject> Json;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(InResponse->GetContentAsString());
		if (!FJsonSerializer::Deserialize(Reader, Json) || !Json.IsValid())
		{
			GenerateStatus->SetText(FText::Format(LOCTEXT("GenerateBadReply", "Unexpected reply from Gemini (HTTP {0})."), FText::AsNumber(InResponse->GetResponseCode())));
			return;
		}

		const TSharedPtr<FJsonObject>* ErrorObject = nullptr;
		if (Json->TryGetObjectField(TEXT("error"), ErrorObject))
		{
			FString Message;
			(*ErrorObject)->TryGetStringField(TEXT("message"), Message);
			UE_LOG(LogLoreSourceControl, Warning, TEXT("LoreHistory: Gemini error (HTTP %d): %s"), InResponse->GetResponseCode(), *Message);
			GenerateStatus->SetText(FText::Format(LOCTEXT("GenerateError", "Gemini error (HTTP {0}): {1}"), FText::AsNumber(InResponse->GetResponseCode()), FText::FromString(Message.Left(160))));
			return;
		}

		FString Generated;
		const TArray<TSharedPtr<FJsonValue>>* Candidates = nullptr;
		if (Json->TryGetArrayField(TEXT("candidates"), Candidates) && Candidates->Num() > 0)
		{
			const TSharedPtr<FJsonObject> Candidate = (*Candidates)[0]->AsObject();
			const TSharedPtr<FJsonObject>* Content = nullptr;
			if (Candidate.IsValid() && Candidate->TryGetObjectField(TEXT("content"), Content))
			{
				const TArray<TSharedPtr<FJsonValue>>* Parts = nullptr;
				if ((*Content)->TryGetArrayField(TEXT("parts"), Parts))
				{
					for (const TSharedPtr<FJsonValue>& Part : *Parts)
					{
						const TSharedPtr<FJsonObject> PartObject = Part->AsObject();
						FString Text;
						if (PartObject.IsValid() && PartObject->TryGetStringField(TEXT("text"), Text))
						{
							Generated += Text;
						}
					}
				}
			}
		}

		Generated = Generated.TrimStartAndEnd();
		if (Generated.IsEmpty())
		{
			GenerateStatus->SetText(LOCTEXT("GenerateEmpty", "Gemini returned no text. Try again."));
			return;
		}
		DescriptionBox->SetText(FText::FromString(Generated));
		GenerateStatus->SetText(LOCTEXT("GenerateDone", "Generated. Edit it if you like, then Submit."));
	}

	// ----- locks

	TSharedRef<SWidget> BuildLocksTab()
	{
		return SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(8.0f, 0.0f, 8.0f, 6.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
				[
					SNew(STextBlock).ColorAndOpacity(FSlateColor::UseSubduedForeground())
						.Text(LOCTEXT("LocksHelp", "Files checked out (locked) by you or by others. A locked file cannot be changed by anyone else."))
				]
				+ SHorizontalBox::Slot().AutoWidth()
				[
					SNew(SButton).Text(LOCTEXT("UnlockAllMine", "Unlock all mine")).OnClicked(this, &SLoreHistoryPanel::OnUnlockAllMine)
				]
			]
			+ SVerticalBox::Slot().FillHeight(1.0f)
			[
				SAssignNew(LockList, SListView<TSharedPtr<FLoreLockInfo>>)
				.ListItemsSource(&Locks)
				.OnGenerateRow(this, &SLoreHistoryPanel::GenerateLockRow)
				.SelectionMode(ESelectionMode::None)
			];
	}

	bool IsMine(const FLoreLockInfo& InLock) const
	{
		const FContext Context = LoreSourceControlUtils::GetCommandContextFromModule();
		const FString User = Context.UserName.IsEmpty() ? LoreSourceControlUtils::DetectAuthenticatedUser(Context) : Context.UserName;
		return !User.IsEmpty() && InLock.Owner.Equals(User, ESearchCase::IgnoreCase);
	}

	TSharedRef<ITableRow> GenerateLockRow(TSharedPtr<FLoreLockInfo> Lock, const TSharedRef<STableViewBase>& OwnerTable)
	{
		const bool bMine = IsMine(*Lock);
		return SNew(STableRow<TSharedPtr<FLoreLockInfo>>, OwnerTable)
			.Padding(FMargin(8.0f, 3.0f))
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 8.0f, 0.0f)
				[
					MakeIcon("Icons.Lock", bMine ? FLinearColor(0.35f, 0.8f, 0.45f) : FLinearColor(0.95f, 0.55f, 0.2f))
				]
				+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
				[
					SNew(SVerticalBox)
					.ToolTipText(FText::FromString(Lock->Path))
					+ SVerticalBox::Slot().AutoHeight()
					[
						SNew(STextBlock).Text(FText::FromString(FPaths::GetCleanFilename(Lock->Path)))
					]
					+ SVerticalBox::Slot().AutoHeight()
					[
						SNew(STextBlock).ColorAndOpacity(FSlateColor::UseSubduedForeground()).Text(FText::FromString(FPaths::GetPath(Lock->Path)))
					]
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8.0f, 0.0f)
				[
					SNew(STextBlock).Font(FAppStyle::GetFontStyle("BoldFont")).Text(FText::Format(LOCTEXT("CheckedOutBy", "Checked out by {0}"), FText::FromString(Lock->Owner)))
				]
				+ SHorizontalBox::Slot().AutoWidth()
				[
					SNew(SButton).Text(LOCTEXT("Unlock", "Unlock")).OnClicked(this, &SLoreHistoryPanel::OnUnlock, Lock)
				]
			];
	}

	FReply OnUnlock(TSharedPtr<FLoreLockInfo> Lock)
	{
		FString Error;
		const bool bMine = IsMine(*Lock);
		if (!bMine && !Confirm(FText::Format(LOCTEXT("ConfirmUnlockOther", "This file is checked out by {0}. Unlock it anyway?\n\nTheir unsaved work on it may no longer be protected."), FText::FromString(Lock->Owner))))
		{
			return FReply::Handled();
		}
		if (!UnlockFile(*Lock, Error))
		{
			Notify(FText::Format(LOCTEXT("UnlockFailed", "Could not unlock:\n{0}"), FText::FromString(Error)));
		}
		else
		{
			RefreshSourceControlStates(LoreSourceControlUtils::GetCommandContextFromModule(), { Lock->Path });
		}
		Reload();
		return FReply::Handled();
	}

	FReply OnUnlockAllMine()
	{
		int32 Released = 0;
		FString LastError;
		for (const TSharedPtr<FLoreLockInfo>& Lock : Locks)
		{
			if (IsMine(*Lock))
			{
				FString Error;
				if (UnlockOwn(*Lock, Error))
				{
					++Released;
				}
				else
				{
					LastError = Error;
				}
			}
		}
		Notify(FText::Format(LOCTEXT("UnlockAllDone", "Released {0} of your locks.{1}"), FText::AsNumber(Released), FText::FromString(LastError.IsEmpty() ? FString() : TEXT("\nSome failed: ") + LastError)));
		Reload();
		return FReply::Handled();
	}

private:
	TArray<TSharedPtr<FLoreRevisionItem>> Revisions;
	TArray<TSharedPtr<FLoreRevisionItem>> Filtered;
	TArray<TSharedPtr<FLoreLocalChange>> Changes;
	TArray<TSharedPtr<FLoreLockInfo>> Locks;

	TSharedPtr<SListView<TSharedPtr<FLoreRevisionItem>>> HistoryList;
	TSharedPtr<SListView<TSharedPtr<FLoreLocalChange>>> ChangeList;
	TSharedPtr<SListView<TSharedPtr<FLoreLockInfo>>> LockList;
	TSharedPtr<SWidgetSwitcher> Switcher;
	TSharedPtr<STextBlock> StatusBlock;
	TSharedPtr<STextBlock> BranchBlock;
	TSharedPtr<SEditableTextBox> FilterBox;

	TSharedPtr<SMultiLineEditableTextBox> DescriptionBox;
	TSharedPtr<STextBlock> GenerateStatus;
	TSharedPtr<SEditableTextBox> ApiKeyBox;
	TSharedPtr<SEditableTextBox> ModelBox;

	FString FilterText;
	FString Language = TEXT("en");
	int32 ActiveTab = 0;
	int32 BehindCount = 0;
	bool bGenerating = false;
	bool bKeepLocked = false;
};

// ------------------------------------------------------------------------- registration

namespace LoreHistoryTab
{
	static TSharedRef<SDockTab> SpawnTab(const FSpawnTabArgs& InArgs)
	{
		return SNew(SDockTab)
			.TabRole(ETabRole::NomadTab)
			[
				SNew(SLoreHistoryPanel)
			];
	}

	void Register()
	{
		FGlobalTabmanager::Get()->RegisterNomadTabSpawner(LoreHistoryTabName, FOnSpawnTab::CreateStatic(&SpawnTab))
			.SetDisplayName(LOCTEXT("TabTitle", "Lore History"))
			.SetTooltipText(LOCTEXT("TabTooltip", "Revision history, restore and locked files for the Lore repository."))
			.SetGroup(WorkspaceMenu::GetMenuStructure().GetDeveloperToolsMiscCategory());
	}

	void Unregister()
	{
		if (FSlateApplication::IsInitialized())
		{
			FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(LoreHistoryTabName);
		}
	}

	void Open()
	{
		FGlobalTabmanager::Get()->TryInvokeTab(LoreHistoryTabName);
	}
}

#undef LOCTEXT_NAMESPACE
