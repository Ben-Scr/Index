#include <pch.hpp>
#include "Gui/ComponentInspectors.hpp"

#include "Assets/AssetRegistry.hpp"
#include "Components/Forward.hpp"
#include "Components/General/General.hpp"
#include "Components/UI/UI.hpp"
#include "Components/Tags.hpp"
#include "Components/Graphics/SpriteRendererComponent.hpp"
#include "Components/Graphics/ImageComponent.hpp"
#include "Components/Graphics/Camera2DComponent.hpp"
#include "Components/Graphics/ParticleSystem2DComponent.hpp"
#include "Components/Graphics/TextRendererComponent.hpp"
#include "Components/Physics/BoxCollider2DComponent.hpp"
#include "Components/Physics/CircleCollider2DComponent.hpp"
#include "Components/Physics/PolygonCollider2DComponent.hpp"
#include "Components/Physics/Rigidbody2DComponent.hpp"
#include "Components/Physics/FastBody2DComponent.hpp"
#include "Components/Physics/FastBoxCollider2DComponent.hpp"
#include "Components/Physics/FastCircleCollider2DComponent.hpp"
#include "Components/Audio/AudioSourceComponent.hpp"
#include "Components/UI/ButtonComponent.hpp"
#include "Components/UI/InspectorEventBinding.hpp"
#include "Editor/EditorComponentRegistration.hpp"
#include "Graphics/TextureManager.hpp"
#include "Graphics/Texture2D.hpp"
#include "Gui/HierarchyDragData.hpp"
#include "Gui/ImGuiUtils.hpp"
#include "Inspector/PropertyDrawer.hpp"
#include "Inspector/ReferencePicker.hpp"
#include "Math/Trigonometry.hpp"
#include "Scene/ComponentRegistry.hpp"
#include "Scene/Scene.hpp"
#include "Scene/SceneManager.hpp"
#include "Scripting/InspectorEventDispatch.hpp"
#include "Scripting/ScriptComponent.hpp"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <optional>
#include <type_traits>
#include <variant>

namespace Index {

	namespace {

		template <typename TComponent>
		void DrawPropertiesFor(std::span<const Entity> entities) {
			const auto* info = SceneManager::Get().GetComponentRegistry().GetInfo<TComponent>();
			if (!info || info->properties.empty()) return;
			PropertyDrawer::DrawAll(entities, info->properties, info->displayName);
		}

		template <int N, typename ChannelGet>
		void SampleVecChannels(std::span<const Entity> entities, ChannelGet&& getChan,
			float (&outValues)[N], bool (&outMixed)[N])
		{
			for (int c = 0; c < N; ++c) { outValues[c] = 0.0f; outMixed[c] = false; }
			if (entities.empty()) return;
			for (int c = 0; c < N; ++c) outValues[c] = getChan(entities[0], c);
			for (std::size_t i = 1; i < entities.size(); ++i) {
				for (int c = 0; c < N; ++c) {
					if (!outMixed[c] && getChan(entities[i], c) != outValues[c]) {
						outMixed[c] = true;
					}
				}
			}
		}

		template <int N, typename ChannelGet, typename ChannelSet>
		bool DrawColumnLabeledFloatRow(const char* rowId,
			const char* (&columnLabels)[N],
			std::span<const Entity> entities,
			ChannelGet&& getChan, ChannelSet&& setChan,
			float speed = 0.5f, const char* format = "%.3f",
			const bool* perColumnDisabled = nullptr)
		{
			float values[N];
			bool  mixed[N];
			SampleVecChannels<N>(entities, getChan, values, mixed);

			ImGui::PushID(rowId);

			const ImGuiStyle& style = ImGui::GetStyle();
			const float fullWidth = ImGui::GetContentRegionAvail().x;
			const float spacing = style.ItemInnerSpacing.x;
			const float colWidth = std::max(1.0f, std::floor(
				(fullWidth - spacing * static_cast<float>(N - 1)) / static_cast<float>(N)));

			// Header row — labels positioned at each column's left edge.
			const float startX = ImGui::GetCursorPosX();
			for (int c = 0; c < N; ++c) {
				if (c > 0) ImGui::SameLine();
				ImGui::SetCursorPosX(startX + static_cast<float>(c) * (colWidth + spacing));
				ImGui::TextUnformatted(columnLabels[c]);
			}
			// Cursor naturally advances to a new line for the value row.

			bool anyChanged = false;
			for (int c = 0; c < N; ++c) {
				if (c > 0) ImGui::SameLine(0.0f, spacing);
				ImGui::PushID(c);
				ImGui::SetNextItemWidth(colWidth);
				const bool disabled = perColumnDisabled && perColumnDisabled[c];
				if (disabled) ImGui::BeginDisabled();
				float channelValue = values[c];
				const char* fmt = mixed[c] ? "-" : format;
				if (ImGui::DragFloat("##c", &channelValue, speed, 0.0f, 0.0f, fmt)) {
					for (const Entity& e : entities) setChan(e, c, channelValue);
					ImGuiUtils::MarkSelectionDirty(entities);
					anyChanged = true;
				}
				if (disabled) ImGui::EndDisabled();
				ImGui::PopID();
			}

			ImGui::PopID();
			return anyChanged;
		}

		template <int N, typename ChannelGet, typename ChannelSet>
		bool DrawAxisLabeledVecMulti(const char* rowLabel,
			const char* (&axisLabels)[N],
			std::span<const Entity> entities,
			ChannelGet&& getChan, ChannelSet&& setChan,
			float speed = 0.01f, const char* format = "%.3f")
		{
			float values[N];
			bool  mixed[N];
			SampleVecChannels<N>(entities, getChan, values, mixed);

			ImGui::PushID(rowLabel);
			ImGuiUtils::BeginInspectorFieldRow(rowLabel);

			const ImGuiStyle& style = ImGui::GetStyle();
			const float fullWidth = ImGui::GetContentRegionAvail().x;
			const float spacing = style.ItemInnerSpacing.x;

			float labelWidths[N];
			float totalLabelWidth = 0.0f;
			for (int c = 0; c < N; ++c) {
				labelWidths[c] = ImGui::CalcTextSize(axisLabels[c]).x;
				totalLabelWidth += labelWidths[c];
			}
			// (label + spacing + field) per cell, plus spacing between cells.
			const float remaining = fullWidth - totalLabelWidth
				- spacing * static_cast<float>(2 * N - 1);
			const float fieldWidth = std::max(1.0f, std::floor(remaining / static_cast<float>(N)));

			bool anyChanged = false;
			for (int c = 0; c < N; ++c) {
				if (c > 0) ImGui::SameLine(0.0f, spacing);
				ImGui::AlignTextToFramePadding();
				ImGui::TextUnformatted(axisLabels[c]);
				ImGui::SameLine(0.0f, spacing);
				ImGui::PushID(c);
				ImGui::SetNextItemWidth(fieldWidth);
				float channelValue = values[c];
				const char* fmt = mixed[c] ? "-" : format;
				if (ImGui::DragFloat("##c", &channelValue, speed, 0.0f, 0.0f, fmt)) {
					for (const Entity& e : entities) setChan(e, c, channelValue);
					ImGuiUtils::MarkSelectionDirty(entities);
					anyChanged = true;
				}
				ImGui::PopID();
			}

			ImGui::PopID();
			return anyChanged;
		}

