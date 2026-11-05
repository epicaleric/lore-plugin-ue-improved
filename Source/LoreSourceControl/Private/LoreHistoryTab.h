#pragma once

#include "CoreMinimal.h"

/**
 * Dockable "Lore History" window: the project-wide revision list (author, date,
 * description), expandable per revision to show the changed files, with restore
 * actions, plus a list of the files currently locked with an Unlock button.
 */
namespace LoreHistoryTab
{
	/** Register the nomad tab spawner (Window menu). Call once on module startup. */
	void Register();

	/** Remove the tab spawner. Call on module shutdown. */
	void Unregister();

	/** Bring the window to the front, opening it if needed. */
	void Open();
}
