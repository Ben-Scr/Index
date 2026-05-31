"""Analyze the most recent Index-Editor minidump: where did it crash, and is
the faulting module the freshly-installed package DLL?"""

import struct
from pathlib import Path
import datetime
import glob

DUMP_DIR = Path(r"C:\Users\benso\AppData\Local\CrashDumps")
dumps = sorted(DUMP_DIR.glob("Index-Editor.exe.*.dmp"), key=lambda p: p.stat().st_mtime, reverse=True)
print(f"Found {len(dumps)} editor dumps; analyzing most recent.")

for DUMP in dumps[:3]:
    print(f"\n###################  {DUMP.name}  ###################")
    print(f"mtime = {datetime.datetime.fromtimestamp(DUMP.stat().st_mtime)}")
    data = DUMP.read_bytes()
    assert data[:4] == b"MDMP"
    num_streams, stream_dir_rva = struct.unpack_from("<II", data, 8)
    streams = {}
    for i in range(num_streams):
        off = stream_dir_rva + i * 12
        stype, ssize, srva = struct.unpack_from("<III", data, off)
        streams[stype] = (ssize, srva)

    THREAD_LIST, MODULE_LIST, EXCEPTION_STREAM = 3, 4, 6

    _, ex_rva = streams[EXCEPTION_STREAM]
    thread_id, _, ex_code, ex_flags, ex_record, ex_addr, num_params, _ = struct.unpack_from("<IIIIQQII", data, ex_rva)
    params = struct.unpack_from(f"<{num_params}Q", data, ex_rva + 40)
    ctx_size, ctx_rva = struct.unpack_from("<II", data, ex_rva + 40 + 15*8)

    print(f"thread_id      = {thread_id}")
    print(f"exception_code = 0x{ex_code:08X}")
    print(f"exception_addr = 0x{ex_addr:016X}")
    for i, p in enumerate(params):
        label = ""
        if num_params == 2 and i == 0:
            label = "  (0=read, 1=write, 8=DEP)"
        elif num_params == 2 and i == 1:
            label = "  (faulting address)"
        print(f"  param[{i}]    = 0x{p:016X}{label}")

    _, mod_rva = streams[MODULE_LIST]
    num_modules = struct.unpack_from("<I", data, mod_rva)[0]
    MODULE_SIZE = 108
    modules = []
    for i in range(num_modules):
        base = mod_rva + 4 + i * MODULE_SIZE
        base_addr, size_of_image, checksum, time_stamp, name_rva = struct.unpack_from("<QIIII", data, base)
        name_len = struct.unpack_from("<I", data, name_rva)[0]
        name = data[name_rva + 4 : name_rva + 4 + name_len].decode("utf-16-le")
        modules.append((base_addr, base_addr + size_of_image, name, size_of_image, time_stamp))

    target_module = None
    for base, end, name, sz, ts in modules:
        if base <= ex_addr < end:
            target_module = (base, end, name, sz, ts)
            break

    if target_module:
        base, end, name, sz, ts = target_module
        rva = ex_addr - base
        print(f"** Exception in: {name}")
        print(f"   base = 0x{base:X}  size = 0x{sz:X}  link-stamp = {datetime.datetime.fromtimestamp(ts, datetime.UTC) if ts else 'n/a'}")
        print(f"   RVA  = 0x{rva:X}")
    else:
        print(f"** Exception address 0x{ex_addr:X} NOT IN ANY MODULE (likely freed/invalid memory)")

    bad_addr = params[1] if num_params >= 2 else 0
    bm = next((m for m in modules if m[0] <= bad_addr < m[1]), None)
    if bm:
        print(f"** Bad address 0x{bad_addr:X} is in: {bm[2]} (RVA 0x{bad_addr - bm[0]:X})")
    elif bad_addr:
        print(f"** Bad address 0x{bad_addr:X} is in process heap/stack/freed memory")

    # Look for the package DLL specifically
    print("\nPackage / Index modules loaded at crash time:")
    for base, end, name, sz, ts in modules:
        n_lc = name.lower()
        if "index" in n_lc or "pkg." in n_lc:
            dt = datetime.datetime.fromtimestamp(ts, datetime.UTC) if ts else "n/a"
            print(f"  {Path(name).name:50s} base=0x{base:X} sz=0x{sz:X} link={dt}")

    # Faulting thread Rip
    ctx = ctx_rva
    flags_off = ctx + 6*8
    seg_off = flags_off + 8
    dr_off = seg_off + 12 + 4
    ints_off = dr_off + 6*8
    regs = ["Rax","Rcx","Rdx","Rbx","Rsp","Rbp","Rsi","Rdi","R8","R9","R10","R11","R12","R13","R14","R15","Rip"]
    values = struct.unpack_from(f"<{len(regs)}Q", data, ints_off)
    rip = values[16]
    rip_mod = next((m for m in modules if m[0] <= rip < m[1]), None)
    if rip_mod:
        print(f"\nRIP = 0x{rip:X}  in {rip_mod[2]} (RVA 0x{rip - rip_mod[0]:X})")
    else:
        print(f"\nRIP = 0x{rip:X}  (not in any loaded module)")
