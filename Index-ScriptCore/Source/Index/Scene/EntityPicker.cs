namespace Index;

/// <summary>AABB hit-test matching the editor click-to-select algorithm. UI entities always override world-space hits; ties broken by SortingLayer/SortingOrder.</summary>
public static class EntityPicker
{
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

    public static Entity? PickEntity(Vector2 worldPoint)
    {
        return TryPickEntity(worldPoint, out Entity? entity) ? entity : null;
    }
}
