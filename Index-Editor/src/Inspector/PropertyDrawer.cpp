#include "pch.hpp"
#include "Inspector/PropertyDrawer.hpp"

#include "Assets/AssetRegistry.hpp"
#include "Gui/HierarchyDragData.hpp"
#include "Gui/ImGuiUtils.hpp"
#include "Gui/SpriteSliceDragPayload.hpp"
#include "Inspector/PropertyType.hpp"
#include "Inspector/PropertyValue.hpp"
#include "Inspector/ReferencePicker.hpp"
#include "Scene/ComponentRegistry.hpp"
#include "Scene/Scene.hpp"
#include "Scene/SceneManager.hpp"
#include "Utils/ExpressionEvaluator.hpp"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
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
			for (const Entity& e : entities) {
				d.Set(const_cast<Entity&>(e), value);
				MarkSceneDirty(e);
			}
		}

		// Per-channel writes: only the edited channel is broadcast, so multi-select doesn't clobber other channels.
		void WriteFloatChannel(std::span<const Entity> entities,
			const PropertyDescriptor& d, std::size_t channel, float newValue)
		{
			for (const Entity& e : entities) {
				PropertyValue current = d.Get(e);
				current.FloatVec[channel] = newValue;
				d.Set(const_cast<Entity&>(e), current);
				MarkSceneDirty(e);
			}
		}
		void WriteIntChannel(std::span<const Entity> entities,
			const PropertyDescriptor& d, std::size_t channel, int32_t newValue)
		{
			for (const Entity& e : entities) {
				PropertyValue current = d.Get(e);
				current.IntVec[channel] = newValue;
				d.Set(const_cast<Entity&>(e), current);
				MarkSceneDirty(e);
			}
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
				changed = ImGui::DragFloat("##Value", &tmp, speed, clampMin, clampMax,
					uniform ? "%.3f" : "-");
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
			const bool showSteppers = !d.Metadata.HideStepperButtons;
			const float buttonWidth = showSteppers
				? std::min(22.0f, std::max(16.0f, componentWidth * 0.22f))
				: 0.0f;
			bool any = false;
			for (int c = 0; c < static_cast<int>(N); ++c) {
				if (c > 0) ImGui::SameLine(0.0f, componentSpacing);
				ImGui::PushID(c);
				const float pre = values[c];
				float channel = pre;
				const char* fmt = mixed[c] ? "-" : "%.3f";
				bool channelChanged = false;
				if (showSteppers) {
					if (ImGui::Button("-", ImVec2(buttonWidth, 0.0f))) {
						channel -= speed;
						channelChanged = true;
					}
					ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);
				}
				const float inputWidth = showSteppers
					? std::max(1.0f, componentWidth - buttonWidth * 2.0f - style.ItemInnerSpacing.x * 2.0f)
					: componentWidth;
				ImGui::SetNextItemWidth(inputWidth);
				channelChanged |= ImGui::DragFloat("##c", &channel, speed, 0.0f, 0.0f, fmt);
				if (showSteppers) {
					ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);
					if (ImGui::Button("+", ImVec2(buttonWidth, 0.0f))) {
						channel += speed;
						channelChanged = true;
					}
				}
				const bool changed = channelChanged && channel != pre;
				if (changed) {
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
			const bool showSteppers = !d.Metadata.HideStepperButtons;
			const float buttonWidth = showSteppers
				? std::min(22.0f, std::max(16.0f, componentWidth * 0.22f))
				: 0.0f;
			bool any = false;
			for (int c = 0; c < static_cast<int>(N); ++c) {
				if (c > 0) ImGui::SameLine(0.0f, componentSpacing);
				ImGui::PushID(c);
				// C7: same per-channel delta gate as DrawFloatVec.
				const int pre = values[c];
				int channel = pre;
				const char* fmt = mixed[c] ? "-" : "%d";
				bool channelChanged = false;
				if (showSteppers) {
					if (ImGui::Button("-", ImVec2(buttonWidth, 0.0f))) {
						channel -= std::max(1, static_cast<int>(speed));
						channelChanged = true;
					}
					ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);
				}
				const float inputWidth = showSteppers
					? std::max(1.0f, componentWidth - buttonWidth * 2.0f - style.ItemInnerSpacing.x * 2.0f)
					: componentWidth;
				ImGui::SetNextItemWidth(inputWidth);
				channelChanged |= ImGui::DragInt("##c", &channel, speed, 0, 0, fmt);
				if (showSteppers) {
					ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);
					if (ImGui::Button("+", ImVec2(buttonWidth, 0.0f))) {
						channel += std::max(1, static_cast<int>(speed));
						channelChanged = true;
					}
				}
				const bool changed = channelChanged && channel != pre;
				if (changed) {
					WriteIntChannel(entities, d, static_cast<std::size_t>(c), channel);
					any = true;
				}
				ImGui::PopID();
			}
			ImGui::PopID();
			return any;
		}

		bool DrawColor(std::span<const Entity> entities, const PropertyDescriptor& d) {
			std::array<float, 4> values{};
			std::array<bool, 4> mixed{};
			SampleFloatChannels<4>(entities, d, values, mixed);
			const bool anyMixed = mixed[0] || mixed[1] || mixed[2] || mixed[3];
			ImGui::PushID(d.Name.c_str());
			ImGuiUtils::BeginInspectorFieldRow(d.DisplayName.c_str());
			bool anyChanged = false;
			if (!anyMixed) {
				float editVals[4] = { values[0], values[1], values[2], values[3] };
				if (ImGui::ColorEdit4("##Value", editVals)) {
					for (int c = 0; c < 4; ++c) {
						if (editVals[c] != values[c]) {
							WriteFloatChannel(entities, d, static_cast<std::size_t>(c), editVals[c]);
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
		template <typename DisplayResolver, typename PickerOpener, typename PayloadAccept>
		bool DrawReference(std::span<const Entity> entities, const PropertyDescriptor& d,
			const std::string& fieldKey, PropertyType valueType,
			DisplayResolver&& displayResolver, PickerOpener&& pickerOpener,
			PayloadAccept&& payloadAccept)
		{
			PropertyValue v;
			const bool uniform = SampleUniform(entities, d, v);
			bool missing = false;
			std::string secondary;
			std::string display = uniform ? displayResolver(v, missing, secondary)
				: std::string("\xe2\x80\x94");

			bool hovered = false;
			const bool clicked = ReferencePicker::DrawReferenceField(
				d.DisplayName.c_str(), display, secondary, missing, !uniform, hovered);

			bool changed = false;
			if (clicked) {
				pickerOpener(fieldKey);
			}
			if (DrawReferenceContextMenu("##RefCtx")) {
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
			if (auto pending = ReferencePicker::ConsumeSelection(fieldKey); pending) {
				PropertyValue out = PropertyValue::FromString(valueType, *pending);
				WriteAll(entities, d, out);
				changed = true;
			}
			(void)hovered;
			return changed;
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

			return DrawReference(entities, d, fieldKey, valueType,
				[kind](const PropertyValue& v, bool& missing, std::string& secondary) {
					return ReferencePicker::ResolveAssetDisplay(v.UIntValue, kind, missing, &secondary);
				},
				[kind, pickerTitle, style](const std::string& key) {
					ReferencePicker::OpenForFieldKey(key, pickerTitle,
						ReferencePicker::CollectAssetsByKind(kind), style);
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

		bool DrawList(std::span<const Entity> entities, const PropertyDescriptor& d) {
			PropertyValue v;
			const bool uniform = SampleUniform(entities, d, v);
			std::vector<PropertyValue> items = v.ListValue;
			const PropertyType itemType = d.Metadata.ListItemType;

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

			for (std::size_t i = 0; i < items.size(); ++i) {
				ImGui::PushID(static_cast<int>(i));

				ImGui::AlignTextToFramePadding();
				ImGui::Text("%2zu", i);
				ImGui::SameLine();

				const float deleteButtonWidth = ImGui::CalcTextSize("Remove").x
					+ ImGui::GetStyle().FramePadding.x * 2.0f;
				const float availWidth = ImGui::GetContentRegionAvail().x;
				const float spacing = ImGui::GetStyle().ItemSpacing.x;
				const float widgetWidth = std::max(40.0f, availWidth - deleteButtonWidth - spacing);
				ImGui::SetNextItemWidth(widgetWidth);

				PropertyValue& item = items[i];
				item.Type = itemType;

				bool itemChanged = false;
				switch (itemType) {
				case PropertyType::Bool: {
					bool b = item.BoolValue;
					if (ImGui::Checkbox("##v", &b)) { item.BoolValue = b; itemChanged = true; }
					break;
				}
				case PropertyType::Int8:
				case PropertyType::Int16:
				case PropertyType::Int32: {
					int tmp = static_cast<int>(item.IntValue);
					const float speed = d.Metadata.DragSpeed > 0 ? d.Metadata.DragSpeed : 1.0f;
					if (ImGui::DragInt("##v", &tmp, speed)) { item.IntValue = tmp; itemChanged = true; }
					break;
				}
				case PropertyType::UInt8:
				case PropertyType::UInt16:
				case PropertyType::UInt32: {
					int tmp = static_cast<int>(std::min<uint64_t>(item.UIntValue, INT_MAX));
					const float speed = d.Metadata.DragSpeed > 0 ? d.Metadata.DragSpeed : 1.0f;
					if (ImGui::DragInt("##v", &tmp, speed, 0, INT_MAX)) {
						item.UIntValue = static_cast<uint64_t>(std::max(tmp, 0));
						itemChanged = true;
					}
					break;
				}
				case PropertyType::Float: {
					float tmp = static_cast<float>(item.FloatValue);
					const float speed = d.Metadata.DragSpeed > 0 ? d.Metadata.DragSpeed : 0.1f;
					if (ImGui::DragFloat("##v", &tmp, speed)) {
						item.FloatValue = static_cast<double>(tmp); itemChanged = true;
					}
					break;
				}
				case PropertyType::Double: {
					double tmp = item.FloatValue;
					const float speed = d.Metadata.DragSpeed > 0 ? d.Metadata.DragSpeed : 0.1f;
					if (ImGui::DragScalar("##v", ImGuiDataType_Double, &tmp, speed)) {
						item.FloatValue = tmp; itemChanged = true;
					}
					break;
				}
				case PropertyType::String: {
					char buf[1024]{};
					std::snprintf(buf, sizeof(buf), "%s", item.StringValue.c_str());
					if (ImGui::InputText("##v", buf, sizeof(buf))) {
						item.StringValue = buf; itemChanged = true;
					}
					break;
				}
				case PropertyType::Vec2: {
					float vec[2] = { item.FloatVec[0], item.FloatVec[1] };
					if (ImGui::DragFloat2("##v", vec, 0.1f)) {
						item.FloatVec[0] = vec[0]; item.FloatVec[1] = vec[1]; itemChanged = true;
					}
					break;
				}
				case PropertyType::Vec3: {
					float vec[3] = { item.FloatVec[0], item.FloatVec[1], item.FloatVec[2] };
					if (ImGui::DragFloat3("##v", vec, 0.1f)) {
						item.FloatVec[0] = vec[0]; item.FloatVec[1] = vec[1]; item.FloatVec[2] = vec[2];
						itemChanged = true;
					}
					break;
				}
				case PropertyType::Vec4: {
					float vec[4] = { item.FloatVec[0], item.FloatVec[1], item.FloatVec[2], item.FloatVec[3] };
					if (ImGui::DragFloat4("##v", vec, 0.1f)) {
						for (int c = 0; c < 4; ++c) item.FloatVec[c] = vec[c];
						itemChanged = true;
					}
					break;
				}
				case PropertyType::Color: {
					float vec[4] = { item.FloatVec[0], item.FloatVec[1], item.FloatVec[2], item.FloatVec[3] };
					if (ImGui::ColorEdit4("##v", vec)) {
						for (int c = 0; c < 4; ++c) item.FloatVec[c] = vec[c];
						itemChanged = true;
					}
					break;
				}
				case PropertyType::Enum: {
					if (d.Metadata.Enum && !d.Metadata.Enum->Options.empty()) {
						const char* preview = "Unknown";
						for (const auto& opt : d.Metadata.Enum->Options) {
							if (opt.Value == item.IntValue) { preview = opt.Name.c_str(); break; }
						}
						if (ImGui::BeginCombo("##v", preview)) {
							for (const auto& opt : d.Metadata.Enum->Options) {
								const bool isSelected = (opt.Value == item.IntValue);
								if (ImGui::Selectable(opt.Name.c_str(), isSelected)) {
									item.IntValue = opt.Value; itemChanged = true;
								}
								if (isSelected) ImGui::SetItemDefaultFocus();
							}
							ImGui::EndCombo();
						}
					}
					break;
				}
				default:
					ImGui::TextDisabled("(unsupported list item type)");
					break;
				}

				ImGui::SameLine();
				if (ImGui::Button("Remove", ImVec2(deleteButtonWidth, 0.0f))) {
					removeIndex = static_cast<int>(i);
				}
				if (ImGui::IsItemHovered()) ImGui::SetTooltip("Remove this entry");

				if (itemChanged) changed = true;
				ImGui::PopID();
			}

			if (removeIndex >= 0 && removeIndex < static_cast<int>(items.size())) {
				items.erase(items.begin() + removeIndex);
				changed = true;
			}

			if (ImGui::Button("+ Add")) {
				PropertyValue blank;
				blank.Type = itemType;
				items.push_back(std::move(blank));
				changed = true;
			}

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
			ImGui::TextUnformatted(d.Metadata.HeaderContent.c_str());
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
		case PropertyType::List:    changed = DrawList(entities, d); break;
		case PropertyType::Vec2:    changed = DrawFloatVec<2>(entities, d); break;
		case PropertyType::Vec3:    changed = DrawFloatVec<3>(entities, d); break;
		case PropertyType::Vec4:    changed = DrawFloatVec<4>(entities, d); break;
		case PropertyType::IntVec2: changed = DrawIntVec<2>(entities, d); break;
		case PropertyType::IntVec3: changed = DrawIntVec<3>(entities, d); break;
		case PropertyType::IntVec4: changed = DrawIntVec<4>(entities, d); break;
		case PropertyType::Color:   changed = DrawColor(entities, d); break;
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
			changed = DrawAssetRefByKind(entities, d, fieldKey, AssetKind::Unknown,
				PropertyType::AssetRef, "Select Asset"); break;
		case PropertyType::EntityRef:    changed = DrawEntityRef(entities, d, fieldKey); break;
		case PropertyType::ComponentRef: changed = DrawComponentRef(entities, d, fieldKey); break;
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
