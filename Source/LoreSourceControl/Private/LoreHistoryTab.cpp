#include "LoreHistoryTab.h"
#include "LoreSourceControlUtils.h"
#include "LoreSourceControlLog.h"
#include "LoreTypes.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Brushes/SlateColorBrush.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "Misc/App.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Framework/Docking/TabManager.h"
#include "Dom/JsonObject.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformApplicationMisc.h"
#include "InputCoreTypes.h"
#include "HAL/PlatformProcess.h"
#include "Fonts/FontMeasure.h"
#include "Rendering/DrawElements.h"
#include "Rendering/SlateRenderer.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Colors/SColorPicker.h"
#include "Widgets/SLeafWidget.h"
#include "Interfaces/IPluginManager.h"
#include "LoreSourceControlModule.h"
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
	const FString PluginRepositoryUrl(TEXT("https://github.com/epicaleric/lore-plugin-ue-improved"));

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
		bool bExpanded = false;
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

	/** Look and feel (accent and avatar colour) is also a per-user choice. */
	const TCHAR* const UiSection = TEXT("LoreSourceControl.UI");

	FString LoadUiSetting(const TCHAR* InKey, const FString& InDefault)
	{
		FString Value;
		GConfig->GetString(UiSection, InKey, Value, GEditorPerProjectIni);
		return Value.IsEmpty() ? InDefault : Value;
	}

	void SaveUiSetting(const TCHAR* InKey, const FString& InValue)
	{
		GConfig->SetString(UiSection, InKey, *InValue, GEditorPerProjectIni);
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
		// Lore keeps real line breaks in a revision description, so bullet lists survive.
		InMessage.ReplaceInline(TEXT("\r\n"), TEXT("\n"));
		InMessage.ReplaceInline(TEXT("\r"), TEXT("\n"));
		InMessage.ReplaceInline(TEXT("\""), TEXT("\\\""));
		return FString::Printf(TEXT("\"%s\""), *InMessage);
	}

	/** The description on one line, for places that have room for only a sentence. */
	FString FlattenDescription(const FString& InDescription)
	{
		FString Flat = InDescription;
		Flat.ReplaceInline(TEXT("\r"), TEXT(""));
		Flat.ReplaceInline(TEXT("\n"), TEXT(" "));
		return Flat;
	}

	FString ShortDescription(const FString& InDescription)
	{
		const FString Flat = FlattenDescription(InDescription);
		return Flat.Len() > 80 ? Flat.Left(77) + TEXT("...") : Flat;
	}

	/** First line of a description, shown beside a collapsed revision. */
	FString FirstDescriptionLine(const FString& InDescription)
	{
		int32 Break = INDEX_NONE;
		if (InDescription.FindChar(TEXT('\n'), Break))
		{
			return InDescription.Left(Break).TrimEnd();
		}
		const int32 Dash = InDescription.Find(TEXT(" - "));
		return Dash == INDEX_NONE ? InDescription : InDescription.Left(Dash).TrimEnd();
	}

	/** A readable layout: the summary first, then one bullet per line. Older revisions stored it all on one line. */
	FString LayoutDescription(const FString& InDescription)
	{
		FString Text = InDescription;
		if (!Text.Contains(TEXT("\n")))
		{
			Text.ReplaceInline(TEXT(" - "), TEXT("\n- "));
		}
		TArray<FString> Lines;
		Text.ParseIntoArrayLines(Lines, false);
		FString Out;
		for (FString& Line : Lines)
		{
			Line.TrimEndInline();
			if (Line.StartsWith(TEXT("- ")) || Line.StartsWith(TEXT("* ")))
			{
				Line = FString(TEXT("• ")) + Line.Mid(2).TrimStart();
			}
			if (!Out.IsEmpty())
			{
				Out += TEXT("\n");
			}
			Out += Line;
		}
		return Out;
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
						Current->Description += TEXT("\n");
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

// ------------------------------------------------------------------------- design system

namespace
{
	// Brush colors are linear, so these small values display as dark greys.
	// The accent colour can be changed on the Settings tab, so it and the brushes that use it are not const.
	FLinearColor Accent(0.05f, 0.35f, 0.38f, 1.0f);
	const FLinearColor Good(0.25f, 0.78f, 0.40f, 1.0f);
	const FLinearColor Warn(0.95f, 0.70f, 0.20f, 1.0f);
	const FLinearColor Bad(0.90f, 0.30f, 0.30f, 1.0f);

	const FSlateRoundedBoxBrush CardBrush(FLinearColor(0.006f, 0.006f, 0.008f, 1.0f), 6.0f);
	const FSlateRoundedBoxBrush PillBrush(FLinearColor(0.020f, 0.020f, 0.024f, 1.0f), 12.0f);
	FSlateRoundedBoxBrush AccentPillBrush(Accent, 9.0f);
	const FSlateRoundedBoxBrush AvatarBrush(FLinearColor::White, 14.0f);
	FSlateRoundedBoxBrush DotBrush(Accent, 4.0f);
	FSlateRoundedBoxBrush HeadBrush(Accent, 10.0f);
	const FSlateRoundedBoxBrush SwatchBrush(FLinearColor::White, 11.0f);
	const FSlateRoundedBoxBrush SwatchRingBrush(FLinearColor::White, 14.0f);

	struct FThemeColor
	{
		const TCHAR* Id;
		FText Label;
		FColor Srgb;
	};

	const TArray<FThemeColor>& ThemeColors()
	{
		static const TArray<FThemeColor> Colors = {
			{ TEXT("teal"),    LOCTEXT("ColorTeal", "Teal"),       FColor(20, 160, 165) },
			{ TEXT("ocean"),   LOCTEXT("ColorOcean", "Ocean"),     FColor(47, 125, 225) },
			{ TEXT("violet"),  LOCTEXT("ColorViolet", "Violet"),   FColor(150, 100, 220) },
			{ TEXT("emerald"), LOCTEXT("ColorEmerald", "Emerald"), FColor(40, 170, 95) },
			{ TEXT("amber"),   LOCTEXT("ColorAmber", "Amber"),     FColor(225, 150, 30) },
			{ TEXT("rose"),    LOCTEXT("ColorRose", "Rose"),       FColor(225, 75, 115) },
			{ TEXT("slate"),   LOCTEXT("ColorSlate", "Slate"),     FColor(115, 130, 155) },
		};
		return Colors;
	}

	const FThemeColor* FindThemeColor(const FString& InId)
	{
		for (const FThemeColor& Color : ThemeColors())
		{
			if (InId == Color.Id)
			{
				return &Color;
			}
		}
		return nullptr;
	}

	/** Avatar colour for the signed-in user; empty id means "pick one from the name". */
	FString OwnAvatarColorId;
	FString OwnUserName;

	/** A saved colour is either a preset id ("teal") or a hex value ("1AA0A5"). */
	bool ParseColorValue(const FString& InValue, FColor& OutColor)
	{
		if (const FThemeColor* Preset = FindThemeColor(InValue))
		{
			OutColor = Preset->Srgb;
			return true;
		}
		if (InValue.Len() != 6)
		{
			return false;
		}
		for (const TCHAR Char : InValue)
		{
			if (!FChar::IsHexDigit(Char))
			{
				return false;
			}
		}
		OutColor = FColor::FromHex(InValue);
		return true;
	}

	FString ColorToValue(const FLinearColor& InColor)
	{
		return InColor.ToFColor(true).ToHex().Left(6);
	}

	FLinearColor ColorFromValue(const FString& InValue, const FLinearColor& InFallback)
	{
		FColor Parsed;
		return ParseColorValue(InValue, Parsed) ? FLinearColor::FromSRGBColor(Parsed) : InFallback;
	}

	/** Dark text on light backgrounds, white text on dark ones, so letters stay readable on any colour. */
	FSlateColor ContrastFor(const FLinearColor& InBackground)
	{
		const float Luminance = 0.2126f * InBackground.R + 0.7152f * InBackground.G + 0.0722f * InBackground.B;
		return Luminance > 0.33f ? FSlateColor(FLinearColor(0.01f, 0.01f, 0.012f, 1.0f)) : FSlateColor(FLinearColor::White);
	}

	void ApplyAccent(const FString& InValue)
	{
		Accent = ColorFromValue(InValue, FLinearColor::FromSRGBColor(ThemeColors()[0].Srgb));
		AccentPillBrush.TintColor = FSlateColor(Accent);
		DotBrush.TintColor = FSlateColor(Accent);
		HeadBrush.TintColor = FSlateColor(Accent);
	}

	/** Lighter version of the accent, used for headings so they stand out from the text below. */
	FLinearColor HeadingColor()
	{
		return FMath::Lerp(Accent, FLinearColor::White, 0.65f);
	}

	void LoadTheme()
	{
		ApplyAccent(LoadUiSetting(TEXT("Accent"), TEXT("teal")));
		OwnAvatarColorId = LoadUiSetting(TEXT("AvatarColor"), FString());
	}
	const FSlateRoundedBoxBrush GoodDotBrush(Good, 5.0f);
	const FSlateRoundedBoxBrush BadDotBrush(Bad, 5.0f);
	const FSlateColorBrush StripeA(FLinearColor(0.003f, 0.003f, 0.004f, 1.0f));
	const FSlateColorBrush StripeB(FLinearColor(0.007f, 0.007f, 0.009f, 1.0f));
	const FSlateColorBrush HoverBrush(FLinearColor(0.030f, 0.030f, 0.040f, 1.0f));
	const FSlateColorBrush ErrorBarBrush(FLinearColor(0.45f, 0.0f, 0.0f, 1.0f));

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
		if (InStatus == TEXT("D")) { return MakeIcon("Icons.Delete", Bad); }
		return MakeIcon("Icons.Edit", Warn);
	}

	FText StateWord(const FString& InStatus)
	{
		if (InStatus == TEXT("A")) { return LOCTEXT("StateAdded", "Added"); }
		if (InStatus == TEXT("D")) { return LOCTEXT("StateDeleted", "Deleted"); }
		return LOCTEXT("StateModified", "Modified");
	}

	/** What kind of asset a path is, with the colour UEFN uses for that kind. */
	struct FAssetKind
	{
		FText Label;
		FLinearColor Color;
	};

	FAssetKind ClassifyAsset(const FString& InPath)
	{
		const FString Name = FPaths::GetBaseFilename(InPath);
		const FString Ext = FPaths::GetExtension(InPath).ToLower();
		if (Ext == TEXT("umap")) { return { LOCTEXT("KindLevel", "Level"), FLinearColor(0.35f, 0.35f, 1.0f) }; }
		if (InPath.Contains(TEXT("__ExternalActors__"))) { return { LOCTEXT("KindActor", "Level Actor"), FLinearColor(0.45f, 0.55f, 0.95f) }; }
		if (InPath.Contains(TEXT("__ExternalObjects__"))) { return { LOCTEXT("KindObject", "Level Object"), FLinearColor(0.45f, 0.55f, 0.95f) }; }
		if (Ext != TEXT("uasset"))
		{
			if (Ext == TEXT("dll") || Ext == TEXT("pdb") || Ext == TEXT("exe")) { return { LOCTEXT("KindBinary", "Binary"), FLinearColor(0.6f, 0.45f, 0.35f) }; }
			if (Ext == TEXT("ini") || Ext == TEXT("uplugin") || Ext == TEXT("uproject") || Ext == TEXT("uefnproject")) { return { LOCTEXT("KindConfig", "Config"), FLinearColor(0.7f, 0.7f, 0.3f) }; }
			return { LOCTEXT("KindFile", "File"), FLinearColor(0.6f, 0.6f, 0.65f) };
		}
		if (Name.StartsWith(TEXT("BP_")) || Name.StartsWith(TEXT("WBP_")) || Name.StartsWith(TEXT("ABP_"))) { return { LOCTEXT("KindBlueprint", "Blueprint Class"), FLinearColor(0.40f, 0.45f, 1.0f) }; }
		if (Name.StartsWith(TEXT("MF_"))) { return { LOCTEXT("KindMaterialFunction", "Material Function"), FLinearColor(1.0f, 0.60f, 0.10f) }; }
		if (Name.StartsWith(TEXT("MI_"))) { return { LOCTEXT("KindMaterialInstance", "Material Instance"), FLinearColor(0.35f, 0.65f, 1.0f) }; }
		if (Name.StartsWith(TEXT("M_"))) { return { LOCTEXT("KindMaterial", "Material"), FLinearColor(0.35f, 0.60f, 1.0f) }; }
		if (Name.StartsWith(TEXT("NS_"))) { return { LOCTEXT("KindNiagara", "Niagara System"), FLinearColor(0.30f, 0.90f, 0.40f) }; }
		if (Name.StartsWith(TEXT("T_"))) { return { LOCTEXT("KindTexture", "Texture"), FLinearColor(1.0f, 0.30f, 0.35f) }; }
		if (Name.StartsWith(TEXT("SM_"))) { return { LOCTEXT("KindStaticMesh", "Static Mesh"), FLinearColor(0.30f, 0.85f, 0.90f) }; }
		if (Name.StartsWith(TEXT("SK_"))) { return { LOCTEXT("KindSkeletalMesh", "Skeletal Mesh"), FLinearColor(0.30f, 0.85f, 0.90f) }; }
		return { LOCTEXT("KindAsset", "Asset"), FLinearColor(0.60f, 0.60f, 0.70f) };
	}

	/** "Content/Foo/Bar.uasset" -> "/Game/Foo/Bar"; empty when the path is not a package. */
	FString PackageNameFromPath(const FString& InPath)
	{
		FString Path = InPath;
		Path.ReplaceInline(TEXT("\\"), TEXT("/"));
		const bool bMap = Path.EndsWith(TEXT(".umap"));
		if (!bMap && !Path.EndsWith(TEXT(".uasset")))
		{
			return FString();
		}
		Path = Path.LeftChop(bMap ? 5 : 7);
		if (Path.StartsWith(TEXT("Content/")))
		{
			return TEXT("/Game/") + Path.Mid(8);
		}
		if (Path.StartsWith(TEXT("Plugins/")))
		{
			const int32 ContentIndex = Path.Find(TEXT("/Content/"));
			if (ContentIndex != INDEX_NONE)
			{
				const FString PluginName = FPaths::GetCleanFilename(Path.Left(ContentIndex));
				return TEXT("/") + PluginName + TEXT("/") + Path.Mid(ContentIndex + 9);
			}
		}
		return FString();
	}

	/** An asset's readable name and kind. Level actors are stored under hash names, so their label comes from the asset registry. */
	struct FAssetDisplay
	{
		FString Name;
		FAssetKind Kind;
	};

	FAssetDisplay DescribeAsset(const FString& InPath)
	{
		FAssetDisplay Result;
		Result.Kind = ClassifyAsset(InPath);
		Result.Name = FPaths::GetBaseFilename(InPath);

		static TMap<FString, TPair<FString, FString>> Cache; // path -> (label, class); game thread only
		TPair<FString, FString> Found;
		if (const TPair<FString, FString>* Cached = Cache.Find(InPath))
		{
			Found = *Cached;
		}
		else
		{
			const FString Package = PackageNameFromPath(InPath);
			IAssetRegistry* Registry = Package.IsEmpty() ? nullptr : IAssetRegistry::Get();
			if (Registry)
			{
				TArray<FAssetData> Assets;
				Registry->GetAssetsByPackageName(FName(*Package), Assets, /*bIncludeOnlyOnDiskAssets=*/true);
				if (!Assets.IsEmpty())
				{
					FString Label;
					if (!Assets[0].GetTagValue(FName(TEXT("ActorLabel")), Label) || Label.IsEmpty())
					{
						Label = Assets[0].AssetName.ToString();
					}
					Found = TPair<FString, FString>(Label, Assets[0].AssetClassPath.GetAssetName().ToString());
					Cache.Add(InPath, Found);
				}
			}
		}

		if (!Found.Key.IsEmpty())
		{
			Result.Name = Found.Key;
		}
		if (InPath.Contains(TEXT("__ExternalActors__")) && !Found.Value.IsEmpty())
		{
			Result.Kind.Label = FText::FromString(FName::NameToDisplayString(Found.Value, false));
			if (Found.Value.Contains(TEXT("StaticMesh"))) { Result.Kind.Color = FLinearColor(0.30f, 0.85f, 0.90f); }
			else if (Found.Value.Contains(TEXT("Light"))) { Result.Kind.Color = FLinearColor(1.0f, 0.90f, 0.30f); }
			else if (Found.Value.Contains(TEXT("Blueprint")) || Found.Value.StartsWith(TEXT("BP_"))) { Result.Kind.Color = FLinearColor(0.40f, 0.45f, 1.0f); }
		}
		return Result;
	}

	/** The short coloured dash that marks an asset's kind. */
	TSharedRef<SWidget> MakeKindDash(const FAssetKind& InKind)
	{
		return SNew(SBox).WidthOverride(10.0f).HeightOverride(4.0f)
			[
				SNew(SBorder).BorderImage(FAppStyle::GetBrush("WhiteBrush")).BorderBackgroundColor(InKind.Color).Padding(0.0f)
			];
	}

	/** Parse "Wed, 4 Nov 2026 21:17:14 +0000" into local time. */
	bool ParseRevisionDate(const FString& InRaw, FDateTime& OutLocal)
	{
		TArray<FString> Tokens;
		InRaw.ParseIntoArrayWS(Tokens);
		if (Tokens.Num() < 5)
		{
			return false;
		}
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
		if (Month == 0 || Time.Num() != 3)
		{
			return false;
		}
		const FDateTime Utc(FCString::Atoi(*Tokens[3]), Month, FCString::Atoi(*Tokens[1]), FCString::Atoi(*Time[0]), FCString::Atoi(*Time[1]), FCString::Atoi(*Time[2]));
		OutLocal = Utc + (FDateTime::Now() - FDateTime::UtcNow());
		return true;
	}

	/** "29 Oct 2026, 17:16", the way UEFN writes it. */
	FString FormatUefnDate(const FString& InRaw)
	{
		FDateTime Local;
		if (!ParseRevisionDate(InRaw, Local))
		{
			return InRaw;
		}
		static const TCHAR* Months[] = { TEXT("Jan"), TEXT("Feb"), TEXT("Mar"), TEXT("Apr"), TEXT("May"), TEXT("Jun"), TEXT("Jul"), TEXT("Aug"), TEXT("Sep"), TEXT("Oct"), TEXT("Nov"), TEXT("Dec") };
		return FString::Printf(TEXT("%d %s %d, %02d:%02d"), Local.GetDay(), Months[Local.GetMonth() - 1], Local.GetYear(), Local.GetHour(), Local.GetMinute());
	}

	/** The colour other people's avatars get, derived from their name. */
	FLinearColor HashAvatarColor(const FString& InName)
	{
		const uint32 Hash = GetTypeHash(InName.ToLower());
		return FLinearColor::MakeFromHSV8(static_cast<uint8>(Hash % 256), 130, 200).CopyWithNewOpacity(1.0f);
	}

	FLinearColor AvatarColorFor(const FString& InName)
	{
		// The signed-in user can pick their own colour on the Settings tab.
		FColor Chosen;
		if (!OwnUserName.IsEmpty() && InName.Equals(OwnUserName, ESearchCase::IgnoreCase) && ParseColorValue(OwnAvatarColorId, Chosen))
		{
			return FLinearColor::FromSRGBColor(Chosen);
		}
		return HashAvatarColor(InName);
	}

	/** A round avatar with the author's initial. */
	TSharedRef<SWidget> MakeAvatar(const FString& InAuthor, float InSize = 24.0f)
	{
		const FString Initial = InAuthor.IsEmpty() ? FString(TEXT("?")) : InAuthor.Left(1).ToUpper();
		const FLinearColor Background = AvatarColorFor(InAuthor);
		return SNew(SBox).WidthOverride(InSize).HeightOverride(InSize)
			[
				SNew(SBorder).BorderImage(&AvatarBrush).BorderBackgroundColor(Background).HAlign(HAlign_Center).VAlign(VAlign_Center).Padding(0.0f)
				[
					SNew(STextBlock).Font(FAppStyle::GetFontStyle("BoldFont")).ColorAndOpacity(ContrastFor(Background)).Text(FText::FromString(Initial))
				]
			];
	}

	/** Pick a random colour for the signed-in user the first time, one that no other known author already has. */
	void EnsureOwnAvatarColor(const TArray<FString>& InOtherAuthors)
	{
		FColor Existing;
		if (OwnUserName.IsEmpty() || ParseColorValue(OwnAvatarColorId, Existing))
		{
			return;
		}
		TArray<float> TakenHues;
		for (const FString& Other : InOtherAuthors)
		{
			if (!Other.Equals(OwnUserName, ESearchCase::IgnoreCase))
			{
				TakenHues.Add(HashAvatarColor(Other).LinearRGBToHSV().R);
			}
		}
		float BestHue = FMath::FRand() * 360.0f;
		float BestDistance = -1.0f;
		for (int32 Try = 0; Try < 24; ++Try)
		{
			const float Hue = FMath::FRand() * 360.0f;
			float Nearest = 360.0f;
			for (const float Taken : TakenHues)
			{
				const float Delta = FMath::Abs(Hue - Taken);
				Nearest = FMath::Min(Nearest, FMath::Min(Delta, 360.0f - Delta));
			}
			if (Nearest > BestDistance)
			{
				BestDistance = Nearest;
				BestHue = Hue;
			}
		}
		const FLinearColor Picked = FLinearColor::MakeFromHSV8(static_cast<uint8>(BestHue / 360.0f * 255.0f), 150, 215);
		OwnAvatarColorId = ColorToValue(Picked);
		SaveUiSetting(TEXT("AvatarColor"), OwnAvatarColorId);
	}

	/** A small rounded count badge, hidden while the count is zero. */
	TSharedRef<SWidget> MakeBadge(TFunction<int32()> InCount)
	{
		return SNew(SBorder).BorderImage(&AccentPillBrush).Padding(FMargin(6.0f, 1.0f))
			.Visibility_Lambda([InCount]() { return InCount() > 0 ? EVisibility::Visible : EVisibility::Collapsed; })
			[
				SNew(STextBlock).Font(FAppStyle::GetFontStyle("SmallFont")).ColorAndOpacity_Lambda([]() { return ContrastFor(Accent); }).Text_Lambda([InCount]() { return FText::AsNumber(InCount()); })
			];
	}

	/** Section heading used inside cards. */
	TSharedRef<SWidget> MakeCardTitle(const FText& InTitle)
	{
		return SNew(STextBlock).Font(FCoreStyle::GetDefaultFontStyle("Bold", 12)).ColorAndOpacity_Lambda([]() { return FSlateColor(HeadingColor()); }).Text(InTitle);
	}

	// ------------------------------------------------------------------ data helpers

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

	/** First dotted number in a text, e.g. "lore 0.12.0+1967" or "v0.1.0-ue5.8" -> {0, 12, 0}. */
	TArray<int32> ParseVersionNumbers(const FString& InText)
	{
		TArray<int32> Numbers;
		int32 Index = 0;
		while (Index < InText.Len() && !FChar::IsDigit(InText[Index]))
		{
			++Index;
		}
		while (Index < InText.Len())
		{
			int32 Value = 0;
			bool bAny = false;
			while (Index < InText.Len() && FChar::IsDigit(InText[Index]))
			{
				Value = Value * 10 + (InText[Index] - TEXT('0'));
				bAny = true;
				++Index;
			}
			if (!bAny)
			{
				break;
			}
			Numbers.Add(Value);
			if (Index < InText.Len() && InText[Index] == TEXT('.') && Index + 1 < InText.Len() && FChar::IsDigit(InText[Index + 1]))
			{
				++Index;
			}
			else
			{
				break;
			}
		}
		return Numbers;
	}

	/** -1 when A is older than B, 0 when equal, 1 when newer. */
	int32 CompareVersions(const FString& InA, const FString& InB)
	{
		const TArray<int32> A = ParseVersionNumbers(InA);
		const TArray<int32> B = ParseVersionNumbers(InB);
		for (int32 Index = 0; Index < FMath::Max(A.Num(), B.Num()); ++Index)
		{
			const int32 Left = A.IsValidIndex(Index) ? A[Index] : 0;
			const int32 Right = B.IsValidIndex(Index) ? B[Index] : 0;
			if (Left != Right)
			{
				return Left < Right ? -1 : 1;
			}
		}
		return 0;
	}

	/** Result of looking up the newest GitHub release of something. */
	struct FReleaseInfo
	{
		FText Status;
		FString Latest;
		FString Url;
		bool bUpdateAvailable = false;
	};

	/** Facts shown on the Server tab. */
	struct FServerInfo
	{
		FString RemoteUrl;
		FString RepositoryName;
		FString RepositoryId;
		FString DefaultBranch;
		FString Creator;
		FString Created;
		FString ClientVersion;
		FString User;
		FString TokenExpires;
		FString LastChecked;
		bool bReachable = false;
		bool bHealthKnown = false;
		int32 LatencyMs = 0;
		int32 LocalRevision = 0;
		int32 RemoteRevision = 0;
	};

	void LoadServerInfo(const FContext& Context, FServerInfo& InOut)
	{
		FString Error;
		TArray<FString> Lines;
		if (RunLore(Context, TEXT("repository info"), TArray<FString>(), TArray<FString>(), Lines, Error))
		{
			for (int32 Index = 0; Index < Lines.Num(); ++Index)
			{
				const FString Line = Lines[Index].TrimStartAndEnd();
				int32 Paren = INDEX_NONE;
				if (Index == 0 && Line.FindChar(TEXT('('), Paren))
				{
					InOut.RepositoryName = Line.Left(Paren).TrimEnd();
					InOut.RepositoryId = Line.Mid(Paren + 1).Replace(TEXT(")"), TEXT("")).TrimStartAndEnd();
				}
				int32 Colon = INDEX_NONE;
				if (!Line.FindChar(TEXT(':'), Colon))
				{
					continue;
				}
				const FString Key = Line.Left(Colon).TrimStartAndEnd();
				const FString Value = Line.Mid(Colon + 1).TrimStartAndEnd();
				if (Key == TEXT("Remote URL")) { InOut.RemoteUrl = Value; }
				else if (Key == TEXT("Default branch")) { { int32 Open = Value.Find(TEXT(" (")); InOut.DefaultBranch = Open == INDEX_NONE ? Value : Value.Left(Open); } }
				else if (Key == TEXT("Creator")) { InOut.Creator = Value; }
				else if (Key == TEXT("Created")) { InOut.Created = FormatUefnDate(Value); }
			}
		}

		FString Version;
		LoreSourceControlUtils::CheckLoreAvailable(Context.BinaryPath, Version);
		InOut.ClientVersion = Version.TrimStartAndEnd();

		Lines.Reset();
		InOut.User = LoreSourceControlUtils::DetectAuthenticatedUser(Context);
		if (RunLore(Context, TEXT("auth list"), TArray<FString>(), TArray<FString>(), Lines, Error))
		{
			for (const FString& Raw : Lines)
			{
				const FString Line = Raw.TrimStartAndEnd();
				if (Line.StartsWith(TEXT("Expires:")) && InOut.TokenExpires.IsEmpty())
				{
					InOut.TokenExpires = FormatUefnDate(Line.Mid(8).TrimStartAndEnd());
				}
			}
		}
		InOut.LastChecked = FDateTime::Now().ToString(TEXT("%H:%M:%S"));
	}
}

