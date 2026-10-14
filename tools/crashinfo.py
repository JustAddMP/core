#!/usr/bin/env python3
"""MafiaCoop crash triage: read a minidump written by the dll's crash handler and print what happened.

Usage:
    python tools\\crashinfo.py [DUMP | --latest] [--log FILE] [--symbols DIR]... [--force-symbols]
                               [--all-threads] [--max-frames N] [--no-scan]

DUMP defaults to the newest out\\logs\\crash-*.dmp (same as --latest). Prints:
  - exception code, flags, address and access-violation details,
  - the faulting module + offset and the registers,
  - the faulting thread's stack, unwound with the x64 unwind data (.pdata/.xdata) of the module images on disk;
    where that fails, the rest of the stack memory is scanned for return addresses into known modules (marked
    "scanned"),
  - symbols (function, file:line, inlined frames) for every module whose matching PDB is in out\\ (or a --symbols
    dir), via dbghelp.dll; everything else (the game exe, Windows) as module+offset,
  - the crash block of the matching log (found automatically by the dump's file name, or --log).

A PDB is only used when its image's CodeView signature (GUID + age) matches the module recorded in the dump: a
rebuilt mafiacoop.dll no longer matches an older dump. --force-symbols uses the PDB anyway (marked "unverified";
only meaningful if the code layout did not change, e.g. the same commit rebuilt). Keep the out\\ folder (or the
release's PDBs) of a build if its dumps need triage later.

Python 3.11 standard library + ctypes only (dbghelp.dll from Windows).
"""
from __future__ import annotations

import argparse
import ctypes
import ctypes.wintypes as wt
import glob
import os
import re
import struct
import sys
import uuid
from dataclasses import dataclass, field

HERE = os.path.dirname(os.path.abspath(__file__))
OUT_DIR = os.path.normpath(os.path.join(HERE, "..", "out"))

# ---------------------------------------------------------------------------------------------------------------------
# Minidump reader (MINIDUMP_* structures from minidumpapiset.h)

STREAM_THREAD_LIST = 3
STREAM_MODULE_LIST = 4
STREAM_MEMORY_LIST = 5
STREAM_EXCEPTION = 6
STREAM_SYSTEM_INFO = 7
STREAM_MEMORY64_LIST = 9
STREAM_UNLOADED_MODULES = 14
STREAM_MISC_INFO = 15

EXCEPTION_NAMES = {
    0xC0000005: "ACCESS_VIOLATION",
    0xC00000FD: "STACK_OVERFLOW",
    0xC000001D: "ILLEGAL_INSTRUCTION",
    0xC0000094: "INT_DIVIDE_BY_ZERO",
    0xC0000096: "PRIV_INSTRUCTION",
    0xC0000006: "IN_PAGE_ERROR",
    0x80000003: "BREAKPOINT",
    0xC0000374: "HEAP_CORRUPTION",
    0xC0000409: "STACK_BUFFER_OVERRUN (fail-fast)",
    0xC0000025: "NONCONTINUABLE_EXCEPTION",
    0xC000008E: "FLOAT_DIVIDE_BY_ZERO",
    0xE06D7363: "C++ EXCEPTION",
    0x406D1388: "SET_THREAD_NAME",
}

# x64 CONTEXT: integer registers in unwind-code order (RAX RCX RDX RBX RSP RBP RSI RDI R8..R15) start at 0x78.
REG_NAMES = ["rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
             "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15"]
CTX_GPR_OFFSET = 0x78
CTX_RIP_OFFSET = 0xF8
CTX_EFLAGS_OFFSET = 0x44


@dataclass
class Module:
    base: int
    size: int
    checksum: int
    timestamp: int
    path: str
    cv: bytes  # raw CodeView record (RSDS)

    @property
    def name(self) -> str:
        return os.path.basename(self.path)

    @property
    def end(self) -> int:
        return self.base + self.size

    def cv_signature(self):
        """(GUID string, age, pdb file name) from an RSDS record, or None."""
        return parse_rsds(self.cv)


@dataclass
class Thread:
    tid: int
    teb: int
    stack_base: int  # lowest address of the captured stack memory
    stack_size: int
    context: bytes


@dataclass
class Context:
    regs: list[int]  # 16 GPRs in unwind order
    rip: int
    eflags: int = 0

    @staticmethod
    def from_bytes(raw: bytes) -> "Context":
        regs = list(struct.unpack_from("<16Q", raw, CTX_GPR_OFFSET))
        rip = struct.unpack_from("<Q", raw, CTX_RIP_OFFSET)[0]
        eflags = struct.unpack_from("<I", raw, CTX_EFLAGS_OFFSET)[0]
        return Context(regs, rip, eflags)

    def copy(self) -> "Context":
        return Context(list(self.regs), self.rip, self.eflags)

    @property
    def rsp(self) -> int:
        return self.regs[4]

    @rsp.setter
    def rsp(self, v: int) -> None:
        self.regs[4] = v & 0xFFFFFFFFFFFFFFFF


def parse_rsds(cv: bytes):
    if len(cv) < 24 or cv[:4] != b"RSDS":
        return None
    guid = uuid.UUID(bytes_le=cv[4:20])
    age = struct.unpack_from("<I", cv, 20)[0]
    pdb = cv[24:].split(b"\0", 1)[0].decode("utf-8", "replace")
    return str(guid).upper(), age, pdb


