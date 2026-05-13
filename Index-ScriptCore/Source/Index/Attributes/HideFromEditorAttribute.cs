using System;

namespace Index;
/// <summary>
/// Marks a field or property to be hidden from the Index Editor inspector,
/// even if it would normally be visible (e.g. public fields).
/// </summary>
[AttributeUsage(AttributeTargets.Field | AttributeTargets.Property)]
public class HideFromEditorAttribute : Attribute
{
}
