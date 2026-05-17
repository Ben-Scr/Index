using System;

namespace Index.Jobs;

/// <summary>
/// Marks a job struct field as read-only inside <c>Execute</c>. The scheduler
/// reflects on the job type the first time it is scheduled and emits a
/// warning via <see cref="Log.Warn(string)"/> if the field's type contradicts
/// the marker (for example, a write-sink type such as
/// <c>EntityCommandBuffer.ParallelWriter</c>).
///
/// <para>
/// For zero-overhead reads inside the hot loop, prefer declaring the field
/// as <c>NativeArray&lt;T&gt;.ReadOnly</c> obtained via
/// <c>NativeArray&lt;T&gt;.AsReadOnly()</c>. The view captures the raw
/// pointer + length once and skips the per-access <c>Volatile.Read</c> that
/// the parent <see cref="Index.Collections.Native.NativeArray{T}"/> indexer
/// performs for disposal safety.
/// </para>
/// </summary>
[AttributeUsage(AttributeTargets.Field, AllowMultiple = false, Inherited = false)]
public sealed class ReadOnlyAttribute : Attribute
{
}
