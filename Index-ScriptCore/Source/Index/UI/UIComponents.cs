using Index;
using Index.Interop;

namespace Index.UI;

// ── RectTransform ───────────────────────────────────────────────────
//
// Screen-space rectangle on a UI entity. Mirrors the C++
// RectTransform2DComponent — the engine's UILayoutSystem walks the
// hierarchy each frame and writes resolved corners back into the
// component, so reads of `Size` / `Position` reflect anchor + parent
// composition, not just the authored fields.

public class RectTransform : Component
{
    public Vector2 AnchorMin
    {
        get
        {
            ulong id = RequireComponent<RectTransform>();
            InternalCalls.RectTransform_GetAnchorMin(id, out float x, out float y);
            return new Vector2(x, y);
        }
        set => InternalCalls.RectTransform_SetAnchorMin(RequireComponent<RectTransform>(), value.X, value.Y);
    }

    public Vector2 AnchorMax
    {
        get
        {
            ulong id = RequireComponent<RectTransform>();
            InternalCalls.RectTransform_GetAnchorMax(id, out float x, out float y);
            return new Vector2(x, y);
        }
        set => InternalCalls.RectTransform_SetAnchorMax(RequireComponent<RectTransform>(), value.X, value.Y);
    }

    public Vector2 Pivot
    {
        get
        {
            ulong id = RequireComponent<RectTransform>();
            InternalCalls.RectTransform_GetPivot(id, out float x, out float y);
            return new Vector2(x, y);
        }
        set => InternalCalls.RectTransform_SetPivot(RequireComponent<RectTransform>(), value.X, value.Y);
    }

    public Vector2 AnchoredPosition
    {
        get
        {
            ulong id = RequireComponent<RectTransform>();
            InternalCalls.RectTransform_GetAnchoredPosition(id, out float x, out float y);
            return new Vector2(x, y);
        }
        set => InternalCalls.RectTransform_SetAnchoredPosition(RequireComponent<RectTransform>(), value.X, value.Y);
    }

    public Vector2 SizeDelta
    {
        get
        {
            ulong id = RequireComponent<RectTransform>();
            InternalCalls.RectTransform_GetSizeDelta(id, out float x, out float y);
            return new Vector2(x, y);
        }
        set => InternalCalls.RectTransform_SetSizeDelta(RequireComponent<RectTransform>(), value.X, value.Y);
    }

    // World rotation/scale — composed from this rect's Local* values
    // and every ancestor's. The getter returns the resolved world value
    // written by UILayoutSystem each frame; the setter writes the
    // authored Local* value (writing to the world cache directly would
    // be overwritten on the next layout pass). Mirrors Transform2D.
    public float Rotation
    {
        get => InternalCalls.RectTransform_GetRotation(RequireComponent<RectTransform>()) * Mathf.Rad2Deg;
        set => InternalCalls.RectTransform_SetRotation(RequireComponent<RectTransform>(), value * Mathf.Deg2Rad);
    }

    public Vector2 Scale
    {
        get
        {
            ulong id = RequireComponent<RectTransform>();
            InternalCalls.RectTransform_GetScale(id, out float x, out float y);
            return new Vector2(x, y);
        }
        set => InternalCalls.RectTransform_SetScale(RequireComponent<RectTransform>(), value.X, value.Y);
    }

    // Local rotation/scale — the authored values stored on this rect.
    // For root rects these match Rotation/Scale; for parented rects
    // they're the offset from the parent's world rotation/scale.
    public float LocalRotation
    {
        get => InternalCalls.RectTransform_GetLocalRotation(RequireComponent<RectTransform>()) * Mathf.Rad2Deg;
        set => InternalCalls.RectTransform_SetLocalRotation(RequireComponent<RectTransform>(), value * Mathf.Deg2Rad);
    }

    public Vector2 LocalScale
    {
        get
        {
            ulong id = RequireComponent<RectTransform>();
            InternalCalls.RectTransform_GetLocalScale(id, out float x, out float y);
            return new Vector2(x, y);
        }
        set => InternalCalls.RectTransform_SetLocalScale(RequireComponent<RectTransform>(), value.X, value.Y);
    }

    // Resolved screen-space size as computed by UILayoutSystem this
    // frame. Combines anchor stretch + SizeDelta + parent rect; useful
    // for code that wants the actual on-screen size (e.g. positioning
    // a tooltip or scaling a child element). Read-only — set the
    // underlying values via SizeDelta / AnchorMin / AnchorMax.
    public Vector2 Size
    {
        get
        {
            ulong id = RequireComponent<RectTransform>();
            InternalCalls.RectTransform_GetResolvedSize(id, out float w, out float h);
            return new Vector2(w, h);
        }
    }
}

// ── Image ───────────────────────────────────────────────────────────

public class Image : Component
{
    public Vector4 Color
    {
        get
        {
            ulong id = RequireComponent<Image>();
            InternalCalls.Image_GetColor(id, out float r, out float g, out float b, out float a);
            return new Color(r, g, b, a);
        }
        set => InternalCalls.Image_SetColor(RequireComponent<Image>(), value.X, value.Y, value.Z, value.W);
    }

    public Texture? Texture
    {
        get
        {
            ulong assetId = InternalCalls.Image_GetTexture(RequireComponent<Image>());
            return Index.Texture.FromAssetUUID(assetId);
        }
        set => InternalCalls.Image_SetTexture(RequireComponent<Image>(), value?.UUID ?? 0);
    }

    public int SortingOrder
    {
        get => InternalCalls.Image_GetSortingOrder(RequireComponent<Image>());
        set => InternalCalls.Image_SetSortingOrder(RequireComponent<Image>(), value);
    }

    public int SortingLayer
    {
        get => InternalCalls.Image_GetSortingLayer(RequireComponent<Image>());
        set => InternalCalls.Image_SetSortingLayer(RequireComponent<Image>(), value);
    }
}

