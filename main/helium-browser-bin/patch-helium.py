#!/usr/bin/env python3
"""Patch Helium's glibc binaries to run on musl.

1. Replace R_AARCH64_IRELATIVE (GNU ifunc) relocations, which musl does
not support, with R_AARCH64_RELATIVE ones pointing at the resolver's
default implementation.

Each resolver in Helium's binaries selects a memory-tagging (MTE) variant
only when glibc passes hwcaps with _IFUNC_ARG_HWCAP (bit 62 of x0) and the
CPU has MTE. Before testing bit 62 it loads the default implementation
into x0 with ADRP+ADD or ADR. That default is what glibc picks on CPUs
without MTE, so it is safe on all of them. Any resolver not matching this
pattern aborts the patch.

2. Skip MaybeUpdateGlibcTidCache() (sandbox/linux/services/
namespace_sandbox.cc), which runs in each child the zygote forks into a
new PID namespace. It reads glibc's private mutex owner field and then
stores the new tid at offset 0xd0 of glibc's struct pthread. On musl the
first read is always 0 and the store lands in musl's own thread
structure, and the check that follows fails. musl's clone() already
refreshes musl's cached tid, so the update is not needed: the branch that
leaves when the cached and real tids match becomes unconditional. Only
the binary with the sandbox has this code; at least one file must.

Usage: patch-helium.py FILE...
"""
import re
import struct
import sys

R_AARCH64_RELATIVE = 1027
R_AARCH64_IRELATIVE = 1032
DT_RELA, DT_RELASZ, DT_JMPREL, DT_PLTRELSZ = 7, 8, 23, 2


def sx(v, bits):
    return v - (1 << bits) if v & (1 << (bits - 1)) else v


class Elf:
    def __init__(self, data):
        self.d = data
        assert data[:4] == b'\x7fELF' and data[4] == 2 and data[5] == 1, 'not ELF64 LE'
        (self.phoff,) = struct.unpack_from('<Q', data, 0x20)
        self.phentsize, self.phnum = struct.unpack_from('<HH', data, 0x36)
        self.phdrs = [struct.unpack_from('<IIQQQQQQ', data, self.phoff + i * self.phentsize)
                      for i in range(self.phnum)]

    def off(self, vaddr):
        for p_type, _, p_off, p_vaddr, _, p_filesz, _, _ in self.phdrs:
            if p_type == 1 and p_vaddr <= vaddr < p_vaddr + p_filesz:
                return vaddr - p_vaddr + p_off
        raise ValueError('address %#x not in a loaded segment' % vaddr)

    def dynamic(self):
        for p_type, _, p_off, _, _, p_filesz, _, _ in self.phdrs:
            if p_type == 2:
                tags = {}
                for i in range(0, p_filesz, 16):
                    tag, val = struct.unpack_from('<qQ', self.d, p_off + i)
                    if tag == 0:
                        break
                    tags[tag] = val
                return tags
        return {}


def default_impl(elf, resolver):
    """Value of x0 when the resolver reaches its 'tbz x8|x0, #62' test."""
    x0 = None
    pc = resolver
    for _ in range(12):
        (ins,) = struct.unpack_from('<I', elf.d, elf.off(pc))
        if (ins & 0x9f00001f) == 0x90000000:          # ADRP x0, imm
            imm = ((ins >> 29) & 3) | (((ins >> 5) & 0x7ffff) << 2)
            x0 = (pc & ~0xfff) + (sx(imm, 21) << 12)
        elif (ins & 0x9f00001f) == 0x10000000:        # ADR x0, imm
            imm = ((ins >> 29) & 3) | (((ins >> 5) & 0x7ffff) << 2)
            x0 = pc + sx(imm, 21)
        elif (ins & 0xffc003ff) == 0x91000000:        # ADD x0, x0, #imm
            assert x0 is not None
            shift = 12 if ins & (1 << 22) else 0
            x0 += ((ins >> 10) & 0xfff) << shift
        elif (ins & 0x7f000000) == 0x36000000:        # TBZ/TBNZ
            bit = ((ins >> 31) << 5) | ((ins >> 19) & 0x1f)
            if bit == 62 and x0 is not None and not (ins & (1 << 24)):
                return x0
            break
        pc += 4
    raise ValueError('resolver at %#x does not match the expected pattern' % resolver)


