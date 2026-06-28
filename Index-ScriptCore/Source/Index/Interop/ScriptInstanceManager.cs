using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Runtime.Loader;
using System.Threading;
using Index;
using Index.Coroutines;

namespace Index.Interop;
/// <summary>
/// Resolves Index-ScriptCore from the default context to prevent duplicate type loading.
/// </summary>
internal class ScriptAssemblyLoadContext : AssemblyLoadContext
{
    private readonly Assembly _coreAssembly;
    private readonly AssemblyDependencyResolver? _resolver;
    private readonly string? _userAssemblyDir;

    public ScriptAssemblyLoadContext(Assembly coreAssembly, string? userAssemblyPath = null)
        : base("IndexUserScripts", isCollectible: true)
    {
        _coreAssembly = coreAssembly;
        if (userAssemblyPath != null)
        {
            _userAssemblyDir = System.IO.Path.GetDirectoryName(
                System.IO.Path.GetFullPath(userAssemblyPath));

            try { _resolver = new AssemblyDependencyResolver(userAssemblyPath); }
            catch (Exception ex)
            {
                Log.Warn($"[ScriptLoader] AssemblyDependencyResolver failed: {ex.Message}");
                _resolver = null;
            }
        }

        // Register Resolving event as a last-resort fallback.
        // This fires when Load() returns null AND the Default context also fails.
        this.Resolving += OnResolving;
    }

    protected override Assembly? Load(AssemblyName name)
    {
        if (name.Name == _coreAssembly.GetName().Name)
            return _coreAssembly;

        // Try .deps.json resolver (resolves NuGet packages from cache or local copy)
        if (_resolver != null)
        {
            string? resolvedPath = _resolver.ResolveAssemblyToPath(name);
            if (resolvedPath != null)
                return LoadFromAssemblyPath(resolvedPath);
        }

        // Probe the user assembly's output directory
        Assembly? probed = ProbeDirectory(_userAssemblyDir, name.Name);
        if (probed != null) return probed;

        return null;
    }

    private Assembly? OnResolving(AssemblyLoadContext context, AssemblyName name)
    {
        // Last-resort: re-probe directory and NuGet cache
        Log.Warn($"[ScriptLoader] Resolving fallback for: {name.Name}");

        Assembly? probed = ProbeDirectory(_userAssemblyDir, name.Name);
        if (probed != null) return probed;

        // Check NuGet global packages cache
        if (name.Name != null && name.Version != null)
        {
            string? nugetDir = System.Environment.GetEnvironmentVariable("NUGET_PACKAGES");
            if (string.IsNullOrEmpty(nugetDir))
                nugetDir = System.IO.Path.Combine(
                    System.Environment.GetFolderPath(System.Environment.SpecialFolder.UserProfile),
                    ".nuget", "packages");

            string packageDir = System.IO.Path.Combine(
                nugetDir, name.Name.ToLowerInvariant(), name.Version.ToString());

            if (System.IO.Directory.Exists(packageDir))
            {
                string[] tfms = { "net9.0", "net8.0", "net7.0", "net6.0", "netstandard2.1", "netstandard2.0" };
                foreach (string tfm in tfms)
                {
                    string dllPath = System.IO.Path.Combine(packageDir, "lib", tfm, name.Name + ".dll");
                    if (System.IO.File.Exists(dllPath))
                        return LoadFromAssemblyPath(dllPath);
                }
            }
        }

        Log.Error($"[ScriptLoader] Failed to resolve: {name.FullName}");
        return null;
    }

    private Assembly? ProbeDirectory(string? dir, string? assemblyName)
    {
        if (dir == null || assemblyName == null) return null;
        string candidate = System.IO.Path.Combine(dir, assemblyName + ".dll");
        if (System.IO.File.Exists(candidate))
            return LoadFromAssemblyPath(candidate);
        return null;
    }
}

internal static class ScriptInstanceManager
{
    // [UnmanagedCallersOnly] entry points may be dispatched from worker threads; ConcurrentDictionary keeps all ops safe.
    private static readonly ConcurrentDictionary<int, ScriptInstanceData> s_Instances = new();
    // (entityID, Type) -> handle secondary index for O(1) per-frame GetComponent<TScript> lookups.
    private static readonly ConcurrentDictionary<(ulong EntityID, Type Type), int> s_InstancesByType = new();
    private static readonly ConcurrentDictionary<int, SceneSystem> s_SceneSystems = new();
    private static readonly ConcurrentDictionary<int, GlobalSystem> s_GlobalSystems = new();
    // DataAsset instances are keyed by their asset GUID (their stable identity), not a minted handle.
    private static readonly ConcurrentDictionary<ulong, object> s_DataAssets = new();
    // Incremented from any thread that creates an instance; ++ on a plain int is
    // a read-modify-write race that can hand the same handle to two threads.
    private static int s_NextHandle = 0;

    private static Assembly? s_CoreAssembly;
    private static Assembly? s_UserAssembly;
    private static AssemblyLoadContext? s_UserLoadContext;
    private static readonly ConcurrentDictionary<string, ScriptClassInfo?> s_ClassCache = new();

    private struct ScriptInstanceData
    {
        public EntityScript Instance;
        public MethodInfo? StartMethod;
        public MethodInfo? UpdateMethod;
        public bool HasStarted;
        public bool HasAwoken;
    }

    private static readonly ConcurrentDictionary<int, byte> s_SceneSystemAwoken = new();

    private class ScriptClassInfo
    {
        public Type Type = null!;
        public bool IsScript;
        public bool IsComponent;
        public bool IsSceneSystem;
        public bool IsGlobalSystem;
        public bool IsDataAsset;
        public MethodInfo? StartMethod;
        public MethodInfo? UpdateMethod;

        // Lazily populated by EnsureInvokableMethods. Public void methods the inspector "On Click/Value Changed" list can target; keyed by name for O(1) dispatch.
        public Dictionary<string, MethodInfo>? InvokableMethodsByName;
        public List<string>? InvokableMethodNames;
        public List<byte>? InvokableMethodArgKinds;
    }

    // Mirrors InspectorEventArgKind in C++. Stays as constants rather than
    // a real enum to keep the wire byte values explicit — adding kinds
    // here without renumbering would silently break already-saved scenes.
    private const byte k_ArgKindVoid      = 0;
    private const byte k_ArgKindBool      = 1;
    private const byte k_ArgKindInt       = 2;
    private const byte k_ArgKindFloat     = 3;
    private const byte k_ArgKindDouble    = 4;
    private const byte k_ArgKindString    = 5;
    private const byte k_ArgKindVec2      = 6;
    private const byte k_ArgKindColor     = 7;
    private const byte k_ArgKindEntityRef = 8;

    private static byte ResolveArgKind(Type t)
    {
        if (t == typeof(bool))    return k_ArgKindBool;
        if (t == typeof(int)
         || t == typeof(short)
         || t == typeof(byte)
         || t == typeof(sbyte)
         || t == typeof(uint)
         || t == typeof(long)
         || t == typeof(ushort)) return k_ArgKindInt;
        if (t == typeof(float))   return k_ArgKindFloat;
        if (t == typeof(double))  return k_ArgKindDouble;
        if (t == typeof(string))  return k_ArgKindString;
        if (t == typeof(Vector2)) return k_ArgKindVec2;
        if (t == typeof(Color))   return k_ArgKindColor;
        if (t == typeof(Entity))  return k_ArgKindEntityRef;
        return 0xFF; // unsupported — caller filters out
    }

    internal static void SetCoreAssembly(Assembly assembly)
    {
        s_CoreAssembly = assembly;
    }

    private static List<ScriptInstanceData> SnapshotInstances()
    {
        return new List<ScriptInstanceData>(s_Instances.Values);
    }

    internal static EntityScript? GetScriptInstance(ulong entityID, string className)
    {
        if (entityID == 0 || string.IsNullOrWhiteSpace(className))
            return null;

        ScriptClassInfo? classInfo = ResolveClassByName(className);
        if (classInfo != null
            && s_InstancesByType.TryGetValue((entityID, classInfo.Type), out int cachedHandle)
            && s_Instances.TryGetValue(cachedHandle, out var cachedData))
        {
            return cachedData.Instance;
        }

        foreach (var data in SnapshotInstances())
        {
            EntityScript instance = data.Instance;
            if (instance.Entity == null || instance.Entity.ID != entityID)
                continue;

            Type type = instance.GetType();
            if (string.Equals(type.Name, className, StringComparison.Ordinal)
                || string.Equals(type.FullName, className, StringComparison.Ordinal))
            {
                return instance;
            }
        }

        return null;
    }

    private static ScriptClassInfo? ResolveClassByName(string className)
    {
        if (s_ClassCache.TryGetValue(className, out ScriptClassInfo? info) && info != null && info.IsScript)
            return info;
        // Scan all cache entries by short name to handle the typeof(T).Name
        // case (e.g. "TutorialScript") when the cache key was "MyNs.TutorialScript".
        foreach (var kv in s_ClassCache)
        {
            ScriptClassInfo? candidate = kv.Value;
            if (candidate == null || !candidate.IsScript) continue;
            if (string.Equals(candidate.Type.Name, className, StringComparison.Ordinal)
                || string.Equals(candidate.Type.FullName, className, StringComparison.Ordinal))
            {
                return candidate;
            }
        }
        return null;
    }

    private static void DispatchToScripts(Action<EntityScript> invoke, string eventName)
    {
        foreach (var data in SnapshotInstances())
        {
            try { invoke(data.Instance); }
            catch (Exception ex) { Console.Error.WriteLine($"Exception in {eventName}: {ex}"); }
        }
    }

    private static void DispatchToSceneSystems(Action<SceneSystem> invoke, string eventName)
    {
        foreach (var system in new List<SceneSystem>(s_SceneSystems.Values))
        {
            try { invoke(system); }
            catch (Exception ex) { Console.Error.WriteLine($"Exception in {eventName}: {ex}"); }
        }
    }

    private static void DispatchToGlobalSystems(Action<GlobalSystem> invoke, string eventName)
    {
        foreach (var system in new List<GlobalSystem>(s_GlobalSystems.Values))
        {
            try { invoke(system); }
            catch (Exception ex) { Console.Error.WriteLine($"Exception in {eventName}: {ex}"); }
        }
    }


    private static Scene SceneFromName(string name) => new Scene { Name = name };

    private static unsafe string PtrToString(byte* value)
    {
        return Marshal.PtrToStringUTF8((IntPtr)value) ?? "";
    }

    // ── Input / Window / Scene / Application event raisers ──────────────
    // Each [UnmanagedCallersOnly] method below is a reverse-pinvoke entry point: a
    // managed exception that unwinds past it into the native caller is FATAL (the
    // CLR fail-fasts the whole process). These raisers fan out to public
    // `event Action<>`s that user scripts subscribe to, and those events have no
    // per-subscriber guard — so a bug in a game handler (e.g. an NRE in OnMouseDown)
    // must be caught and logged here, never allowed to escape. The try/catch is
    // inlined per method rather than via a shared Action helper to stay
    // allocation-free in the per-frame input path (RaiseMouseMove). Log.* is itself
    // boundary-safe (Log.RaiseLogMessage swallows throwing subscribers).

    [UnmanagedCallersOnly]
    public static void RaiseApplicationStart()
    {
        try
        {
            Application.RaiseApplicationStart();
            DispatchToScripts(script => script.OnApplicationStart(), nameof(EntityScript.OnApplicationStart));
        }
        catch (Exception ex) { Log.Error($"Exception in Application.OnStart handler: {ex}"); }
    }

    [UnmanagedCallersOnly]
    public static void RaiseApplicationPaused()
    {
        try
        {
            Application.RaiseApplicationPaused();
            DispatchToScripts(script => script.OnApplicationPaused(), nameof(EntityScript.OnApplicationPaused));
            DispatchToSceneSystems(system => system.OnApplicationPaused(), nameof(SceneSystem.OnApplicationPaused));
            DispatchToGlobalSystems(system => system.OnApplicationPaused(), nameof(GlobalSystem.OnApplicationPaused));
        }
        catch (Exception ex) { Log.Error($"Exception in Application.OnPaused handler: {ex}"); }
    }

    [UnmanagedCallersOnly]
    public static void RaiseApplicationQuit()
    {
        try
        {
            Application.RaiseApplicationQuit();
            DispatchToScripts(script => script.OnApplicationQuit(), nameof(EntityScript.OnApplicationQuit));
            DispatchToSceneSystems(system => system.OnApplicationQuit(), nameof(SceneSystem.OnApplicationQuit));
            DispatchToGlobalSystems(system => system.OnApplicationQuit(), nameof(GlobalSystem.OnApplicationQuit));
        }
        catch (Exception ex) { Log.Error($"Exception in Application.OnQuit handler: {ex}"); }
    }

    [UnmanagedCallersOnly]
    public static void RaiseFocusChanged(int focused)
    {
        try
        {
            bool isFocused = focused != 0;
            Window.RaiseFocusChanged(isFocused);
            DispatchToScripts(script => script.OnFocusChanged(isFocused), nameof(EntityScript.OnFocusChanged));
            DispatchToSceneSystems(system => system.OnFocusChanged(isFocused), nameof(SceneSystem.OnFocusChanged));
            DispatchToGlobalSystems(system => system.OnFocusChanged(isFocused), nameof(GlobalSystem.OnFocusChanged));
        }
        catch (Exception ex) { Log.Error($"Exception in Window.FocusChanged handler: {ex}"); }
    }

