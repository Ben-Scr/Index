using System;
using System.Collections;
using System.Linq;
using System.Reflection;
using Xunit;

namespace IndexScriptCoreTests;

/// <summary>
/// Pins the native→managed entity-invalidation contract: when the engine destroys
/// an entity (Scene::DestroyEntityInternal / ClearEntities / scene unload) it calls
/// into <c>ScriptInstanceManager.NotifyEntityDestroyed</c> →
/// <c>Entity.OnNativeEntityDestroyed</c>, which must drop exactly that UUID's cached
/// wrappers from <c>Entity.s_ManagedComponentStore</c> (and invalidate them) while
/// leaving other entities' wrappers untouched. Without this the store leaks wrappers
/// for natively-destroyed entities until the next assembly unload.
///
/// Reached via reflection so the production interop files are not modified for testing.
/// The [UnmanagedCallersOnly] thunk itself cannot be invoked from managed code, so the
/// test drives the managed entry point it forwards to.
/// </summary>
public class ManagedComponentStoreTests
{
    private static readonly Assembly s_Asm = typeof(Index.Vector2).Assembly;
    private static readonly Type s_EntityType = Resolve("Index.Entity");
    private static readonly Type s_ComponentType = Resolve("Index.Component");

    private static Type Resolve(string fullName)
    {
        Type? t = s_Asm.GetType(fullName) ?? s_Asm.GetTypes().FirstOrDefault(x => x.FullName == fullName);
        Assert.True(t is not null, $"{fullName} not found in Index-ScriptCore.");
        return t!;
    }

    private sealed class FakeComponent : Index.Component { }

    private static IDictionary Store()
    {
        FieldInfo f = s_EntityType.GetField("s_ManagedComponentStore",
            BindingFlags.NonPublic | BindingFlags.Static)!;
        Assert.True(f is not null, "Entity.s_ManagedComponentStore not found — update this test.");
        return (IDictionary)f!.GetValue(null)!;
    }

    private static void OnNativeEntityDestroyed(ulong id)
    {
        MethodInfo m = s_EntityType.GetMethod("OnNativeEntityDestroyed",
            BindingFlags.NonPublic | BindingFlags.Static, binder: null,
            types: new[] { typeof(ulong) }, modifiers: null)!;
        Assert.True(m is not null, "Entity.OnNativeEntityDestroyed(ulong) not found — update this test.");
        m!.Invoke(null, new object[] { id });
    }

    // Dictionary key is the ValueTuple<ulong, Type> behind (ulong EntityID, Type ComponentType).
    private static object Key(ulong id, Type type) => (id, type);

    private static Index.Entity MakeEntity(ulong id) =>
        (Index.Entity)Activator.CreateInstance(s_EntityType,
            BindingFlags.Instance | BindingFlags.NonPublic, binder: null,
            args: new object[] { id }, culture: null)!;

    private static void SetEntity(Index.Component component, Index.Entity entity) =>
        s_ComponentType.GetProperty("Entity")!.GetSetMethod(nonPublic: true)!
            .Invoke(component, new object[] { entity });

    private static Index.Entity GetEntity(Index.Component component) =>
        (Index.Entity)s_ComponentType.GetProperty("Entity")!.GetValue(component)!;

    private static Index.Entity Invalid() =>
        (Index.Entity)s_EntityType.GetField("Invalid", BindingFlags.Public | BindingFlags.Static)!
            .GetValue(null)!;

    [Fact]
    public void DropsOnlyTheDestroyedEntitysWrappers()
    {
        const ulong destroyed = 0xDEAD_0001UL;
        const ulong survivor = 0xDEAD_0002UL;

        IDictionary store = Store();
        object keyDestroyed = Key(destroyed, typeof(FakeComponent));
        object keySurvivor = Key(survivor, typeof(FakeComponent));

        var compDestroyed = new FakeComponent();
        var compSurvivor = new FakeComponent();
        SetEntity(compDestroyed, MakeEntity(destroyed));
        SetEntity(compSurvivor, MakeEntity(survivor));

        store[keyDestroyed] = compDestroyed;
        store[keySurvivor] = compSurvivor;
        try
        {
            OnNativeEntityDestroyed(destroyed);

            Assert.False(store.Contains(keyDestroyed), "destroyed entity's wrapper must be removed");
            Assert.True(store.Contains(keySurvivor), "other entities' wrappers must survive");

            // Removed wrapper is invalidated (Entity reset to the shared Invalid sentinel);
            // the survivor keeps its live entity.
            Assert.Same(Invalid(), GetEntity(compDestroyed));
            Assert.NotSame(Invalid(), GetEntity(compSurvivor));
        }
        finally
        {
            store.Remove(keyDestroyed);
            store.Remove(keySurvivor);
        }
    }

    [Fact]
    public void UnknownEntityIsANoOp()
    {
        IDictionary store = Store();
        const ulong present = 0xBEEF_0001UL;
        const ulong absent = 0xBEEF_9999UL;
        object keyPresent = Key(present, typeof(FakeComponent));

        var comp = new FakeComponent();
        SetEntity(comp, MakeEntity(present));
        store[keyPresent] = comp;
        try
        {
            // No entry for `absent` — must not throw and must not disturb existing entries.
            OnNativeEntityDestroyed(absent);
            Assert.True(store.Contains(keyPresent));
            Assert.NotSame(Invalid(), GetEntity(comp));
        }
        finally
        {
            store.Remove(keyPresent);
        }
    }
}
