#pragma once
#include "Core/Export.hpp"
#include "Scene/EntityHandle.hpp"
#include "Scripting/DotNetHost.hpp"
#include "Scripting/ScriptGlue.hpp"
#include <cstddef>
#include <string>
#include <cstdint>
#include <vector>

namespace Index {

	class Scene;

	class INDEX_API ScriptEngine {
	public:
		static void Init();
		static void Shutdown();
		static bool IsInitialized();

		static void LoadCoreAssembly(const std::string& path);
		static void LoadUserAssembly(const std::string& path);
		static bool HasUserAssembly();
		static void ReloadAssemblies();
		static std::size_t RestoreDynamicComponentsForScene(Scene& scene);

		static void SetScene(Scene* scene);
		static Scene* GetScene();

		// ── Instance management ────────────────────────────────────────
		static uint32_t CreateScriptInstance(const std::string& className, EntityHandle entity);
		static void DestroyScriptInstance(uint32_t handle);
		static void InvokeStart(uint32_t handle);
		static void InvokeUpdate(uint32_t handle);
		static void InvokeOnDestroy(uint32_t handle);
		static void InvokeOnEnable(uint32_t handle);
		static void InvokeOnDisable(uint32_t handle);
		static void InvokeCollisionEnter2D(uint32_t handle, uint64_t selfEntityID, uint64_t otherEntityID, uint64_t entityAID, uint64_t entityBID, float contactPointX, float contactPointY);
		static void InvokeCollisionStay2D(uint32_t handle, uint64_t selfEntityID, uint64_t otherEntityID, uint64_t entityAID, uint64_t entityBID, float contactPointX, float contactPointY);
		static void InvokeCollisionExit2D(uint32_t handle, uint64_t selfEntityID, uint64_t otherEntityID, uint64_t entityAID, uint64_t entityBID, float contactPointX, float contactPointY);
		static bool ClassExists(const std::string& className);

		static void RaiseApplicationStart();
		static void RaiseApplicationPaused();
		static void RaiseApplicationQuit();
		static void RaiseFocusChanged(bool focused);
		static void RaiseKeyDown(int key);
		static void RaiseKeyUp(int key);
		static void RaiseEnterChar(uint32_t codepoint);
		static void RaiseMouseDown(int button);
		static void RaiseMouseUp(int button);
		static void RaiseMouseScroll(float delta);
		static void RaiseMouseMove(float x, float y);
		static void RaiseBeforeSceneLoaded(const std::string& sceneName);
		static void RaiseSceneLoaded(const std::string& sceneName);
		static void RaiseBeforeSceneUnloaded(const std::string& sceneName);
		static void RaiseSceneUnloaded(const std::string& sceneName);
		static void RaiseUiEventDispatch();
		// Fired by Application::DispatchEvent on WindowResizeEvent — the
		// managed `Index.Window.OnResize` event runs same-frame as GLFW's
		// framebuffer-size callback.
		static void RaiseWindowResize();

		// Strips static-event subscribers from the user assembly; safe to call from runtime (no-op when uninitialized).
		static void OnPlayModeExited();

		static uint32_t CreateSceneScriptInstance(const std::string& className, const std::string& sceneName);
		static void DestroySceneScriptInstance(uint32_t handle);
		static void InvokeSceneScriptStart(uint32_t handle);
		static void InvokeSceneScriptUpdate(uint32_t handle);
		static void InvokeSceneScriptEnable(uint32_t handle);
		static void InvokeSceneScriptDisable(uint32_t handle);
		static void InvokeSceneScriptDestroy(uint32_t handle);
		static bool SceneScriptClassExists(const std::string& className);

		static void InitializeGlobalScripts(const std::vector<std::string>& classNames);
		static void UpdateGlobalScripts();
		static void FixedUpdateGlobalScripts();
		static void ShutdownGlobalScripts();
		static void SetGlobalScriptEnabled(const std::string& className, bool enabled);
		static bool GlobalScriptClassExists(const std::string& className);

		// ── New lifecycle thunks (appended for binary compat) ──
		static void InvokeAwake(uint32_t handle);
		static void InvokeFixedUpdate(uint32_t handle);
		static void InvokeSceneScriptAwake(uint32_t handle);
		static void InvokeSceneScriptFixedUpdate(uint32_t handle);
		static void InvokeGlobalScriptFixedUpdate(uint32_t handle);

		// Returns JSON array of [ShowInEditor] fields; pointer valid until the next field accessor call.
		static const char* GetSceneScriptFields(uint32_t handle);
		static void SetSceneScriptField(uint32_t handle, const char* fieldName, const char* value);

		static void PumpCoroutinesUpdate(float deltaTime);
		static void PumpCoroutinesFixedUpdate();

		static const ManagedCallbacks& GetCallbacks() { return s_Callbacks; }

		struct GlobalScriptInstance {
			std::string ClassName;
			uint32_t Handle = 0;
			bool Enabled = true;
		};

	private:
		static bool s_Initialized;
		static Scene* s_CurrentScene;
		static std::string s_CoreAssemblyPath;
		static std::string s_UserAssemblyPath;
		static bool s_HasUserAssembly;
		static std::vector<GlobalScriptInstance> s_GlobalScripts;

		static DotNetHost s_Host;
		static ManagedCallbacks s_Callbacks;
	};

} // namespace Index
