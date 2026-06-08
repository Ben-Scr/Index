#pragma once
#include "Inspector/PropertyDescriptor.hpp"
#include "Scene/Entity.hpp"

#include <span>

namespace Index {
	class UndoStack;

	// Coalesces inspector property edits into one undo step per widget gesture.
	// PropertyDrawer's Write* funcs bracket each write with CaptureBefore/After
	// (keyed on the active ImGui widget id); the editor calls Flush once per
	// frame to commit gestures that have ended into the undo stack.
	namespace InspectorUndoRecorder {
		void CaptureBefore(std::span<const Entity> entities, const PropertyDescriptor& descriptor);
		void CaptureAfter(std::span<const Entity> entities, const PropertyDescriptor& descriptor);
		void Flush(UndoStack& stack);
		void Reset();
	}
}