    [UnmanagedCallersOnly]
    public static void RaiseKeyDown(int key)
    {
        try { Input.RaiseKeyDown((KeyCode)key); }
        catch (Exception ex) { Log.Error($"Exception in Input.KeyDown handler: {ex}"); }
    }

    [UnmanagedCallersOnly]
    public static void RaiseKeyUp(int key)
    {
        try { Input.RaiseKeyUp((KeyCode)key); }
        catch (Exception ex) { Log.Error($"Exception in Input.KeyUp handler: {ex}"); }
    }

    [UnmanagedCallersOnly]
    public static void RaiseEnterChar(uint codepoint)
    {
        if (codepoint > char.MaxValue) return;
        char c = (char)codepoint;
        if (char.IsControl(c)) return;
        try { Input.RaiseEnterChar(c); }
        catch (Exception ex) { Log.Error($"Exception in Input.EnterChar handler: {ex}"); }
    }

    [UnmanagedCallersOnly]
    public static void RaiseMouseDown(int button)
    {
        try { Input.RaiseMouseDown((MouseButton)button); }
        catch (Exception ex) { Log.Error($"Exception in Input.MouseDown handler: {ex}"); }
    }

    [UnmanagedCallersOnly]
    public static void RaiseMouseUp(int button)
    {
        try { Input.RaiseMouseUp((MouseButton)button); }
        catch (Exception ex) { Log.Error($"Exception in Input.MouseUp handler: {ex}"); }
    }

    [UnmanagedCallersOnly]
    public static void RaiseMouseScroll(float delta)
    {
        try { Input.RaiseMouseScroll(delta); }
        catch (Exception ex) { Log.Error($"Exception in Input.MouseScroll handler: {ex}"); }
    }

    [UnmanagedCallersOnly]
    public static void RaiseMouseMove(float x, float y)
    {
        try { Input.RaiseMouseMove(new Vector2(x, y)); }
        catch (Exception ex) { Log.Error($"Exception in Input.MouseMove handler: {ex}"); }
    }

    [UnmanagedCallersOnly]
    public static unsafe void RaiseBeforeSceneLoaded(byte* sceneName)
    {
        try { SceneManager.RaiseBeforeSceneLoaded(PtrToString(sceneName)); }
        catch (Exception ex) { Log.Error($"Exception in SceneManager.BeforeSceneLoaded handler: {ex}"); }
    }

    [UnmanagedCallersOnly]
    public static unsafe void RaiseSceneLoaded(byte* sceneName)
    {
        try { SceneManager.RaiseSceneLoaded(PtrToString(sceneName)); }
        catch (Exception ex) { Log.Error($"Exception in SceneManager.SceneLoaded handler: {ex}"); }
    }

    [UnmanagedCallersOnly]
    public static unsafe void RaiseBeforeSceneUnloaded(byte* sceneName)
    {
        try { SceneManager.RaiseBeforeSceneUnloaded(PtrToString(sceneName)); }
        catch (Exception ex) { Log.Error($"Exception in SceneManager.BeforeSceneUnloaded handler: {ex}"); }
    }

    [UnmanagedCallersOnly]
    public static unsafe void RaiseSceneUnloaded(byte* sceneName)
    {
        try { SceneManager.RaiseSceneUnloaded(PtrToString(sceneName)); }
        catch (Exception ex) { Log.Error($"Exception in SceneManager.SceneUnloaded handler: {ex}"); }
    }

    [UnmanagedCallersOnly]
    public static void RaiseWindowResize()
    {
        try { Window.InvokeResize(); }
        catch (Exception ex) { Log.Error($"Exception in Window.OnResize handler: {ex}"); }
    }

    [UnmanagedCallersOnly]
    public static void RaiseUiEventDispatch()
    {
        Index.UI.UIEventDispatcher.Tick();
    }

    private static void UnloadCurrentUserAssemblyContext()
    {
        if (s_UserLoadContext == null)
            return;

        // Remaining handlers after OnDisable swept them indicate leaks; they would survive ALC.Unload and point into freed code on the next event raise.
        int strayHandlers = SweepUserStaticEventHandlers(SweepScope.AssemblyUnload);
        if (strayHandlers > 0)
        {
            Log.Warn($"{strayHandlers} registered event(s) still active. Register and unregister events using OnEnable() and OnDisable().");
        }

        // Drop cached managed Component instances *before* unloading: each entry in
        // Entity.s_ManagedComponentStore references a user-defined type, which roots
        // the AssemblyLoadContext and would prevent it from unloading on hot reload.
        Entity.ClearManagedComponentStore();

        var weakContext = new WeakReference(s_UserLoadContext, trackResurrection: false);
        s_UserLoadContext.Unload();
        s_UserLoadContext = null;
        s_UserAssembly = null;
        ReleaseFieldJsonBuffer();

        GC.Collect();
        GC.WaitForPendingFinalizers();
        GC.Collect();

        if (weakContext.IsAlive)
            Log.Warn("[ScriptLoader] User assembly load context is still alive after unload; lingering references may delay full cleanup.");
    }

    // Called after the editor reloads the pre-play snapshot; GlobalSystem subscriptions are preserved because they survive play mode.
    [UnmanagedCallersOnly]
    public static void OnPlayModeExited()
    {
        int strayHandlers = SweepUserStaticEventHandlers(SweepScope.PlayModeExit);
        if (strayHandlers > 0)
        {
            Log.Warn($"{strayHandlers} registered event(s) still active. Register and unregister events using OnEnable() and OnDisable().");
        }

        // Drop cached managed Component wrappers (Slot, Button, ...) so they don't carry
        // runtime-mutated field values into the next play session. s_ManagedComponentStore is
        // keyed by the stable entity UUID, which the scene reload restores identically — without
        // this, a plain Stop (no assembly reload) leaves the previous session's wrappers cached,
        // and GetComponent<T>() returns them WITHOUT re-applying the authored PendingFieldValues
        // (those are only applied on first wrapper creation). The native reload already destroyed
        // the entities and re-seeded authored values, so the next fetch rebuilds wrappers cleanly.
        // (EntityScript instances revert correctly because they ARE torn down each session.)
        // invalidate:false — the reloaded edit-scene instances already hold freshly-resolved wrappers
        // for live entities; invalidating them here would orphan restored component refs (shows "(None)").
        Entity.ClearManagedComponentStore(invalidate: false);
    }

    // Native→managed: the engine destroyed an entity outside any C# path
    // (Scene::DestroyEntityInternal / ClearEntities / scene unload), so drop the
    // cached managed Component wrappers for its UUID. Without this they leak in
    // Entity.s_ManagedComponentStore until the next assembly unload. Called per
    // entity on teardown, so the empty-store fast path lives in
    // Entity.OnNativeEntityDestroyed.
    [UnmanagedCallersOnly]
    public static void NotifyEntityDestroyed(ulong entityID)
    {
        try { Entity.OnNativeEntityDestroyed(entityID); }
        catch (Exception ex) { Log.Error($"NotifyEntityDestroyed failed: {ex}"); }
    }

    // AssemblyUnload: strip all user-assembly handlers. PlayModeExit: strip only EntityScript/SceneSystem handlers; GlobalSystem subscriptions remain valid.
    private enum SweepScope
    {
        AssemblyUnload,
        PlayModeExit,
    }

    // Walks every static event in every loaded engine-side assembly and removes user-assembly subscribers matching the scope policy.
    // Reads/writes the implicit backing field directly (same name as event) so cleanup is atomic. Closure types are unwound to their user-facing outer class for PlayModeExit filtering.
    private static int SweepUserStaticEventHandlers(SweepScope scope)
    {
        Assembly? userAssembly = s_UserAssembly;
        if (userAssembly == null) return 0;

        Type? entityScriptBase = null;
        Type? gameSystemBase = null;
        if (scope == SweepScope.PlayModeExit && s_CoreAssembly != null)
        {
            entityScriptBase = s_CoreAssembly.GetType("Index.EntityScript");
            gameSystemBase = s_CoreAssembly.GetType("Index.SceneSystem");
        }

        int totalRemoved = 0;

        const BindingFlags eventFlags =
            BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Static;
        const BindingFlags fieldFlags =
            BindingFlags.NonPublic | BindingFlags.Static;

        foreach (Assembly assembly in AppDomain.CurrentDomain.GetAssemblies())
        {
            if (assembly == userAssembly) continue;

            string? name = assembly.GetName().Name;
            if (name == null) continue;
            if (name.StartsWith("System.", StringComparison.Ordinal)) continue;
            if (name.StartsWith("Microsoft.", StringComparison.Ordinal)) continue;
            if (name == "System" || name == "mscorlib" || name == "netstandard") continue;

            Type[] types;
            try { types = assembly.GetTypes(); }
            catch (ReflectionTypeLoadException ex)
            {
                // Partial type loads from a failed dependent — sweep what did load.
                types = Array.FindAll(ex.Types, t => t != null)!;
            }
            catch
            {
                continue;
            }

            foreach (Type type in types)
            {
                if (type == null) continue;

                EventInfo[] events;
                try { events = type.GetEvents(eventFlags); }
                catch { continue; }

                foreach (EventInfo evt in events)
                {
                    if (evt.AddMethod == null || !evt.AddMethod.IsStatic) continue;

                    // Implicit event backing field has the same name as the event; explicit add/remove accessors have no backing field to walk.
                    FieldInfo? field = type.GetField(evt.Name, fieldFlags);
                    if (field == null) continue;
                    if (!typeof(Delegate).IsAssignableFrom(field.FieldType)) continue;

                    Delegate? current;
                    try { current = field.GetValue(null) as Delegate; }
                    catch { continue; }
                    if (current == null) continue;

                    Delegate? cleaned = null;
                    int removedFromThisEvent = 0;
                    foreach (Delegate invocation in current.GetInvocationList())
                    {
                        if (ShouldRemoveInvocation(invocation, scope, userAssembly, entityScriptBase, gameSystemBase))
                        {
                            ++removedFromThisEvent;
                            continue;
                        }
                        cleaned = Delegate.Combine(cleaned, invocation);
                    }

                    if (removedFromThisEvent > 0)
                    {
                        try { field.SetValue(null, cleaned); }
                        catch (Exception ex)
                        {
                            Log.Warn($"[ScriptLoader] Failed to clear stale subscribers on {type.FullName}.{evt.Name}: {ex.Message}");
                            continue;
                        }
                        totalRemoved += removedFromThisEvent;
                    }
                }
            }
        }

        return totalRemoved;
    }

    private static bool ShouldRemoveInvocation(
        Delegate invocation,
        SweepScope scope,
        Assembly userAssembly,
        Type? entityScriptBase,
        Type? gameSystemBase)
    {
        MethodInfo method = invocation.Method;
        Type? declaring = method.DeclaringType;
        if (declaring == null) return false;
        if (declaring.Module.Assembly != userAssembly) return false;

        // Assembly-unload sweep: the whole user assembly is going away,
        // anything pointing into it must die.
        if (scope == SweepScope.AssemblyUnload) return true;

        // PlayModeExit: walk past compiler-generated closure types to find the user-facing outer class for base-type filtering.
        Type? owner = declaring;
        while (owner != null && owner.IsNested && IsCompilerGenerated(owner))
        {
            owner = owner.DeclaringType;
        }
        if (owner == null) return false;

        if (entityScriptBase != null && entityScriptBase.IsAssignableFrom(owner)) return true;
        if (gameSystemBase != null && gameSystemBase.IsAssignableFrom(owner)) return true;
        return false;
    }

    private static bool IsCompilerGenerated(Type t)
    {
        try
        {
            return Attribute.IsDefined(t, typeof(CompilerGeneratedAttribute), inherit: false);
        }
        catch
        {
            // IsDefined can throw on malformed metadata; treat as non-compiler-generated and stop the closure walk here.
            return false;
        }
    }

    [UnmanagedCallersOnly]
    public static unsafe int CreateScriptInstance(byte* classNamePtr, ulong entityID)
    {
        try
        {
            string className = Marshal.PtrToStringUTF8((IntPtr)classNamePtr)!;
            var classInfo = GetOrCacheClass(className);
            if (classInfo == null || !classInfo.IsScript) return 0;

            var instance = (EntityScript)Activator.CreateInstance(classInfo.Type)!;
            instance._SetEntityID(entityID);

            int handle = Interlocked.Increment(ref s_NextHandle);
            s_Instances[handle] = new ScriptInstanceData
            {
                Instance = instance,
                StartMethod = classInfo.StartMethod,
                UpdateMethod = classInfo.UpdateMethod,
                HasStarted = false,
                HasAwoken = false
            };
            s_InstancesByType[(entityID, classInfo.Type)] = handle;

            return handle;
        }
        catch (Exception ex)
        {
            Log.Error($"CreateScriptInstance failed: {ex}");
            return 0;
        }
    }

    [UnmanagedCallersOnly]
    public static void DestroyScriptInstance(int handle)
    {
        if (!s_Instances.TryRemove(handle, out ScriptInstanceData data))
            return;
        ulong entityID = data.Instance.Entity != null ? data.Instance.Entity.ID : 0;
        if (entityID != 0)
        {
            Type type = data.Instance.GetType();
            s_InstancesByType.TryRemove((entityID, type), out _);
        }
    }

