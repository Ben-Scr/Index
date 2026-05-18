#include "pch.hpp"
#include "Systems/UIFocusSystem.hpp"

#include "Collections/Viewport.hpp"
#include "Components/General/RectTransform2DComponent.hpp"
#include "Components/Tags.hpp"
#include "Components/UI/InputFieldComponent.hpp"
#include "Components/UI/InteractableComponent.hpp"
#include "Core/Application.hpp"
#include "Core/Input.hpp"
#include "Core/KeyCode.hpp"
#include "Core/MouseButton.hpp"
#include "Core/Window.hpp"
#include "Profiling/Profiler.hpp"
#include "Scene/Scene.hpp"

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

namespace Index {

	namespace {
		// Same conversion UIEventSystem uses so mouse-focus hit-tests
		// agree on coordinate space (centred origin, +Y up).
		Vec2 ScreenPixelToUiSpace(const Vec2& mouseRaw, int viewportWidth, int viewportHeight) {
			return Vec2{
				mouseRaw.x - static_cast<float>(viewportWidth) * 0.5f,
				static_cast<float>(viewportHeight) * 0.5f - mouseRaw.y
			};
		}

		// Stick deflection threshold for a "this counts as a press" edge.
		// Matches the typical XInput "menu" deadzone — looser than gameplay
		// deadzones so menus feel responsive.
		constexpr float k_StickNavThreshold = 0.5f;

		// Read every nav-axis-as-button across all connected gamepads.
		// Returns true if any pad's stick or D-pad pushed past threshold
		// in that direction this frame.
		bool AnyPadPushed(const Input& input, GamepadAxis axis, float sign,
			GamepadButton dpad)
		{
			for (int i = 0; i < Input::k_MaxGamepads; ++i) {
				if (!input.IsGamepadConnected(i)) continue;
				if (input.GetGamepadButton(dpad, i)) return true;
				const float v = input.GetGamepadAxis(axis, i);
				if (sign > 0.f && v >  k_StickNavThreshold) return true;
				if (sign < 0.f && v < -k_StickNavThreshold) return true;
			}
			return false;
		}

		bool AnyPadButtonDown(const Input& input, GamepadButton button) {
			for (int i = 0; i < Input::k_MaxGamepads; ++i) {
				if (!input.IsGamepadConnected(i)) continue;
				if (input.GetGamepadButtonDown(button, i)) return true;
			}
			return false;
		}
	}

