using System;
using System.IO;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace Index.Interop;

// DllImport resolver for Pkg.*.Native DLLs: .NET's default search doesn't find package native siblings. Probe order: <exeDir>/../<lib>/, <exeDir>/Packages/<lib>/, <exeDir>/ (fallback).
internal static class PackageNativeResolver
{
    private const string PackagePrefix = "Pkg.";

    // CA2255 warns about [ModuleInitializer] in library assemblies because the
    // timing isn't obvious to library consumers. Here we *want* exactly that —
    // ScriptCore's runtime users (Index-Sandbox, Pkg.<Name>.dll plug-ins) all
    // benefit from the resolver being installed before any of their P/Invoke
    // calls execute. The behavior is documented at the type level above.
#pragma warning disable CA2255
    [ModuleInitializer]
#pragma warning restore CA2255
    internal static void Init()
    {
        try
        {
            NativeLibrary.SetDllImportResolver(typeof(PackageNativeResolver).Assembly, Resolve);
        }
        catch
        {
            // SetDllImportResolver throws if a resolver is already registered
            // for this assembly. Swallow — first-wins semantics suit us.
        }

        AppDomain.CurrentDomain.AssemblyLoad += static (_, args) =>
        {
            string? name = args.LoadedAssembly.GetName().Name;
            if (name == null || !name.StartsWith(PackagePrefix, StringComparison.Ordinal))
                return;
            try
            {
                NativeLibrary.SetDllImportResolver(args.LoadedAssembly, Resolve);
            }
            catch
            {
                // Idempotent: already registered (e.g. by an earlier
                // ModuleInitializer in this assembly). Ignore.
            }
        };
    }

    private static IntPtr Resolve(string libraryName, Assembly assembly, DllImportSearchPath? searchPath)
    {
        if (string.IsNullOrEmpty(libraryName))
            return IntPtr.Zero;
        if (!libraryName.StartsWith(PackagePrefix, StringComparison.Ordinal))
            return IntPtr.Zero;

        string baseDir = AppContext.BaseDirectory;
        if (string.IsNullOrEmpty(baseDir))
            return IntPtr.Zero;

        // Strip a single trailing dir-sep so GetParent() resolves predictably.
        baseDir = baseDir.TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);

        string? parentDir = Directory.GetParent(baseDir)?.FullName;
        string fileName = libraryName + GetNativeLibraryExtension();

        // Probe order documented in the type comment.
        if (parentDir != null)
        {
            string devCandidate = Path.Combine(parentDir, libraryName, fileName);
            if (TryLoad(devCandidate, out IntPtr handle))
                return handle;
        }

        string shippedCandidate = Path.Combine(baseDir, "Packages", libraryName, fileName);
        if (TryLoad(shippedCandidate, out IntPtr shippedHandle))
            return shippedHandle;

        string flatCandidate = Path.Combine(baseDir, fileName);
        if (TryLoad(flatCandidate, out IntPtr flatHandle))
            return flatHandle;

        return IntPtr.Zero;
    }

    private static bool TryLoad(string path, out IntPtr handle)
    {
        handle = IntPtr.Zero;
        if (!File.Exists(path))
            return false;
        try
        {
            handle = NativeLibrary.Load(path);
            return handle != IntPtr.Zero;
        }
        catch
        {
            return false;
        }
    }

    private static string GetNativeLibraryExtension()
    {
        if (RuntimeInformation.IsOSPlatform(OSPlatform.Windows)) return ".dll";
        if (RuntimeInformation.IsOSPlatform(OSPlatform.OSX)) return ".dylib";
        return ".so";
    }
}