// ------------------------------------------------------------------------- AI button

/** A pill with a gradient outline and sparkles, used to ask the AI for a description. */
class SLoreAiButton : public SLeafWidget
{
public:
	SLATE_BEGIN_ARGS(SLoreAiButton) : _IsBusy(false) {}
		SLATE_ATTRIBUTE(bool, IsBusy)
		SLATE_EVENT(FOnClicked, OnClicked)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs)
	{
		IsBusy = InArgs._IsBusy;
		OnClicked = InArgs._OnClicked;
		SetCursor(EMouseCursor::Hand);
	}

	virtual FVector2D ComputeDesiredSize(float) const override
	{
		return FVector2D(Width, Height);
	}

	virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect, FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const override
	{
		static const FSlateRoundedBoxBrush ButtonPill(FLinearColor::White, Height * 0.5f);
		const bool bBusy = IsBusy.Get(false);
		const float Fade = bBusy ? 0.55f : 1.0f;
		const FVector2f Size = AllottedGeometry.GetLocalSize();

		// A plain pill in the plugin's accent colour; text and sparkles use whichever of dark or light reads best on it.
		FLinearColor Fill = IsHovered() ? FMath::Lerp(Accent, FLinearColor::White, 0.18f) : Accent;
		Fill.A = Fade;
		FSlateDrawElement::MakeBox(OutDrawElements, LayerId, AllottedGeometry.ToPaintGeometry(), &ButtonPill, ESlateDrawEffect::None, Fill);

		const FLinearColor Ink = ContrastFor(Accent).GetSpecifiedColor().CopyWithNewOpacity(Fade);
		const FSlateFontInfo Font = FCoreStyle::GetDefaultFontStyle("Bold", 9);
		const FString Label = bBusy ? TEXT("Working...") : TEXT("Generate");
		const TSharedRef<FSlateFontMeasure> Measure = FSlateApplication::Get().GetRenderer()->GetFontMeasureService();
		const FVector2f TextSize = Measure->Measure(Label, Font);

		// Sparkles and text as one group, centred in the pill.
		constexpr float StarsWidth = 15.0f;
		constexpr float Gap = 6.0f;
		const float Left = FMath::RoundToFloat((Size.X - (StarsWidth + Gap + TextSize.X)) * 0.5f);
		DrawSparkle(OutDrawElements, LayerId + 1, AllottedGeometry, FVector2f(Left + 4.5f, Size.Y * 0.5f + 1.0f), 4.5f, Ink);
		DrawSparkle(OutDrawElements, LayerId + 1, AllottedGeometry, FVector2f(Left + 12.5f, Size.Y * 0.5f - 4.0f), 2.4f, Ink);
		FSlateDrawElement::MakeText(OutDrawElements, LayerId + 1,
			AllottedGeometry.ToPaintGeometry(TextSize, FSlateLayoutTransform(FVector2f(Left + StarsWidth + Gap, (Size.Y - TextSize.Y) * 0.5f))),
			Label, Font, ESlateDrawEffect::None, Ink);
		return LayerId + 1;
	}

	virtual FReply OnMouseButtonDown(const FGeometry&, const FPointerEvent& InEvent) override
	{
		return InEvent.GetEffectingButton() == EKeys::LeftMouseButton ? FReply::Handled().CaptureMouse(AsShared()) : FReply::Unhandled();
	}

	virtual FReply OnMouseButtonUp(const FGeometry& InGeometry, const FPointerEvent& InEvent) override
	{
		if (InEvent.GetEffectingButton() != EKeys::LeftMouseButton || !HasMouseCapture())
		{
			return FReply::Unhandled();
		}
		FReply Reply = FReply::Handled().ReleaseMouseCapture();
		if (InGeometry.IsUnderLocation(InEvent.GetScreenSpacePosition()) && OnClicked.IsBound())
		{
			Reply = OnClicked.Execute().ReleaseMouseCapture();
		}
		return Reply;
	}

