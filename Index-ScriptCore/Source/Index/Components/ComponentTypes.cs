using System;
using System.Collections.Generic;
using System.Reflection;
using System.Runtime.CompilerServices;
using Index.Interop;

namespace Index.Components;

// Maps IComponent struct types to native names; static-generic so the JIT hoists lookups per call site.
internal static class ComponentTypes
{
    private static readonly Dictionary<Type, string> s_NativeNames = new()
    {
        { typeof(Native.NativeTransform2D),    Native.NativeTransform2D.NativeName },
        { typeof(Native.NativeSpriteRenderer), Native.NativeSpriteRenderer.NativeName },
    };

    internal static string? TryGetNativeName(Type t)
    {
        if (s_NativeNames.TryGetValue(t, out string? n))
            return n;

        return ResolveNativeName(t, 0, out _);
    }

    internal static string? ResolveNativeName(Type t, int managedSize, out int nativeSize)
    {
        var candidates = new List<string>(6);
        if (s_NativeNames.TryGetValue(t, out string? registered))
            candidates.Add(registered);

        const BindingFlags flags = BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Static;
        FieldInfo? nativeNameField = t.GetField("NativeName", flags);
        if (nativeNameField?.FieldType == typeof(string)
            && nativeNameField.GetValue(null) is string fieldName
            && !string.IsNullOrWhiteSpace(fieldName))
        {
            candidates.Add(fieldName);
        }

        PropertyInfo? nativeNameProperty = t.GetProperty("NativeName", flags);
        if (nativeNameProperty?.PropertyType == typeof(string)
            && nativeNameProperty.GetValue(null) is string propertyName
            && !string.IsNullOrWhiteSpace(propertyName))
        {
            candidates.Add(propertyName);
        }

        AddNameCandidates(t.Name, candidates);

        var seen = new HashSet<string>(StringComparer.Ordinal);
        foreach (string candidate in candidates)
        {
            if (string.IsNullOrWhiteSpace(candidate) || !seen.Add(candidate))
                continue;

            nativeSize = InternalCalls.Entity_GetComponentSize(candidate);
            if (nativeSize != 0)
                return candidate;
        }

        nativeSize = 0;
        return null;
    }

    private static void AddNameCandidates(string typeName, List<string> candidates)
    {
        if (string.IsNullOrWhiteSpace(typeName))
            return;

        candidates.Add(typeName);

        string stripped = typeName.StartsWith("Native", StringComparison.Ordinal)
            ? typeName.Substring("Native".Length)
            : typeName;

        if (!string.IsNullOrWhiteSpace(stripped))
            candidates.Add(stripped);

        const string suffix = "Component";
        if (stripped.EndsWith(suffix, StringComparison.Ordinal) && stripped.Length > suffix.Length)
            candidates.Add(stripped.Substring(0, stripped.Length - suffix.Length));
    }

    internal static void RunAllStaticInitializers()
    {
        Type generic = typeof(ComponentTypes<>);
        foreach (Type t in s_NativeNames.Keys)
        {
            RuntimeHelpers.RunClassConstructor(generic.MakeGenericType(t).TypeHandle);
        }
    }
}

internal static class ComponentTypes<T> where T : unmanaged, IComponent
{
    internal static readonly string NativeName;
    // Zero-cost cache of the stable native type ID; used by ECB playback for pointer-only resolution.
    internal static readonly uint NativeId;

    static ComponentTypes()
    {
        int managedSize = Unsafe.SizeOf<T>();
        string? name = ComponentTypes.ResolveNativeName(typeof(T), managedSize, out int nativeSize);
        if (name == null)
        {
            throw new InvalidOperationException(
                $"Component '{typeof(T).Name}' is not registered for the ECS ref-API. " +
                "If this is a user-authored component, ensure the user assembly is loaded " +
                "and the type implements IComponent. If this is a hand-mirror of a built-in, " +
                "declare `internal const string NativeName = \"Display Name\";` on the struct.");
        }

        // C# mirror is a prefix view: nativeSize >= managedSize is safe (C++ may have trailing fields); nativeSize < managedSize would walk past the pool entry.
        if (nativeSize != 0 && nativeSize < managedSize)
        {
            throw new InvalidOperationException(
                $"Layout mismatch for component '{typeof(T).Name}' (native '{name}'): " +
                $"C++ sizeof = {nativeSize} is SMALLER than C# sizeof = {managedSize}. " +
                "Reading via GetRef<T> would walk past the end of the native component. " +
                "Either trim the C# mirror's fields or extend the C++ struct so its size " +
                "matches or exceeds the mirror.");
        }

        NativeName = name;
        NativeId   = InternalCalls.Component_GetTypeId(name);
        if (NativeId == 0)
        {
            throw new InvalidOperationException(
                $"Component '{typeof(T).Name}' (native '{name}') did not resolve to a stable type ID. " +
                "The native ComponentRegistry must register the component with a non-zero typeIdU32 " +
                "before any managed code references ComponentTypes<T>.");
        }
    }
}
