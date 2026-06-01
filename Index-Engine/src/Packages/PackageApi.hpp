#pragma once

#if defined(_WIN32)
    #define INDEX_PACKAGE_API __declspec(dllexport)
#else
    #define INDEX_PACKAGE_API __attribute__((visibility("default")))
#endif

extern "C" {

    // Called once after the package's DLL is loaded. Return 0 on success;
    // non-zero is logged but the module stays loaded. Optional — packages
    // without it just appear in the loaded list with no init step.
    INDEX_PACKAGE_API int IndexPackage_OnLoad();

    // Called once before the package's DLL is unloaded (engine shutdown,
    // currently). Optional.
    INDEX_PACKAGE_API void IndexPackage_OnUnload();

}
