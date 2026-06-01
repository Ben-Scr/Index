using System;
using System.Collections.Generic;
using Index;
using Index.Interop;

namespace Index.UI;

// Called from RaiseUiEventDispatch (tail of UIEventSystem::Update). IsHovered fires only on
// rising edge; IsPressed fires every frame; InputField text change is diffed per entity.
internal static class UIEventDispatcher
{
    private static readonly HashSet<ulong> s_PrevHovered = new();
    private static readonly HashSet<ulong> s_PrevFocused = new();
    private static readonly HashSet<ulong> s_PrevFocusedInputFields = new();
    private static readonly Dictionary<ulong, string> s_LastInputFieldText = new();

    // Reusable buffers — UI rarely has thousands of widgets, so a
    // single per-component-type list keeps allocations off the hot path.
    private static ulong[] s_QueryBuffer = new ulong[64];

    internal static void MarkInputFieldText(ulong entityId, string text)
        => s_LastInputFieldText[entityId] = text ?? "";

    public static void Tick()
    {
        try
        {
            DispatchInteractables();
            DispatchButtons();
            DispatchSliders();
            DispatchToggles();
            DispatchDropdowns();
            DispatchInputFields();
            DispatchScrollbars();
            DispatchScrollRects();
            DispatchCircularSliders();
        }
        catch (Exception ex)
        {
            Log.Error($"UIEventDispatcher.Tick failed: {ex}");
        }
    }

    private static int Query(string componentName)
    {
        while (true)
        {
            int count = InternalCalls.Scene_QueryEntities(componentName, s_QueryBuffer);
            if (count <= s_QueryBuffer.Length)
                return count < 0 ? 0 : count;

            // Buffer too small — grow and retry. Native side returns the
            // total it would have written when the buffer is undersized.
            s_QueryBuffer = new ulong[count];
        }
    }

    private static void DispatchInteractables()
    {
        int count = Query("Interactable");
        if (count == 0)
        {
            s_PrevHovered.Clear();
            s_PrevFocused.Clear();
            return;
        }

        var nowHovered = new HashSet<ulong>();
        var nowFocused = new HashSet<ulong>();

        for (int i = 0; i < count; i++)
        {
            ulong id = s_QueryBuffer[i];

            Entity entity = new Entity(id);
            Interactable? interactable = entity.GetComponent<Interactable>();
            if (interactable == null) continue;

            bool hovered = InternalCalls.Interactable_GetIsHovered(id);
            bool pressed = InternalCalls.Interactable_GetIsPressed(id);
            bool clicked = InternalCalls.Interactable_GetIsClicked(id);
            bool mouseDown = InternalCalls.Interactable_GetIsMouseDown(id);
            bool mouseUp = InternalCalls.Interactable_GetIsMouseUp(id);
            bool focused = InternalCalls.Interactable_GetIsFocused(id);

            if (hovered)
            {
                nowHovered.Add(id);
                if (!s_PrevHovered.Contains(id))
                    interactable.RaiseHovered();
            }

            if (pressed)
                interactable.RaisePressed();

            if (focused)
            {
                nowFocused.Add(id);
                if (!s_PrevFocused.Contains(id))
                    interactable.RaiseFocusEnter();
            }
            else if (s_PrevFocused.Contains(id))
            {
                interactable.RaiseFocusExit();
            }

            if (clicked)
            {
                interactable.RaiseClicked();
                // Button.OnClick handled in DispatchButtons to cover TargetGraphic-owned Interactables.
            }

            if (mouseDown) interactable.RaiseMouseDown();
            if (mouseUp) interactable.RaiseMouseUp();
        }

        s_PrevHovered.Clear();
        foreach (ulong id in nowHovered) s_PrevHovered.Add(id);
        s_PrevFocused.Clear();
        foreach (ulong id in nowFocused) s_PrevFocused.Add(id);
    }

    private static void DispatchButtons()
    {
        int count = Query("Button");
        for (int i = 0; i < count; i++)
        {
            ulong buttonId = s_QueryBuffer[i];
            ulong interactableId = 0;
            if (InternalCalls.Entity_HasComponent(buttonId, "Interactable"))
            {
                interactableId = buttonId;
            }
            else
            {
                ulong target = InternalCalls.Button_GetTargetGraphic(buttonId);
                if (target != 0 && InternalCalls.Entity_HasComponent(target, "Interactable"))
                {
                    interactableId = target;
                }
            }
            if (interactableId == 0) continue;

            bool mouseDown = InternalCalls.Interactable_GetIsMouseDown(interactableId);
            bool clicked   = InternalCalls.Interactable_GetIsClicked(interactableId);
            bool mouseUp   = InternalCalls.Interactable_GetIsMouseUp(interactableId);
            bool pressed   = InternalCalls.Interactable_GetIsPressed(interactableId);
            if (!mouseDown && !clicked && !mouseUp && !pressed) continue;

            Button? button = new Entity(buttonId).GetComponent<Button>();
            if (button == null) continue;

            if (mouseDown) button.RaiseClickDown();
            if (clicked)   button.RaiseClick();
            if (mouseUp)   button.RaiseClickUp();
            if (pressed)   button.RaisePressed();
        }
    }