    [UnmanagedCallersOnly]
    public static void InvokeStart(int handle)
    {
        if (!s_Instances.TryGetValue(handle, out var data) || data.HasStarted) return;

        try
        {
            data.Instance.OnStart();
            data.StartMethod?.Invoke(data.Instance, null);
            data.HasStarted = true;
            s_Instances[handle] = data;
        }
        catch (TargetInvocationException ex) { Log.Error($"Exception in OnStart(): {ex.InnerException}"); }
        catch (Exception ex) { Log.Error($"Exception in OnStart(): {ex}"); }
    }

    [UnmanagedCallersOnly]
    public static void InvokeAwake(int handle)
    {
        if (!s_Instances.TryGetValue(handle, out var data) || data.HasAwoken) return;

        try
        {
            data.Instance.OnAwake();
            data.HasAwoken = true;
            s_Instances[handle] = data;
        }
        catch (TargetInvocationException ex) { Log.Error($"Exception in OnAwake(): {ex.InnerException}"); }
        catch (Exception ex) { Log.Error($"Exception in OnAwake(): {ex}"); }
    }

    [UnmanagedCallersOnly]
    public static void InvokeUpdate(int handle)
    {
        if (!s_Instances.TryGetValue(handle, out var data)) return;
        try
        {
            data.Instance.OnUpdate();
            data.UpdateMethod?.Invoke(data.Instance, null);
        }
        catch (TargetInvocationException ex) { Log.Error($"Exception in OnUpdate(): {ex.InnerException}"); }
        catch (Exception ex) { Log.Error($"Exception in OnUpdate(): {ex}"); }
    }

    [UnmanagedCallersOnly]
    public static void InvokeFixedUpdate(int handle)
    {
        if (!s_Instances.TryGetValue(handle, out var data)) return;
        try
        {
            data.Instance.OnFixedUpdate();
        }
        catch (TargetInvocationException ex) { Log.Error($"Exception in OnFixedUpdate(): {ex.InnerException}"); }
        catch (Exception ex) { Log.Error($"Exception in OnFixedUpdate(): {ex}"); }
    }

    [UnmanagedCallersOnly]
    public static void InvokeOnDestroy(int handle)
    {
        if (!s_Instances.TryGetValue(handle, out var data)) return;
        // Cancel coroutines BEFORE user OnDestroy so any pending awaits unwind
        // with OCE rather than racing against the script's teardown logic.
        try { data.Instance._CancelPendingCoroutines(); }
        catch (Exception ex) { Log.Error($"Exception cancelling coroutines: {ex}"); }
        try { data.Instance.OnDestroy(); }
        catch (TargetInvocationException ex) { Log.Error($"Exception in OnDestroy(): {ex.InnerException}"); }
        catch (Exception ex) { Log.Error($"Exception in OnDestroy(): {ex}"); }
    }

    [UnmanagedCallersOnly]
    public static void InvokeOnEnable(int handle)
    {
        if (!s_Instances.TryGetValue(handle, out var data)) return;
        try { data.Instance.OnEnable(); }
        catch (Exception ex) { Log.Error($"Exception in OnEnable(): {ex}"); }
    }

    [UnmanagedCallersOnly]
    public static void InvokeOnDisable(int handle)
    {
        if (!s_Instances.TryGetValue(handle, out var data)) return;
        try { data.Instance.OnDisable(); }
        catch (Exception ex) { Log.Error($"Exception in OnDisable(): {ex}"); }
    }

    [UnmanagedCallersOnly]
    public static void InvokeCollisionEnter2D(int handle, ulong selfEntityId, ulong otherEntityId, ulong entityAId, ulong entityBId, float contactPointX, float contactPointY)
    {
        InvokeCollision2D(handle, selfEntityId, otherEntityId, entityAId, entityBId, new Vector2(contactPointX, contactPointY), CollisionPhase.Enter);
    }

    [UnmanagedCallersOnly]
    public static void InvokeCollisionStay2D(int handle, ulong selfEntityId, ulong otherEntityId, ulong entityAId, ulong entityBId, float contactPointX, float contactPointY)
    {
        InvokeCollision2D(handle, selfEntityId, otherEntityId, entityAId, entityBId, new Vector2(contactPointX, contactPointY), CollisionPhase.Stay);
    }

    [UnmanagedCallersOnly]
    public static void InvokeCollisionExit2D(int handle, ulong selfEntityId, ulong otherEntityId, ulong entityAId, ulong entityBId, float contactPointX, float contactPointY)
    {
        InvokeCollision2D(handle, selfEntityId, otherEntityId, entityAId, entityBId, new Vector2(contactPointX, contactPointY), CollisionPhase.Exit);
    }

    private enum CollisionPhase { Enter, Stay, Exit }

    private static void InvokeCollision2D(int handle, ulong selfEntityId, ulong otherEntityId, ulong entityAId, ulong entityBId, Vector2 contactPoint, CollisionPhase phase)
    {
        if (!s_Instances.TryGetValue(handle, out var data)) return;

        try
        {
            var collision = new Collision2D(selfEntityId, otherEntityId, entityAId, entityBId, contactPoint);
            if (phase == CollisionPhase.Enter)
                data.Instance.OnCollisionEnter2D(collision);
            else if (phase == CollisionPhase.Stay)
                data.Instance.OnCollisionStay2D(collision);
            else
                data.Instance.OnCollisionExit2D(collision);
        }
        catch (Exception ex)
        {
            string callbackName = phase switch
            {
                CollisionPhase.Enter => nameof(EntityScript.OnCollisionEnter2D),
                CollisionPhase.Stay => nameof(EntityScript.OnCollisionStay2D),
                _ => nameof(EntityScript.OnCollisionExit2D)
            };
            Log.Error($"Exception in {callbackName}: {ex}");
        }
    }

    [UnmanagedCallersOnly]
    public static unsafe int ClassExists(byte* classNamePtr)
    {
        string className = Marshal.PtrToStringUTF8((IntPtr)classNamePtr)!;
        return GetOrCacheClass(className)?.IsScript == true ? 1 : 0;
    }

    [UnmanagedCallersOnly]
    public static unsafe int CreateSceneSystemInstance(byte* classNamePtr, byte* sceneNamePtr)
    {
        try
        {
            string className = PtrToString(classNamePtr);
            string sceneName = PtrToString(sceneNamePtr);
            var classInfo = GetOrCacheClass(className);
            if (classInfo == null || !classInfo.IsSceneSystem) return 0;

            var instance = (SceneSystem)Activator.CreateInstance(classInfo.Type)!;
            instance._SetSceneName(sceneName);

            int handle = Interlocked.Increment(ref s_NextHandle);
            s_SceneSystems[handle] = instance;
            return handle;
        }
        catch (Exception ex)
        {
            Log.Error($"CreateSceneSystemInstance failed: {ex}");
            return 0;
        }
    }

    [UnmanagedCallersOnly]
    public static void DestroySceneSystemInstance(int handle)
    {
        s_SceneSystems.TryRemove(handle, out _);
        s_SceneSystemAwoken.TryRemove(handle, out _);
    }

    [UnmanagedCallersOnly]
    public static void InvokeSceneSystemStart(int handle)
    {
        if (!s_SceneSystems.TryGetValue(handle, out var system)) return;
        try { system.OnStart(); }
        catch (Exception ex) { Log.Error($"Exception in SceneSystem.OnStart(): {ex}"); }
    }

    [UnmanagedCallersOnly]
    public static void InvokeSceneSystemAwake(int handle)
    {
        if (!s_SceneSystems.TryGetValue(handle, out var system)) return;
        if (!s_SceneSystemAwoken.TryAdd(handle, 0)) return;
        try { system.OnAwake(); }
        catch (TargetInvocationException ex) { Log.Error($"Exception in SceneSystem.OnAwake(): {ex.InnerException}"); }
        catch (Exception ex) { Log.Error($"Exception in SceneSystem.OnAwake(): {ex}"); }
    }

    [UnmanagedCallersOnly]
    public static void InvokeSceneSystemUpdate(int handle)
    {
        if (!s_SceneSystems.TryGetValue(handle, out var system)) return;
        try { system.OnUpdate(); }
        catch (Exception ex) { Log.Error($"Exception in SceneSystem.OnUpdate(): {ex}"); }
    }

    [UnmanagedCallersOnly]
    public static void InvokeSceneSystemFixedUpdate(int handle)
    {
        if (!s_SceneSystems.TryGetValue(handle, out var system)) return;
        try { system.OnFixedUpdate(); }
        catch (TargetInvocationException ex) { Log.Error($"Exception in SceneSystem.OnFixedUpdate(): {ex.InnerException}"); }
        catch (Exception ex) { Log.Error($"Exception in SceneSystem.OnFixedUpdate(): {ex}"); }
    }

    [UnmanagedCallersOnly]
    public static void InvokeSceneSystemEnable(int handle)
    {
        if (!s_SceneSystems.TryGetValue(handle, out var system)) return;
        try { system.OnEnable(); }
        catch (Exception ex) { Log.Error($"Exception in SceneSystem.OnEnable(): {ex}"); }
    }

    [UnmanagedCallersOnly]
    public static void InvokeSceneSystemDisable(int handle)
    {
        if (!s_SceneSystems.TryGetValue(handle, out var system)) return;
        try { system.OnDisable(); }
        catch (Exception ex) { Log.Error($"Exception in SceneSystem.OnDisable(): {ex}"); }
    }

    [UnmanagedCallersOnly]
    public static void InvokeSceneSystemDestroy(int handle)
    {
        if (!s_SceneSystems.TryGetValue(handle, out var system)) return;
        try { system.OnDestroy(); }
        catch (Exception ex) { Log.Error($"Exception in SceneSystem.OnDestroy(): {ex}"); }
    }

    [UnmanagedCallersOnly]
    public static unsafe int SceneSystemClassExists(byte* classNamePtr)
    {
        string className = PtrToString(classNamePtr);
        return GetOrCacheClass(className)?.IsSceneSystem == true ? 1 : 0;
    }

    [UnmanagedCallersOnly]
    public static unsafe int CreateGlobalSystemInstance(byte* classNamePtr)
    {
        try
        {
            string className = PtrToString(classNamePtr);
            var classInfo = GetOrCacheClass(className);
            if (classInfo == null || !classInfo.IsGlobalSystem) return 0;

            var instance = (GlobalSystem)Activator.CreateInstance(classInfo.Type)!;
            int handle = Interlocked.Increment(ref s_NextHandle);
            s_GlobalSystems[handle] = instance;
            return handle;
        }
        catch (Exception ex)
        {
            Log.Error($"CreateGlobalSystemInstance failed: {ex}");
            return 0;
        }
    }

    [UnmanagedCallersOnly]
    public static void DestroyGlobalSystemInstance(int handle) => s_GlobalSystems.TryRemove(handle, out _);

    [UnmanagedCallersOnly]
    public static void InvokeGlobalSystemInitialize(int handle)
    {
        if (!s_GlobalSystems.TryGetValue(handle, out var system)) return;
        try { system.OnInitialize(); }
        catch (Exception ex) { Log.Error($"Exception in GlobalSystem.OnInitialize(): {ex}"); }
    }

    [UnmanagedCallersOnly]
    public static void InvokeGlobalSystemUpdate(int handle)
    {
        if (!s_GlobalSystems.TryGetValue(handle, out var system)) return;
        try { system.OnUpdate(); }
        catch (Exception ex) { Log.Error($"Exception in GlobalSystem.OnUpdate(): {ex}"); }
    }

    [UnmanagedCallersOnly]
    public static void InvokeGlobalSystemFixedUpdate(int handle)
    {
        if (!s_GlobalSystems.TryGetValue(handle, out var system)) return;
        try { system.OnFixedUpdate(); }
        catch (TargetInvocationException ex) { Log.Error($"Exception in GlobalSystem.OnFixedUpdate(): {ex.InnerException}"); }
        catch (Exception ex) { Log.Error($"Exception in GlobalSystem.OnFixedUpdate(): {ex}"); }
    }

    [UnmanagedCallersOnly]
    public static void InvokeGlobalSystemEnable(int handle)
    {
        if (!s_GlobalSystems.TryGetValue(handle, out var system)) return;
        try { system.OnEnable(); }
        catch (Exception ex) { Log.Error($"Exception in GlobalSystem.OnEnable(): {ex}"); }
    }

    [UnmanagedCallersOnly]
    public static void InvokeGlobalSystemDisable(int handle)
    {
        if (!s_GlobalSystems.TryGetValue(handle, out var system)) return;
        try { system.OnDisable(); }
        catch (Exception ex) { Log.Error($"Exception in GlobalSystem.OnDisable(): {ex}"); }
    }

    [UnmanagedCallersOnly]
    public static unsafe int GlobalSystemClassExists(byte* classNamePtr)
    {
        string className = PtrToString(classNamePtr);
        return GetOrCacheClass(className)?.IsGlobalSystem == true ? 1 : 0;
    }

    // ── DataAsset (appended for binary compat) ──
    // Standalone instances keyed by asset GUID. The native DataAssetManager
    // drives create + field replay and owns the loaded set; these only touch
    // the managed-side instance table.

