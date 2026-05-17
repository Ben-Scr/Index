using System;

namespace Index.Jobs;

/// <summary>
/// Marks a job struct field as a write-target inside <c>Execute</c>. The
/// scheduler reflects on the job type the first time it is scheduled and
/// emits a warning via <see cref="Log.Warn(string)"/> if the field's type
/// contradicts the marker (for example, a read-source view such as
/// <c>NativeArray&lt;T&gt;.ReadOnly</c>).
///
/// <para>
/// The canonical pairings are
/// <c>EntityCommandBuffer.ParallelWriter</c> (write-only by construction)
/// and <c>NativeArray&lt;T&gt;.WriteOnly</c> (obtained via
/// <c>NativeArray&lt;T&gt;.AsWriteOnly()</c>). The latter exposes a
/// set-only API so accidental reads do not compile.
/// </para>
/// </summary>
[AttributeUsage(AttributeTargets.Field, AllowMultiple = false, Inherited = false)]
public sealed class WriteOnlyAttribute : Attribute
{
}
