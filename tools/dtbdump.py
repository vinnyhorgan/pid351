#!/usr/bin/env python3
"""Minimal flattened device tree dumper.

Exists so we can read a DTB without depending on dtc being installed, and so
we can diff the vendor tree against mainline's rk3326-anbernic-rg351m later.
Usage: dtbdump.py <file.dtb> [node-name-substring ...]
"""
import struct, sys

FDT_BEGIN_NODE, FDT_END_NODE, FDT_PROP, FDT_NOP, FDT_END = 1, 2, 3, 4, 9


def parse(blob):
    magic, _tot, off_s, off_str, _rsv, ver = struct.unpack(">6I", blob[:24])
    if magic != 0xD00DFEED:
        sys.exit("not a DTB (bad magic)")
    size_str, size_s = struct.unpack(">2I", blob[32:40])
    return blob[off_s:off_s + size_s], blob[off_str:off_str + size_str], ver


def cstr(buf, off):
    end = buf.index(b"\0", off)
    return buf[off:end].decode("ascii", "replace")


def fmt(name, data):
    """Best-effort rendering: printable strings, else u32 cells, else hex."""
    if not data:
        return ""
    if data[-1] == 0 and all(32 <= b < 127 or b == 0 for b in data[:-1]):
        parts = [p.decode() for p in data[:-1].split(b"\0")]
        return " ".join('"%s"' % p for p in parts)
    if len(data) % 4 == 0 and len(data) <= 64:
        cells = struct.unpack(">%dI" % (len(data) // 4), data)
        return "<" + " ".join("0x%x" % c for c in cells) + ">"
    return "[%s]" % data[:32].hex()


def walk(structs, strings, wanted):
    """No filter: print the whole tree. With filters: print matching subtrees."""
    off, depth = 0, 0
    show_from = 0 if not wanted else None
    while off < len(structs):
        (tok,) = struct.unpack(">I", structs[off:off + 4])
        off += 4
        if tok == FDT_BEGIN_NODE:
            name = cstr(structs, off)
            off += (len(name) + 4) & ~3
            if show_from is None and any(w in name for w in wanted):
                show_from = depth
                print()
            if show_from is not None:
                print("%s%s {" % ("  " * (depth - show_from), name or "/"))
            depth += 1
        elif tok == FDT_END_NODE:
            depth -= 1
            if show_from is not None:
                print("%s}" % ("  " * (depth - show_from)))
                if wanted and depth == show_from:
                    show_from = None
        elif tok == FDT_PROP:
            length, nameoff = struct.unpack(">2I", structs[off:off + 8])
            off += 8
            data = structs[off:off + length]
            off += (length + 3) & ~3
            if show_from is not None:
                pname = cstr(strings, nameoff)
                print("%s%s = %s" % ("  " * (depth - show_from), pname, fmt(pname, data)))
        elif tok == FDT_END:
            break


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    with open(sys.argv[1], "rb") as f:
        blob = f.read()
    s, st, ver = parse(blob)
    print("# %s (FDT version %d)" % (sys.argv[1], ver))
    walk(s, st, sys.argv[2:])