    [UnmanagedCallersOnly]
    public static unsafe int CreateDataAssetInstance(byte* typeNamePtr, ulong guid)
    {
        try
        {
            string typeName = PtrToString(typeNamePtr);
            var classInfo = GetOrCacheClass(typeName);
            if (classInfo == null || !classInfo.IsDataAsset) return 0;

            var instance = (DataAsset)Activator.CreateInstance(classInfo.Type)!;
            instance._SetUUID(guid);
            s_DataAssets[guid] = instance;
            return 1;
        }
        catch (Exception ex)
        {
            Log.Error($"CreateDataAssetInstance failed: {ex}");
            return 0;
        }
    }

    [UnmanagedCallersOnly]
    public static void DestroyDataAssetInstance(ulong guid) => s_DataAssets.TryRemove(guid, out _);

    [UnmanagedCallersOnly]
    public static unsafe byte* GetDataAssetFields(ulong guid)
    {
        try
        {
            if (!s_DataAssets.TryGetValue(guid, out var instance))
                return NullTerminated("[]");

            return SerializeInstanceFields(instance, typeof(DataAsset));
        }
        catch (Exception ex)
        {
            Log.Error($"GetDataAssetFields failed: {ex.Message}");
            return NullTerminated("[]");
        }
    }

    [UnmanagedCallersOnly]
    public static unsafe void SetDataAssetField(ulong guid, byte* fieldNamePtr, byte* valuePtr)
    {
        try
        {
            if (!s_DataAssets.TryGetValue(guid, out var instance)) return;
            ApplyFieldEdit(instance, fieldNamePtr, valuePtr);
            try { (instance as DataAsset)?.OnValidate(); }
            catch (Exception ex) { Log.Error($"DataAsset.OnValidate() threw: {ex.Message}"); }
        }
        catch (Exception ex)
        {
            Log.Error($"SetDataAssetField failed: {ex.Message}");
        }
    }

    [UnmanagedCallersOnly]
    public static unsafe int DataAssetClassExists(byte* typeNamePtr)
    {
        string typeName = PtrToString(typeNamePtr);
        return GetOrCacheClass(typeName)?.IsDataAsset == true ? 1 : 0;
    }

    // Returns JSON [{"type","menu"}] of every DataAsset subclass in the user assembly, for the editor's Create menu.
    [UnmanagedCallersOnly]
    public static unsafe int GetDataAssetTypesBuffer(byte* outBuffer, int capacity)
    {
        try
        {
            var sb = new System.Text.StringBuilder();
            sb.Append('[');
            bool first = true;

            if (s_UserAssembly != null)
            {
                Type[] types;
                try { types = s_UserAssembly.GetTypes(); }
                catch (ReflectionTypeLoadException ex)
                {
                    var loaded = new List<Type>();
                    foreach (var t in ex.Types) if (t != null) loaded.Add(t);
                    types = loaded.ToArray();
                }

                foreach (var type in types)
                {
                    if (type == null || type.IsAbstract || !type.IsSubclassOf(typeof(DataAsset)))
                        continue;

                    var attr = type.GetCustomAttribute<CreateDataAssetAttribute>();
                    string menu = attr != null && !string.IsNullOrEmpty(attr.MenuPath) ? attr.MenuPath : type.Name;

                    if (!first) sb.Append(',');
                    first = false;
                    sb.Append("{\"type\":\"").Append(EscapeJson(type.Name))
                      .Append("\",\"menu\":\"").Append(EscapeJson(menu)).Append("\"}");
                }
            }

            sb.Append(']');
            return WriteUtf8(sb.ToString(), outBuffer, capacity);
        }
        catch (Exception ex)
        {
            Log.Error($"GetDataAssetTypesBuffer failed: {ex.Message}");
            return WriteUtf8("[]", outBuffer, capacity);
        }
    }

    internal static object? GetDataAssetObject(ulong guid)
        => s_DataAssets.TryGetValue(guid, out var instance) ? instance : null;

    [UnmanagedCallersOnly]
    public static unsafe int LoadUserAssembly(byte* pathPtr)
    {
        try
        {
            string path = Marshal.PtrToStringUTF8((IntPtr)pathPtr)!;

            if (s_UserLoadContext != null)
            {
                // Drop previous dynamic registrations before tearing down
                // the existing assembly load context — same reason
                // UnloadUserAssembly does, just for the reload path.
                DynamicComponentRegistrar.UnregisterAll();

                CancelAllInstanceCoroutines();
                s_Instances.Clear();
                s_InstancesByType.Clear();
                s_SceneSystems.Clear();
                s_SceneSystemAwoken.Clear();
                s_GlobalSystems.Clear();
                s_DataAssets.Clear();
                s_ClassCache.Clear();
                UnloadCurrentUserAssemblyContext();
            }

            s_ClassCache.Clear();

            // Load from bytes to avoid file-locking the DLL on disk
            string fullPath = System.IO.Path.GetFullPath(path);
            s_UserLoadContext = new ScriptAssemblyLoadContext(s_CoreAssembly!, fullPath);
            byte[] assemblyBytes = System.IO.File.ReadAllBytes(fullPath);

            // Load PDB so stack traces have file/line info; without symbols the editor's log-link parser points to this file instead of the user's script.
            System.IO.MemoryStream? symbolsStream = null;
            string pdbPath = System.IO.Path.ChangeExtension(fullPath, ".pdb");
            if (System.IO.File.Exists(pdbPath))
            {
                try
                {
                    symbolsStream = new System.IO.MemoryStream(
                        System.IO.File.ReadAllBytes(pdbPath));
                }
                catch (Exception pdbEx)
                {
                    Log.Warn($"User assembly PDB load failed ({pdbEx.Message}); stack traces will lack source info.");
                    symbolsStream = null;
                }
            }

            s_UserAssembly = s_UserLoadContext.LoadFromStream(
                new System.IO.MemoryStream(assemblyBytes),
                symbolsStream);

            Log.Info($"User assembly loaded: {path}");

            DynamicComponentRegistrar.RegisterAll(s_UserAssembly);
            return 1;
        }
        catch (Exception ex)
        {
            Log.Error($"Failed to load user assembly: {ex.Message}");
            return 0;
        }
    }

    [UnmanagedCallersOnly]
    public static void UnloadUserAssembly()
    {
        // Drop dynamic component registrations BEFORE the ALC unloads so
        // captured DynamicComponentStorage pointers in native callbacks
        // don't outlive the storage that owns them.
        DynamicComponentRegistrar.UnregisterAll();

        CancelAllInstanceCoroutines();
        s_Instances.Clear();
        s_InstancesByType.Clear();
        s_SceneSystems.Clear();
        s_SceneSystemAwoken.Clear();
        s_GlobalSystems.Clear();
        s_DataAssets.Clear();
        s_ClassCache.Clear();
        ReleaseFieldJsonBuffer();

        UnloadCurrentUserAssemblyContext();
    }

    [UnmanagedCallersOnly]
    public static void PumpCoroutinesUpdate(float deltaTime)
    {
        CoroutineScheduler.PumpUpdate(deltaTime);
    }

    [UnmanagedCallersOnly]
    public static void PumpCoroutinesFixedUpdate()
    {
        CoroutineScheduler.PumpFixedUpdate();
    }

    // Cancel all coroutines before unload so captured user-ALC types are released for the GC dance in UnloadCurrentUserAssemblyContext.
    private static void CancelAllInstanceCoroutines()
    {
        foreach (var data in s_Instances.Values)
        {
            try { data.Instance._CancelPendingCoroutines(); }
            catch (Exception ex) { Log.Error($"Exception cancelling coroutines during reload: {ex}"); }
        }
        CoroutineScheduler.Reset();
    }

    // ── Field reflection for [ShowInEditor] ──────────────────────

    private static readonly object s_FieldJsonBufferLock = new();
    private static byte[] s_FieldJsonBuffer = Array.Empty<byte>();
    private static GCHandle s_FieldJsonBufferHandle;

    [UnmanagedCallersOnly]
    public static unsafe byte* GetScriptFields(int handle)
    {
        try
        {
            if (!s_Instances.TryGetValue(handle, out var data))
                return NullTerminated("[]");

            var instance = data.Instance;
            var type = instance.GetType();

            var sb = new System.Text.StringBuilder();
            sb.Append('[');
            bool first = true;

            foreach (var member in CollectEditorMembers(type))
            {
                if (member.DeclaringType == typeof(EntityScript)) continue;
                if (!IsMemberEditorVisible(member)) continue;

                object? val = TryGetMemberValue(member, instance);
                AppendEditorMember(sb, member, val, "", 0, ref first);
            }

            sb.Append(']');
            return NullTerminated(sb.ToString());
        }
        catch (Exception ex)
        {
            Log.Error($"GetScriptFields failed: {ex.Message}");
            return NullTerminated("[]");
        }
    }

    [UnmanagedCallersOnly]
    public static unsafe void SetScriptField(int handle, byte* fieldNamePtr, byte* valuePtr)
    {
        try
        {
            if (!s_Instances.TryGetValue(handle, out var data)) return;
            ApplyFieldEdit(data.Instance, fieldNamePtr, valuePtr);
        }
        catch (Exception ex)
        {
            Log.Error($"SetScriptField failed: {ex.Message}");
        }
    }

    [UnmanagedCallersOnly]
    public static unsafe byte* GetSceneSystemFields(int handle)
    {
        try
        {
            if (!s_SceneSystems.TryGetValue(handle, out var system))
                return NullTerminated("[]");

            return SerializeInstanceFields(system, typeof(SceneSystem));
        }
        catch (Exception ex)
        {
            Log.Error($"GetSceneSystemFields failed: {ex.Message}");
            return NullTerminated("[]");
        }
    }

    [UnmanagedCallersOnly]
    public static unsafe void SetSceneSystemField(int handle, byte* fieldNamePtr, byte* valuePtr)
    {
        try
        {
            if (!s_SceneSystems.TryGetValue(handle, out var system)) return;
            ApplyFieldEdit(system, fieldNamePtr, valuePtr);
        }
        catch (Exception ex)
        {
            Log.Error($"SetSceneSystemField failed: {ex.Message}");
        }
    }

    // Resolve an inspector write. ParseFieldValue returns null for BOTH a parse
    // failure AND an empty reference value (a cleared Texture -> FromAssetUUID(0)
    // == null). The first must be skipped so stale/malformed posts don't clobber
    // data; the second is an explicit "Clear" and must write null. Disambiguate by
    // input: an empty string targeting a reference type is a clear.
    private static bool TryResolveInspectorWrite(Type memberType, string valueStr, out object? value)
    {
        value = ParseFieldValue(memberType, valueStr);
        if (value != null) return true;
        return valueStr.Length == 0 && !memberType.IsValueType;
    }

    // Fields first (faster), then properties; silently skips on miss/parse failure since the editor may post stale names after a script edit.
    private static unsafe void ApplyFieldEdit(object instance, byte* fieldNamePtr, byte* valuePtr)
    {
        string fieldName = Marshal.PtrToStringUTF8((IntPtr)fieldNamePtr) ?? "";
        string valueStr = Marshal.PtrToStringUTF8((IntPtr)valuePtr) ?? "";

        const BindingFlags k_Flags = BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Instance;
        Type type = instance.GetType();

        // Dotted names address a member of a flattened struct/class field ("P.X").
        if (fieldName.IndexOf('.') >= 0)
        {
            ApplyNestedFieldEdit(instance, fieldName, valueStr);
            return;
        }

        var field = type.GetField(fieldName, k_Flags);
        if (field != null)
        {
            if (TryResolveInspectorWrite(field.FieldType, valueStr, out object? parsed))
                field.SetValue(instance, parsed);
            return;
        }

        var property = type.GetProperty(fieldName, k_Flags);
        if (property == null) return;

        // CanWrite alone returns true for init-only and private setters, so check SetMethod.IsPublic explicitly.
        if (!property.CanWrite || property.SetMethod == null || !property.SetMethod.IsPublic)
            return;

        if (!TryResolveInspectorWrite(property.PropertyType, valueStr, out object? parsedProp))
            return;

        try
        {
            property.SetValue(instance, parsedProp);
        }
        catch (TargetInvocationException tie) when (tie.InnerException != null)
        {
            // Surface the underlying validation error from the user's
            // setter (e.g. ArgumentOutOfRangeException) without the
            // reflection wrapper.
            Log.Error($"Inspector write to property '{type.Name}.{fieldName}' threw: {tie.InnerException.Message}");
        }
        catch (Exception ex)
        {
            Log.Error($"Inspector write to property '{type.Name}.{fieldName}' failed: {ex.Message}");
        }
    }

