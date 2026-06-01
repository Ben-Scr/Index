using System;

namespace Index;

/// <summary>Hides a field/property/struct/class from the inspector. On a type: all its fields are treated as unsupported; a composite only becomes unsupported when every non-ignored sub-field is.</summary>
[AttributeUsage(AttributeTargets.Field | AttributeTargets.Property | AttributeTargets.Struct | AttributeTargets.Class)]
public class EditorIgnoreAttribute : Attribute
{
}
