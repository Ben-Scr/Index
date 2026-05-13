#include "pch.hpp"
#include "EntityHelper.hpp"

#include "Components/Graphics/Camera2DComponent.hpp"
#include "Components/Graphics/SpriteRendererComponent.hpp"
#include "Components/Graphics/TextRendererComponent.hpp"
#include "Components/General/RectTransform2DComponent.hpp"
#include "Components/Graphics/ImageComponent.hpp"
#include "Components/General/NameComponent.hpp"
#include "Components/Tags.hpp"
#include "Components/UI/ButtonComponent.hpp"
#include "Components/UI/CircularSliderComponent.hpp"
#include "Components/UI/DropdownComponent.hpp"
#include "Components/UI/InputFieldComponent.hpp"
#include "Components/UI/GridLayoutGroupComponent.hpp"
#include "Components/UI/HorizontalLayoutGroupComponent.hpp"
#include "Components/UI/InteractableComponent.hpp"
#include "Components/UI/ScrollRectComponent.hpp"
#include "Components/UI/ScrollbarComponent.hpp"
#include "Components/UI/SliderComponent.hpp"
#include "Components/UI/ToggleComponent.hpp"
#include "Components/UI/VerticalLayoutGroupComponent.hpp"
#include "Graphics/TextureManager.hpp"

namespace Index {
	void EntityHelper::SetEnabled(Entity entity, bool enabled) {
		entity.SetEnabled(enabled);
	}
	bool EntityHelper::IsEnabled(Entity entity) {
		return !entity.HasComponent<DisabledTag>();
	}

	std::size_t EntityHelper::EntitiesCount() {
		std::size_t count = 0;

		SceneManager::Get().ForeachLoadedScene([&](const Scene& scene) {
			count += scene.GetRegistry().view<EntityHandle>().size();
			});

		return count;
	}

	Entity EntityHelper::CreateCamera2DEntity(Scene& scene) {
		Entity entity = CreateWith<Transform2DComponent, Camera2DComponent>(scene);
		entity.AddComponent<NameComponent>(NameComponent("Camera 2D"));
		return entity;
	}

	Entity EntityHelper::CreateCamera2DEntity() {
		Entity entity = CreateWith<Transform2DComponent, Camera2DComponent>();
		entity.AddComponent<NameComponent>(NameComponent("Camera 2D"));
		return entity;
	}

	Entity EntityHelper::CreateSpriteEntity(Scene& scene) {
		return CreateWith<Transform2DComponent, SpriteRendererComponent>(scene);
	}

	Entity EntityHelper::CreateSpriteEntity() {
		return CreateWith<Transform2DComponent, SpriteRendererComponent >();
	}

	Entity EntityHelper::CreateImageEntity(Scene& scene) {
		Entity e = CreateWith<RectTransform2DComponent, ImageComponent>(scene);
		e.AddComponent<NameComponent>(NameComponent("Image"));
		auto& rect = e.GetComponent<RectTransform2DComponent>();
		rect.SizeDelta = Vec2{ 100.0f, 100.0f };
		return e;
	}

	Entity EntityHelper::CreateImageEntity() {
		Entity e = CreateWith<RectTransform2DComponent, ImageComponent>();
		e.AddComponent<NameComponent>(NameComponent("Image"));
		auto& rect = e.GetComponent<RectTransform2DComponent>();
		rect.SizeDelta = Vec2{ 100.0f, 100.0f };
		return e;
	}

	// ── UI presets ──────────────────────────────────────────────────────
	// Each preset configures a "parent" entity (the widget root) plus
	// optional child entities for visuals that need their own rect /
	// renderer. Sizes are in framebuffer pixels (UIRenderer is screen-
	// space at 1 unit per pixel). UIEventSystem auto-resolves the
	// cross-entity references each frame, so the child structure here
	// is also what survives scene reload.

	Entity EntityHelper::CreateUIPanel(Scene& scene) {
		Entity entity = CreateWith<RectTransform2DComponent, ImageComponent>(scene);
		entity.AddComponent<NameComponent>(NameComponent("Panel"));
		auto& img = entity.GetComponent<ImageComponent>();
		img.Color = Color{ 0.18f, 0.18f, 0.20f, 0.92f };
		auto& rect = entity.GetComponent<RectTransform2DComponent>();
		rect.SizeDelta = Vec2{ 320.0f, 220.0f };
		return entity;
	}

