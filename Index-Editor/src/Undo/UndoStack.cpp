#include <pch.hpp>
#include "Undo/UndoStack.hpp"

namespace Index {

	void UndoStack::Push(std::unique_ptr<EditCommand> command) {
		if (!command) return;

		// A fresh edit invalidates the redo branch.
		m_Redo.clear();

		if (!m_Undo.empty() && m_Undo.back()->TryMergeWith(*command)) {
			return;
		}

		m_Undo.push_back(std::move(command));

		if (m_Undo.size() > k_MaxDepth) {
			m_Undo.erase(m_Undo.begin(), m_Undo.begin() + (m_Undo.size() - k_MaxDepth));
		}
	}

	std::string UndoStack::Undo(Scene& scene) {
		if (m_Undo.empty()) return {};
		std::unique_ptr<EditCommand> command = std::move(m_Undo.back());
		m_Undo.pop_back();
		std::string label = command->Label();
		command->Undo(scene);
		m_Redo.push_back(std::move(command));
		return label;
	}

	std::string UndoStack::Redo(Scene& scene) {
		if (m_Redo.empty()) return {};
		std::unique_ptr<EditCommand> command = std::move(m_Redo.back());
		m_Redo.pop_back();
		std::string label = command->Label();
		command->Redo(scene);
		m_Undo.push_back(std::move(command));
		return label;
	}

	void UndoStack::Clear() {
		m_Undo.clear();
		m_Redo.clear();
	}
}