    // Writes a value addressed by a dotted path ("P.X") into a flattened
    // struct/class member. Walks the chain, then writes each link back into its
    // owner so value-type (struct) mutations and lazily-created reference
    // instances along the path persist.
    private static void ApplyNestedFieldEdit(object root, string path, string valueStr)
    {
        const BindingFlags k_Flags = BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Instance;
        string[] segs = path.Split('.');
        if (segs.Length < 2) return;

        var chain = new object[segs.Length];           // chain[i] owns segs[i]; chain[^1] owns the leaf
        var links = new MemberInfo[segs.Length - 1];    // links[i]: chain[i] -> chain[i + 1]
        chain[0] = root;

        for (int i = 0; i < segs.Length - 1; i++)
        {
            MemberInfo? m = GetPathMember(chain[i].GetType(), segs[i], k_Flags);
            if (m == null) return;
            object? next = GetPathMemberValue(m, chain[i]);
            if (next == null)
            {
                try { next = Activator.CreateInstance(PathMemberType(m)); }
                catch { return; }
                if (next == null) return;
            }
            links[i] = m;
            chain[i + 1] = next;
        }

        MemberInfo? leaf = GetPathMember(chain[^1].GetType(), segs[^1], k_Flags);
        if (leaf == null) return;
        if (!TryResolveInspectorWrite(PathMemberType(leaf), valueStr, out object? parsed))
            return;
        if (!TrySetPathMember(leaf, chain[^1], parsed)) return;

        for (int i = segs.Length - 2; i >= 0; i--)
        {
            if (!TrySetPathMember(links[i], chain[i], chain[i + 1])) return;
        }
    }

    private static MemberInfo? GetPathMember(Type t, string name, BindingFlags flags)
        => (MemberInfo?)t.GetField(name, flags) ?? t.GetProperty(name, flags);

    private static Type PathMemberType(MemberInfo m)
        => m is FieldInfo f ? f.FieldType : ((PropertyInfo)m).PropertyType;

    private static object? GetPathMemberValue(MemberInfo m, object owner)
        => m is FieldInfo f ? f.GetValue(owner) : ((PropertyInfo)m).GetValue(owner);

    private static bool TrySetPathMember(MemberInfo m, object owner, object? value)
    {
        if (m is FieldInfo f)
        {
            if (f.IsInitOnly || f.IsLiteral) return false;
            f.SetValue(owner, value);
            return true;
        }
        var p = (PropertyInfo)m;
        if (!p.CanWrite || p.SetMethod == null) return false;
        p.SetValue(owner, value);
        return true;
    }

    private static unsafe byte* SerializeInstanceFields(object instance, Type ignoreBaseType)
    {
        var type = instance.GetType();

        var sb = new System.Text.StringBuilder();
        sb.Append('[');
        bool first = true;

        foreach (var member in CollectEditorMembers(type))
        {
            if (member.DeclaringType == ignoreBaseType) continue;
            if (!IsMemberEditorVisible(member)) continue;

            object? val = TryGetMemberValue(member, instance);
            AppendEditorMember(sb, member, val, "", 0, ref first);
        }

        sb.Append(']');
        return NullTerminated(sb.ToString());
    }

    // Element type of a List<T> or single-rank T[] the inspector can render as
    // a list; null for non-collections or unsupported element types.
    private static Type? GetListElementType(Type t)
    {
        if (t.IsArray && t.GetArrayRank() == 1) return t.GetElementType();
        if (t.IsGenericType && t.GetGenericTypeDefinition() == typeof(List<>))
            return t.GetGenericArguments()[0];
        return null;
    }

    // Base declaring types whose members are skipped when reflecting an object-
    // list element's inline fields (engine plumbing, not the user's data).
    private static readonly Type[] s_ObjectListBaseSkip =
    {
        typeof(object), typeof(Component), typeof(EntityScript),
        typeof(SceneSystem), typeof(GlobalSystem), typeof(DataAsset),
    };

    // Ordered inline sub-fields of an object-list element: writable, editor-visible
    // members declared on the type itself that map to a supported LEAF type. This
    // order is the canonical encode/decode order, so encode, decode, and the
    // itemFields schema all iterate it. Collection sub-fields are excluded (no
    // nested lists in v1) which also breaks recursion on self-referential types.
    private static List<EditorMember> GetObjectListFields(Type elem)
    {
        var result = new List<EditorMember>();
        foreach (var m in CollectEditorMembers(elem))
        {
            if (m.DeclaringType != null && Array.IndexOf(s_ObjectListBaseSkip, m.DeclaringType) >= 0) continue;
            if (!IsMemberEditorVisible(m)) continue;
            if (!m.CanWrite) continue;
            if (GetListElementType(m.ValueType) != null) continue;  // no list/array sub-fields (v1)
            string tag = MapFieldType(m.ValueType);
            if (tag == "unsupported" || tag == "graph") continue;   // leaf scalar/ref/enum/component/dataasset only
            result.Add(m);
        }
        return result;
    }

    // True for a type whose list elements render as an INLINE object (edited
    // field-by-field) rather than a reference. Plain data classes/structs and
    // managed Component data classes qualify; native IComponent structs (which
    // forward to ECS storage) and field-less types do not.
    private static bool IsObjectListable(Type t)
    {
        if (t.IsPrimitive || t.IsEnum || t.IsArray || t.IsPointer) return false;
        if (t == typeof(string)) return false;
        if (t.IsGenericType) return false;
        if (t.IsAbstract || t.IsInterface) return false;
        if (typeof(Delegate).IsAssignableFrom(t)) return false;
        if (t.IsValueType && typeof(Index.Components.IComponent).IsAssignableFrom(t)) return false;
        if (t.Namespace != null && t.Namespace.StartsWith("System", StringComparison.Ordinal)) return false;
        if (!t.IsValueType && t.GetConstructor(Type.EmptyTypes) == null) return false;
        return GetObjectListFields(t).Count > 0;
    }

    // Wire tag for a list element type, or null when it can't be a list item. Leaf
    // scalar/reference types map directly; component-typed or plain data-class
    // elements with inline fields become an "object" (inline) list; a registered
    // native component with no inline fields stays a reference list.
    private static string? MapListItemType(Type elem)
    {
        if (GetListElementType(elem) != null) return null;  // no nested lists
        string tag = MapFieldType(elem);
        // A registered native component (built-in wrapper like Transform2D, or a
        // native struct) forwards to ECS storage, so it can't hold data by value —
        // it stays a reference list. This must win over IsObjectListable.
        if (tag.StartsWith("component:", StringComparison.Ordinal)) return tag;
        if (tag != "unsupported" && tag != "graph") return tag;   // leaf scalar/ref/enum/dataasset
        // Plain data class/struct or a user-defined (non-native) managed component
        // data class -> inline object list.
        if (IsObjectListable(elem)) return "object";
        return null;
    }

    private static string MapFieldType(Type t)
    {
        // Primitve types
        if (t == typeof(float)) return "float";
        if (t == typeof(double)) return "double";
        if (t == typeof(int)) return "int";
        if (t == typeof(short)) return "short";
        if (t == typeof(byte)) return "byte";
        if (t == typeof(long)) return "long";
        if (t == typeof(uint)) return "uint";
        if (t == typeof(ushort)) return "ushort";
        if (t == typeof(sbyte)) return "sbyte";
        if (t == typeof(ulong)) return "ulong";
        if (t == typeof(bool)) return "bool";
        if (t == typeof(string)) return "string";

        // Index-specific types
        if (t == typeof(Color)) return "color";
        if (t == typeof(Entity)) return "entity";
        if (t == typeof(Scene)) return "scene";
        if (t == typeof(Texture)) return "texture";
        if (t == typeof(Audio)) return "audio";
        if (t == typeof(TextureRef)) return "texture";
        if (t == typeof(AudioRef)) return "audio";
        if(t == typeof(Vector2)) return "vector2";
        if (t == typeof(Vector2Int)) return "vector2Int";
        if (t == typeof(Vector3)) return "vector3";
        if (t == typeof(Vector3Int)) return "vector3Int";
        if (t == typeof(Vector4)) return "vector4";
        if (t == typeof(Vector4Int)) return "vector4Int";
        if (t == typeof(Graph)) return "graph";

        if (t.IsEnum)
        {
            // [Flags] enums get their own dispatch type so the editor renders
            // a per-bit checkbox combo instead of a single-selection dropdown.
            return t.GetCustomAttribute<FlagsAttribute>() != null ? "flagenum" : "enum";
        }

        // A DataAsset subclass reference serializes as its asset GUID; the subtype name rides the code so the editor picker can filter to it.
        if (t.IsSubclassOf(typeof(DataAsset)))
            return "dataasset:" + t.Name;

        if (t.IsSubclassOf(typeof(Component)))
        {
            return Entity.TryGetNativeComponentName(t, out string? nativeName)
                ? "component:" + nativeName
                : "unsupported";
        }
        if (t.IsValueType && typeof(Index.Components.IComponent).IsAssignableFrom(t))
        {
            return Entity.TryGetNativeComponentName(t, out string? nativeName)
                ? "component:" + nativeName
                : "unsupported";
        }

        // List<T> / T[] of a supported element type → generic inspector list.
        if (GetListElementType(t) is Type listElem && MapListItemType(listElem) != null)
            return "list";

        return "unsupported";
    }

    private static string FormatFieldValue(Type t, object? val)
    {
        if (val == null) return "";

        //INFO(Ben-Scr): This may causes bugs when sharing projects across different locales
        var ic = System.Globalization.CultureInfo.InvariantCulture;

        // Primitve types
        if (t == typeof(float)) return ((float)val).ToString(ic);
        if (t == typeof(double)) return ((double)val).ToString(ic);
        if (t == typeof(int)) return ((int)val).ToString(ic);
        if (t == typeof(short)) return ((short)val).ToString(ic);
        if (t == typeof(byte)) return ((byte)val).ToString(ic);
        if (t == typeof(long)) return ((long)val).ToString(ic);
        if (t == typeof(uint)) return ((uint)val).ToString(ic);
        if (t == typeof(ushort)) return ((ushort)val).ToString(ic);
        if (t == typeof(sbyte)) return ((sbyte)val).ToString(ic);
        if (t == typeof(ulong)) return ((ulong)val).ToString(ic);
        if (t == typeof(bool)) return (bool)val ? "true" : "false";
        if (t == typeof(string)) return (string)val;


        if (t == typeof(Color))
        {
            var c = (Color)val;
            return $"{c.R.ToString(ic)},{c.G.ToString(ic)},{c.B.ToString(ic)},{c.A.ToString(ic)}";
        }
        if (t == typeof(Entity))
        {
            var entity = (Entity)val;
            if (entity == null || entity == Entity.Invalid) return "0";

            return entity.IsPrefabAsset
                ? "prefab:" + entity.PrefabGUID.ToString(ic)
                : entity.ID.ToString(ic);
        }
        if (t == typeof(Scene))
        {
            var scene = (Scene)val;
            return scene.AssetUUID != 0 ? scene.AssetUUID.ToString(ic) : "";
        }
        if (t == typeof(Texture))
        {
            Texture texture = (Texture)val;
            return texture.UUID != 0 ? texture.UUID.ToString(ic) : "";
        }
        if (t == typeof(Audio))
        {
            Audio audio = (Audio)val;
            return audio.UUID != 0 ? audio.UUID.ToString(ic) : "";
        }
        if (t == typeof(TextureRef))
        {
            TextureRef assetRef = (TextureRef)val;
            return assetRef.UUID != 0 ? assetRef.UUID.ToString(ic) : "";
        }
        if (t == typeof(AudioRef))
        {
            AudioRef assetRef = (AudioRef)val;
            return assetRef.UUID != 0 ? assetRef.UUID.ToString(ic) : "";
        }
        if(t == typeof(Vector2))
        {
            var v = (Vector2)val;
            return $"{v.X.ToString(ic)},{v.Y.ToString(ic)}";
        }
        if (t == typeof(Vector2Int))
        {
            var v = (Vector2Int)val;
            return $"{v.X.ToString(ic)},{v.Y.ToString(ic)}";
        }
        if (t == typeof(Vector3))
        {
            var v = (Vector3)val;
            return $"{v.X.ToString(ic)},{v.Y.ToString(ic)},{v.Z.ToString(ic)}";
        }
        if (t == typeof(Vector3Int))
        {
            var v = (Vector3Int)val;
            return $"{v.X.ToString(ic)},{v.Y.ToString(ic)},{v.Z.ToString(ic)}";
        }
        if (t == typeof(Vector4))
        {
            var v = (Vector4)val;
            return $"{v.X.ToString(ic)},{v.Y.ToString(ic)},{v.Z.ToString(ic)},{v.W.ToString(ic)}";
        }
        if (t == typeof(Vector4Int))
        {
            var v = (Vector4Int)val;
            return $"{v.X.ToString(ic)},{v.Y.ToString(ic)},{v.Z.ToString(ic)},{v.W.ToString(ic)}";
        }
        if (t == typeof(Graph))
            return ((Graph)val).Serialize();

        if (t.IsSubclassOf(typeof(DataAsset)))
            return val is DataAsset dataAsset && dataAsset.UUID != 0 ? dataAsset.UUID.ToString(ic) : "";

        if (t.IsSubclassOf(typeof(Component)))
        {
            var comp = (Component)val;
            if (comp?.Entity == null || comp.Entity == Entity.Invalid) return "";
            string typeName = Entity.TryGetNativeComponentName(t, out string? nativeName)
                ? nativeName!
                : t.Name;
            return comp.Entity.ID.ToString(ic) + ":" + typeName;
        }

        if (t.IsEnum)
        {
            // Enums round-trip as the underlying integer's invariant decimal
            // form. Flag enums are bitwise OR-able integers so the same path
            // works for both single-value and [Flags] enums.
            return Convert.ToInt64(val, ic).ToString(ic);
        }

        // List<T> / T[]: each element formatted by its own type, joined with the
        // newline+backslash codec the editor's list drawer decodes. Object items
        // are two-level: each element's leaf fields are inner-encoded, then the
        // elements are outer-encoded (the outer pass escapes the inner strings).
        if (GetListElementType(t) is Type listElem)
        {
            var parts = new List<string>();
            if (val is System.Collections.IEnumerable en)
            {
                bool isObject = MapListItemType(listElem) == "object";
                var fields = isObject ? GetObjectListFields(listElem) : null;
                foreach (var item in en)
                {
                    if (isObject)
                    {
                        var fieldStrs = new List<string>(fields!.Count);
                        foreach (var f in fields!)
                            fieldStrs.Add(FormatFieldValue(f.ValueType, item == null ? null : TryGetMemberValue(f, item)));
                        parts.Add(EncodeListString(fieldStrs));
                    }
                    else
                    {
                        parts.Add(FormatFieldValue(listElem, item));
                    }
                }
            }
            return EncodeListString(parts);
        }

        return val.ToString() ?? "";
    }