	Entity EntityHelper::CreateUIButton(Scene& scene) {
		Entity entity = CreateWith<RectTransform2DComponent, ImageComponent,
			InteractableComponent, ButtonComponent>(scene);
		entity.AddComponent<NameComponent>(NameComponent("Button"));
		auto& rect = entity.GetComponent<RectTransform2DComponent>();
		rect.SizeDelta = Vec2{ 180.0f, 44.0f };

		// Pleasant default palette (looks good on a dark editor view).
		auto& btn = entity.GetComponent<ButtonComponent>();
		btn.NormalColor   = Color{ 0.30f, 0.55f, 0.95f, 1.0f };
		btn.HoveredColor  = Color{ 0.42f, 0.65f, 1.00f, 1.0f };
		btn.PressedColor  = Color{ 0.20f, 0.40f, 0.80f, 1.0f };
		btn.DisabledColor = Color{ 0.45f, 0.45f, 0.45f, 0.5f };

		// Image starts at NormalColor; UIEventSystem retints from
		// ButtonComponent every frame, so the explicit color is just
		// the value at rest before the first system tick.
		entity.GetComponent<ImageComponent>().Color = btn.NormalColor;

		// Label child — point-anchored at the button's centre with an
		// explicit SizeDelta matching the button. Children no longer
		// inherit parent width/height, so the label needs its own size
		// for left/right text alignment to land at the right place.
		Entity label = CreateWith<RectTransform2DComponent, TextRendererComponent>(scene);
		label.AddComponent<NameComponent>(NameComponent("Label"));
		auto& labelRect = label.GetComponent<RectTransform2DComponent>();
		labelRect.AnchorMin = Vec2{ 0.5f, 0.5f };
		labelRect.AnchorMax = Vec2{ 0.5f, 0.5f };
		labelRect.Pivot = Vec2{ 0.5f, 0.5f };
		labelRect.AnchoredPosition = Vec2{ 0.0f, 0.0f };
		labelRect.SizeDelta = Vec2{ 180.0f, 44.0f };
		auto& labelText = label.GetComponent<TextRendererComponent>();
		labelText.Text = "Button";
		labelText.Color = Color{ 1.0f, 1.0f, 1.0f, 1.0f };
		labelText.HAlign = TextAlignment::Center;
		labelText.FontSize = 18.0f;
		label.SetParent(entity);

		return entity;
	}

	Entity EntityHelper::CreateUISlider(Scene& scene) {
		Entity entity = CreateWith<RectTransform2DComponent, ImageComponent,
			SliderComponent>(scene);
		entity.AddComponent<NameComponent>(NameComponent("Slider"));
		auto& rect = entity.GetComponent<RectTransform2DComponent>();
		rect.SizeDelta = Vec2{ 220.0f, 20.0f };
		entity.GetComponent<ImageComponent>().Color = Color{ 0.18f, 0.20f, 0.24f, 1.0f };

		// Fill child — UIEventSystem updates SizeDelta.x to span [0, t]
		// of the track each frame; the rest of the rect (anchor / pivot /
		// position / height) is authored here and stays editable in the
		// inspector. Point-anchored at the track's left-centre with the
		// fill's pivot also at its left edge so SizeDelta.x grows the
		// fill rightward from the track's left edge. Initial Width=0
		// keeps the fill invisible at Value=0; ApplySliderVisuals fills
		// it on the first tick.
		Entity fill = CreateWith<RectTransform2DComponent, ImageComponent>(scene);
		fill.AddComponent<NameComponent>(NameComponent("Fill"));
		auto& fillRect = fill.GetComponent<RectTransform2DComponent>();
		fillRect.AnchorMin = Vec2{ 0.0f, 0.5f };
		fillRect.AnchorMax = Vec2{ 0.0f, 0.5f };
		fillRect.Pivot = Vec2{ 0.0f, 0.5f };
		fillRect.AnchoredPosition = Vec2{ 0.0f, 0.0f };
		fillRect.SizeDelta = Vec2{ 0.0f, 20.0f };
		fill.GetComponent<ImageComponent>().Color = Color{ 0.30f, 0.55f, 0.95f, 1.0f };
		fill.SetParent(entity);

		// Handle child — the bit the user visually drags. The handle
		// owns the InteractableComponent so the draggable surface is
		// the thumb itself rather than the whole track. UIEventSystem
		// repositions it along the track each frame from SliderComponent::Value.
		Entity handle = CreateWith<RectTransform2DComponent, ImageComponent,
			InteractableComponent>(scene);
		handle.AddComponent<NameComponent>(NameComponent("Handle"));
		auto& handleRect = handle.GetComponent<RectTransform2DComponent>();
		handleRect.SizeDelta = Vec2{ 18.0f, 28.0f };
		handle.GetComponent<ImageComponent>().Color = Color{ 0.95f, 0.95f, 0.95f, 1.0f };
		handle.SetParent(entity);

		auto& slider = entity.GetComponent<SliderComponent>();
		slider.HandleEntity = handle.GetHandle();
		slider.FillEntity = fill.GetHandle();
		return entity;
	}

