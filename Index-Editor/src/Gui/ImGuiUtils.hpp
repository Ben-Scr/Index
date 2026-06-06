#pragma once
#include <imgui.h>
#include "Collections/Color.hpp"
#include "Collections/Vec2.hpp"
#include "Scene/Entity.hpp"
#include <magic_enum/magic_enum.hpp>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <functional>
#include <span>
#include <string>
#include <type_traits>

namespace Index { class Texture2D; struct Curve; }

namespace Index::ImGuiUtils {
	float GetInspectorLabelColumnWidth();
	void BeginInspectorFieldRow(const char* label);
	void DrawInspectorLabel(const char* label);

	void MarkSelectionDirty(std::span<const Entity> entities);

	template<typename Draw>
	bool DrawInspectorControl(const char* label, Draw&& draw)
	{
		ImGui::PushID(label);
		BeginInspectorFieldRow(label);
		const bool changed = draw("##Value");
		ImGui::PopID();
		return changed;
	}

	template<typename Draw>
	void DrawInspectorValue(const char* label, Draw&& draw)
	{
		ImGui::PushID(label);
		BeginInspectorFieldRow(label);
		draw("##Value");
		ImGui::PopID();
	}

	inline Color DrawColorPick4(const char* label, const Color& color)
	{
		float values[4] = { color.r, color.g, color.b, color.a };
		DrawInspectorValue(label, [&values](const char* id) {
			ImGui::ColorEdit4(id, values);
		});
		return Color(values[0], values[1], values[2], values[3]);
	}
	template<typename Draw>
	void DrawEnabled(bool enabled, Draw&& draw) {
				if (!enabled) {
			ImGui::BeginDisabled();
		}

		draw();

		if (!enabled) {
			ImGui::EndDisabled();
		}
	}

	template<typename TEnum, typename Setter>
	bool DrawEnumCombo(const char* label, TEnum currentValue, Setter&& setter)
	{
		static_assert(std::is_enum_v<TEnum>, "DrawEnumCombo requires an enum type.");

		auto previewView = magic_enum::enum_name(currentValue);
		const char* preview = previewView.empty() ? "Unknown" : previewView.data();

		bool changed = false;

		ImGui::PushID(label);
		BeginInspectorFieldRow(label);
		if (ImGui::BeginCombo("##Value", preview)) {
			for (TEnum value : magic_enum::enum_values<TEnum>()) {
				const bool isSelected = (currentValue == value);

				auto name = magic_enum::enum_name(value);
				if (name.empty()) {
					continue;
				}

				if (ImGui::Selectable(name.data(), isSelected)) {
					setter(value);
					changed = true;
				}

				if (isSelected) {
					ImGui::SetItemDefaultFocus();
				}
			}

			ImGui::EndCombo();
		}
		ImGui::PopID();

		return changed;
	}

