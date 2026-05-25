"""Inspect a PE32+ DLL to determine if it was linked against debug or release CRT,
based on imported DLLs (ucrtbase.dll = release, ucrtbased.dll = debug)."""

import sys
from pathlib import Path
import struct

def get_imports(path: Path):
    data = path.read_bytes()
    # PE: MZ at 0; e_lfanew at offset 0x3C points to "PE\0\0"
    pe_off = struct.unpack_from("<I", data, 0x3C)[0]
    assert data[pe_off:pe_off+4] == b"PE\0\0", f"Not PE: {path}"
    # COFF header: u16 Machine, u16 NumberOfSections, u32 TimeDateStamp,
    # u32 PointerToSymbolTable, u32 NumberOfSymbols, u16 SizeOfOptionalHeader, u16 Characteristics
    coff = pe_off + 4
    machine, num_sections, time_stamp, _, _, opt_size, characteristics = (
        struct.unpack_from("<HHIIIHH", data, coff)
    )
    opt = coff + 20
    magic = struct.unpack_from("<H", data, opt)[0]
    is_pe32_plus = (magic == 0x20B)
    # ImageBase offset, SizeOfImage offset, etc. ImportTable is in DataDirectory[1].
    # In PE32+ DataDirectory starts at opt + 112; in PE32 at opt + 96.
    dd_off = opt + (112 if is_pe32_plus else 96)
    import_rva, import_size = struct.unpack_from("<II", data, dd_off + 1*8)
    # Sections start at opt + opt_size
    sect_off = opt + opt_size
    sections = []
    for i in range(num_sections):
        s = sect_off + i * 40
        name = data[s:s+8].rstrip(b"\0").decode("ascii", errors="replace")
        virt_size, virt_addr, raw_size, raw_ptr = struct.unpack_from("<IIII", data, s + 8)
        sections.append((name, virt_addr, virt_size, raw_ptr, raw_size))

    def rva_to_off(rva):
        for name, va, vs, rp, rs in sections:
            if va <= rva < va + vs:
                return rp + (rva - va)
        return None

    imports = []
    if import_rva == 0:
        return imports, time_stamp, is_pe32_plus
    off = rva_to_off(import_rva)
    while True:
        # IMAGE_IMPORT_DESCRIPTOR: u32 OriginalFirstThunk, u32 TimeDateStamp,
        # u32 ForwarderChain, u32 Name, u32 FirstThunk
        entry = struct.unpack_from("<IIIII", data, off)
        if all(v == 0 for v in entry):
            break
        name_rva = entry[3]
        name_off = rva_to_off(name_rva)
        # null-terminated ascii
        end = data.index(b"\0", name_off)
        imports.append(data[name_off:end].decode("ascii", errors="replace"))
        off += 20
    return imports, time_stamp, is_pe32_plus

import datetime
for path in [
    Path(r"C:\Users\benso\Index\Projects\New-Game3\Packages\Index.Tilemap2D\Bin\Windows-x64\Pkg.Index.Tilemap2D.Native.dll"),
    Path(r"C:\Users\benso\source\repos\2026\Pending\Index\bin\Release-windows-x86_64\Pkg.Index.Tilemap2D.Native\Pkg.Index.Tilemap2D.Native.dll"),
    Path(r"C:\Users\benso\source\repos\2026\Pending\Index\bin\Release-windows-x86_64\Index-Engine.dll"),
    Path(r"C:\Users\benso\source\repos\2026\Pending\Index\bin\Release-windows-x86_64\Index-Editor\Index-Editor.exe"),
]:
    if not path.exists():
        print(f"-- missing: {path}")
        continue
    imp, ts, x64 = get_imports(path)
    dt = datetime.datetime.utcfromtimestamp(ts)
    has_debug_crt = any("ucrtbased" in i.lower() or "vcruntime140d" in i.lower() or "msvcp140d" in i.lower() for i in imp)
    has_release_crt = any("ucrtbase.dll" in i.lower() or i.lower() == "vcruntime140.dll" or i.lower() == "msvcp140.dll" for i in imp)
    print(f"=== {path.name} ===")
    print(f"  PE32+={x64}  TimeStamp=0x{ts:X} ({dt} UTC)")
    print(f"  CRT={'DEBUG' if has_debug_crt else 'release' if has_release_crt else 'unknown'}")
    interesting = [i for i in imp if any(k in i.lower() for k in ['ucrt', 'vcruntime', 'msvcp', 'index', 'pkg'])]
    print(f"  imports: {interesting}")
    print()
