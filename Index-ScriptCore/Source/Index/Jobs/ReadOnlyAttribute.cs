using System;

namespace Index.Jobs;

/// <summary>Marks a job struct field as read-only; the scheduler warns if the type contradicts this. Prefer <c>NativeArray&lt;T&gt;.AsReadOnly()</c> in hot loops to skip per-access <c>Volatile.Read</c>.</summary>
[AttributeUsage(AttributeTargets.Field, AllowMultiple = false, Inherited = false)]
public sealed class ReadOnlyAttribute : Attribute
{
}