	Entity EntityHelper::CreateUIProgressBar(Scene& scene) {
		// Same component shape as a slider — a Slider preset with
		// IsReadOnly true, no draggable Handle, and a centred Label
		// child the slider system fills with "{N}%". We deliberately
		// don't add an InteractableComponent here: read-only sliders
		// take hover / press visuals from theirs when present, but a
		// progress bar visually has no interaction at all so leaving
		// it off keeps the engine from trying to hit-test it.
		Entity entity = CreateWith<RectTransform2DComponent, ImageComponent,
			SliderComponent>(scene);
		entity.AddComponent<NameComponent>(NameComponent("Progress Bar"));
		auto& rect = entity.GetComponent<RectTransform2DComponent>();
		rect.SizeDelta = Vec2{ 220.0f, 24.0f };
		entity.GetComponent<ImageComponent>().Color = Color{ 0.18f, 0.20f, 0.24f, 1.0f };

		auto& slider = entity.GetComponent<SliderComponent>();
		slider.IsReadOnly = true;
		slider.Value = 1.0f;
		slider.MinValue = 0.0f;
		slider.MaxValue = 1.0f;

		// Fill child — the visible "blue bar". UIEventSystem's slider
		// pass rewrites its anchors / pivot / SizeDelta every frame
		// from the current Value, exactly the same path the regular
		// slider's fill goes through.
		Entity fill = CreateWith<RectTransform2DComponent, ImageComponent>(scene);
		fill.AddComponent<NameComponent>(NameComponent("Fill"));
		auto& fillRect = fill.GetComponent<RectTransform2DComponent>();
		fillRect.AnchorMin = Vec2{ 0.0f, 0.0f };
		fillRect.AnchorMax = Vec2{ 0.5f, 1.0f };
		fillRect.Pivot = Vec2{ 0.0f, 0.5f };
		fillRect.AnchoredPosition = Vec2{ 0.0f, 0.0f };
		fillRect.SizeDelta = Vec2{ 0.0f, 0.0f };
		fill.GetComponent<ImageComponent>().Color = Color{ 0.30f, 0.55f, 0.95f, 1.0f };
		fill.SetParent(entity);

		// Label child — TextRenderer centred over the bar. SliderComponent
		// updates its Text to "{N}%" each frame; we just author the
		// initial copy so freshly-spawned bars look right before the
		// first event-system tick.
		Entity label = CreateWith<RectTransform2DComponent, TextRendererComponent>(scene);
		label.AddComponent<NameComponent>(NameComponent("Label"));
		auto& labelRect = label.GetComponent<RectTransform2DComponent>();
		labelRect.AnchorMin = Vec2{ 0.5f, 0.5f };
		labelRect.AnchorMax = Vec2{ 0.5f, 0.5f };
		labelRect.Pivot = Vec2{ 0.5f, 0.5f };
		labelRect.AnchoredPosition = Vec2{ 0.0f, 0.0f };
		labelRect.SizeDelta = Vec2{ 220.0f, 24.0f };
		auto& labelText = label.GetComponent<TextRendererComponent>();
		labelText.Text = "100%";
		labelText.Color = Color{ 1.0f, 1.0f, 1.0f, 1.0f };
		labelText.HAlign = TextAlignment::Center;
		labelText.FontSize = 14.0f;
		// Sort the label above the fill so the percent is readable.
		labelText.SortingOrder = 1;
		label.SetParent(entity);

		slider.FillEntity = fill.GetHandle();
		slider.LabelEntity = label.GetHandle();
		return entity;
	}

