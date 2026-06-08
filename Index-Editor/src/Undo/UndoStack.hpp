#pragma once
#include "Undo/EditCommand.hpp"
#include <memory>
#include <vector>

namespace Index {
	class Scene;

	// Per-context-scene undo/redo history. The owner clears it when the editing
	// context changes (scene switch, prefab-edit toggle, play-mode boundaries).
	class UndoStack {
	public:
		void Push(std::unique_ptr<EditCommand> command);
		// Undo/Redo return the affected command's Label() (empty if nothing to do),
		// so the caller can surface it in the toast.
		std::string Undo(Scene& scene);
		std::string Redo(Scene& scene);
		void Clear();

		bool CanUndo() const { return !m_Undo.empty(); }
		bool CanRedo() const { return !m_Redo.empty(); }

	private:
		// Oldest entries drop past this depth; per-drag/per-edit churn would
		// otherwise make the history an unbounded memory sink.
		static constexpr std::size_t k_MaxDepth = 256;

		std::vector<std::unique_ptr<EditCommand>> m_Undo;
		std::vector<std::unique_ptr<EditCommand>> m_Redo;
	};
}