// ── Interactable ────────────────────────────────────────────────────
//
// Per-frame UI input state. Reads of IsHovered/IsClicked/etc are
// updated by UIEventSystem before any user code runs, so a script's
// OnUpdate() can react to clicks the same frame they happen.
//
// Events fire per-instance from UIEventDispatcher after UIEventSystem
// updates each frame. Subscribe on the specific Interactable whose
// transitions you care about — the handler receives that Interactable
// so you can check IsFocused / IsHovered / Entity directly:
//   interactable.OnClicked += i => Debug.Log($"{i.Entity.Name} clicked");

public class Interactable : Component
{
    public event Action<Interactable>? OnHovered;
    public event Action<Interactable>? OnClicked;
    public event Action<Interactable>? OnPressed;
    public event Action<Interactable>? OnMouseDown;
    public event Action<Interactable>? OnMouseUp;

    // Keyboard / controller focus transitions. OnFocusEnter fires on
    // the rising edge of IsFocused, OnFocusExit on the falling edge.
    // Both pass the Interactable so the handler can grab .Entity or
    // any sibling component without a closure-captured reference.
    public event Action<Interactable>? OnFocusEnter;
    public event Action<Interactable>? OnFocusExit;

    public bool IsInteractable
    {
        get => InternalCalls.Interactable_GetInteractable(RequireComponent<Interactable>());
        set => InternalCalls.Interactable_SetInteractable(RequireComponent<Interactable>(), value);
    }

    // Opt-in to keyboard / controller focus navigation. Defaults to
    // false (preserving simple mouse-only UI) — flip this to true on
    // any widget you want UIFocusSystem to include in Tab / D-pad /
    // arrow-key cycling. Existing scenes don't gain any focus-ring
    // visual unless you also set FocusedColor on the widget itself.
    public bool Focusable
    {
        get => InternalCalls.Interactable_GetFocusable(RequireComponent<Interactable>());
        set => InternalCalls.Interactable_SetFocusable(RequireComponent<Interactable>(), value);
    }

    // Read-only: these are driven entirely by UIEventSystem's hit-test
    // and press-tracking. Setting them from script wouldn't be honored
    // (they get overwritten next frame).
    public bool IsHovered => InternalCalls.Interactable_GetIsHovered(RequireComponent<Interactable>());
    public bool IsClicked => InternalCalls.Interactable_GetIsClicked(RequireComponent<Interactable>());
    public bool IsPressed => InternalCalls.Interactable_GetIsPressed(RequireComponent<Interactable>());
    public bool IsMouseDown => InternalCalls.Interactable_GetIsMouseDown(RequireComponent<Interactable>());
    public bool IsMouseUp => InternalCalls.Interactable_GetIsMouseUp(RequireComponent<Interactable>());

    // The currently-focused state, owned by UIFocusSystem. Setting
    // this from script is honoured for one frame as a programmatic
    // "give this widget focus" gesture; the system reconciles next
    // tick (and clears it if Focusable is false).
    public bool IsFocused
    {
        get => InternalCalls.Interactable_GetIsFocused(RequireComponent<Interactable>());
        set => InternalCalls.Interactable_SetIsFocused(RequireComponent<Interactable>(), value);
    }

    // Instance-side raisers used by UIEventDispatcher. Kept internal
    // (the events are public-subscribe only) so external code can't
    // synthesize fake transitions — only the engine pipeline drives
    // them. Each passes `this` to the handler so subscribers can read
    // any sibling state without a captured reference.
    internal void RaiseHovered() => OnHovered?.Invoke(this);
    internal void RaiseClicked() => OnClicked?.Invoke(this);
    internal void RaisePressed() => OnPressed?.Invoke(this);
    internal void RaiseMouseDown() => OnMouseDown?.Invoke(this);
    internal void RaiseMouseUp() => OnMouseUp?.Invoke(this);
    internal void RaiseFocusEnter() => OnFocusEnter?.Invoke(this);
    internal void RaiseFocusExit() => OnFocusExit?.Invoke(this);
}

// ── Button ──────────────────────────────────────────────────────────
//
// State-tint preset that the engine applies to the Image on the same
// entity each frame. Click semantics live on the sibling Interactable;
// Button.OnClicked is a per-instance convenience event that fires once
// on the frame the sibling Interactable reports a click — same edge
// semantics as Interactable.OnClicked, no per-frame retriggering while
// the mouse is held. Passes the Button so subscribers can read any
// sibling component (Image, RectTransform, etc.) directly:
//   button.OnClicked += b => b.Entity.GetComponent<Image>().Color = Color.Red;

public class Button : Component
{
    public event Action<Button>? OnClicked;

    // Optional explicit visual target. When set, the button retints /
    // sprite-swaps the ImageComponent (or, lacking one, the
    // TextRendererComponent) on this entity. Leaving it null falls
    // back to the button's own entity, picking Image first then
    // TextRenderer — so a button with a single Text child works
    // with no preset wiring.
    public Entity? TargetGraphic
    {
        get
        {
            ulong id = InternalCalls.Button_GetTargetGraphic(RequireComponent<Button>());
            return id == 0 ? null : new Entity(id);
        }
        set => InternalCalls.Button_SetTargetGraphic(RequireComponent<Button>(), value?.ID ?? 0);
    }

    public Vector4 NormalColor
    {
        get
        {
            ulong id = RequireComponent<Button>();
            InternalCalls.Button_GetNormalColor(id, out float r, out float g, out float b, out float a);
            return new Color(r, g, b, a);
        }
        set => InternalCalls.Button_SetNormalColor(RequireComponent<Button>(), value.X, value.Y, value.Z, value.W);
    }

    public Vector4 HoveredColor
    {
        get
        {
            ulong id = RequireComponent<Button>();
            InternalCalls.Button_GetHoveredColor(id, out float r, out float g, out float b, out float a);
            return new Color(r, g, b, a);
        }
        set => InternalCalls.Button_SetHoveredColor(RequireComponent<Button>(), value.X, value.Y, value.Z, value.W);
    }

