using System;
using System.Runtime.CompilerServices;
using System.Threading;

namespace Axiom.Coroutines;

internal enum WaitKind
{
    Frames,
    Seconds,
    FixedUpdate,
    Condition,
}

/// <summary>
/// Awaiter struct shared by coroutine wait instructions. Public
/// because user assemblies must be able to call IsCompleted / OnCompleted
/// / GetResult through the compiler-generated state machine.
/// </summary>
public struct CoroutineAwaiter : INotifyCompletion
{
    private readonly WaitKind m_Kind;
    private readonly float m_Seconds;
    private readonly int m_Frames;
    private readonly Func<bool>? m_Predicate;
    private readonly CancellationToken m_Token;

    internal CoroutineAwaiter(WaitKind kind, float seconds, int frames, Func<bool>? predicate, CancellationToken token)
    {
        m_Kind = kind;
        m_Seconds = seconds;
        m_Frames = frames;
        m_Predicate = predicate;
        m_Token = token;
    }

    /// <summary>
    /// True only when the script's destroy token already fired before the
    /// await began (e.g. awaiting inside OnDestroy). The await completes
    /// inline and GetResult immediately throws OCE — no scheduler work.
    /// </summary>
    public bool IsCompleted => m_Token.IsCancellationRequested;

    public void OnCompleted(Action continuation)
    {
        switch (m_Kind)
        {
            case WaitKind.Frames:
                CoroutineScheduler.RegisterFrame(continuation, m_Token, m_Frames);
                break;
            case WaitKind.Seconds:
                CoroutineScheduler.RegisterSeconds(continuation, m_Token, m_Seconds);
                break;
            case WaitKind.FixedUpdate:
                CoroutineScheduler.RegisterFixedUpdate(continuation, m_Token);
                break;
            case WaitKind.Condition:
                CoroutineScheduler.RegisterCondition(continuation, m_Token, m_Predicate!);
                break;
        }
    }

    public void GetResult() => m_Token.ThrowIfCancellationRequested();
}

internal static class CoroutineContext
{
    internal static readonly AsyncLocal<CancellationToken> CurrentToken = new();

    internal static CancellationToken Resolve(CancellationToken token)
        => token.CanBeCanceled ? token : CurrentToken.Value;
}

/// <summary>Awaitable used as <c>await new WaitForNextFrame()</c>.</summary>
public readonly struct WaitForNextFrame
{
    private readonly CancellationToken m_Token;
    public WaitForNextFrame(CancellationToken token = default) { m_Token = CoroutineContext.Resolve(token); }
    public CoroutineAwaiter GetAwaiter()
        => new CoroutineAwaiter(WaitKind.Frames, 0f, 1, null, m_Token);
}

/// <summary>Awaitable used as <c>await new WaitForFrames(count)</c>.</summary>
public readonly struct WaitForFrames
{
    private readonly int m_Frames;
    private readonly CancellationToken m_Token;
    public WaitForFrames(int frames, CancellationToken token = default) { m_Frames = frames; m_Token = CoroutineContext.Resolve(token); }
    public CoroutineAwaiter GetAwaiter()
        => new CoroutineAwaiter(WaitKind.Frames, 0f, m_Frames, null, m_Token);
}

/// <summary>Awaitable used as <c>await new WaitForSeconds(seconds)</c>.</summary>
public readonly struct WaitForSeconds
{
    private readonly float m_Seconds;
    private readonly CancellationToken m_Token;
    public WaitForSeconds(float seconds, CancellationToken token = default) { m_Seconds = seconds; m_Token = CoroutineContext.Resolve(token); }
    public CoroutineAwaiter GetAwaiter()
        => new CoroutineAwaiter(WaitKind.Seconds, m_Seconds, 0, null, m_Token);
}

/// <summary>Awaitable used as <c>await new WaitForFixedUpdate()</c>.</summary>
public readonly struct WaitForFixedUpdate
{
    private readonly CancellationToken m_Token;
    public WaitForFixedUpdate(CancellationToken token = default) { m_Token = CoroutineContext.Resolve(token); }
    public CoroutineAwaiter GetAwaiter()
        => new CoroutineAwaiter(WaitKind.FixedUpdate, 0f, 0, null, m_Token);
}

/// <summary>Awaitable used as <c>await new WaitUntil(predicate)</c>.</summary>
public readonly struct WaitUntil
{
    private readonly Func<bool> m_Predicate;
    private readonly CancellationToken m_Token;
    public WaitUntil(Func<bool> predicate, CancellationToken token = default) { m_Predicate = predicate; m_Token = CoroutineContext.Resolve(token); }
    public CoroutineAwaiter GetAwaiter()
        => new CoroutineAwaiter(WaitKind.Condition, 0f, 0, m_Predicate, m_Token);
}