		template <typename Getter, typename Setter>
		bool DrawAxisLabeledFloatMulti(const char* rowLabel,
			const char* axisLabel,
			std::span<const Entity> entities,
			Getter&& get, Setter&& set,
			float speed = 0.1f, const char* format = "%.3f")
		{
			float value = 0.0f;
			const bool uniform = ImGuiUtils::MultiEdit::SampleUniform<float>(entities, get, value);

			ImGui::PushID(rowLabel);
			ImGuiUtils::BeginInspectorFieldRow(rowLabel);

			const ImGuiStyle& style = ImGui::GetStyle();
			const float axisWidth = ImGui::CalcTextSize(axisLabel).x;
			const float fieldWidth = std::max(1.0f,
				ImGui::GetContentRegionAvail().x - axisWidth - style.ItemInnerSpacing.x);

			ImGui::AlignTextToFramePadding();
			ImGui::TextUnformatted(axisLabel);
			ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);
			ImGui::SetNextItemWidth(fieldWidth);

			const bool changed = ImGui::DragFloat("##Value", &value, speed, 0.0f, 0.0f,
				uniform ? format : "-");
			if (changed) {
				for (const Entity& e : entities) set(e, value);
				ImGuiUtils::MarkSelectionDirty(entities);
			}

			ImGui::PopID();
			return changed;
		}

	} // namespace

	void DrawSpriteRendererInspector(std::span<const Entity> entities)
	{
		DrawPropertiesFor<SpriteRendererComponent>(entities);

		if (entities.empty()) return;

		bool textureUniform = true;
		TextureHandle firstHandle = entities[0].GetComponent<SpriteRendererComponent>().TextureHandle;
		for (std::size_t i = 1; i < entities.size(); ++i) {
			if (entities[i].GetComponent<SpriteRendererComponent>().TextureHandle != firstHandle) {
				textureUniform = false;
				break;
			}
		}

		if (!textureUniform) {
			ImGui::TextDisabled("Mixed texture - pick to apply to all");
		}
		else if (Texture2D* texture = firstHandle.IsValid()
			? TextureManager::GetTexture(firstHandle) : nullptr)
		{
			const float texWidth = texture->GetWidth();
			const float texHeight = texture->GetHeight();
			ImGuiUtils::DrawTexturePreview(texture->GetHandle(), texWidth, texHeight, 96.0f, texture->GetFilter());
			ImGui::Text("%.0f x %.0f", texWidth, texHeight);
		}
		else {
			ImGuiUtils::DrawTexturePlaceholder();
		}
	}

	void DrawTransform2DInspector(std::span<const Entity> entities)
	{
		using TC = Transform2DComponent;
		const char* xyLabels[2] = { "X", "Y" };

		DrawAxisLabeledVecMulti<2>("Position", xyLabels, entities,
			[](const Entity& e, int c) {
				const Vec2& p = e.GetComponent<TC>().LocalPosition;
				return c == 0 ? p.x : p.y;
			},
			[](const Entity& e, int c, float v) {
				TC& t = const_cast<Entity&>(e).GetComponent<TC>();
				Vec2 p = t.LocalPosition;
				if (c == 0) p.x = v; else p.y = v;
				t.SetPosition(p);
			},
			0.1f);

		DrawAxisLabeledFloatMulti("Rotation", "Z", entities,
			[](const Entity& e) {
				return Degrees(e.GetComponent<TC>().LocalRotation);
			},
			[](const Entity& e, float v) {
				const_cast<Entity&>(e).GetComponent<TC>().SetRotation(Radians(v));
			},
			1.0f);

		DrawAxisLabeledVecMulti<2>("Scale", xyLabels, entities,
			[](const Entity& e, int c) {
				const Vec2& s = e.GetComponent<TC>().LocalScale;
				return c == 0 ? s.x : s.y;
			},
			[](const Entity& e, int c, float v) {
				TC& t = const_cast<Entity&>(e).GetComponent<TC>();
				Vec2 s = t.LocalScale;
				if (c == 0) s.x = v; else s.y = v;
				t.SetScale(s);
			},
			0.1f);
	}

	// Camera2D properties flow through the auto-drawer.
	void DrawCamera2DInspector(std::span<const Entity> entities)
	{
		DrawPropertiesFor<Camera2DComponent>(entities);
	}

	void DrawFastBody2DInspector(std::span<const Entity> entities)
	{
		DrawPropertiesFor<FastBody2DComponent>(entities);

		if (entities.size() == 1) {
			const auto& body = entities[0].GetComponent<FastBody2DComponent>();
			if (body.IsValid()) {
				ImGui::Separator();
				ImGui::TextDisabled("Runtime");
				Vec2 vel = body.GetVelocity();
				ImGuiUtils::DrawVec2ReadOnly("Velocity", vel);
				Vec2 pos = body.GetPosition();
				ImGuiUtils::DrawVec2ReadOnly("Position", pos);
			}
		}
	}

	void DrawParticleSystem2DInspector(std::span<const Entity> entities)
	{
		DrawPropertiesFor<ParticleSystem2DComponent>(entities);

		if (entities.size() == 1) {
			Entity entity = entities[0];
			auto& ps = const_cast<Entity&>(entity).GetComponent<ParticleSystem2DComponent>();
			TextureHandle previewHandle = ps.GetTextureHandle();
			Texture2D* texture = previewHandle.IsValid()
				? TextureManager::GetTexture(previewHandle) : nullptr;
			if (texture) {
				ImGuiUtils::DrawTexturePreview(texture->GetHandle(), texture->GetWidth(), texture->GetHeight(), 96.0f, texture->GetFilter());
			}
			else {
				ImGuiUtils::DrawTexturePlaceholder();
			}

			ImGui::Spacing();
			ImGui::TextUnformatted("Bursts");
			ImGui::Separator();

			auto bursts = ps.GetBursts();
			int removeIndex = -1;
			for (std::size_t i = 0; i < bursts.size(); ++i) {
				ImGui::PushID(static_cast<int>(i));
				std::string label = "Burst " + std::to_string(i + 1);
				const bool open = ImGui::TreeNodeEx(label.c_str(), ImGuiTreeNodeFlags_DefaultOpen);
				ImGui::SameLine();
				if (ImGui::SmallButton("-")) {
					removeIndex = static_cast<int>(i);
				}
				if (open) {
					int count = static_cast<int>(bursts[i].Count);
					ImGui::SetNextItemWidth(120.0f);
					if (ImGui::InputInt("Count", &count, 0, 0)) {
						bursts[i].Count = static_cast<uint32_t>(std::max(0, count));
						ImGuiUtils::MarkSelectionDirty(entities);
					}
					float interval = bursts[i].Interval;
					ImGui::SetNextItemWidth(120.0f);
					if (ImGui::InputFloat("Interval", &interval, 0.0f, 0.0f, "%.3f")) {
						bursts[i].Interval = std::max(0.0f, interval);
						ImGuiUtils::MarkSelectionDirty(entities);
					}
					ImGui::TreePop();
				}
				ImGui::PopID();
			}

			if (removeIndex >= 0) {
				ps.RemoveBurst(static_cast<std::size_t>(removeIndex));
				ImGuiUtils::MarkSelectionDirty(entities);
			}

			if (ImGui::Button("+ Add Burst")) {
				ps.AddBurst(ParticleSystem2DComponent::Burst{});
				ImGuiUtils::MarkSelectionDirty(entities);
			}

			ImGui::Spacing();
			// "Scale over Lifetime" as a collapsible tab with an inline on/off toggle;
			// the curve editor shows inside when expanded and enabled.
			auto& sol = ps.ParticleSettings.ScaleOverLifetime;
			bool solEnabled = sol.Enabled;
			if (ImGui::Checkbox("##ScaleOverLifetimeEnabled", &solEnabled)) {
				sol.Enabled = solEnabled;
				ImGuiUtils::MarkSelectionDirty(entities);
			}
			ImGui::SameLine();
			if (ImGui::CollapsingHeader("Scale over Lifetime")) {
				// Always show the graph when expanded; pass the toggle through so it
				// renders greyed-out + non-interactive (instead of vanishing) when off.
				if (ImGuiUtils::DrawCurveEditor("ScaleOverLifetimeCurve", sol.Curve, sol.Enabled)) {
					ImGuiUtils::MarkSelectionDirty(entities);
				}
			}

			ImGui::Spacing();
			// "Color over Lifetime" as a collapsible tab with an inline on/off toggle; when
			// enabled, each particle's emit colour is multiplied by this gradient sampled over
			// its normalized lifetime (colour stops sit below the bar, alpha stops above).
			auto& col = ps.ParticleSettings.ColorOverLifetime;
			bool colEnabled = col.Enabled;
			if (ImGui::Checkbox("##ColorOverLifetimeEnabled", &colEnabled)) {
				col.Enabled = colEnabled;
				ImGuiUtils::MarkSelectionDirty(entities);
			}
			ImGui::SameLine();
			if (ImGui::CollapsingHeader("Color over Lifetime")) {
				// Greyed-out + non-interactive (instead of vanishing) when the toggle is off.
				if (ImGuiUtils::DrawGradientEditor("ColorOverLifetimeGradient", col.Gradient, col.Enabled)) {
					ImGuiUtils::MarkSelectionDirty(entities);
				}
			}
		}
	}

	// MUST lock these fields: SliderComponent writes SizeDelta.x each frame and assumes fixed anchor/pivot geometry.
	bool IsEntitySliderFill(const Entity& entity) {
		const Scene* scene = entity.GetScene();
		if (!scene) return false;
		entt::registry& registry = const_cast<entt::registry&>(scene->GetRegistry());
		const EntityHandle target = entity.GetHandle();
		auto view = registry.view<SliderComponent>();
		for (auto&& [_, slider] : view.each()) {
			if (slider.FillEntity == target) return true;
		}
		return false;
	}

	bool AnyEntitySliderFill(std::span<const Entity> entities) {
		for (const auto& e : entities) {
			if (IsEntitySliderFill(e)) return true;
		}
		return false;
	}

	void DrawRectTransform2DInspector(std::span<const Entity> entities)
	{
		using RTC = RectTransform2DComponent;

		const bool fillReadOnly = AnyEntitySliderFill(entities);

		const char* posLabels[] = { "Pos X", "Pos Y" };
		DrawColumnLabeledFloatRow<2>("##Position", posLabels, entities,
			[](const Entity& e, int c) -> float {
				const Vec2& p = e.GetComponent<RTC>().AnchoredPosition;
				return c == 0 ? p.x : p.y;
			},
			[](const Entity& e, int c, float v) {
				Vec2& p = const_cast<Entity&>(e).GetComponent<RTC>().AnchoredPosition;
				(c == 0 ? p.x : p.y) = v;
			});

		// Disable an axis when ContentSizeFitter or WrapMode::None owns it — edits would be overwritten next tick.
		bool sizeAxisDisabled[2] = { false, false };
		for (const Entity& e : entities) {
			if (e.HasComponent<ContentSizeFitterComponent>()) {
				const auto& csf = e.GetComponent<ContentSizeFitterComponent>();
				if (csf.HorizontalFit) sizeAxisDisabled[0] = true;
				if (csf.VerticalFit)   sizeAxisDisabled[1] = true;
			}
			if (e.HasComponent<TextRendererComponent>()) {
				const auto& text = e.GetComponent<TextRendererComponent>();
				if (text.WrapMode == TextWrapMode::None && text.AutoSize) {
					// FitTextNaturalSize (only while AutoSize is on) overwrites both axes.
					sizeAxisDisabled[0] = true;
					sizeAxisDisabled[1] = true;
				}
				else {
					// Wrap active: width drives the wrap boundary (user must
					// set it), height is conceptually owned by the wrapped
					// line count — locking the field avoids a misleading
					// "fixed height" authoring surface for content that
					// auto-flows vertically.
					sizeAxisDisabled[1] = true;
				}
			}
		}

		const char* sizeLabels[] = { "Width", "Height" };
		DrawColumnLabeledFloatRow<2>("##Size", sizeLabels, entities,
			[](const Entity& e, int c) -> float {
				const Vec2& s = e.GetComponent<RTC>().SizeDelta;
				return c == 0 ? s.x : s.y;
			},
			[](const Entity& e, int c, float v) {
				Vec2& s = const_cast<Entity&>(e).GetComponent<RTC>().SizeDelta;
				(c == 0 ? s.x : s.y) = v;
			},
			1.0f, "%.3f", sizeAxisDisabled);

		// Edge-position view: same rect data as Position + Size, but edits move
		// one edge while the opposite edge stays pinned. Useful when the user
		// wants "extend the right edge by 20px" without recalculating
		// AnchoredPosition + SizeDelta around the pivot in their head. Both
		// AnchoredPosition and SizeDelta update so the authored representation
		// stays self-consistent with whatever Position / Size shows above.
		if (ImGui::CollapsingHeader("Edges")) {
			ImGui::PushID("Edges");
			ImGui::Indent(8.0f);

			auto setEdge = [](Entity& e, char which, float newEdge) {
				auto& rect = e.GetComponent<RTC>();
				const Vec2 bl = rect.GetAuthoredBottomLeft();
				const Vec2 tr = rect.GetAuthoredTopRight();
				float left   = bl.x;
				float right  = tr.x;
				float bottom = bl.y;
				float top    = tr.y;
				switch (which) {
					case 'L': left   = newEdge; break;
					case 'R': right  = newEdge; break;
					case 'B': bottom = newEdge; break;
					case 'T': top    = newEdge; break;
				}
				// Clamp to non-negative size — flipping edges past each other
				// would silently produce a negative SizeDelta, which the rest
				// of the UI math treats as zero-extent (invisible rect).
				if (right  < left)   right  = left;
				if (top    < bottom) top    = bottom;
				const Vec2 newSize{ right - left, top - bottom };
				rect.SizeDelta = Vec2{
					rect.LocalScale.x != 0.0f ? newSize.x / rect.LocalScale.x : newSize.x,
					rect.LocalScale.y != 0.0f ? newSize.y / rect.LocalScale.y : newSize.y
				};
				rect.AnchoredPosition = Vec2{
					left   + newSize.x * rect.Pivot.x,
					bottom + newSize.y * rect.Pivot.y
				};
			};

			// Same axis-disable as the Size row above: editing Left/Right
			// changes width, editing Bottom/Top changes height — gating these
			// keeps the alternative edge-edit view consistent with whatever
			// owns the Width/Height field (UILayoutSystem auto-fit, or the
			// wrap-active height lock).
			const bool lrDisabled[2] = { sizeAxisDisabled[0], sizeAxisDisabled[0] };
			const bool btDisabled[2] = { sizeAxisDisabled[1], sizeAxisDisabled[1] };

			const char* lrLabels[] = { "Left", "Right" };
			DrawColumnLabeledFloatRow<2>("##LR", lrLabels, entities,
				[](const Entity& e, int c) -> float {
					const auto& r = e.GetComponent<RTC>();
					return c == 0 ? r.GetAuthoredBottomLeft().x : r.GetAuthoredTopRight().x;
				},
				[&setEdge](const Entity& e, int c, float v) {
					setEdge(const_cast<Entity&>(e), c == 0 ? 'L' : 'R', v);
				},
				1.0f, "%.3f", lrDisabled);

			const char* btLabels[] = { "Bottom", "Top" };
			DrawColumnLabeledFloatRow<2>("##BT", btLabels, entities,
				[](const Entity& e, int c) -> float {
					const auto& r = e.GetComponent<RTC>();
					return c == 0 ? r.GetAuthoredBottomLeft().y : r.GetAuthoredTopRight().y;
				},
				[&setEdge](const Entity& e, int c, float v) {
					setEdge(const_cast<Entity&>(e), c == 0 ? 'B' : 'T', v);
				},
				1.0f, "%.3f", btDisabled);

			ImGui::Unindent(8.0f);
			ImGui::PopID();
		}

		const char* xyLabels[] = { "X", "Y" };

		if (ImGui::CollapsingHeader("Anchors", ImGuiTreeNodeFlags_DefaultOpen)) {
			ImGui::PushID("Anchors");
			ImGui::Indent(8.0f);

			auto applyAnchorPreset = [&](const Vec2& anchor) {
				for (const Entity& e : entities) {
					auto& rect = const_cast<Entity&>(e).GetComponent<RTC>();
					rect.AnchorMin = anchor;
					rect.AnchorMax = anchor;
					rect.Pivot = anchor;
					if (Scene* scene = const_cast<Entity&>(e).GetScene()) {
						scene->MarkDirty();
					}
				}
			};

			struct AnchorPreset {
				const char* Label;
				Vec2 Anchor;
			};
			static const AnchorPreset kAnchorPresets[] = {
				{ "Top Left", { 0.0f, 1.0f } },
				{ "Middle Top", { 0.5f, 1.0f } },
				{ "Top Right", { 1.0f, 1.0f } },
				{ "Middle Left", { 0.0f, 0.5f } },
				{ "Center", { 0.5f, 0.5f } },
				{ "Middle Right", { 1.0f, 0.5f } },
				{ "Bottom Left", { 0.0f, 0.0f } },
				{ "Middle Bottom", { 0.5f, 0.0f } },
				{ "Bottom Right", { 1.0f, 0.0f } },
			};

			if (fillReadOnly) ImGui::BeginDisabled();
			if (ImGui::Button("Presets...")) {
				ImGui::OpenPopup("AnchorPresetsPopup");
			}
			if (ImGui::BeginPopup("AnchorPresetsPopup")) {
				for (int i = 0; i < IM_ARRAYSIZE(kAnchorPresets); ++i) {
					if (i > 0 && (i % 3) != 0) {
						ImGui::SameLine();
					}
					ImGui::PushID(i);
					if (ImGui::Button(kAnchorPresets[i].Label, ImVec2(116.0f, 0.0f))) {
						applyAnchorPreset(kAnchorPresets[i].Anchor);
						ImGui::CloseCurrentPopup();
					}
					ImGui::PopID();
				}
				ImGui::EndPopup();
			}
			if (fillReadOnly) ImGui::EndDisabled();

			if (fillReadOnly) ImGui::BeginDisabled();

			DrawAxisLabeledVecMulti<2>("Min", xyLabels, entities,
				[](const Entity& e, int c) -> float {
					const Vec2& a = e.GetComponent<RTC>().AnchorMin;
					return c == 0 ? a.x : a.y;
				},
				[](const Entity& e, int c, float v) {
					Vec2& a = const_cast<Entity&>(e).GetComponent<RTC>().AnchorMin;
					(c == 0 ? a.x : a.y) = v;
				});

			DrawAxisLabeledVecMulti<2>("Max", xyLabels, entities,
				[](const Entity& e, int c) -> float {
					const Vec2& a = e.GetComponent<RTC>().AnchorMax;
					return c == 0 ? a.x : a.y;
				},
				[](const Entity& e, int c, float v) {
					Vec2& a = const_cast<Entity&>(e).GetComponent<RTC>().AnchorMax;
					(c == 0 ? a.x : a.y) = v;
				});

			if (fillReadOnly) ImGui::EndDisabled();

			ImGui::Unindent(8.0f);
			ImGui::PopID();
		}

		DrawAxisLabeledVecMulti<2>("Pivot", xyLabels, entities,
			[](const Entity& e, int c) -> float {
				const Vec2& p = e.GetComponent<RTC>().Pivot;
				return c == 0 ? p.x : p.y;
			},
			[](const Entity& e, int c, float v) {
				Vec2& p = const_cast<Entity&>(e).GetComponent<RTC>().Pivot;
				(c == 0 ? p.x : p.y) = v;
			});

		ImGuiUtils::DragFloatMulti("Rotation", entities,
			[](const Entity& e) -> float {
				return Degrees(e.GetComponent<RTC>().LocalRotation);
			},
			[](const Entity& e, float v) {
				const_cast<Entity&>(e).GetComponent<RTC>().LocalRotation = Radians(v);
			},
			1.0f);

		DrawAxisLabeledVecMulti<2>("Scale", xyLabels, entities,
			[](const Entity& e, int c) -> float {
				const Vec2& s = e.GetComponent<RTC>().LocalScale;
				return c == 0 ? s.x : s.y;
			},
			[](const Entity& e, int c, float v) {
				Vec2& s = const_cast<Entity&>(e).GetComponent<RTC>().LocalScale;
				(c == 0 ? s.x : s.y) = v;
			},
			0.1f);
	}

	namespace {

		bool EventListsEqual(const InspectorEventList& a, const InspectorEventList& b) {
			if (a.Bindings.size() != b.Bindings.size()) return false;
			for (std::size_t i = 0; i < a.Bindings.size(); ++i) {
				const auto& x = a.Bindings[i];
				const auto& y = b.Bindings[i];
				if (x.TargetEntityUUID != y.TargetEntityUUID) return false;
				if (x.ScriptClassName  != y.ScriptClassName)  return false;
				if (x.MethodName       != y.MethodName)       return false;
				if (x.Enabled          != y.Enabled)          return false;
				if (x.ArgumentKind     != y.ArgumentKind)     return false;
				if (x.ArgumentValue    != y.ArgumentValue)    return false;
			}
			return true;
		}

		std::string DescribeEventTarget(uint64_t uuid) {
			if (uuid == 0) return "(Self)";
			bool missing = false;
			std::string secondary;
			std::string display = ReferencePicker::ResolveEntityDisplay(uuid, missing, &secondary);
			if (missing || display.empty()) return "(Missing)";
			return display;
		}

		std::vector<std::string> CollectTargetScriptClasses(const Scene& scene, EntityHandle target) {
			std::vector<std::string> out;
			if (target == entt::null) return out;
			ScriptComponent* sc = nullptr;
			if (!const_cast<Scene&>(scene).TryGetComponent<ScriptComponent>(target, sc) || sc == nullptr) {
				return out;
			}
			out.reserve(sc->Scripts.size() + sc->ManagedComponents.size());
			for (const ScriptInstance& s : sc->Scripts) {
				if (s.GetClassName().empty()) continue;
				if (std::find(out.begin(), out.end(), s.GetClassName()) == out.end()) {
					out.push_back(s.GetClassName());
				}
			}
			for (const std::string& className : sc->ManagedComponents) {
				if (className.empty()) continue;
				if (std::find(out.begin(), out.end(), className) == out.end()) {
					out.push_back(className);
				}
			}
			return out;
		}

		EntityHandle ResolveRowTarget(const Scene& scene, EntityHandle ownerEntity, uint64_t uuid) {
			if (uuid == 0) return ownerEntity;
			EntityHandle resolved = entt::null;
			const_cast<Scene&>(scene).TryResolveEntityRef(uuid, resolved);
			return resolved;
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

		std::optional<uint64_t> AcceptEntityDrop() {
			std::optional<uint64_t> result;
			if (!ImGui::BeginDragDropTarget()) return result;
			if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("HIERARCHY_ENTITY")) {
				if (payload->DataSize == sizeof(HierarchyDragData)) {
					const auto* data = static_cast<const HierarchyDragData*>(payload->Data);
					const EntityHandle handle = static_cast<EntityHandle>(data->EntityHandle);
					const Scene* scene = ResolveHierarchyPayloadScene(*data);
					if (scene && scene->IsValid(handle)) {
						const uint64_t uuid = scene->GetEntityPersistentID(handle);
						if (uuid != 0) result = uuid;
					}
				}
			}
			ImGui::EndDragDropTarget();
			return result;
		}

		struct MethodEntry {
			std::string Name;
			InspectorEventArgKind Kind = InspectorEventArgKind::Void;
		};

		std::vector<MethodEntry> ParseMethodEntries(const std::vector<std::string>& raw) {
			std::vector<MethodEntry> out;
			out.reserve(raw.size());
			for (const std::string& entry : raw) {
				const std::size_t sep = entry.find_last_of(':');
				MethodEntry me;
				if (sep == std::string::npos) {
					me.Name = entry;
					me.Kind = InspectorEventArgKind::Void;
				}
				else {
					me.Name = entry.substr(0, sep);
					int parsed = 0;
					try { parsed = std::stoi(entry.substr(sep + 1)); } catch (...) { parsed = 0; }
					if (parsed < 0 || parsed > static_cast<int>(InspectorEventArgKind::EntityRef)) {
						parsed = 0;
					}
					me.Kind = static_cast<InspectorEventArgKind>(parsed);
				}
				out.push_back(std::move(me));
			}
			return out;
		}

		const char* ArgKindLabel(InspectorEventArgKind k) {
			switch (k) {
				case InspectorEventArgKind::Void:      return "void";
				case InspectorEventArgKind::Bool:      return "bool";
				case InspectorEventArgKind::Int:       return "int";
				case InspectorEventArgKind::Float:     return "float";
				case InspectorEventArgKind::Double:    return "double";
				case InspectorEventArgKind::String:    return "string";
				case InspectorEventArgKind::Vec2:      return "Vector2";
				case InspectorEventArgKind::Color:     return "Color";
				case InspectorEventArgKind::EntityRef: return "Entity";
			}
			return "?";
		}

		float DecodeFloat(const std::string& s, float fallback = 0.0f) {
			if (s.empty()) return fallback;
			try { return std::stof(s); } catch (...) { return fallback; }
		}
		int DecodeInt(const std::string& s, int fallback = 0) {
			if (s.empty()) return fallback;
			try { return std::stoi(s); } catch (...) { return fallback; }
		}
		double DecodeDouble(const std::string& s, double fallback = 0.0) {
			if (s.empty()) return fallback;
			try { return std::stod(s); } catch (...) { return fallback; }
		}
		uint64_t DecodeUInt64(const std::string& s, uint64_t fallback = 0) {
			if (s.empty()) return fallback;
			try { return std::stoull(s); } catch (...) { return fallback; }
		}
		std::array<float, 2> DecodeVec2(const std::string& s) {
			std::array<float, 2> out{ 0.0f, 0.0f };
			const std::size_t sep = s.find(',');
			if (sep == std::string::npos) return out;
			out[0] = DecodeFloat(s.substr(0, sep));
			out[1] = DecodeFloat(s.substr(sep + 1));
			return out;
		}
		std::array<float, 4> DecodeColor(const std::string& s) {
			std::array<float, 4> out{ 1.0f, 1.0f, 1.0f, 1.0f };
			std::size_t i = 0;
			std::size_t start = 0;
			std::size_t channel = 0;
			while (channel < 4 && start <= s.size()) {
				const std::size_t sep = s.find(',', start);
				const std::string slice = (sep == std::string::npos)
					? s.substr(start) : s.substr(start, sep - start);
				out[channel++] = DecodeFloat(slice, channel <= 3 ? 1.0f : 1.0f);
				if (sep == std::string::npos) break;
				start = sep + 1;
				++i;
			}
			return out;
		}

		template <typename SetValue>
		void DrawArgValueEditor(const InspectorEventBinding& row, float width, SetValue&& setValue) {
			const InspectorEventArgKind k = row.ArgumentKind;
			ImGui::SetNextItemWidth(width);
			switch (k) {
				case InspectorEventArgKind::Void:
					// No editable argument — display a disabled stub so
					// the layout still allocates a column (keeps row
					// alignment consistent across mixed kinds).
					ImGui::BeginDisabled();
					ImGui::Button("(no arg)##NoArg", ImVec2(width, 0.0f));
					ImGui::EndDisabled();
					break;
				case InspectorEventArgKind::Bool: {
					bool v = row.ArgumentValue == "1";
					if (ImGui::Checkbox("##ArgBool", &v)) setValue(v ? "1" : "0");
					break;
				}
				case InspectorEventArgKind::Int: {
					int v = DecodeInt(row.ArgumentValue);
					if (ImGui::DragInt("##ArgInt", &v)) setValue(std::to_string(v));
					break;
				}
				case InspectorEventArgKind::Float: {
					float v = DecodeFloat(row.ArgumentValue);
					if (ImGui::DragFloat("##ArgFloat", &v, 0.1f)) {
						char buf[32];
						std::snprintf(buf, sizeof(buf), "%g", v);
						setValue(std::string(buf));
					}
					break;
				}
				case InspectorEventArgKind::Double: {
					double v = DecodeDouble(row.ArgumentValue);
					if (ImGui::InputDouble("##ArgDouble", &v, 0.0, 0.0, "%.6g")) {
						char buf[32];
						std::snprintf(buf, sizeof(buf), "%.10g", v);
						setValue(std::string(buf));
					}
					break;
				}
				case InspectorEventArgKind::String: {
					char buf[256];
					std::snprintf(buf, sizeof(buf), "%s", row.ArgumentValue.c_str());
					if (ImGui::InputText("##ArgString", buf, sizeof(buf))) {
						setValue(std::string(buf));
					}
					break;
				}
				case InspectorEventArgKind::Vec2: {
					auto vv = DecodeVec2(row.ArgumentValue);
					float v[2] = { vv[0], vv[1] };
					if (ImGui::DragFloat2("##ArgVec2", v, 0.1f)) {
						char buf[64];
						std::snprintf(buf, sizeof(buf), "%g,%g", v[0], v[1]);
						setValue(std::string(buf));
					}
					break;
				}
				case InspectorEventArgKind::Color: {
					auto cc = DecodeColor(row.ArgumentValue);
					float v[4] = { cc[0], cc[1], cc[2], cc[3] };
					if (ImGui::ColorEdit4("##ArgColor", v,
							ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar))
					{
						char buf[96];
						std::snprintf(buf, sizeof(buf), "%g,%g,%g,%g", v[0], v[1], v[2], v[3]);
						setValue(std::string(buf));
					}
					break;
				}
				case InspectorEventArgKind::EntityRef: {
					const uint64_t uuid = DecodeUInt64(row.ArgumentValue);
					std::string label = uuid == 0 ? k_NoneLabel : DescribeEventTarget(uuid);
					ImGui::Button((label + "##ArgEntity").c_str(), ImVec2(width, 0.0f));
					break;
				}
			}
		}

	} // namespace

	template <typename GetList, typename MutateAll>
	void DrawEventListFoldout(const char* label, const char* idSuffix,
		std::span<const Entity> entities,
		GetList&& getList, MutateAll&& mutate)
	{
		const std::string foldoutId = std::string(label) + "##" + idSuffix;
		if (!ImGui::CollapsingHeader(foldoutId.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
			return;
		}
		if (entities.empty()) return;

		// MUST push: two event lists (e.g. OnValueChanged + OnSubmitted) share row IDs; without this outer push they collide and trigger the "ID stack collision" assertion.
		ImGui::PushID(idSuffix);

		const InspectorEventList& first = getList(entities[0]);
		bool uniform = true;
		for (std::size_t i = 1; i < entities.size(); ++i) {
			if (!EventListsEqual(first, getList(entities[i]))) {
				uniform = false;
				break;
			}
		}
		if (!uniform) {
			ImGui::TextDisabled("Multiple values");
			ImGui::SameLine();
			std::string applyLabel = std::string("Apply primary's list to all##Apply") + idSuffix;
			if (ImGui::SmallButton(applyLabel.c_str())) {
				const InspectorEventList src = first;
				mutate([&src](InspectorEventList& dst) { dst = src; });
			}
			ImGui::PopID();
			return;
		}

		const Scene* scene = entities[0].GetScene();
		const EntityHandle owner = entities[0].GetHandle();
		const auto& bindings = first.Bindings;

		const ImGuiStyle& style = ImGui::GetStyle();
		const float spacing = style.ItemInnerSpacing.x;
		const float minusWidth = ImGui::CalcTextSize("-").x + style.FramePadding.x * 2.0f;
		const float checkWidth = ImGui::GetFrameHeight();

		ImGui::Indent();
		if (bindings.empty()) {
			ImGui::TextDisabled("List is Empty");
		}
		else {
			std::optional<std::size_t> rowToRemove;
			for (std::size_t i = 0; i < bindings.size(); ++i) {
				const InspectorEventBinding& row = bindings[i];
				ImGui::PushID(static_cast<int>(i));

				const float totalWidth = ImGui::GetContentRegionAvail().x;
				const float fieldsWidth = std::max(160.0f,
					totalWidth - checkWidth - minusWidth - spacing * 5.0f);
				const float fieldWidth = std::floor(fieldsWidth / 4.0f);

				bool enabled = row.Enabled;
				if (ImGui::Checkbox("##Enabled", &enabled)) {
					mutate([i, enabled](InspectorEventList& list) {
						if (i < list.Bindings.size()) list.Bindings[i].Enabled = enabled;
					});
				}
				ImGui::SameLine(0.0f, spacing);

				const std::string targetKey = std::string(idSuffix) + ".target." + std::to_string(i);
				if (auto picked = ReferencePicker::ConsumeSelection(targetKey)) {
					uint64_t newUuid = 0;
					const std::string& s = *picked;
					if (s.rfind("prefab:", 0) != 0 && !s.empty()) {
						newUuid = std::strtoull(s.c_str(), nullptr, 10);
					}
					mutate([i, newUuid](InspectorEventList& list) {
						if (i < list.Bindings.size()) list.Bindings[i].TargetEntityUUID = newUuid;
					});
				}
				const std::string targetLabel = DescribeEventTarget(row.TargetEntityUUID);
				bool truncatedTarget = false;
				const std::string targetText = ImGuiUtils::Ellipsize(targetLabel,
					fieldWidth - style.FramePadding.x * 2.0f, &truncatedTarget);
				if (ImGui::Button((targetText + "##Target").c_str(), ImVec2(fieldWidth, 0.0f))) {
					ReferencePicker::OpenForFieldKey(targetKey, "Select Entity",
						ReferencePicker::CollectEntities(/*includePrefabAssets=*/false));
				}
				if (auto droppedUuid = AcceptEntityDrop()) {
					mutate([i, uuid = *droppedUuid](InspectorEventList& list) {
						if (i < list.Bindings.size()) list.Bindings[i].TargetEntityUUID = uuid;
					});
				}
				if (truncatedTarget && ImGui::IsItemHovered()) {
					ImGui::SetTooltip("%s", targetLabel.c_str());
				}
				ImGui::SameLine(0.0f, spacing);

				const EntityHandle resolvedTarget = scene
					? ResolveRowTarget(*scene, owner, row.TargetEntityUUID)
					: entt::null;
				const std::vector<std::string> classes = scene
					? CollectTargetScriptClasses(*scene, resolvedTarget)
					: std::vector<std::string>{};
				const char* classPreview = row.ScriptClassName.empty()
					? (classes.empty() ? "<no scripts>" : "<select>")
					: row.ScriptClassName.c_str();
				ImGui::SetNextItemWidth(fieldWidth);
				if (ImGui::BeginCombo("##Class", classPreview)) {
					for (const std::string& className : classes) {
						const bool selected = (className == row.ScriptClassName);
						if (ImGui::Selectable(className.c_str(), selected)) {
							mutate([i, &className](InspectorEventList& list) {
								if (i < list.Bindings.size()) {
									list.Bindings[i].ScriptClassName = className;
									list.Bindings[i].MethodName.clear();
									list.Bindings[i].ArgumentKind = InspectorEventArgKind::Void;
									list.Bindings[i].ArgumentValue.clear();
								}
							});
						}
						if (selected) ImGui::SetItemDefaultFocus();
					}
					ImGui::EndCombo();
				}
				ImGui::SameLine(0.0f, spacing);

				std::string methodPreview;
				if (row.MethodName.empty()) {
					methodPreview = row.ScriptClassName.empty() ? "<pick class>" : "<select>";
				}
				else {
					methodPreview = row.MethodName;
					if (row.ArgumentKind != InspectorEventArgKind::Void) {
						methodPreview += " (";
						methodPreview += ArgKindLabel(row.ArgumentKind);
						methodPreview += ")";
					}
				}
				ImGui::SetNextItemWidth(fieldWidth);
				if (ImGui::BeginCombo("##Method", methodPreview.c_str())) {
					if (!row.ScriptClassName.empty()) {
						const std::vector<std::string> raw =
							InspectorEvents::GetInvokableMethods(row.ScriptClassName);
						const std::vector<MethodEntry> methods = ParseMethodEntries(raw);
						if (methods.empty()) {
							ImGui::TextDisabled("(no invokable methods)");
						}
						for (const MethodEntry& m : methods) {
							std::string display = m.Name;
							if (m.Kind != InspectorEventArgKind::Void) {
								display += " (";
								display += ArgKindLabel(m.Kind);
								display += ")";
							}
							const bool selected = (m.Name == row.MethodName);
							if (ImGui::Selectable(display.c_str(), selected)) {
								mutate([i, m](InspectorEventList& list) {
									if (i < list.Bindings.size()) {
										auto& b = list.Bindings[i];
										b.MethodName = m.Name;
										// Reset value when arg type changes
										// — the old encoded payload makes
										// no sense for the new kind.
										if (b.ArgumentKind != m.Kind) {
											b.ArgumentValue.clear();
										}
										b.ArgumentKind = m.Kind;
									}
								});
							}
							if (selected) ImGui::SetItemDefaultFocus();
						}
					}
					ImGui::EndCombo();
				}
				ImGui::SameLine(0.0f, spacing);

				if (row.ArgumentKind == InspectorEventArgKind::EntityRef) {
					const std::string argKey = std::string(idSuffix) + ".arg." + std::to_string(i);
					if (auto picked = ReferencePicker::ConsumeSelection(argKey)) {
						uint64_t newUuid = 0;
						const std::string& s = *picked;
						if (s.rfind("prefab:", 0) != 0 && !s.empty()) {
							newUuid = std::strtoull(s.c_str(), nullptr, 10);
						}
						mutate([i, newUuid](InspectorEventList& list) {
							if (i < list.Bindings.size()) {
								list.Bindings[i].ArgumentValue = std::to_string(newUuid);
							}
						});
					}
					const uint64_t argUuid = DecodeUInt64(row.ArgumentValue);
					std::string argLabel = argUuid == 0 ? k_NoneLabel : DescribeEventTarget(argUuid);
					bool truncatedArg = false;
					const std::string argText = ImGuiUtils::Ellipsize(argLabel,
						fieldWidth - style.FramePadding.x * 2.0f, &truncatedArg);
					if (ImGui::Button((argText + "##ArgEntityPick").c_str(), ImVec2(fieldWidth, 0.0f))) {
						ReferencePicker::OpenForFieldKey(argKey, "Select Entity",
							ReferencePicker::CollectEntities(/*includePrefabAssets=*/false));
					}
					if (auto droppedUuid = AcceptEntityDrop()) {
						mutate([i, uuid = *droppedUuid](InspectorEventList& list) {
							if (i < list.Bindings.size()) {
								list.Bindings[i].ArgumentValue = std::to_string(uuid);
							}
						});
					}
					if (truncatedArg && ImGui::IsItemHovered()) {
						ImGui::SetTooltip("%s", argLabel.c_str());
					}
				}
				else {
					DrawArgValueEditor(row, fieldWidth, [&mutate, i](std::string newValue) {
						mutate([i, value = std::move(newValue)](InspectorEventList& list) {
							if (i < list.Bindings.size()) list.Bindings[i].ArgumentValue = value;
						});
					});
				}
				ImGui::SameLine(0.0f, spacing);

				if (ImGui::Button("-##Remove", ImVec2(minusWidth, 0.0f))) {
					rowToRemove = i;
				}
				ImGui::PopID();
			}
			if (rowToRemove) {
				const std::size_t i = *rowToRemove;
				mutate([i](InspectorEventList& list) {
					if (i < list.Bindings.size()) list.Bindings.erase(list.Bindings.begin() + i);
				});
			}
		}
		ImGui::Unindent();

		ImGui::Spacing();
		std::string addLabel = std::string("+##Add") + idSuffix;
		if (ImGui::Button(addLabel.c_str())) {
			mutate([](InspectorEventList& list) {
				list.Bindings.emplace_back();
			});
		}

		ImGui::PopID();
	}

	template <typename TComponent, typename ListPtr>
	static void DrawComponentEventList(const char* label, const char* idSuffix,
		std::span<const Entity> entities, ListPtr listPtr)
	{
		auto getList = [listPtr](const Entity& e) -> const InspectorEventList& {
			return e.GetComponent<TComponent>().*listPtr;
		};
		auto mutateAll = [listPtr, entities](auto&& fn) {
			for (const Entity& e : entities) {
				auto& c = const_cast<Entity&>(e).GetComponent<TComponent>();
				fn(c.*listPtr);
				if (Scene* s = const_cast<Entity&>(e).GetScene()) s->MarkDirty();
			}
		};
		DrawEventListFoldout(label, idSuffix, entities, getList, mutateAll);
	}

	void DrawButtonInspector(std::span<const Entity> entities)
	{
		DrawPropertiesFor<ButtonComponent>(entities);
		if (entities.empty()) return;

		ImGui::Spacing();
		DrawComponentEventList<ButtonComponent>("On Click ()", "Button.OnClick",
			entities, &ButtonComponent::OnClick);

		ReferencePicker::RenderPopup();
	}

	void DrawSliderInspector(std::span<const Entity> entities)
	{
		DrawPropertiesFor<SliderComponent>(entities);
		if (entities.empty()) return;
		ImGui::Spacing();
		DrawComponentEventList<SliderComponent>("On Value Changed (float)",
			"Slider.OnValueChanged", entities, &SliderComponent::OnValueChanged);
		ReferencePicker::RenderPopup();
	}

	void DrawToggleInspector(std::span<const Entity> entities)
	{
		DrawPropertiesFor<ToggleComponent>(entities);
		if (entities.empty()) return;
		ImGui::Spacing();
		DrawComponentEventList<ToggleComponent>("On Value Changed (bool)",
			"Toggle.OnValueChanged", entities, &ToggleComponent::OnValueChanged);
		ReferencePicker::RenderPopup();
	}

	void DrawInputFieldInspector(std::span<const Entity> entities)
	{
		DrawPropertiesFor<InputFieldComponent>(entities);
		if (entities.empty()) return;
		ImGui::Spacing();
		DrawComponentEventList<InputFieldComponent>("On Value Changed (string)",
			"InputField.OnValueChanged", entities, &InputFieldComponent::OnValueChanged);
		ImGui::Spacing();
		DrawComponentEventList<InputFieldComponent>("On Submitted (string)",
			"InputField.OnSubmitted", entities, &InputFieldComponent::OnSubmitted);
		ReferencePicker::RenderPopup();
	}

	void DrawDropdownInspector(std::span<const Entity> entities)
	{
		DrawPropertiesFor<DropdownComponent>(entities);
		if (entities.empty()) return;
		ImGui::Spacing();
		DrawComponentEventList<DropdownComponent>("On Value Changed (int)",
			"Dropdown.OnValueChanged", entities, &DropdownComponent::OnValueChanged);
		ReferencePicker::RenderPopup();
	}

} // namespace Index
