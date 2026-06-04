using System;
using System.Runtime.InteropServices;
using System.Threading;
using Index.Components;
using Index.Coroutines;

namespace Index.Interop;
/// <summary>
/// Layout must match the C++ ManagedCallbacks struct exactly.
/// </summary>
[StructLayout(LayoutKind.Sequential)]
internal unsafe struct ManagedCallbacksStruct
{
    public delegate* unmanaged<byte*, ulong, int> CreateScriptInstance;
    public delegate* unmanaged<int, void> DestroyScriptInstance;
    public delegate* unmanaged<int, void> InvokeStart;
    public delegate* unmanaged<int, void> InvokeUpdate;
    public delegate* unmanaged<int, void> InvokeOnDestroy;
    public delegate* unmanaged<int, void> InvokeOnEnable;
    public delegate* unmanaged<int, void> InvokeOnDisable;
    public delegate* unmanaged<int, ulong, ulong, ulong, ulong, float, float, void> InvokeCollisionEnter2D;
    public delegate* unmanaged<int, ulong, ulong, ulong, ulong, float, float, void> InvokeCollisionStay2D;
    public delegate* unmanaged<int, ulong, ulong, ulong, ulong, float, float, void> InvokeCollisionExit2D;
    public delegate* unmanaged<byte*, int> ClassExists;
    public delegate* unmanaged<byte*, int> LoadUserAssembly;
    public delegate* unmanaged<void> UnloadUserAssembly;
    public delegate* unmanaged<int, byte*> GetScriptFields;
    public delegate* unmanaged<int, byte*, byte*, void> SetScriptField;
    public delegate* unmanaged<byte*, byte*> GetClassFieldDefs;
    public delegate* unmanaged<void> RaiseApplicationStart;
    public delegate* unmanaged<void> RaiseApplicationPaused;
    public delegate* unmanaged<void> RaiseApplicationQuit;
    public delegate* unmanaged<int, void> RaiseFocusChanged;
    public delegate* unmanaged<int, void> RaiseKeyDown;
    public delegate* unmanaged<int, void> RaiseKeyUp;
    public delegate* unmanaged<int, void> RaiseMouseDown;
    public delegate* unmanaged<int, void> RaiseMouseUp;
    public delegate* unmanaged<float, void> RaiseMouseScroll;
    public delegate* unmanaged<float, float, void> RaiseMouseMove;
    public delegate* unmanaged<byte*, void> RaiseBeforeSceneLoaded;
    public delegate* unmanaged<byte*, void> RaiseSceneLoaded;
    public delegate* unmanaged<byte*, void> RaiseBeforeSceneUnloaded;
    public delegate* unmanaged<byte*, void> RaiseSceneUnloaded;
    public delegate* unmanaged<byte*, byte*, int> CreateSceneScriptInstance;
    public delegate* unmanaged<int, void> DestroySceneScriptInstance;
    public delegate* unmanaged<int, void> InvokeSceneScriptStart;
    public delegate* unmanaged<int, void> InvokeSceneScriptUpdate;
    public delegate* unmanaged<int, void> InvokeSceneScriptEnable;
    public delegate* unmanaged<int, void> InvokeSceneScriptDisable;
    public delegate* unmanaged<int, void> InvokeSceneScriptDestroy;
    public delegate* unmanaged<byte*, int> SceneScriptClassExists;
    public delegate* unmanaged<byte*, int> CreateGlobalScriptInstance;
    public delegate* unmanaged<int, void> DestroyGlobalScriptInstance;
    public delegate* unmanaged<int, void> InvokeGlobalScriptInitialize;
    public delegate* unmanaged<int, void> InvokeGlobalScriptUpdate;
    public delegate* unmanaged<int, void> InvokeGlobalScriptEnable;
    public delegate* unmanaged<int, void> InvokeGlobalScriptDisable;
    public delegate* unmanaged<byte*, int> GlobalScriptClassExists;

    // ── New lifecycle slots (appended for binary compat) ──
    public delegate* unmanaged<int, void> InvokeAwake;
    public delegate* unmanaged<int, void> InvokeFixedUpdate;
    public delegate* unmanaged<int, void> InvokeSceneScriptAwake;
    public delegate* unmanaged<int, void> InvokeSceneScriptFixedUpdate;
    public delegate* unmanaged<int, void> InvokeGlobalScriptFixedUpdate;

    // ── SceneScript field reflection (appended for binary compat) ──
    public delegate* unmanaged<int, byte*> GetSceneScriptFields;
    public delegate* unmanaged<int, byte*, byte*, void> SetSceneScriptField;

    public delegate* unmanaged<void> RaiseUiEventDispatch;

