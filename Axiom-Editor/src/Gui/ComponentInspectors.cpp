#include <pch.hpp>
#include "Gui/ComponentInspectors.hpp"

#include "Components/Components.hpp"
#include "Editor/EditorComponentRegistration.hpp"
#include "Graphics/TextureManager.hpp"
#include "Graphics/Texture2D.hpp"
#include "Gui/ImGuiUtils.hpp"
#include "Inspector/PropertyDrawer.hpp"
#include "Math/Trigonometry.hpp"
#include "Scene/ComponentRegistry.hpp"
#include "Scene/Scene.hpp"
#include "Scene/SceneManager.hpp"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <type_traits>
#include <variant>

namespace Axiom {

	namespace {

		// Common helper for hybrid inspectors: render every PropertyDescriptor
		// the component declared in BuiltInComponentRegistration.cpp, then the
		// caller appends its own extras. The component's display name is the
		// field-key prefix so reference fields don't collide with same-named
		// fields on other components.
		template <typename TComponent>
		void DrawPropertiesFor(std::span<const Entity> entities) {
			const auto* info = SceneManager::Get().GetComponentRegistry().GetInfo<TComponent>();
			if (!info || info->properties.empty()) return;
			PropertyDrawer::DrawAll(entities, info->properties, info->displayName);
		}

		// ── RectTransform2D layout helpers ───────────────────────────────
		// Sample N channels across `entities` via getChan(entity, channel).
		// Out: per-channel value of entities[0] + per-channel mixed mask.
		// Used by both DrawColumnLabeledFloatRow and DrawAxisLabeledVecMulti.
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

		// Layout: full inspector width split into N columns; each column is
		// a stacked (header text, drag-float) pair (Unity's Pos X / Pos Y).
		// On edit, only the touched channel is written per entity so
		// untouched channels survive across the selection.
		// `perColumnDisabled` (optional, may be null) gates each column
		// individually behind ImGui::BeginDisabled — used by the
		// RectTransform inspector to lock Width when ContentSizeFitter
		// or a no-wrap TextRenderer owns the rect, without losing the
		// drag affordance on the sibling Height column.
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

		// Layout: standard inspector row (label column + value column), with
		// the value column carrying N (axis-letter, drag-float) pairs side
		// by side. Used for the Min / Max / Pivot / Scale rows where the
		// row's identity comes from the left label.
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

	} // namespace

	// ── SpriteRenderer ───────────────────────────────────────────────
	// Properties (Color, SortingOrder, SortingLayer, Texture) flow through
	// the auto-drawer. Texture picker uses the unified ReferencePicker
	// (thumbnail style). The extras here are the per-texture preview +
	// filter/wrap controls that live on Texture2D, not the component.

	void DrawSpriteRendererInspector(std::span<const Entity> entities)
	{
		DrawPropertiesFor<SpriteRendererComponent>(entities);

		if (entities.empty()) return;

		// Preview only when the selection's textures are uniform — otherwise
		// "which texture's filter would we be editing?" has no sane answer.
		bool textureUniform = true;
		TextureHandle firstHandle = entities[0].GetComponent<SpriteRendererComponent>().TextureHandle;
		for (std::size_t i = 1; i < entities.size(); ++i) {
			if (entities[i].GetComponent<SpriteRendererComponent>().TextureHandle != firstHandle) {
				textureUniform = false;
				break;
			}
		}

		if (textureUniform && firstHandle.IsValid()) {
			if (Texture2D* texture = TextureManager::GetTexture(firstHandle)) {
				const float texWidth = texture->GetWidth();
				const float texHeight = texture->GetHeight();
				ImGuiUtils::DrawTexturePreview(texture->GetHandle(), texWidth, texHeight);
				ImGui::Text("%.0f x %.0f", texWidth, texHeight);

				ImGui::PushID("TextureSettings");
				ImGuiUtils::DrawEnumCombo<Filter>("Filter", texture->GetFilter(),
					[&texture](Filter newFilter) { texture->SetFilter(newFilter); });
				ImGuiUtils::DrawEnumCombo<Wrap>("Wrap U", texture->GetWrapU(),
					[&texture](Wrap wrapU) { texture->SetWrapU(wrapU); });
				ImGuiUtils::DrawEnumCombo<Wrap>("Wrap V", texture->GetWrapV(),
					[&texture](Wrap wrapV) { texture->SetWrapV(wrapV); });
				ImGui::PopID();
			}
		}
		else if (!textureUniform) {
			ImGui::TextDisabled("Mixed texture - pick to apply to all");
		}
	}

