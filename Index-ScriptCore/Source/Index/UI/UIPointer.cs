using Index;
using Index.Interop;

namespace Index.UI;

/// <summary>
/// Pointer queries against the UI layer. Only entities carrying an enabled <see cref="Interactable"/>
/// component are ever tracked, so anything reported here is guaranteed to be an interactable UI entity.
/// </summary>
public static class UIPointer
{
    /// <summary>
    /// The front-most interactable UI entity currently under the pointer, or <c>null</c> when nothing is hovered.
    /// </summary>
    public static Entity? HoveredEntity
        => InternalCalls.UI_GetHoveredEntity(out ulong id) ? new Entity(id) : null;

    /// <summary>
    /// True when an interactable UI entity is under the pointer, returning it via <paramref name="entity"/>.
    /// </summary>
    public static bool TryGetHoveredEntity(out Entity? entity)
    {
        if (InternalCalls.UI_GetHoveredEntity(out ulong id))
        {
            entity = new Entity(id);
            return true;
        }
        entity = null;
        return false;
    }
}