private:
	static constexpr float Width = 96.0f;
	static constexpr float Height = 26.0f;

	static FLinearColor ShiftHue(const FLinearColor& InColor, float InDegrees)
	{
		FLinearColor Hsv = InColor.LinearRGBToHSV();
		Hsv.R = FMath::Fmod(Hsv.R + InDegrees + 360.0f, 360.0f);
		Hsv.G = FMath::Max(Hsv.G, 0.65f);
		Hsv.B = 1.0f;
		return Hsv.HSVToLinearRGB();
	}

	static void DrawSparkle(FSlateWindowElementList& OutDrawElements, int32 InLayer, const FGeometry& InGeometry, const FVector2f& InCenter, float InRadius, const FLinearColor& InColor)
	{
		for (int32 Step = 0; Step < 4; ++Step)
		{
			const float Outer = InRadius * (1.0f - 0.25f * Step);
			const float Inner = Outer * 0.30f;
			TArray<FVector2f> Points;
			Points.Add(InCenter + FVector2f(0.0f, -Outer));
			Points.Add(InCenter + FVector2f(Inner, -Inner));
			Points.Add(InCenter + FVector2f(Outer, 0.0f));
			Points.Add(InCenter + FVector2f(Inner, Inner));
			Points.Add(InCenter + FVector2f(0.0f, Outer));
			Points.Add(InCenter + FVector2f(-Inner, Inner));
			Points.Add(InCenter + FVector2f(-Outer, 0.0f));
			Points.Add(InCenter + FVector2f(-Inner, -Inner));
			Points.Add(InCenter + FVector2f(0.0f, -Outer));
			FSlateDrawElement::MakeLines(OutDrawElements, InLayer, InGeometry.ToPaintGeometry(), Points, ESlateDrawEffect::None, InColor, true, 1.3f);
		}
	}

	TAttribute<bool> IsBusy;
	FOnClicked OnClicked;
};

// ------------------------------------------------------------------------- file history window

class SLoreFileHistory : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SLoreFileHistory) {}
		SLATE_ARGUMENT(FString, Path)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs)
	{
		LoadTheme();
		const FContext Context = LoreSourceControlUtils::GetCommandContextFromModule();
		TArray<FString> Lines;
		FString Error;
		if (RunLore(Context, TEXT("file history"), TArray<FString>(), { InArgs._Path }, Lines, Error))
		{
			ParseHistory(Lines, Items);
		}

		ChildSlot
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(10.0f, 10.0f, 10.0f, 4.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 8.0f, 0.0f)
				[
					MakeIcon("Icons.Edit", Warn, 14.0f)
				]
				+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
				[
					SNew(STextBlock).Font(FAppStyle::GetFontStyle("BoldFont")).Text(FText::FromString(InArgs._Path))
				]
			]
			+ SVerticalBox::Slot().FillHeight(1.0f).Padding(10.0f, 4.0f)
			[
				SNew(SBorder).BorderImage(&CardBrush).Padding(FMargin(0.0f, 4.0f))
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight().Padding(10.0f, 2.0f)
					[
						MakeRow(LOCTEXT("ColRevision", "Revision"), LOCTEXT("ColDate", "Date Submitted"), LOCTEXT("ColBy", "Submitted By"), LOCTEXT("ColDescription", "Description"), true)
					]
					+ SVerticalBox::Slot().FillHeight(1.0f)
					[
						SAssignNew(List, SListView<TSharedPtr<FLoreRevisionItem>>)
						.ListItemsSource(&Items)
						.OnGenerateRow(this, &SLoreFileHistory::GenerateRow)
						.OnSelectionChanged(this, &SLoreFileHistory::OnSelected)
						.SelectionMode(ESelectionMode::Single)
					]
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(10.0f, 4.0f, 10.0f, 10.0f)
			[
				SNew(SBorder).BorderImage(&CardBrush).Padding(FMargin(12.0f))
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 6.0f)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().FillWidth(1.0f)[ MakeDetail(LOCTEXT("DetRevision", "Revision"), DetailRevision) ]
						+ SHorizontalBox::Slot().FillWidth(1.0f)[ MakeDetail(LOCTEXT("DetSigned", "Signature"), DetailSignature) ]
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 6.0f)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().FillWidth(1.0f)[ MakeDetail(LOCTEXT("DetDate", "Date Submitted"), DetailDate) ]
						+ SHorizontalBox::Slot().FillWidth(1.0f)[ MakeDetail(LOCTEXT("DetBy", "Submitted By"), DetailBy) ]
					]
					+ SVerticalBox::Slot().AutoHeight()
					[
						MakeDetail(LOCTEXT("DetDescription", "Description"), DetailDescription)
					]
				]
			]
		];

		if (!Items.IsEmpty())
		{
			List->SetSelection(Items[0]);
		}
	}

private:
	TSharedRef<SWidget> MakeRow(const FText& InA, const FText& InB, const FText& InC, const FText& InD, bool bHeader) const
	{
		const FSlateColor Color = bHeader ? FSlateColor::UseSubduedForeground() : FSlateColor::UseForeground();
		return SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 2.0f)[ SNew(SBox).WidthOverride(80.0f)[ SNew(STextBlock).ColorAndOpacity(Color).Text(InA) ] ]
			+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 2.0f)[ SNew(SBox).WidthOverride(180.0f)[ SNew(STextBlock).ColorAndOpacity(Color).Text(InB) ] ]
			+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 2.0f)[ SNew(SBox).WidthOverride(150.0f)[ SNew(STextBlock).ColorAndOpacity(Color).Text(InC) ] ]
			+ SHorizontalBox::Slot().FillWidth(1.0f).Padding(0.0f, 2.0f)[ SNew(STextBlock).ColorAndOpacity(Color).Text(InD) ];
	}

	TSharedRef<SWidget> MakeDetail(const FText& InLabel, const FString& InValueRef) const
	{
		return SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 10.0f, 0.0f)
			[
				SNew(SBox).WidthOverride(100.0f)[ SNew(STextBlock).ColorAndOpacity(FSlateColor::UseSubduedForeground()).Text(InLabel) ]
			]
			+ SHorizontalBox::Slot().FillWidth(1.0f)
			[
				SNew(STextBlock).AutoWrapText(true).Text_Lambda([&InValueRef]() { return FText::FromString(InValueRef); })
			];
	}

	TSharedRef<ITableRow> GenerateRow(TSharedPtr<FLoreRevisionItem> Item, const TSharedRef<STableViewBase>& OwnerTable)
	{
		return SNew(STableRow<TSharedPtr<FLoreRevisionItem>>, OwnerTable)
			.Padding(FMargin(10.0f, 1.0f))
			[
				MakeRow(FText::AsNumber(Item->Number), FText::FromString(FormatUefnDate(Item->Date)), FText::FromString(Item->Author), FText::FromString(FirstDescriptionLine(Item->Description)), false)
			];
	}

	void OnSelected(TSharedPtr<FLoreRevisionItem> Item, ESelectInfo::Type)
	{
		if (!Item.IsValid())
		{
			return;
		}
		DetailRevision = FString::FromInt(Item->Number);
		DetailSignature = Item->Signature.Left(16) + TEXT("...");
		DetailDate = FormatUefnDate(Item->Date);
		DetailBy = Item->Author;
		DetailDescription = LayoutDescription(Item->Description);
	}

	TArray<TSharedPtr<FLoreRevisionItem>> Items;
	TSharedPtr<SListView<TSharedPtr<FLoreRevisionItem>>> List;
	FString DetailRevision;
	FString DetailSignature;
	FString DetailDate;
	FString DetailBy;
	FString DetailDescription;
};