	Entity EntityHelper::CreateUICircularSlider(Scene& scene) {
		// Ring-only widget — no ImageComponent on the root because the
		// ring is rendered procedurally by GuiRenderer's circular-slider
		// pass. We DO add an InteractableComponent so UIEventSystem's
		// hover loop sees the entity and runs the annulus hit-test on
		// it. The default 200×200 rect with a 20 px ring matches a
		// readable dial size; adjust SizeDelta on the RectTransform or
		// RingThickness on the slider for a chunkier / thinner ring.
		Entity entity = CreateWith<RectTransform2DComponent, InteractableComponent,
			CircularSliderComponent>(scene);
		entity.AddComponent<NameComponent>(NameComponent("Circular Slider"));
		auto& rect = entity.GetComponent<RectTransform2DComponent>();
		rect.SizeDelta = Vec2{ 200.0f, 200.0f };

		auto& cs = entity.GetComponent<CircularSliderComponent>();
		cs.Value = 0.5f;
		cs.RingThickness = 20.0f;

		// Optional handle child — a small dot that rides the ring at
		// the value angle. UIEventSystem's circular-slider pass rewrites
		// its AnchoredPosition every frame, so the user sees a thumb
		// indicator without writing any code. The handle is point-
		// anchored at the slider's centre so its AnchoredPosition is
		// directly the (cosθ·r, sinθ·r) the system writes. Adds an
		// InteractableComponent so hover/press feedback resolves
		// against the cursor sitting on the thumb specifically — gives
		// the same color-swap behaviour as a linear slider's handle.
		// We also use circle.png as the texture so the visual is round,
		// matching the disc-shaped slider body.
		Entity handle = CreateWith<RectTransform2DComponent, ImageComponent, InteractableComponent>(scene);
		handle.AddComponent<NameComponent>(NameComponent("Handle"));
		auto& handleRect = handle.GetComponent<RectTransform2DComponent>();
		handleRect.AnchorMin = Vec2{ 0.5f, 0.5f };
		handleRect.AnchorMax = Vec2{ 0.5f, 0.5f };
		handleRect.Pivot = Vec2{ 0.5f, 0.5f };
		handleRect.SizeDelta = Vec2{ 20.0f, 20.0f };
		auto& handleImg = handle.GetComponent<ImageComponent>();
		handleImg.Color = cs.NormalColor;
		// Round handle by default; uses the bundled circle.png
		// (alpha-masks the corners off a square quad so the visible
		// thumb is a disc). Skipping TextureAssetId — runtime
		// rendering only consults TextureHandle, and the
		// inspector-visible "Texture" picker shows "(None)" until
		// the user picks one explicitly, which is fine.
		handleImg.TextureHandle = TextureManager::GetDefaultTexture(DefaultTexture::Circle);
		handle.SetParent(entity);
		cs.HandleEntity = handle.GetHandle();

		return entity;
	}

	Entity EntityHelper::CreateUIInputField(Scene& scene) {
		Entity entity = CreateWith<RectTransform2DComponent, ImageComponent,
			InteractableComponent, InputFieldComponent>(scene);
		entity.AddComponent<NameComponent>(NameComponent("Input Field"));
		auto& rect = entity.GetComponent<RectTransform2DComponent>();
		rect.SizeDelta = Vec2{ 240.0f, 36.0f };
		entity.GetComponent<ImageComponent>().Color = Color{ 0.95f, 0.95f, 0.95f, 1.0f };

		// Text child — point-anchored at the field's centre with an
		// explicit SizeDelta matching the input field. Left-aligned text
		// uses the rect's left edge as the origin, so the rect needs a
		// real width.
		Entity textChild = CreateWith<RectTransform2DComponent, TextRendererComponent>(scene);
		textChild.AddComponent<NameComponent>(NameComponent("Text"));
		auto& textRect = textChild.GetComponent<RectTransform2DComponent>();
		textRect.AnchorMin = Vec2{ 0.5f, 0.5f };
		textRect.AnchorMax = Vec2{ 0.5f, 0.5f };
		textRect.Pivot = Vec2{ 0.5f, 0.5f };
		textRect.AnchoredPosition = Vec2{ 0.0f, 0.0f };
		textRect.SizeDelta = Vec2{ 240.0f, 36.0f };
		auto& tc = textChild.GetComponent<TextRendererComponent>();
		tc.Text = "Enter text...";
		tc.Color = Color{ 0.55f, 0.55f, 0.55f, 1.0f };
		tc.FontSize = 16.0f;
		tc.HAlign = TextAlignment::Left;
		textChild.SetParent(entity);

		entity.GetComponent<InputFieldComponent>().TextEntity = textChild.GetHandle();
		return entity;
	}

