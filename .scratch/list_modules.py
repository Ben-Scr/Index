"""List all modules in the minidump matching a pattern."""
import struct
from pathlib import Path

DUMP = Path(r"C:\Users\benso\AppData\Local\CrashDumps\Index-Editor.exe.3588.dmp")
data = DUMP.read_bytes()

# Stream dir
num_streams, dir_rva = struct.unpack_from("<II", data, 8)
streams = {}
for i in range(num_streams):
    stype, ssize, srva = struct.unpack_from("<III", data, dir_rva + i * 12)
    streams[stype] = (ssize, srva)

# Module list (type 4)
_, mod_rva = streams[4]
num = struct.unpack_from("<I", data, mod_rva)[0]
print(f"Modules: {num}")
for i in range(num):
    b = mod_rva + 4 + i * 108
    base, sz, _, ts, name_rva = struct.unpack_from("<QIIII", data, b)
    nlen = struct.unpack_from("<I", data, name_rva)[0]
    name = data[name_rva+4:name_rva+4+nlen].decode("utf-16-le")
    if any(k in name.lower() for k in ['tilemap', 'pkg.', 'index-', 'ucrtbase', 'vcruntime', 'msvcp']):
        print(f"  base=0x{base:X} end=0x{base+sz:X} ({sz/1024:.0f} KB)  {name}")
