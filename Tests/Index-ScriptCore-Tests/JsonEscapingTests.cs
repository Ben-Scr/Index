using System;
using System.Linq;
using System.Reflection;
using Xunit;

namespace IndexScriptCoreTests;

/// <summary>
/// Pins the behavior of the private <c>ScriptInstanceManager.EscapeJson(string)</c>
/// helper that builds the inspector field JSON sent across the native boundary.
/// Reached via reflection so the (currently in-flux) interop file is not modified.
/// </summary>
public class JsonEscapingTests
{
    private static readonly MethodInfo? s_EscapeJson = ResolveEscapeJson();

    private static MethodInfo? ResolveEscapeJson()
    {
        // Any public ScriptCore type anchors us to the right assembly.
        Assembly asm = typeof(Index.Vector2).Assembly;
        Type? type = asm.GetType("Index.Interop.ScriptInstanceManager")
                     ?? asm.GetTypes().FirstOrDefault(t => t.Name == "ScriptInstanceManager");
        return type?.GetMethod(
            "EscapeJson",
            BindingFlags.NonPublic | BindingFlags.Static,
            binder: null,
            types: new[] { typeof(string) },
            modifiers: null);
    }

    private static string Escape(string s)
    {
        Assert.True(
            s_EscapeJson is not null,
            "ScriptInstanceManager.EscapeJson(string) was not found — it was renamed or moved. Update JsonEscapingTests.");
        return (string)s_EscapeJson!.Invoke(null, new object[] { s })!;
    }

    [Fact]
    public void Escapes_Backslash_Quote_Newline_CarriageReturn()
    {
        Assert.Equal("a\\\"b", Escape("a\"b"));
        Assert.Equal("a\\\\b", Escape("a\\b"));
        Assert.Equal("a\\nb", Escape("a\nb"));
        Assert.Equal("a\\rb", Escape("a\rb"));
    }

    // CHARACTERIZATION OF A KNOWN BUG (audit finding):
    // EscapeJson does not escape tab or other control characters (< 0x20), so a
    // script field value containing them produces invalid JSON. The asserts below
    // pin the CURRENT (incorrect) behavior. When EscapeJson is fixed to emit \t and
    // \uXXXX, this test will fail — that is the signal to update it to assert the
    // corrected output (e.g. "a\\tb" and "a\\u0000b").
    [Fact]
    public void DoesNotEscapeTabOrControlChars_KnownBug()
    {
        Assert.Equal("a\tb", Escape("a\tb")); // raw tab survives; should become "a\\tb"
        Assert.Equal("a\0b", Escape("a\0b")); // raw NUL survives; should become "a\\u0000b"
    }
}
