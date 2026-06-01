using System;

namespace Index.Jobs;

/// <summary>Marks a job struct field as write-only; the scheduler warns if the field's type contradicts this (e.g. a ReadOnly view).</summary>
[AttributeUsage(AttributeTargets.Field, AllowMultiple = false, Inherited = false)]
public sealed class WriteOnlyAttribute : Attribute
{
}