    private static ulong ParseAssetUUID(string value)
    {
        if (string.IsNullOrWhiteSpace(value))
            return 0;

        if (ulong.TryParse(value, System.Globalization.NumberStyles.None,
            System.Globalization.CultureInfo.InvariantCulture, out ulong assetId))
        {
            return assetId;
        }

        return InternalCalls.Asset_GetOrCreateUUIDFromPath(value);
    }

    private static bool MatchesComponentReferenceType(Type componentType, string serializedTypeName)
    {
        if (Entity.TryGetNativeComponentName(componentType, out string? nativeName)
            && string.Equals(nativeName, serializedTypeName, StringComparison.Ordinal))
        {
            return true;
        }

        return string.Equals(componentType.Name, serializedTypeName, StringComparison.Ordinal);
    }

    private static object? ParseFieldValue(Type t, string s)
    {
        var ic = System.Globalization.CultureInfo.InvariantCulture;
        try
        {
            if (t == typeof(float)) return float.Parse(s, ic);
            if (t == typeof(double)) return double.Parse(s, ic);
            if (t == typeof(int)) return int.Parse(s, ic);
            if (t == typeof(short)) return short.Parse(s, ic);
            if (t == typeof(byte)) return byte.Parse(s, ic);
            if (t == typeof(long)) return long.Parse(s, ic);
            if (t == typeof(uint)) return uint.Parse(s, ic);
            if (t == typeof(ushort)) return ushort.Parse(s, ic);
            if (t == typeof(sbyte)) return sbyte.Parse(s, ic);
            if (t == typeof(ulong)) return ulong.Parse(s, ic);
            if (t == typeof(bool)) return s == "true" || s == "True" || s == "1";
            if (t == typeof(string)) return s;
            if (t == typeof(Color))
            {
                var parts = s.Split(',');
                if (parts.Length >= 4)
                    return new Color(
                        float.Parse(parts[0], ic), float.Parse(parts[1], ic),
                        float.Parse(parts[2], ic), float.Parse(parts[3], ic));
                return new Color(1, 1, 1, 1);
            }
            if (t == typeof(Vector2))
            {
                var parts = s.Split(',', StringSplitOptions.TrimEntries);
                if (parts.Length >= 2)
                    return new Vector2(
                        float.Parse(parts[0], ic),
                        float.Parse(parts[1], ic));
                return null;
            }
            if (t == typeof(Vector2Int))
            {
                var parts = s.Split(',', StringSplitOptions.TrimEntries);
                if (parts.Length >= 2)
                    return new Vector2Int(
                        int.Parse(parts[0], ic),
                        int.Parse(parts[1], ic));
                return null;
            }
            if (t == typeof(Vector3))
            {
                var parts = s.Split(',', StringSplitOptions.TrimEntries);
                if (parts.Length >= 3)
                    return new Vector3(
                        float.Parse(parts[0], ic),
                        float.Parse(parts[1], ic),
                        float.Parse(parts[2], ic));
                return null;
            }
            if (t == typeof(Vector3Int))
            {
                var parts = s.Split(',', StringSplitOptions.TrimEntries);
                if (parts.Length >= 3)
                    return new Vector3Int(
                        int.Parse(parts[0], ic),
                        int.Parse(parts[1], ic),
                        int.Parse(parts[2], ic));
                return null;
            }
            if (t == typeof(Vector4))
            {
                var parts = s.Split(',', StringSplitOptions.TrimEntries);
                if (parts.Length >= 4)
                    return new Vector4(
                        float.Parse(parts[0], ic),
                        float.Parse(parts[1], ic),
                        float.Parse(parts[2], ic),
                        float.Parse(parts[3], ic));
                return null;
            }
            if (t == typeof(Vector4Int))
            {
                var parts = s.Split(',', StringSplitOptions.TrimEntries);
                if (parts.Length >= 4)
                    return new Vector4Int(
                        int.Parse(parts[0], ic),
                        int.Parse(parts[1], ic),
                        int.Parse(parts[2], ic),
                        int.Parse(parts[3], ic));
                return null;
            }
            if (t == typeof(Graph))
            {
                // Total parser (never throws / never null) — return it directly.
                return Graph.Deserialize(s);
            }
            if (t == typeof(Entity))
            {
                return Entity.ParseEntityReference(s);
            }
            if (t == typeof(Scene))
            {
                return Scene.FromAssetUUID(ParseAssetUUID(s));
            }
            if (t == typeof(Texture)) return Texture.FromAssetUUID(ParseAssetUUID(s));
            if (t == typeof(Audio)) return Audio.FromAssetUUID(ParseAssetUUID(s));
            if (t == typeof(TextureRef)) return new TextureRef(ParseAssetUUID(s));
            if (t == typeof(AudioRef)) return new AudioRef(ParseAssetUUID(s));
            if (t.IsSubclassOf(typeof(DataAsset))) return DataAssetRuntime.LoadByGuid(ParseAssetUUID(s), t);
            if (t.IsSubclassOf(typeof(Component)))
            {
                // Format: "EntityID:ComponentName". Must go through GetComponentByType so inspector fields share the same instance as GetComponent<T>() — otherwise UI event handlers silently never fire.
                var sep = s.IndexOf(':');
                if (sep > 0)
                {
                    ulong entityId = ulong.Parse(s.Substring(0, sep), ic);
                    string componentTypeName = s.Substring(sep + 1);
                    if (!MatchesComponentReferenceType(t, componentTypeName))
                        return null;

                    if (entityId != 0)
                    {
                        return new Entity(entityId).GetComponentByType(t);
                    }
                }
                return null;
            }

            if (t.IsEnum)
            {
                long parsed = string.IsNullOrWhiteSpace(s) ? 0 : long.Parse(s, ic);
                return Enum.ToObject(t, parsed);
            }

            if (GetListElementType(t) is Type listElem)
            {
                var pieces = DecodeListString(s);
                bool isObject = MapListItemType(listElem) == "object";
                if (t.IsArray)
                {
                    Array arr = Array.CreateInstance(listElem, pieces.Count);
                    for (int i = 0; i < pieces.Count; i++)
                        arr.SetValue(isObject ? ParseObjectListElement(listElem, pieces[i]) : ParseListElement(listElem, pieces[i]), i);
                    return arr;
                }
                var list = (System.Collections.IList)Activator.CreateInstance(
                    typeof(List<>).MakeGenericType(listElem))!;
                foreach (var piece in pieces)
                    list.Add(isObject ? ParseObjectListElement(listElem, piece) : ParseListElement(listElem, piece));
                return list;
            }
        }
        catch (Exception ex) when (ex is FormatException || ex is OverflowException || ex is ArgumentException)
        {
            Log.Warn($"Failed to parse script field value '{s}' as {t.Name}: {ex.Message}");
        }
        return null;
    }

    // Container codec for list fields. Each element is prefixed by a '\n' marker
    // (so an empty list is "" while a one-element list whose element is empty is
    // "\n" — the two are distinguishable); embedded backslash/newline in each
    // element are escaped. Must match the editor-side DecodeScriptList byte-for-
    // byte so values round-trip C# <-> native and survive scene save/load.
    private static string EncodeListString(List<string> parts)
    {
        var sb = new System.Text.StringBuilder();
        foreach (var part in parts)
        {
            sb.Append('\n');
            foreach (char c in part)
            {
                if (c == '\\') sb.Append("\\\\");
                else if (c == '\n') sb.Append("\\n");
                else sb.Append(c);
            }
        }
        return sb.ToString();
    }

    private static List<string> DecodeListString(string s)
    {
        var items = new List<string>();
        if (string.IsNullOrEmpty(s)) return items;
        // A non-empty payload always opens with the '\n' element marker; skip it
        // so the first element isn't read as a spurious leading empty entry.
        int start = (s[0] == '\n') ? 1 : 0;
        var current = new System.Text.StringBuilder();
        for (int i = start; i < s.Length; i++)
        {
            char c = s[i];
            if (c == '\\' && i + 1 < s.Length)
            {
                char next = s[i + 1];
                if (next == 'n') { current.Append('\n'); i++; continue; }
                if (next == '\\') { current.Append('\\'); i++; continue; }
            }
            if (c == '\n') { items.Add(current.ToString()); current.Clear(); continue; }
            current.Append(c);
        }
        items.Add(current.ToString());
        return items;
    }

    // Parse one list element; value types fall back to default() on parse failure
    // so a malformed piece can't null a List<int>/array slot.
    private static object? ParseListElement(Type elem, string piece)
    {
        object? parsed = ParseFieldValue(elem, piece);
        if (parsed == null && elem.IsValueType) parsed = Activator.CreateInstance(elem);
        return parsed;
    }

    // Parse one inline-object list element: inner-decode the leaf field values and
    // write them into a fresh instance (in GetObjectListFields order).
    private static object? ParseObjectListElement(Type elem, string piece)
    {
        object? obj;
        try { obj = Activator.CreateInstance(elem); }
        catch { return elem.IsValueType ? Activator.CreateInstance(elem) : null; }
        if (obj == null) return null;

        var fields = GetObjectListFields(elem);
        var fieldStrs = DecodeListString(piece);
        for (int i = 0; i < fields.Count && i < fieldStrs.Count; i++)
        {
            object? fv = ParseFieldValue(fields[i].ValueType, fieldStrs[i]);
            if (fv == null && fields[i].ValueType.IsValueType) fv = Activator.CreateInstance(fields[i].ValueType);
            try { fields[i].SetValue(obj, fv); } catch { /* skip a field that rejects the value */ }
        }
        return obj;
    }

    private static string EscapeJson(string s)
    {
        return s.Replace("\\", "\\\\").Replace("\"", "\\\"").Replace("\n", "\\n").Replace("\r", "\\r");
    }

    private static bool IsFieldEditorVisible(FieldInfo field)
    {
        if (field.GetCustomAttribute<EditorIgnoreAttribute>() != null) return false;
        if (field.GetCustomAttribute<HideFromEditorAttribute>() != null) return false;
        if (field.IsPublic) return true;
        return field.GetCustomAttribute<ShowInEditorAttribute>() != null;
    }

    private readonly struct EditorMember
    {
        public readonly MemberInfo Member;
        public readonly Type ValueType;
        public readonly bool IsProperty;
        public readonly bool CanWrite;
        public readonly bool IsPublic;
        public readonly string Name;
        public readonly Type? DeclaringType;

        private EditorMember(MemberInfo m, Type t, bool isProperty, bool canWrite, bool isPublic, string name, Type? declType)
        {
            Member = m;
            ValueType = t;
            IsProperty = isProperty;
            CanWrite = canWrite;
            IsPublic = isPublic;
            Name = name;
            DeclaringType = declType;
        }

        public static EditorMember FromField(FieldInfo f) => new(
            f, f.FieldType, isProperty: false,
            canWrite: !f.IsInitOnly && !f.IsLiteral,
            isPublic: f.IsPublic,
            name: f.Name, declType: f.DeclaringType);

        public static EditorMember FromProperty(PropertyInfo p)
        {
            bool canWrite = p.CanWrite
                && p.SetMethod != null
                && p.SetMethod.IsPublic;
            // "Public" for visibility purposes is whether the GETTER is
            // public — a property without a public reader can't be
            // displayed regardless of [ShowInEditor].
            bool isPublic = p.GetMethod != null && p.GetMethod.IsPublic;
            return new EditorMember(p, p.PropertyType, isProperty: true,
                canWrite, isPublic, p.Name, p.DeclaringType);
        }

        public T? GetCustomAttribute<T>() where T : Attribute => Member.GetCustomAttribute<T>();

        public object? GetValue(object instance) =>
            IsProperty ? ((PropertyInfo)Member).GetValue(instance)
                       : ((FieldInfo)Member).GetValue(instance);

        public void SetValue(object instance, object? value)
        {
            if (IsProperty) ((PropertyInfo)Member).SetValue(instance, value);
            else ((FieldInfo)Member).SetValue(instance, value);
        }
    }

