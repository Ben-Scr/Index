using Index.Coroutines;
using Index.Interop;
using System.Threading;
using System.Threading.Tasks;

namespace Index;

public abstract class SceneSystem
{
    public Scene Scene { get; private set; } = new();

    public bool Enabled
    {
        get => !string.IsNullOrEmpty(Scene.Name)
            && InternalCalls.Scene_IsSceneSystemEnabled(Scene.Name, GetType().Name);
        set
        {
            if (!string.IsNullOrEmpty(Scene.Name))
                InternalCalls.Scene_SetSceneSystemEnabled(Scene.Name, GetType().Name, value);
        }
    }

    protected Entity Create(string? name = null) => Entity.Create(name);
    protected Entity Create(string? name, Entity? parent) => Entity.Create(name, parent);
    protected Entity Create(Entity source) => Entity.Create(source);
    protected Entity Instantiate(Entity prefabOrSource) => Entity.Instantiate(prefabOrSource);
    protected Entity Instantiate(Entity prefabOrSource, Entity? parent) => Entity.Instantiate(prefabOrSource, parent);
    protected Entity Instantiate(Entity prefabOrSource, Vector3 position, float rotation = 0.0f, Entity? parent = null) => Entity.Instantiate(prefabOrSource, position, rotation, parent);

    protected void Print(object? obj) => Log.Print(obj);

    internal void _SetSceneName(string sceneName)
    {
        Scene = new Scene { Name = sceneName };
    }

    public QueryBuilder<TComponent> Query<TComponent>()
        where TComponent : Component, new()
        => Scene.Query<TComponent>();

    public QueryBuilder<T1, T2> Query<T1, T2>()
        where T1 : Component, new()
        where T2 : Component, new()
        => Scene.Query<T1, T2>();

    public QueryBuilder<T1, T2, T3> Query<T1, T2, T3>()
        where T1 : Component, new()
        where T2 : Component, new()
        where T3 : Component, new()
        => Scene.Query<T1, T2, T3>();

    public QueryBuilder<T1, T2, T3, T4> Query<T1, T2, T3, T4>()
        where T1 : Component, new()
        where T2 : Component, new()
        where T3 : Component, new()
        where T4 : Component, new()
        => Scene.Query<T1, T2, T3, T4>();

    public QueryBuilder<T1, T2, T3, T4, T5> Query<T1, T2, T3, T4, T5>()
        where T1 : Component, new()
        where T2 : Component, new()
        where T3 : Component, new()
        where T4 : Component, new()
        where T5 : Component, new()
        => Scene.Query<T1, T2, T3, T4, T5>();

    public QueryBuilder<T1, T2, T3, T4, T5, T6> Query<T1, T2, T3, T4, T5, T6>()
        where T1 : Component, new()
        where T2 : Component, new()
        where T3 : Component, new()
        where T4 : Component, new()
        where T5 : Component, new()
        where T6 : Component, new()
        => Scene.Query<T1, T2, T3, T4, T5, T6>();

    public QueryBuilder<T1, T2, T3, T4, T5, T6, T7> Query<T1, T2, T3, T4, T5, T6, T7>()
        where T1 : Component, new()
        where T2 : Component, new()
        where T3 : Component, new()
        where T4 : Component, new()
        where T5 : Component, new()
        where T6 : Component, new()
        where T7 : Component, new()
        => Scene.Query<T1, T2, T3, T4, T5, T6, T7>();

    public QueryBuilder<T1, T2, T3, T4, T5, T6, T7, T8> Query<T1, T2, T3, T4, T5, T6, T7, T8>()
        where T1 : Component, new()
        where T2 : Component, new()
        where T3 : Component, new()
        where T4 : Component, new()
        where T5 : Component, new()
        where T6 : Component, new()
        where T7 : Component, new()
        where T8 : Component, new()
        => Scene.Query<T1, T2, T3, T4, T5, T6, T7, T8>();


    private CancellationTokenSource? m_CoroutineCts;
    private bool m_CoroutineCtsTerminated;

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

    public virtual void OnEnable() { }
    public virtual void OnDisable() { }

    public virtual void OnApplicationPaused() { }
    public virtual void OnApplicationQuit() { }
    public virtual void OnFocusChanged(bool focused) { }
}