    public Vector4 PressedColor
    {
        get
        {
            ulong id = RequireComponent<Button>();
            InternalCalls.Button_GetPressedColor(id, out float r, out float g, out float b, out float a);
            return new Color(r, g, b, a);
        }
        set => InternalCalls.Button_SetPressedColor(RequireComponent<Button>(), value.X, value.Y, value.Z, value.W);
    }

    public Vector4 DisabledColor
    {
        get
        {
            ulong id = RequireComponent<Button>();
            InternalCalls.Button_GetDisabledColor(id, out float r, out float g, out float b, out float a);
            return new Color(r, g, b, a);
        }
        set => InternalCalls.Button_SetDisabledColor(RequireComponent<Button>(), value.X, value.Y, value.Z, value.W);
    }

    // Tint applied when the sibling Interactable is focused via
    // UIFocusSystem. Alpha == 0 (the default) is treated as a
    // sentinel — the widget falls through to Hovered / Normal as if
    // focus didn't exist, so users who want navigation without any
    // visible indicator simply leave this color zeroed.
    public Vector4 FocusedColor
    {
        get
        {
            ulong id = RequireComponent<Button>();
            InternalCalls.Button_GetFocusedColor(id, out float r, out float g, out float b, out float a);
            return new Color(r, g, b, a);
        }
        set => InternalCalls.Button_SetFocusedColor(RequireComponent<Button>(), value.X, value.Y, value.Z, value.W);
    }

    // Picks between color-tint, sprite-swap, and "no automatic
    // transition" for this widget. Default ColorTint preserves the
    // original mouse-only behaviour. See UITransitionMode for details.
    public UITransitionMode TransitionMode
    {
        get => (UITransitionMode)InternalCalls.Button_GetTransitionMode(RequireComponent<Button>());
        set => InternalCalls.Button_SetTransitionMode(RequireComponent<Button>(), (int)value);
    }

    // Per-state texture overrides used when TransitionMode is
    // SpriteSwap. null leaves the slot unset; unset slots fall back
    // to NormalSprite. NormalSprite being null disables the swap.
    public Texture? NormalSprite
    {
        get => Index.Texture.FromAssetUUID(InternalCalls.Button_GetNormalSprite(RequireComponent<Button>()));
        set => InternalCalls.Button_SetNormalSprite(RequireComponent<Button>(), value?.UUID ?? 0);
    }
    public Texture? HoveredSprite
    {
        get => Index.Texture.FromAssetUUID(InternalCalls.Button_GetHoveredSprite(RequireComponent<Button>()));
        set => InternalCalls.Button_SetHoveredSprite(RequireComponent<Button>(), value?.UUID ?? 0);
    }
    public Texture? PressedSprite
    {
        get => Index.Texture.FromAssetUUID(InternalCalls.Button_GetPressedSprite(RequireComponent<Button>()));
        set => InternalCalls.Button_SetPressedSprite(RequireComponent<Button>(), value?.UUID ?? 0);
    }
    public Texture? DisabledSprite
    {
        get => Index.Texture.FromAssetUUID(InternalCalls.Button_GetDisabledSprite(RequireComponent<Button>()));
        set => InternalCalls.Button_SetDisabledSprite(RequireComponent<Button>(), value?.UUID ?? 0);
    }
    public Texture? FocusedSprite
    {
        get => Index.Texture.FromAssetUUID(InternalCalls.Button_GetFocusedSprite(RequireComponent<Button>()));
        set => InternalCalls.Button_SetFocusedSprite(RequireComponent<Button>(), value?.UUID ?? 0);
    }

    // Convenience: returns true on the frame the user clicked the
    // button. Reads the sibling Interactable component's IsClicked
    // flag, so the host entity must have one (the Button preset adds
    // it automatically).
    public bool WasClicked
    {
        get
        {
            Interactable? interactable = Entity.GetComponent<Interactable>();
            return interactable?.IsClicked ?? false;
        }
    }

    internal void RaiseClick() => OnClicked?.Invoke(this);
}

// ── Slider ──────────────────────────────────────────────────────────

public class Slider : Component
{
    // Fires when the engine commits a value change this frame (drag /
    // keyboard / SetValue with notify), passing the new value. Subscribe
    // per-instance:
    //   slider.OnValueChanged += v => Debug.Log($"Volume = {v}");
    public event Action<float>? OnValueChanged;


    public float Value
    {
        get => InternalCalls.Slider_GetValue(RequireComponent<Slider>());
        set => InternalCalls.Slider_SetValue(RequireComponent<Slider>(), value);
    }

    public float MinValue
    {
        get => InternalCalls.Slider_GetMinValue(RequireComponent<Slider>());
        set => InternalCalls.Slider_SetMinValue(RequireComponent<Slider>(), value);
    }

    public float MaxValue
    {
        get => InternalCalls.Slider_GetMaxValue(RequireComponent<Slider>());
        set => InternalCalls.Slider_SetMaxValue(RequireComponent<Slider>(), value);
    }

    public bool WholeNumbers
    {
        get => InternalCalls.Slider_GetWholeNumbers(RequireComponent<Slider>());
        set => InternalCalls.Slider_SetWholeNumbers(RequireComponent<Slider>(), value);
    }

    // When true the slider's Value can't be changed by user drag /
    // keyboard / controller input. Programmatic Value writes still
    // work; visual state tracks normally.
    public bool IsReadOnly
    {
        get => InternalCalls.Slider_GetIsReadOnly(RequireComponent<Slider>());
        set => InternalCalls.Slider_SetIsReadOnly(RequireComponent<Slider>(), value);
    }

    // Static track background entity. The slider system never mutates
    // it, but exposing the ref lets script tweak / theme it without
    // re-finding the child by name. UIEventSystem auto-resolves a
    // child named "Background" into this slot when it's null.
    public Entity? BackgroundEntity
    {
        get
        {
            ulong id = InternalCalls.Slider_GetBackgroundEntity(RequireComponent<Slider>());
            return id == 0 ? null : new Entity(id);
        }
        set => InternalCalls.Slider_SetBackgroundEntity(RequireComponent<Slider>(), value?.ID ?? 0);
    }

