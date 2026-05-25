namespace Index;

/// <summary>
/// AABB-based "what entity is at this world point" query. Mirrors the
/// algorithm the editor viewport uses for click-to-select, so a script
/// querying <c>EntityPicker.TryPickEntity</c> sees the same result the
/// editor would have selected if the user clicked there.
///
/// Hit-test rules:
///   - World-space entities (Transform2DComponent) hit-test against their
///     AABB. Sprite renderers break ties by SortingLayer then SortingOrder
///     (highest = on top); non-sprite entities sort at (0, 0).
///   - UI entities (RectTransform2DComponent) always override world-space
///     hits because UI draws on top; among UI hits, the later-iterated
///     entity wins (matches render order).
/// </summary>
public static class EntityPicker
{
    /// <summary>
    /// Hit-test the active scene at <paramref name="worldPoint"/> and return
    /// the topmost entity whose AABB contains the point.
    /// </summary>
    /// <param name="worldPoint">World-space query position.</param>
    /// <param name="entity">The hit entity on success; <c>null</c> otherwise.</param>
    /// <returns><c>true</c> when an entity was hit.</returns>
    public static bool TryPickEntity(Vector2 worldPoint, out Entity? entity)
    {
        if (InternalCalls.EntityPicker_TryPickEntity(worldPoint.X, worldPoint.Y, out ulong entityID))
        {
            entity = new Entity(entityID);
            return true;
        }
        entity = null;
        return false;
    }

    /// <summary>
    /// Convenience wrapper: returns the picked entity, or <c>null</c> if
    /// nothing was hit at <paramref name="worldPoint"/>.
    /// </summary>
    public static Entity? PickEntity(Vector2 worldPoint)
    {
        return TryPickEntity(worldPoint, out Entity? entity) ? entity : null;
    }
}
