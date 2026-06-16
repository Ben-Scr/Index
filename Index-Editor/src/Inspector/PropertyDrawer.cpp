#include "pch.hpp"
#include "Inspector/PropertyDrawer.hpp"

#include "Assets/AssetRegistry.hpp"
#include "Collections/Curve.hpp"
#include "Gui/AssetBrowser.hpp"
#include "Gui/EditorIcons.hpp"
#include "Gui/HierarchyDragData.hpp"
#include "Gui/ImGuiFonts.hpp"
#include "Gui/ImGuiUtils.hpp"
#include "Gui/SpriteSliceDragPayload.hpp"
#include "Inspector/PropertyType.hpp"
#include "Inspector/PropertyValue.hpp"
#include "Inspector/ReferencePicker.hpp"
#include "Scene/ComponentRegistry.hpp"
#include "Scene/Scene.hpp"
#include "Scene/SceneManager.hpp"
#include "Undo/InspectorUndoRecorder.hpp"
#include "Utils/ExpressionEvaluator.hpp"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Index::PropertyDrawer {

	namespace {

		constexpr int kMixedValueFlag = 1 << 12; // ImGuiItemFlags_MixedValue (internal)

		bool SampleUniform(std::span<const Entity> entities, const PropertyDescriptor& d,
			PropertyValue& outValue)
		{
			if (entities.empty()) {
				outValue = PropertyValue{};
				outValue.Type = d.Type;
				return true;
			}
			outValue = d.Get(entities[0]);
			for (std::size_t i = 1; i < entities.size(); ++i) {
				const PropertyValue other = d.Get(entities[i]);
				if (!(other == outValue)) return false;
			}
			return true;
		}

		// For per-channel float vectors: returns the sampled values + a
		// per-channel mixed mask.
		template <std::size_t N>
		void SampleFloatChannels(std::span<const Entity> entities,
			const PropertyDescriptor& d,
			std::array<float, N>& outValues, std::array<bool, N>& mixedMask)
		{
			for (std::size_t c = 0; c < N; ++c) mixedMask[c] = false;
			if (entities.empty()) {
				for (std::size_t c = 0; c < N; ++c) outValues[c] = 0.0f;
				return;
			}
			const PropertyValue first = d.Get(entities[0]);
			for (std::size_t c = 0; c < N; ++c) outValues[c] = first.FloatVec[c];
			for (std::size_t i = 1; i < entities.size(); ++i) {
				const PropertyValue v = d.Get(entities[i]);
				for (std::size_t c = 0; c < N; ++c) {
					if (!mixedMask[c] && v.FloatVec[c] != outValues[c]) {
						mixedMask[c] = true;
					}
				}
			}
		}

		template <std::size_t N>
		void SampleIntChannels(std::span<const Entity> entities,
			const PropertyDescriptor& d,
			std::array<int32_t, N>& outValues, std::array<bool, N>& mixedMask)
		{
			for (std::size_t c = 0; c < N; ++c) mixedMask[c] = false;
			if (entities.empty()) {
				for (std::size_t c = 0; c < N; ++c) outValues[c] = 0;
				return;
			}
			const PropertyValue first = d.Get(entities[0]);
			for (std::size_t c = 0; c < N; ++c) outValues[c] = first.IntVec[c];
			for (std::size_t i = 1; i < entities.size(); ++i) {
				const PropertyValue v = d.Get(entities[i]);
				for (std::size_t c = 0; c < N; ++c) {
					if (!mixedMask[c] && v.IntVec[c] != outValues[c]) {
						mixedMask[c] = true;
					}
				}
			}
		}

		inline void MarkSceneDirty(const Entity& entity) {
			if (Scene* scene = const_cast<Entity&>(entity).GetScene()) {
				scene->MarkDirty();
			}
		}

		const Scene* ResolveHierarchyPayloadScene(const HierarchyDragData& data) {
			const Scene* resolved = nullptr;
			const uint64_t sourceSceneId = data.SourceSceneId;
			if (sourceSceneId != 0) {
				SceneManager::Get().ForeachLoadedScene([&](const Scene& scene) {
					if (!resolved && static_cast<uint64_t>(scene.GetSceneId()) == sourceSceneId) {
						resolved = &scene;
					}
				});
			}
			return resolved ? resolved : SceneManager::Get().GetActiveScene();
		}

		// Write a value to every entity in the span via the descriptor.
		void WriteAll(std::span<const Entity> entities, const PropertyDescriptor& d,
			const PropertyValue& value)
		{
			InspectorUndoRecorder::CaptureBefore(entities, d);
			for (const Entity& e : entities) {
				d.Set(const_cast<Entity&>(e), value);
				MarkSceneDirty(e);
			}
			InspectorUndoRecorder::CaptureAfter(entities, d);
		}

		// Per-channel writes: only the edited channel is broadcast, so multi-select doesn't clobber other channels.
		void WriteFloatChannel(std::span<const Entity> entities,
			const PropertyDescriptor& d, std::size_t channel, float newValue)
		{
			InspectorUndoRecorder::CaptureBefore(entities, d);
			for (const Entity& e : entities) {
				PropertyValue current = d.Get(e);
				current.FloatVec[channel] = newValue;
				d.Set(const_cast<Entity&>(e), current);
				MarkSceneDirty(e);
			}
			InspectorUndoRecorder::CaptureAfter(entities, d);
		}
		void WriteIntChannel(std::span<const Entity> entities,
			const PropertyDescriptor& d, std::size_t channel, int32_t newValue)
		{
			InspectorUndoRecorder::CaptureBefore(entities, d);
			for (const Entity& e : entities) {
				PropertyValue current = d.Get(e);
				current.IntVec[channel] = newValue;
				d.Set(const_cast<Entity&>(e), current);
				MarkSceneDirty(e);
			}
			InspectorUndoRecorder::CaptureAfter(entities, d);
		}

		// Right-click "Clear" context menu for reference rows. Returns true
		// if the user picked Clear; the caller then writes a null reference.
		bool DrawReferenceContextMenu(const char* popupId) {
			bool cleared = false;
			if (ImGui::BeginPopupContextItem(popupId)) {
				if (ImGui::MenuItem("Clear")) cleared = true;
				ImGui::EndPopup();
			}
			return cleared;
		}

		bool DrawAssetLocateButton(bool enabled) {
			const float buttonSize = ImGui::GetFrameHeight();
			const ImVec2 size(buttonSize, buttonSize);
			if (!enabled) {
				ImGui::BeginDisabled();
			}

			const bool clicked = ImGui::Button("##LocateAssetInBrowser", size);
			const ImVec2 min = ImGui::GetItemRectMin();
			const ImVec2 max = ImGui::GetItemRectMax();
			const float iconSize = std::min(16.0f, std::max(8.0f, buttonSize - ImGui::GetStyle().FramePadding.y * 2.0f));
			const ImVec2 iconPos(
				min.x + (max.x - min.x - iconSize) * 0.5f,
				min.y + (max.y - min.y - iconSize) * 0.5f);
			const ImU32 tint = ImGui::GetColorU32(enabled ? ImGuiCol_Text : ImGuiCol_TextDisabled);
			ImDrawList* drawList = ImGui::GetWindowDrawList();
			const uint64_t icon = EditorIcons::Get("circle_dot", static_cast<int>(iconSize));
			if (icon != 0) {
				drawList->AddImage(
					static_cast<ImTextureID>(static_cast<intptr_t>(icon)),
					iconPos,
					ImVec2(iconPos.x + iconSize, iconPos.y + iconSize),
					ImVec2(0.0f, 1.0f),
					ImVec2(1.0f, 0.0f),
					tint);
			}
			else {
				const ImVec2 center(iconPos.x + iconSize * 0.5f, iconPos.y + iconSize * 0.5f);
				drawList->AddCircle(center, iconSize * 0.38f, tint, 0, std::max(1.0f, iconSize * 0.11f));
				drawList->AddCircleFilled(center, std::max(1.0f, iconSize * 0.12f), tint);
			}

			if (!enabled) {
				ImGui::EndDisabled();
			}
			if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
				ImGui::SetTooltip("%s", enabled ? "Select in Asset Browser" : "No asset selected");
			}
			return clicked && enabled;
		}

		uint64_t ResolveDroppedAssetId(const ImGuiPayload* payload) {
			if (!payload || !payload->Data || payload->DataSize <= 0) {
				return 0;
			}

			const char* data = static_cast<const char*>(payload->Data);
			std::string droppedPath(data, static_cast<std::size_t>(payload->DataSize));
			if (!droppedPath.empty() && droppedPath.back() == '\0') {
				droppedPath.pop_back();
			}
			if (droppedPath.empty()) {
				return 0;
			}

			const uint64_t assetId = AssetRegistry::GetOrCreateAssetUUID(droppedPath);
			if (assetId != 0) {
				return assetId;
			}

			AssetRegistry::MarkDirty();
			AssetRegistry::Sync();
			return AssetRegistry::GetOrCreateAssetUUID(droppedPath);
		}

		// ── Per-type drawers ─────────────────────────────────────────

		bool DrawBool(std::span<const Entity> entities, const PropertyDescriptor& d) {
			PropertyValue v;
			const bool uniform = SampleUniform(entities, d, v);
			ImGui::PushID(d.Name.c_str());
			ImGuiUtils::BeginInspectorFieldRow(d.DisplayName.c_str());
			if (!uniform) ImGui::PushItemFlag(static_cast<ImGuiItemFlags>(kMixedValueFlag), true);
			bool tmp = v.BoolValue;
			const bool changed = ImGui::Checkbox("##Value", &tmp);
			if (!uniform) ImGui::PopItemFlag();
			ImGui::PopID();
			if (changed) {
				PropertyValue out;
				out.Type = PropertyType::Bool;
				out.BoolValue = tmp;
				WriteAll(entities, d, out);
			}
			return changed;
		}

		bool DrawIntScalar(std::span<const Entity> entities, const PropertyDescriptor& d,
			int valueMin, int valueMax)
		{
			PropertyValue v;
			const bool uniform = SampleUniform(entities, d, v);
			int tmp = static_cast<int>(v.IntValue);
			const int clampMin = d.Metadata.HasClamp
				? std::max(static_cast<int>(d.Metadata.ClampMin), valueMin)
				: valueMin;
			const int clampMax = d.Metadata.HasClamp
				? std::min(static_cast<int>(d.Metadata.ClampMax), valueMax)
				: valueMax;
			ImGui::PushID(d.Name.c_str());
			ImGuiUtils::BeginInspectorFieldRow(d.DisplayName.c_str());
			bool changed = false;
			if (d.Metadata.HasClamp) {
				// SliderInt + InputInt combo: slider visualises [Min, Max]; input lets user type exact value.
				const float fullW = ImGui::CalcItemWidth();
				const float style = ImGui::GetStyle().ItemInnerSpacing.x;
				const float inputW = std::min(80.0f, fullW * 0.30f);
				const float sliderW = std::max(40.0f, fullW - inputW - style);
				ImGui::SetNextItemWidth(sliderW);
				changed |= ImGui::SliderInt("##Slider", &tmp, clampMin, clampMax,
					uniform ? "%d" : "-");
				ImGui::SameLine(0.0f, style);
				ImGui::SetNextItemWidth(inputW);
				changed |= ImGui::InputInt("##Value", &tmp, 0, 0,
					ImGuiInputTextFlags_CharsDecimal);
			}
			else {
				const float speed = d.Metadata.DragSpeed > 0 ? d.Metadata.DragSpeed : 1.0f;
				changed = ImGui::DragInt("##Value", &tmp, speed, clampMin, clampMax,
					uniform ? "%d" : "-");
			}
			ImGui::PopID();
			if (changed) {
				if (tmp < clampMin) tmp = clampMin;
				if (tmp > clampMax) tmp = clampMax;
				PropertyValue out;
				out.Type = d.Type;
				out.IntValue = tmp;
				WriteAll(entities, d, out);
			}
			return changed;
		}

		bool DrawUIntScalar(std::span<const Entity> entities, const PropertyDescriptor& d,
			uint32_t valueMax)
		{
			PropertyValue v;
			const bool uniform = SampleUniform(entities, d, v);
			int tmp = static_cast<int>(std::min<uint64_t>(v.UIntValue, INT_MAX));
			const int clampMin = d.Metadata.HasClamp
				? std::max(static_cast<int>(d.Metadata.ClampMin), 0)
				: 0;
			const int clampMax = d.Metadata.HasClamp
				? std::min(static_cast<int>(d.Metadata.ClampMax), static_cast<int>(std::min<uint32_t>(valueMax, INT_MAX)))
				: static_cast<int>(std::min<uint32_t>(valueMax, INT_MAX));
			ImGui::PushID(d.Name.c_str());
			ImGuiUtils::BeginInspectorFieldRow(d.DisplayName.c_str());
			const float speed = d.Metadata.DragSpeed > 0 ? d.Metadata.DragSpeed : 1.0f;
			const bool changed = ImGui::DragInt("##Value", &tmp, speed, clampMin, clampMax,
				uniform ? "%d" : "-");
			ImGui::PopID();
			if (changed) {
				if (tmp < clampMin) tmp = clampMin;
				if (tmp > clampMax) tmp = clampMax;
				PropertyValue out;
				out.Type = d.Type;
				out.UIntValue = static_cast<uint64_t>(tmp);
				WriteAll(entities, d, out);
			}
			return changed;
		}

		bool DrawScalar64(std::span<const Entity> entities, const PropertyDescriptor& d) {
			PropertyValue v;
			const bool uniform = SampleUniform(entities, d, v);
			char buf[64];
			if (d.Type == PropertyType::Int64) {
				if (uniform) std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(v.IntValue));
				else buf[0] = '\0';
			}
			else { // UInt64
				if (uniform) std::snprintf(buf, sizeof(buf), "%llu", static_cast<unsigned long long>(v.UIntValue));
				else buf[0] = '\0';
			}
			ImGui::PushID(d.Name.c_str());
			ImGuiUtils::BeginInspectorFieldRow(d.DisplayName.c_str());
			const bool committed = uniform
				? ImGui::InputText("##Value", buf, sizeof(buf), ImGuiInputTextFlags_CharsDecimal | ImGuiInputTextFlags_EnterReturnsTrue)
				: ImGui::InputTextWithHint("##Value", "-", buf, sizeof(buf), ImGuiInputTextFlags_CharsDecimal | ImGuiInputTextFlags_EnterReturnsTrue);
			ImGui::PopID();
			if (committed && buf[0] != '\0') {
				PropertyValue out;
				out.Type = d.Type;
				// Plain integers parse exactly via strto*; only inputs with operators
				// fall through to the evaluator, which is double-based — so a 64-bit
				// magnitude above 2^53 used *inside* an expression loses precision.
				// Plain entry of any 64-bit value stays exact. (CharsDecimal limits
				// typable operators to + - * / here; %, ^ and parens aren't reachable.)
				char* parseEnd = nullptr;
				if (d.Type == PropertyType::Int64) {
					const long long exact = std::strtoll(buf, &parseEnd, 10);
					while (*parseEnd == ' ') ++parseEnd;
					if (parseEnd != buf && *parseEnd == '\0') {
						out.IntValue = exact;
					}
					else {
						double r = 0.0;
						if (!ExpressionEvaluator::Evaluate(buf,
								uniform ? static_cast<double>(v.IntValue) : 0.0, r)) {
							return false;
						}
						r = std::round(r);
						out.IntValue = r >= 9223372036854775808.0 ? INT64_MAX
							: r <= -9223372036854775808.0 ? INT64_MIN
							: static_cast<int64_t>(r);
					}
				}
				else { // UInt64
					const unsigned long long exact = std::strtoull(buf, &parseEnd, 10);
					while (*parseEnd == ' ') ++parseEnd;
					if (parseEnd != buf && *parseEnd == '\0') {
						out.UIntValue = exact;
					}
					else {
						double r = 0.0;
						if (!ExpressionEvaluator::Evaluate(buf,
								uniform ? static_cast<double>(v.UIntValue) : 0.0, r)) {
							return false;
						}
						r = std::round(r);
						out.UIntValue = r <= 0.0 ? 0ull
							: r >= 18446744073709551616.0 ? UINT64_MAX
							: static_cast<uint64_t>(r);
					}
				}
				WriteAll(entities, d, out);
				return true;
			}
			return false;
		}

		bool DrawFloat(std::span<const Entity> entities, const PropertyDescriptor& d) {
			PropertyValue v;
			const bool uniform = SampleUniform(entities, d, v);
			float tmp = static_cast<float>(v.FloatValue);
			const float clampMin = d.Metadata.HasClamp ? static_cast<float>(d.Metadata.ClampMin) : 0.0f;
			const float clampMax = d.Metadata.HasClamp ? static_cast<float>(d.Metadata.ClampMax) : 0.0f;
			ImGui::PushID(d.Name.c_str());
			ImGuiUtils::BeginInspectorFieldRow(d.DisplayName.c_str());
			bool changed = false;
			if (d.Metadata.HasClamp && clampMax > clampMin) {
				// SliderFloat + InputFloat combo for [ClampValue] fields; DragFloat path stays for unbounded fields.
				const float fullW = ImGui::CalcItemWidth();
				const float style = ImGui::GetStyle().ItemInnerSpacing.x;
				const float inputW = std::min(80.0f, fullW * 0.30f);
				const float sliderW = std::max(40.0f, fullW - inputW - style);
				ImGui::SetNextItemWidth(sliderW);
				changed |= ImGui::SliderFloat("##Slider", &tmp, clampMin, clampMax,
					uniform ? "%.3f" : "-");
				ImGui::SameLine(0.0f, style);
				ImGui::SetNextItemWidth(inputW);
				changed |= ImGui::InputFloat("##Value", &tmp, 0.0f, 0.0f, "%.3f",
					ImGuiInputTextFlags_CharsDecimal);
				if (tmp < clampMin) tmp = clampMin;
				if (tmp > clampMax) tmp = clampMax;
			}
			else {
				const float speed = d.Metadata.DragSpeed > 0 ? d.Metadata.DragSpeed : 0.1f;
				// Soft drag clamp keeps a normal DragFloat but bounds the value (no slider).
				float dragMin = clampMin, dragMax = clampMax;
				ImGuiSliderFlags dragFlags = 0;
				if (d.Metadata.HasDragClamp) {
					dragMin = static_cast<float>(d.Metadata.DragClampMin);
					dragMax = static_cast<float>(d.Metadata.DragClampMax);
					dragFlags = ImGuiSliderFlags_AlwaysClamp;
				}
				changed = ImGui::DragFloat("##Value", &tmp, speed, dragMin, dragMax,
					uniform ? "%.3f" : "-", dragFlags);
			}
			ImGui::PopID();
			if (changed) {
				PropertyValue out;
				out.Type = d.Type;
				out.FloatValue = static_cast<double>(tmp);
				WriteAll(entities, d, out);
			}
			return changed;
		}

		bool DrawDouble(std::span<const Entity> entities, const PropertyDescriptor& d) {
			PropertyValue v;
			const bool uniform = SampleUniform(entities, d, v);
			double tmp = v.FloatValue;
			ImGui::PushID(d.Name.c_str());
			ImGuiUtils::BeginInspectorFieldRow(d.DisplayName.c_str());
			const float speed = d.Metadata.DragSpeed > 0 ? d.Metadata.DragSpeed : 0.1f;
			const bool changed = ImGui::DragScalar("##Value", ImGuiDataType_Double, &tmp,
				speed, nullptr, nullptr, uniform ? "%.6f" : "-");
			ImGui::PopID();
			if (changed) {
				PropertyValue out;
				out.Type = PropertyType::Double;
				out.FloatValue = tmp;
				WriteAll(entities, d, out);
			}
			return changed;
		}

		bool DrawString(std::span<const Entity> entities, const PropertyDescriptor& d) {
			PropertyValue v;
			const bool uniform = SampleUniform(entities, d, v);
			ImGui::PushID(d.Name.c_str());
			ImGuiUtils::BeginInspectorFieldRow(d.DisplayName.c_str());

			bool changed = false;
			if (d.Metadata.MultiLine) {
				static constexpr int k_MultiLineCapacity = 4096;
				// Static buffer: DrawString runs serially on the UI thread so no allocation per field per frame.
				static char buf[k_MultiLineCapacity];
				buf[0] = '\0';
				if (uniform) {
					std::snprintf(buf, sizeof(buf), "%s", v.StringValue.c_str());
				}
				const int rows = std::max(2, d.Metadata.MultiLineRows);
				const float lineHeight = ImGui::GetTextLineHeight();
				const ImVec2 size{ -FLT_MIN, lineHeight * static_cast<float>(rows) + 8.0f };
				changed = ImGui::InputTextMultiline("##Value", buf, sizeof(buf), size);
				if (changed) {
					PropertyValue out;
					out.Type = PropertyType::String;
					out.StringValue.assign(buf);
					WriteAll(entities, d, out);
				}
			}
			else {
				// 4096 covers realistic single-line use (paths, identifiers,
				// short prose). Multi-line strings should set MultiLineRows
				// in metadata to take the InputTextMultiline branch above.
				char buf[4096]{};
				if (uniform) std::snprintf(buf, sizeof(buf), "%s", v.StringValue.c_str());
				changed = uniform
					? ImGui::InputText("##Value", buf, sizeof(buf))
					: ImGui::InputTextWithHint("##Value", "-", buf, sizeof(buf));
				if (changed) {
					PropertyValue out;
					out.Type = PropertyType::String;
					out.StringValue = buf;
					WriteAll(entities, d, out);
				}
			}
			ImGui::PopID();
			return changed;
		}

		template <std::size_t N>
		bool DrawFloatVec(std::span<const Entity> entities, const PropertyDescriptor& d) {
			std::array<float, N> values{};
			std::array<bool, N> mixed{};
			SampleFloatChannels<N>(entities, d, values, mixed);
			ImGui::PushID(d.Name.c_str());
			ImGuiUtils::BeginInspectorFieldRow(d.DisplayName.c_str());
			const float speed = d.Metadata.DragSpeed > 0 ? d.Metadata.DragSpeed : 0.1f;
			const ImGuiStyle& style = ImGui::GetStyle();
			const float fullWidth = ImGui::GetContentRegionAvail().x;
			const float componentSpacing = style.ItemInnerSpacing.x;
			const float componentWidth = std::max(1.0f,
				(fullWidth - componentSpacing * static_cast<float>(N - 1)) / static_cast<float>(N));
			// Each channel is prefixed with its axis name (Transform-style); no +/- steppers.
			static constexpr const char* axisLabels[4] = { "X", "Y", "Z", "W" };
			bool any = false;
			for (int c = 0; c < static_cast<int>(N); ++c) {
				if (c > 0) ImGui::SameLine(0.0f, componentSpacing);
				ImGui::PushID(c);
				ImGui::AlignTextToFramePadding();
				ImGui::TextUnformatted(axisLabels[c]);
				ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);
				const float channelWidth = std::max(1.0f, componentWidth
					- ImGui::CalcTextSize(axisLabels[c]).x - style.ItemInnerSpacing.x);
				const float pre = values[c];
				float channel = pre;
				const char* fmt = mixed[c] ? "-" : "%.3f";
				ImGui::SetNextItemWidth(channelWidth);
				const bool channelChanged = ImGui::DragFloat("##c", &channel, speed, 0.0f, 0.0f, fmt);
				if (channelChanged && channel != pre) {
					WriteFloatChannel(entities, d, static_cast<std::size_t>(c), channel);
					any = true;
				}
				ImGui::PopID();
			}
			ImGui::PopID();
			return any;
		}

		template <std::size_t N>
		bool DrawIntVec(std::span<const Entity> entities, const PropertyDescriptor& d) {
			std::array<int32_t, N> values{};
			std::array<bool, N> mixed{};
			SampleIntChannels<N>(entities, d, values, mixed);
			ImGui::PushID(d.Name.c_str());
			ImGuiUtils::BeginInspectorFieldRow(d.DisplayName.c_str());
			const float speed = d.Metadata.DragSpeed > 0 ? d.Metadata.DragSpeed : 1.0f;
			const ImGuiStyle& style = ImGui::GetStyle();
			const float fullWidth = ImGui::GetContentRegionAvail().x;
			const float componentSpacing = style.ItemInnerSpacing.x;
			const float componentWidth = std::max(1.0f,
				(fullWidth - componentSpacing * static_cast<float>(N - 1)) / static_cast<float>(N));
			// Each channel is prefixed with its axis name (Transform-style); no +/- steppers.
			static constexpr const char* axisLabels[4] = { "X", "Y", "Z", "W" };
			bool any = false;
			for (int c = 0; c < static_cast<int>(N); ++c) {
				if (c > 0) ImGui::SameLine(0.0f, componentSpacing);
				ImGui::PushID(c);
				ImGui::AlignTextToFramePadding();
				ImGui::TextUnformatted(axisLabels[c]);
				ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);
				const float channelWidth = std::max(1.0f, componentWidth
					- ImGui::CalcTextSize(axisLabels[c]).x - style.ItemInnerSpacing.x);
				const int pre = values[c];
				int channel = pre;
				const char* fmt = mixed[c] ? "-" : "%d";
				ImGui::SetNextItemWidth(channelWidth);
				const bool channelChanged = ImGui::DragInt("##c", &channel, speed, 0, 0, fmt);
				if (channelChanged && channel != pre) {
					WriteIntChannel(entities, d, static_cast<std::size_t>(c), channel);
					any = true;
				}
				ImGui::PopID();
			}
			ImGui::PopID();
			return any;
		}

		bool DrawColor(std::span<const Entity> entities, const PropertyDescriptor& d,
			const std::string& fieldKey) {
			std::array<float, 4> values{};
			std::array<bool, 4> mixed{};
			SampleFloatChannels<4>(entities, d, values, mixed);
			const bool anyMixed = mixed[0] || mixed[1] || mixed[2] || mixed[3];
			ImGui::PushID(d.Name.c_str());
			ImGuiUtils::BeginInspectorFieldRow(d.DisplayName.c_str());
			bool anyChanged = false;
			if (!anyMixed) {
				// Persistent edit buffer so ImGui's ColorPicker reads back its OWN exact
				// float output each frame. Re-seeding from the stored value (which round-trips
				// through the property Get/Set) makes ImGui's lossy RGB<->HSV hue backup
				// mismatch, jumping the hue bar. The buffer must stay ImGui-owned for the WHOLE
				// edit session — not just while an item is active — otherwise the re-seed on the
				// frame the SV square is released still jumps the hue.
				static std::unordered_map<std::string, std::array<float, 4>> s_Working;
				std::array<float, 4>& work = s_Working[fieldKey];

				// Is this field's picker popup open? ColorEdit4 does PushID(label) then
				// OpenPopup("picker"), so re-enter that exact ID scope to query it.
				ImGui::PushID("##Value");
				const bool pickerOpen = ImGui::IsPopupOpen("picker", ImGuiPopupFlags_AnyPopupLevel);
				ImGui::PopID();

				// Re-seed from storage only when fully idle: picker closed AND nothing active.
				if (!pickerOpen && !ImGui::IsAnyItemActive()) work = values;

				if (ImGui::ColorEdit4("##Value", work.data())) {
					for (int c = 0; c < 4; ++c) {
						if (work[c] != values[c]) {
							WriteFloatChannel(entities, d, static_cast<std::size_t>(c), work[c]);
							anyChanged = true;
						}
					}
				}
			}
			else {
				// C7: mixed-channel drag fallback. Mirrors the non-mixed
				// branch above: only commit when the channel the user
				// touched actually moved.
				anyChanged = ImGuiUtils::MultiEdit::MultiItemRow(4, [&](int c) -> bool {
					ImGui::PushID(c);
					const float pre = values[c];
					float channel = pre;
					const char* fmt = mixed[c] ? "-" : "%.3f";
					const bool committed = ImGui::DragFloat("##c", &channel, 0.005f, 0.0f, 1.0f, fmt);
					const bool changed = committed && channel != pre;
					if (changed) WriteFloatChannel(entities, d, static_cast<std::size_t>(c), channel);
					ImGui::PopID();
					return changed;
				});
			}
			ImGui::PopID();
			return anyChanged;
		}

		// Curve <-> string codec, matching the C# Index.Graph wire format verbatim:
		// keys ';'-joined, each key's six floats ','-joined as posX,posY,inX,inY,outX,outY.
		std::string EncodeCurve(const Curve& c) {
			std::string out;
			char buf[64];
			auto app = [&](float f) {
				auto [p, ec] = std::to_chars(buf, buf + sizeof(buf), f, std::chars_format::general);
				if (ec == std::errc{}) out.append(buf, p); else out.push_back('0');
			};
			for (std::size_t i = 0; i < c.Keys.size(); ++i) {
				if (i) out.push_back(';');
				const Curve::Key& k = c.Keys[i];
				app(k.Pos.x);        out.push_back(',');
				app(k.Pos.y);        out.push_back(',');
				app(k.InTangent.x);  out.push_back(',');
				app(k.InTangent.y);  out.push_back(',');
				app(k.OutTangent.x); out.push_back(',');
				app(k.OutTangent.y);
			}
			return out;
		}

		Curve DecodeCurve(const std::string& s) {
			Curve c; // DefaultKeys
			if (s.empty()) return c;

			auto toFloat = [](std::string_view sv) -> float {
				float r = 0.0f;
				std::from_chars(sv.data(), sv.data() + sv.size(), r);
				return r;
			};
			std::vector<Curve::Key> keys;
			std::size_t start = 0;
			while (start <= s.size()) {
				const std::size_t semi = s.find(';', start);
				const std::string_view part(s.data() + start,
					(semi == std::string::npos ? s.size() : semi) - start);
				float f[6] = { 0, 0, 0, 0, 0, 0 };
				int n = 0;
				std::size_t fstart = 0;
				while (n < 6) {
					const std::size_t comma = part.find(',', fstart);
					const std::string_view field = part.substr(fstart,
						(comma == std::string_view::npos ? part.size() : comma) - fstart);
					f[n++] = toFloat(field);
					if (comma == std::string_view::npos) break;
					fstart = comma + 1;
				}
				if (n >= 6) {
					keys.push_back(Curve::Key{ Vec2{ f[0], f[1] }, Vec2{ f[2], f[3] }, Vec2{ f[4], f[5] } });
				}
				if (semi == std::string::npos) break;
				start = semi + 1;
			}
			if (keys.size() >= 2) c.Keys = std::move(keys);
			return c;
		}

		bool DrawGraph(std::span<const Entity> entities, const PropertyDescriptor& d,
			const std::string& fieldKey) {
			PropertyValue v;
			const bool uniform = SampleUniform(entities, d, v);

			ImGui::PushID(d.Name.c_str());
			ImGuiUtils::BeginInspectorFieldRow(d.DisplayName.c_str());

			// DrawCurveEditor keeps selection/drag state keyed by the Curve's address, so the
			// working copy must persist across frames. Keyed by fieldKey (unique per field);
			// reseeded from the stored value only while nothing is being dragged, so an
			// in-progress edit isn't clobbered by the round-tripped value.
			static std::unordered_map<std::string, Curve> s_Working;
			Curve& work = s_Working[fieldKey];
			if (!ImGui::IsAnyItemActive()) {
				work = DecodeCurve(uniform ? v.StringValue : std::string());
			}

			const bool changed = ImGuiUtils::DrawCurveEditor("##Value", work, uniform);
			ImGui::PopID();

			if (changed) {
				PropertyValue out;
				out.Type = PropertyType::Graph;
				out.StringValue = EncodeCurve(work);
				WriteAll(entities, d, out);
			}
			return changed;
		}

		bool DrawEnum(std::span<const Entity> entities, const PropertyDescriptor& d) {
			if (!d.Metadata.Enum || d.Metadata.Enum->Options.empty()) return false;
			PropertyValue v;
			const bool uniform = SampleUniform(entities, d, v);

			const char* preview = "-";
			std::string previewBuf;
			if (uniform) {
				for (const auto& opt : d.Metadata.Enum->Options) {
					if (opt.Value == v.IntValue) {
						previewBuf = opt.Name;
						preview = previewBuf.c_str();
						break;
					}
				}
				if (previewBuf.empty()) preview = "Unknown";
			}

			bool changed = false;
			ImGui::PushID(d.Name.c_str());
			ImGuiUtils::BeginInspectorFieldRow(d.DisplayName.c_str());
			if (ImGui::BeginCombo("##Value", preview)) {
				for (const auto& opt : d.Metadata.Enum->Options) {
					const bool isSelected = uniform && (v.IntValue == opt.Value);
					if (ImGui::Selectable(opt.Name.c_str(), isSelected)) {
						PropertyValue out;
						out.Type = PropertyType::Enum;
						out.IntValue = opt.Value;
						WriteAll(entities, d, out);
						changed = true;
					}
					if (isSelected) ImGui::SetItemDefaultFocus();
				}
				ImGui::EndCombo();
			}
			ImGui::PopID();
			return changed;
		}

		bool DrawFlagEnum(std::span<const Entity> entities, const PropertyDescriptor& d) {
			if (!d.Metadata.Enum || d.Metadata.Enum->Options.empty()) return false;

			std::vector<bool> bitOn(d.Metadata.Enum->Options.size(), false);
			std::vector<bool> bitMixed(d.Metadata.Enum->Options.size(), false);
			int64_t sampleAll = 0;
			if (!entities.empty()) {
				const PropertyValue first = d.Get(entities[0]);
				sampleAll = first.IntValue;
				for (std::size_t b = 0; b < d.Metadata.Enum->Options.size(); ++b) {
					const int64_t bit = d.Metadata.Enum->Options[b].Value;
					if (bit == 0) continue;
					bitOn[b] = (first.IntValue & bit) == bit;
				}
				for (std::size_t i = 1; i < entities.size(); ++i) {
					const PropertyValue v = d.Get(entities[i]);
					for (std::size_t b = 0; b < d.Metadata.Enum->Options.size(); ++b) {
						if (bitMixed[b]) continue;
						const int64_t bit = d.Metadata.Enum->Options[b].Value;
						if (bit == 0) continue;
						const bool on = (v.IntValue & bit) == bit;
						if (on != bitOn[b]) bitMixed[b] = true;
					}
				}
			}

			std::string preview;
			bool anyMixed = false;
			for (std::size_t b = 0; b < d.Metadata.Enum->Options.size(); ++b) {
				if (d.Metadata.Enum->Options[b].Value == 0) continue;
				if (bitMixed[b]) { anyMixed = true; break; }
			}
			if (anyMixed) {
				preview = "-";
			}
			else {
				for (std::size_t b = 0; b < d.Metadata.Enum->Options.size(); ++b) {
					if (d.Metadata.Enum->Options[b].Value == 0) continue;
					if (bitOn[b]) {
						if (!preview.empty()) preview += ", ";
						preview += d.Metadata.Enum->Options[b].Name;
					}
				}
				if (preview.empty()) preview = k_NoneLabel;
			}

			// Strip undeclared bits on write so garbage flags from old saves don't propagate (M27).
			int64_t declaredMask = 0;
			for (const auto& opt : d.Metadata.Enum->Options) declaredMask |= opt.Value;

			bool changed = false;
			ImGui::PushID(d.Name.c_str());
			ImGuiUtils::BeginInspectorFieldRow(d.DisplayName.c_str());
			if (ImGui::BeginCombo("##Value", preview.c_str())) {
				for (std::size_t b = 0; b < d.Metadata.Enum->Options.size(); ++b) {
					const auto& opt = d.Metadata.Enum->Options[b];
					if (opt.Value == 0) continue;
					ImGui::PushID(static_cast<int>(b));
					if (bitMixed[b]) ImGui::PushItemFlag(static_cast<ImGuiItemFlags>(kMixedValueFlag), true);
					bool tmp = bitOn[b];
					if (ImGui::Checkbox(opt.Name.c_str(), &tmp)) {
						for (const Entity& e : entities) {
							PropertyValue current = d.Get(e);
							if (tmp) current.IntValue |= opt.Value;
							else     current.IntValue &= ~opt.Value;
							const int64_t before = current.IntValue;
							current.IntValue &= declaredMask;
							if (current.IntValue != before) {
								IDX_CORE_WARN_TAG("Inspector",
									"FlagEnum '{}': dropping undeclared bits 0x{:x}",
									d.Name, static_cast<uint64_t>(before & ~declaredMask));
							}
							d.Set(const_cast<Entity&>(e), current);
							MarkSceneDirty(e);
						}
						changed = true;
					}
					if (bitMixed[b]) ImGui::PopItemFlag();
					ImGui::PopID();
				}
				ImGui::EndCombo();
			}
			ImGui::PopID();
			(void)sampleAll;
			return changed;
		}

		// Generic reference-type drawer. `displayResolver` produces the
		// button text and tooltip; `pickerOpener` builds the picker entries
		// and opens the popup; `payloadAccept` handles drag-drop targets.
		template <typename DisplayResolver, typename PickerOpener, typename PayloadAccept, typename LocatePathResolver>
		bool DrawReference(std::span<const Entity> entities, const PropertyDescriptor& d,
			const std::string& fieldKey, PropertyType valueType,
			DisplayResolver&& displayResolver, PickerOpener&& pickerOpener,
			PayloadAccept&& payloadAccept, bool showLocateButton,
			LocatePathResolver&& locatePathResolver)
		{
			PropertyValue v;
			const bool uniform = SampleUniform(entities, d, v);
			bool missing = false;
			std::string secondary;
			std::string display = uniform ? displayResolver(v, missing, secondary)
				: std::string("\xe2\x80\x94");
			std::string locatePath;
			if (showLocateButton && uniform && !missing) {
				locatePath = locatePathResolver(v);
			}

			bool hovered = false;
			const float locateButtonWidth = showLocateButton ? ImGui::GetFrameHeight() : 0.0f;
			const bool clicked = ReferencePicker::DrawReferenceField(
				d.DisplayName.c_str(), display, secondary, missing, !uniform, hovered,
				locateButtonWidth);

			bool changed = false;
			if (clicked) {
				pickerOpener(fieldKey);
			}
			// Scope the popup id per field: DrawReferenceField's PushID is already
			// popped here, so without this every reference field in the component
			// shares "##RefCtx" — a duplicate ImGui id plus one stacked "Clear" per field.
			ImGui::PushID(fieldKey.c_str());
			const bool refCtxCleared = DrawReferenceContextMenu("##RefCtx");
			ImGui::PopID();
			if (refCtxCleared) {
				PropertyValue cleared;
				cleared.Type = valueType;
				WriteAll(entities, d, cleared);
				changed = true;
			}
			if (ImGui::BeginDragDropTarget()) {
				PropertyValue dropped;
				dropped.Type = valueType;
				if (payloadAccept(dropped)) {
					WriteAll(entities, d, dropped);
					changed = true;
				}
				ImGui::EndDragDropTarget();
			}
			if (showLocateButton) {
				ImGui::SameLine(0.0f, ImGui::GetStyle().ItemSpacing.x);
				ImGui::PushID(fieldKey.c_str());
				if (DrawAssetLocateButton(!locatePath.empty())) {
					AssetBrowser::RequestRevealAsset(locatePath);
				}
				ImGui::PopID();
			}
			if (auto pending = ReferencePicker::ConsumeSelection(fieldKey); pending) {
				PropertyValue out = PropertyValue::FromString(valueType, *pending);
				WriteAll(entities, d, out);
				changed = true;
			}
			(void)hovered;
			return changed;
		}

		template <typename DisplayResolver, typename PickerOpener, typename PayloadAccept>
		bool DrawReference(std::span<const Entity> entities, const PropertyDescriptor& d,
			const std::string& fieldKey, PropertyType valueType,
			DisplayResolver&& displayResolver, PickerOpener&& pickerOpener,
			PayloadAccept&& payloadAccept)
		{
			return DrawReference(entities, d, fieldKey, valueType,
				std::forward<DisplayResolver>(displayResolver),
				std::forward<PickerOpener>(pickerOpener),
				std::forward<PayloadAccept>(payloadAccept),
				/*showLocateButton=*/false,
				[](const PropertyValue&) { return std::string(); });
		}

		bool DrawAssetRefByKind(std::span<const Entity> entities, const PropertyDescriptor& d,
			const std::string& fieldKey, AssetKind kind, PropertyType valueType,
			const char* pickerTitle)
		{
			// Texture pickers get the thumbnail-grid layout (matching the
			// SpriteRenderer's existing browser); other asset kinds keep the
			// plain selectable list. The list contents are identical — only
			// the visual layout differs.
			const ReferencePicker::Style style = (kind == AssetKind::Texture)
				? ReferencePicker::Style::Thumbnails
				: ReferencePicker::Style::Plain;
			const bool includeSlices = !d.Metadata.SuppressTextureSlices;

			return DrawReference(entities, d, fieldKey, valueType,
				[kind](const PropertyValue& v, bool& missing, std::string& secondary) {
					return ReferencePicker::ResolveAssetDisplay(v.UIntValue, kind, missing, &secondary);
				},
				[kind, pickerTitle, style, includeSlices](const std::string& key) {
					ReferencePicker::OpenForFieldKey(key, pickerTitle,
						ReferencePicker::CollectAssetsByKind(kind, includeSlices), style);
				},
				[kind](PropertyValue& outValue) {
					// Check sprite-slice payload before ASSET_BROWSER_ITEM so a slice drag sets both UUID and slice name rather than falling into the texture-only branch.
					if (kind == AssetKind::Texture) {
						if (const ImGuiPayload* slicePayload =
							ImGui::AcceptDragDropPayload(k_SpriteSliceDragPayloadType))
						{
							if (slicePayload->DataSize == sizeof(SpriteSliceDragPayload)) {
								const auto* p = static_cast<const SpriteSliceDragPayload*>(slicePayload->Data);
								const std::string texturePath(p->TexturePath);
								const std::string sliceName(p->SliceName);
								if (!texturePath.empty()) {
									const uint64_t assetId = AssetRegistry::GetOrCreateAssetUUID(texturePath);
									if (assetId != 0 && AssetRegistry::GetKind(assetId) == AssetKind::Texture) {
										outValue.UIntValue = assetId;
										outValue.StringValue = sliceName;
										return true;
									}
								}
							}
						}
					}
					if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_BROWSER_ITEM")) {
						const uint64_t assetId = ResolveDroppedAssetId(payload);
						if (assetId != 0 && AssetRegistry::GetKind(assetId) == kind) {
							outValue.UIntValue = assetId;
							// Clear any stale slice from a previous drop on the
							// same field so the slice-aware setter reverts the
							// entity to "full texture" rendering.
							outValue.StringValue.clear();
							return true;
						}
					}
					return false;
				},
				/*showLocateButton=*/true,
				[kind](const PropertyValue& v) {
					if (v.UIntValue == 0) {
						return std::string();
					}
					if (kind != AssetKind::Unknown && AssetRegistry::GetKind(v.UIntValue) != kind) {
						return std::string();
					}
					return AssetRegistry::ResolvePath(v.UIntValue);
				});
		}

		bool DrawEntityRef(std::span<const Entity> entities, const PropertyDescriptor& d,
			const std::string& fieldKey)
		{
			return DrawReference(entities, d, fieldKey, PropertyType::EntityRef,
				[](const PropertyValue& v, bool& missing, std::string& secondary) {
					if (v.StringValue == "prefab") {
						return ReferencePicker::ResolvePrefabDisplay(v.UIntValue, missing, &secondary);
					}
					return ReferencePicker::ResolveEntityDisplay(v.UIntValue, missing, &secondary);
				},
				[](const std::string& key) {
					ReferencePicker::OpenForFieldKey(key, "Select Entity",
						ReferencePicker::CollectEntities(),
						ReferencePicker::Style::Plain,
						/*useEntityTabs=*/true);
				},
				[](PropertyValue& outValue) {
					if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("HIERARCHY_ENTITY")) {
						if (payload->DataSize == sizeof(HierarchyDragData)) {
							const auto* data = static_cast<const HierarchyDragData*>(payload->Data);
							const EntityHandle handle = static_cast<EntityHandle>(data->EntityHandle);
							const Scene* scene = ResolveHierarchyPayloadScene(*data);
							if (scene && scene->IsValid(handle)) {
								// Persistent UUID — see ReferencePicker::CollectEntities for why.
								outValue.UIntValue = scene->GetEntityPersistentID(handle);
								outValue.StringValue.clear();
								return outValue.UIntValue != 0;
							}
						}
					}
					if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_BROWSER_ITEM")) {
						const uint64_t assetId = ResolveDroppedAssetId(payload);
						if (assetId != 0 && AssetRegistry::GetKind(assetId) == AssetKind::Prefab) {
							outValue.UIntValue = assetId;
							outValue.StringValue = "prefab";
							return true;
						}
					}
					return false;
				});
		}

		bool DrawComponentRef(std::span<const Entity> entities, const PropertyDescriptor& d,
			const std::string& fieldKey)
		{
			const std::string& componentTypeName = d.Metadata.ComponentTypeName;
			return DrawReference(entities, d, fieldKey, PropertyType::ComponentRef,
				[&componentTypeName](const PropertyValue& v, bool& missing, std::string& secondary) {
					return ReferencePicker::ResolveComponentRefDisplay(v.UIntValue,
						componentTypeName, missing, &secondary);
				},
				[&componentTypeName](const std::string& key) {
					ReferencePicker::OpenForFieldKey(key, "Select " + componentTypeName,
						ReferencePicker::CollectComponentTargets(componentTypeName));
				},
				[&componentTypeName](PropertyValue& outValue) {
					if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("COMPONENT_REF")) {
						// Payload is raw bytes, not a C string; trim trailing NUL for back-compat.
						std::string refStr(static_cast<const char*>(payload->Data),
							static_cast<size_t>(payload->DataSize));
						if (!refStr.empty() && refStr.back() == '\0') refStr.pop_back();
						const std::size_t sep = refStr.find(':');
						if (sep != std::string::npos) {
							const std::string typeName = refStr.substr(sep + 1);
							if (typeName == componentTypeName) {
								outValue.UIntValue = std::strtoull(refStr.substr(0, sep).c_str(), nullptr, 10);
								outValue.StringValue = typeName;
								return outValue.UIntValue != 0;
							}
						}
					}
					// Also accept HIERARCHY_ENTITY: auto-derive ComponentRef if the dropped entity owns the matching component.
					if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("HIERARCHY_ENTITY")) {
						if (payload->DataSize == sizeof(HierarchyDragData)) {
							const auto* data = static_cast<const HierarchyDragData*>(payload->Data);
							const EntityHandle handle = static_cast<EntityHandle>(data->EntityHandle);
							const Scene* scene = ResolveHierarchyPayloadScene(*data);
							if (scene && scene->IsValid(handle)) {
								const ComponentInfo* info = nullptr;
								SceneManager::Get().GetComponentRegistry().ForEachComponentInfo(
									[&](const std::type_index&, const ComponentInfo& candidate) {
										if (info) return;
										if (candidate.displayName == componentTypeName) info = &candidate;
									});
								Entity dropped = scene->GetEntity(handle);
								if (info && info->has && info->has(dropped)) {
									outValue.UIntValue = scene->GetEntityPersistentID(handle);
									outValue.StringValue = componentTypeName;
									return outValue.UIntValue != 0;
								}
							}
						}
					}
					return false;
				});
		}

		// Per-row reference widget for DrawList's reference item types. Mutates
		// `item`; returns true on change. `rowKey` must be unique per row so the
		// shared ReferencePicker routes its selection back to the right element.
		bool DrawReferenceListItem(PropertyType itemType, const PropertyMetadata& meta,
			PropertyValue& item, const std::string& rowKey, float buttonWidth)
		{
			item.Type = itemType;

			const auto assetKindFor = [&]() -> AssetKind {
				switch (itemType) {
				case PropertyType::TextureRef: return AssetKind::Texture;
				case PropertyType::AudioRef:   return AssetKind::Audio;
				case PropertyType::FontRef:    return AssetKind::Font;
				case PropertyType::SceneRef:   return AssetKind::Scene;
				case PropertyType::PrefabRef:  return AssetKind::Prefab;
				case PropertyType::AssetRef:   return meta.AssetKindFilter;
				default:                       return AssetKind::Unknown;
				}
			};

			bool missing = false;
			std::string secondary;
			std::string display;
			switch (itemType) {
			case PropertyType::EntityRef:
				display = (item.StringValue == "prefab")
					? ReferencePicker::ResolvePrefabDisplay(item.UIntValue, missing, &secondary)
					: ReferencePicker::ResolveEntityDisplay(item.UIntValue, missing, &secondary);
				break;
			case PropertyType::ComponentRef:
				display = ReferencePicker::ResolveComponentRefDisplay(item.UIntValue,
					meta.ComponentTypeName, missing, &secondary);
				break;
			default:
				display = ReferencePicker::ResolveAssetDisplay(item.UIntValue, assetKindFor(), missing, &secondary);
				break;
			}

			const ImGuiStyle& style = ImGui::GetStyle();
			if (missing) {
				ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.30f, 0.12f, 0.12f, 1.0f));
				ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.38f, 0.16f, 0.16f, 1.0f));
				ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.26f, 0.10f, 0.10f, 1.0f));
			}
			else {
				ImGui::PushStyleColor(ImGuiCol_Button,        ImGui::GetStyleColorVec4(ImGuiCol_FrameBg));
				ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4(ImGuiCol_FrameBgHovered));
				ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImGui::GetStyleColorVec4(ImGuiCol_FrameBgActive));
			}
			bool truncated = false;
			const float textMaxWidth = std::max(1.0f, buttonWidth - style.FramePadding.x * 2.0f);
			const std::string buttonText = ImGuiUtils::Ellipsize(display, textMaxWidth, &truncated);
			const bool clicked = ImGui::Button((buttonText + "##v").c_str(), ImVec2(buttonWidth, 0.0f));
			if (ImGui::IsItemHovered() && (truncated || !secondary.empty())) {
				ImGui::BeginTooltip();
				ImGui::TextUnformatted(display.c_str());
				if (!secondary.empty()) { ImGui::Separator(); ImGui::TextDisabled("%s", secondary.c_str()); }
				ImGui::EndTooltip();
			}
			ImGui::PopStyleColor(3);

			bool changed = false;

			if (clicked) {
				switch (itemType) {
				case PropertyType::EntityRef:
					ReferencePicker::OpenForFieldKey(rowKey, "Select Entity",
						ReferencePicker::CollectEntities(), ReferencePicker::Style::Plain, /*useEntityTabs=*/true);
					break;
				case PropertyType::ComponentRef:
					ReferencePicker::OpenForFieldKey(rowKey, "Select " + meta.ComponentTypeName,
						ReferencePicker::CollectComponentTargets(meta.ComponentTypeName));
					break;
				default: {
					const AssetKind kind = assetKindFor();
					const ReferencePicker::Style pickerStyle = (kind == AssetKind::Texture)
						? ReferencePicker::Style::Thumbnails : ReferencePicker::Style::Plain;
					ReferencePicker::OpenForFieldKey(rowKey, "Select",
						ReferencePicker::CollectAssetsByKind(kind, !meta.SuppressTextureSlices), pickerStyle);
					break;
				}
				}
			}

			if (DrawReferenceContextMenu("##RefListCtx")) {
				PropertyValue cleared;
				cleared.Type = itemType;
				item = cleared;
				changed = true;
			}

			if (ImGui::BeginDragDropTarget()) {
				if (itemType == PropertyType::EntityRef) {
					if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("HIERARCHY_ENTITY")) {
						if (payload->DataSize == sizeof(HierarchyDragData)) {
							const auto* data = static_cast<const HierarchyDragData*>(payload->Data);
							const EntityHandle handle = static_cast<EntityHandle>(data->EntityHandle);
							const Scene* scene = ResolveHierarchyPayloadScene(*data);
							if (scene && scene->IsValid(handle)) {
								const uint64_t id = scene->GetEntityPersistentID(handle);
								if (id != 0) { item.UIntValue = id; item.StringValue.clear(); changed = true; }
							}
						}
					}
					if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_BROWSER_ITEM")) {
						const uint64_t assetId = ResolveDroppedAssetId(payload);
						if (assetId != 0 && AssetRegistry::GetKind(assetId) == AssetKind::Prefab) {
							item.UIntValue = assetId; item.StringValue = "prefab"; changed = true;
						}
					}
				}
				else if (itemType == PropertyType::ComponentRef) {
					if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("COMPONENT_REF")) {
						std::string refStr(static_cast<const char*>(payload->Data), static_cast<size_t>(payload->DataSize));
						if (!refStr.empty() && refStr.back() == '\0') refStr.pop_back();
						const std::size_t sep = refStr.find(':');
						if (sep != std::string::npos && refStr.substr(sep + 1) == meta.ComponentTypeName) {
							item.UIntValue = std::strtoull(refStr.substr(0, sep).c_str(), nullptr, 10);
							item.StringValue = meta.ComponentTypeName;
							changed = item.UIntValue != 0;
						}
					}
				}
				else {
					const AssetKind kind = assetKindFor();
					if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_BROWSER_ITEM")) {
						const uint64_t assetId = ResolveDroppedAssetId(payload);
						if (assetId != 0 && AssetRegistry::GetKind(assetId) == kind) {
							item.UIntValue = assetId; item.StringValue.clear(); changed = true;
						}
					}
				}
				ImGui::EndDragDropTarget();
			}

			if (auto pending = ReferencePicker::ConsumeSelection(rowKey); pending) {
				item = PropertyValue::FromString(itemType, *pending);
				changed = true;
			}

			return changed;
		}

		// Persistent multi-selection for a list field's rows (keyed by fieldKey),
		// driving the Remove-selected button plus Ctrl-toggle and Shift-range picks.
		// Survives across frames; stale indices are pruned/guarded at use.
		struct ListSelectionState {
			std::unordered_set<int> Selected;
			int LastClicked = -1;
		};
		std::unordered_map<std::string, ListSelectionState> s_ListSelection;

		// Square, icon-only button (UV-flipped like the rest of the editor). Returns
		// true on click; greys out and swallows clicks when !enabled.
		bool IconButton(const char* id, const char* iconName, float size, bool enabled) {
			if (!enabled) ImGui::BeginDisabled();
			const uint64_t icon = EditorIcons::Get(iconName, 16);
			bool clicked = false;
			if (icon) {
				const float img = ImGui::GetFontSize();
				clicked = ImGui::ImageButton(id,
					static_cast<ImTextureID>(static_cast<intptr_t>(icon)),
					ImVec2(img, img), ImVec2(0, 1), ImVec2(1, 0));
			}
			else {
				clicked = ImGui::Button(id, ImVec2(size, size));
			}
			if (!enabled) ImGui::EndDisabled();
			return clicked && enabled;
		}

		// A list row's drag handle (left gutter): click selects (Ctrl toggles,
		// Shift selects a range from the anchor); dragging it reorders. Shared by
		// scalar/reference rows and object-element headers.
		void DrawListRowHandle(int idx, ListSelectionState& sel, int count,
			float handleW, float rowH, ImDrawList* dl, int& moveFrom, int& moveTo)
		{
			ImGui::InvisibleButton("##handle", ImVec2(handleW, rowH));
			if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
				const ImGuiIO& io = ImGui::GetIO();
				if (io.KeyShift && sel.LastClicked >= 0 && sel.LastClicked < count) {
					sel.Selected.clear();
					const int a = std::min(sel.LastClicked, idx);
					const int b = std::max(sel.LastClicked, idx);
					for (int k = a; k <= b; ++k) sel.Selected.insert(k);
				}
				else if (io.KeyCtrl) {
					if (sel.Selected.count(idx)) sel.Selected.erase(idx); else sel.Selected.insert(idx);
					sel.LastClicked = idx;
				}
				else {
					sel.Selected.clear();
					sel.Selected.insert(idx);
					sel.LastClicked = idx;
				}
			}
			if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
				ImGui::SetDragDropPayload("LIST_ITEM_REORDER", &idx, sizeof(int));
				if (const uint64_t moveIcon = EditorIcons::Get("move", 16)) {
						ImGui::Image(static_cast<ImTextureID>(static_cast<intptr_t>(moveIcon)), ImVec2(14, 14), ImVec2(0, 1), ImVec2(1, 0));
						ImGui::SameLine();
					}
					ImGui::Text("Element %d", idx);
				ImGui::EndDragDropSource();
			}
			if (ImGui::BeginDragDropTarget()) {
				if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("LIST_ITEM_REORDER")) {
					moveFrom = *static_cast<const int*>(p->Data);
					moveTo = idx;
				}
				ImGui::EndDragDropTarget();
			}
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("Drag to reorder \xc2\xb7 click to select");
			if (const uint64_t gripIcon = EditorIcons::Get("move", 16)) {
				const ImVec2 hMin = ImGui::GetItemRectMin();
				const ImVec2 hMax = ImGui::GetItemRectMax();
				const float gs = std::min(14.0f, rowH - 6.0f);
				const ImVec2 c((hMin.x + hMax.x) * 0.5f, (hMin.y + hMax.y) * 0.5f);
				dl->AddImage(static_cast<ImTextureID>(static_cast<intptr_t>(gripIcon)),
					ImVec2(c.x - gs * 0.5f, c.y - gs * 0.5f),
					ImVec2(c.x + gs * 0.5f, c.y + gs * 0.5f),
					ImVec2(0, 1), ImVec2(1, 0),
					ImGui::GetColorU32(ImGuiCol_TextDisabled));
			}
		}

		// Draws one list value widget for `item` (a scalar/reference element, or an
		// object element's sub-field). `meta` supplies Enum / ComponentTypeName /
		// AssetKindFilter / DragSpeed for the widget; `rowKey` keys any reference
		// picker. Returns true on change. Sets the next item width itself.
		bool DrawListItemValue(PropertyType itemType, const PropertyMetadata& meta,
			PropertyValue& item, const std::string& rowKey, float width)
		{
			item.Type = itemType;
			bool changed = false;
			ImGui::SetNextItemWidth(width);
			switch (itemType) {
			case PropertyType::Bool: {
				bool b = item.BoolValue;
				if (ImGui::Checkbox("##v", &b)) { item.BoolValue = b; changed = true; }
				break;
			}
			case PropertyType::Int8:
			case PropertyType::Int16:
			case PropertyType::Int32: {
				int tmp = static_cast<int>(item.IntValue);
				const float speed = meta.DragSpeed > 0 ? meta.DragSpeed : 1.0f;
				if (ImGui::DragInt("##v", &tmp, speed)) { item.IntValue = tmp; changed = true; }
				break;
			}
			case PropertyType::UInt8:
			case PropertyType::UInt16:
			case PropertyType::UInt32: {
				int tmp = static_cast<int>(std::min<uint64_t>(item.UIntValue, INT_MAX));
				const float speed = meta.DragSpeed > 0 ? meta.DragSpeed : 1.0f;
				if (ImGui::DragInt("##v", &tmp, speed, 0, INT_MAX)) {
					item.UIntValue = static_cast<uint64_t>(std::max(tmp, 0));
					changed = true;
				}
				break;
			}
			case PropertyType::Int64: {
				int64_t tmp = item.IntValue;
				if (ImGui::InputScalar("##v", ImGuiDataType_S64, &tmp)) { item.IntValue = tmp; changed = true; }
				break;
			}
			case PropertyType::UInt64: {
				uint64_t tmp = item.UIntValue;
				if (ImGui::InputScalar("##v", ImGuiDataType_U64, &tmp)) { item.UIntValue = tmp; changed = true; }
				break;
			}
			case PropertyType::IntVec2: {
				int vec[2] = { item.IntVec[0], item.IntVec[1] };
				if (ImGui::DragInt2("##v", vec)) { item.IntVec[0] = vec[0]; item.IntVec[1] = vec[1]; changed = true; }
				break;
			}
			case PropertyType::IntVec3: {
				int vec[3] = { item.IntVec[0], item.IntVec[1], item.IntVec[2] };
				if (ImGui::DragInt3("##v", vec)) { for (int c = 0; c < 3; ++c) item.IntVec[c] = vec[c]; changed = true; }
				break;
			}
			case PropertyType::IntVec4: {
				int vec[4] = { item.IntVec[0], item.IntVec[1], item.IntVec[2], item.IntVec[3] };
				if (ImGui::DragInt4("##v", vec)) { for (int c = 0; c < 4; ++c) item.IntVec[c] = vec[c]; changed = true; }
				break;
			}
			case PropertyType::Float: {
				float tmp = static_cast<float>(item.FloatValue);
				const float speed = meta.DragSpeed > 0 ? meta.DragSpeed : 0.1f;
				if (ImGui::DragFloat("##v", &tmp, speed)) { item.FloatValue = static_cast<double>(tmp); changed = true; }
				break;
			}
			case PropertyType::Double: {
				double tmp = item.FloatValue;
				const float speed = meta.DragSpeed > 0 ? meta.DragSpeed : 0.1f;
				if (ImGui::DragScalar("##v", ImGuiDataType_Double, &tmp, speed)) { item.FloatValue = tmp; changed = true; }
				break;
			}
			case PropertyType::String: {
				char buf[1024]{};
				std::snprintf(buf, sizeof(buf), "%s", item.StringValue.c_str());
				if (ImGui::InputText("##v", buf, sizeof(buf))) { item.StringValue = buf; changed = true; }
				break;
			}
			case PropertyType::Vec2: {
				float vec[2] = { item.FloatVec[0], item.FloatVec[1] };
				if (ImGui::DragFloat2("##v", vec, 0.1f)) { item.FloatVec[0] = vec[0]; item.FloatVec[1] = vec[1]; changed = true; }
				break;
			}
			case PropertyType::Vec3: {
				float vec[3] = { item.FloatVec[0], item.FloatVec[1], item.FloatVec[2] };
				if (ImGui::DragFloat3("##v", vec, 0.1f)) {
					item.FloatVec[0] = vec[0]; item.FloatVec[1] = vec[1]; item.FloatVec[2] = vec[2]; changed = true;
				}
				break;
			}
			case PropertyType::Vec4: {
				float vec[4] = { item.FloatVec[0], item.FloatVec[1], item.FloatVec[2], item.FloatVec[3] };
				if (ImGui::DragFloat4("##v", vec, 0.1f)) { for (int c = 0; c < 4; ++c) item.FloatVec[c] = vec[c]; changed = true; }
				break;
			}
			case PropertyType::Color: {
				float vec[4] = { item.FloatVec[0], item.FloatVec[1], item.FloatVec[2], item.FloatVec[3] };
				if (ImGui::ColorEdit4("##v", vec)) { for (int c = 0; c < 4; ++c) item.FloatVec[c] = vec[c]; changed = true; }
				break;
			}
			case PropertyType::Enum: {
				if (meta.Enum && !meta.Enum->Options.empty()) {
					const char* preview = "Unknown";
					for (const auto& opt : meta.Enum->Options) {
						if (opt.Value == item.IntValue) { preview = opt.Name.c_str(); break; }
					}
					if (ImGui::BeginCombo("##v", preview)) {
						for (const auto& opt : meta.Enum->Options) {
							const bool isSelected = (opt.Value == item.IntValue);
							if (ImGui::Selectable(opt.Name.c_str(), isSelected)) { item.IntValue = opt.Value; changed = true; }
							if (isSelected) ImGui::SetItemDefaultFocus();
						}
						ImGui::EndCombo();
					}
				}
				break;
			}
			case PropertyType::FlagEnum: {
				if (meta.Enum && !meta.Enum->Options.empty()) {
					int64_t declaredMask = 0;
					for (const auto& opt : meta.Enum->Options) declaredMask |= opt.Value;
					std::string preview;
					for (const auto& opt : meta.Enum->Options) {
						if (opt.Value == 0) continue;
						if ((item.IntValue & opt.Value) == opt.Value) {
							if (!preview.empty()) preview += ", ";
							preview += opt.Name;
						}
					}
					if (preview.empty()) preview = "None";
					if (ImGui::BeginCombo("##v", preview.c_str())) {
						for (const auto& opt : meta.Enum->Options) {
							if (opt.Value == 0) continue;
							bool on = (item.IntValue & opt.Value) == opt.Value;
							if (ImGui::Checkbox(opt.Name.c_str(), &on)) {
								if (on) item.IntValue |= opt.Value;
								else    item.IntValue &= ~opt.Value;
								item.IntValue &= declaredMask;
								changed = true;
							}
						}
						ImGui::EndCombo();
					}
				}
				break;
			}
			case PropertyType::TextureRef:
			case PropertyType::AudioRef:
			case PropertyType::FontRef:
			case PropertyType::SceneRef:
			case PropertyType::PrefabRef:
			case PropertyType::AssetRef:
			case PropertyType::EntityRef:
			case PropertyType::ComponentRef: {
				if (DrawReferenceListItem(itemType, meta, item, rowKey, width)) changed = true;
				break;
			}
			default:
				ImGui::TextDisabled("(unsupported list item type)");
				break;
			}
			return changed;
		}

		// ── List element clipboard (Duplicate / Copy / Paste context menu) ──
		struct ListElementClipboard {
			bool HasValue = false;
			PropertyValue Value;
			std::string Signature;   // item-type / schema key; paste only into a matching list
		};
		ListElementClipboard s_ListClipboard;

		// Paste-compatibility key: scalar lists key on the item type plus the reference's
		// component-type / asset-kind filter; object lists key on the ordered sub-field types
		// plus their ref filters. So an int can't paste onto an entity list, a {Entity,int}
		// slot can't paste onto a {float} slot, and a ComponentRef(Rigidbody) can't paste onto
		// a ComponentRef(SpriteRenderer).
		std::string ListElementSignature(PropertyType itemType, const PropertyMetadata& meta)
		{
			if (meta.ListItemFields.empty()) {
				std::string sig = "s:" + std::to_string(static_cast<int>(itemType));
				if (!meta.ComponentTypeName.empty()) sig += "#" + meta.ComponentTypeName;
				if (meta.AssetKindFilter != AssetKind::Unknown) sig += "@" + std::to_string(static_cast<int>(meta.AssetKindFilter));
				return sig;
			}
			std::string sig = "o:";
			for (const PropertyMetadata::ListItemField& f : meta.ListItemFields) {
				sig += std::to_string(static_cast<int>(f.Type));
				if (!f.ComponentTypeName.empty()) sig += "#" + f.ComponentTypeName;
				if (f.AssetKindFilter != AssetKind::Unknown) sig += "@" + std::to_string(static_cast<int>(f.AssetKindFilter));
				sig.push_back(',');
			}
			return sig;
		}

		// Right-click menu for one list element (opens on the last-submitted item). Copy
		// applies immediately; Duplicate/Paste set deferred indices so the list isn't
		// mutated mid-iteration.
		void DrawListElementContextMenu(const char* popupId, std::vector<PropertyValue>& items, int idx,
			PropertyType itemType, const PropertyMetadata& meta,
			int& outDuplicate, int& outPaste)
		{
			if (!ImGui::BeginPopupContextItem(popupId)) return;
			const std::string sig = ListElementSignature(itemType, meta);
			if (ImGui::MenuItem("Duplicate")) outDuplicate = idx;
			if (ImGui::MenuItem("Copy")) {
				s_ListClipboard.Value = items[static_cast<std::size_t>(idx)];
				s_ListClipboard.Signature = sig;
				s_ListClipboard.HasValue = true;
			}
			const bool canPaste = s_ListClipboard.HasValue && s_ListClipboard.Signature == sig;
			if (ImGui::MenuItem("Paste", nullptr, false, canPaste)) outPaste = idx;
			ImGui::EndPopup();
		}

		bool DrawList(std::span<const Entity> entities, const PropertyDescriptor& d,
			const std::string& fieldKey) {
			PropertyValue v;
			const bool uniform = SampleUniform(entities, d, v);
			std::vector<PropertyValue> items = v.ListValue;
			const PropertyType itemType = d.Metadata.ListItemType;

			ImGui::PushID(d.Name.c_str());

			// Collapsible header for the whole field: collapsing it skips the row
			// loop entirely (the perf win on large lists). ImGui persists the state.
			std::string headerLabel = d.DisplayName;
				headerLabel += uniform
					? "  (" + std::to_string(items.size()) + (items.size() == 1 ? " item)" : " items)")
					: "  (mixed)";
				headerLabel += "###";   // stable ID so the count text doesn't reset open state
				headerLabel += d.Name;
				// Framed = an always-visible rounded header bar (clearer than a bare tree
				// node); dark-gray colors keep it on-theme rather than the accent Header.
				ImGui::PushStyleColor(ImGuiCol_Header,        IM_COL32(58, 58, 64, 255));
				ImGui::PushStyleColor(ImGuiCol_HeaderHovered, IM_COL32(70, 70, 78, 255));
				ImGui::PushStyleColor(ImGuiCol_HeaderActive,  IM_COL32(66, 66, 74, 255));
				const bool fieldOpen = ImGui::TreeNodeEx(headerLabel.c_str(),
					ImGuiTreeNodeFlags_Framed | ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_DefaultOpen);
				ImGui::PopStyleColor(3);

			if (!fieldOpen) {
				ImGui::PopID();
				return false;
			}

			// Selection is per (field, inspected-entity-set): fieldKey alone is reused
			// across entities (same script class + slot), so without the entity
			// identity a selection — and a Remove — would carry onto the wrong
			// entity's list when the inspected entity changes.
			std::size_t entityHash = entities.size();
			for (const Entity& e : entities) {
				entityHash = entityHash * 1099511628211ull
					^ static_cast<std::size_t>(static_cast<uint32_t>(e.GetHandle()));
			}
			ListSelectionState& sel = s_ListSelection[fieldKey + "@" + std::to_string(entityHash)];
			// Prune selection to the current row range (the list may have shrunk since
			// last frame, or this field key may be reused for a shorter list).
			std::erase_if(sel.Selected, [&](int k) { return k < 0 || k >= static_cast<int>(items.size()); });
			if (sel.LastClicked >= static_cast<int>(items.size())) sel.LastClicked = -1;

			bool changed = false;
			int moveFrom = -1, moveTo = -1;
			int duplicateIdx = -1, pasteIdx = -1;

			ImDrawList* dl = ImGui::GetWindowDrawList();
			const ImGuiStyle& style = ImGui::GetStyle();
			// The card fill MUST stay distinct from the field FrameBg/FrameBgHovered the
			// value widgets paint themselves with (same token == invisible inset). Derive a
			// recessed well from FrameBg (tracks theme edits); hover lifts only partway — NOT
			// to FrameBgHovered — so a hovered field still reads as an inset on the card.
			const ImVec4 frameBgCol = style.Colors[ImGuiCol_FrameBg];
			const auto scaleRGB = [](const ImVec4& c, float k) {
				return ImGui::GetColorU32(ImVec4(c.x * k, c.y * k, c.z * k, 1.0f));
			};
			const ImU32 colNormal    = scaleRGB(frameBgCol, 0.78f);
			const ImU32 colHovered   = scaleRGB(frameBgCol, 0.90f);
			// Light-gray selection (the theme's accent Header color doesn't fit the dark theme).
			const ImU32 colSelected   = IM_COL32(108, 108, 114, 115);
			const ImU32 colSelBorder  = IM_COL32(165, 165, 172, 220);
			const ImU32 colCardBorder = ImGui::GetColorU32(ImGuiCol_Border);   // crisp card edge vs the child bg
			const float rounding = std::max(style.FrameRounding, 3.0f);
			const float rowH = ImGui::GetFrameHeight();
			const float handleW = ImGui::GetFrameHeight();
			const bool isObjectList = !d.Metadata.ListItemFields.empty();

			for (std::size_t i = 0; i < items.size(); ++i) {
				ImGui::PushID(static_cast<int>(i));
				const int idx = static_cast<int>(i);
				const bool isSel = sel.Selected.count(idx) != 0;

				if (isObjectList) {
					// Inline object element: a collapsible "Element N" group inside a
					// variable-height rounded card (drawn behind via a channel split so
					// it wraps the whole group, header + expanded sub-fields).
					const std::string rowKey = fieldKey + "/" + std::to_string(i);
					PropertyValue& item = items[i];
					if (item.Type != PropertyType::List) item.Type = PropertyType::List;
					for (std::size_t k = item.ListValue.size(); k < d.Metadata.ListItemFields.size(); ++k) {
						PropertyValue sub; sub.Type = d.Metadata.ListItemFields[k].Type;
						item.ListValue.push_back(std::move(sub));
					}

					ImDrawListSplitter splitter;
					splitter.Split(dl, 2);
					splitter.SetCurrentChannel(dl, 1);  // content above the card

					const ImVec2 slotMin = ImGui::GetCursorScreenPos();
					const float slotW = ImGui::GetContentRegionAvail().x;
					ImGui::BeginGroup();
					DrawListRowHandle(idx, sel, static_cast<int>(items.size()), handleW, rowH, dl, moveFrom, moveTo);
					ImGui::SameLine(0.0f, style.ItemSpacing.x);

					const std::string header = "Element " + std::to_string(i) + "##el";
					bool itemChanged = false;
					const bool elemOpen = ImGui::TreeNodeEx(header.c_str(),
							ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_FramePadding);
						DrawListElementContextMenu("##elemCtx", items, idx, itemType, d.Metadata, duplicateIdx, pasteIdx);
						if (elemOpen) {
						for (std::size_t k = 0; k < d.Metadata.ListItemFields.size(); ++k) {
							const PropertyMetadata::ListItemField& f = d.Metadata.ListItemFields[k];
							ImGui::PushID(static_cast<int>(k));
							PropertyMetadata subMeta;
							subMeta.Enum = f.Enum;
							subMeta.ComponentTypeName = f.ComponentTypeName;
							subMeta.AssetKindFilter = f.AssetKindFilter;
							subMeta.SuppressTextureSlices = d.Metadata.SuppressTextureSlices;
							// Indent-aware label column: BeginInspectorFieldRow's absolute SameLine(160)
							// ignores the tree indent and overruns the value. Pad RELATIVE to the label
							// (indent-agnostic) and inset the value from the card's right edge.
							const float labelColW = std::min(130.0f, slotW * 0.42f);
							ImGui::AlignTextToFramePadding();
							bool subTrunc = false;
							const std::string subLbl = ImGuiUtils::Ellipsize(f.DisplayName, labelColW - style.ItemSpacing.x, &subTrunc);
							ImGui::TextUnformatted(subLbl.c_str());
							if (subTrunc && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", f.DisplayName.c_str());
							ImGui::SameLine(0.0f, std::max(style.ItemSpacing.x, labelColW - ImGui::CalcTextSize(subLbl.c_str()).x));
							const float subWidth = std::max(40.0f, ImGui::GetContentRegionAvail().x - 6.0f);
							if (DrawListItemValue(f.Type, subMeta, item.ListValue[k], rowKey + "." + f.Name, subWidth))
								itemChanged = true;
							ImGui::PopID();
						}
						ImGui::TreePop();
					}
					ImGui::EndGroup();

					const ImVec2 slotMax(slotMin.x + slotW, ImGui::GetItemRectMax().y);
					const bool rowHovered = ImGui::IsMouseHoveringRect(slotMin, slotMax)
						&& ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);
					splitter.SetCurrentChannel(dl, 0);  // background card
					dl->AddRectFilled(slotMin, slotMax,
						isSel ? colSelected : (rowHovered ? colHovered : colNormal), rounding);
						dl->AddRect(slotMin, slotMax, colCardBorder, rounding, 0, 1.0f);
					if (isSel) {
							splitter.SetCurrentChannel(dl, 1);  // border ON TOP of content so the widgets don't clip it
							dl->AddRect(slotMin, slotMax, colSelBorder, rounding, 0, 1.5f);
						}
					splitter.Merge(dl);

					if (itemChanged) changed = true;
					ImGui::PopID();
					continue;
				}

				// Rounded "tab" card behind the row, drawn first so the widgets sit on top.
				const ImVec2 rowMin = ImGui::GetCursorScreenPos();
				const float fullW = ImGui::GetContentRegionAvail().x;
				const ImVec2 rowMax(rowMin.x + fullW, rowMin.y + rowH);
				// AND the window-hover test so the card doesn't light up under an open
				// combo/picker popup or an overlapping window (IsMouseHoveringRect is
				// pure geometry and ignores z-order).
				const bool rowHovered = ImGui::IsMouseHoveringRect(rowMin, rowMax)
					&& ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);
				dl->AddRectFilled(rowMin, rowMax,
					isSel ? colSelected : (rowHovered ? colHovered : colNormal), rounding);
					dl->AddRect(rowMin, rowMax, colCardBorder, rounding, 0, 1.0f);
				// (selection outline drawn AFTER the value widget below so it can't be clipped)

				DrawListRowHandle(idx, sel, static_cast<int>(items.size()), handleW, rowH, dl, moveFrom, moveTo);
				DrawListElementContextMenu("##elemCtx", items, idx, itemType, d.Metadata, duplicateIdx, pasteIdx);

				ImGui::SameLine(0.0f, style.ItemSpacing.x);
				const float widgetWidth = std::max(40.0f, ImGui::GetContentRegionAvail().x);

				const std::string rowKey = fieldKey + "/" + std::to_string(i);
				PropertyValue& item = items[i];
				if (DrawListItemValue(itemType, d.Metadata, item, rowKey, widgetWidth)) changed = true;

				if (isSel) dl->AddRect(rowMin, rowMax, colSelBorder, rounding, 0, 1.5f);

				ImGui::PopID();
			}

			// Deferred drag-reorder: rotate the dragged element to the drop row.
			if (moveFrom >= 0 && moveTo >= 0 && moveFrom != moveTo
				&& moveFrom < static_cast<int>(items.size())
				&& moveTo < static_cast<int>(items.size())) {
				if (moveFrom < moveTo)
					std::rotate(items.begin() + moveFrom, items.begin() + moveFrom + 1, items.begin() + moveTo + 1);
				else
					std::rotate(items.begin() + moveTo, items.begin() + moveFrom, items.begin() + moveFrom + 1);
				sel.Selected.clear();
				sel.Selected.insert(moveTo);
				sel.LastClicked = moveTo;
				changed = true;
				ReferencePicker::CancelForPrefix(fieldKey + "/");
			}

			// Deferred Duplicate (insert a copy right after the element) / Paste (replace).
				// Skip if a reorder fired this frame: it rotated `items`, so the captured
				// pre-rotate indices would point at the wrong element. (Can't co-fire via mouse
				// today; this documents/future-proofs the invariant.)
				const bool reorderedThisFrame = (moveFrom >= 0 && moveTo >= 0 && moveFrom != moveTo);
			if (!reorderedThisFrame && duplicateIdx >= 0 && duplicateIdx < static_cast<int>(items.size())) {
				PropertyValue copy = items[static_cast<std::size_t>(duplicateIdx)];
				items.insert(items.begin() + duplicateIdx + 1, std::move(copy));
				sel.Selected.clear();
				sel.Selected.insert(duplicateIdx + 1);
				sel.LastClicked = duplicateIdx + 1;
				changed = true;
				ReferencePicker::CancelForPrefix(fieldKey + "/");
			}
			if (!reorderedThisFrame && pasteIdx >= 0 && pasteIdx < static_cast<int>(items.size())
				&& s_ListClipboard.HasValue
				&& s_ListClipboard.Signature == ListElementSignature(itemType, d.Metadata)) {
				items[static_cast<std::size_t>(pasteIdx)] = s_ListClipboard.Value;
				changed = true;
			}

			// Footer: icon-only Add, and Remove-selected (enabled only with a selection).
			const float footerBtn = ImGui::GetFrameHeight();
			if (IconButton("##listAdd", "plus", footerBtn, /*enabled=*/true)) {
				PropertyValue blank;
				if (isObjectList) {
						blank.Type = PropertyType::List;
						for (const PropertyMetadata::ListItemField& f : d.Metadata.ListItemFields) {
							PropertyValue sub; sub.Type = f.Type;
							blank.ListValue.push_back(std::move(sub));
						}
					}
					else {
						blank.Type = itemType;
					}
				items.push_back(std::move(blank));
				changed = true;
			}
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("Add item");
			ImGui::SameLine();
			const bool hasSelection = !sel.Selected.empty();
			const bool removeClicked = IconButton("##listRemove", "minus", footerBtn, hasSelection);
			if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
				ImGui::SetTooltip(hasSelection ? "Remove selected" : "Remove selected (select rows first)");
			}
			if (removeClicked) {
				std::vector<int> indices(sel.Selected.begin(), sel.Selected.end());
				std::sort(indices.begin(), indices.end());
				for (auto it = indices.rbegin(); it != indices.rend(); ++it) {
					if (*it >= 0 && *it < static_cast<int>(items.size())) {
						items.erase(items.begin() + *it);
					}
				}
				sel.Selected.clear();
				sel.LastClicked = -1;
				changed = true;
				ReferencePicker::CancelForPrefix(fieldKey + "/");
			}

			ImGui::TreePop();
			ImGui::PopID();

			if (changed) {
				PropertyValue out;
				out.Type = PropertyType::List;
				out.ListValue = std::move(items);
				WriteAll(entities, d, out);
			}
			return changed;
		}

		bool DrawStringList(std::span<const Entity> entities, const PropertyDescriptor& d) {
			PropertyValue v;
			const bool uniform = SampleUniform(entities, d, v);
			std::vector<std::string> items = v.StringListValue;

			ImGui::PushID(d.Name.c_str());
			ImGuiUtils::BeginInspectorFieldRow(d.DisplayName.c_str());
			if (!uniform) {
				ImGui::TextDisabled("— (multiple values, edits overwrite all)");
			}
			else {
				ImGui::TextDisabled("%d %s", static_cast<int>(items.size()),
					items.size() == 1 ? "item" : "items");
			}

			bool changed = false;
			int removeIndex = -1;

			for (size_t i = 0; i < items.size(); ++i) {
				ImGui::PushID(static_cast<int>(i));

				ImGui::AlignTextToFramePadding();
				ImGui::Text("%2zu", i);
				ImGui::SameLine();

				const float deleteButtonWidth = ImGui::CalcTextSize("Remove").x
					+ ImGui::GetStyle().FramePadding.x * 2.0f;
				const float availWidth = ImGui::GetContentRegionAvail().x;
				const float spacing = ImGui::GetStyle().ItemSpacing.x;
				ImGui::SetNextItemWidth(std::max(40.0f, availWidth - deleteButtonWidth - spacing));

				char buf[256]{};
				std::snprintf(buf, sizeof(buf), "%s", items[i].c_str());
				if (ImGui::InputText("##entry", buf, sizeof(buf))) {
					items[i] = buf;
					changed = true;
				}
				ImGui::SameLine();
				if (ImGui::Button("Remove", ImVec2(deleteButtonWidth, 0.0f))) {
					removeIndex = static_cast<int>(i);
				}
				if (ImGui::IsItemHovered()) ImGui::SetTooltip("Remove this entry");
				ImGui::PopID();
			}

			if (removeIndex >= 0 && removeIndex < static_cast<int>(items.size())) {
				items.erase(items.begin() + removeIndex);
				changed = true;
			}

			if (ImGui::Button("+ Add")) {
				items.emplace_back();
				changed = true;
			}

			ImGui::PopID();

			if (changed) {
				PropertyValue out;
				out.Type = PropertyType::StringList;
				out.StringListValue = std::move(items);
				WriteAll(entities, d, out);
			}
			return changed;
		}

	} // namespace

	namespace {

		// Resolve EnabledIfFn across the multi-selection. Returns false iff
		// any entity in the span returns false from the predicate. An empty
		// span or no predicate means "always enabled".
		bool EvaluateEnabledIf(std::span<const Entity> entities, const PropertyDescriptor& d) {
			if (!d.Metadata.EnabledIfFn) return true;
			for (const Entity& e : entities) {
				if (!d.Metadata.EnabledIfFn(e)) return false;
			}
			return true;
		}

	} // namespace

	bool Draw(std::span<const Entity> entities, const PropertyDescriptor& d,
		const std::string& fieldKey)
	{
		// Header / spacer / read-only style come from metadata.
		if (!d.Metadata.HeaderContent.empty()) {
			ImGui::Spacing();
			// HeaderSize is a "points over base" bump ([Header]/WithHeader default 5).
			const bool bigHeader = d.Metadata.HeaderSize > 0;
			if (bigHeader) {
				// Bump is clamped to [1,100] (mirrors the [Header] attribute) so a bad value
				// can't blow up the font / layout, whatever the source.
				const int headerSize = std::clamp(d.Metadata.HeaderSize, 1, 100);
				// FontSizeBase is the editor base font (zoom + DPI already folded in). Scale the
				// bump by base/design so the header keeps the same prominence at any zoom/DPI.
				// Fall back to the unscaled design size — never GetFontSize(), which is post-scale
				// and double-applies global scale when fed to PushFont (see imgui.h PushFont docs).
				const float baseFont = ImGui::GetStyle().FontSizeBase > 0.0f
					? ImGui::GetStyle().FontSizeBase
					: k_IndexImGuiFontSize;
				const float bump = static_cast<float>(headerSize)
					* (baseFont / k_IndexImGuiFontSize);
				ImGui::PushFont(nullptr, baseFont + bump);
			}
			ImGui::TextUnformatted(d.Metadata.HeaderContent.c_str());
			if (bigHeader) ImGui::PopFont();
			ImGui::Separator();
			ImGui::Spacing();
		}
		if (d.Metadata.HasSpace && d.Metadata.SpaceHeight > 0.0f) {
			ImGui::Dummy(ImVec2(0.0f, d.Metadata.SpaceHeight));
		}

		const bool enabled = EvaluateEnabledIf(entities, d);
		const bool readOnly = d.Metadata.ReadOnly || !enabled;
		if (readOnly) ImGui::BeginDisabled();

		bool changed = false;
		switch (d.Type) {
		case PropertyType::None:
			break;
		case PropertyType::Bool:    changed = DrawBool(entities, d); break;
		case PropertyType::Int8:    changed = DrawIntScalar(entities, d, -128, 127); break;
		case PropertyType::Int16:   changed = DrawIntScalar(entities, d, -32768, 32767); break;
		case PropertyType::Int32:   changed = DrawIntScalar(entities, d, INT_MIN, INT_MAX); break;
		case PropertyType::UInt8:   changed = DrawUIntScalar(entities, d, 255); break;
		case PropertyType::UInt16:  changed = DrawUIntScalar(entities, d, 65535); break;
		case PropertyType::UInt32:  changed = DrawUIntScalar(entities, d, INT_MAX); break;
		case PropertyType::Int64:
		case PropertyType::UInt64:  changed = DrawScalar64(entities, d); break;
		case PropertyType::Float:   changed = DrawFloat(entities, d); break;
		case PropertyType::Double:  changed = DrawDouble(entities, d); break;
		case PropertyType::String:  changed = DrawString(entities, d); break;
		case PropertyType::StringList: changed = DrawStringList(entities, d); break;
		case PropertyType::List:    changed = DrawList(entities, d, fieldKey); break;
		case PropertyType::Vec2:    changed = DrawFloatVec<2>(entities, d); break;
		case PropertyType::Vec3:    changed = DrawFloatVec<3>(entities, d); break;
		case PropertyType::Vec4:    changed = DrawFloatVec<4>(entities, d); break;
		case PropertyType::IntVec2: changed = DrawIntVec<2>(entities, d); break;
		case PropertyType::IntVec3: changed = DrawIntVec<3>(entities, d); break;
		case PropertyType::IntVec4: changed = DrawIntVec<4>(entities, d); break;
		case PropertyType::Color:   changed = DrawColor(entities, d, fieldKey); break;
		case PropertyType::Enum:    changed = DrawEnum(entities, d); break;
		case PropertyType::FlagEnum:changed = DrawFlagEnum(entities, d); break;
		case PropertyType::TextureRef:
			changed = DrawAssetRefByKind(entities, d, fieldKey, AssetKind::Texture,
				PropertyType::TextureRef, "Select Texture"); break;
		case PropertyType::AudioRef:
			changed = DrawAssetRefByKind(entities, d, fieldKey, AssetKind::Audio,
				PropertyType::AudioRef, "Select Audio"); break;
		case PropertyType::FontRef:
			changed = DrawAssetRefByKind(entities, d, fieldKey, AssetKind::Font,
				PropertyType::FontRef, "Select Font"); break;
		case PropertyType::SceneRef:
			changed = DrawAssetRefByKind(entities, d, fieldKey, AssetKind::Scene,
				PropertyType::SceneRef, "Select Scene"); break;
		case PropertyType::PrefabRef:
			changed = DrawAssetRefByKind(entities, d, fieldKey, AssetKind::Prefab,
				PropertyType::PrefabRef, "Select Prefab"); break;
		case PropertyType::AssetRef:
			changed = DrawAssetRefByKind(entities, d, fieldKey, d.Metadata.AssetKindFilter,
				PropertyType::AssetRef,
				d.Metadata.AssetKindFilter == AssetKind::DataAsset ? "Select Data Asset" : "Select Asset"); break;
		case PropertyType::EntityRef:    changed = DrawEntityRef(entities, d, fieldKey); break;
		case PropertyType::ComponentRef: changed = DrawComponentRef(entities, d, fieldKey); break;
		case PropertyType::Graph:        changed = DrawGraph(entities, d, fieldKey); break;
		}

		if (readOnly) ImGui::EndDisabled();

		// Tooltip (rendered after the row so the rect is captured).
		if (!d.Metadata.Tooltip.empty() && ImGui::IsItemHovered()) {
			ImGui::SetTooltip("%s", d.Metadata.Tooltip.c_str());
		}

		if (!d.VariantBranches.empty() && !entities.empty()) {
			PropertyValue tag;
			const bool tagUniform = SampleUniform(entities, d, tag);
			ImGui::Indent(8.0f);
			if (!tagUniform) {
				ImGui::TextDisabled("Mixed variant — pick one to apply to all");
			}
			else {
				const PropertyDescriptor::Branch* matching = nullptr;
				for (const auto& branch : d.VariantBranches) {
					if (branch.TagValue == tag.IntValue) { matching = &branch; break; }
				}
				if (matching) {
					for (const PropertyDescriptor& sub : matching->Properties) {
						const std::string subKey = fieldKey + "." + sub.Name;
						Draw(entities, sub, subKey);
					}
				}
			}
			ImGui::Unindent(8.0f);
		}

		return changed;
	}

	bool DrawWithPrefix(std::span<const Entity> entities, const PropertyDescriptor& d,
		const std::string& fieldKeyPrefix)
	{
		return Draw(entities, d, fieldKeyPrefix + "." + d.Name);
	}

	void DrawAll(std::span<const Entity> entities,
		std::span<const PropertyDescriptor> descriptors,
		const std::string& fieldKeyPrefix)
	{
		// HeaderContent on a descriptor opens a CollapsingHeader; subsequent descriptors render inside it.
		// Strip HeaderContent from the copy passed to Draw() to avoid the inline separator duplicating the header.
		bool sectionOpen = false;
		bool sectionVisible = false;
		for (const PropertyDescriptor& desc : descriptors) {
			const bool startsSection = !desc.Metadata.HeaderContent.empty();
			if (startsSection) {
				if (sectionOpen) {
					sectionOpen = false;
				}
				ImGui::Spacing();
				const std::string headerLabel = desc.Metadata.HeaderContent
					+ "##" + fieldKeyPrefix + "_" + desc.Metadata.HeaderContent;
				sectionVisible = ImGui::CollapsingHeader(
					headerLabel.c_str(),
					ImGuiTreeNodeFlags_DefaultOpen);

				if (desc.Metadata.ResetSectionFn) {
					const std::string popupId = "##ctx_" + headerLabel;
					if (ImGui::BeginPopupContextItem(popupId.c_str(),
						ImGuiPopupFlags_MouseButtonRight))
					{
						const std::string menuLabel = "Reset \""
							+ desc.Metadata.HeaderContent
							+ "\" to defaults";
						if (ImGui::MenuItem(menuLabel.c_str())) {
							for (const Entity& e : entities) {
								Entity mutEntity = e;
								desc.Metadata.ResetSectionFn(mutEntity);
							}
						}
						ImGui::EndPopup();
					}
				}

				sectionOpen = true;
			}

			if (sectionOpen && !sectionVisible) {
				continue;
			}

			if (startsSection) {
				PropertyDescriptor inner = desc;
				inner.Metadata.HeaderContent.clear();
				inner.Metadata.HeaderSize = 0;
				DrawWithPrefix(entities, inner, fieldKeyPrefix);
			}
			else {
				DrawWithPrefix(entities, desc, fieldKeyPrefix);
			}
		}
	}

} // namespace Index::PropertyDrawer