    // Optional fill rect: child Image whose width grows with Value.
    public Entity? FillEntity
    {
        get
        {
            ulong id = InternalCalls.Slider_GetFillEntity(RequireComponent<Slider>());
            return id == 0 ? null : new Entity(id);
        }
        set => InternalCalls.Slider_SetFillEntity(RequireComponent<Slider>(), value?.ID ?? 0);
    }

    // Optional handle (thumb) entity. Drag interactable usually lives
    // on the handle in the preset; the slider parent is the fallback.
    public Entity? HandleEntity
    {
        get
        {
            ulong id = InternalCalls.Slider_GetHandleEntity(RequireComponent<Slider>());
            return id == 0 ? null : new Entity(id);
        }
        set => InternalCalls.Slider_SetHandleEntity(RequireComponent<Slider>(), value?.ID ?? 0);
    }

    // Programmatic value set with optional event suppression. Two
    // jobs in one call:
    //  - notifyEvent=true: fire OnValueChanged synchronously with the
    //    new value, the same way a same-frame consumer would expect.
    //  - notifyEvent=false: stay silent for this assignment.
    // Both branches call Slider_MarkValueObserved so UIEventSystem's
    // diff (which now catches inspector / property-setter changes too)
    // doesn't double-fire on the next tick.
    public void SetValue(float value, bool notifyEvent = true)
    {
        ulong id = RequireComponent<Slider>();
        float previous = InternalCalls.Slider_GetValue(id);
        InternalCalls.Slider_SetValue(id, value);
        InternalCalls.Slider_MarkValueObserved(id);
        if (notifyEvent)
        {
            float applied = InternalCalls.Slider_GetValue(id);
            if (applied != previous)
                OnValueChanged?.Invoke(applied);
        }
    }

    // Normalized [0, 1] value — convenient for piping into colors,
    // alphas, audio volumes, etc. without re-deriving from MinValue/MaxValue.
    public float NormalizedValue
    {
        get
        {
            float min = MinValue;
            float max = MaxValue;
            float range = max - min;
            return range != 0.0f ? (Value - min) / range : 0.0f;
        }
    }

    public Vector4 NormalColor
    {
        get
        {
            ulong id = RequireComponent<Slider>();
            InternalCalls.Slider_GetNormalColor(id, out float r, out float g, out float b, out float a);
            return new Color(r, g, b, a);
        }
        set => InternalCalls.Slider_SetNormalColor(RequireComponent<Slider>(), value.X, value.Y, value.Z, value.W);
    }

    public Vector4 HoveredColor
    {
        get
        {
            ulong id = RequireComponent<Slider>();
            InternalCalls.Slider_GetHoveredColor(id, out float r, out float g, out float b, out float a);
            return new Color(r, g, b, a);
        }
        set => InternalCalls.Slider_SetHoveredColor(RequireComponent<Slider>(), value.X, value.Y, value.Z, value.W);
    }

    public Vector4 PressedColor
    {
        get
        {
            ulong id = RequireComponent<Slider>();
            InternalCalls.Slider_GetPressedColor(id, out float r, out float g, out float b, out float a);
            return new Color(r, g, b, a);
        }
        set => InternalCalls.Slider_SetPressedColor(RequireComponent<Slider>(), value.X, value.Y, value.Z, value.W);
    }

    public Vector4 DisabledColor
    {
        get
        {
            ulong id = RequireComponent<Slider>();
            InternalCalls.Slider_GetDisabledColor(id, out float r, out float g, out float b, out float a);
            return new Color(r, g, b, a);
        }
        set => InternalCalls.Slider_SetDisabledColor(RequireComponent<Slider>(), value.X, value.Y, value.Z, value.W);
    }

    // Alpha == 0 sentinel = no focus tint; see Button.FocusedColor.
    public Vector4 FocusedColor
    {
        get
        {
            ulong id = RequireComponent<Slider>();
            InternalCalls.Slider_GetFocusedColor(id, out float r, out float g, out float b, out float a);
            return new Color(r, g, b, a);
        }
        set => InternalCalls.Slider_SetFocusedColor(RequireComponent<Slider>(), value.X, value.Y, value.Z, value.W);
    }

    public UITransitionMode TransitionMode
    {
        get => (UITransitionMode)InternalCalls.Slider_GetTransitionMode(RequireComponent<Slider>());
        set => InternalCalls.Slider_SetTransitionMode(RequireComponent<Slider>(), (int)value);
    }
    public Texture? NormalSprite
    {
        get => Index.Texture.FromAssetUUID(InternalCalls.Slider_GetNormalSprite(RequireComponent<Slider>()));
        set => InternalCalls.Slider_SetNormalSprite(RequireComponent<Slider>(), value?.UUID ?? 0);
    }
    public Texture? HoveredSprite
    {
        get => Index.Texture.FromAssetUUID(InternalCalls.Slider_GetHoveredSprite(RequireComponent<Slider>()));
        set => InternalCalls.Slider_SetHoveredSprite(RequireComponent<Slider>(), value?.UUID ?? 0);
    }
    public Texture? PressedSprite
    {
        get => Index.Texture.FromAssetUUID(InternalCalls.Slider_GetPressedSprite(RequireComponent<Slider>()));
        set => InternalCalls.Slider_SetPressedSprite(RequireComponent<Slider>(), value?.UUID ?? 0);
    }
    public Texture? DisabledSprite
    {
        get => Index.Texture.FromAssetUUID(InternalCalls.Slider_GetDisabledSprite(RequireComponent<Slider>()));
        set => InternalCalls.Slider_SetDisabledSprite(RequireComponent<Slider>(), value?.UUID ?? 0);
    }
    public Texture? FocusedSprite
    {
        get => Index.Texture.FromAssetUUID(InternalCalls.Slider_GetFocusedSprite(RequireComponent<Slider>()));
        set => InternalCalls.Slider_SetFocusedSprite(RequireComponent<Slider>(), value?.UUID ?? 0);
    }

