#include <pch.hpp>
#include "Undo/InspectorUndoRecorder.hpp"

#include "Undo/InspectorEditCommand.hpp"
#include "Undo/UndoStack.hpp"
#include "Scene/Scene.hpp"

#include <imgui.h>
#include <imgui_internal.h> // ImGui::GetActiveID — coalesces a gesture by widget

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace Index::InspectorUndoRecorder {

	namespace {
		struct State {
			bool Active = false;
			ImGuiID ActiveId = 0;          // widget being edited; a change ends the gesture
			std::string Name;
			InspectorEditCommand::ApplyFn Apply;
			std::vector<uint64_t> Ids;
			std::vector<PropertyValue> Old; // captured pre-write on the gesture's first frame
			std::vector<PropertyValue> New; // refreshed each write; the last is the final value
			std::vector<std::unique_ptr<EditCommand>> Queue;
		};

		State& Get() { static State s; return s; }

		void Finalize() {
			State& s = Get();
			if (!s.Active) return;
			s.Active = false;

			const std::size_t n = std::min({ s.Ids.size(), s.Old.size(), s.New.size() });
			std::vector<InspectorEditCommand::Entry> entries;
			for (std::size_t i = 0; i < n; ++i) {
				if (s.Ids[i] == 0) continue;
				if (s.Old[i] == s.New[i]) continue;
				entries.push_back({ s.Ids[i], s.Old[i], s.New[i] });
			}
			s.Old.clear(); s.New.clear(); s.Ids.clear();

			if (entries.empty() || !s.Apply) return;
			s.Queue.push_back(std::make_unique<InspectorEditCommand>(std::move(entries), s.Apply, s.Name));
		}
	}

	void CaptureBefore(std::span<const Entity> entities, const PropertyDescriptor& descriptor) {
		State& s = Get();
		const ImGuiID id = ImGui::GetActiveID();
		// A different active widget means the previous gesture ended.
		if (s.Active && id != s.ActiveId) {
			Finalize();
		}
		if (s.Active) return;

		s.Active = true;
		s.ActiveId = id;
		s.Name = descriptor.DisplayName.empty() ? descriptor.Name : descriptor.DisplayName;
		s.Apply = descriptor.Set;
		s.Ids.clear(); s.Old.clear(); s.New.clear();
		s.Ids.reserve(entities.size());
		s.Old.reserve(entities.size());
		for (const Entity& e : entities) {
			Scene* scene = const_cast<Entity&>(e).GetScene();
			s.Ids.push_back(scene ? scene->GetEntityPersistentID(e.GetHandle()) : 0);
			s.Old.push_back(descriptor.Get(e));
		}
	}

	void CaptureAfter(std::span<const Entity> entities, const PropertyDescriptor& descriptor) {
		State& s = Get();
		if (!s.Active) return;
		s.New.clear();
		s.New.reserve(entities.size());
		for (const Entity& e : entities) {
			s.New.push_back(descriptor.Get(e));
		}
	}

	void Flush(UndoStack& stack) {
		State& s = Get();
		// A pending gesture with no active widget has been released — commit it.
		if (s.Active && !ImGui::IsAnyItemActive()) {
			Finalize();
		}
		for (auto& cmd : s.Queue) {
			stack.Push(std::move(cmd));
		}
		s.Queue.clear();
	}

	void Reset() {
		State& s = Get();
		s.Active = false;
		s.ActiveId = 0;
		s.Apply = nullptr;
		s.Name.clear();
		s.Ids.clear(); s.Old.clear(); s.New.clear();
		s.Queue.clear();
	}
}