class Minidump:
    def __init__(self, path: str):
        self.path = path
        with open(path, "rb") as f:
            self.data = f.read()
        d = self.data
        sig, ver, nstreams, dir_rva, _checksum, self.timestamp, self.flags = struct.unpack_from("<4sIIIIIQ", d, 0)
        if sig != b"MDMP":
            raise ValueError(f"{path}: not a minidump (signature {sig!r})")
        self.streams: dict[int, tuple[int, int]] = {}
        for i in range(nstreams):
            stype, size, rva = struct.unpack_from("<III", d, dir_rva + 12 * i)
            if stype not in self.streams:
                self.streams[stype] = (rva, size)
        self.modules = self._read_modules()
        self.unloaded = self._read_unloaded()
        self.threads = self._read_threads()
        self.memory = self._read_memory()
        self.exception = self._read_exception()
        self.system = self._read_system_info()

    # -- streams --
    def _string(self, rva: int) -> str:
        n = struct.unpack_from("<I", self.data, rva)[0]
        return self.data[rva + 4: rva + 4 + n].decode("utf-16-le", "replace")

    def _read_modules(self) -> list[Module]:
        if STREAM_MODULE_LIST not in self.streams:
            return []
        rva, _ = self.streams[STREAM_MODULE_LIST]
        count = struct.unpack_from("<I", self.data, rva)[0]
        mods = []
        for i in range(count):
            off = rva + 4 + 108 * i
            base, size, checksum, ts, name_rva = struct.unpack_from("<QIIII", self.data, off)
            cv_size, cv_rva = struct.unpack_from("<II", self.data, off + 24 + 52)
            cv = self.data[cv_rva: cv_rva + cv_size] if cv_size else b""
            mods.append(Module(base, size, checksum, ts, self._string(name_rva), cv))
        mods.sort(key=lambda m: m.base)
        return mods

    def _read_unloaded(self) -> list[tuple[int, int, str]]:
        if STREAM_UNLOADED_MODULES not in self.streams:
            return []
        rva, _ = self.streams[STREAM_UNLOADED_MODULES]
        hdr_size, entry_size, count = struct.unpack_from("<III", self.data, rva)
        out = []
        for i in range(count):
            off = rva + hdr_size + entry_size * i
            base, size, _cs, _ts, name_rva = struct.unpack_from("<QIIII", self.data, off)
            out.append((base, size, self._string(name_rva)))
        return out

    def _read_threads(self) -> dict[int, Thread]:
        if STREAM_THREAD_LIST not in self.streams:
            return {}
        rva, _ = self.streams[STREAM_THREAD_LIST]
        count = struct.unpack_from("<I", self.data, rva)[0]
        threads = {}
        for i in range(count):
            off = rva + 4 + 48 * i
            tid, _susp, _pc, _prio, teb, stack_start, stack_size, _stack_rva, ctx_size, ctx_rva = \
                struct.unpack_from("<IIIIQQIIII", self.data, off)
            threads[tid] = Thread(tid, teb, stack_start, stack_size, self.data[ctx_rva: ctx_rva + ctx_size])
        return threads

    def _read_memory(self) -> list[tuple[int, int, int]]:
        """Sorted (start, size, file offset) ranges from both memory list streams."""
        ranges = []
        if STREAM_MEMORY_LIST in self.streams:
            rva, _ = self.streams[STREAM_MEMORY_LIST]
            count = struct.unpack_from("<I", self.data, rva)[0]
            for i in range(count):
                start, size, mrva = struct.unpack_from("<QII", self.data, rva + 4 + 16 * i)
                ranges.append((start, size, mrva))
        if STREAM_MEMORY64_LIST in self.streams:
            rva, _ = self.streams[STREAM_MEMORY64_LIST]
            count, base_rva = struct.unpack_from("<QQ", self.data, rva)
            cur = base_rva
            for i in range(count):
                start, size = struct.unpack_from("<QQ", self.data, rva + 16 + 16 * i)
                ranges.append((start, size, cur))
                cur += size
        ranges.sort()
        return ranges

    def _read_exception(self):
        if STREAM_EXCEPTION not in self.streams:
            return None
        rva, _ = self.streams[STREAM_EXCEPTION]
        tid = struct.unpack_from("<I", self.data, rva)[0]
        code, flags, _rec, addr, nparams = struct.unpack_from("<IIQQI", self.data, rva + 8)
        params = struct.unpack_from("<15Q", self.data, rva + 8 + 32)[:nparams]
        ctx_size, ctx_rva = struct.unpack_from("<II", self.data, rva + 8 + 152)
        return {"tid": tid, "code": code, "flags": flags, "address": addr, "params": list(params),
                "context": self.data[ctx_rva: ctx_rva + ctx_size]}

    def _read_system_info(self):
        if STREAM_SYSTEM_INFO not in self.streams:
            return None
        rva, _ = self.streams[STREAM_SYSTEM_INFO]
        arch, _level, _rev, ncpu, _ptype, major, minor, build = struct.unpack_from("<HHHBBIII", self.data, rva)
        return {"arch": arch, "cpus": ncpu, "version": f"{major}.{minor}.{build}"}

    # -- memory access --
    def read(self, addr: int, size: int) -> bytes | None:
        import bisect
        i = bisect.bisect_right(self.memory, (addr, float("inf"), 0)) - 1
        if i < 0:
            return None
        start, rsize, off = self.memory[i]
        if addr < start or addr + size > start + rsize:
            return None
        return self.data[off + (addr - start): off + (addr - start) + size]

    def read_u64(self, addr: int) -> int | None:
        b = self.read(addr, 8)
        return None if b is None else struct.unpack("<Q", b)[0]

    def module_at(self, addr: int) -> Module | None:
        for m in self.modules:
            if m.base <= addr < m.end:
                return m
        return None