namespace
{
	void OpenFileHistoryWindow(const FString& InPath)
	{
		const TSharedRef<SWindow> Window = SNew(SWindow)
			.Title(LOCTEXT("FileHistoryTitle", "File History"))
			.ClientSize(FVector2D(900.0f, 520.0f))
			[
				SNew(SLoreFileHistory).Path(InPath)
			];
		FSlateApplication::Get().AddWindow(Window);
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
		Language = LoadAiSetting(TEXT("Language"), TEXT("English"));
		if (Language == TEXT("nl")) { Language = TEXT("Nederlands"); }
		else if (Language == TEXT("en")) { Language = TEXT("English"); }
		LoadTheme();

		ChildSlot
		[
			SNew(SVerticalBox)
			// Tab bar
			+ SVerticalBox::Slot().AutoHeight().Padding(8.0f, 6.0f, 8.0f, 0.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth()[ MakeTab(LOCTEXT("TabHistory", "History"), 0, nullptr) ]
				+ SHorizontalBox::Slot().AutoWidth()[ MakeTab(LOCTEXT("TabChanges", "Changes"), 1, [this]() { return Changes.Num(); }) ]
				+ SHorizontalBox::Slot().AutoWidth()[ MakeTab(LOCTEXT("TabLocks", "Locks"), 2, [this]() { return Locks.Num(); }) ]
				+ SHorizontalBox::Slot().AutoWidth()[ MakeTab(LOCTEXT("TabServer", "Server"), 3, nullptr) ]
				+ SHorizontalBox::Slot().AutoWidth()[ MakeTab(LOCTEXT("TabSettings", "Settings"), 4, nullptr) ]
				+ SHorizontalBox::Slot().FillWidth(1.0f)[ SNew(SSpacer) ]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 8.0f, 4.0f)
				[
					SNew(SBorder).BorderImage(&PillBrush).Padding(FMargin(12.0f, 7.0f))
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 6.0f, 0.0f)
						[
							MakeIcon("Icons.Check", Good, 14.0f)
						]
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
						[
							SAssignNew(BranchBlock, STextBlock).Text(LOCTEXT("BranchDefault", "main"))
						]
					]
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 0.0f, 4.0f)
				[
					SNew(SButton).ButtonStyle(&FAppStyle::Get().GetWidgetStyle<FButtonStyle>("SimpleButton"))
						.OnClicked(this, &SLoreHistoryPanel::OnRefresh).ToolTipText(LOCTEXT("RefreshTip", "Reload revisions, changes, locks and server info"))
					[
						MakeIcon("Icons.Refresh", FLinearColor::White, 16.0f)
					]
				]
			]
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(SBorder).BorderImage(FAppStyle::GetBrush("WhiteBrush")).BorderBackgroundColor(FLinearColor(0.02f, 0.02f, 0.025f)).Padding(0.0f)
				[
					SNew(SSpacer).Size(FVector2D(1.0f, 1.0f))
				]
			]
			// Sync banner
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(SBorder).BorderImage(FAppStyle::GetBrush("WhiteBrush")).BorderBackgroundColor(FLinearColor(0.12f, 0.07f, 0.0f)).Padding(FMargin(12.0f, 6.0f))
				.Visibility_Lambda([this]() { return BehindCount > 0 ? EVisibility::Visible : EVisibility::Collapsed; })
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 8.0f, 0.0f)
					[
						MakeIcon("Icons.Download", Warn, 14.0f)
					]
					+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
					[
						SNew(STextBlock).Text_Lambda([this]() { return FText::Format(LOCTEXT("BehindText", "The server has {0} newer revision(s). Your project is not up to date."), FText::AsNumber(BehindCount)); })
					]
					+ SHorizontalBox::Slot().AutoWidth()
					[
						SNew(SButton).Text(LOCTEXT("SyncLatestButton", "Sync Latest")).OnClicked(this, &SLoreHistoryPanel::OnSyncLatest)
							.ToolTipText(LOCTEXT("SyncLatestTip", "Download the newest revisions from the server into your project"))
					]
				]
			]
			+ SVerticalBox::Slot().FillHeight(1.0f)
			[
				SAssignNew(Switcher, SWidgetSwitcher)
				.WidgetIndex(0)
				+ SWidgetSwitcher::Slot()[ BuildHistoryTab() ]
				+ SWidgetSwitcher::Slot()[ BuildChangesTab() ]
				+ SWidgetSwitcher::Slot()[ BuildLocksTab() ]
				+ SWidgetSwitcher::Slot()[ BuildServerTab() ]
				+ SWidgetSwitcher::Slot()[ BuildSettingsTab() ]
			]
		];

		Reload();
	}