    private static void DispatchScrollbars()
    {
        int count = Query("Scrollbar");
        for (int i = 0; i < count; i++)
        {
            ulong id = s_QueryBuffer[i];
            if (!InternalCalls.Scrollbar_GetValueChangedThisFrame(id)) continue;
            Scrollbar? scrollbar = new Entity(id).GetComponent<Scrollbar>();
            scrollbar?.RaiseValueChanged();
        }
    }

    private static void DispatchScrollRects()
    {
        int count = Query("Scroll Rect");
        for (int i = 0; i < count; i++)
        {
            ulong id = s_QueryBuffer[i];
            if (!InternalCalls.ScrollRect_GetValueChangedThisFrame(id)) continue;
            ScrollRect? scrollRect = new Entity(id).GetComponent<ScrollRect>();
            scrollRect?.RaiseValueChanged();
        }
    }

    private static void DispatchCircularSliders()
    {
        int count = Query("Circular Slider");
        for (int i = 0; i < count; i++)
        {
            ulong id = s_QueryBuffer[i];
            if (!InternalCalls.CircularSlider_GetValueChangedThisFrame(id)) continue;
            CircularSlider? circular = new Entity(id).GetComponent<CircularSlider>();
            circular?.RaiseValueChanged();
        }
    }

    private static void DispatchSliders()
    {
        int count = Query("Slider");
        for (int i = 0; i < count; i++)
        {
            ulong id = s_QueryBuffer[i];
            if (!InternalCalls.Slider_GetValueChangedThisFrame(id)) continue;
            Slider? slider = new Entity(id).GetComponent<Slider>();
            slider?.RaiseValueChanged();
        }
    }

    private static void DispatchToggles()
    {
        int count = Query("Toggle");
        for (int i = 0; i < count; i++)
        {
            ulong id = s_QueryBuffer[i];
            if (!InternalCalls.Toggle_GetValueChangedThisFrame(id)) continue;
            Toggle? toggle = new Entity(id).GetComponent<Toggle>();
            toggle?.RaiseValueChanged();
        }
    }

    private static void DispatchDropdowns()
    {
        int count = Query("Dropdown");
        for (int i = 0; i < count; i++)
        {
            ulong id = s_QueryBuffer[i];
            if (!InternalCalls.Dropdown_GetSelectionChangedThisFrame(id)) continue;
            Dropdown? dropdown = new Entity(id).GetComponent<Dropdown>();
            dropdown?.RaiseSelectedIndexChange();
        }
    }

    private static void DispatchInputFields()
    {
        int count = Query("Input Field");
        if (count == 0)
        {
            // Any field that vanished while focused never gets an explicit
            // OnDeselect — that matches the "destroyed entity" semantics
            // already used elsewhere (no zombie events on dead handles).
            s_LastInputFieldText.Clear();
            s_PrevFocusedInputFields.Clear();
            return;
        }

        // Track which entities we saw this frame so we can prune cache
        // entries for destroyed input fields without a separate validity
        // ping per entry.
        var seen = new HashSet<ulong>();
        var nowFocused = new HashSet<ulong>();

        for (int i = 0; i < count; i++)
        {
            ulong id = s_QueryBuffer[i];
            seen.Add(id);

            InputField? field = new Entity(id).GetComponent<InputField>();
            if (field == null) continue;

            // ── Text change ────────────────────────────────────────
            string current = InternalCalls.InputField_GetText(id) ?? "";
            if (s_LastInputFieldText.TryGetValue(id, out string? previous))
            {
                if (previous != current)
                {
                    s_LastInputFieldText[id] = current;
                    field.RaiseValueChanged(current);
                }
            }
            else
            {
                // First time we've seen this field — seed without firing
                // (we have no prior baseline to call this a change against).
                s_LastInputFieldText[id] = current;
            }

            bool focused = InternalCalls.InputField_GetIsFocused(id);
            bool wasFocused = s_PrevFocusedInputFields.Contains(id);
            if (focused)
            {
                nowFocused.Add(id);
                if (!wasFocused) field.RaiseSelect();
            }
            else if (wasFocused)
            {
                field.RaiseDeselect();
            }
        }

        if (s_LastInputFieldText.Count > seen.Count)
        {
            List<ulong>? stale = null;
            foreach (ulong id in s_LastInputFieldText.Keys)
            {
                if (!seen.Contains(id))
                    (stale ??= new List<ulong>()).Add(id);
            }
            if (stale != null)
                foreach (ulong id in stale)
                    s_LastInputFieldText.Remove(id);
        }

        s_PrevFocusedInputFields.Clear();
        foreach (ulong id in nowFocused) s_PrevFocusedInputFields.Add(id);
    }
}