    internal void RaiseValueChanged() => OnValueChanged?.Invoke(Value);
}

// ── Toggle ──────────────────────────────────────────────────────────

public class Toggle : Component
{
    // Fires when IsOn flips this frame, passing the new state.
    // Subscribe per-instance:
    //   toggle.OnValueChanged += isOn => audio.Mute = isOn;
    public event Action<bool>? OnValueChanged;

    public bool IsOn
    {
        get => InternalCalls.Toggle_GetIsOn(RequireComponent<Toggle>());
        set => InternalCalls.Toggle_SetIsOn(RequireComponent<Toggle>(), value);
    }

    // When true the toggle ignores user clicks (mouse + keyboard
    // activate). Programmatic IsOn writes still work; visual state
    // tracks normally so the widget feels alive.
    public bool IsReadOnly
    {
        get => InternalCalls.Toggle_GetIsReadOnly(RequireComponent<Toggle>());
        set => InternalCalls.Toggle_SetIsReadOnly(RequireComponent<Toggle>(), value);
    }

    // The entity whose ImageComponent is shown / hidden as IsOn flips.
    // null clears the link. Writes resolve via the persistent UUID so
    // refs survive scene reload.
    public Entity? CheckmarkEntity
    {
        get
        {
            ulong id = InternalCalls.Toggle_GetCheckmarkEntity(RequireComponent<Toggle>());
            return id == 0 ? null : new Entity(id);
        }
        set => InternalCalls.Toggle_SetCheckmarkEntity(RequireComponent<Toggle>(), value?.ID ?? 0);
    }

    // Programmatic toggle with optional event suppression. See Slider.SetValue.
    public void SetValue(bool value, bool notifyEvent = true)
    {
        ulong id = RequireComponent<Toggle>();
        bool previous = InternalCalls.Toggle_GetIsOn(id);
        InternalCalls.Toggle_SetIsOn(id, value);
        InternalCalls.Toggle_MarkIsOnObserved(id);
        if (notifyEvent && previous != value)
            OnValueChanged?.Invoke(value);
    }

    public bool ValueChangedThisFrame => InternalCalls.Toggle_GetValueChangedThisFrame(RequireComponent<Toggle>());

    public Vector4 NormalColor
    {
        get
        {
            ulong id = RequireComponent<Toggle>();
            InternalCalls.Toggle_GetNormalColor(id, out float r, out float g, out float b, out float a);
            return new Color(r, g, b, a);
        }
        set => InternalCalls.Toggle_SetNormalColor(RequireComponent<Toggle>(), value.X, value.Y, value.Z, value.W);
    }

    public Vector4 HoveredColor
    {
        get
        {
            ulong id = RequireComponent<Toggle>();
            InternalCalls.Toggle_GetHoveredColor(id, out float r, out float g, out float b, out float a);
            return new Color(r, g, b, a);
        }
        set => InternalCalls.Toggle_SetHoveredColor(RequireComponent<Toggle>(), value.X, value.Y, value.Z, value.W);
    }

    public Vector4 PressedColor
    {
        get
        {
            ulong id = RequireComponent<Toggle>();
            InternalCalls.Toggle_GetPressedColor(id, out float r, out float g, out float b, out float a);
            return new Color(r, g, b, a);
        }
        set => InternalCalls.Toggle_SetPressedColor(RequireComponent<Toggle>(), value.X, value.Y, value.Z, value.W);
    }

    public Vector4 DisabledColor
    {
        get
        {
            ulong id = RequireComponent<Toggle>();
            InternalCalls.Toggle_GetDisabledColor(id, out float r, out float g, out float b, out float a);
            return new Color(r, g, b, a);
        }
        set => InternalCalls.Toggle_SetDisabledColor(RequireComponent<Toggle>(), value.X, value.Y, value.Z, value.W);
    }

    // Alpha == 0 sentinel = no focus tint; see Button.FocusedColor.
    public Vector4 FocusedColor
    {
        get
        {
            ulong id = RequireComponent<Toggle>();
            InternalCalls.Toggle_GetFocusedColor(id, out float r, out float g, out float b, out float a);
            return new Color(r, g, b, a);
        }
        set => InternalCalls.Toggle_SetFocusedColor(RequireComponent<Toggle>(), value.X, value.Y, value.Z, value.W);
    }

    public UITransitionMode TransitionMode
    {
        get => (UITransitionMode)InternalCalls.Toggle_GetTransitionMode(RequireComponent<Toggle>());
        set => InternalCalls.Toggle_SetTransitionMode(RequireComponent<Toggle>(), (int)value);
    }
    public Texture? NormalSprite
    {
        get => Index.Texture.FromAssetUUID(InternalCalls.Toggle_GetNormalSprite(RequireComponent<Toggle>()));
        set => InternalCalls.Toggle_SetNormalSprite(RequireComponent<Toggle>(), value?.UUID ?? 0);
    }
    public Texture? HoveredSprite
    {
        get => Index.Texture.FromAssetUUID(InternalCalls.Toggle_GetHoveredSprite(RequireComponent<Toggle>()));
        set => InternalCalls.Toggle_SetHoveredSprite(RequireComponent<Toggle>(), value?.UUID ?? 0);
    }
    public Texture? PressedSprite
    {
        get => Index.Texture.FromAssetUUID(InternalCalls.Toggle_GetPressedSprite(RequireComponent<Toggle>()));
        set => InternalCalls.Toggle_SetPressedSprite(RequireComponent<Toggle>(), value?.UUID ?? 0);
    }
    public Texture? DisabledSprite
    {
        get => Index.Texture.FromAssetUUID(InternalCalls.Toggle_GetDisabledSprite(RequireComponent<Toggle>()));
        set => InternalCalls.Toggle_SetDisabledSprite(RequireComponent<Toggle>(), value?.UUID ?? 0);
    }
    public Texture? FocusedSprite
    {
        get => Index.Texture.FromAssetUUID(InternalCalls.Toggle_GetFocusedSprite(RequireComponent<Toggle>()));
        set => InternalCalls.Toggle_SetFocusedSprite(RequireComponent<Toggle>(), value?.UUID ?? 0);
    }