	Entity EntityHelper::CreateUIDropdown(Scene& scene) {
		Entity entity = CreateWith<RectTransform2DComponent, ImageComponent,
			InteractableComponent, DropdownComponent>(scene);
		entity.AddComponent<NameComponent>(NameComponent("Dropdown"));
		auto& rect = entity.GetComponent<RectTransform2DComponent>();
		rect.SizeDelta = Vec2{ 220.0f, 36.0f };
		entity.GetComponent<ImageComponent>().Color = Color{ 0.95f, 0.95f, 0.95f, 1.0f };

		auto& dd = entity.GetComponent<DropdownComponent>();
		dd.Options = { "Option A", "Option B", "Option C" };
		dd.SelectedIndex = 0;

		// Label child — point-anchored at the dropdown's centre with an
		// explicit SizeDelta matching the dropdown. (Children no longer
		// inherit parent dimensions.)
		Entity labelChild = CreateWith<RectTransform2DComponent, TextRendererComponent>(scene);
		labelChild.AddComponent<NameComponent>(NameComponent("Label"));
		auto& labelRect = labelChild.GetComponent<RectTransform2DComponent>();
		labelRect.AnchorMin = Vec2{ 0.5f, 0.5f };
		labelRect.AnchorMax = Vec2{ 0.5f, 0.5f };
		labelRect.Pivot = Vec2{ 0.5f, 0.5f };
		labelRect.AnchoredPosition = Vec2{ 0.0f, 0.0f };
		labelRect.SizeDelta = Vec2{ 220.0f, 36.0f };
		auto& tc = labelChild.GetComponent<TextRendererComponent>();
		tc.Text = dd.Options.empty() ? "" : dd.Options[0];
		tc.Color = Color{ 0.10f, 0.10f, 0.10f, 1.0f };
		tc.FontSize = 16.0f;
		tc.HAlign = TextAlignment::Left;
		labelChild.SetParent(entity);

		dd.LabelEntity = labelChild.GetHandle();
		return entity;
	}

	Entity EntityHelper::CreateUIToggle(Scene& scene) {
		Entity entity = CreateWith<RectTransform2DComponent, ImageComponent,
			InteractableComponent, ToggleComponent>(scene);
		entity.AddComponent<NameComponent>(NameComponent("Toggle"));
		auto& rect = entity.GetComponent<RectTransform2DComponent>();
		rect.SizeDelta = Vec2{ 28.0f, 28.0f };
		entity.GetComponent<ImageComponent>().Color = Color{ 0.95f, 0.95f, 0.95f, 1.0f };

		// Checkmark child — point-anchored to the box's centre so the
		// child's size is its own SizeDelta and never inherits the
		// parent's width/height. UIEventSystem flips it enabled /
		// disabled from ToggleComponent::IsOn.
		Entity check = CreateWith<RectTransform2DComponent, ImageComponent>(scene);
		check.AddComponent<NameComponent>(NameComponent("Checkmark"));
		auto& checkRect = check.GetComponent<RectTransform2DComponent>();
		checkRect.AnchorMin = Vec2{ 0.5f, 0.5f };
		checkRect.AnchorMax = Vec2{ 0.5f, 0.5f };
		checkRect.Pivot = Vec2{ 0.5f, 0.5f };
		checkRect.AnchoredPosition = Vec2{ 0.0f, 0.0f };
		// 4px inset on each side: toggle is 28x28, checkmark 20x20.
		checkRect.SizeDelta = Vec2{ 20.0f, 20.0f };
		check.GetComponent<ImageComponent>().Color = Color{ 0.30f, 0.65f, 0.30f, 1.0f };
		check.AddComponent<DisabledTag>();
		check.SetParent(entity);

		entity.GetComponent<ToggleComponent>().CheckmarkEntity = check.GetHandle();
		return entity;
	}

