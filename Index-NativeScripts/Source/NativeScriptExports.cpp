#include <Scripting/NativeScript.hpp>

#if defined(_WIN32)
#define INDEX_NATIVE_SCRIPT_EXPORT extern "C" __declspec(dllexport)
#elif defined(__GNUC__)
#define INDEX_NATIVE_SCRIPT_EXPORT extern "C" __attribute__((visibility("default")))
#else
#define INDEX_NATIVE_SCRIPT_EXPORT extern "C"
#endif

INDEX_NATIVE_SCRIPT_EXPORT void IndexInitialize(void* engineAPI) {
	Index::g_EngineAPI = static_cast<Index::NativeEngineAPI*>(engineAPI);
}

INDEX_NATIVE_SCRIPT_EXPORT Index::NativeScript* IndexCreateScript(const char* className) {
	return Index::NativeScriptRegistry::Create(className);
}

INDEX_NATIVE_SCRIPT_EXPORT int IndexHasScript(const char* className) {
	return Index::NativeScriptRegistry::Has(className) ? 1 : 0;
}

INDEX_NATIVE_SCRIPT_EXPORT void IndexDestroyScript(Index::NativeScript* script) {
	delete script;
}