    internal void RaiseValueChanged() => OnValueChanged?.Invoke(IsOn);
}

// ── InputField ──────────────────────────────────────────────────────

public class InputField : Component
{
    // Fires when the typed text changes (engine-driven keystroke or
    // SetValue with notify), passing the new text. Subscribe per-instance:
    //   field.OnValueChanged += text => searchSystem.Filter = text;
    public event Action<string>? OnValueChanged;

    // Focus gained / lost — pass the field so the handler can read
    // .Text or other state without a closure.
    public event Action<InputField>? OnSelect;
    public event Action<InputField>? OnDeselect;

    public string Text
    {
        get => InternalCalls.InputField_GetText(RequireComponent<InputField>());
        set => InternalCalls.InputField_SetText(RequireComponent<InputField>(), value ?? "");
    }

    public string PlaceholderText
    {
        get => InternalCalls.InputField_GetPlaceholderText(RequireComponent<InputField>());
        set => InternalCalls.InputField_SetPlaceholderText(RequireComponent<InputField>(), value ?? "");
    }

    public bool IsFocused
    {
        get => InternalCalls.InputField_GetIsFocused(RequireComponent<InputField>());
        set => InternalCalls.InputField_SetIsFocused(RequireComponent<InputField>(), value);
    }

    // The child entity whose TextRendererComponent shows the typed
    // text (or PlaceholderText). Auto-resolved by name "Text" when
    // null — set explicitly to point at a different entity.
    public Entity? TextEntity
    {
        get
        {
            ulong id = InternalCalls.InputField_GetTextEntity(RequireComponent<InputField>());
            return id == 0 ? null : new Entity(id);
        }
        set => InternalCalls.InputField_SetTextEntity(RequireComponent<InputField>(), value?.ID ?? 0);
    }

    // True on the frame the user pressed Enter while the field was
    // focused. Read once and act — clears at the start of next tick.
    public bool SubmittedThisFrame => InternalCalls.InputField_GetSubmittedThisFrame(RequireComponent<InputField>());

    public int CharacterLimit
    {
        get => InternalCalls.InputField_GetCharacterLimit(RequireComponent<InputField>());
        set => InternalCalls.InputField_SetCharacterLimit(RequireComponent<InputField>(), value);
    }

    // Programmatic text set with optional event suppression. The Text
    // setter alone never raises OnValueChanged; use this method when
    // you want script-driven changes to fan out to subscribers. The
    // dispatcher's text-diffing pass also ignores this assignment when
    // notifyEvent is false (the cached "last seen" string is bumped).
    public void SetValue(string value, bool notifyEvent = true)
    {
        ulong id = RequireComponent<InputField>();
        string previous = InternalCalls.InputField_GetText(id) ?? "";
        string next = value ?? "";
        InternalCalls.InputField_SetText(id, next);
        UIEventDispatcher.MarkInputFieldText(id, next);
        if (notifyEvent && previous != next)
            OnValueChanged?.Invoke(next);
    }

    public Vector4 NormalColor
    {
        get
        {
            ulong id = RequireComponent<InputField>();
            InternalCalls.InputField_GetNormalColor(id, out float r, out float g, out float b, out float a);
            return new Color(r, g, b, a);
        }
        set => InternalCalls.InputField_SetNormalColor(RequireComponent<InputField>(), value.X, value.Y, value.Z, value.W);
    }

    public Vector4 HoveredColor
    {
        get
        {
            ulong id = RequireComponent<InputField>();
            InternalCalls.InputField_GetHoveredColor(id, out float r, out float g, out float b, out float a);
            return new Color(r, g, b, a);
        }
        set => InternalCalls.InputField_SetHoveredColor(RequireComponent<InputField>(), value.X, value.Y, value.Z, value.W);
    }

    public Vector4 PressedColor
    {
        get
        {
            ulong id = RequireComponent<InputField>();
            InternalCalls.InputField_GetPressedColor(id, out float r, out float g, out float b, out float a);
            return new Color(r, g, b, a);
        }
        set => InternalCalls.InputField_SetPressedColor(RequireComponent<InputField>(), value.X, value.Y, value.Z, value.W);
    }

    public Vector4 DisabledColor
    {
        get
        {
            ulong id = RequireComponent<InputField>();
            InternalCalls.InputField_GetDisabledColor(id, out float r, out float g, out float b, out float a);
            return new Color(r, g, b, a);
        }
        set => InternalCalls.InputField_SetDisabledColor(RequireComponent<InputField>(), value.X, value.Y, value.Z, value.W);
    }

    // Alpha == 0 sentinel = no focus tint; see Button.FocusedColor.
    public Vector4 FocusedColor
    {
        get
        {
            ulong id = RequireComponent<InputField>();
            InternalCalls.InputField_GetFocusedColor(id, out float r, out float g, out float b, out float a);
            return new Color(r, g, b, a);
        }
        set => InternalCalls.InputField_SetFocusedColor(RequireComponent<InputField>(), value.X, value.Y, value.Z, value.W);
    }

