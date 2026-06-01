using System;
using System.Reflection;
using System.Runtime.InteropServices;

using Index;
using Index.Components;

namespace Index.Interop;

// RegisterAll must be called AFTER the ALC loads; UnregisterAll BEFORE it tears down (native callbacks must not outlive DynamicComponentStorage).
internal static class DynamicComponentRegistrar
{
    public static void RegisterAll(Assembly userAssembly)
    {
        if (userAssembly is null)
        {
            return;
        }

        Type?[] types;
        try
        {
            types = userAssembly.GetTypes();
        }
        catch (ReflectionTypeLoadException ex)
        {
            // Some types failed to load — log and continue with what we got.
            string message = ex.Message;
            for (int i = 0; i < ex.LoaderExceptions.Length; ++i)
            {
                var inner = ex.LoaderExceptions[i];
                if (inner is not null)
                {
                    message += "\n  " + inner.Message;
                }
            }
            Log.Error($"DynamicComponentRegistrar: partial type load — {message}");
            types = ex.Types ?? Array.Empty<Type?>();
        }

        int registered = 0;
        foreach (Type? type in types)
        {
            if (type is null) continue;
            if (!type.IsValueType || type.IsEnum || type.IsPrimitive) continue;
            if (!typeof(IComponent).IsAssignableFrom(type)) continue;

            if (!TryRegisterStruct(type))
            {
                continue;
            }
            ++registered;
        }

        if (registered > 0)
        {
            Log.Info($"DynamicComponentRegistrar: registered {registered} user component(s)");
        }
    }

    public static void UnregisterAll()
    {
        InternalCalls.Component_UnregisterAllDynamic();
    }

    private static bool TryRegisterStruct(Type type)
    {
        // Layout-sequential required so the C++ side can treat the component
        // bytes as a flat blittable struct that mirrors C#'s field layout.
        // C# doesn't auto-enforce sequential on value types — we do.
        var layout = type.StructLayoutAttribute?.Value;
        if (layout is not null && layout != LayoutKind.Sequential && layout != LayoutKind.Explicit)
        {
            Log.Error(
                $"DynamicComponentRegistrar: '{type.FullName}' implements IComponent " +
                "but lacks [StructLayout(LayoutKind.Sequential)] — required so C# field " +
                "order matches the native byte layout. Component skipped.");
            return false;
        }

        // MUST use Unsafe.SizeOf (managed layout), NOT Marshal.SizeOf: Marshal sees bool as 4-byte Win32 BOOL, causing a C++/C# size mismatch that breaks bool-field components.
        int size = ComputeManagedSize(type);
        if (size <= 0)
        {
            Log.Error($"DynamicComponentRegistrar: '{type.FullName}' has non-positive managed size {size}.");
            return false;
        }

        string displayName = type.Name;
        string subcategory = "Native Components (C#)";
        uint   categoryCode = 0u; // 0 = Component (Tag is reserved for engine-internal)

        if (!TryComputeAlignment(type, out uint alignment))
        {
            return false;
        }

        uint typeIdU32 = InternalCalls.Component_RegisterDynamic(
            displayName,
            displayName,   // serializedName == displayName
            subcategory,
            categoryCode,
            (uint)size,
            alignment);

        if (typeIdU32 == 0)
        {
            Log.Error($"DynamicComponentRegistrar: native side refused registration for '{type.FullName}'.");
            return false;
        }

        return true;
    }

    private static int ComputeManagedSize(Type structType)
    {
        try
        {
            MethodInfo? sizeOf = typeof(System.Runtime.CompilerServices.Unsafe)
                .GetMethod(nameof(System.Runtime.CompilerServices.Unsafe.SizeOf),
                    BindingFlags.Public | BindingFlags.Static, types: Type.EmptyTypes);
            if (sizeOf == null) return -1;
            object? boxed = sizeOf.MakeGenericMethod(structType).Invoke(null, null);
            return boxed is int i ? i : -1;
        }
        catch
        {
            return -1;
        }
    }

    private static bool TryComputeAlignment(Type structType, out uint alignment)
    {
        alignment = 1;
        var fields = structType.GetFields(BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic);
        foreach (var field in fields)
        {
            if (field.IsStatic) continue;

            if (!TryGetFieldAlignment(field.FieldType, out uint fieldAlign))
            {
                Log.Error(
                    $"DynamicComponentRegistrar: '{structType.FullName}.{field.Name}' has field type " +
                    $"'{field.FieldType.FullName}' which is not in the supported ECS-component field " +
                    "whitelist (sbyte/byte/short/ushort/int/uint/long/ulong/float/double/bool, " +
                    "or a recursively-blittable struct of those). Component skipped.");
                return false;
            }
            if (fieldAlign > alignment) alignment = fieldAlign;
        }
        return true;
    }

    private static bool TryGetFieldAlignment(Type fieldType, out uint alignment)
    {
        if (fieldType == typeof(bool) || fieldType == typeof(sbyte) || fieldType == typeof(byte))
        { alignment = 1; return true; }
        if (fieldType == typeof(short) || fieldType == typeof(ushort))
        { alignment = 2; return true; }
        if (fieldType == typeof(int) || fieldType == typeof(uint) || fieldType == typeof(float))
        { alignment = 4; return true; }
        if (fieldType == typeof(long) || fieldType == typeof(ulong) || fieldType == typeof(double))
        { alignment = 8; return true; }

        // Recurse into nested blittable value types (e.g. Vector2, Vector3).
        if (fieldType.IsValueType && !fieldType.IsEnum && !fieldType.IsPrimitive)
        {
            return TryComputeAlignment(fieldType, out alignment);
        }

        // Enums: align to underlying integral type.
        if (fieldType.IsEnum)
        {
            return TryGetFieldAlignment(Enum.GetUnderlyingType(fieldType), out alignment);
        }

        alignment = 0;
        return false;
    }
}
