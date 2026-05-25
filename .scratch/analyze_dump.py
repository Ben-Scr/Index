"""Quick minidump analysis: find which module contains the exception address,
and dump the faulting thread's stack."""

import struct
import sys
from pathlib import Path

DUMP = Path(r"C:\Users\benso\AppData\Local\CrashDumps\Index-Editor.exe.3588.dmp")
data = DUMP.read_bytes()

assert data[:4] == b"MDMP", "Not a minidump"

# Header: u32 Signature, u32 Version, u32 NumberOfStreams, u32 StreamDirRva, ...
num_streams, stream_dir_rva = struct.unpack_from("<II", data, 8)

streams = {}
for i in range(num_streams):
    off = stream_dir_rva + i * 12
    stype, ssize, srva = struct.unpack_from("<III", data, off)
    streams[stype] = (ssize, srva)

# Stream types
THREAD_LIST = 3
MODULE_LIST = 4
MEMORY_LIST = 5
EXCEPTION_STREAM = 6
SYSTEM_INFO = 7
MEMORY64_LIST = 9

# --- exception stream ---
_, ex_rva = streams[EXCEPTION_STREAM]
thread_id, _, ex_code, ex_flags, ex_record, ex_addr, num_params, _ = (
    struct.unpack_from("<IIIIQQII", data, ex_rva)
)
params = struct.unpack_from(f"<{num_params}Q", data, ex_rva + 40)
# Thread context: u32 DataSize, u32 Rva
ctx_size, ctx_rva = struct.unpack_from("<II", data, ex_rva + 40 + 15*8)

print(f"=== EXCEPTION ===")
print(f"thread_id      = {thread_id}")
print(f"exception_code = 0x{ex_code:08X}")
print(f"exception_addr = 0x{ex_addr:016X}")
print(f"num_params     = {num_params}")
for i, p in enumerate(params):
    label = ""
    if num_params == 2 and i == 0:
        label = "  (0=read, 1=write, 8=DEP)"
    elif num_params == 2 and i == 1:
        label = "  (faulting address)"
    print(f"  param[{i}]    = 0x{p:016X}{label}")
print(f"ctx_size = {ctx_size}, ctx_rva = 0x{ctx_rva:X}")

# --- module list ---
_, mod_rva = streams[MODULE_LIST]
num_modules = struct.unpack_from("<I", data, mod_rva)[0]
print(f"\n=== MODULES ({num_modules}) ===")

# MINIDUMP_MODULE is 108 bytes
MODULE_SIZE = 108

target_module = None
modules = []
for i in range(num_modules):
    base = mod_rva + 4 + i * MODULE_SIZE
    base_addr, size_of_image, checksum, time_stamp, name_rva = (
        struct.unpack_from("<QIIII", data, base)
    )
    # Name is MINIDUMP_STRING at name_rva: u32 length (bytes) + UTF-16LE chars
    name_len = struct.unpack_from("<I", data, name_rva)[0]
    name = data[name_rva + 4 : name_rva + 4 + name_len].decode("utf-16-le")
    end_addr = base_addr + size_of_image
    modules.append((base_addr, end_addr, name, size_of_image))
    if base_addr <= ex_addr < end_addr:
        target_module = (base_addr, end_addr, name, size_of_image)

if target_module:
    base, end, name, sz = target_module
    rva = ex_addr - base
    print(f"\n** Exception in: {name}")
    print(f"   base = 0x{base:X}  size = 0x{sz:X}")
    print(f"   RVA  = 0x{rva:X}")
else:
    print(f"\n** Exception address 0x{ex_addr:X} not in any module")

# Look up the bad read address
bad_addr = params[1] if num_params >= 2 else 0
target_addr_module = None
for base, end, name, sz in modules:
    if base <= bad_addr < end:
        target_addr_module = (base, end, name)
        break
if target_addr_module:
    print(f"\n** Bad address 0x{bad_addr:X} is in: {target_addr_module[2]} (RVA 0x{bad_addr - target_addr_module[0]:X})")
else:
    print(f"\n** Bad address 0x{bad_addr:X} is in process heap/stack (not in a module)")

# --- thread context (CONTEXT_AMD64) ---
print(f"\n=== THREAD CONTEXT (offset 0x{ctx_rva:X}) ===")
# Skip P1Home..P6Home (6*Q), ContextFlags (u32), MxCsr (u32),
# SegCs..SegSs (6*u16), EFlags (u32),
# Dr0..Dr7 (8*Q),
# Rax, Rcx, Rdx, Rbx, Rsp, Rbp, Rsi, Rdi, R8..R15, Rip
ctx = ctx_rva
p1_home_off = ctx
flags_off = ctx + 6*8
context_flags = struct.unpack_from("<I", data, flags_off)[0]
print(f"ContextFlags = 0x{context_flags:X}")
seg_off = flags_off + 8  # MxCsr is between
mx_csr = struct.unpack_from("<I", data, flags_off + 4)[0]
print(f"MxCsr = 0x{mx_csr:X}")
# Segments u16 x6
seg_cs, seg_ds, seg_es, seg_fs, seg_gs, seg_ss = struct.unpack_from("<6H", data, seg_off)
eflags = struct.unpack_from("<I", data, seg_off + 12)[0]
print(f"SegCs=0x{seg_cs:X} SegDs=0x{seg_ds:X} EFlags=0x{eflags:X}")

dr_off = seg_off + 12 + 4
# Dr0..Dr7 are 6 dwords actually (Dr0..Dr3 and Dr6, Dr7)? On AMD64 the
# CONTEXT struct has DRs as ULONG64 — 6 entries.
ints_off = dr_off + 6*8
regs = ["Rax","Rcx","Rdx","Rbx","Rsp","Rbp","Rsi","Rdi",
        "R8","R9","R10","R11","R12","R13","R14","R15","Rip"]
values = struct.unpack_from(f"<{len(regs)}Q", data, ints_off)
for name_r, v in zip(regs, values):
    in_mod = ""
    for base, end, name, sz in modules:
        if base <= v < end:
            in_mod = f"  [{name} +0x{v - base:X}]"
            break
    print(f"{name_r:>4} = 0x{v:016X}{in_mod}")

rsp = values[4]
rip = values[16]
print(f"\nRIP = 0x{rip:X}")
print(f"RSP = 0x{rsp:X}")