    public UITransitionMode TransitionMode
    {
        get => (UITransitionMode)InternalCalls.InputField_GetTransitionMode(RequireComponent<InputField>());
        set => InternalCalls.InputField_SetTransitionMode(RequireComponent<InputField>(), (int)value);
    }
    public Texture? NormalSprite
    {
        get => Index.Texture.FromAssetUUID(InternalCalls.InputField_GetNormalSprite(RequireComponent<InputField>()));
        set => InternalCalls.InputField_SetNormalSprite(RequireComponent<InputField>(), value?.UUID ?? 0);
    }
    public Texture? HoveredSprite
    {
        get => Index.Texture.FromAssetUUID(InternalCalls.InputField_GetHoveredSprite(RequireComponent<InputField>()));
        set => InternalCalls.InputField_SetHoveredSprite(RequireComponent<InputField>(), value?.UUID ?? 0);
    }
    public Texture? PressedSprite
    {
        get => Index.Texture.FromAssetUUID(InternalCalls.InputField_GetPressedSprite(RequireComponent<InputField>()));
        set => InternalCalls.InputField_SetPressedSprite(RequireComponent<InputField>(), value?.UUID ?? 0);
    }
    public Texture? DisabledSprite
    {
        get => Index.Texture.FromAssetUUID(InternalCalls.InputField_GetDisabledSprite(RequireComponent<InputField>()));
        set => InternalCalls.InputField_SetDisabledSprite(RequireComponent<InputField>(), value?.UUID ?? 0);
    }
    public Texture? FocusedSprite
    {
        get => Index.Texture.FromAssetUUID(InternalCalls.InputField_GetFocusedSprite(RequireComponent<InputField>()));
        set => InternalCalls.InputField_SetFocusedSprite(RequireComponent<InputField>(), value?.UUID ?? 0);
    }

    internal void RaiseValueChanged(string text) => OnValueChanged?.Invoke(text);
    internal void RaiseSelect() => OnSelect?.Invoke(this);
    internal void RaiseDeselect() => OnDeselect?.Invoke(this);
}

// ── Dropdown ────────────────────────────────────────────────────────

public class Dropdown : Component
{
    // Fires when the user (or a script with notifyEvent) picks a
    // different option this frame. Passes the new index so handlers
    // don't have to re-read the property.
    //   dropdown.OnSelectedIndexChange += i => qualityPreset = i;
    public event Action<int>? OnSelectedIndexChange;

    public int SelectedIndex
    {
        get => InternalCalls.Dropdown_GetSelectedIndex(RequireComponent<Dropdown>());
        set => InternalCalls.Dropdown_SetSelectedIndex(RequireComponent<Dropdown>(), value);
    }

    public bool IsOpen
    {
        get => InternalCalls.Dropdown_GetIsOpen(RequireComponent<Dropdown>());
        set => InternalCalls.Dropdown_SetIsOpen(RequireComponent<Dropdown>(), value);
    }

    // When true the dropdown can't be opened or have its selection
    // changed by user input. Programmatic IsOpen / SelectedIndex /
    // option mutation still work.
    public bool IsReadOnly
    {
        get => InternalCalls.Dropdown_GetIsReadOnly(RequireComponent<Dropdown>());
        set => InternalCalls.Dropdown_SetIsReadOnly(RequireComponent<Dropdown>(), value);
    }

    // Optional child entity whose TextRendererComponent shows the
    // currently-selected option's text on the closed header cell.
    public Entity? LabelEntity
    {
        get
        {
            ulong id = InternalCalls.Dropdown_GetLabelEntity(RequireComponent<Dropdown>());
            return id == 0 ? null : new Entity(id);
        }
        set => InternalCalls.Dropdown_SetLabelEntity(RequireComponent<Dropdown>(), value?.ID ?? 0);
    }

    // Per-option-row tints for the popup. Alpha == 0 = "no override":
    // unset states fall through to the next-lower precedence and
    // ultimately to PopupBackgroundColor. Precedence:
    // pressed > hovered > selected > normal.
    public Vector4 OptionNormalColor
    {
        get
        {
            ulong id = RequireComponent<Dropdown>();
            InternalCalls.Dropdown_GetOptionNormalColor(id, out float r, out float g, out float b, out float a);
            return new Color(r, g, b, a);
        }
        set => InternalCalls.Dropdown_SetOptionNormalColor(RequireComponent<Dropdown>(), value.X, value.Y, value.Z, value.W);
    }
    public Vector4 OptionHoverColor
    {
        get
        {
            ulong id = RequireComponent<Dropdown>();
            InternalCalls.Dropdown_GetOptionHoverColor(id, out float r, out float g, out float b, out float a);
            return new Color(r, g, b, a);
        }
        set => InternalCalls.Dropdown_SetOptionHoverColor(RequireComponent<Dropdown>(), value.X, value.Y, value.Z, value.W);
    }
    public Vector4 OptionPressedColor
    {
        get
        {
            ulong id = RequireComponent<Dropdown>();
            InternalCalls.Dropdown_GetOptionPressedColor(id, out float r, out float g, out float b, out float a);
            return new Color(r, g, b, a);
        }
        set => InternalCalls.Dropdown_SetOptionPressedColor(RequireComponent<Dropdown>(), value.X, value.Y, value.Z, value.W);
    }
    public Vector4 OptionSelectedColor
    {
        get
        {
            ulong id = RequireComponent<Dropdown>();
            InternalCalls.Dropdown_GetOptionSelectedColor(id, out float r, out float g, out float b, out float a);
            return new Color(r, g, b, a);
        }
        set => InternalCalls.Dropdown_SetOptionSelectedColor(RequireComponent<Dropdown>(), value.X, value.Y, value.Z, value.W);
    }

    // The container background colour drawn behind every popup option
    // row. Acts as the implicit fallback when OptionNormalColor is
    // unset (alpha == 0).
    public Vector4 PopupBackgroundColor
    {
        get
        {
            ulong id = RequireComponent<Dropdown>();
            InternalCalls.Dropdown_GetPopupBackgroundColor(id, out float r, out float g, out float b, out float a);
            return new Color(r, g, b, a);
        }
        set => InternalCalls.Dropdown_SetPopupBackgroundColor(RequireComponent<Dropdown>(), value.X, value.Y, value.Z, value.W);
    }

    // Tint applied to each popup option's label text. Independent of
    // the per-state row tints — it only stains the glyphs.
    public Vector4 OptionTextColor
    {
        get
        {
            ulong id = RequireComponent<Dropdown>();
            InternalCalls.Dropdown_GetOptionTextColor(id, out float r, out float g, out float b, out float a);
            return new Color(r, g, b, a);
        }
        set => InternalCalls.Dropdown_SetOptionTextColor(RequireComponent<Dropdown>(), value.X, value.Y, value.Z, value.W);
    }