	// ── Camera2D ─────────────────────────────────────────────────────
	// Properties (Zoom, OrthographicSize, ClearColor) flow through the
	// auto-drawer. Read-only viewport size + world viewport are diagnostic
	// extras only meaningful when ONE camera is selected.

	void DrawCamera2DInspector(std::span<const Entity> entities)
	{
		DrawPropertiesFor<Camera2DComponent>(entities);

		if (entities.size() == 1) {
			const auto& camera = entities[0].GetComponent<Camera2DComponent>();
			if (Viewport* vp = camera.GetViewport()) {
				ImGuiUtils::DrawVec2ReadOnly("Viewport Size", vp->GetSize());
			}
			ImGuiUtils::DrawVec2ReadOnly("World Viewport", camera.WorldViewPort());
		}
	}

	// ── FastBody2D (Axiom-Physics) ───────────────────────────────────
	// Properties (Type, Mass, UseGravity, BoundaryCheck) flow through the
	// auto-drawer. Runtime velocity + position are read each frame from the
	// physics body; only meaningful when one entity is selected.

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

	// ── ParticleSystem2D ─────────────────────────────────────────────
	// Hybrid: every field is declared in BuiltInComponentRegistration.cpp
	// (including the Shape variant via Properties::MakeVariantWith and the
	// Gravity-Value EnabledIf gate). Properties flow through the auto-drawer.
	// The two non-property bits are the Play / Pause button (a runtime
	// toggle, not a serialized field) and the texture preview (only sane
	// with one entity selected).

	void DrawParticleSystem2DInspector(std::span<const Entity> entities)
	{
		// Play / Pause toggle.
		bool firstPlaying = false;
		bool playUniform = true;
		if (!entities.empty()) {
			firstPlaying = entities[0].GetComponent<ParticleSystem2DComponent>().IsPlaying();
			for (std::size_t i = 1; i < entities.size(); ++i) {
				if (entities[i].GetComponent<ParticleSystem2DComponent>().IsPlaying() != firstPlaying) {
					playUniform = false;
					break;
				}
			}
		}
		const std::string playbackLabel = (playUniform ? (firstPlaying ? "Pause" : "Play") : "Play / Pause") + std::string("##Value");
		if (ImGuiUtils::DrawInspectorControl("Playback", [&playbackLabel](const char*) {
			return ImGui::Button(playbackLabel.c_str());
		})) {
			for (const Entity& e : entities) {
				auto& ps = const_cast<Entity&>(e).GetComponent<ParticleSystem2DComponent>();
				if (ps.IsPlaying()) ps.Pause();
				else                ps.Play();
			}
		}

		// All declarative fields — auto-drawer renders the standard rows
		// with proper EnabledIf / Variant / Clamp behavior.
		DrawPropertiesFor<ParticleSystem2DComponent>(entities);

		// Texture preview is only meaningful with one entity selected.
		if (entities.size() == 1) {
			const auto& ps = entities[0].GetComponent<ParticleSystem2DComponent>();
			TextureHandle previewHandle = ps.GetTextureHandle();
			if (!previewHandle.IsValid()) {
				previewHandle = TextureManager::GetDefaultTexture(DefaultTexture::Square);
			}
			if (Texture2D* texture = TextureManager::GetTexture(previewHandle)) {
				ImGuiUtils::DrawTexturePreview(texture->GetHandle(), texture->GetWidth(), texture->GetHeight());
			}
		}
	}