# ---------------------------------------------------------------------------------------------------------------------
# PE images on disk: sections, .pdata, unwind info, CodeView signature


class PeImage:
    def __init__(self, path: str):
        self.path = path
        with open(path, "rb") as f:
            self.data = f.read()
        d = self.data
        if d[:2] != b"MZ":
            raise ValueError("not a PE file")
        pe = struct.unpack_from("<I", d, 0x3C)[0]
        if d[pe:pe + 4] != b"PE\0\0":
            raise ValueError("no PE signature")
        self.machine, nsect, self.timestamp, _, _, opt_size, _ = struct.unpack_from("<HHIIIHH", d, pe + 4)
        opt = pe + 24
        magic = struct.unpack_from("<H", d, opt)[0]
        if magic != 0x20B:
            raise ValueError("not a PE32+ image")
        self.size_of_image = struct.unpack_from("<I", d, opt + 56)[0]
        ndirs = struct.unpack_from("<I", d, opt + 108)[0]
        self.dirs = [struct.unpack_from("<II", d, opt + 112 + 8 * i) for i in range(min(ndirs, 16))]
        self.sections = []
        sec = opt + opt_size
        for i in range(nsect):
            name, vsize, va, rawsize, rawptr = struct.unpack_from("<8sIIII", d, sec + 40 * i)
            chars = struct.unpack_from("<I", d, sec + 40 * i + 36)[0]
            self.sections.append((name.rstrip(b"\0").decode("ascii", "replace"), va, max(vsize, rawsize),
                                  rawptr, rawsize, chars))
        self._pdata = None

    def rva_to_off(self, rva: int) -> int | None:
        for _name, va, vsize, rawptr, rawsize, _ch in self.sections:
            if va <= rva < va + vsize:
                if rva - va >= rawsize:
                    return None
                return rawptr + (rva - va)
        return None

    def read_rva(self, rva: int, size: int) -> bytes | None:
        off = self.rva_to_off(rva)
        if off is None or off + size > len(self.data):
            return None
        return self.data[off: off + size]

    def is_code(self, rva: int) -> bool:
        for _name, va, vsize, _rp, _rs, chars in self.sections:
            if va <= rva < va + vsize:
                return bool(chars & 0x20000000)  # IMAGE_SCN_MEM_EXECUTE
        return False

    def cv_signature(self):
        if len(self.dirs) <= 6:
            return None
        rva, size = self.dirs[6]
        for i in range(size // 28):
            ent = self.read_rva(rva + 28 * i, 28)
            if not ent:
                break
            _ch, _ts, _maj, _min, typ, dsize, draw, dptr = struct.unpack("<IIHHIIII", ent)
            if typ == 2 and dptr:  # IMAGE_DEBUG_TYPE_CODEVIEW
                return parse_rsds(self.data[dptr: dptr + dsize])
        return None

    def runtime_functions(self) -> list[tuple[int, int, int]]:
        if self._pdata is None:
            self._pdata = []
            if len(self.dirs) > 3 and self.dirs[3][1]:
                rva, size = self.dirs[3]
                raw = self.read_rva(rva, size) or b""
                self._pdata = [struct.unpack_from("<III", raw, 12 * i) for i in range(len(raw) // 12)]
        return self._pdata

    def lookup_function(self, rva: int):
        import bisect
        funcs = self.runtime_functions()
        i = bisect.bisect_right(funcs, (rva, 0xFFFFFFFF, 0xFFFFFFFF)) - 1
        if i >= 0 and funcs[i][0] <= rva < funcs[i][1]:
            return funcs[i]
        return None


# ---------------------------------------------------------------------------------------------------------------------
# x64 unwinder (UNWIND_INFO / UNWIND_CODE as documented in "x64 exception handling")

UWOP_PUSH_NONVOL, UWOP_ALLOC_LARGE, UWOP_ALLOC_SMALL, UWOP_SET_FPREG = 0, 1, 2, 3
UWOP_SAVE_NONVOL, UWOP_SAVE_NONVOL_FAR, UWOP_EPILOG, UWOP_SPARE = 4, 5, 6, 7
UWOP_SAVE_XMM128, UWOP_SAVE_XMM128_FAR, UWOP_PUSH_MACHFRAME = 8, 9, 10
UNW_FLAG_CHAININFO = 4


class UnwindError(Exception):
    pass


def code_slots(op: int, info: int) -> int:
    if op == UWOP_ALLOC_LARGE:
        return 2 if info == 0 else 3
    if op in (UWOP_SAVE_NONVOL, UWOP_SAVE_XMM128, UWOP_EPILOG):
        return 2
    if op in (UWOP_SAVE_NONVOL_FAR, UWOP_SAVE_XMM128_FAR):
        return 3
    return 1


def virtual_unwind(dump: Minidump, img: PeImage, base: int, func, ctx: Context, first_frame: bool) -> Context:
    """Returns the caller's context. Raises UnwindError when the stack memory is missing or the data is odd."""
    new = ctx.copy()
    rva = ctx.rip - base

    def mem(addr):
        v = dump.read_u64(addr)
        if v is None:
            raise UnwindError(f"stack memory 0x{addr:x} not in dump")
        return v

    # Epilog emulation (only the trivial "ret" case: frame 0 stopped exactly on it).
    code = img.read_rva(rva, 1)
    if first_frame and code == b"\xC3":
        new.rip = mem(new.rsp)
        new.rsp = new.rsp + 8
        return new

    begin, _end, info_rva = func
    machframe = False
    chained = False
    while True:
        hdr = img.read_rva(info_rva, 4)
        if not hdr:
            raise UnwindError("unwind info outside the image")
        ver_flags, prolog_size, count, frame = hdr
        version, flags = ver_flags & 7, ver_flags >> 3
        if version not in (1, 2):
            raise UnwindError(f"unwind info version {version}")
        frame_reg, frame_off = frame & 15, frame >> 4
        codes = img.read_rva(info_rva + 4, 2 * count) or b""
        offset_in_func = rva - begin
        i = 0
        while i < count:
            code_off, opinfo = codes[2 * i], codes[2 * i + 1]
            op, info = opinfo & 15, opinfo >> 4
            slots = code_slots(op, info)
            # In the prolog of the frame we stopped in, only the operations already executed count.
            applies = chained or not first_frame or offset_in_func >= prolog_size or code_off <= offset_in_func
            if version == 2 and op == UWOP_EPILOG:
                applies = False
            if applies:
                if op == UWOP_PUSH_NONVOL:
                    new.regs[info] = mem(new.rsp)
                    new.rsp = new.rsp + 8
                elif op == UWOP_ALLOC_LARGE:
                    if info == 0:
                        size = struct.unpack_from("<H", codes, 2 * i + 2)[0] * 8
                    else:
                        size = struct.unpack_from("<I", codes, 2 * i + 2)[0]
                    new.rsp = new.rsp + size
                elif op == UWOP_ALLOC_SMALL:
                    new.rsp = new.rsp + info * 8 + 8
                elif op == UWOP_SET_FPREG:
                    new.rsp = new.regs[frame_reg] - frame_off * 16
                elif op == UWOP_SAVE_NONVOL:
                    new.regs[info] = mem(new.rsp + struct.unpack_from("<H", codes, 2 * i + 2)[0] * 8)
                elif op == UWOP_SAVE_NONVOL_FAR:
                    new.regs[info] = mem(new.rsp + struct.unpack_from("<I", codes, 2 * i + 2)[0])
                elif op in (UWOP_SAVE_XMM128, UWOP_SAVE_XMM128_FAR):
                    pass  # vector registers are not needed for the walk
                elif op == UWOP_PUSH_MACHFRAME:
                    sp = new.rsp + (8 if info else 0)
                    new.rip = mem(sp)
                    new.rsp = mem(sp + 24)
                    machframe = True
                else:
                    raise UnwindError(f"unknown unwind op {op}")
            i += slots
        if flags & UNW_FLAG_CHAININFO:
            off = 4 + 2 * (count + (count & 1))
            chain = img.read_rva(info_rva + off, 12)
            if not chain:
                raise UnwindError("bad chained unwind info")
            begin, _end, info_rva = struct.unpack("<III", chain)
            chained = True
            continue
        break
    if not machframe:
        new.rip = mem(new.rsp)
        new.rsp = new.rsp + 8
    return new


# ---------------------------------------------------------------------------------------------------------------------
# dbghelp symbolisation


class SYMBOL_INFOW(ctypes.Structure):
    _fields_ = [("SizeOfStruct", wt.ULONG), ("TypeIndex", wt.ULONG), ("Reserved", ctypes.c_uint64 * 2),
                ("Index", wt.ULONG), ("Size", wt.ULONG), ("ModBase", ctypes.c_uint64), ("Flags", wt.ULONG),
                ("Value", ctypes.c_uint64), ("Address", ctypes.c_uint64), ("Register", wt.ULONG),
                ("Scope", wt.ULONG), ("Tag", wt.ULONG), ("NameLen", wt.ULONG), ("MaxNameLen", wt.ULONG),
                ("Name", wt.WCHAR * 1)]


class IMAGEHLP_LINEW64(ctypes.Structure):
    _fields_ = [("SizeOfStruct", wt.DWORD), ("Key", ctypes.c_void_p), ("LineNumber", wt.DWORD),
                ("FileName", ctypes.c_wchar_p), ("Address", ctypes.c_uint64)]


SYMOPT_UNDNAME = 0x2
SYMOPT_DEFERRED_LOADS = 0x4
SYMOPT_LOAD_LINES = 0x10
SYMOPT_LOAD_ANYTHING = 0x40
SYMOPT_FAIL_CRITICAL_ERRORS = 0x200
SYMOPT_EXACT_SYMBOLS = 0x400
SYMOPT_NO_PROMPTS = 0x80000


class Symbols:
    MAX_NAME = 1024

    def __init__(self, search_dirs: list[str], exact: bool):
        path = os.path.join(os.environ.get("SystemRoot", r"C:\Windows"), "System32", "dbghelp.dll")
        self.dbg = ctypes.WinDLL(path, use_last_error=True)
        d = self.dbg
        d.SymSetOptions.argtypes = [wt.DWORD]
        d.SymSetOptions.restype = wt.DWORD
        d.SymInitializeW.argtypes = [wt.HANDLE, wt.LPCWSTR, wt.BOOL]
        d.SymInitializeW.restype = wt.BOOL
        d.SymLoadModuleExW.argtypes = [wt.HANDLE, wt.HANDLE, wt.LPCWSTR, wt.LPCWSTR, ctypes.c_uint64, wt.DWORD,
                                       ctypes.c_void_p, wt.DWORD]
        d.SymLoadModuleExW.restype = ctypes.c_uint64
        d.SymFromAddrW.argtypes = [wt.HANDLE, ctypes.c_uint64, ctypes.POINTER(ctypes.c_uint64), ctypes.c_void_p]
        d.SymFromAddrW.restype = wt.BOOL
        d.SymGetLineFromAddrW64.argtypes = [wt.HANDLE, ctypes.c_uint64, ctypes.POINTER(wt.DWORD),
                                            ctypes.POINTER(IMAGEHLP_LINEW64)]
        d.SymGetLineFromAddrW64.restype = wt.BOOL
        d.SymCleanup.argtypes = [wt.HANDLE]
        self.inline = hasattr(d, "SymAddrIncludeInlineTrace")
        if self.inline:
            d.SymAddrIncludeInlineTrace.argtypes = [wt.HANDLE, ctypes.c_uint64]
            d.SymAddrIncludeInlineTrace.restype = wt.DWORD
            d.SymQueryInlineTrace.argtypes = [wt.HANDLE, ctypes.c_uint64, wt.DWORD, ctypes.c_uint64,
                                              ctypes.c_uint64, ctypes.POINTER(wt.DWORD), ctypes.POINTER(wt.DWORD)]
            d.SymQueryInlineTrace.restype = wt.BOOL
            d.SymFromInlineContextW.argtypes = [wt.HANDLE, ctypes.c_uint64, wt.ULONG,
                                                ctypes.POINTER(ctypes.c_uint64), ctypes.c_void_p]
            d.SymFromInlineContextW.restype = wt.BOOL
            d.SymGetLineFromInlineContextW.argtypes = [wt.HANDLE, ctypes.c_uint64, wt.ULONG, ctypes.c_uint64,
                                                       ctypes.POINTER(wt.DWORD), ctypes.POINTER(IMAGEHLP_LINEW64)]
            d.SymGetLineFromInlineContextW.restype = wt.BOOL
        opts = SYMOPT_UNDNAME | SYMOPT_LOAD_LINES | SYMOPT_FAIL_CRITICAL_ERRORS | SYMOPT_NO_PROMPTS
        opts |= SYMOPT_EXACT_SYMBOLS if exact else SYMOPT_LOAD_ANYTHING
        d.SymSetOptions(opts)
        self.handle = wt.HANDLE(0x4D43)  # any unique value: no live process behind it
        if not d.SymInitializeW(self.handle, ";".join(search_dirs), False):
            raise OSError(ctypes.get_last_error(), "SymInitializeW failed")

    def load(self, image_path: str, module_name: str, base: int, size: int) -> bool:
        r = self.dbg.SymLoadModuleExW(self.handle, None, image_path, module_name, base, size, None, 0)
        return bool(r) or ctypes.get_last_error() == 0

    def _symbol_buffer(self):
        buf = ctypes.create_string_buffer(ctypes.sizeof(SYMBOL_INFOW) + 2 * self.MAX_NAME)
        info = ctypes.cast(buf, ctypes.POINTER(SYMBOL_INFOW)).contents
        info.SizeOfStruct = ctypes.sizeof(SYMBOL_INFOW)
        info.MaxNameLen = self.MAX_NAME
        return buf, info

    @staticmethod
    def _name(buf, info) -> str:
        off = SYMBOL_INFOW.Name.offset
        return bytes(buf[off: off + 2 * info.NameLen]).decode("utf-16-le", "replace")

    def _line(self, addr: int, inline_ctx: int | None, base: int) -> str:
        line = IMAGEHLP_LINEW64()
        line.SizeOfStruct = ctypes.sizeof(IMAGEHLP_LINEW64)
        disp = wt.DWORD(0)
        if inline_ctx is None:
            ok = self.dbg.SymGetLineFromAddrW64(self.handle, addr, ctypes.byref(disp), ctypes.byref(line))
        else:
            ok = self.dbg.SymGetLineFromInlineContextW(self.handle, addr, inline_ctx, base, ctypes.byref(disp),
                                                       ctypes.byref(line))
        if not ok or not line.FileName:
            return ""
        return f"{line.FileName}:{line.LineNumber}"

    def resolve(self, addr: int, base: int) -> list[tuple[str, int, str]]:
        """[(function, displacement, file:line)], innermost (inlined) first; empty if unknown."""
        frames = []
        if self.inline:
            n = self.dbg.SymAddrIncludeInlineTrace(self.handle, addr)
            ctx, idx = wt.DWORD(0), wt.DWORD(0)
            if n and self.dbg.SymQueryInlineTrace(self.handle, addr, 0, addr, addr, ctypes.byref(ctx),
                                                  ctypes.byref(idx)):
                for i in range(n):
                    buf, info = self._symbol_buffer()
                    disp = ctypes.c_uint64(0)
                    if self.dbg.SymFromInlineContextW(self.handle, addr, ctx.value + i, ctypes.byref(disp), buf):
                        frames.append((self._name(buf, info), disp.value,
                                       self._line(addr, ctx.value + i, base)))
        buf, info = self._symbol_buffer()
        disp = ctypes.c_uint64(0)
        if self.dbg.SymFromAddrW(self.handle, addr, ctypes.byref(disp), buf):
            frames.append((self._name(buf, info), disp.value, self._line(addr, None, base)))
        return frames


# ---------------------------------------------------------------------------------------------------------------------
# Triage


@dataclass
class ModInfo:
    module: Module
    image: PeImage | None = None  # disk image used for unwind data (must match the dump's timestamp/size)
    image_note: str = ""
    sym_image: str | None = None  # image in a symbols dir whose CodeView matches (or forced)
    sym_note: str = ""
    sym_loaded: bool = False


@dataclass
class Frame:
    rip: int
    rsp: int
    how: str  # "context", "unwind", "leaf", "scanned"
    notes: list[str] = field(default_factory=list)


class Triage:
    def __init__(self, dump: Minidump, sym_dirs: list[str], force_symbols: bool):
        self.dump = dump
        self.sym_dirs = sym_dirs
        self.force = force_symbols
        self.info: dict[int, ModInfo] = {m.base: ModInfo(m) for m in dump.modules}
        self.sym_index = self._index_symbol_images()
        self.symbols: Symbols | None = None
        try:
            self.symbols = Symbols(sym_dirs, exact=not force_symbols)
        except OSError as e:
            print(f"warning: dbghelp unavailable ({e}); no symbols", file=sys.stderr)
        # Modules that have something in the symbol dirs are prepared now (symbols loaded, mismatches noted);
        # the rest (game exe, Windows) only when a frame lands in them.
        by_name, _ = self.sym_index
        for m in dump.modules:
            sig = m.cv_signature()
            pdb_stem = os.path.splitext(os.path.basename(sig[2].replace("/", "\\")))[0].lower() if sig else ""
            stems = {os.path.splitext(n)[0] for n in by_name}
            if m.name.lower() in by_name or pdb_stem in stems:
                self.mod_info(m)

    def _index_symbol_images(self):
        """{lower file name: [paths]} and {(guid, age): path} for every exe/dll in the symbol dirs."""
        by_name, by_sig = {}, {}
        for d in self.sym_dirs:
            for p in glob.glob(os.path.join(d, "*.dll")) + glob.glob(os.path.join(d, "*.exe")):
                by_name.setdefault(os.path.basename(p).lower(), []).append(p)
                try:
                    sig = PeImage(p).cv_signature()
                except (OSError, ValueError, struct.error):
                    continue
                if sig:
                    by_sig.setdefault((sig[0], sig[1]), p)
        return by_name, by_sig

    def mod_info(self, m: Module) -> ModInfo:
        mi = self.info[m.base]
        if mi.image is None and not mi.image_note:
            self._prepare(mi)
        return mi

    def _prepare(self, mi: ModInfo) -> None:
        m = mi.module
        dsig = m.cv_signature()
        by_name, by_sig = self.sym_index
        # 1) a symbols-dir image with the same CodeView signature (GUID + age) as the dump's module: exact;
        # 2) otherwise an image with the same pdb or module name: used only with --force-symbols.
        if dsig and (dsig[0], dsig[1]) in by_sig:
            mi.sym_image = by_sig[(dsig[0], dsig[1])]
            pdb = os.path.join(os.path.dirname(mi.sym_image), os.path.basename(dsig[2].replace("/", "\\")))
            mi.sym_note = "symbols" if os.path.isfile(pdb) else "matching image, no PDB (export names only)"
        elif dsig:
            pdb_stem = os.path.splitext(os.path.basename(dsig[2].replace("/", "\\")))[0].lower()
            cands = by_name.get(m.name.lower(), []) + by_name.get(pdb_stem + ".dll", []) +                 by_name.get(pdb_stem + ".exe", [])
            if cands:
                if self.force:
                    mi.sym_image = cands[0]
                    mi.sym_note = "symbols UNVERIFIED (--force-symbols)"
                else:
                    sig = pe_signature(cands[0])
                    have = f"{{{sig[0]}}} age {sig[1]}" if sig else "no CodeView record"
                    mi.sym_note = (f"no matching PDB: the dump's module is {{{dsig[0]}}} age {dsig[1]}, "
                                   f"{cands[0]} is {have} (rebuilt since; --force-symbols to use it anyway)")
        # Disk image for unwind data: the module's own path, else the symbols image; must match the dump.
        for path in [m.path, mi.sym_image]:
            if not path or not os.path.isfile(path):
                continue
            try:
                img = PeImage(path)
            except (OSError, ValueError, struct.error):
                continue
            if img.timestamp == m.timestamp and img.size_of_image == m.size:
                mi.image = img
                mi.image_note = path
                break
            if self.force and path == mi.sym_image:
                mi.image = img
                mi.image_note = path + " (unverified)"
                break
        if mi.image is None:
            mi.image_note = "no matching image on disk"
        if mi.sym_image and self.symbols:
            mi.sym_loaded = self.symbols.load(mi.sym_image, os.path.splitext(m.name)[0], m.base, m.size)
            if not mi.sym_loaded:
                mi.sym_note += " (dbghelp could not load it)"

    # -- walking --
    def walk(self, ctx: Context, max_frames: int, scan: bool) -> list[Frame]:
        frames = [Frame(ctx.rip, ctx.rsp, "context")]
        cur = ctx
        first = True
        while len(frames) < max_frames:
            m = self.dump.module_at(cur.rip)
            mi = self.mod_info(m) if m else None
            try:
                if m is None and first:
                    # Call through a bad pointer (rip = 0 or garbage): the caller's return address is at [rsp].
                    ret = self.dump.read_u64(cur.rsp)
                    if ret is None or self.dump.module_at(ret) is None:
                        raise UnwindError("address outside every module")
                    nxt = cur.copy()
                    nxt.rip, nxt.rsp = ret, cur.rsp + 8
                    frames[-1].notes.append("not in a module: assumed a call through a bad pointer, "
                                            "return address taken from [rsp]")
                    first = False
                    frames.append(Frame(nxt.rip, nxt.rsp, "leaf"))
                    cur = nxt
                    continue
                if mi is None or mi.image is None:
                    raise UnwindError("no unwind data" if m else "address outside every module")
                func = mi.image.lookup_function(cur.rip - m.base)
                if func is None:  # leaf function: the return address is at [rsp]
                    ret = self.dump.read_u64(cur.rsp)
                    if ret is None:
                        raise UnwindError("stack memory missing")
                    nxt = cur.copy()
                    nxt.rip, nxt.rsp = ret, cur.rsp + 8
                    how = "leaf"
                else:
                    nxt = virtual_unwind(self.dump, mi.image, m.base, func, cur, first)
                    how = "unwind"
            except UnwindError as e:
                frames[-1].notes.append(f"unwind stopped: {e}")
                break
            first = False
            if nxt.rip == 0 or nxt.rsp <= cur.rsp:
                break
            frames.append(Frame(nxt.rip, nxt.rsp, how))
            cur = nxt
        if scan and len(frames) < max_frames:
            frames += self.scan(frames[-1].rsp + 8, max_frames - len(frames), skip=frames[-1].rip)
        return frames

    def scan(self, start: int, limit: int, skip: int, window: int = 64 * 1024) -> list[Frame]:
        """Values on the stack that point just after a call instruction in a known module."""
        out = []
        addr = start & ~7
        end = addr + window
        while addr < end and len(out) < limit:
            v = self.dump.read_u64(addr)
            if v is None:
                break
            m = self.dump.module_at(v) if v != skip else None
            if m:
                mi = self.mod_info(m)
                if mi.image and mi.image.is_code(v - m.base) and self._after_call(mi.image, v - m.base):
                    out.append(Frame(v, addr, "scanned"))
            addr += 8
        return out

    @staticmethod
    def _after_call(img: PeImage, rva: int) -> bool:
        b = img.read_rva(rva - 7, 7)
        if not b:
            return False
        if b[2] == 0xE8:  # call rel32
            return True
        if b[5] == 0xFF and (b[6] >> 3) & 7 == 2:  # call r/m (2-byte, e.g. call rax)
            return True
        if b[4] == 0xFF and (b[5] >> 3) & 7 == 2:  # call [reg+disp8] (3 bytes)
            return True
        if b[1] == 0xFF and (b[2] >> 3) & 7 == 2:  # call [rip+disp32] / [reg+disp32] (6 bytes)
            return True
        if b[0] in (0x41, 0x48, 0x49) and b[1] == 0xFF:  # REX + call [rip+disp32]
            return True
        if b[3] == 0xFF and (b[4] >> 3) & 7 == 2:  # call [reg+reg*s+disp8] (4 bytes)
            return True
        return False

    # -- output --
    def describe(self, addr: int, is_return: bool) -> tuple[str, list[str]]:
        m = self.dump.module_at(addr)
        if not m:
            for base, size, name in self.dump.unloaded:
                if base <= addr < base + size:
                    return f"{name}+0x{addr - base:x} (unloaded module)", []
            return "(no module)", []
        where = f"{m.name}+0x{addr - m.base:x}"
        mi = self.mod_info(m)
        lines = []
        if mi.sym_loaded and self.symbols:
            look = addr - 1 if is_return else addr  # a return address points after the call
            syms = self.symbols.resolve(look, m.base)
            for i, (name, disp, src) in enumerate(syms):
                tag = "[inline] " if i < len(syms) - 1 else ""
                lines.append(f"{tag}{name}+0x{disp:x}" + (f"  {src}" if src else ""))
            if "UNVERIFIED" in mi.sym_note and lines:
                lines[-1] += "  (unverified symbols)"
        return where, lines


def pe_signature(path: str):
    try:
        return PeImage(path).cv_signature()
    except (OSError, ValueError, struct.error):
        return None


def find_log_for(dump_path: str) -> str | None:
    name = os.path.basename(dump_path)
    logs_dir = os.path.dirname(dump_path)
    for p in sorted(glob.glob(os.path.join(logs_dir, "*.log*")), key=os.path.getmtime, reverse=True):
        try:
            with open(p, encoding="utf-8", errors="replace") as f:
                if any(name in line for line in f):
                    return p
        except OSError:
            pass
    return None


def print_log_block(log_path: str, dump_path: str) -> None:
    with open(log_path, encoding="utf-8", errors="replace") as f:
        lines = f.read().splitlines()
    name = os.path.basename(dump_path)
    end = next((i for i, line in enumerate(lines) if name in line), None)
    if end is None:  # no mention of this dump: show the last crash block
        starts = [i for i, line in enumerate(lines) if "FATAL EXCEPTION" in line]
        if not starts:
            print(f"(no crash block in {log_path})")
            return
        start = starts[-1]
        end = next((i for i in range(start, len(lines)) if "minidump" in lines[i]), len(lines) - 1)
    else:
        start = max((i for i in range(end + 1) if "FATAL EXCEPTION" in lines[i]), default=end)
    lo = max(0, start - 5)
    for line in lines[lo: end + 1]:
        print("  " + line)


def main() -> int:
    ap = argparse.ArgumentParser(description="Summarise a MafiaCoop crash dump.")
    ap.add_argument("dump", nargs="?", help="crash .dmp (default: newest out\\logs\\crash-*.dmp)")
    ap.add_argument("--latest", action="store_true", help="use the newest out\\logs\\crash-*.dmp")
    ap.add_argument("--log", help="log file with the crash block (default: found by the dump's name)")
    ap.add_argument("--symbols", action="append", default=[],
                    help="extra folder with matching dll/exe + pdb (default: out\\); repeatable")
    ap.add_argument("--force-symbols", action="store_true",
                    help="use PDBs even if their signature does not match the dump (marked unverified)")
    ap.add_argument("--all-threads", action="store_true", help="also walk every other thread")
    ap.add_argument("--max-frames", type=int, default=40)
    ap.add_argument("--no-scan", action="store_true", help="no stack scanning after the unwind stops")
    ap.add_argument("--modules", action="store_true", help="list every module in the dump")
    args = ap.parse_args()

    path = args.dump
    if not path or args.latest:
        dumps = sorted(glob.glob(os.path.join(OUT_DIR, "logs", "crash-*.dmp")), key=os.path.getmtime)
        if not dumps:
            print("no crash-*.dmp in out\\logs", file=sys.stderr)
            return 1
        path = dumps[-1]
    dump = Minidump(path)
    sym_dirs = [os.path.abspath(d) for d in args.symbols] + [OUT_DIR]
    tri = Triage(dump, sym_dirs, args.force_symbols)

    print(f"dump:    {path}")
    sysinfo = dump.system
    if sysinfo:
        print(f"system:  Windows {sysinfo['version']}, {sysinfo['cpus']} CPUs")
    exe = next((m for m in dump.modules if m.path.lower().endswith(".exe")), None)
    if exe:
        print(f"process: {exe.path}")
    print(f"threads: {len(dump.threads)}, modules: {len(dump.modules)}, memory ranges: {len(dump.memory)}")

    exc = dump.exception
    if not exc:
        print("no exception stream (not a crash dump?)")
        return 1
    code = exc["code"]
    addr = exc["address"]
    print()
    print(f"exception: 0x{code:08X} {EXCEPTION_NAMES.get(code, '?')} on thread {exc['tid']}"
          f"{' (non-continuable)' if exc['flags'] & 1 else ''}")
    where, syms = tri.describe(addr, is_return=False)
    print(f"address:   0x{addr:016x} {where}")
    for s in syms:
        print(f"           {s}")
    if code in (0xC0000005, 0xC0000006) and len(exc["params"]) >= 2:
        op = {0: "read", 1: "write", 8: "execute (DEP)"}.get(exc["params"][0], f"op {exc['params'][0]}")
        print(f"fault:     {op} of address 0x{exc['params'][1]:016x}")
    elif exc["params"]:
        print("params:    " + " ".join(f"0x{p:x}" for p in exc["params"]))

    ctx = Context.from_bytes(exc["context"])
    print()
    for row in range(0, 16, 4):
        print("  " + "  ".join(f"{REG_NAMES[i]:>3}={ctx.regs[i]:016x}" for i in range(row, row + 4)))
    print(f"  rip={ctx.rip:016x}  eflags={ctx.eflags:08x}")

    def print_stack(c: Context):
        frames = tri.walk(c, args.max_frames, scan=not args.no_scan)
        for i, f in enumerate(frames):
            where, syms = tri.describe(f.rip, is_return=i > 0)
            tag = "  [scanned]" if f.how == "scanned" else ""
            print(f"  #{i:02d} {f.rip:016x} sp={f.rsp:016x} {where}{tag}")
            for s in syms:
                print(f"         {s}")
            for n in f.notes:
                print(f"         ({n})")

    print()
    print(f"stack of the faulting thread {exc['tid']}:")
    print_stack(ctx)

    if args.all_threads:
        for tid, t in dump.threads.items():
            if tid == exc["tid"] or not t.context:
                continue
            print()
            print(f"thread {tid}:")
            print_stack(Context.from_bytes(t.context))

    print()
    print("modules with symbols / notes:")
    for m in dump.modules:
        mi = tri.info[m.base]
        if mi.sym_image or mi.sym_note or args.modules:
            ts = f"{m.timestamp:08x}"
            note = mi.sym_note or ("" if mi.sym_image else "no symbols")
            print(f"  {m.base:016x} {m.size:8x} {ts} {m.name:32} {note}")
            if mi.sym_image:
                print(f"  {'':16} {'':8} {'':8} {'':32} image {mi.sym_image}")

    log_path = args.log or find_log_for(path)
    print()
    if log_path:
        print(f"log: {log_path}")
        print_log_block(log_path, path)
    else:
        print("log: no log mentions this dump (pass --log)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