def word(data, off):
    return struct.unpack_from('<I', data, off)[0]


def skip_tid_cache_update(elf):
    """Find, in executable segments:
           mov   w0, #178          ; gettid, up to 32 instructions before
           ...
           cmp   wC, wR            ; cached tid == real tid?
           b.eq  out
           bl    pthread_self
           ldr   wT, [x0, #0xd0]   ; glibc struct pthread::tid
           cmp   wC, wT
       and turn the b.eq into b. Returns the patched address or None."""
    data = elf.d
    hits = []
    for p_type, p_flags, p_off, p_vaddr, _, p_filesz, _, _ in elf.phdrs:
        if p_type != 1 or not p_flags & 1:
            continue
        seg = bytes(data[p_off:p_off + p_filesz])
        for m in re.finditer(rb'[\x00-\x1e]\xd0\x40\xb9', seg):
            o = p_off + m.start()
            if (o - p_off) % 4 or o - 12 < p_off:
                continue
            ldr, bl, beq, cmp1 = (word(data, o - i) for i in (0, 4, 8, 12))
            cmp2 = word(data, o + 4)
            rt = ldr & 0x1f
            rc = (cmp1 >> 5) & 0x1f
            if ((bl & 0xfc000000) != 0x94000000 or
                    (beq & 0xff00001f) != 0x54000000 or
                    (cmp1 & 0xffe0fc1f) != 0x6b00001f or
                    cmp2 & 0xffe0fc1f != 0x6b00001f or
                    (cmp2 >> 5) & 0x1f != rc or (cmp2 >> 16) & 0x1f != rt):
                continue
            if not any(word(data, o - 12 - 4 * i) == 0x52801640  # mov w0, #178
                       for i in range(1, 33)):
                continue
            hits.append((o - 8, beq, p_off, p_vaddr))
    if not hits:
        return None
    if len(hits) != 1:
        raise ValueError('tid cache check found %d times' % len(hits))
    o, beq, p_off, p_vaddr = hits[0]
    # B.cond imm19 -> B imm26, same target.
    disp = sx((beq >> 5) & 0x7ffff, 19)
    struct.pack_into('<I', data, o, 0x14000000 | (disp & 0x3ffffff))
    return o - p_off + p_vaddr


def patch(path):
    data = bytearray(open(path, 'rb').read())
    elf = Elf(data)
    dyn = elf.dynamic()
    tables = []
    if DT_RELA in dyn:
        tables.append((dyn[DT_RELA], dyn[DT_RELASZ]))
    if DT_JMPREL in dyn:
        tables.append((dyn[DT_JMPREL], dyn[DT_PLTRELSZ]))
    count = 0
    for vaddr, size in tables:
        base = elf.off(vaddr)
        for i in range(0, size, 24):
            r_offset, r_info, r_addend = struct.unpack_from('<QQq', data, base + i)
            if r_info & 0xffffffff != R_AARCH64_IRELATIVE:
                continue
            impl = default_impl(elf, r_addend)
            struct.pack_into('<QQq', data, base + i, r_offset,
                             (r_info & ~0xffffffff) | R_AARCH64_RELATIVE, impl)
            print('%s: IRELATIVE at %#x: resolver %#x -> %#x' % (path, r_offset, r_addend, impl))
            count += 1
    print('%s: %d ifunc relocation(s) replaced' % (path, count))
    tid = skip_tid_cache_update(elf)
    if tid is not None:
        print('%s: glibc tid cache update skipped at %#x' % (path, tid))
    open(path, 'wb').write(data)
    return tid is not None


if not [f for f in sys.argv[1:] if patch(f)]:
    sys.exit('glibc tid cache update not found')