	// True when this entity is referenced as the FillEntity of any
	// SliderComponent in its scene. The slider system writes the
	// fill's SizeDelta.x each frame; locking the anchors here keeps
	// the user from authoring a rect setup that the slider's
	// "anchor=(0,0.5), pivot=(0,0.5), grow rightward" math would
	// silently break.
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

	// ── RectTransform2D ──────────────────────────────────────────────
	// Unity-style layout: stacked-header columns for Position + Size, then
	// a collapsible Anchors group with Min / Max, followed by Pivot,
	// Rotation, and Scale. The auto-drawer fallback can't express the
	// per-row sub-labels or the column-header-above-value rows, so this
	// component opts into a fully custom drawer (its property descriptors
	// are dropped — custom drawer wins over properties anyway).
	void DrawRectTransform2DInspector(std::span<const Entity> entities)
	{
		using RTC = RectTransform2DComponent;

		// Lock the slider-driven rect fields when this entity is a
		// SliderComponent's FillEntity. The slider system rewrites
		// SizeDelta.x every frame and assumes the fill is point-anchored
		// at the parent's left edge with pivot at the fill's left edge —
		// editing those fields manually would either be overwritten on
		// the next tick (Width) or would break the fill's geometry
		// (anchors / pivot / left-edge AnchoredPosition).
		const bool fillReadOnly = AnyEntitySliderFill(entities);

		// Position (Pos X / Pos Y) — column-header-above-value layout.
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

		// Size (Width / Height) — same column-header layout. Drag speed of
		// 1.0f matches the integer-pixel feel users expect for size fields.
		// Each axis is disabled when something else owns it:
		//   • ContentSizeFitter with HorizontalFit / VerticalFit — fitter
		//     overwrites SizeDelta from children every frame, so manual
		//     edits would just snap back next tick.
		//   • TextRendererComponent with WrapMode::None — the text's
		//     measured natural size is written into SizeDelta by
		//     UILayoutSystem::FitTextNaturalSize each frame; same snap-
		//     back risk on both axes if the user edited.
		// On a multi-selection any entity in the list claiming an axis
		// disables that axis for the whole row — safer than allowing a
		// partial write some entities would silently revert.
		bool sizeAxisDisabled[2] = { false, false };
		for (const Entity& e : entities) {
			if (e.HasComponent<ContentSizeFitterComponent>()) {
				const auto& csf = e.GetComponent<ContentSizeFitterComponent>();
				if (csf.HorizontalFit) sizeAxisDisabled[0] = true;
				if (csf.VerticalFit)   sizeAxisDisabled[1] = true;
			}
			if (e.HasComponent<TextRendererComponent>()) {
				const auto& text = e.GetComponent<TextRendererComponent>();
				if (text.WrapMode == TextWrapMode::None) {
					sizeAxisDisabled[0] = true;
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

		// Axis sub-labels reused by every "label + Vec2" row below.
		const char* xyLabels[] = { "X", "Y" };

		// Anchors group — collapsible to mirror Unity's foldout. Slow drag
		// speed because anchors live in [0, 1]. When the entity is a
		// slider Fill, the rows render disabled (read-only) since the
		// slider's geometry math depends on a fixed anchor configuration.
		if (ImGui::CollapsingHeader("Anchors", ImGuiTreeNodeFlags_DefaultOpen)) {
			ImGui::PushID("Anchors");
			ImGui::Indent(8.0f);

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

		// Rotation: 2D engines only need the Z component, edited in degrees
		// to match the Transform2D inspector (component stores radians).
		// Edits write LocalRotation — UILayoutSystem composes it against
		// the parent's world rotation each frame and writes the result
		// into the rect's Rotation field.
		ImGuiUtils::DragFloatMulti("Rotation", entities,
			[](const Entity& e) -> float {
				return Degrees(e.GetComponent<RTC>().LocalRotation);
			},
			[](const Entity& e, float v) {
				const_cast<Entity&>(e).GetComponent<RTC>().LocalRotation = Radians(v);
			},
			1.0f);

		// Scale: edits write LocalScale; UILayoutSystem composes it
		// (Hadamard) with the parent's world scale into the rect's Scale.
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

} // namespace Axiom
