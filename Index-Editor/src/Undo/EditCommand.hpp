#pragma once
#include <string>

namespace Index {
	class Scene;

	// One reversible editor action, pushed onto the UndoStack already applied
	// (direct manipulations record after the fact, so there is no Execute — Redo
	// re-applies the post-edit state). Commands store stable persistent ids, not
	// raw EntityHandles, so they survive handle recycling; the owning stack passes
	// the live context scene at apply time.
	class EditCommand {
	public:
		virtual ~EditCommand() = default;

		virtual void Undo(Scene& scene) = 0;
		virtual void Redo(Scene& scene) = 0;

		// Fold a follow-on edit into this one when both are the same continuous
		// gesture (e.g. successive keystrokes on one inspector field) so the stack
		// keeps a single entry. Default keeps them separate.
		virtual bool TryMergeWith(const EditCommand& next) { (void)next; return false; }

		virtual const char* Name() const { return "Edit"; }
		// Human-readable action shown in the undo/redo toast (no "Undo:" prefix).
		virtual std::string Label() const { return Name(); }
	};
}