	template<typename TVec2>
	void DrawVec2ReadOnly(const char* id, const TVec2& vec2)
	{
		float values[2] = { static_cast<float>(vec2.x), static_cast<float>(vec2.y) };
		DrawInspectorValue(id, [&values](const char*) {
			const float componentWidth = std::max(45.0f, (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f);
			ImGui::BeginDisabled();
			ImGui::SetNextItemWidth(componentWidth);
			ImGui::InputFloat("##X", &values[0], 0.0f, 0.0f, "%.3f", ImGuiInputTextFlags_ReadOnly);
			ImGui::SameLine();
			ImGui::SetNextItemWidth(componentWidth);
			ImGui::InputFloat("##Y", &values[1], 0.0f, 0.0f, "%.3f", ImGuiInputTextFlags_ReadOnly);
			ImGui::EndDisabled();
		});
	}

	template<typename TVec2>
	bool DrawVec2(const char* id, TVec2& vec2)
	{
		float values[2] = { vec2.x, vec2.y };
		bool changed = false;
		DrawInspectorValue(id, [&values, &changed](const char*) {
			const float componentWidth = std::max(45.0f, (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f);
			ImGui::SetNextItemWidth(componentWidth);
			changed |= ImGui::InputFloat("##X", &values[0], 0.0f, 0.0f, "%.3f");
			ImGui::SameLine();
			ImGui::SetNextItemWidth(componentWidth);
			changed |= ImGui::InputFloat("##Y", &values[1], 0.0f, 0.0f, "%.3f");
		});

		if (changed) {
			vec2.x = values[0];
			vec2.y = values[1];
		}

		return changed;
	}

	void DrawTexturePreview(uint64_t rendererId, float texWidth, float texHeight, float previewSize = 96.0f);

	void DrawTexturePreview(const Texture2D& tex, float previewSize = 96.0f);

	// Empty-slot stand-in for a texture preview: checkerboard panel, border, and
	// a centered "No Texture" label. Use wherever a preview would draw but no
	// texture is assigned, so the slot is visible before one is applied.
	void DrawTexturePlaceholder(float previewSize = 96.0f);

	// Reusable editable curve graph for Index::Curve. Draggable keys + per-key in/out
	// tangent handles, double-click to add a point, right-click context menu
	// (reset / delete / copy / paste values — the clipboard is shared so values copy
	// between any graphs). Hold Left-Ctrl while dragging to snap to 0.15 increments.
	// When 'enabled' is false the graph is drawn greyed-out and non-interactive (so a
	// disabled feature still shows its curve instead of vanishing). Returns true if
	// the curve was edited this frame.
	bool DrawCurveEditor(const char* id, Curve& curve, bool enabled = true);

	void CenterNextModal();
	std::string Ellipsize(const std::string& text, float maxWidth, bool* outTruncated = nullptr);
	void TextEllipsis(const std::string& text, float maxWidth = -1.0f);
	void TextDisabledEllipsis(const std::string& text, float maxWidth = -1.0f);
	bool SelectableEllipsis(const std::string& text, const char* id, bool selected = false,
		ImGuiSelectableFlags flags = 0, const ImVec2& size = ImVec2(0.0f, 0.0f), float maxWidth = -1.0f);
	bool MenuItemEllipsis(const std::string& text, const char* id,
		const char* shortcut = nullptr, bool selected = false, bool enabled = true, float maxWidth = -1.0f);

	bool BeginComponentSection(const char* label, bool& removeRequested, const std::function<void()>& contextMenu = {});

	void EndComponentSection();

	namespace MultiEdit {

		template <typename T, typename Getter>
		bool SampleUniform(std::span<const Entity> entities, Getter&& get, T& out) {
			if (entities.empty()) {
				out = T{};
				return true;
			}
			out = get(entities[0]);
			for (std::size_t i = 1; i < entities.size(); ++i) {
				T v = get(entities[i]);
				if (!(v == out)) {
					return false;
				}
			}
			return true;
		}

		template <std::size_t N, typename ChannelGet>
		void SamplePerChannel(std::span<const Entity> entities, ChannelGet&& getChan,
			float (&outValues)[N], std::array<bool, N>& mixedMask)
		{
			for (std::size_t c = 0; c < N; ++c) {
				mixedMask[c] = false;
			}
			if (entities.empty()) {
				for (std::size_t c = 0; c < N; ++c) outValues[c] = 0.0f;
				return;
			}
			for (std::size_t c = 0; c < N; ++c) {
				outValues[c] = getChan(entities[0], c);
			}
			for (std::size_t i = 1; i < entities.size(); ++i) {
				for (std::size_t c = 0; c < N; ++c) {
					if (!mixedMask[c]) {
						const float v = getChan(entities[i], c);
						if (v != outValues[c]) {
							mixedMask[c] = true;
						}
					}
				}
			}
		}

		template <typename Body>
		inline bool MultiItemRow(int components, Body&& body) {
			const ImGuiStyle& style = ImGui::GetStyle();
			const float fullWidth = ImGui::GetContentRegionAvail().x;
			const float spacing = style.ItemInnerSpacing.x;
			const float wItemOne = std::max(1.0f, std::floor(
				(fullWidth - spacing * static_cast<float>(components - 1)) / static_cast<float>(components)));
			const float wItemLast = std::max(1.0f, std::floor(
				fullWidth - (wItemOne + spacing) * static_cast<float>(components - 1)));
			bool any = false;
			for (int c = 0; c < components; ++c) {
				if (c > 0) ImGui::SameLine(0.0f, spacing);
				ImGui::SetNextItemWidth(c == components - 1 ? wItemLast : wItemOne);
				any = body(c) || any;
			}
			return any;
		}
	}

	template <typename Getter, typename Setter>
	bool DragFloatMulti(const char* label, std::span<const Entity> entities,
		Getter&& get, Setter&& set,
		float speed = 0.1f, float vmin = 0.0f, float vmax = 0.0f,
		const char* format = "%.3f")
	{
		float v = 0.0f;
		const bool uniform = MultiEdit::SampleUniform<float>(entities, get, v);
		ImGui::PushID(label);
		BeginInspectorFieldRow(label);
		const bool changed = ImGui::DragFloat("##Value", &v, speed, vmin, vmax, uniform ? format : "-");
		if (changed) {
			for (const Entity& e : entities) set(e, v);
			MarkSelectionDirty(entities);
		}
		ImGui::PopID();
		return changed;
	}

	template <typename Getter, typename Setter>
	bool SliderFloatMulti(const char* label, std::span<const Entity> entities,
		Getter&& get, Setter&& set,
		float vmin, float vmax, const char* format = "%.3f")
	{
		float v = 0.0f;
		const bool uniform = MultiEdit::SampleUniform<float>(entities, get, v);
		ImGui::PushID(label);
		BeginInspectorFieldRow(label);
		const bool changed = ImGui::SliderFloat("##Value", &v, vmin, vmax, uniform ? format : "-");
		if (changed) {
			for (const Entity& e : entities) set(e, v);
			MarkSelectionDirty(entities);
		}
		ImGui::PopID();
		return changed;
	}

	template <typename Getter, typename Setter>
	bool InputFloatMulti(const char* label, std::span<const Entity> entities,
		Getter&& get, Setter&& set,
		float step = 0.0f, float stepFast = 0.0f, const char* format = "%.3f")
	{
		float v = 0.0f;
		const bool uniform = MultiEdit::SampleUniform<float>(entities, get, v);
		ImGui::PushID(label);
		BeginInspectorFieldRow(label);
		const bool changed = ImGui::InputFloat("##Value", &v, step, stepFast, uniform ? format : "-");
		if (changed) {
			for (const Entity& e : entities) set(e, v);
			MarkSelectionDirty(entities);
		}
		ImGui::PopID();
		return changed;
	}

	template <typename Getter, typename Setter>
	bool DragIntMulti(const char* label, std::span<const Entity> entities,
		Getter&& get, Setter&& set,
		float speed = 1.0f, int vmin = 0, int vmax = 0,
		const char* format = "%d")
	{
		int v = 0;
		const bool uniform = MultiEdit::SampleUniform<int>(entities, get, v);
		ImGui::PushID(label);
		BeginInspectorFieldRow(label);
		const bool changed = ImGui::DragInt("##Value", &v, speed, vmin, vmax, uniform ? format : "-");
		if (changed) {
			for (const Entity& e : entities) set(e, v);
			MarkSelectionDirty(entities);
		}
		ImGui::PopID();
		return changed;
	}

	template <typename Getter, typename Setter>
	bool InputIntMulti(const char* label, std::span<const Entity> entities,
		Getter&& get, Setter&& set,
		int step = 1, int stepFast = 100)
	{
		int v = 0;
		const bool uniform = MultiEdit::SampleUniform<int>(entities, get, v);
		ImGui::PushID(label);
		BeginInspectorFieldRow(label);
		// InputInt has no format-string knob; use InputTextWithHint to show "—" when mixed.
		bool changed = false;
		if (uniform) {
			changed = ImGui::InputInt("##Value", &v, step, stepFast);
		}
		else {
			char buf[32] = "";
			const bool committed = ImGui::InputTextWithHint("##Value", "-", buf, sizeof(buf),
				ImGuiInputTextFlags_CharsDecimal | ImGuiInputTextFlags_EnterReturnsTrue);
			if (committed && buf[0] != '\0') {
				v = std::atoi(buf);
				changed = true;
			}
		}
		if (changed) {
			for (const Entity& e : entities) set(e, v);
			MarkSelectionDirty(entities);
		}
		ImGui::PopID();
		return changed;
	}

	template <typename T, typename Getter, typename Setter>
	bool DragScalarMulti(const char* label, std::span<const Entity> entities,
		ImGuiDataType dataType, Getter&& get, Setter&& set,
		float speed = 1.0f, const T* vmin = nullptr, const T* vmax = nullptr,
		const char* format = nullptr)
	{
		T v{};
		const bool uniform = MultiEdit::SampleUniform<T>(entities, get, v);
		ImGui::PushID(label);
		BeginInspectorFieldRow(label);
		const bool changed = ImGui::DragScalar("##Value", dataType, &v, speed,
			vmin, vmax, uniform ? format : "-");
		if (changed) {
			for (const Entity& e : entities) set(e, v);
			MarkSelectionDirty(entities);
		}
		ImGui::PopID();
		return changed;
	}

	template <typename T, typename Getter, typename Setter>
	bool InputScalarMulti(const char* label, std::span<const Entity> entities,
		ImGuiDataType dataType, Getter&& get, Setter&& set,
		const char* format = nullptr)
	{
		T v{};
		const bool uniform = MultiEdit::SampleUniform<T>(entities, get, v);
		ImGui::PushID(label);
		BeginInspectorFieldRow(label);
		bool changed = false;
		if (uniform) {
			changed = ImGui::InputScalar("##Value", dataType, &v, nullptr, nullptr, format);
		}
		else {
			char buf[64] = "";
			const bool committed = ImGui::InputTextWithHint("##Value", "-", buf, sizeof(buf),
				ImGuiInputTextFlags_CharsDecimal | ImGuiInputTextFlags_EnterReturnsTrue);
			if (committed && buf[0] != '\0') {
				if (dataType == ImGuiDataType_U64) {
					unsigned long long parsed = std::strtoull(buf, nullptr, 10);
					std::memcpy(&v, &parsed, sizeof(v) < sizeof(parsed) ? sizeof(v) : sizeof(parsed));
				}
				else if (dataType == ImGuiDataType_S64) {
					long long parsed = std::strtoll(buf, nullptr, 10);
					std::memcpy(&v, &parsed, sizeof(v) < sizeof(parsed) ? sizeof(v) : sizeof(parsed));
				}
				else {
					int parsed = std::atoi(buf);
					std::memcpy(&v, &parsed, sizeof(v) < sizeof(parsed) ? sizeof(v) : sizeof(parsed));
				}
				changed = true;
			}
		}
		if (changed) {
			for (const Entity& e : entities) set(e, v);
			MarkSelectionDirty(entities);
		}
		ImGui::PopID();
		return changed;
	}

	template <typename Getter, typename Setter>
	bool CheckboxMulti(const char* label, std::span<const Entity> entities,
		Getter&& get, Setter&& set)
	{
		bool v = false;
		const bool uniform = MultiEdit::SampleUniform<bool>(entities, get, v);
		ImGui::PushID(label);
		BeginInspectorFieldRow(label);
		// ImGuiItemFlags_MixedValue = 1<<12: part of the public ABI, used by Checkbox for the indeterminate dash.
		constexpr int kMixedValueFlag = 1 << 12;
		if (!uniform) {
			ImGui::PushItemFlag(static_cast<ImGuiItemFlags>(kMixedValueFlag), true);
		}
		bool tmp = v;
		const bool changed = ImGui::Checkbox("##Value", &tmp);
		if (!uniform) {
			ImGui::PopItemFlag();
		}
		if (changed) {
			for (const Entity& e : entities) set(e, tmp);
			MarkSelectionDirty(entities);
		}
		ImGui::PopID();
		return changed;
	}

	template <typename Getter, typename Setter>
	bool InputTextMulti(const char* label, std::span<const Entity> entities,
		Getter&& get, Setter&& set)
	{
		std::string sample;
		const bool uniform = MultiEdit::SampleUniform<std::string>(entities, get, sample);
		ImGui::PushID(label);
		BeginInspectorFieldRow(label);
		char buf[256]{};
		if (uniform) {
			std::snprintf(buf, sizeof(buf), "%s", sample.c_str());
		}
		const bool changed = uniform
			? ImGui::InputText("##Value", buf, sizeof(buf))
			: ImGui::InputTextWithHint("##Value", "-", buf, sizeof(buf));
		if (changed) {
			std::string newValue(buf);
			for (const Entity& e : entities) set(e, newValue);
			MarkSelectionDirty(entities);
		}
		ImGui::PopID();
		return changed;
	}

	template <typename ChannelGet, typename ChannelSet>
	bool DragFloatNMulti(const char* label, std::span<const Entity> entities,
		int componentCount, ChannelGet&& getChan, ChannelSet&& setChan,
		float speed = 0.1f, float vmin = 0.0f, float vmax = 0.0f,
		const char* format = "%.3f")
	{
		if (componentCount < 1 || componentCount > 4) return false;

		float values[4] = {};
		std::array<bool, 4> mixedMask{};
		for (int c = 0; c < componentCount; ++c) mixedMask[c] = false;
		if (!entities.empty()) {
			for (int c = 0; c < componentCount; ++c) values[c] = getChan(entities[0], c);
			for (std::size_t i = 1; i < entities.size(); ++i) {
				for (int c = 0; c < componentCount; ++c) {
					if (!mixedMask[c]) {
						const float v = getChan(entities[i], c);
						if (v != values[c]) mixedMask[c] = true;
					}
				}
			}
		}

		ImGui::PushID(label);
		BeginInspectorFieldRow(label);
		const bool anyChanged = MultiEdit::MultiItemRow(componentCount, [&](int c) -> bool {
			ImGui::PushID(c);
			// Only write this channel when it actually changed; don't broadcast other channels' mixed values.
			const float pre = values[c];
			float channelValue = pre;
			const char* fmt = mixedMask[c] ? "-" : format;
			const bool committed = ImGui::DragFloat("##c", &channelValue, speed, vmin, vmax, fmt);
			const bool changed = committed && channelValue != pre;
			if (changed) {
				for (const Entity& e : entities) setChan(e, c, channelValue);
				MarkSelectionDirty(entities);
			}
			ImGui::PopID();
			return changed;
		});
		ImGui::PopID();
		return anyChanged;
	}

	template <typename ChannelGet, typename ChannelSet>
	bool ColorEdit4Multi(const char* label, std::span<const Entity> entities,
		ChannelGet&& getChan, ChannelSet&& setChan,
		ImGuiColorEditFlags flags = 0)
	{
		float values[4] = {};
		std::array<bool, 4> mixedMask{};
		MultiEdit::SamplePerChannel<4>(entities, getChan, values, mixedMask);
		const bool anyMixed = mixedMask[0] || mixedMask[1] || mixedMask[2] || mixedMask[3];

		ImGui::PushID(label);
		BeginInspectorFieldRow(label);
		bool anyChanged = false;
		if (!anyMixed) {
			float editVals[4] = { values[0], values[1], values[2], values[3] };
			if (ImGui::ColorEdit4("##Value", editVals, flags)) {
				for (int c = 0; c < 4; ++c) {
					if (editVals[c] != values[c]) {
						const float nv = editVals[c];
						for (const Entity& e : entities) setChan(e, c, nv);
						anyChanged = true;
					}
				}
				if (anyChanged) MarkSelectionDirty(entities);
			}
		}
		else {
			// Per-channel drag fallback: ColorEdit4 can't show per-channel mixed state.
			anyChanged = MultiEdit::MultiItemRow(4, [&](int c) -> bool {
				ImGui::PushID(c);
				const float pre = values[c];
				float channelValue = pre;
				const char* fmt = mixedMask[c] ? "-" : "%.3f";
				const bool committed = ImGui::DragFloat("##c", &channelValue, 0.005f, 0.0f, 1.0f, fmt);
				const bool changed = committed && channelValue != pre;
				if (changed) {
					for (const Entity& e : entities) setChan(e, c, channelValue);
					MarkSelectionDirty(entities);
				}
				ImGui::PopID();
				return changed;
			});
		}
		ImGui::PopID();
		return anyChanged;
	}

	template <typename TEnum, typename Getter, typename Setter>
	bool EnumComboMulti(const char* label, std::span<const Entity> entities,
		Getter&& get, Setter&& set)
	{
		static_assert(std::is_enum_v<TEnum>, "EnumComboMulti requires an enum type.");

		TEnum sampled{};
		const bool uniform = MultiEdit::SampleUniform<TEnum>(entities, get, sampled);

		const char* preview;
		std::string previewBuf;
		if (uniform) {
			auto view = magic_enum::enum_name(sampled);
			previewBuf.assign(view.data(), view.size());
			preview = previewBuf.empty() ? "Unknown" : previewBuf.c_str();
		}
		else {
			preview = "-";
		}

		bool changed = false;
		ImGui::PushID(label);
		BeginInspectorFieldRow(label);
		if (ImGui::BeginCombo("##Value", preview)) {
			for (TEnum value : magic_enum::enum_values<TEnum>()) {
				auto name = magic_enum::enum_name(value);
				if (name.empty()) continue;
				const bool isSelected = uniform && (sampled == value);
				if (ImGui::Selectable(std::string(name).c_str(), isSelected)) {
					for (const Entity& e : entities) set(e, value);
					MarkSelectionDirty(entities);
					changed = true;
				}
				if (isSelected) {
					ImGui::SetItemDefaultFocus();
				}
			}
			ImGui::EndCombo();
		}
		ImGui::PopID();
		return changed;
	}

	template <typename Getter, typename Setter>
	bool ComboMulti(const char* label, std::span<const Entity> entities,
		Getter&& get, Setter&& set,
		const char* const* itemNames, int itemCount)
	{
		int sampled = 0;
		const bool uniform = MultiEdit::SampleUniform<int>(entities, get, sampled);
		const char* preview = "-";
		if (uniform && sampled >= 0 && sampled < itemCount) {
			preview = itemNames[sampled];
		}
		bool changed = false;
		ImGui::PushID(label);
		BeginInspectorFieldRow(label);
		if (ImGui::BeginCombo("##Value", preview)) {
			for (int i = 0; i < itemCount; ++i) {
				const bool isSelected = uniform && (sampled == i);
				if (ImGui::Selectable(itemNames[i], isSelected)) {
					for (const Entity& e : entities) set(e, i);
					MarkSelectionDirty(entities);
					changed = true;
				}
				if (isSelected) {
					ImGui::SetItemDefaultFocus();
				}
			}
			ImGui::EndCombo();
		}
		ImGui::PopID();
		return changed;
	}
}
