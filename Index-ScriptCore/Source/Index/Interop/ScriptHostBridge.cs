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
    public delegate* unmanaged<byte*, byte*, int> CreateSceneSystemInstance;
    public delegate* unmanaged<int, void> DestroySceneSystemInstance;
    public delegate* unmanaged<int, void> InvokeSceneSystemStart;
    public delegate* unmanaged<int, void> InvokeSceneSystemUpdate;
    public delegate* unmanaged<int, void> InvokeSceneSystemEnable;
    public delegate* unmanaged<int, void> InvokeSceneSystemDisable;
    public delegate* unmanaged<int, void> InvokeSceneSystemDestroy;
    public delegate* unmanaged<byte*, int> SceneSystemClassExists;
    public delegate* unmanaged<byte*, int> CreateGlobalSystemInstance;
    public delegate* unmanaged<int, void> DestroyGlobalSystemInstance;
    public delegate* unmanaged<int, void> InvokeGlobalSystemInitialize;
    public delegate* unmanaged<int, void> InvokeGlobalSystemUpdate;
    public delegate* unmanaged<int, void> InvokeGlobalSystemEnable;
    public delegate* unmanaged<int, void> InvokeGlobalSystemDisable;
    public delegate* unmanaged<byte*, int> GlobalSystemClassExists;

    // ── New lifecycle slots (appended for binary compat) ──
    public delegate* unmanaged<int, void> InvokeAwake;
    public delegate* unmanaged<int, void> InvokeFixedUpdate;
    public delegate* unmanaged<int, void> InvokeSceneSystemAwake;
    public delegate* unmanaged<int, void> InvokeSceneSystemFixedUpdate;
    public delegate* unmanaged<int, void> InvokeGlobalSystemFixedUpdate;

    // ── SceneSystem field reflection (appended for binary compat) ──
    public delegate* unmanaged<int, byte*> GetSceneSystemFields;
    public delegate* unmanaged<int, byte*, byte*, void> SetSceneSystemField;

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

    // ── DataAsset (appended for binary compat) ──
    // Order must match ScriptGlue.hpp's ManagedCallbacks struct exactly.
    public delegate* unmanaged<byte*, ulong, int> CreateDataAssetInstance;
    public delegate* unmanaged<ulong, void> DestroyDataAssetInstance;
    public delegate* unmanaged<ulong, byte*> GetDataAssetFields;
    public delegate* unmanaged<ulong, byte*, byte*, void> SetDataAssetField;
    public delegate* unmanaged<byte*, int> DataAssetClassExists;
    public delegate* unmanaged<byte*, int, int> GetDataAssetTypes;
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
            managedCallbacks->CreateSceneSystemInstance = &ScriptInstanceManager.CreateSceneSystemInstance;
            managedCallbacks->DestroySceneSystemInstance = &ScriptInstanceManager.DestroySceneSystemInstance;
            managedCallbacks->InvokeSceneSystemStart = &ScriptInstanceManager.InvokeSceneSystemStart;
            managedCallbacks->InvokeSceneSystemUpdate = &ScriptInstanceManager.InvokeSceneSystemUpdate;
            managedCallbacks->InvokeSceneSystemEnable = &ScriptInstanceManager.InvokeSceneSystemEnable;
            managedCallbacks->InvokeSceneSystemDisable = &ScriptInstanceManager.InvokeSceneSystemDisable;
            managedCallbacks->InvokeSceneSystemDestroy = &ScriptInstanceManager.InvokeSceneSystemDestroy;
            managedCallbacks->SceneSystemClassExists = &ScriptInstanceManager.SceneSystemClassExists;
            managedCallbacks->CreateGlobalSystemInstance = &ScriptInstanceManager.CreateGlobalSystemInstance;
            managedCallbacks->DestroyGlobalSystemInstance = &ScriptInstanceManager.DestroyGlobalSystemInstance;
            managedCallbacks->InvokeGlobalSystemInitialize = &ScriptInstanceManager.InvokeGlobalSystemInitialize;
            managedCallbacks->InvokeGlobalSystemUpdate = &ScriptInstanceManager.InvokeGlobalSystemUpdate;
            managedCallbacks->InvokeGlobalSystemEnable = &ScriptInstanceManager.InvokeGlobalSystemEnable;
            managedCallbacks->InvokeGlobalSystemDisable = &ScriptInstanceManager.InvokeGlobalSystemDisable;
            managedCallbacks->GlobalSystemClassExists = &ScriptInstanceManager.GlobalSystemClassExists;

            // ── New lifecycle slots (appended for binary compat) ──
            managedCallbacks->InvokeAwake = &ScriptInstanceManager.InvokeAwake;
            managedCallbacks->InvokeFixedUpdate = &ScriptInstanceManager.InvokeFixedUpdate;
            managedCallbacks->InvokeSceneSystemAwake = &ScriptInstanceManager.InvokeSceneSystemAwake;
            managedCallbacks->InvokeSceneSystemFixedUpdate = &ScriptInstanceManager.InvokeSceneSystemFixedUpdate;
            managedCallbacks->InvokeGlobalSystemFixedUpdate = &ScriptInstanceManager.InvokeGlobalSystemFixedUpdate;

            // ── SceneSystem field reflection (appended for binary compat) ──
            managedCallbacks->GetSceneSystemFields = &ScriptInstanceManager.GetSceneSystemFields;
            managedCallbacks->SetSceneSystemField = &ScriptInstanceManager.SetSceneSystemField;

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

            // ── DataAsset (appended for binary compat) ──
            managedCallbacks->CreateDataAssetInstance = &ScriptInstanceManager.CreateDataAssetInstance;
            managedCallbacks->DestroyDataAssetInstance = &ScriptInstanceManager.DestroyDataAssetInstance;
            managedCallbacks->GetDataAssetFields = &ScriptInstanceManager.GetDataAssetFields;
            managedCallbacks->SetDataAssetField = &ScriptInstanceManager.SetDataAssetField;
            managedCallbacks->DataAssetClassExists = &ScriptInstanceManager.DataAssetClassExists;
            managedCallbacks->GetDataAssetTypes = &ScriptInstanceManager.GetDataAssetTypesBuffer;

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
