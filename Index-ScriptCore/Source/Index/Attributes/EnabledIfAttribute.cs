using System;

namespace Index;

/// <summary>Gates a field's inspector row on another field's value. When <see cref="ExpectedValue"/> is null, any truthy value enables the row.</summary>
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
