namespace Index;

public abstract class GlobalSystem
{
    protected void Print(object? obj) => Log.Print(obj);

    public virtual void OnInitialize() { }
    public virtual void OnUpdate() { }
    public virtual void OnFixedUpdate() { }
    public virtual void OnEnable() { }
    public virtual void OnDisable() { }

    public virtual void OnApplicationPaused() { }
    public virtual void OnApplicationQuit() { }
    public virtual void OnFocusChanged(bool focused) { }

}