	void UIFocusSystem::Update(Scene& scene) {
		Application* app = Application::GetInstance();
		if (!app) return;
		INDEX_PROFILE_SCOPE("UIFocus");
		Input& input = app->GetInput();

		auto& registry = scene.GetRegistry();

		// ── 1. Validate the persisted focus target ──────────────────
		// The entity may have been destroyed, disabled, or had its
		// Focusable flag turned off via the inspector / script since
		// last frame; in any of those cases the navigation system has
		// to abandon it.
		auto IsValidFocusTarget = [&](EntityHandle e) {
			if (e == entt::null) return false;
			if (!registry.valid(e)) return false;
			if (registry.all_of<DisabledTag>(e)) return false;
			auto* interact = registry.try_get<InteractableComponent>(e);
			if (!interact) return false;
			return interact->Focusable && interact->Interactable;
		};
		if (!IsValidFocusTarget(m_FocusedEntity)) {
			m_FocusedEntity = entt::null;
		}

		// ── 1b. Honour programmatic focus writes ────────────────────
		// Script / inspector can set Interactable.IsFocused directly
		// to "give this widget focus" or clear focus. Detect those
		// before we overwrite IsFocused below — a Focusable entity
		// with IsFocused=true that isn't our tracked target adopts
		// focus; the tracked target with IsFocused=false relinquishes
		// it. Both gestures are one-frame: on subsequent ticks the
		// system reconciles to its tracker again.
		{
			EntityHandle externallyFocused = entt::null;
			auto preScanView = registry.view<InteractableComponent>(entt::exclude<DisabledTag>);
			for (auto&& [entity, interact] : preScanView.each()) {
				if (!interact.Focusable || !interact.Interactable) continue;
				if (interact.IsFocused && entity != m_FocusedEntity) {
					externallyFocused = entity;
					break;
				}
			}
			if (externallyFocused != entt::null) {
				m_FocusedEntity = externallyFocused;
			}
			else if (m_FocusedEntity != entt::null) {
				if (auto* interact = registry.try_get<InteractableComponent>(m_FocusedEntity);
					interact && !interact->IsFocused)
				{
					m_FocusedEntity = entt::null;
				}
			}
		}

		// ── 2. Collect focusables in registration order ─────────────
		// EnTT's view iteration is registration order, which matches
		// the scene-author's expectation of "tab through these in the
		// order I made them" without an explicit TabIndex field. Add a
		// TabIndex sort here if/when the engine grows one.
		std::vector<EntityHandle> focusables;
		auto focusView = registry.view<InteractableComponent, RectTransform2DComponent>(entt::exclude<DisabledTag>);
		focusables.reserve(64);
		for (auto&& [entity, interact, rect] : focusView.each()) {
			if (interact.Focusable && interact.Interactable) {
				focusables.push_back(entity);
			}
		}

		// ── 3. Mouse-focus promotion ────────────────────────────────
		// A click on a focusable widget transfers focus there so mouse
		// and keyboard mix naturally. Hit-test mirrors UIEventSystem's
		// approach (panel-relative coords when the editor publishes a
		// UIRegion, else the OS window viewport).
		const bool mouseDownThisFrame = input.GetMouseDown(MouseButton::Left);
		if (mouseDownThisFrame) {
			const Window::UIRegion uiRegion = Window::GetUIRegion();
			Vec2 mouseRaw = input.GetMousePosition();
			int vpW = 0, vpH = 0;
			if (uiRegion.IsActive()) {
				mouseRaw.x -= static_cast<float>(uiRegion.OffsetX);
				mouseRaw.y -= static_cast<float>(uiRegion.OffsetY);
				vpW = uiRegion.Width;
				vpH = uiRegion.Height;
			}
			else if (Viewport* viewport = Window::GetMainViewport()) {
				vpW = viewport->GetWidth();
				vpH = viewport->GetHeight();
			}
			if (vpW > 0 && vpH > 0) {
				const Vec2 mouseUi = ScreenPixelToUiSpace(mouseRaw, vpW, vpH);
				EntityHandle hitFocusable = entt::null;
				for (EntityHandle e : focusables) {
					const auto& rect = registry.get<RectTransform2DComponent>(e);
					if (rect.ContainsPoint(mouseUi)) {
						// Last hit wins — same precedence as UIEventSystem's
						// hit-test (later-iterated entities draw on top, so
						// they should also receive focus first).
						hitFocusable = e;
					}
				}
				if (hitFocusable != entt::null) {
					m_FocusedEntity = hitFocusable;
				}
			}
		}

		// ── 4. Read input actions ───────────────────────────────────
		// "Action" semantics gate on rising-edge so holding a key fires
		// once. Stick edges are tracked across frames so the user can
		// hold the stick, release, and push again to step one widget
		// at a time.
		const bool shiftDown = input.GetKey(KeyCode::LeftShift) || input.GetKey(KeyCode::RightShift);

		// Whether arrow keys are reserved for the focused InputField's
		// caret handler. Tab is never reserved — the user can always
		// tab out of a field.
		bool inputFieldOwnsArrows = false;
		if (m_FocusedEntity != entt::null && registry.valid(m_FocusedEntity)) {
			if (auto* field = registry.try_get<InputFieldComponent>(m_FocusedEntity)) {
				inputFieldOwnsArrows = field->IsFocused;
			}
		}

		auto navKeyDown = [&](KeyCode k) {
			return !inputFieldOwnsArrows && input.GetKeyDown(k);
		};

		// Stick / D-pad rising edges — fire once per push.
		const bool padUpPushed    = AnyPadPushed(input, GamepadAxis::LeftY, -1.f, GamepadButton::DPadUp);
		const bool padDownPushed  = AnyPadPushed(input, GamepadAxis::LeftY,  1.f, GamepadButton::DPadDown);
		const bool padLeftPushed  = AnyPadPushed(input, GamepadAxis::LeftX, -1.f, GamepadButton::DPadLeft);
		const bool padRightPushed = AnyPadPushed(input, GamepadAxis::LeftX,  1.f, GamepadButton::DPadRight);

		const bool padUpEdge    = padUpPushed    && !m_PrevAxisUpPushed;
		const bool padDownEdge  = padDownPushed  && !m_PrevAxisDownPushed;
		const bool padLeftEdge  = padLeftPushed  && !m_PrevAxisLeftPushed;
		const bool padRightEdge = padRightPushed && !m_PrevAxisRightPushed;

		m_PrevAxisUpPushed    = padUpPushed;
		m_PrevAxisDownPushed  = padDownPushed;
		m_PrevAxisLeftPushed  = padLeftPushed;
		m_PrevAxisRightPushed = padRightPushed;

		const bool tabPressed = input.GetKeyDown(KeyCode::Tab);
		const bool wantNext = (tabPressed && !shiftDown)
			|| navKeyDown(KeyCode::Bottom) || navKeyDown(KeyCode::Right)
			|| padDownEdge || padRightEdge;
		const bool wantPrev = (tabPressed && shiftDown)
			|| navKeyDown(KeyCode::Up) || navKeyDown(KeyCode::Left)
			|| padUpEdge || padLeftEdge;

		const bool wantActivate = input.GetKeyDown(KeyCode::Enter)
			|| input.GetKeyDown(KeyCode::KpEnter)
			|| input.GetKeyDown(KeyCode::Space)
			|| AnyPadButtonDown(input, GamepadButton::A);

		const bool wantCancel = input.GetKeyDown(KeyCode::Esc)
			|| AnyPadButtonDown(input, GamepadButton::B);

		// ── 5. Apply navigation actions ─────────────────────────────
		if (!focusables.empty() && (wantNext || wantPrev)) {
			// Find current index. If nothing is focused, "next" lands on
			// the first entry and "prev" lands on the last — same
			// behaviour as every other Tab-cycle UI.
			int currentIdx = -1;
			if (m_FocusedEntity != entt::null) {
				for (size_t i = 0; i < focusables.size(); ++i) {
					if (focusables[i] == m_FocusedEntity) {
						currentIdx = static_cast<int>(i);
						break;
					}
				}
			}
			const int n = static_cast<int>(focusables.size());
			int nextIdx = currentIdx;
			if (wantNext) {
				nextIdx = (currentIdx < 0) ? 0 : (currentIdx + 1) % n;
			}
			else if (wantPrev) {
				nextIdx = (currentIdx < 0) ? (n - 1) : (currentIdx - 1 + n) % n;
			}
			m_FocusedEntity = focusables[nextIdx];
		}

		if (wantCancel) {
			m_FocusedEntity = entt::null;
		}

		// ── 6. Apply Activate ───────────────────────────────────────
		// Stamp ActivatedThisFrame so UIEventSystem's hit-test loop
		// (which runs immediately after this system) synthesises a
		// click on the focused entity. We don't write IsClicked /
		// IsMouseDown directly because UIEventSystem's hit-test would
		// overwrite them — ActivatedThisFrame is the survives-hit-test
		// signal it reads after.
		if (wantActivate && m_FocusedEntity != entt::null) {
			if (auto* interact = registry.try_get<InteractableComponent>(m_FocusedEntity)) {
				interact->ActivatedThisFrame = true;
			}
		}

		// ── 7. Sync InputField.IsFocused with the navigation focus ──
		// When the user tabs INTO an input field, set its IsFocused so
		// the caret appears and typed input flows. When the user tabs
		// AWAY from an input field that was focused, blur it. We don't
		// touch input fields whose owning entity isn't Focusable —
		// those are mouse-driven and live entirely under UIEventSystem.
		auto fieldView = registry.view<InteractableComponent, InputFieldComponent>();
		for (auto&& [entity, interact, field] : fieldView.each()) {
			if (!interact.Focusable) continue;
			const bool shouldBeFocused = (entity == m_FocusedEntity);
			if (shouldBeFocused && !field.IsFocused) {
				field.IsFocused = true;
				// Place the caret at the end on tab-in — common UX, and
				// keeps the existing click-to-place-caret path untouched
				// (mouse focus still uses ByteFromMouseX in UIEventSystem).
				field.CaretBytePos = static_cast<int>(field.Text.size());
				field.SelectionAnchorBytePos = field.CaretBytePos;
			}
			else if (!shouldBeFocused && field.IsFocused) {
				// UIEventSystem's per-frame defocus loop only fires on
				// click-outside, so we have to clear here when the user
				// tabs away. MouseSelecting / hold timers are reset by
				// UIEventSystem itself once IsFocused is false.
				field.IsFocused = false;
			}
		}

		// ── 8. Write the IsFocused flag every frame ─────────────────
		// Always reconcile, even when the user did nothing this frame.
		// This is the cheap way to guarantee no stale "IsFocused = true"
		// can survive on a non-focusable widget if a script (or the
		// inspector) flipped Focusable off mid-session.
		for (auto&& [entity, interact, rect] : focusView.each()) {
			const bool nowFocused = interact.Focusable
				&& interact.Interactable
				&& (entity == m_FocusedEntity);
			interact.IsFocused = nowFocused;
		}
	}

}
