#pragma once

// EntityScriptOps — routes AddComponent/HasComponent/GetComponent/RemoveComponent for native-script types through NativeScriptHost.
// Cycle-safe: intentionally avoids including Scene.hpp or NativeScript.hpp to prevent a circular dependency via Entity.hpp.

#include "Scene/EntityHandle.hpp"
#include "Scripting/NativeScriptRegistry.hpp"

#include <type_traits>
#include <typeinfo>

namespace Index {
	class Scene;
	class NativeScript;
}

namespace Index::EntityScriptOps::detail {
	NativeScript* AddScriptByName(Scene* scene, EntityHandle entity, const char* name);
	NativeScript* GetNativeScriptOnEntity(Scene* scene, EntityHandle entity, const char* name);
	bool HasNativeScriptOnEntity(Scene* scene, EntityHandle entity, const char* name);
	bool RemoveNativeScriptOnEntity(Scene* scene, EntityHandle entity, const char* name);
}

namespace Index::EntityScriptOps {

	// Helper: lookup the registered string name for T. Returns nullptr
	// when T was never registered via REGISTER_SCRIPT — the call site
	// short-circuits without touching ScriptComponent or the host.
	template<typename T>
	const char* ResolveScriptName() {
		return NativeScriptRegistry::NameOfType(typeid(T));
	}

	template<typename T>
		requires std::is_base_of_v<NativeScript, T>
	T* AddScriptToEntity(Scene* scene, EntityHandle entity) {
		const char* name = ResolveScriptName<T>();
		if (!name) return nullptr;
		NativeScript* ns = detail::AddScriptByName(scene, entity, name);
		return ns ? dynamic_cast<T*>(ns) : nullptr;
	}

	template<typename T>
		requires std::is_base_of_v<NativeScript, T>
	T* GetScriptOnEntity(Scene* scene, EntityHandle entity) {
		const char* name = ResolveScriptName<T>();
		if (!name) return nullptr;
		NativeScript* ns = detail::GetNativeScriptOnEntity(scene, entity, name);
		return ns ? dynamic_cast<T*>(ns) : nullptr;
	}

	template<typename T>
		requires std::is_base_of_v<NativeScript, T>
	bool HasScriptOnEntity(Scene* scene, EntityHandle entity) {
		const char* name = ResolveScriptName<T>();
		if (!name) return false;
		return detail::HasNativeScriptOnEntity(scene, entity, name);
	}

	template<typename T>
		requires std::is_base_of_v<NativeScript, T>
	bool TryGetScriptOnEntity(Scene* scene, EntityHandle entity, T*& out) {
		out = GetScriptOnEntity<T>(scene, entity);
		return out != nullptr;
	}

	template<typename T>
		requires std::is_base_of_v<NativeScript, T>
	bool RemoveScriptFromEntity(Scene* scene, EntityHandle entity) {
		const char* name = ResolveScriptName<T>();
		if (!name) return false;
		return detail::RemoveNativeScriptOnEntity(scene, entity, name);
	}

} // namespace Index::EntityScriptOps
