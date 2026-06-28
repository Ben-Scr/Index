using Index.Coroutines;
using System.Threading;
using System.Threading.Tasks;

namespace Index;

public abstract class GlobalSystem
{
    protected void Print(object? obj) => Log.Print(obj);

    protected Entity Create(string? name = null) => Entity.Create(name);
    protected Entity Create(string? name, Entity? parent) => Entity.Create(name, parent);
    protected Entity Create(Entity source) => Entity.Create(source);
    protected Entity Instantiate(Entity prefabOrSource) => Entity.Instantiate(prefabOrSource);
    protected Entity Instantiate(Entity prefabOrSource, Entity? parent) => Entity.Instantiate(prefabOrSource, parent);
    protected Entity Instantiate(Entity prefabOrSource, Vector3 position, float rotation = 0.0f, Entity? parent = null) => Entity.Instantiate(prefabOrSource, position, rotation, parent);
    protected void Destroy(Entity entity) => Entity.Destroy(entity);

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

    public virtual void OnInitialize() { }
    public virtual void OnUpdate() { }
    public virtual void OnFixedUpdate() { }
    public virtual void OnEnable() { }
    public virtual void OnDisable() { }

    public virtual void OnApplicationPaused() { }
    public virtual void OnApplicationQuit() { }
    public virtual void OnFocusChanged(bool focused) { }

}