    // ── Coroutine pump (appended for binary compat) ──
    public delegate* unmanaged<float, void> PumpCoroutinesUpdate;
    public delegate* unmanaged<void> PumpCoroutinesFixedUpdate;

    // ── Inspector event bindings (appended for binary compat) ──
    // Order must match ScriptGlue.hpp's ManagedCallbacks struct exactly.
    public delegate* unmanaged<byte*, byte*, int, int> GetInvokableMethodsBuffer;
    // Typed-argument variant: argKind is the byte value of
    // InspectorEventArgKind, argValue is the encoded string per the kind
    // (or null when argKind == 0 / Void).
    public delegate* unmanaged<int, byte*, byte, byte*, int> InvokeScriptMethodByName;

    // ── Window events (appended for binary compat) ──
    // Routed from Application::DispatchEvent on WindowResizeEvent.
    public delegate* unmanaged<void> RaiseWindowResize;
    public delegate* unmanaged<uint, void> RaiseEnterChar;

    // ── Play-mode lifecycle (appended for binary compat) ──
    // Called on stop BEFORE scene snapshot restore; sweeps play-mode static event subscribers to prevent edit-mode ghost firings.
    public delegate* unmanaged<void> OnPlayModeExited;
}

/// <summary>Entry point called from C++ via hostfxr; exchanges native/managed function pointers.</summary>
internal static class ScriptHostBridge
{
    [UnmanagedCallersOnly]
    internal static unsafe int Initialize(
        NativeBindingsStruct* nativeBindings,
        ManagedCallbacksStruct* managedCallbacks)
    {
        try
        {
            NativeCallbacks.SetFrom(nativeBindings);

            managedCallbacks->CreateScriptInstance = &ScriptInstanceManager.CreateScriptInstance;
            managedCallbacks->DestroyScriptInstance = &ScriptInstanceManager.DestroyScriptInstance;
            managedCallbacks->InvokeStart = &ScriptInstanceManager.InvokeStart;
            managedCallbacks->InvokeUpdate = &ScriptInstanceManager.InvokeUpdate;
            managedCallbacks->InvokeOnDestroy = &ScriptInstanceManager.InvokeOnDestroy;
            managedCallbacks->InvokeOnEnable = &ScriptInstanceManager.InvokeOnEnable;
            managedCallbacks->InvokeOnDisable = &ScriptInstanceManager.InvokeOnDisable;
            managedCallbacks->InvokeCollisionEnter2D = &ScriptInstanceManager.InvokeCollisionEnter2D;
            managedCallbacks->InvokeCollisionStay2D = &ScriptInstanceManager.InvokeCollisionStay2D;
            managedCallbacks->InvokeCollisionExit2D = &ScriptInstanceManager.InvokeCollisionExit2D;
            managedCallbacks->ClassExists = &ScriptInstanceManager.ClassExists;
            managedCallbacks->LoadUserAssembly = &ScriptInstanceManager.LoadUserAssembly;
            managedCallbacks->UnloadUserAssembly = &ScriptInstanceManager.UnloadUserAssembly;
            managedCallbacks->GetScriptFields = &ScriptInstanceManager.GetScriptFields;
            managedCallbacks->SetScriptField = &ScriptInstanceManager.SetScriptField;
            managedCallbacks->GetClassFieldDefs = &ScriptInstanceManager.GetClassFieldDefs;
            managedCallbacks->RaiseApplicationStart = &ScriptInstanceManager.RaiseApplicationStart;
            managedCallbacks->RaiseApplicationPaused = &ScriptInstanceManager.RaiseApplicationPaused;
            managedCallbacks->RaiseApplicationQuit = &ScriptInstanceManager.RaiseApplicationQuit;
            managedCallbacks->RaiseFocusChanged = &ScriptInstanceManager.RaiseFocusChanged;
            managedCallbacks->RaiseKeyDown = &ScriptInstanceManager.RaiseKeyDown;
            managedCallbacks->RaiseKeyUp = &ScriptInstanceManager.RaiseKeyUp;
            managedCallbacks->RaiseMouseDown = &ScriptInstanceManager.RaiseMouseDown;
            managedCallbacks->RaiseMouseUp = &ScriptInstanceManager.RaiseMouseUp;
            managedCallbacks->RaiseMouseScroll = &ScriptInstanceManager.RaiseMouseScroll;
            managedCallbacks->RaiseMouseMove = &ScriptInstanceManager.RaiseMouseMove;
            managedCallbacks->RaiseBeforeSceneLoaded = &ScriptInstanceManager.RaiseBeforeSceneLoaded;
            managedCallbacks->RaiseSceneLoaded = &ScriptInstanceManager.RaiseSceneLoaded;
            managedCallbacks->RaiseBeforeSceneUnloaded = &ScriptInstanceManager.RaiseBeforeSceneUnloaded;
            managedCallbacks->RaiseSceneUnloaded = &ScriptInstanceManager.RaiseSceneUnloaded;
            managedCallbacks->CreateSceneScriptInstance = &ScriptInstanceManager.CreateSceneScriptInstance;
            managedCallbacks->DestroySceneScriptInstance = &ScriptInstanceManager.DestroySceneScriptInstance;
            managedCallbacks->InvokeSceneScriptStart = &ScriptInstanceManager.InvokeSceneScriptStart;
            managedCallbacks->InvokeSceneScriptUpdate = &ScriptInstanceManager.InvokeSceneScriptUpdate;
            managedCallbacks->InvokeSceneScriptEnable = &ScriptInstanceManager.InvokeSceneScriptEnable;
            managedCallbacks->InvokeSceneScriptDisable = &ScriptInstanceManager.InvokeSceneScriptDisable;
            managedCallbacks->InvokeSceneScriptDestroy = &ScriptInstanceManager.InvokeSceneScriptDestroy;
            managedCallbacks->SceneScriptClassExists = &ScriptInstanceManager.SceneScriptClassExists;
            managedCallbacks->CreateGlobalScriptInstance = &ScriptInstanceManager.CreateGlobalScriptInstance;
            managedCallbacks->DestroyGlobalScriptInstance = &ScriptInstanceManager.DestroyGlobalScriptInstance;
            managedCallbacks->InvokeGlobalScriptInitialize = &ScriptInstanceManager.InvokeGlobalScriptInitialize;
            managedCallbacks->InvokeGlobalScriptUpdate = &ScriptInstanceManager.InvokeGlobalScriptUpdate;
            managedCallbacks->InvokeGlobalScriptEnable = &ScriptInstanceManager.InvokeGlobalScriptEnable;
            managedCallbacks->InvokeGlobalScriptDisable = &ScriptInstanceManager.InvokeGlobalScriptDisable;
            managedCallbacks->GlobalScriptClassExists = &ScriptInstanceManager.GlobalScriptClassExists;

            // ── New lifecycle slots (appended for binary compat) ──
            managedCallbacks->InvokeAwake = &ScriptInstanceManager.InvokeAwake;
            managedCallbacks->InvokeFixedUpdate = &ScriptInstanceManager.InvokeFixedUpdate;
            managedCallbacks->InvokeSceneScriptAwake = &ScriptInstanceManager.InvokeSceneScriptAwake;
            managedCallbacks->InvokeSceneScriptFixedUpdate = &ScriptInstanceManager.InvokeSceneScriptFixedUpdate;
            managedCallbacks->InvokeGlobalScriptFixedUpdate = &ScriptInstanceManager.InvokeGlobalScriptFixedUpdate;

            // ── SceneScript field reflection (appended for binary compat) ──
            managedCallbacks->GetSceneScriptFields = &ScriptInstanceManager.GetSceneScriptFields;
            managedCallbacks->SetSceneScriptField = &ScriptInstanceManager.SetSceneScriptField;

            // ── UI event dispatch (appended for binary compat) ──
            managedCallbacks->RaiseUiEventDispatch = &ScriptInstanceManager.RaiseUiEventDispatch;

            // ── Coroutine pump (appended for binary compat) ──
            managedCallbacks->PumpCoroutinesUpdate = &ScriptInstanceManager.PumpCoroutinesUpdate;
            managedCallbacks->PumpCoroutinesFixedUpdate = &ScriptInstanceManager.PumpCoroutinesFixedUpdate;

            // ── Inspector event bindings (appended for binary compat) ──
            managedCallbacks->GetInvokableMethodsBuffer = &ScriptInstanceManager.GetInvokableMethodsBuffer;
            managedCallbacks->InvokeScriptMethodByName = &ScriptInstanceManager.InvokeScriptMethodByName;

            // ── Window events (appended for binary compat) ──
            managedCallbacks->RaiseWindowResize = &ScriptInstanceManager.RaiseWindowResize;
            managedCallbacks->RaiseEnterChar = &ScriptInstanceManager.RaiseEnterChar;

            // ── Play-mode lifecycle (appended for binary compat) ──
            managedCallbacks->OnPlayModeExited = &ScriptInstanceManager.OnPlayModeExited;

            ScriptInstanceManager.SetCoreAssembly(typeof(ScriptHostBridge).Assembly);

            ComponentTypes.RunAllStaticInitializers();

            SynchronizationContext.SetSynchronizationContext(IndexSynchronizationContext.Instance);
            return 0;
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"ScriptHostBridge.Initialize failed: {ex}");
            return -1;
        }
    }
}
