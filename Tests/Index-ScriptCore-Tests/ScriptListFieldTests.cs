using System;
using System.Collections;
using System.Collections.Generic;
using System.Linq;
using System.Reflection;
using Xunit;

namespace IndexScriptCoreTests;

/// <summary>
/// Pins the C# side of the script-component list-field wire: type classification
/// (<c>MapFieldType</c>) and the value round-trip (<c>FormatFieldValue</c> →
/// <c>ParseFieldValue</c>) for <c>List&lt;T&gt;</c> / <c>T[]</c>. The single-string
/// container codec must match the editor's DecodeScriptList exactly, so the
/// escaping cases below are the contract the native side relies on.
/// Reached via reflection so the interop file is not modified for testing.
/// </summary>
public class ScriptListFieldTests
{
    private static readonly Type s_Type = ResolveType();

    private static Type ResolveType()
    {
        Assembly asm = typeof(Index.Vector2).Assembly;
        Type? type = asm.GetType("Index.Interop.ScriptInstanceManager")
                     ?? asm.GetTypes().FirstOrDefault(t => t.Name == "ScriptInstanceManager");
        Assert.True(type is not null, "Index.Interop.ScriptInstanceManager not found.");
        return type!;
    }

    private static MethodInfo Method(string name, params Type[] argTypes)
    {
        MethodInfo? m = s_Type.GetMethod(name,
            BindingFlags.NonPublic | BindingFlags.Static, binder: null, types: argTypes, modifiers: null);
        Assert.True(m is not null, $"ScriptInstanceManager.{name}({string.Join(", ", argTypes.Select(t => t.Name))}) not found — update ScriptListFieldTests.");
        return m!;
    }

    private static string MapFieldType(Type t) =>
        (string)Method("MapFieldType", typeof(Type)).Invoke(null, new object[] { t })!;

    private static string Format(Type t, object? val) =>
        (string)Method("FormatFieldValue", typeof(Type), typeof(object)).Invoke(null, new object?[] { t, val })!;

    private static object? Parse(Type t, string s) =>
        Method("ParseFieldValue", typeof(Type), typeof(string)).Invoke(null, new object?[] { t, s });

    // ── Type classification ───────────────────────────────────────────

    [Fact]
    public void SupportedListsMapToList()
    {
        Assert.Equal("list", MapFieldType(typeof(List<int>)));
        Assert.Equal("list", MapFieldType(typeof(List<float>)));
        Assert.Equal("list", MapFieldType(typeof(List<string>)));
        Assert.Equal("list", MapFieldType(typeof(List<bool>)));
        Assert.Equal("list", MapFieldType(typeof(List<Index.Vector2>)));
        Assert.Equal("list", MapFieldType(typeof(int[])));
        // Reference-element lists are supported (rendered with a per-row picker).
        Assert.Equal("list", MapFieldType(typeof(List<Index.Entity>)));
    }

    [Fact]
    public void UnsupportedCollectionsAreNotLists()
    {
        Assert.Equal("unsupported", MapFieldType(typeof(List<List<int>>)));   // nested
        Assert.Equal("unsupported", MapFieldType(typeof(int[,])));            // multi-rank
        Assert.Equal("unsupported", MapFieldType(typeof(Dictionary<int, int>)));
    }

    // ── Value round-trip ──────────────────────────────────────────────

    [Fact]
    public void IntList_RoundTrips()
    {
        var src = new List<int> { 1, 2, -3, 42 };
        string wire = Format(typeof(List<int>), src);
        // Each element is prefixed by a '\n' marker (see EncodeListString).
        Assert.Equal("\n1\n2\n-3\n42", wire);

        var back = (List<int>)Parse(typeof(List<int>), wire)!;
        Assert.Equal(src, back);
    }

    [Fact]
    public void FloatList_RoundTrips()
    {
        var src = new List<float> { 0.5f, -1.25f, 3f };
        var back = (List<float>)Parse(typeof(List<float>), Format(typeof(List<float>), src))!;
        Assert.Equal(src, back);
    }

    [Fact]
    public void IntArray_RoundTrips()
    {
        var src = new[] { 10, 20, 30 };
        var back = (int[])Parse(typeof(int[]), Format(typeof(int[]), src))!;
        Assert.Equal(src, back);
    }

    [Fact]
    public void Vector2List_RoundTrips()
    {
        var src = new List<Index.Vector2> { new(1, 2), new(-3.5f, 4) };
        var back = (List<Index.Vector2>)Parse(typeof(List<Index.Vector2>), Format(typeof(List<Index.Vector2>), src))!;
        Assert.Equal(src.Count, back.Count);
        for (int i = 0; i < src.Count; i++)
        {
            Assert.Equal(src[i].X, back[i].X);
            Assert.Equal(src[i].Y, back[i].Y);
        }
    }

    [Fact]
    public void StringList_WithDelimiterAndEscapeChars_RoundTrips()
    {
        // Element strings that contain the container delimiter (newline) and the
        // escape char (backslash) must survive the codec losslessly.
        var src = new List<string> { "plain", "has\nnewline", "back\\slash", "both\\\nmix", "" };
        var back = (List<string>)Parse(typeof(List<string>), Format(typeof(List<string>), src))!;
        Assert.Equal(src, back);
    }

    [Fact]
    public void EmptyList_EncodesEmpty_AndDecodesEmpty()
    {
        Assert.Equal("", Format(typeof(List<int>), new List<int>()));
        var back = (List<int>)Parse(typeof(List<int>), "")!;
        Assert.Empty(back);
    }

    [Fact]
    public void SingleEmptyElement_IsDistinctFromEmptyList()
    {
        // The codec must distinguish [] (wire "") from [""] (wire "\n"); otherwise
        // "+ Add"-ing a blank reference/string row collapses to zero elements on
        // the next round-trip (the bug fix #1 addresses).
        var one = new List<string> { "" };
        string wire = Format(typeof(List<string>), one);
        Assert.NotEqual("", wire);

        var back = (List<string>)Parse(typeof(List<string>), wire)!;
        Assert.Single(back);
        Assert.Equal("", back[0]);
    }

    [Fact]
    public void NullList_FormatsEmpty()
    {
        Assert.Equal("", Format(typeof(List<int>), null));
    }

    // Note: the malformed-element fallback (parse failure → default(T)) is not
    // unit-tested here because ParseFieldValue's catch path calls Log.Warn, which
    // dispatches through unbound native interop and faults the headless test host.
    // The behavior is exercised in the editor where the binding is live.

    [Fact]
    public void SingleElementList_RoundTrips()
    {
        var back = (List<int>)Parse(typeof(List<int>), Format(typeof(List<int>), new List<int> { 7 }))!;
        Assert.Equal(new List<int> { 7 }, back);
    }
}
