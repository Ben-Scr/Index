using System;
using System.Threading;
using System.Threading.Tasks;
using Index.Components;
using Index.Coroutines;

namespace Index;

/// <summary>Base class for user C# scripts; also acts as a Component so GetComponent/AddComponent/HasComponent work uniformly.</summary>
public abstract class EntityScript : Component
{
    // Throws if entity has no Transform2D; use Entity.Transform (nullable) for tag entities.
    public Transform2D Transform => Entity.Transform
        ?? throw new InvalidOperationException(
            $"EntityScript '{GetType().Name}' accessed `Transform` on an entity without a Transform2D. " +
            "Probe `Entity.Transform` (nullable) when the script may run on tag entities.");

    private CancellationTokenSource? m_CoroutineCts;
    private bool m_CoroutineCtsTerminated;

    internal void _SetEntityID(ulong id)
    {
        Entity = new Entity(id);
    }

    protected Entity? FindEntityByName(string name) => Entity.FindByName(name);

    protected T? GetComponent<T>() where T : Component, new() => Entity.GetComponent<T>();
    protected object? GetComponent(string componentOrScriptName) => Entity.GetComponent(componentOrScriptName);
    protected EntityScript? GetScript(string scriptName) => Entity.GetScript(scriptName);
    protected T? AddComponent<T>() where T : Component, new() => Entity.AddComponent<T>();
    protected bool RemoveComponent<T>() where T : Component, new() => Entity.RemoveComponent<T>();

    protected bool AddNativeComponent<T>() where T : unmanaged, IComponent => Entity.AddNativeComponent<T>();
    protected unsafe ref T GetNativeComponent<T>() where T : unmanaged, IComponent => ref Entity.GetNativeComponent<T>();


    protected Entity Create(string? name = null) => Entity.Create(name);
    protected Entity Create(Entity source) => Entity.Create(source);
    protected Entity Instantiate(Entity prefabOrSource) => Entity.Instantiate(prefabOrSource);
    protected Entity Instantiate(Entity prefabOrSource, Vector3 position, float rotation = 0.0f, Transform2D? parent = null) => Entity.Instantiate(prefabOrSource, position, rotation, parent);

    protected void Print(object? obj)
    {
        Log.Info(obj?.ToString() ?? "null");
    }

    /// <summary>Token cancelled before OnDestroy(); pass to external async work for automatic teardown.</summary>
    protected CancellationToken DestroyToken
    {
        get
        {
            if (m_CoroutineCtsTerminated)
                return new CancellationToken(canceled: true);
            return (m_CoroutineCts ??= new CancellationTokenSource()).Token;
        }
    }

    /// <summary>Fire-and-forget coroutine entry point. Swallows OperationCanceledException on destroy; logs other exceptions.</summary>
    protected void RunCoroutine(Func<Task> coroutine)
    {
        CancellationToken token = DestroyToken;
        CancellationToken previous = CoroutineContext.CurrentToken.Value;
        CoroutineContext.CurrentToken.Value = token;
        try
        {
            _ = ObserveCoroutine(coroutine());
        }
        finally
        {
            CoroutineContext.CurrentToken.Value = previous;
        }
    }

    private static async Task ObserveCoroutine(Task task)
    {
        try { await task; }
        catch (OperationCanceledException) { }
        catch (Exception ex) { Log.Error($"[Coroutine] {ex}"); }
    }

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

    public virtual void OnEnable() { }
    public virtual void OnDisable() { }

    public virtual void OnCollisionEnter2D(Collision2D collision) { }
    public virtual void OnCollisionStay2D(Collision2D collision) { }
    public virtual void OnCollisionExit2D(Collision2D collision) { }
}
