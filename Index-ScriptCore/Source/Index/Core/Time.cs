using Index.Interop;

namespace Index;
public static class Time
{
    public static float DeltaTime => InternalCalls.Application_GetDeltaTime();
    public static float UnscaledDeltaTime => InternalCalls.Application_GetUnscaledDeltaTime();

    /// <summary>
    /// Interval in seconds between FixedUpdate ticks. Settable from scripts;
    /// the engine clamps to [1/240, 1] and warns on a non-positive value
    /// (which would otherwise disable FixedUpdate). Lowering it raises physics
    /// fidelity at higher CPU cost; raising it does the inverse.
    /// </summary>
    public static float FixedDeltaTime
    {
        get => InternalCalls.Application_GetFixedDeltaTime();
        set => InternalCalls.Application_SetFixedDeltaTime(value);
    }

    public static float FixedUnscaledDeltaTime => InternalCalls.Application_GetFixedUnscaledDeltaTime();

    public static float ElapsedTime => InternalCalls.Application_GetElapsedTime();

    /// <summary>Time since the game started (engine init excluded). Scales with TimeScale —
    /// at 2x scale this advances twice as fast as wallclock. In editor preview, resets to
    /// zero on each play-mode entry.</summary>
    public static float TimeSinceStartup => InternalCalls.Time_GetTimeSinceStartup();

    /// <summary>Time since the game started (engine init excluded). Wallclock — ignores
    /// TimeScale and pause. In editor preview, resets to zero on each play-mode entry.</summary>
    public static float RealtimeSinceStartup => InternalCalls.Time_GetRealtimeSinceStartup();

    public static float TimeScale
    {
        get => InternalCalls.Application_GetTimeScale();
        set => InternalCalls.Application_SetTimeScale(value);
    }

    /// <summary>Monotonic frame index since process start. Advances once per Update tick.</summary>
    public static int FrameCount => InternalCalls.Time_GetFrameCount();
}
