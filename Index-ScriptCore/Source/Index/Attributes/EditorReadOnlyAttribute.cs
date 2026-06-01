using System;

namespace Index;

/// <summary>Marks a field or property as read-only in the inspector; equivalent to <c>[ShowInEditor(readOnly: true)]</c> but usable on auto-shown public fields.</summary>
[AttributeUsage(AttributeTargets.Field | AttributeTargets.Property)]
public sealed class EditorReadOnlyAttribute : Attribute
{
}