private:
	// ----- chrome

	TSharedRef<SWidget> MakeTab(const FText& InLabel, int32 InIndex, TFunction<int32()> InBadge)
	{
		const TSharedRef<SHorizontalBox> Label = SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				SNew(STextBlock).Text(InLabel).Font(FAppStyle::GetFontStyle("BoldFont"))
					.ColorAndOpacity_Lambda([this, InIndex]() { return ActiveTab == InIndex ? FSlateColor::UseForeground() : FSlateColor::UseSubduedForeground(); })
			];
		if (InBadge)
		{
			Label->AddSlot().AutoWidth().VAlign(VAlign_Center).Padding(6.0f, 0.0f, 0.0f, 0.0f)[ MakeBadge(InBadge) ];
		}

		return SNew(SButton)
			.ButtonStyle(&FAppStyle::Get().GetWidgetStyle<FButtonStyle>("SimpleButton"))
			.ContentPadding(FMargin(10.0f, 6.0f, 10.0f, 0.0f))
			.OnClicked(this, &SLoreHistoryPanel::OnShowTab, InIndex)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 6.0f)[ Label ]
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(SBox).HeightOverride(2.0f)
					[
						SNew(SBorder).BorderImage(FAppStyle::GetBrush("WhiteBrush")).Padding(0.0f)
							.BorderBackgroundColor_Lambda([this, InIndex]() { return ActiveTab == InIndex ? FSlateColor(Accent) : FSlateColor(FLinearColor::Transparent); })
					]
				]
			];
	}

	FReply OnShowTab(int32 InIndex)
	{
		ActiveTab = InIndex;
		Switcher->SetActiveWidgetIndex(InIndex);
		if (InIndex == 3)
		{
			ReloadServer();
			if (!bUpdatesChecked)
			{
				CheckUpdates();
			}
		}
		else if (InIndex == 4)
		{
			if (Server.RemoteUrl.IsEmpty())
			{
				LoadServerInfo(LoreSourceControlUtils::GetCommandContextFromModule(), Server);
			}
			// Show what is saved on disk, so a stale box can never hide or overwrite the key.
			if (ApiKeyBox.IsValid()) { ApiKeyBox->SetText(FText::FromString(LoadAiSetting(TEXT("GeminiApiKey"), FString()))); }
			if (LanguageBox.IsValid()) { LanguageBox->SetText(FText::FromString(Language)); }
		}
		return FReply::Handled();
	}

	FReply OnRefresh()
	{
		Reload();
		return FReply::Handled();
	}

	void Reload()
	{
		const FContext Context = LoreSourceControlUtils::GetCommandContextFromModule();

		if (OwnUserName.IsEmpty())
		{
			OwnUserName = LoreSourceControlUtils::DetectAuthenticatedUser(Context);
		}

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
		Server.LocalRevision = LocalRevision;
		Server.RemoteRevision = RemoteRevision;

		TArray<FString> KnownAuthors;
		for (const TSharedPtr<FLoreRevisionItem>& Revision : Revisions)
		{
			KnownAuthors.AddUnique(Revision->Author);
		}
		for (const TSharedPtr<FLoreLockInfo>& Lock : Locks)
		{
			KnownAuthors.AddUnique(Lock->Owner);
		}
		EnsureOwnAvatarColor(KnownAuthors);

		BranchBlock->SetText(FText::FromString(GetBranchName(Context)));
		ApplyFilter();
		ChangeList->RequestListRefresh();
		LockList->RequestListRefresh();
		if (ActiveTab == 3)
		{
			ReloadServer();
		}
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

	// ----- history tab

	TSharedRef<SWidget> BuildHistoryTab()
	{
		return SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(8.0f, 8.0f, 8.0f, 6.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 6.0f, 0.0f)
				[
					SNew(SBorder).BorderImage(&PillBrush).Padding(FMargin(10.0f, 5.0f))
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 6.0f, 0.0f)[ MakeIcon("Icons.Lock", FLinearColor(0.7f, 0.7f, 0.8f), 12.0f) ]
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)[ SNew(STextBlock).Text(FText::FromString(FApp::GetProjectName())) ]
					]
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 6.0f, 0.0f)
				[
					SNew(SComboButton)
					.OnGetMenuContent(this, &SLoreHistoryPanel::BuildDateFilterMenu)
					.ButtonContent()
					[
						SNew(STextBlock).Text_Lambda([this]() { return DateFilterLabel(); })
					]
				]
				+ SHorizontalBox::Slot().FillWidth(1.0f).Padding(0.0f, 0.0f, 6.0f, 0.0f)
				[
					SAssignNew(FilterBox, SEditableTextBox)
					.HintText(LOCTEXT("FilterHint", "Filter by author, text or number"))
					.OnTextChanged(this, &SLoreHistoryPanel::OnFilterChanged)
				]
				+ SHorizontalBox::Slot().AutoWidth()
				[
					SNew(SButton).Text(LOCTEXT("ClearFilter", "Clear Filter")).OnClicked(this, &SLoreHistoryPanel::OnClearFilter)
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(8.0f, 0.0f, 8.0f, 2.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().Padding(52.0f, 0.0f, 0.0f, 0.0f)
				[
					SNew(STextBlock).ColorAndOpacity(FSlateColor::UseSubduedForeground()).Text(LOCTEXT("ColNo", "No."))
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(36.0f, 0.0f, 0.0f, 0.0f)
				[
					SNew(STextBlock).ColorAndOpacity(FSlateColor::UseSubduedForeground()).Text(LOCTEXT("ColDetails", "Details"))
				]
			]
			+ SVerticalBox::Slot().FillHeight(1.0f)
			[
				SAssignNew(HistoryList, SListView<TSharedPtr<FLoreRevisionItem>>)
				.ListItemsSource(&Filtered)
				.OnGenerateRow(this, &SLoreHistoryPanel::GenerateRevisionRow)
				.SelectionMode(ESelectionMode::None)
			];
	}

	FText DateFilterLabel() const
	{
		switch (DateFilterDays)
		{
		case 1: return LOCTEXT("DateToday", "Today");
		case 7: return LOCTEXT("Date7", "Last 7 days");
		case 30: return LOCTEXT("Date30", "Last 30 days");
		default: return LOCTEXT("DateNone", "No date filter");
		}
	}

	TSharedRef<SWidget> BuildDateFilterMenu()
	{
		FMenuBuilder Menu(true, nullptr);
		const auto AddEntry = [this, &Menu](const FText& InLabel, int32 InDays)
		{
			Menu.AddMenuEntry(InLabel, FText::GetEmpty(), FSlateIcon(), FUIAction(FExecuteAction::CreateLambda([this, InDays]() { DateFilterDays = InDays; ApplyFilter(); })));
		};
		AddEntry(LOCTEXT("DateNone", "No date filter"), 0);
		AddEntry(LOCTEXT("DateToday", "Today"), 1);
		AddEntry(LOCTEXT("Date7", "Last 7 days"), 7);
		AddEntry(LOCTEXT("Date30", "Last 30 days"), 30);
		return Menu.MakeWidget();
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
		DateFilterDays = 0;
		ApplyFilter();
		return FReply::Handled();
	}

	void ApplyFilter()
	{
		Filtered.Reset();
		const FDateTime Now = FDateTime::Now();
		for (const TSharedPtr<FLoreRevisionItem>& Revision : Revisions)
		{
			if (DateFilterDays > 0)
			{
				FDateTime When;
				if (ParseRevisionDate(Revision->Date, When) && (Now - When).GetTotalDays() > DateFilterDays)
				{
					continue;
				}
			}
			if (FilterText.IsEmpty()
				|| Revision->Author.Contains(FilterText, ESearchCase::IgnoreCase)
				|| Revision->Description.Contains(FilterText, ESearchCase::IgnoreCase)
				|| FormatUefnDate(Revision->Date).Contains(FilterText, ESearchCase::IgnoreCase)
				|| FString::FromInt(Revision->Number) == FilterText)
			{
				Filtered.Add(Revision);
			}
		}
		HistoryList->RequestListRefresh();
	}

	TSharedRef<ITableRow> GenerateRevisionRow(TSharedPtr<FLoreRevisionItem> Item, const TSharedRef<STableViewBase>& OwnerTable)
	{
		const bool bIsHead = !Revisions.IsEmpty() && Revisions[0] == Item;
		const bool bExpanded = Item->bExpanded;

		const TSharedRef<SVerticalBox> Content = SNew(SVerticalBox);

		// Header line: avatar, author, description (collapsed) or author only (expanded).
		Content->AddSlot().AutoHeight().Padding(0.0f, 7.0f, 8.0f, 7.0f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 8.0f, 0.0f)[ MakeAvatar(Item->Author) ]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 10.0f, 0.0f)
			[
				SNew(STextBlock).Font(FAppStyle::GetFontStyle("BoldFont")).Text(FText::FromString(Item->Author))
			]
			+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
			[
				SNew(STextBlock).Text(bExpanded ? FText::GetEmpty() : FText::FromString(FirstDescriptionLine(Item->Description)))
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock).ColorAndOpacity(FSlateColor::UseSubduedForeground()).Text(bExpanded ? FText::GetEmpty() : FText::FromString(FormatUefnDate(Item->Date)))
			]
		];

		if (bExpanded)
		{
			Content->AddSlot().AutoHeight().Padding(0.0f, 0.0f, 8.0f, 8.0f)[ BuildRevisionCard(Item, bIsHead) ];
		}

		return SNew(STableRow<TSharedPtr<FLoreRevisionItem>>, OwnerTable)
			.Padding(FMargin(0.0f))
			[
				SNew(SBorder).BorderImage(FAppStyle::GetBrush("NoBorder")).Padding(0.0f)
				.OnMouseButtonDown(this, &SLoreHistoryPanel::OnRevisionClicked, Item)
				[
					SNew(SHorizontalBox)
					// Timeline
					+ SHorizontalBox::Slot().AutoWidth()
					[
						SNew(SBox).WidthOverride(34.0f)
						[
							SNew(SOverlay)
							+ SOverlay::Slot().HAlign(HAlign_Center)
							[
								SNew(SBox).WidthOverride(2.0f)[ SNew(SBorder).BorderImage(FAppStyle::GetBrush("WhiteBrush")).BorderBackgroundColor_Lambda([]() { return FSlateColor(Accent); }) ]
							]
							+ SOverlay::Slot().HAlign(HAlign_Center).VAlign(VAlign_Top).Padding(0.0f, 14.0f, 0.0f, 0.0f)
							[
								bIsHead
									? StaticCastSharedRef<SWidget>(SNew(SBox).WidthOverride(20.0f).HeightOverride(20.0f)
										[
											SNew(SOverlay)
											+ SOverlay::Slot()[ SNew(SBorder).BorderImage(&HeadBrush) ]
											+ SOverlay::Slot().HAlign(HAlign_Center).VAlign(VAlign_Center)[ MakeIcon("Icons.Check", FLinearColor::White, 12.0f) ]
										])
									: StaticCastSharedRef<SWidget>(SNew(SBox).WidthOverride(8.0f).HeightOverride(8.0f)[ SNew(SBorder).BorderImage(&DotBrush) ])
							]
						]
					]
					// Arrow and number
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Top).Padding(0.0f, 10.0f, 0.0f, 0.0f)
					[
						SNew(SBox).WidthOverride(64.0f)
						[
							SNew(SHorizontalBox)
							+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 6.0f, 0.0f)
							[
								SNew(SImage).Image(FAppStyle::GetBrush(bExpanded ? "TreeArrow_Expanded" : "TreeArrow_Collapsed")).ColorAndOpacity(FSlateColor::UseSubduedForeground())
							]
							+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
							[
								SNew(STextBlock).Text(FText::AsNumber(Item->Number))
							]
						]
					]
					+ SHorizontalBox::Slot().FillWidth(1.0f)[ Content ]
				]
			];
	}

	FReply OnRevisionClicked(const FGeometry&, const FPointerEvent& InMouseEvent, TSharedPtr<FLoreRevisionItem> Item)
	{
		if (InMouseEvent.GetEffectingButton() != EKeys::LeftMouseButton)
		{
			return FReply::Unhandled();
		}
		Item->bExpanded = !Item->bExpanded;
		if (Item->bExpanded && !Item->bFilesLoaded)
		{
			LoadRevisionFiles(LoreSourceControlUtils::GetCommandContextFromModule(), *Item);
		}
		HistoryList->RebuildList();
		return FReply::Handled();
	}

	TSharedRef<SWidget> BuildRevisionCard(const TSharedPtr<FLoreRevisionItem>& Item, bool bIsHead)
	{
		int32 Added = 0;
		int32 Modified = 0;
		int32 Deleted = 0;
		for (const TSharedPtr<FLoreFileChange>& File : Item->Files)
		{
			if (File->Status == TEXT("A")) { ++Added; }
			else if (File->Status == TEXT("D")) { ++Deleted; }
			else { ++Modified; }
		}
		const FText Breakdown = FText::Format(LOCTEXT("Breakdown", "{0} Modified   |   {1} Added   |   {2} Deleted"), FText::AsNumber(Modified), FText::AsNumber(Added), FText::AsNumber(Deleted));

		const TSharedRef<SVerticalBox> Card = SNew(SVerticalBox);
		Card->AddSlot().AutoHeight()
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 12.0f, 0.0f)
				[
					SNew(STextBlock).ColorAndOpacity(FSlateColor::UseSubduedForeground()).Text(FText::FromString(FormatUefnDate(Item->Date)))
				]
				+ SHorizontalBox::Slot().AutoWidth()
				[
					SNew(SHorizontalBox).ToolTipText(Breakdown)
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 5.0f, 0.0f)[ MakeIcon("Icons.Edit", FLinearColor(0.7f, 0.7f, 0.75f), 12.0f) ]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
					[
						SNew(STextBlock).ColorAndOpacity(FSlateColor::UseSubduedForeground()).Text(FText::Format(LOCTEXT("FileCount", "{0} file(s)"), FText::AsNumber(Item->Files.Num())))
					]
				]
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(2.0f, 0.0f)
			[
				SNew(SButton).ButtonStyle(&FAppStyle::Get().GetWidgetStyle<FButtonStyle>("SimpleButton"))
					.IsEnabled(!bIsHead)
					.ToolTipText(FText::Format(LOCTEXT("RestoreTip", "Sync your local copy of the project to revision {0}"), FText::AsNumber(Item->Number)))
					.OnClicked(this, &SLoreHistoryPanel::OnRestoreToRevision, Item)
				[
					MakeIcon("Icons.Download", bIsHead ? FLinearColor(0.35f, 0.35f, 0.35f) : FLinearColor(0.65f, 0.95f, 0.25f), 18.0f)
				]
			]
		];
		Card->AddSlot().AutoHeight().Padding(0.0f, 6.0f, 0.0f, 10.0f)
		[
			SNew(STextBlock).AutoWrapText(true).Text(FText::FromString(LayoutDescription(Item->Description)))
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
			Card->AddSlot().AutoHeight()
			[
				SNew(SBorder).BorderImage(&StripeB).Padding(FMargin(12.0f, 7.0f))
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().FillWidth(1.0f)[ SNew(STextBlock).ColorAndOpacity(FSlateColor::UseSubduedForeground()).Text(LOCTEXT("ColAsset", "Asset")) ]
					+ SHorizontalBox::Slot().AutoWidth()[ SNew(STextBlock).ColorAndOpacity(FSlateColor::UseSubduedForeground()).Text(LOCTEXT("ColState", "State")) ]
				]
			];
		}

		int32 RowIndex = 0;
		for (const TSharedPtr<FLoreFileChange>& File : Item->Files)
		{
			const FAssetDisplay Display = DescribeAsset(File->Path);
		const FAssetKind& Kind = Display.Kind;
			const bool bOdd = (RowIndex++ % 2) == 1;
			TSharedPtr<SBorder> RowBorder;
			Card->AddSlot().AutoHeight()
			[
				SAssignNew(RowBorder, SBorder)
				.BorderImage_Lambda([bOdd]() { return bOdd ? &StripeB : &StripeA; })
				.Padding(FMargin(12.0f, 7.0f))
				.ToolTipText(FText::Format(LOCTEXT("AssetTip", "{0}\n{1}"), Kind.Label, FText::FromString(File->Path)))
				.OnMouseButtonUp(this, &SLoreHistoryPanel::OnFileRowMouseUp, Item, File)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 10.0f, 0.0f)[ MakeKindDash(Kind) ]
					+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
					[
						SNew(STextBlock).Text(FText::FromString(Display.Name))
					]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8.0f, 0.0f, 0.0f, 0.0f)[ MakeStateIcon(File->Status) ]
				]
			];
		}

		return SNew(SBorder).BorderImage(&CardBrush).Padding(FMargin(20.0f, 16.0f))[ Card ];
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
		Menu.AddMenuEntry(LOCTEXT("MenuDiffPrev", "Diff Against Previous Revision"), LOCTEXT("MenuDiffPrevTip", "Not available yet for Lore assets"), FSlateIcon(),
			FUIAction(FExecuteAction(), FCanExecuteAction::CreateLambda([]() { return false; })));
		Menu.AddMenuEntry(LOCTEXT("MenuDiffWorkspace", "Diff Against Workspace"), LOCTEXT("MenuDiffWorkspaceTip", "Not available yet for Lore assets"), FSlateIcon(),
			FUIAction(FExecuteAction(), FCanExecuteAction::CreateLambda([]() { return false; })));
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
		OpenFileHistoryWindow(InPath);
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

	// ----- changes tab (check-in)

	TSharedRef<SWidget> BuildChangesTab()
	{
		return SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(16.0f, 14.0f, 16.0f, 6.0f)
			[
				MakeCardTitle(LOCTEXT("DescriptionLabel", "Changelist Description"))
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(16.0f, 0.0f)
			[
				SNew(SBox).MinDesiredHeight(110.0f)
				[
					SNew(SOverlay)
					+ SOverlay::Slot()
					[
						SAssignNew(DescriptionBox, SMultiLineEditableTextBox)
						.HintText(LOCTEXT("DescriptionHint", "Describe your changes, or click Generate to let the AI write it."))
						.AutoWrapText(true)
					]
					+ SOverlay::Slot().HAlign(HAlign_Right).VAlign(VAlign_Bottom).Padding(0.0f, 0.0f, 10.0f, 10.0f)
					[
							SNew(SLoreAiButton)
								.IsBusy_Lambda([this]() { return bGenerating; })
								.ToolTipText(LOCTEXT("GenerateTip", "Write the description with Gemini, from the names of the selected files"))
								.OnClicked(this, &SLoreHistoryPanel::OnGenerate)
					]
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(16.0f, 6.0f, 16.0f, 0.0f)
			[
				SAssignNew(GenerateStatus, STextBlock).ColorAndOpacity(FSlateColor::UseSubduedForeground())
			]
			// Validation bar, like UEFN's Confirm Submit.
			+ SVerticalBox::Slot().AutoHeight().Padding(16.0f, 12.0f, 16.0f, 0.0f)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(SBorder).BorderImage(&ErrorBarBrush).Padding(FMargin(8.0f, 4.0f)).HAlign(HAlign_Center)
					.Visibility_Lambda([this]() { return DescriptionBox->GetText().IsEmptyOrWhitespace() ? EVisibility::Visible : EVisibility::Collapsed; })
					[
						SNew(STextBlock).Text(LOCTEXT("DescriptionRequired", "Description is required to submit"))
					]
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 6.0f, 0.0f, 0.0f)
				[
					SNew(SBorder).BorderImage(&CardBrush).Padding(FMargin(10.0f, 6.0f))
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 8.0f, 0.0f)[ MakeIcon("Icons.Check", Good, 16.0f) ]
						+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
						[
							SNew(STextBlock).Text_Lambda([this]() { return FText::Format(LOCTEXT("SelectedCount", "{0} of {1} file(s) selected"), FText::AsNumber(SelectedChangePaths().Num()), FText::AsNumber(Changes.Num())); })
						]
					]
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(16.0f, 12.0f, 16.0f, 4.0f)
			[
				SNew(SBorder).BorderImage(&StripeB).Padding(FMargin(8.0f, 4.0f))
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 10.0f, 0.0f)
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
					+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center).Padding(26.0f, 0.0f, 0.0f, 0.0f)
					[
						SNew(STextBlock).ColorAndOpacity(FSlateColor::UseSubduedForeground()).Text(LOCTEXT("ColAsset", "Asset"))
					]
					+ SHorizontalBox::Slot().AutoWidth()[ SNew(STextBlock).ColorAndOpacity(FSlateColor::UseSubduedForeground()).Text(LOCTEXT("ColState", "State")) ]
				]
			]
			+ SVerticalBox::Slot().FillHeight(1.0f).Padding(16.0f, 0.0f)
			[
				SAssignNew(ChangeList, SListView<TSharedPtr<FLoreLocalChange>>)
				.ListItemsSource(&Changes)
				.OnGenerateRow(this, &SLoreHistoryPanel::GenerateChangeRow)
				.SelectionMode(ESelectionMode::None)
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(16.0f, 12.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					SNew(SCheckBox)
					.IsChecked_Lambda([this]() { return bKeepLocked ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
					.OnCheckStateChanged_Lambda([this](ECheckBoxState InState) { bKeepLocked = (InState == ECheckBoxState::Checked); })
					[
						SNew(STextBlock).Text(LOCTEXT("KeepLocked", "Keep Files Checked Out"))
					]
				]
				+ SHorizontalBox::Slot().FillWidth(1.0f)[ SNew(SSpacer) ]
				+ SHorizontalBox::Slot().AutoWidth()
				[
					SNew(SButton).ButtonStyle(&FAppStyle::Get().GetWidgetStyle<FButtonStyle>("PrimaryButton"))
						.IsEnabled(this, &SLoreHistoryPanel::CanSubmit)
						.OnClicked(this, &SLoreHistoryPanel::OnSubmit)
						.ContentPadding(FMargin(24.0f, 4.0f))
					[
						SNew(STextBlock).Text(LOCTEXT("Submit", "Submit"))
					]
				]
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
		const FAssetDisplay Display = DescribeAsset(Change->Path);
		const FAssetKind& Kind = Display.Kind;
		return SNew(STableRow<TSharedPtr<FLoreLocalChange>>, OwnerTable)
			.Padding(FMargin(0.0f, 1.0f))
			[
				SNew(SBorder).BorderImage(&StripeA).Padding(FMargin(10.0f, 8.0f))
				.ToolTipText(FText::Format(LOCTEXT("AssetTip", "{0}\n{1}"), Kind.Label, FText::FromString(Change->Path)))
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 10.0f, 0.0f)
					[
						SNew(SCheckBox)
						.IsChecked_Lambda([Change]() { return Change->bSelected ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
						.OnCheckStateChanged_Lambda([Change](ECheckBoxState InState) { Change->bSelected = (InState == ECheckBoxState::Checked); })
					]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 10.0f, 0.0f)[ MakeKindDash(Kind) ]
					+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
					[
						SNew(SVerticalBox)
						+ SVerticalBox::Slot().AutoHeight()[ SNew(STextBlock).Text(FText::FromString(Display.Name)) ]
						+ SVerticalBox::Slot().AutoHeight()[ SNew(STextBlock).Font(FAppStyle::GetFontStyle("SmallFont")).ColorAndOpacity(FSlateColor::UseSubduedForeground()).Text(FText::FromString(FPaths::GetPath(Change->Path))) ]
					]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8.0f, 0.0f)[ MakeStateIcon(Change->Status) ]
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

	/** Flash models to try, in order. Replaced by the models the API key can actually use once they are listed. */
	static TArray<FString> DefaultGeminiModels()
	{
		return { TEXT("gemini-3.5-flash-lite"), TEXT("gemini-3.5-flash"), TEXT("gemini-2.5-flash-lite"), TEXT("gemini-2.5-flash") };
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
			GenerateStatus->SetText(LOCTEXT("NeedKey", "Enter your Gemini API key on the Settings tab first."));
			return FReply::Handled();
		}
		SaveAiSetting(TEXT("GeminiApiKey"), Key);
		GeminiKey = Key;

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
				UserText += TEXT("- ") + FlattenDescription(Revisions[Index]->Description) + TEXT("\n");
			}
		}

		FString Wanted = LanguageBox.IsValid() ? LanguageBox->GetText().ToString().TrimStartAndEnd() : Language;
		if (Wanted.IsEmpty())
		{
			Wanted = TEXT("English");
		}
		Language = Wanted;
		SaveAiSetting(TEXT("Language"), Language);

		const FString LanguageRule = FString::Printf(TEXT("Write in %s. Use the past tense (or the past participle), for example 'Added', 'Updated', 'Removed', 'Fixed' in English, so each line reads as something that was done."), *Language);
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

		GeminiBody.Reset();
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&GeminiBody);
		FJsonSerializer::Serialize(Body.ToSharedRef(), Writer);

		bGenerating = true;
		GenerateStatus->SetText(LOCTEXT("Generating", "Generating..."));
		UE_LOG(LogLoreSourceControl, Log, TEXT("LoreHistory: asking Gemini to describe %d file(s)"), Listed);
		if (GeminiModels.IsEmpty())
		{
			ListGeminiModels();
		}
		else
		{
			TryGeminiModel(0);
		}
		return FReply::Handled();
	}

	/** Ask which models this key can use, so the flash models to try (and fall back to) are the real ones. */
	void ListGeminiModels()
	{
		const TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
		Request->SetURL(TEXT("https://generativelanguage.googleapis.com/v1beta/models?pageSize=200"));
		Request->SetVerb(TEXT("GET"));
		Request->SetHeader(TEXT("x-goog-api-key"), GeminiKey);
		Request->SetTimeout(15.0f);
		Request->OnProcessRequestComplete().BindSP(this, &SLoreHistoryPanel::OnGeminiModelsListed);
		Request->ProcessRequest();
	}

	void OnGeminiModelsListed(FHttpRequestPtr, FHttpResponsePtr InResponse, bool bSucceeded)
	{
		struct FCandidate { FString Name; TArray<int32> Version; bool bLite = false; bool bPreview = false; };
		TArray<FCandidate> Candidates;

		TSharedPtr<FJsonObject> Json;
		if (bSucceeded && InResponse.IsValid() && InResponse->GetResponseCode() == 200
			&& FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(InResponse->GetContentAsString()), Json) && Json.IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* Models = nullptr;
			if (Json->TryGetArrayField(TEXT("models"), Models))
			{
				for (const TSharedPtr<FJsonValue>& Value : *Models)
				{
					const TSharedPtr<FJsonObject> Model = Value->AsObject();
					FString Name;
					if (!Model.IsValid() || !Model->TryGetStringField(TEXT("name"), Name))
					{
						continue;
					}
					Name.RemoveFromStart(TEXT("models/"));
					const FString Lower = Name.ToLower();
					if (!Lower.StartsWith(TEXT("gemini-")) || !Lower.Contains(TEXT("flash")))
					{
						continue;
					}
					static const TCHAR* const Excluded[] = { TEXT("image"), TEXT("tts"), TEXT("live"), TEXT("audio"), TEXT("embed"), TEXT("robot"), TEXT("computer"), TEXT("native"), TEXT("8b") };
					bool bSkip = false;
					for (const TCHAR* Word : Excluded)
					{
						bSkip |= Lower.Contains(Word);
					}
					const TArray<TSharedPtr<FJsonValue>>* Methods = nullptr;
					bool bCanGenerate = false;
					if (Model->TryGetArrayField(TEXT("supportedGenerationMethods"), Methods))
					{
						for (const TSharedPtr<FJsonValue>& Method : *Methods)
						{
							bCanGenerate |= Method->AsString() == TEXT("generateContent");
						}
					}
					if (bSkip || !bCanGenerate)
					{
						continue;
					}
					FCandidate Candidate;
					Candidate.Name = Name;
					Candidate.Version = ParseVersionNumbers(Name);
					Candidate.bLite = Lower.Contains(TEXT("lite"));
					Candidate.bPreview = Lower.Contains(TEXT("preview")) || Lower.Contains(TEXT("exp"));
					Candidates.Add(Candidate);
				}
			}
		}

		// Newest stable version first, the lighter model before the full one (it has the higher free limits).
		Candidates.Sort([](const FCandidate& A, const FCandidate& B)
		{
			if (A.bPreview != B.bPreview) { return !A.bPreview; }
			const int32 Compare = CompareVersions(A.Name, B.Name);
			if (Compare != 0) { return Compare > 0; }
			return A.bLite && !B.bLite;
		});

		GeminiModels.Reset();
		for (const FCandidate& Candidate : Candidates)
		{
			if (GeminiModels.Num() < 6)
			{
				GeminiModels.Add(Candidate.Name);
			}
		}
		if (GeminiModels.IsEmpty())
		{
			GeminiModels = DefaultGeminiModels();
		}
		TryGeminiModel(0);
	}

	void TryGeminiModel(int32 InIndex)
	{
		GeminiModelIndex = InIndex;
		const FString& Model = GeminiModels[InIndex];
		const TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
		Request->SetURL(FString::Printf(TEXT("https://generativelanguage.googleapis.com/v1beta/models/%s:generateContent"), *Model));
		Request->SetVerb(TEXT("POST"));
		Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
		Request->SetHeader(TEXT("x-goog-api-key"), GeminiKey);
		Request->SetContentAsString(GeminiBody);
		Request->SetTimeout(60.0f);
		Request->OnProcessRequestComplete().BindSP(this, &SLoreHistoryPanel::OnGenerated);
		Request->ProcessRequest();
	}

	void OnGenerated(FHttpRequestPtr, FHttpResponsePtr InResponse, bool bSucceeded)
	{
		const FString UsedModel = GeminiModels.IsValidIndex(GeminiModelIndex) ? GeminiModels[GeminiModelIndex] : FString();

		// Limit reached or model unavailable: quietly move on to the next flash model.
		if (bSucceeded && InResponse.IsValid())
		{
			const int32 Code = InResponse->GetResponseCode();
			if ((Code == 429 || Code == 503 || Code == 500 || Code == 404) && GeminiModels.IsValidIndex(GeminiModelIndex + 1))
			{
				UE_LOG(LogLoreSourceControl, Log, TEXT("LoreHistory: Gemini model %s answered HTTP %d, trying %s"), *UsedModel, Code, *GeminiModels[GeminiModelIndex + 1]);
				GenerateStatus->SetText(FText::Format(LOCTEXT("GenerateFallback", "{0} is not available right now (limit reached). Trying {1}..."), FText::FromString(UsedModel), FText::FromString(GeminiModels[GeminiModelIndex + 1])));
				TryGeminiModel(GeminiModelIndex + 1);
				return;
			}
		}

		bGenerating = false;
		if (!bSucceeded || !InResponse.IsValid())
		{
			GenerateStatus->SetText(LOCTEXT("GenerateNoConnection", "Could not reach Gemini. Check your internet connection."));
			return;
		}
		if (InResponse->GetResponseCode() == 429)
		{
			GenerateStatus->SetText(LOCTEXT("GenerateAllLimited", "The Gemini limit is reached for all flash models. Try again later (the daily limit resets at 09:00 Dutch time) or write the description yourself."));
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
		GenerateStatus->SetText(FText::Format(LOCTEXT("GenerateDone", "Written by {0}. Edit it if you like, then Submit."), FText::FromString(UsedModel)));
	}

	// ----- locks tab

	TSharedRef<SWidget> BuildLocksTab()
	{
		return SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(16.0f, 14.0f, 16.0f, 10.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
				[
					SNew(STextBlock).AutoWrapText(true).ColorAndOpacity(FSlateColor::UseSubduedForeground())
						.Text(LOCTEXT("LocksHelp", "Files checked out (locked) by you or by others. A locked file cannot be changed by anyone else."))
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(8.0f, 0.0f, 0.0f, 0.0f)
				[
					SNew(SButton).Text(LOCTEXT("UnlockAllMine", "Unlock all mine")).OnClicked(this, &SLoreHistoryPanel::OnUnlockAllMine)
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(16.0f, 0.0f, 16.0f, 4.0f)
			[
				SNew(SBorder).BorderImage(&StripeB).Padding(FMargin(8.0f, 4.0f))
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().FillWidth(1.0f).Padding(34.0f, 0.0f, 0.0f, 0.0f)[ SNew(STextBlock).ColorAndOpacity(FSlateColor::UseSubduedForeground()).Text(LOCTEXT("ColAsset", "Asset")) ]
					+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 90.0f, 0.0f)[ SNew(STextBlock).ColorAndOpacity(FSlateColor::UseSubduedForeground()).Text(LOCTEXT("ColCheckedOutBy", "Checked out by")) ]
				]
			]
			+ SVerticalBox::Slot().FillHeight(1.0f).Padding(16.0f, 0.0f)
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
		const FAssetDisplay Display = DescribeAsset(Lock->Path);
		const FAssetKind& Kind = Display.Kind;
		return SNew(STableRow<TSharedPtr<FLoreLockInfo>>, OwnerTable)
			.Padding(FMargin(0.0f, 1.0f))
			[
				SNew(SBorder).BorderImage(&StripeA).Padding(FMargin(10.0f, 8.0f))
				.ToolTipText(FText::Format(LOCTEXT("AssetTip", "{0}\n{1}"), Kind.Label, FText::FromString(Lock->Path)))
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 8.0f, 0.0f)[ MakeIcon("Icons.Lock", bMine ? Good : Warn) ]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 10.0f, 0.0f)[ MakeKindDash(Kind) ]
					+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
					[
						SNew(SVerticalBox)
						+ SVerticalBox::Slot().AutoHeight()[ SNew(STextBlock).Text(FText::FromString(Display.Name)) ]
						+ SVerticalBox::Slot().AutoHeight()[ SNew(STextBlock).Font(FAppStyle::GetFontStyle("SmallFont")).ColorAndOpacity(FSlateColor::UseSubduedForeground()).Text(FText::FromString(FPaths::GetPath(Lock->Path))) ]
					]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8.0f, 0.0f)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 8.0f, 0.0f)[ MakeAvatar(Lock->Owner, 20.0f) ]
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)[ SNew(STextBlock).Font(FAppStyle::GetFontStyle("BoldFont")).Text(FText::FromString(Lock->Owner)) ]
					]
					+ SHorizontalBox::Slot().AutoWidth()
					[
						SNew(SButton).Text(LOCTEXT("Unlock", "Unlock")).OnClicked(this, &SLoreHistoryPanel::OnUnlock, Lock)
					]
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
			LoreSourceControlUtils::ForgetRememberedLock(Lock->Path);
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
					LoreSourceControlUtils::ForgetRememberedLock(Lock->Path);
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

	// ----- server tab

	TSharedRef<SWidget> MakeInfoRow(const FText& InLabel, TFunction<FText()> InValue)
	{
		return SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 6.0f, 16.0f, 6.0f)
			[
				SNew(SBox).WidthOverride(130.0f)[ SNew(STextBlock).ColorAndOpacity(FSlateColor::UseSubduedForeground()).Text(InLabel) ]
			]
			+ SHorizontalBox::Slot().FillWidth(1.0f).Padding(0.0f, 6.0f)
			[
				SNew(STextBlock).AutoWrapText(true).Text_Lambda([InValue]() { return InValue(); })
			];
	}

	TSharedRef<SWidget> MakeInfoCard(const FText& InTitle, const TSharedRef<SWidget>& InBody)
	{
		return SNew(SBorder).BorderImage(&CardBrush).Padding(FMargin(20.0f, 16.0f))
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 12.0f)[ MakeCardTitle(InTitle) ]
				+ SVerticalBox::Slot().AutoHeight()[ InBody ]
			];
	}

	TSharedRef<SWidget> BuildServerTab()
	{
		const auto Str = [](const FString& InValue) { return InValue.IsEmpty() ? FText::FromString(TEXT("-")) : FText::FromString(InValue); };

		return SNew(SScrollBox)
			+ SScrollBox::Slot().Padding(16.0f, 16.0f, 16.0f, 8.0f)
			[
				MakeInfoCard(LOCTEXT("CardConnection", "Connection"),
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f, 0.0f, 6.0f)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 8.0f, 0.0f)
						[
							SNew(SBox).WidthOverride(10.0f).HeightOverride(10.0f)
							[
								SNew(SBorder).BorderImage_Lambda([this]() { return Server.bReachable ? &GoodDotBrush : &BadDotBrush; })
							]
						]
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
						[
							SNew(STextBlock).Font(FAppStyle::GetFontStyle("BoldFont"))
								.Text_Lambda([this]() { return !Server.bHealthKnown ? LOCTEXT("ServerChecking", "Checking...") : Server.bReachable ? FText::Format(LOCTEXT("ServerOnline", "Online ({0} ms)"), FText::AsNumber(Server.LatencyMs)) : LOCTEXT("ServerOffline", "Not reachable"); })
						]
					]
					+ SVerticalBox::Slot().AutoHeight()[ MakeInfoRow(LOCTEXT("RowUrl", "Server"), [this, Str]() { return Str(Server.RemoteUrl); }) ]
					+ SVerticalBox::Slot().AutoHeight()[ MakeInfoRow(LOCTEXT("RowChecked", "Last checked"), [this, Str]() { return Str(Server.LastChecked); }) ])
			]
			+ SScrollBox::Slot().Padding(16.0f, 8.0f)
			[
				MakeInfoCard(LOCTEXT("CardRepository", "Repository"),
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight()[ MakeInfoRow(LOCTEXT("RowName", "Name"), [this, Str]() { return Str(Server.RepositoryName); }) ]
					+ SVerticalBox::Slot().AutoHeight()[ MakeInfoRow(LOCTEXT("RowId", "Id"), [this, Str]() { return Str(Server.RepositoryId); }) ]
					+ SVerticalBox::Slot().AutoHeight()[ MakeInfoRow(LOCTEXT("RowBranch", "Default branch"), [this, Str]() { return Str(Server.DefaultBranch); }) ]
					+ SVerticalBox::Slot().AutoHeight()[ MakeInfoRow(LOCTEXT("RowCreator", "Created by"), [this, Str]() { return Str(Server.Creator); }) ]
					+ SVerticalBox::Slot().AutoHeight()[ MakeInfoRow(LOCTEXT("RowCreated", "Created"), [this, Str]() { return Str(Server.Created); }) ]
					+ SVerticalBox::Slot().AutoHeight()[ MakeInfoRow(LOCTEXT("RowRevisions", "Revisions"), [this]() { return FText::Format(LOCTEXT("RevisionsValue", "Local {0}  |  Server {1}"), FText::AsNumber(Server.LocalRevision), FText::AsNumber(Server.RemoteRevision)); }) ])
			]
			+ SScrollBox::Slot().Padding(16.0f, 8.0f)
			[
				MakeInfoCard(LOCTEXT("CardAccount", "Account"),
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight()[ MakeInfoRow(LOCTEXT("RowUser", "Signed in as"), [this, Str]() { return Str(Server.User); }) ]
					+ SVerticalBox::Slot().AutoHeight()[ MakeInfoRow(LOCTEXT("RowExpires", "Login valid until"), [this, Str]() { return Str(Server.TokenExpires); }) ]
					+ SVerticalBox::Slot().AutoHeight()[ MakeInfoRow(LOCTEXT("RowClient", "Lore client"), [this, Str]() { return Str(Server.ClientVersion); }) ])
			]
			+ SScrollBox::Slot().Padding(16.0f, 8.0f)
			[
				MakeInfoCard(LOCTEXT("CardUpdates", "Updates"),
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight()[ MakeInfoRow(LOCTEXT("RowLoreClient", "Lore client"), [this]() { return LoreRelease.Status; }) ]
					+ SVerticalBox::Slot().AutoHeight()[ MakeInfoRow(LOCTEXT("RowPluginVersion", "This plugin"), [this]() { return PluginRelease.Status; }) ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 16.0f, 0.0f, 0.0f)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 12.0f, 0.0f)
						[
							SNew(SButton).Text(LOCTEXT("CheckUpdates", "Check for updates")).ContentPadding(FMargin(16.0f, 6.0f)).OnClicked(this, &SLoreHistoryPanel::OnCheckUpdates)
						]
						+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 12.0f, 0.0f)
						[
							SNew(SButton).Text(LOCTEXT("OpenLoreRelease", "Open Lore download page")).ContentPadding(FMargin(16.0f, 6.0f))
								.IsEnabled_Lambda([this]() { return LoreRelease.bUpdateAvailable && !LoreRelease.Url.IsEmpty(); })
								.OnClicked_Lambda([this]() { FPlatformProcess::LaunchURL(*LoreRelease.Url, nullptr, nullptr); return FReply::Handled(); })
						]
						+ SHorizontalBox::Slot().AutoWidth()
						[
							SNew(SButton).Text(LOCTEXT("OpenPluginRelease", "Open plugin page")).ContentPadding(FMargin(16.0f, 6.0f))
								.IsEnabled_Lambda([this]() { return !PluginRelease.Url.IsEmpty(); })
								.OnClicked_Lambda([this]() { FPlatformProcess::LaunchURL(*PluginRelease.Url, nullptr, nullptr); return FReply::Handled(); })
						]
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 16.0f, 0.0f, 0.0f)
					[
						SNew(STextBlock).AutoWrapText(true).ColorAndOpacity(FSlateColor::UseSubduedForeground())
							.Text(LOCTEXT("UpdatesNote", "The Lore client is checked against EpicGames/lore and this plugin against its own GitHub repository. The Lore server and the client need to be the same version, so update the server container too."))
					])
			]
			;
	}

	// ----- updates

	FReply OnCheckUpdates()
	{
		CheckUpdates();
		return FReply::Handled();
	}

	void CheckUpdates()
	{
		bUpdatesChecked = true;
		LoreRelease = FReleaseInfo();
		PluginRelease = FReleaseInfo();
		LoreRelease.Status = LOCTEXT("UpdateChecking", "Checking...");
		PluginRelease.Status = LOCTEXT("UpdateChecking", "Checking...");
		PluginRelease.Url = PluginRepositoryUrl;
		StartReleaseCheck(TEXT("https://api.github.com/repos/EpicGames/lore/releases/latest"), true);
		// This plugin's own version is read from the .uplugin file on the main branch of its repository.
		StartReleaseCheck(TEXT("https://raw.githubusercontent.com/epicaleric/lore-plugin-ue-improved/main/LoreSourceControl.uplugin"), false);
	}

	void StartReleaseCheck(const FString& InUrl, bool bInLoreClient)
	{
		const TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
		Request->SetURL(InUrl);
		Request->SetVerb(TEXT("GET"));
		Request->SetHeader(TEXT("Accept"), TEXT("application/vnd.github+json"));
		Request->SetHeader(TEXT("User-Agent"), TEXT("LoreSourceControl-UE"));
		Request->SetTimeout(10.0f);
		Request->OnProcessRequestComplete().BindSP(this, &SLoreHistoryPanel::OnReleaseChecked, bInLoreClient);
		Request->ProcessRequest();
	}

	void OnReleaseChecked(FHttpRequestPtr, FHttpResponsePtr InResponse, bool bSucceeded, bool bInLoreClient)
	{
		FReleaseInfo& Info = bInLoreClient ? LoreRelease : PluginRelease;
		Info = FReleaseInfo();
		Info.Url = bInLoreClient ? FString() : PluginRepositoryUrl;

		TSharedPtr<FJsonObject> Json;
		if (!bSucceeded || !InResponse.IsValid() || InResponse->GetResponseCode() != 200
			|| !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(InResponse->GetContentAsString()), Json) || !Json.IsValid())
		{
			Info.Status = LOCTEXT("UpdateUnknown", "Could not check (no connection or GitHub did not answer).");
			return;
		}
		if (bInLoreClient)
		{
			Json->TryGetStringField(TEXT("tag_name"), Info.Latest);
			Json->TryGetStringField(TEXT("html_url"), Info.Url);
		}
		else
		{
			Json->TryGetStringField(TEXT("VersionName"), Info.Latest);
		}

		FString Installed;
		if (bInLoreClient)
		{
			Installed = Server.ClientVersion;
		}
		else if (const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("LoreSourceControl")))
		{
			Installed = Plugin->GetDescriptor().VersionName;
		}
		const int32 Compare = CompareVersions(Installed, Info.Latest);
		const FText InstalledText = FText::FromString(Installed.IsEmpty() ? FString(TEXT("?")) : Installed);
		const FText LatestText = FText::FromString(Info.Latest);
		if (Compare < 0)
		{
			Info.bUpdateAvailable = true;
			Info.Status = FText::Format(LOCTEXT("UpdateAvailable", "Update available: {1} (you have {0})"), InstalledText, LatestText);
		}
		else if (Compare == 0)
		{
			Info.Status = FText::Format(LOCTEXT("UpToDate", "Up to date ({0}, latest {1})"), InstalledText, LatestText);
		}
		else
		{
			Info.Status = FText::Format(LOCTEXT("NewerThanRelease", "{0}, newer than the published version ({1})"), InstalledText, LatestText);
		}
	}

	// ----- settings tab

	static FString NormalizeColorValue(const FString& InValue)
	{
		FColor Parsed;
		return ParseColorValue(InValue, Parsed) ? Parsed.ToHex().Left(6).ToUpper() : FString();
	}

	TSharedRef<SWidget> MakeSwatch(const FThemeColor& InColor, TFunction<FString()> InCurrent, TFunction<void(const FString&)> InPick)
	{
		const FString Hex = InColor.Srgb.ToHex().Left(6).ToUpper();
		const FLinearColor Tint = FLinearColor::FromSRGBColor(InColor.Srgb);
		return SNew(SButton).ButtonStyle(&FAppStyle::Get().GetWidgetStyle<FButtonStyle>("SimpleButton"))
			.ContentPadding(FMargin(3.0f))
			.ToolTipText(InColor.Label)
			.OnClicked_Lambda([Hex, InPick]() { InPick(Hex); return FReply::Handled(); })
			[
				SNew(SBox).WidthOverride(28.0f).HeightOverride(28.0f)
				[
					SNew(SOverlay)
					+ SOverlay::Slot()
					[
						SNew(SBorder).BorderImage(&SwatchRingBrush).Padding(0.0f)
							.BorderBackgroundColor_Lambda([Hex, InCurrent]() { return NormalizeColorValue(InCurrent()) == Hex ? FSlateColor(FLinearColor::White) : FSlateColor(FLinearColor::Transparent); })
					]
					+ SOverlay::Slot().Padding(3.0f)
					[
						SNew(SBorder).BorderImage(&SwatchBrush).Padding(0.0f).BorderBackgroundColor(Tint)
					]
				]
			];
	}

	/** Opens the editor's colour picker so any colour can be chosen. */
	TSharedRef<SWidget> MakeCustomColorButton(TFunction<FString()> InCurrent, TFunction<void(const FString&)> InPick)
	{
		return SNew(SButton)
			.ContentPadding(FMargin(12.0f, 5.0f))
			.ToolTipText(LOCTEXT("PickColorTip", "Choose any colour"))
			.OnClicked_Lambda([InCurrent, InPick]()
			{
				FColorPickerArgs Args(ColorFromValue(InCurrent(), Accent), FOnLinearColorValueChanged::CreateLambda([InPick](FLinearColor InColor) { InPick(ColorToValue(InColor)); }));
				Args.bOnlyRefreshOnMouseUp = true;
					Args.bOnlyRefreshOnOk = true;
				Args.bUseAlpha = false;
				OpenColorPicker(Args);
				return FReply::Handled();
			})
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 8.0f, 0.0f)
				[
					SNew(SBox).WidthOverride(16.0f).HeightOverride(16.0f)
					[
						SNew(SBorder).BorderImage(&SwatchBrush).Padding(0.0f)
							.BorderBackgroundColor_Lambda([InCurrent]() { return FSlateColor(ColorFromValue(InCurrent(), Accent)); })
					]
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					SNew(STextBlock).Text(LOCTEXT("CustomColor", "Choose colour..."))
				]
			];
	}

	TSharedRef<SWidget> MakeColorChooser(TFunction<FString()> InCurrent, TFunction<void(const FString&)> InPick)
	{
		const TSharedRef<SHorizontalBox> Row = SNew(SHorizontalBox);
		for (const FThemeColor& Color : ThemeColors())
		{
			Row->AddSlot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 4.0f, 0.0f)[ MakeSwatch(Color, InCurrent, InPick) ];
		}
		Row->AddSlot().AutoWidth().VAlign(VAlign_Center).Padding(8.0f, 0.0f, 0.0f, 0.0f)[ MakeCustomColorButton(InCurrent, InPick) ];
		return Row;
	}

	TSharedRef<SWidget> MakeSettingRow(const FText& InLabel, const TSharedRef<SWidget>& InControl)
	{
		return SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 8.0f, 16.0f, 8.0f)
			[
				SNew(SBox).WidthOverride(150.0f)[ SNew(STextBlock).ColorAndOpacity(FSlateColor::UseSubduedForeground()).Text(InLabel) ]
			]
			+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center).Padding(0.0f, 8.0f)
			[
				InControl
			];
	}

	TSharedRef<SWidget> BuildSettingsTab()
	{
		AccentId = LoadUiSetting(TEXT("Accent"), TEXT("teal"));

		const auto Str = [](const FString& InValue) { return InValue.IsEmpty() ? FText::FromString(TEXT("-")) : FText::FromString(InValue); };

		const TSharedRef<SWidget> AppearanceBody = SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()
			[
				MakeSettingRow(LOCTEXT("SettingAccent", "Accent colour"),
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 14.0f, 0.0f)
					[
						SNew(SBox).WidthOverride(30.0f).HeightOverride(30.0f)[ SNew(SBorder).BorderImage(&AvatarBrush).Padding(0.0f).BorderBackgroundColor_Lambda([]() { return FSlateColor(Accent); }) ]
					]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
					[
					MakeColorChooser(
						[this]() { return AccentId; },
						[this](const FString& InValue)
						{
							AccentId = InValue;
							SaveUiSetting(TEXT("Accent"), InValue);
							ApplyAccent(InValue);
							Invalidate(EInvalidateWidgetReason::Paint);
						})
					]
				)
			]
			+ SVerticalBox::Slot().AutoHeight()
			[
				MakeSettingRow(LOCTEXT("SettingAvatar", "Your avatar colour"),
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 14.0f, 0.0f)
					[
						SNew(SBox).WidthOverride(30.0f).HeightOverride(30.0f)
						[
							SNew(SBorder).BorderImage(&AvatarBrush).HAlign(HAlign_Center).VAlign(VAlign_Center).Padding(0.0f)
								.BorderBackgroundColor_Lambda([]() { return FSlateColor(AvatarColorFor(OwnUserName)); })
							[
								SNew(STextBlock).Font(FAppStyle::GetFontStyle("BoldFont"))
									.ColorAndOpacity_Lambda([]() { return ContrastFor(AvatarColorFor(OwnUserName)); })
									.Text_Lambda([]() { return FText::FromString(OwnUserName.IsEmpty() ? FString(TEXT("?")) : OwnUserName.Left(1).ToUpper()); })
							]
						]
					]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
					[
						MakeColorChooser(
							[]() { return OwnAvatarColorId; },
							[this](const FString& InValue)
							{
								OwnAvatarColorId = InValue;
								SaveUiSetting(TEXT("AvatarColor"), InValue);
								if (HistoryList.IsValid()) { HistoryList->RebuildList(); }
								if (LockList.IsValid()) { LockList->RebuildList(); }
							})
					])
			];

		const TSharedRef<SWidget> IdentityBody = SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()
			[
				MakeSettingRow(LOCTEXT("SettingSignedIn", "Signed in as"), SNew(STextBlock).Text_Lambda([this, Str]() { return Str(Server.User.IsEmpty() ? OwnUserName : Server.User); }))
			]
			+ SVerticalBox::Slot().AutoHeight()
			[
				MakeSettingRow(LOCTEXT("SettingIdentity", "Identity name"),
					SNew(SEditableTextBox)
						.HintText(LOCTEXT("IdentityHint", "Leave empty to use the name you are logged in with"))
						.ToolTipText(LOCTEXT("IdentityTip", "Passed to Lore as --identity. Only set this if you know the name exists on the Lore server; a wrong name makes Lore commands fail."))
						.Text(FText::FromString(FLoreSourceControlModule::Get().AccessSettings().GetUserName()))
						.OnTextCommitted_Lambda([](const FText& InText, ETextCommit::Type)
						{
							FLoreSourceControlModule& Module = FLoreSourceControlModule::Get();
							Module.AccessSettings().SetUserName(InText.ToString().TrimStartAndEnd());
							Module.SaveSettings();
						}))
			]
			+ SVerticalBox::Slot().AutoHeight()
			[
				MakeSettingRow(LOCTEXT("SettingServer", "Server address"),
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
					[
						SNew(STextBlock).Text_Lambda([this, Str]() { return Str(Server.RemoteUrl); })
					]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(12.0f, 0.0f, 0.0f, 0.0f)
					[
						SNew(SButton).ButtonStyle(&FAppStyle::Get().GetWidgetStyle<FButtonStyle>("SimpleButton"))
							.ToolTipText(LOCTEXT("CopyServerTip", "Copy the server address"))
							.OnClicked_Lambda([this]() { FPlatformApplicationMisc::ClipboardCopy(*Server.RemoteUrl); return FReply::Handled(); })
						[
							MakeIcon("GenericCommands.Copy", FLinearColor::White, 16.0f)
						]
					])
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 8.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock).AutoWrapText(true).ColorAndOpacity(FSlateColor::UseSubduedForeground())
					.Text(LOCTEXT("ServerAddressNote", "The server address is stored in the project's Lore working copy and Lore has no command to change it, so it can only be read here. To point the project at another server, clone the repository from that server."))
			];

		const TSharedRef<SWidget> GeminiCardBody = SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 8.0f)
			[
				SNew(STextBlock).AutoWrapText(true).ColorAndOpacity(FSlateColor::UseSubduedForeground())
					.Text(LOCTEXT("AiPrivacy", "Generate in the Changes tab sends only the file names and change types, plus the last few descriptions, to Google. On the free tier Google may use prompts to improve its products. The key is saved in this project's Saved/Config folder, not in the repository. The newest Gemini flash model is used, and the plugin switches to the next flash model by itself when a limit is reached."))
			]
			+ SVerticalBox::Slot().AutoHeight()
			[
				MakeSettingRow(LOCTEXT("SettingApiKey", "Gemini API key"),
					SAssignNew(ApiKeyBox, SEditableTextBox)
						.IsPassword(true)
						.HintText(LOCTEXT("ApiKeyHint", "Gemini API key"))
						.Text(FText::FromString(LoadAiSetting(TEXT("GeminiApiKey"), FString())))
						.OnTextCommitted(this, &SLoreHistoryPanel::OnAiSettingCommitted, FString(TEXT("GeminiApiKey"))))
			]
			+ SVerticalBox::Slot().AutoHeight()
			[
				MakeSettingRow(LOCTEXT("LanguageLabel", "Description language"),
					SAssignNew(LanguageBox, SEditableTextBox)
						.HintText(LOCTEXT("LanguageHint", "For example Nederlands, English, Deutsch, Francais"))
						.Text(FText::FromString(Language))
						.OnTextCommitted_Lambda([this](const FText& InText, ETextCommit::Type)
						{
							const FString Value = InText.ToString().TrimStartAndEnd();
							Language = Value.IsEmpty() ? FString(TEXT("English")) : Value;
							SaveAiSetting(TEXT("Language"), Language);
						}))
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 8.0f, 0.0f, 0.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth()
				[
					SNew(SButton).Text(LOCTEXT("OpenAiStudio", "See my Gemini limits and usage")).ContentPadding(FMargin(16.0f, 6.0f))
						.ToolTipText(LOCTEXT("OpenAiStudioTip", "Opens Google AI Studio, where the free-tier limits and your usage are shown per project"))
						.OnClicked_Lambda([]() { FPlatformProcess::LaunchURL(TEXT("https://aistudio.google.com/rate-limit"), nullptr, nullptr); return FReply::Handled(); })
				]
			];

		return SNew(SScrollBox)
			+ SScrollBox::Slot().Padding(16.0f, 16.0f, 16.0f, 8.0f)[ MakeInfoCard(LOCTEXT("CardAppearance", "Appearance"), AppearanceBody) ]
			+ SScrollBox::Slot().Padding(16.0f, 8.0f)[ MakeInfoCard(LOCTEXT("CardIdentity", "Identity and server"), IdentityBody) ]
			+ SScrollBox::Slot().Padding(16.0f, 8.0f, 16.0f, 24.0f)[ MakeInfoCard(LOCTEXT("CardGemini", "Gemini (AI descriptions)"), GeminiCardBody) ];
	}

	void ReloadServer()
	{
		LoadServerInfo(LoreSourceControlUtils::GetCommandContextFromModule(), Server);

		// Health check on the server's HTTP port (41339) to see if it is up and how fast it answers.
		FString Host = Server.RemoteUrl;
		Host.RemoveFromStart(TEXT("lores://"));
		Host.RemoveFromStart(TEXT("lore://"));
		int32 Colon = INDEX_NONE;
		if (Host.FindChar(TEXT(':'), Colon))
		{
			Host = Host.Left(Colon);
		}
		Server.bHealthKnown = false;
		if (Host.IsEmpty())
		{
			Server.bHealthKnown = true;
			Server.bReachable = false;
			return;
		}
		HealthStarted = FPlatformTime::Seconds();
		const TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
		Request->SetURL(FString::Printf(TEXT("http://%s:41339/health_check"), *Host));
		Request->SetVerb(TEXT("GET"));
		Request->SetTimeout(5.0f);
		Request->OnProcessRequestComplete().BindSP(this, &SLoreHistoryPanel::OnHealth);
		Request->ProcessRequest();
	}

	void OnHealth(FHttpRequestPtr, FHttpResponsePtr InResponse, bool bSucceeded)
	{
		Server.bHealthKnown = true;
		Server.bReachable = bSucceeded && InResponse.IsValid() && InResponse->GetResponseCode() == 200;
		Server.LatencyMs = static_cast<int32>((FPlatformTime::Seconds() - HealthStarted) * 1000.0);
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
	TSharedPtr<STextBlock> BranchBlock;
	TSharedPtr<SEditableTextBox> FilterBox;

	TSharedPtr<SMultiLineEditableTextBox> DescriptionBox;
	TSharedPtr<STextBlock> GenerateStatus;
	TSharedPtr<SEditableTextBox> ApiKeyBox;
	TSharedPtr<SEditableTextBox> LanguageBox;
	TArray<FString> GeminiModels;
	int32 GeminiModelIndex = 0;
	FString GeminiKey;
	FString GeminiBody;

	FServerInfo Server;
	FReleaseInfo LoreRelease;
	FReleaseInfo PluginRelease;
	FString AccentId;
	bool bUpdatesChecked = false;
	double HealthStarted = 0.0;
	FString FilterText;
	FString Language = TEXT("en");
	int32 ActiveTab = 0;
	int32 BehindCount = 0;
	int32 DateFilterDays = 0;
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
			.SetDisplayName(LOCTEXT("TabTitle", "Lore Revision Control"))
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
