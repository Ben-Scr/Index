using System;
using static Index.Physics.PhysicsShared;

namespace Index.Physics;

// Index-Physics (lightweight AABB) query API. Backs the FastBody2D / FastBoxCollider2D /
// FastCircleCollider2D stack — distinct from Physics2D, which queries the Box2D world.
public static class FastPhysics2D
{
    public static int MaxPolygonVertices = 8;

    public static RaycastHit2D Raycast(Vector2 origin, Vector2 direction, float maxDistance = Mathf.Infinity)
    {
        RaycastHit2D result = new();

        if (!IsValidDirection(direction) || !IsValidDistance(maxDistance))
            return result;

        bool hit = InternalCalls.FastPhysics2D_Raycast(
            origin.X, origin.Y,
            direction.X, direction.Y,
            maxDistance,
            out ulong hitEntityID,
            out float hitX, out float hitY,
            out float hitNormalX, out float hitNormalY,
            out float distance
        );

        result.Hit = hit;
        if (hit)
        {
            result.Entity = ToEntity(hitEntityID);
            result.Point = new Vector2(hitX, hitY);
            result.Normal = new Vector2(hitNormalX, hitNormalY);
            result.Distance = distance;
        }

        return result;
    }

    public static bool RaycastCheck(Vector2 origin, Vector2 direction, float maxDistance = Mathf.Infinity)
        => Raycast(origin, direction, maxDistance).Hit;

    public static Entity? OverlapCircle(Vector2 origin, float radius)
    {
        if (radius <= 0.0f || float.IsNaN(radius))
            return null;

        return InternalCalls.FastPhysics2D_OverlapCircle(origin.X, origin.Y, radius, 0, out ulong entityID)
            ? ToEntity(entityID)
            : null;
    }

    public static bool OverlapCircleCheck(Vector2 origin, float radius)
        => OverlapCircle(origin, radius) != null;

    public static Entity? OverlapBox(Vector2 origin, Vector2 size, float rotation = 0)
    {
        if (size.X <= 0.0f || size.Y <= 0.0f || float.IsNaN(rotation))
            return null;

        Vector2 halfExtents = size * 0.5f;
        return InternalCalls.FastPhysics2D_OverlapBox(origin.X, origin.Y, halfExtents.X, halfExtents.Y, rotation, 0, out ulong entityID)
            ? ToEntity(entityID)
            : null;
    }

    public static bool OverlapBoxCheck(Vector2 origin, Vector2 size, float rotation = 0)
        => OverlapBox(origin, size, rotation) != null;

    public static Entity[] OverlapCircleAll(Vector2 origin, float radius)
    {
        if (radius <= 0.0f || float.IsNaN(radius))
            return Array.Empty<Entity>();

        return ToEntities(buffer => InternalCalls.FastPhysics2D_OverlapCircleAll(
            origin.X, origin.Y, radius, buffer));
    }

    public static Entity[] OverlapBoxAll(Vector2 origin, Vector2 size, float rotation = 0)
    {
        if (size.X <= 0.0f || size.Y <= 0.0f || float.IsNaN(rotation))
            return Array.Empty<Entity>();

        Vector2 halfExtents = size * 0.5f;
        return ToEntities(buffer => InternalCalls.FastPhysics2D_OverlapBoxAll(
            origin.X, origin.Y, halfExtents.X, halfExtents.Y, rotation, buffer));
    }

    public static Entity? OverlapPolygon(Vector2 origin, params float[] points)
    {
        if (!IsValidPolygon(points, MaxPolygonVertices))
            return null;

        return InternalCalls.FastPhysics2D_OverlapPolygon(origin.X, origin.Y, points, 0, out ulong entityID)
            ? ToEntity(entityID)
            : null;
    }

    public static bool OverlapPolygonCheck(Vector2 origin, params float[] points)
        => OverlapPolygon(origin, points) != null;

    public static Entity[] OverlapPolygonAll(Vector2 origin, params float[] points)
    {
        if (!IsValidPolygon(points, MaxPolygonVertices))
            return Array.Empty<Entity>();

        return ToEntities(buffer => InternalCalls.FastPhysics2D_OverlapPolygonAll(origin.X, origin.Y, points, buffer));
    }

    public static Entity? ContainsPoint(Vector2 origin)
    {
        return InternalCalls.FastPhysics2D_ContainsPoint(origin.X, origin.Y, 0, out ulong entityID)
            ? ToEntity(entityID)
            : null;
    }

    public static Entity[] ContainsPointAll(Vector2 origin)
    {
        return ToEntities(buffer => InternalCalls.FastPhysics2D_ContainsPointAll(origin.X, origin.Y, buffer));
    }

    public static bool ContainsPointCheck(Vector2 origin)
        => ContainsPoint(origin) != null;
}
