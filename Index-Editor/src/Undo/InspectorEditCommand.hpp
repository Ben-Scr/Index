#pragma once
#include "Undo/EditCommand.hpp"
#include "Inspector/PropertyValue.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace Index {
	class Entity;

	// One undo step for an inspector field edit across one or more entities.
	// Replays the descriptor's own Set lambda (captured by value, safe to store)
	// with the before/after PropertyValue per entity, resolved by persistent id.
	class InspectorEditCommand : public EditCommand {
	public:
		struct Entry {
			uint64_t PersistentId = 0;
			PropertyValue Before;
			PropertyValue After;
		};
		using ApplyFn = std::function<void(Entity&, const PropertyValue&)>;

		InspectorEditCommand(std::vector<Entry> entries, ApplyFn apply, std::string label)
			: m_Entries(std::move(entries)), m_Apply(std::move(apply)), m_Label(std::move(label)) {}

		bool HasChange() const;

		void Undo(Scene& scene) override;
		void Redo(Scene& scene) override;
		const char* Name() const override { return "Property"; }
		std::string Label() const override { return m_Label.empty() ? "Edit Property" : ("Edit " + m_Label); }

	private:
		void Restore(Scene& scene, bool redo);
		std::vector<Entry> m_Entries;
		ApplyFn m_Apply;
		std::string m_Label;
	};
}
