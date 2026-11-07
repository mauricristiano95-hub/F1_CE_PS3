# Reads guest memory of a running f1ce.exe: python -I tools/peek.py <pid> <hex ea>[:<hex size>] ...
import ctypes, ctypes.wintypes as w, struct, sys

PID = int(sys.argv[1])
k32 = ctypes.WinDLL("kernel32", use_last_error=True)
h = k32.OpenProcess(0x0410, False, PID)  # QUERY_INFORMATION | VM_READ
assert h, ctypes.get_last_error()

class MBI(ctypes.Structure):
    _fields_ = [("BaseAddress", ctypes.c_uint64), ("AllocationBase", ctypes.c_uint64), ("AllocationProtect", w.DWORD),
                ("PartitionId", w.WORD), ("RegionSize", ctypes.c_uint64), ("State", w.DWORD), ("Protect", w.DWORD),
                ("Type", w.DWORD)]
k32.VirtualQueryEx.argtypes = [w.HANDLE, ctypes.c_uint64, ctypes.POINTER(MBI), ctypes.c_size_t]
k32.ReadProcessMemory.argtypes = [w.HANDLE, ctypes.c_uint64, ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]

# find the allocation whose total span is 4 GiB + 0x10000
allocs = {}
addr = 0
m = MBI()
while addr < 0x7FFFFFFFFFFF and k32.VirtualQueryEx(h, addr, ctypes.byref(m), ctypes.sizeof(m)):
    if m.State != 0x10000:
        allocs[m.AllocationBase] = allocs.get(m.AllocationBase, 0) + m.RegionSize
    addr = m.BaseAddress + m.RegionSize
base = [a for a, s in allocs.items() if s == 0x100010000]
assert len(base) == 1, [hex(a) for a, s in allocs.items() if s > 0x80000000]
base = base[0]

def rd(ea, n):
    buf = ctypes.create_string_buffer(n)
    got = ctypes.c_size_t()
    if not k32.ReadProcessMemory(h, base + ea, buf, n, ctypes.byref(got)):
        return None
    return buf.raw

def words(ea, n):
    d = rd(ea, n)
    return None if d is None else struct.unpack(">%dI" % (n // 4), d)

def dump(name, ea, n):
    ws = words(ea, n)
    if ws is None:
        print(f"{name} @{ea:08x}: unreadable"); return
    for i in range(0, len(ws), 8):
        print(f"{name} @{ea + 4*i:08x}: " + " ".join(f"{x:08x}" for x in ws[i:i+8]))

if __name__ == "__main__":
    print(f"guest base {base:x}")
    for a in sys.argv[2:]:
        ea, _, n = a.partition(":")
        dump("mem", int(ea, 16), int(n, 16) if n else 0x80)