    private static IEnumerable<EditorMember> CollectEditorMembers(Type type)
    {
        const BindingFlags k_Flags = BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Instance;
        foreach (var field in type.GetFields(k_Flags))
        {
            // Auto-property backing fields (`<Prop>k__BackingField`) are
            // tagged with [CompilerGenerated] — surface only the property
            // wrapper so the user sees the name they wrote.
            if (field.IsDefined(typeof(CompilerGeneratedAttribute), false)) continue;
            yield return EditorMember.FromField(field);
        }
        foreach (var property in type.GetProperties(k_Flags))
        {
            if (property.GetIndexParameters().Length > 0) continue;  // indexers (`this[int]`)
            if (property.GetMethod == null) continue;                 // write-only — nothing to display
            yield return EditorMember.FromProperty(property);
        }
    }

    private static bool IsMemberEditorVisible(in EditorMember member)
    {
        if (member.GetCustomAttribute<EditorIgnoreAttribute>() != null) return false;
        if (member.GetCustomAttribute<HideFromEditorAttribute>() != null) return false;
        if (member.IsPublic) return true;
        return member.GetCustomAttribute<ShowInEditorAttribute>() != null;
    }

    // Emits the JSON PropertyDescriptor shape for one member; enum fields gain enumIsFlags/enumOptions arrays for the combo/checkbox UI.
    // namePrefix is the dotted path of any enclosing struct/class ("" at top level, "P." for a member of a flattened `Point P`).
    private static void AppendMemberJson(System.Text.StringBuilder sb, in EditorMember member, object? value, string namePrefix)
    {
        var ic = System.Globalization.CultureInfo.InvariantCulture;
        // showAttr is optional now: public members are visible by default
        // and only need ShowInEditor when overriding the display name.
        var showAttr = member.GetCustomAttribute<ShowInEditorAttribute>();

        string fieldType = MapFieldType(member.ValueType);
        string valueStr = FormatFieldValue(member.ValueType, value);
        string displayName = (showAttr != null && !string.IsNullOrEmpty(showAttr.DisplayName))
            ? showAttr.DisplayName
            : member.Name;
        bool readOnly = (showAttr?.ReadOnly ?? false)
            || member.GetCustomAttribute<EditorReadOnlyAttribute>() != null
            || !member.CanWrite;

        float clampMin = 0, clampMax = 0;
        bool hasClamp = false;
        var clampAttr = member.GetCustomAttribute<ClampValueAttribute>();
        if (clampAttr != null)
        {
            clampMin = clampAttr.Min;
            clampMax = clampAttr.Max;
            hasClamp = true;
        }

        string tooltip = "";
        var tooltipAttr = member.GetCustomAttribute<ToolTipAttribute>();
        if (tooltipAttr != null) tooltip = tooltipAttr.Text;

        string headerContent = "";
        int headerSize = 0;
        var headerAttr = member.GetCustomAttribute<HeaderAttribute>();
        if (headerAttr != null)
        {
            headerContent = headerAttr.Content;
            headerSize = headerAttr.Size;
        }

        float spaceHeight = 0.0f;
        bool hasSpace = false;
        var spaceAttr = member.GetCustomAttribute<SpaceAttribute>();
        if (spaceAttr != null)
        {
            spaceHeight = spaceAttr.Height;
            hasSpace = true;
        }

        string enabledIfField = "";
        string enabledIfValue = "";
        bool hasEnabledIf = false;
        bool enabledIfAny = false;  // true == "enable when any truthy value" (no expected value supplied)
        var enabledIfAttr = member.GetCustomAttribute<EnabledIfAttribute>();
        if (enabledIfAttr != null)
        {
            enabledIfField = enabledIfAttr.FieldName;
            hasEnabledIf = true;
            if (enabledIfAttr.ExpectedValue == null)
            {
                enabledIfAny = true;
            }
            else
            {
                // Format using the same wire shape FormatFieldValue uses for
                // that type so the C++ side can string-compare directly
                // against the snapshot it already has.
                enabledIfValue = FormatFieldValue(enabledIfAttr.ExpectedValue.GetType(), enabledIfAttr.ExpectedValue);
            }
        }

        sb.Append("{\"name\":\"").Append(EscapeJson(namePrefix + member.Name))
          .Append("\",\"displayName\":\"").Append(EscapeJson(displayName))
          .Append("\",\"type\":\"").Append(fieldType)
          .Append("\",\"value\":\"").Append(EscapeJson(valueStr))
          .Append("\",\"readOnly\":").Append(readOnly ? "true" : "false")
          .Append(",\"hasClamp\":").Append(hasClamp ? "true" : "false")
          .Append(",\"clampMin\":").Append(clampMin.ToString(ic))
          .Append(",\"clampMax\":").Append(clampMax.ToString(ic))
          .Append(",\"tooltip\":\"").Append(EscapeJson(tooltip))
          .Append("\",\"headerContent\":\"").Append(EscapeJson(headerContent))
          .Append("\",\"headerSize\":").Append(headerSize.ToString(ic))
          .Append(",\"hasSpace\":").Append(hasSpace ? "true" : "false")
          .Append(",\"spaceHeight\":").Append(spaceHeight.ToString(ic))
          .Append(",\"hasEnabledIf\":").Append(hasEnabledIf ? "true" : "false")
          .Append(",\"enabledIfField\":\"").Append(EscapeJson(enabledIfField))
          .Append("\",\"enabledIfValue\":\"").Append(EscapeJson(enabledIfValue))
          .Append("\",\"enabledIfAny\":").Append(enabledIfAny ? "true" : "false");

        if (member.ValueType.IsEnum)
        {
            bool isFlags = member.ValueType.GetCustomAttribute<FlagsAttribute>() != null;
            sb.Append(",\"enumIsFlags\":").Append(isFlags ? "true" : "false");
            sb.Append(",\"enumOptions\":[");
            bool firstOpt = true;
            foreach (var name in Enum.GetNames(member.ValueType))
            {
                object enumValue = Enum.Parse(member.ValueType, name);
                long underlying = Convert.ToInt64(enumValue, ic);
                if (!firstOpt) sb.Append(',');
                firstOpt = false;
                sb.Append("{\"name\":\"").Append(EscapeJson(name))
                  .Append("\",\"value\":").Append(underlying.ToString(ic))
                  .Append('}');
            }
            sb.Append(']');
        }

        // List<T> / T[]: emit the element wire type (and enum options for
        // List<enum>) so the editor's list drawer picks the per-row widget.
        if (GetListElementType(member.ValueType) is Type listElem)
        {
            string itemTag = MapListItemType(listElem) ?? "";
            sb.Append(",\"itemType\":\"").Append(EscapeJson(itemTag)).Append('"');
            if (listElem.IsEnum)
            {
                bool isFlags = listElem.GetCustomAttribute<FlagsAttribute>() != null;
                sb.Append(",\"enumIsFlags\":").Append(isFlags ? "true" : "false");
                sb.Append(",\"enumOptions\":[");
                bool firstOpt = true;
                foreach (var name in Enum.GetNames(listElem))
                {
                    object enumValue = Enum.Parse(listElem, name);
                    long underlying = Convert.ToInt64(enumValue, ic);
                    if (!firstOpt) sb.Append(',');
                    firstOpt = false;
                    sb.Append("{\"name\":\"").Append(EscapeJson(name))
                      .Append("\",\"value\":").Append(underlying.ToString(ic))
                      .Append('}');
                }
                sb.Append(']');
            }
            if (itemTag == "object")
            {
                // Schema for the inline sub-fields, in encode/decode order, so the
                // editor can render and edit each element's fields.
                sb.Append(",\"itemFields\":[");
                bool firstField = true;
                foreach (var f in GetObjectListFields(listElem))
                {
                    if (!firstField) sb.Append(',');
                    firstField = false;
                    string ftag = MapFieldType(f.ValueType);
                    var fShow = f.GetCustomAttribute<ShowInEditorAttribute>();
                    string fdisplay = (fShow != null && !string.IsNullOrEmpty(fShow.DisplayName)) ? fShow.DisplayName : f.Name;
                    sb.Append("{\"name\":\"").Append(EscapeJson(f.Name))
                      .Append("\",\"displayName\":\"").Append(EscapeJson(fdisplay))
                      .Append("\",\"type\":\"").Append(EscapeJson(ftag)).Append('"');
                    if (f.ValueType.IsEnum)
                    {
                        bool fIsFlags = f.ValueType.GetCustomAttribute<FlagsAttribute>() != null;
                        sb.Append(",\"enumIsFlags\":").Append(fIsFlags ? "true" : "false");
                        sb.Append(",\"enumOptions\":[");
                        bool fo = true;
                        foreach (var nm in Enum.GetNames(f.ValueType))
                        {
                            long u = Convert.ToInt64(Enum.Parse(f.ValueType, nm), ic);
                            if (!fo) sb.Append(',');
                            fo = false;
                            sb.Append("{\"name\":\"").Append(EscapeJson(nm)).Append("\",\"value\":").Append(u.ToString(ic)).Append('}');
                        }
                        sb.Append(']');
                    }
                    sb.Append('}');
                }
                sb.Append(']');
            }
        }

        sb.Append("}");
    }

    private const int k_MaxNestingDepth = 4;

    // Emits one editor member. A directly-supported type (see MapFieldType)
    // becomes a single field record; a plain user struct/class is flattened
    // into dotted child records (P -> "P.X", "P.Y") so the editor renders it
    // as a nested, editable group. Genuinely unsupported types are skipped.
    private static void AppendEditorMember(System.Text.StringBuilder sb, in EditorMember member,
        object? value, string namePrefix, int depth, ref bool first)
    {
        if (MapFieldType(member.ValueType) != "unsupported")
        {
            if (!first) sb.Append(',');
            first = false;
            AppendMemberJson(sb, member, value, namePrefix);
            return;
        }

        if (depth >= k_MaxNestingDepth || !IsInspectableNestedType(member.ValueType))
            return;

        // Read defaults from a throwaway instance when the field is null so the
        // group still shows its members (editing lazily creates the real one).
        object? nested = value;
        if (nested == null)
        {
            try { nested = Activator.CreateInstance(member.ValueType); }
            catch { return; }
        }

        string childPrefix = namePrefix + member.Name + ".";
        foreach (var child in CollectEditorMembers(member.ValueType))
        {
            if (!IsMemberEditorVisible(child)) continue;
            object? childValue = nested != null ? TryGetMemberValue(child, nested) : null;
            AppendEditorMember(sb, child, childValue, childPrefix, depth + 1, ref first);
        }
    }

    // True for a plain user struct/class the inspector can flatten. Excludes
    // primitives/enums/strings, known engine types (handled by MapFieldType),
    // Component/IComponent, collections, delegates, and framework (System.*)
    // types. Classes need a public parameterless ctor so defaults can be read.
    private static bool IsInspectableNestedType(Type t)
    {
        if (t.IsPrimitive || t.IsEnum || t.IsArray || t.IsPointer) return false;
        if (t == typeof(string)) return false;
        if (t.IsGenericType) return false;
        if (t.IsAbstract || t.IsInterface) return false;
        if (typeof(Delegate).IsAssignableFrom(t)) return false;
        if (t.IsSubclassOf(typeof(Component))) return false;
        if (typeof(Index.Components.IComponent).IsAssignableFrom(t)) return false;
        if (t.Namespace != null && t.Namespace.StartsWith("System", StringComparison.Ordinal)) return false;
        if (!t.IsValueType && t.GetConstructor(Type.EmptyTypes) == null) return false;
        return t.IsClass || t.IsValueType;
    }

    private static object? TryGetMemberValue(in EditorMember member, object instance)
    {
        try { return member.GetValue(instance); }
        catch (Exception ex)
        {
            Log.Warn($"Reading {(member.IsProperty ? "property" : "field")} '{instance.GetType().Name}.{member.Name}' threw: {ex.Message}");
            return null;
        }
    }

    private static void ReleaseFieldJsonBuffer()
    {
        lock (s_FieldJsonBufferLock)
        {
            if (s_FieldJsonBufferHandle.IsAllocated)
                s_FieldJsonBufferHandle.Free();

            s_FieldJsonBuffer = Array.Empty<byte>();
        }
    }

    private static unsafe byte* NullTerminated(string s)
    {
        lock (s_FieldJsonBufferLock)
        {
            int byteCount = System.Text.Encoding.UTF8.GetByteCount(s);
            if (s_FieldJsonBuffer.Length < byteCount + 1)
            {
                if (s_FieldJsonBufferHandle.IsAllocated)
                    s_FieldJsonBufferHandle.Free();

                s_FieldJsonBuffer = new byte[byteCount + 1];
                s_FieldJsonBufferHandle = GCHandle.Alloc(s_FieldJsonBuffer, GCHandleType.Pinned);
            }
            else if (!s_FieldJsonBufferHandle.IsAllocated)
            {
                s_FieldJsonBufferHandle = GCHandle.Alloc(s_FieldJsonBuffer, GCHandleType.Pinned);
            }

            System.Text.Encoding.UTF8.GetBytes(s, s_FieldJsonBuffer);
            s_FieldJsonBuffer[byteCount] = 0;
            return (byte*)s_FieldJsonBufferHandle.AddrOfPinnedObject();
        }
    }

