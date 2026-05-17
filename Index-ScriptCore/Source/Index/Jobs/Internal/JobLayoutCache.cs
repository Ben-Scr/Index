using System;
using System.Collections.Concurrent;
using System.Reflection;
using Index.Collections.Native;

namespace Index.Jobs.Internal;

/// <summary>
/// One-shot, per-type reflection pass that walks a job struct's fields and
/// emits warnings when <see cref="ReadOnlyAttribute"/> / <see cref="WriteOnlyAttribute"/>
/// markers contradict the field's actual type. Cached after the first walk,
/// so repeated schedules of the same job type pay only a
/// <see cref="ConcurrentDictionary{TKey, TValue}"/> lookup.
///
/// <para>
/// Warnings only — never throws. The job runs whether the contract is met or
/// not; the warning surfaces the mismatch in the log so the author can fix
/// it without crashing the play session.
/// </para>
/// </summary>
internal static class JobLayoutCache
{
    private static readonly ConcurrentDictionary<Type, byte> s_Validated = new();

    public static void EnsureValidated<TJob>() where TJob : struct
    {
        Type t = typeof(TJob);
        if (s_Validated.ContainsKey(t))
            return;

        ValidateLayoutOnce(t);
        s_Validated.TryAdd(t, 1);
    }

    private static void ValidateLayoutOnce(Type jobType)
    {
        FieldInfo[] fields = jobType.GetFields(BindingFlags.Public | BindingFlags.Instance);
        for (int i = 0; i < fields.Length; ++i)
        {
            FieldInfo f = fields[i];
            bool isReadOnly = f.IsDefined(typeof(ReadOnlyAttribute), inherit: false);
            bool isWriteOnly = f.IsDefined(typeof(WriteOnlyAttribute), inherit: false);

            if (!isReadOnly && !isWriteOnly)
                continue;

            if (isReadOnly && isWriteOnly)
            {
                Log.Warn($"[Job] {jobType.Name}.{f.Name}: cannot be both [ReadOnly] and [WriteOnly]; one of the markers will be ignored.");
                continue;
            }

            Type fieldType = f.FieldType;

            if (isWriteOnly && IsRecognizedReadSource(fieldType))
            {
                Log.Warn($"[Job] {jobType.Name}.{f.Name}: [WriteOnly] on a read-source type {FormatTypeName(fieldType)}; the marker contradicts the type.");
            }

            if (isReadOnly && IsRecognizedWriteSink(fieldType))
            {
                Log.Warn($"[Job] {jobType.Name}.{f.Name}: [ReadOnly] on a write-sink type {FormatTypeName(fieldType)}; the marker contradicts the type.");
            }

            if (isReadOnly && IsMutableNativeContainer(fieldType))
            {
                Log.Warn($"[Job] {jobType.Name}.{f.Name}: [ReadOnly] on mutable {FormatTypeName(fieldType)}; consider {FormatTypeName(fieldType)}.ReadOnly via AsReadOnly() to skip per-access safety checks.");
            }

            if (isWriteOnly && IsMutableNativeContainer(fieldType))
            {
                Log.Warn($"[Job] {jobType.Name}.{f.Name}: [WriteOnly] on mutable {FormatTypeName(fieldType)}; consider {FormatTypeName(fieldType)}.WriteOnly via AsWriteOnly() to lock out reads at the type level.");
            }
        }
    }

    private static bool IsRecognizedReadSource(Type t) => IsNativeArrayReadOnlyView(t);

    private static bool IsRecognizedWriteSink(Type t)
    {
        if (t == typeof(EntityCommandBuffer.ParallelWriter))
            return true;
        return IsNativeArrayWriteOnlyView(t);
    }

    private static bool IsMutableNativeContainer(Type t)
        => t.IsGenericType && t.GetGenericTypeDefinition() == typeof(NativeArray<>);

    private static bool IsNativeArrayReadOnlyView(Type t)
        => IsNestedViewOfNativeArray(t, "ReadOnly");

    private static bool IsNativeArrayWriteOnlyView(Type t)
        => IsNestedViewOfNativeArray(t, "WriteOnly");

    private static bool IsNestedViewOfNativeArray(Type t, string expectedName)
    {
        if (!t.IsNested || t.Name != expectedName)
            return false;
        Type? declaring = t.DeclaringType;
        if (declaring == null || !declaring.IsGenericType)
            return false;
        return declaring.GetGenericTypeDefinition() == typeof(NativeArray<>);
    }

    private static string FormatTypeName(Type t)
    {
        if (!t.IsGenericType)
            return t.Name;

        string baseName = t.Name;
        int tickIndex = baseName.IndexOf('`');
        if (tickIndex >= 0)
            baseName = baseName.Substring(0, tickIndex);

        return baseName;
    }
}