    public bool SelectionChangedThisFrame => InternalCalls.Dropdown_GetSelectionChangedThisFrame(RequireComponent<Dropdown>());

    public int OptionCount => InternalCalls.Dropdown_GetOptionCount(RequireComponent<Dropdown>());

    // Currently-selected option text. Empty when no options exist.
    public string SelectedOption
    {
        get
        {
            int idx = SelectedIndex;
            int count = OptionCount;
            if (count <= 0) return "";
            if (idx < 0) idx = 0;
            if (idx >= count) idx = count - 1;
            return InternalCalls.Dropdown_GetOption(RequireComponent<Dropdown>(), idx);
        }
    }

    public string GetOption(int index)
    {
        return InternalCalls.Dropdown_GetOption(RequireComponent<Dropdown>(), index);
    }

    public void SetOption(int index, string text, bool ignoreEvent = true)
    {
        InternalCalls.Dropdown_SetOption(RequireComponent<Dropdown>(), index, text ?? "");
    }

    public void AddOption(string text)
    {
        InternalCalls.Dropdown_AddOption(RequireComponent<Dropdown>(), text ?? "");
    }

    public void RemoveOption(int index)
    {
        InternalCalls.Dropdown_RemoveOption(RequireComponent<Dropdown>(), index);
    }

    public void ClearOptions()
    {
        InternalCalls.Dropdown_ClearOptions(RequireComponent<Dropdown>());
    }

    // Convenience: replace the entire option list in one call.
    public void SetOptions(params string[] options)
    {
        ClearOptions();
        if (options == null) return;
        foreach (string opt in options)
        {
            AddOption(opt ?? "");
        }
    }

    public Vector4 NormalColor
    {
        get
        {
            ulong id = RequireComponent<Dropdown>();
            InternalCalls.Dropdown_GetNormalColor(id, out float r, out float g, out float b, out float a);
            return new Color(r, g, b, a);
        }
        set => InternalCalls.Dropdown_SetNormalColor(RequireComponent<Dropdown>(), value.X, value.Y, value.Z, value.W);
    }

    public Vector4 HoveredColor
    {
        get
        {
            ulong id = RequireComponent<Dropdown>();
            InternalCalls.Dropdown_GetHoveredColor(id, out float r, out float g, out float b, out float a);
            return new Color(r, g, b, a);
        }
        set => InternalCalls.Dropdown_SetHoveredColor(RequireComponent<Dropdown>(), value.X, value.Y, value.Z, value.W);
    }

    public Vector4 PressedColor
    {
        get
        {
            ulong id = RequireComponent<Dropdown>();
            InternalCalls.Dropdown_GetPressedColor(id, out float r, out float g, out float b, out float a);
            return new Color(r, g, b, a);
        }
        set => InternalCalls.Dropdown_SetPressedColor(RequireComponent<Dropdown>(), value.X, value.Y, value.Z, value.W);
    }

    public Vector4 DisabledColor
    {
        get
        {
            ulong id = RequireComponent<Dropdown>();
            InternalCalls.Dropdown_GetDisabledColor(id, out float r, out float g, out float b, out float a);
            return new Color(r, g, b, a);
        }
        set => InternalCalls.Dropdown_SetDisabledColor(RequireComponent<Dropdown>(), value.X, value.Y, value.Z, value.W);
    }

    // Alpha == 0 sentinel = no focus tint; see Button.FocusedColor.
    public Vector4 FocusedColor
    {
        get
        {
            ulong id = RequireComponent<Dropdown>();
            InternalCalls.Dropdown_GetFocusedColor(id, out float r, out float g, out float b, out float a);
            return new Color(r, g, b, a);
        }
        set => InternalCalls.Dropdown_SetFocusedColor(RequireComponent<Dropdown>(), value.X, value.Y, value.Z, value.W);
    }

    public UITransitionMode TransitionMode
    {
        get => (UITransitionMode)InternalCalls.Dropdown_GetTransitionMode(RequireComponent<Dropdown>());
        set => InternalCalls.Dropdown_SetTransitionMode(RequireComponent<Dropdown>(), (int)value);
    }
    public Texture? NormalSprite
    {
        get => Index.Texture.FromAssetUUID(InternalCalls.Dropdown_GetNormalSprite(RequireComponent<Dropdown>()));
        set => InternalCalls.Dropdown_SetNormalSprite(RequireComponent<Dropdown>(), value?.UUID ?? 0);
    }
    public Texture? HoveredSprite
    {
        get => Index.Texture.FromAssetUUID(InternalCalls.Dropdown_GetHoveredSprite(RequireComponent<Dropdown>()));
        set => InternalCalls.Dropdown_SetHoveredSprite(RequireComponent<Dropdown>(), value?.UUID ?? 0);
    }
    public Texture? PressedSprite
    {
        get => Index.Texture.FromAssetUUID(InternalCalls.Dropdown_GetPressedSprite(RequireComponent<Dropdown>()));
        set => InternalCalls.Dropdown_SetPressedSprite(RequireComponent<Dropdown>(), value?.UUID ?? 0);
    }
    public Texture? DisabledSprite
    {
        get => Index.Texture.FromAssetUUID(InternalCalls.Dropdown_GetDisabledSprite(RequireComponent<Dropdown>()));
        set => InternalCalls.Dropdown_SetDisabledSprite(RequireComponent<Dropdown>(), value?.UUID ?? 0);
    }
    public Texture? FocusedSprite
    {
        get => Index.Texture.FromAssetUUID(InternalCalls.Dropdown_GetFocusedSprite(RequireComponent<Dropdown>()));
        set => InternalCalls.Dropdown_SetFocusedSprite(RequireComponent<Dropdown>(), value?.UUID ?? 0);
    }

    internal void RaiseSelectedIndexChange() => OnSelectedIndexChange?.Invoke(SelectedIndex);
}