	Entity EntityHelper::CreateUIScrollbar(Scene& scene) {
		Entity entity = CreateWith<RectTransform2DComponent, ImageComponent,
			InteractableComponent, ScrollbarComponent>(scene);
		entity.AddComponent<NameComponent>(NameComponent("Scrollbar"));
		auto& rect = entity.GetComponent<RectTransform2DComponent>();
		rect.SizeDelta = Vec2{ 220.0f, 20.0f };
		entity.GetComponent<ImageComponent>().Color = Color{ 0.18f, 0.20f, 0.24f, 1.0f };

		// Sliding Area: invisible child that the handle anchors against.
		// In Unity-like setups the handle is parented to a Sliding Area
		// rather than the scrollbar root; we keep the handle directly on
		// the scrollbar to match the slider preset and simplify the
		// resolved-rect math.
		Entity handle = CreateWith<RectTransform2DComponent, ImageComponent,
			InteractableComponent>(scene);
		handle.AddComponent<NameComponent>(NameComponent("Handle"));
		auto& handleRect = handle.GetComponent<RectTransform2DComponent>();
		// Anchored at the track's left-centre (LTR default) — UIEventSystem
		// rewrites AnchoredPosition + SizeDelta every frame from Value /
		// Size, so these are just at-rest defaults.
		handleRect.AnchorMin = Vec2{ 0.0f, 0.5f };
		handleRect.AnchorMax = Vec2{ 0.0f, 0.5f };
		handleRect.Pivot = Vec2{ 0.0f, 0.5f };
		handleRect.AnchoredPosition = Vec2{ 0.0f, 0.0f };
		handleRect.SizeDelta = Vec2{ 44.0f, 20.0f };
		handle.GetComponent<ImageComponent>().Color = Color{ 0.95f, 0.95f, 0.95f, 1.0f };
		handle.SetParent(entity);

		entity.GetComponent<ScrollbarComponent>().HandleEntity = handle.GetHandle();
		return entity;
	}

	Entity EntityHelper::CreateUIScrollRect(Scene& scene) {
		Entity entity = CreateWith<RectTransform2DComponent, ImageComponent,
			InteractableComponent, ScrollRectComponent>(scene);
		entity.AddComponent<NameComponent>(NameComponent("Scroll View"));
		auto& rect = entity.GetComponent<RectTransform2DComponent>();
		rect.SizeDelta = Vec2{ 320.0f, 220.0f };
		entity.GetComponent<ImageComponent>().Color = Color{ 0.18f, 0.18f, 0.20f, 0.92f };

		// Viewport — clips the Content. Sized as the parent's authored
		// rect minus 20px on each axis so the scrollbars sit visually
		// outside. Point-anchored at the parent's centre (children no
		// longer inherit parent dimensions, so SizeDelta is the literal
		// pixel size — author it explicitly).
		Entity viewport = CreateWith<RectTransform2DComponent, ImageComponent>(scene);
		viewport.AddComponent<NameComponent>(NameComponent("Viewport"));
		auto& vRect = viewport.GetComponent<RectTransform2DComponent>();
		vRect.AnchorMin = Vec2{ 0.5f, 0.5f };
		vRect.AnchorMax = Vec2{ 0.5f, 0.5f };
		vRect.Pivot = Vec2{ 0.5f, 0.5f };
		vRect.AnchoredPosition = Vec2{ 0.0f, 0.0f };
		vRect.SizeDelta = Vec2{ 300.0f, 200.0f };
		viewport.GetComponent<ImageComponent>().Color = Color{ 0.10f, 0.10f, 0.12f, 1.0f };
		viewport.SetParent(entity);

		// Content — the scrollable child. Sized larger than the viewport
		// on both axes so there's something to scroll out of the box.
		// Anchored at the viewport's top-left corner so AnchoredPosition
		// in [-(content - viewport), 0] is the standard scroll range.
		Entity content = CreateWith<RectTransform2DComponent>(scene);
		content.AddComponent<NameComponent>(NameComponent("Content"));
		auto& cRect = content.GetComponent<RectTransform2DComponent>();
		cRect.AnchorMin = Vec2{ 0.0f, 1.0f };
		cRect.AnchorMax = Vec2{ 0.0f, 1.0f };
		cRect.Pivot = Vec2{ 0.0f, 1.0f };
		cRect.AnchoredPosition = Vec2{ 0.0f, 0.0f };
		cRect.SizeDelta = Vec2{ 600.0f, 400.0f };
		content.SetParent(viewport);

		// Horizontal scrollbar — sits along the bottom edge of the
		// scroll view. Point-anchored at parent's bottom-centre.
		Entity hbar = CreateUIScrollbar(scene);
		hbar.GetComponent<NameComponent>().Name = "Scrollbar Horizontal";
		auto& hbarRect = hbar.GetComponent<RectTransform2DComponent>();
		hbarRect.AnchorMin = Vec2{ 0.5f, 0.0f };
		hbarRect.AnchorMax = Vec2{ 0.5f, 0.0f };
		hbarRect.Pivot = Vec2{ 0.5f, 0.0f };
		hbarRect.AnchoredPosition = Vec2{ 0.0f, 0.0f };
		hbarRect.SizeDelta = Vec2{ 300.0f, 16.0f };
		hbar.GetComponent<ScrollbarComponent>().Direction = ScrollbarDirection::LeftToRight;
		hbar.SetParent(entity);

		// Vertical scrollbar — sits along the right edge of the scroll
		// view. Point-anchored at parent's right-centre.
		Entity vbar = CreateUIScrollbar(scene);
		vbar.GetComponent<NameComponent>().Name = "Scrollbar Vertical";
		auto& vbarRect = vbar.GetComponent<RectTransform2DComponent>();
		vbarRect.AnchorMin = Vec2{ 1.0f, 0.5f };
		vbarRect.AnchorMax = Vec2{ 1.0f, 0.5f };
		vbarRect.Pivot = Vec2{ 1.0f, 0.5f };
		vbarRect.AnchoredPosition = Vec2{ 0.0f, 0.0f };
		vbarRect.SizeDelta = Vec2{ 16.0f, 200.0f };
		vbar.GetComponent<ScrollbarComponent>().Direction = ScrollbarDirection::TopToBottom;
		vbar.SetParent(entity);

		auto& sr = entity.GetComponent<ScrollRectComponent>();
		sr.Viewport = viewport.GetHandle();
		sr.Content = content.GetHandle();
		sr.HorizontalScrollbar = hbar.GetHandle();
		sr.VerticalScrollbar = vbar.GetHandle();
		return entity;
	}

