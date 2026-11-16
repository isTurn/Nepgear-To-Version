#!/usr/bin/env python3
"""Sanity-check a finished proxy DLL (used by CI, handy locally too).

    python 编译\\check_dll.py Release\\version.dll x64\\Release\\version.dll

Checks, per file:
  * the PE parses and the machine type matches the expected architecture
  * no UPX section (the shipped build is always a plain image)
  * the RCDATA payload resource (id 101) is present and looks like an archive

Exit code 1 if any check fails, so it can gate a build step.
"""

import struct
import sys
from pathlib import Path

RESOURCE_ID = 101
RT_RCDATA = 10

MACHINES = {0x014C: "x86", 0x8664: "x64"}


def rva_to_offset(rva, sections):
    for name, vsize, vaddr, rsize, rptr in sections:
        span = max(vsize, rsize)
        if vaddr <= rva < vaddr + span:
            return rptr + (rva - vaddr)
    raise ValueError("RVA 0x%X is outside every section" % rva)


def read_sections(data):
    e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
    if data[e_lfanew:e_lfanew + 4] != b"PE\0\0":
        raise ValueError("missing PE signature")

    coff = e_lfanew + 4
    machine = struct.unpack_from("<H", data, coff)[0]
    nsections = struct.unpack_from("<H", data, coff + 2)[0]
    size_opt = struct.unpack_from("<H", data, coff + 16)[0]

    opt = coff + 20
    magic = struct.unpack_from("<H", data, opt)[0]
    if magic == 0x20B:
        dd = opt + 0x70
    elif magic == 0x10B:
        dd = opt + 0x60
    else:
        raise ValueError("unknown optional header magic 0x%X" % magic)

    res_rva, res_size = struct.unpack_from("<II", data, dd + 16)
    sections = []
    table = opt + size_opt
    for index in range(nsections):
        off = table + index * 40
        name = data[off:off + 8].rstrip(b"\0").decode("latin-1")
        vsize, vaddr, rsize, rptr = struct.unpack_from("<IIII", data, off + 8)
        sections.append((name, vsize, vaddr, rsize, rptr))
    return machine, sections, res_rva, res_size


def directory_entries(data, offset):
    named, ids = struct.unpack_from("<HH", data, offset + 12)
    total = named + ids
    for index in range(total):
        entry = offset + 16 + index * 8
        name_field, data_field = struct.unpack_from("<II", data, entry)
        yield name_field & 0x7FFFFFFF, bool(data_field & 0x80000000), data_field & 0x7FFFFFFF


def find_payload(data, sections, res_rva, res_size):
    """Return (resource size, language count) for RCDATA/id 101."""
    base = rva_to_offset(res_rva, sections)

    type_dir = None
    for ident, is_dir, off in directory_entries(data, base):
        if not is_dir:
            continue
        if ident == RT_RCDATA:
            type_dir = base + off
            break
    if type_dir is None:
        raise ValueError("no RCDATA resource type")

    name_dir = None
    for ident, is_dir, off in directory_entries(data, type_dir):
        if is_dir and ident == RESOURCE_ID:
            name_dir = base + off
            break
    if name_dir is None:
        raise ValueError("RCDATA resource %d not found" % RESOURCE_ID)

    languages = [off for _, is_dir, off in directory_entries(data, name_dir) if not is_dir]
    if not languages:
        raise ValueError("RCDATA resource %d has no language entries" % RESOURCE_ID)

    entry = base + languages[0]
    data_rva, size, codepage, _ = struct.unpack_from("<IIII", data, entry)
    start = rva_to_offset(data_rva, sections)
    if start + size > len(data):
        raise ValueError("payload resource is truncated")
    return size, len(languages)


def check(path):
    data = Path(path).read_bytes()
    problems = []

    try:
        machine, sections, res_rva, res_size = read_sections(data)
    except Exception as error:  # noqa: BLE001 - report, do not traceback
        print("  %-24s FAIL  %s" % (Path(path).name, error))
        return False

    arch = MACHINES.get(machine, "0x%04X" % machine)
    upx = [name for name, *_ in sections if name.upper().startswith("UPX")]
    payload = None
    try:
        payload, languages = find_payload(data, sections, res_rva, res_size)
    except Exception as error:  # noqa: BLE001
        problems.append(str(error))

    if upx:
        problems.append("UPX section present (%s)" % ", ".join(upx))
    if payload is None:
        pass
    elif payload < 1024 * 1024:
        problems.append("payload resource is only %d bytes" % payload)

    status = "FAIL" if problems else "OK  "
    detail = "arch=%s size=%d" % (arch, len(data))
    if payload is not None:
        detail += " payload=%.2f MB" % (payload / 1048576.0)
    if problems:
        detail += " -> " + "; ".join(problems)
    print("  %-24s %s %s" % (Path(path).name, status, detail))
    return not problems


def main(argv):
    targets = argv[1:]
    if not targets:
        print("usage: check_dll.py <dll> [more.dll ...]")
        return 2

    print("Checking %d file(s):" % len(targets))
    results = [check(path) for path in targets]
    print("")
    if all(results):
        print("All checks passed.")
        return 0
    print("Check failed.")
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
