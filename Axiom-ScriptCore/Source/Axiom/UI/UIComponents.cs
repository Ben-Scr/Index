using Axiom;
using Axiom.Interop;

namespace Axiom.UI;

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

    public float Rotation
    {
        get => InternalCalls.RectTransform_GetRotation(RequireComponent<RectTransform>());
        set => InternalCalls.RectTransform_SetRotation(RequireComponent<RectTransform>(), value);
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
            return Axiom.Texture.FromAssetUUID(assetId);
        }
        set => InternalCalls.Image_SetTexture(RequireComponent<Image>(), value?.UUID ?? 0);
    }
}

// ── Interactable ────────────────────────────────────────────────────
//
// Per-frame UI input state. Reads of IsHovered/IsClicked/etc are
// updated by UIEventSystem before any user code runs, so a script's
// OnUpdate() can react to clicks the same frame they happen.

public class Interactable : Component
{
    public bool IsInteractable
    {
        get => InternalCalls.Interactable_GetInteractable(RequireComponent<Interactable>());
        set => InternalCalls.Interactable_SetInteractable(RequireComponent<Interactable>(), value);
    }

    // Read-only: these are driven entirely by UIEventSystem's hit-test
    // and press-tracking. Setting them from script wouldn't be honored
    // (they get overwritten next frame).
    public bool IsHovered   => InternalCalls.Interactable_GetIsHovered(RequireComponent<Interactable>());
    public bool IsClicked   => InternalCalls.Interactable_GetIsClicked(RequireComponent<Interactable>());
    public bool IsPressed   => InternalCalls.Interactable_GetIsPressed(RequireComponent<Interactable>());
    public bool IsMouseDown => InternalCalls.Interactable_GetIsMouseDown(RequireComponent<Interactable>());
    public bool IsMouseUp   => InternalCalls.Interactable_GetIsMouseUp(RequireComponent<Interactable>());
}

// ── Button ──────────────────────────────────────────────────────────
//
// State-tint preset that the engine applies to the Image on the same
// entity each frame. The "button was clicked" event is read via the
// sibling Interactable component's IsClicked.

public class Button : Component
{
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
}

// ── Slider ──────────────────────────────────────────────────────────

public class Slider : Component
{
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

    // True on the frame Value moved (either from user drag or a
    // scripted Set). Mirrors the SliderComponent's transient flag
    // — UIEventSystem clears it at the start of the next tick.
    public bool ValueChangedThisFrame => InternalCalls.Slider_GetValueChangedThisFrame(RequireComponent<Slider>());

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
}

// ── Toggle ──────────────────────────────────────────────────────────

public class Toggle : Component
{
    public bool IsOn
    {
        get => InternalCalls.Toggle_GetIsOn(RequireComponent<Toggle>());
        set => InternalCalls.Toggle_SetIsOn(RequireComponent<Toggle>(), value);
    }

    public bool ValueChangedThisFrame => InternalCalls.Toggle_GetValueChangedThisFrame(RequireComponent<Toggle>());
}

// ── InputField ──────────────────────────────────────────────────────

public class InputField : Component
{
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

    // True on the frame the user pressed Enter while the field was
    // focused. Read once and act — clears at the start of next tick.
    public bool SubmittedThisFrame => InternalCalls.InputField_GetSubmittedThisFrame(RequireComponent<InputField>());

    public int CharacterLimit
    {
        get => InternalCalls.InputField_GetCharacterLimit(RequireComponent<InputField>());
        set => InternalCalls.InputField_SetCharacterLimit(RequireComponent<InputField>(), value);
    }
}

// ── Dropdown ────────────────────────────────────────────────────────

public class Dropdown : Component
{
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

    public void SetOption(int index, string text)
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
}