	Entity EntityHelper::CreateUIHorizontalLayout(Scene& scene) {
		Entity entity = CreateWith<RectTransform2DComponent, HorizontalLayoutGroupComponent>(scene);
		entity.AddComponent<NameComponent>(NameComponent("Horizontal Layout"));
		auto& rect = entity.GetComponent<RectTransform2DComponent>();
		rect.SizeDelta = Vec2{ 320.0f, 64.0f };
		auto& lg = entity.GetComponent<HorizontalLayoutGroupComponent>();
		lg.Spacing = 8.0f;
		lg.PaddingLeft = lg.PaddingRight = lg.PaddingTop = lg.PaddingBottom = 8.0f;
		return entity;
	}

	Entity EntityHelper::CreateUIVerticalLayout(Scene& scene) {
		Entity entity = CreateWith<RectTransform2DComponent, VerticalLayoutGroupComponent>(scene);
		entity.AddComponent<NameComponent>(NameComponent("Vertical Layout"));
		auto& rect = entity.GetComponent<RectTransform2DComponent>();
		rect.SizeDelta = Vec2{ 200.0f, 320.0f };
		auto& lg = entity.GetComponent<VerticalLayoutGroupComponent>();
		lg.Spacing = 8.0f;
		lg.PaddingLeft = lg.PaddingRight = lg.PaddingTop = lg.PaddingBottom = 8.0f;
		return entity;
	}

	Entity EntityHelper::CreateUIGridLayout(Scene& scene) {
		Entity entity = CreateWith<RectTransform2DComponent, GridLayoutGroupComponent>(scene);
		entity.AddComponent<NameComponent>(NameComponent("Grid Layout"));
		auto& rect = entity.GetComponent<RectTransform2DComponent>();
		rect.SizeDelta = Vec2{ 320.0f, 320.0f };
		auto& lg = entity.GetComponent<GridLayoutGroupComponent>();
		lg.CellSize = Vec2{ 100.0f, 100.0f };
		lg.Spacing = Vec2{ 8.0f, 8.0f };
		lg.PaddingLeft = lg.PaddingRight = lg.PaddingTop = lg.PaddingBottom = 8.0f;
		return entity;
	}
}
