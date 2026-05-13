using System;

namespace Index;

/// <summary>
/// Gates a field's editor inspector row on the value of another field on
/// the same object. The row stays visible but is rendered disabled
/// (greyed-out, edits ignored) whenever the named field's current value
/// does not equal the expected value.
///
/// The expected value is matched against the other field's editor wire
/// representation — bool, integer, float, string, and enum fields all
/// work; pass a literal value:
///   <code>
///   public bool UseGravity;
///
///   [ShowInEditor, EnabledIf(nameof(UseGravity), true)]
///   public Vector2 Gravity;
///   </code>
///
/// When <see cref="ExpectedValue"/> is null (the default), the gate
/// passes for any "truthy" value of the field (non-zero / non-empty /
/// true). This is the common case — "enable this field when the boolean
/// up there is on".
///
/// Mirrors the native C++ <c>PropertyMetadata::EnabledIfFn</c> path so
/// behaviour is identical between built-in components and C# scripts.
/// </summary>
[AttributeUsage(AttributeTargets.Field | AttributeTargets.Property)]
public sealed class EnabledIfAttribute : Attribute
{
    public string FieldName { get; }
    public object? ExpectedValue { get; }

    public EnabledIfAttribute(string fieldName)
    {
        FieldName = fieldName;
        ExpectedValue = null;
    }

    public EnabledIfAttribute(string fieldName, object expectedValue)
    {
        FieldName = fieldName;
        ExpectedValue = expectedValue;
    }
}