    /// <summary>Returns field definitions for a class using a temporary instance for default values; used by the editor in Edit Mode.</summary>
    [UnmanagedCallersOnly]
    public static unsafe byte* GetClassFieldDefs(byte* classNamePtr)
    {
        try
        {
            string className = Marshal.PtrToStringUTF8((IntPtr)classNamePtr) ?? "";
            var classInfo = GetOrCacheClass(className);
            if (classInfo == null) return NullTerminated("[]");

            // Create a temporary instance just to read default values.
            object? tempInstance = null;
            try { tempInstance = Activator.CreateInstance(classInfo.Type)!; }
            catch { return NullTerminated("[]"); }

            var sb = new System.Text.StringBuilder();
            sb.Append('[');
            bool first = true;

            foreach (var member in CollectEditorMembers(classInfo.Type))
            {
                if (member.DeclaringType == typeof(EntityScript)) continue;
                if (member.DeclaringType == typeof(Component)) continue;
                if (member.DeclaringType == typeof(SceneSystem)) continue;
                if (member.DeclaringType == typeof(GlobalSystem)) continue;
                if (member.DeclaringType == typeof(DataAsset)) continue;
                if (!IsMemberEditorVisible(member)) continue;

                object? val = TryGetMemberValue(member, tempInstance);
                AppendEditorMember(sb, member, val, "", 0, ref first);
            }

            sb.Append(']');
            return NullTerminated(sb.ToString());
        }
        catch (Exception ex)
        {
            Log.Error($"GetClassFieldDefs failed: {ex.Message}");
            return NullTerminated("[]");
        }
    }

    // ── Inspector event bindings ────────────────────────────────

    private static readonly Type[] s_InvokableExcludedDeclaringTypes =
    {
        typeof(object),
        typeof(EntityScript),
        typeof(Component),
        typeof(SceneSystem),
        typeof(GlobalSystem),
    };

    private static void EnsureInvokableMethods(ScriptClassInfo info)
    {
        if (info.InvokableMethodsByName != null) return;

        var byName = new Dictionary<string, MethodInfo>(StringComparer.Ordinal);
        var nameKinds = new List<(string Name, byte Kind)>();

        var methods = info.Type.GetMethods(BindingFlags.Public | BindingFlags.Instance);
        foreach (var method in methods)
        {
            if (method.ReturnType != typeof(void)) continue;
            if (method.IsGenericMethod) continue;
            if (method.IsSpecialName) continue;  // property accessors, op_*, etc.

            var parameters = method.GetParameters();
            if (parameters.Length > 1) continue;

            byte argKind;
            if (parameters.Length == 0)
            {
                argKind = k_ArgKindVoid;
            }
            else
            {
                if (parameters[0].IsOut || parameters[0].ParameterType.IsByRef) continue;
                argKind = ResolveArgKind(parameters[0].ParameterType);
                if (argKind == 0xFF) continue; // unsupported argument type
            }

            // Skip lifecycle methods and anything inherited from the framework
            // bases. Users want to wire up their own gameplay methods, not
            // OnUpdate / ToString.
            bool excluded = false;
            for (int i = 0; i < s_InvokableExcludedDeclaringTypes.Length; ++i)
            {
                if (method.DeclaringType == s_InvokableExcludedDeclaringTypes[i])
                {
                    excluded = true;
                    break;
                }
            }
            if (excluded) continue;

            // Name-only dispatch: first overload wins; if a class needs both signatures, rename one.
            if (byName.TryAdd(method.Name, method))
            {
                nameKinds.Add((method.Name, argKind));
            }
        }

        nameKinds.Sort((a, b) => string.CompareOrdinal(a.Name, b.Name));

        var names = new List<string>(nameKinds.Count);
        var kinds = new List<byte>(nameKinds.Count);
        foreach (var nk in nameKinds)
        {
            names.Add(nk.Name);
            kinds.Add(nk.Kind);
        }

        info.InvokableMethodsByName = byName;
        info.InvokableMethodNames = names;
        info.InvokableMethodArgKinds = kinds;
    }

    [UnmanagedCallersOnly]
    public static unsafe int GetInvokableMethodsBuffer(byte* classNamePtr, byte* outBuffer, int capacity)
    {
        try
        {
            string className = PtrToString(classNamePtr);
            var classInfo = GetOrCacheClass(className);
            if (classInfo == null) return WriteUtf8("[]", outBuffer, capacity);

            EnsureInvokableMethods(classInfo);

            var sb = new System.Text.StringBuilder();
            sb.Append('[');
            bool first = true;
            for (int i = 0; i < classInfo.InvokableMethodNames!.Count; ++i)
            {
                string name = classInfo.InvokableMethodNames[i];
                byte kind = classInfo.InvokableMethodArgKinds![i];
                if (!first) sb.Append(',');
                first = false;
                sb.Append('"')
                  .Append(EscapeJson(name))
                  .Append(':')
                  .Append(kind)
                  .Append('"');
            }
            sb.Append(']');
            return WriteUtf8(sb.ToString(), outBuffer, capacity);
        }
        catch (Exception ex)
        {
            Log.Error($"GetInvokableMethodsBuffer failed: {ex.Message}");
            return WriteUtf8("[]", outBuffer, capacity);
        }
    }

    private static bool TryParseFloatList(string s, int expected, Span<float> outValues)
    {
        if (string.IsNullOrEmpty(s) || expected <= 0 || outValues.Length < expected) return false;
        int filled = 0;
        int start = 0;
        for (int i = 0; i <= s.Length; ++i)
        {
            if (i == s.Length || s[i] == ',')
            {
                if (filled >= expected) return false;
                var slice = s.AsSpan(start, i - start).Trim();
                if (slice.IsEmpty) return false;
                if (!float.TryParse(slice,
                        System.Globalization.NumberStyles.Float,
                        System.Globalization.CultureInfo.InvariantCulture,
                        out outValues[filled])) return false;
                ++filled;
                start = i + 1;
            }
        }
        return filled == expected;
    }

    private static unsafe object?[]? BuildInvokeArguments(MethodInfo method, byte argKind, byte* argValuePtr)
    {
        var parameters = method.GetParameters();
        if (parameters.Length == 0)
        {
            // Method takes no arg — the editor's `argKind != 0` is just
            // stale data from when the user picked a different overload.
            // Drop the arg silently rather than failing the dispatch.
            return null;
        }
        if (parameters.Length != 1) return null;

        // No value provided — fall through to default(T) so a binding
        // saved before the user filled in a value still fires the method
        // (rather than throwing on a null Span<byte>).
        string raw = argValuePtr == null ? string.Empty : PtrToString(argValuePtr);
        var ic = System.Globalization.CultureInfo.InvariantCulture;

        switch (argKind)
        {
            case k_ArgKindBool:
                return new object?[] { raw == "1" || string.Equals(raw, "true", StringComparison.OrdinalIgnoreCase) };
            case k_ArgKindInt:
            {
                long v = 0;
                long.TryParse(raw, System.Globalization.NumberStyles.Integer, ic, out v);
                Type pt = parameters[0].ParameterType;
                if (pt == typeof(int))    return new object?[] { (int)v };
                if (pt == typeof(short))  return new object?[] { (short)v };
                if (pt == typeof(byte))   return new object?[] { (byte)v };
                if (pt == typeof(sbyte))  return new object?[] { (sbyte)v };
                if (pt == typeof(uint))   return new object?[] { (uint)v };
                if (pt == typeof(long))   return new object?[] { v };
                if (pt == typeof(ushort)) return new object?[] { (ushort)v };
                return new object?[] { (int)v };
            }
            case k_ArgKindFloat:
            {
                float f = 0f;
                float.TryParse(raw, System.Globalization.NumberStyles.Float, ic, out f);
                return new object?[] { f };
            }
            case k_ArgKindDouble:
            {
                double d = 0.0;
                double.TryParse(raw, System.Globalization.NumberStyles.Float, ic, out d);
                return new object?[] { d };
            }
            case k_ArgKindString:
                return new object?[] { raw };
            case k_ArgKindVec2:
            {
                Span<float> v = stackalloc float[2];
                v[0] = 0f; v[1] = 0f;
                TryParseFloatList(raw, 2, v);
                return new object?[] { new Vector2(v[0], v[1]) };
            }
            case k_ArgKindColor:
            {
                Span<float> v = stackalloc float[4];
                v[0] = 1f; v[1] = 1f; v[2] = 1f; v[3] = 1f;
                TryParseFloatList(raw, 4, v);
                return new object?[] { new Color(v[0], v[1], v[2], v[3]) };
            }
            case k_ArgKindEntityRef:
            {
                ulong uuid = 0;
                ulong.TryParse(raw, System.Globalization.NumberStyles.Integer, ic, out uuid);
                return new object?[] { uuid == 0 ? Entity.Invalid : new Entity(uuid) };
            }
            default:
                return null;
        }
    }

    [UnmanagedCallersOnly]
    public static unsafe int InvokeScriptMethodByName(int handle, byte* methodNamePtr,
        byte argKind, byte* argValuePtr)
    {
        if (!s_Instances.TryGetValue(handle, out var data)) return 0;

        EntityScript instance = data.Instance;
        string methodName = PtrToString(methodNamePtr);

        var info = GetOrCacheClass(instance.GetType().Name);
        if (info == null) return 0;

        EnsureInvokableMethods(info);

        if (!info.InvokableMethodsByName!.TryGetValue(methodName, out var method))
        {
            return 0;
        }

        object?[]? args = null;
        if (argKind != k_ArgKindVoid)
        {
            args = BuildInvokeArguments(method, argKind, argValuePtr);
            if (args == null && method.GetParameters().Length > 0)
            {
                // Method needs an arg but the binding's encoded value was
                // unparseable for its declared kind — log once and skip
                // rather than crash inside method.Invoke with a TypeMismatch.
                Log.Warn($"Inspector binding {info.Type.Name}.{methodName}: argument value could not be parsed for kind {argKind}");
                return 1; // method located; just couldn't dispatch this fire
            }
        }

        try
        {
            method.Invoke(instance, args);
            return 1;
        }
        catch (TargetInvocationException ex)
        {
            Log.Error($"Exception in {info.Type.Name}.{methodName}(): {ex.InnerException}");
            return 1;  // method exists; the user's body threw — don't log "missing".
        }
        catch (Exception ex)
        {
            Log.Error($"Exception invoking {info.Type.Name}.{methodName}(): {ex}");
            return 1;
        }
    }

    // Can't reuse NullTerminated() — it's a singleton pinned buffer; concurrent reflection calls from the same frame would clobber each other.
    private static unsafe int WriteUtf8(string s, byte* outBuffer, int capacity)
    {
        int byteCount = System.Text.Encoding.UTF8.GetByteCount(s);
        if (outBuffer == null || capacity <= 0) return byteCount + 1;
        if (capacity < byteCount + 1)
        {
            // Caller's buffer too small — write null terminator and return
            // the required capacity so they can resize and retry.
            outBuffer[0] = 0;
            return byteCount + 1;
        }

        var span = new Span<byte>(outBuffer, capacity);
        int written = System.Text.Encoding.UTF8.GetBytes(s, span);
        span[written] = 0;
        return written + 1;
    }

    // ── Class cache ─────────────────────────────────────────────

    private static ScriptClassInfo? GetOrCacheClass(string className)
    {
        if (s_ClassCache.TryGetValue(className, out var cached))
            return cached;

        Type? type = FindType(className);
        bool isScript = type != null && type.IsSubclassOf(typeof(EntityScript));
        bool isComponent = type != null
            && ((type.IsSubclassOf(typeof(Component)) && !Entity.TryGetNativeComponentName(type, out _))
                || (type.IsValueType && typeof(Index.Components.IComponent).IsAssignableFrom(type)));
        bool isSceneSystem = type != null && type.IsSubclassOf(typeof(SceneSystem));
        bool isGlobalSystem = type != null && type.IsSubclassOf(typeof(GlobalSystem));
        bool isDataAsset = type != null && type.IsSubclassOf(typeof(DataAsset));
        if (type == null || (!isScript && !isComponent && !isSceneSystem && !isGlobalSystem && !isDataAsset))
        {
            s_ClassCache[className] = null;
            return null;
        }

        var info = new ScriptClassInfo
        {
            Type = type,
            IsScript = isScript,
            IsComponent = isComponent,
            IsSceneSystem = isSceneSystem,
            IsGlobalSystem = isGlobalSystem,
            IsDataAsset = isDataAsset,
            StartMethod = isScript ? type.GetMethod("Start", BindingFlags.Public | BindingFlags.Instance, Type.EmptyTypes) : null,
            UpdateMethod = isScript ? type.GetMethod("Update", BindingFlags.Public | BindingFlags.Instance, Type.EmptyTypes) : null,
        };

        s_ClassCache[className] = info;
        return info;
    }

    private static Type? FindType(string className)
    {
        if (s_UserAssembly != null)
        {
            var type = s_UserAssembly.GetType($"Index.{className}")
                    ?? s_UserAssembly.GetType($"Index.Components.{className}")
                    ?? s_UserAssembly.GetType(className);
            if (type != null) return type;
        }

        if (s_CoreAssembly != null)
        {
            var type = s_CoreAssembly.GetType($"Index.{className}")
                    ?? s_CoreAssembly.GetType($"Index.Components.{className}")
                    ?? s_CoreAssembly.GetType(className);
            if (type != null) return type;
        }

        return null;
    }
}
