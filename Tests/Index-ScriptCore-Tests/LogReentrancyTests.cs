using System;
using Index;
using Xunit;

namespace IndexScriptCoreTests;

/// <summary>
/// Pins the boundary-safety contract of <c>Log</c>. Every native→managed entry point in
/// ScriptInstanceManager catches user-script exceptions and reports them via <c>Log.*</c>.
/// <c>Log.Write</c> re-raises the public <c>Log.LogMessage</c> event, which itself runs user
/// code — so a throwing log subscriber must NOT propagate out of <c>Log.*</c>, or it would
/// unwind across the reverse-pinvoke boundary and crash the process (the exact failure the
/// per-entry-point guards exist to prevent). See <c>Log.RaiseLogMessage</c>.
/// </summary>
public class LogReentrancyTests
{
    [Fact]
    public void LogDoesNotPropagateAThrowingSubscriber()
    {
        Action<string> bad = _ => throw new InvalidOperationException("boom");
        Log.LogMessage += bad;
        try
        {
            // Must be swallowed: a throw here would escape the boundary guard's own Log.Error call.
            Log.Error("triggering message");
            Log.Info("triggering message");
        }
        finally
        {
            Log.LogMessage -= bad;
        }
    }

    [Fact]
    public void LogDeliversMessageToSubscriber()
    {
        string? captured = null;
        Action<string> capture = msg => captured = msg;
        Log.LogMessage += capture;
        try
        {
            Log.Warn("hello-log");
        }
        finally
        {
            Log.LogMessage -= capture;
        }

        Assert.Equal("hello-log", captured);
    }

    [Fact]
    public void LogIsSafeWithNoNativeBindingAndNoSubscribers()
    {
        // The headless test harness never binds native logging; Log.* must no-op, not deref a null fn pointer.
        Log.Trace("trace");
        Log.Info("info");
        Log.Warn("warn");
        Log.Error("error");
    }
}
