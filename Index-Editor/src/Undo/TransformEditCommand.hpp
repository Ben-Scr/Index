#pragma once
#include "Undo/EditCommand.hpp"
#include "Collections/Vec2.hpp"
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace Index {

	// Full Transform2DComponent state. Both world and local are kept: the gizmo
	// writes both, and restoring only one would desync them until the next
	// hierarchy pass.
	struct TransformSnapshot {
		Vec2 Position{ 0.0f, 0.0f };
		Vec2 Scale{ 1.0f, 1.0f };
		float Rotation = 0.0f;
		Vec2 LocalPosition{ 0.0f, 0.0f };
		Vec2 LocalScale{ 1.0f, 1.0f };
		float LocalRotation = 0.0f;
	};

	// One undo step for a gizmo drag spanning one or more selected entities.
	class TransformEditCommand : public EditCommand {
	public:
		struct Entry {
			uint64_t PersistentId = 0;
			TransformSnapshot Before;
			TransformSnapshot After;
		};

		explicit TransformEditCommand(std::vector<Entry> entries, std::string label = "Transform")
			: m_Entries(std::move(entries)), m_Label(std::move(label)) {}

		// False when nothing actually moved, so a click-without-drag never pushes
		// an empty step.
		bool HasChange() const;

		void Undo(Scene& scene) override;
		void Redo(Scene& scene) override;
		const char* Name() const override { return "Transform"; }
		std::string Label() const override { return m_Label; }

	private:
		void Restore(Scene& scene, bool redo);
		std::vector<Entry> m_Entries;
		std::string m_Label;
	};
}
