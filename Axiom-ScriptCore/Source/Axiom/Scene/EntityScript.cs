using System;
using System.Threading;
using System.Threading.Tasks;
using Axiom.Coroutines;

namespace Axiom;

/// <summary>
/// Base class for user C# scripts. Subclass and override lifecycle callbacks.
/// </summary>
public abstract class EntityScript
{
    public Entity Entity { get; internal set; } = Entity.Invalid;
    public Transform2D Transform => Entity.Transform;

    // ── Coroutine destroy lifetime ───────────────────────────────
    // Lazy-initialised so scripts that never use coroutines pay zero cost.
    // Once cancelled, m_CoroutineCtsTerminated stays true so subsequent
    // DestroyToken accesses always return a pre-cancelled token — awaits
    // started after destroy short-circuit through GetResult immediately.
    private CancellationTokenSource? m_CoroutineCts;
    private bool m_CoroutineCtsTerminated;

    internal void _SetEntityID(ulong id)
    {
        Entity = new Entity(id);
    }

    protected Entity? FindEntityByName(string name) => Entity.FindByName(name);

    protected T? GetComponent<T>() where T : Component, new() => Entity.GetComponent<T>();
    protected T? AddComponent<T>() where T : Component, new() => Entity.AddComponent<T>();
    protected bool RemoveComponent<T>() where T : Component, new() => Entity.RemoveComponent<T>();

    protected Entity Create(string name = "") => Entity.Create(name);
    protected Entity Create(Entity source) => Entity.Create(source);
    protected Entity Instantiate(Entity prefabOrSource) => Entity.Instantiate(prefabOrSource);

    /// <summary>
    /// Cancellation token tied to this script's lifetime. Cancelled
    /// immediately before OnDestroy() runs. Pass it to any external async
    /// work you start so the work tears itself down with the script.
    /// </summary>
    protected CancellationToken DestroyToken
    {
        get
        {
            if (m_CoroutineCtsTerminated)
                return new CancellationToken(canceled: true);
            return (m_CoroutineCts ??= new CancellationTokenSource()).Token;
        }
    }

    /// <summary>Resume on the engine's next Update tick.</summary>
    protected WaitForNextFrame WaitForNextFrame()
        => new WaitForNextFrame(DestroyToken);

    /// <summary>Resume after <paramref name="frameCount"/> Update ticks.</summary>
    protected WaitForFrames WaitForFrames(int frameCount)
        => new WaitForFrames(frameCount, DestroyToken);

    /// <summary>
    /// Resume after the given number of seconds (scaled by Time.TimeScale,
    /// matching Time.DeltaTime). Use a separate helper if you need
    /// realtime — not yet provided.
    /// </summary>
    protected WaitForSeconds WaitForSeconds(float seconds)
        => new WaitForSeconds(seconds, DestroyToken);

    /// <summary>Resume on the engine's next FixedUpdate tick.</summary>
    protected WaitForFixedUpdate WaitForFixedUpdate()
        => new WaitForFixedUpdate(DestroyToken);

    /// <summary>Resume the first frame the predicate returns true.</summary>
    protected WaitUntil WaitUntil(Func<bool> predicate)
        => new WaitUntil(predicate, DestroyToken);

    /// <summary>
    /// Recommended fire-and-forget entry point for `async Task` coroutines.
    /// Wraps the task so OperationCanceledException is silently swallowed
    /// (the expected outcome on script destroy) and other exceptions are
    /// logged instead of escaping to TaskScheduler.UnobservedTaskException.
    /// </summary>
    /// <example>
    /// <code>
    /// public override void OnStart() => RunCoroutine(BombSequence);
    /// async Task BombSequence() {
    ///     await WaitForSeconds(3.0f);
    ///     Entity.Destroy();
    /// }
    /// </code>
    /// </example>
    protected void RunCoroutine(Func<Task> coroutine)
    {
        _ = ObserveCoroutine(coroutine());
    }

    private static async Task ObserveCoroutine(Task task)
    {
        try { await task; }
        catch (OperationCanceledException) { }
        catch (Exception ex) { Log.Error($"[Coroutine] {ex}"); }
    }

    /// <summary>
    /// Cancel any pending coroutines owned by this script. Called by
    /// ScriptInstanceManager.InvokeOnDestroy immediately before the user's
    /// OnDestroy override runs, and as belt-and-braces from
    /// UnloadUserAssembly on hot reload.
    /// </summary>
    internal void _CancelPendingCoroutines()
    {
        var cts = m_CoroutineCts;
        m_CoroutineCtsTerminated = true;
        m_CoroutineCts = null;
        if (cts == null) return;
        try { cts.Cancel(); }
        finally { cts.Dispose(); }
    }

    public virtual void OnAwake() { }
    public virtual void OnStart() { }
    public virtual void OnUpdate() { }
    public virtual void OnFixedUpdate() { }
    public virtual void OnDestroy() { }

    public virtual void OnApplicationStart() { }
    public virtual void OnApplicationPaused() { }
    public virtual void OnApplicationQuit() { }
    public virtual void OnFocusChanged(bool focused) { }
    public virtual void OnLogMessage(string message) { }

    public virtual void OnKeyDown(KeyCode key) { }
    public virtual void OnKeyUp(KeyCode key) { }
    public virtual void OnMouseDown(MouseButton button) { }
    public virtual void OnMouseUp(MouseButton button) { }
    public virtual void OnMouseScroll(float delta) { }
    public virtual void OnMouseMove(Vector2 position) { }

    public virtual void OnBeforeSceneLoaded(Scene scene) { }
    public virtual void OnSceneLoaded(Scene scene) { }
    public virtual void OnBeforeSceneUnloaded(Scene scene) { }
    public virtual void OnSceneUnloaded(Scene scene) { }

    public virtual void OnEnable()  { }
    public virtual void OnDisable() { }

    public virtual void OnCollisionEnter2D(Collision2D collision) { }
    public virtual void OnCollisionStay2D(Collision2D collision) { }
    public virtual void OnCollisionExit2D(Collision2D collision) { }
}
