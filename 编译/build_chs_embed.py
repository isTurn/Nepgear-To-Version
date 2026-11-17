#!/usr/bin/env python3
"""Nepgear embedded-archive builder.

Ships a .chs payload inside the proxy DLL as an RCDATA resource, so a deployed
build only needs version.dll + Nepgear.ini (no external Nepgear.chs / .dat).

    Nepgear.chs
        -> chs_payload.dat                     resource input (pre-decompressed)
        -> chs_payload.rc -> rc.exe -> .res
    Release\\version.dll / x64\\Release\\version.dll   (plain, not UPX-packed)

Usage:
    python 编译\\build_chs_embed.py [Nepgear.chs] [x86|x64|all] [options]

Options:
    --dll NAME      Proxy host to build: version (default) or winmm.
    --no-build      Only regenerate the resource, do not run MSBuild.
    --rebuild       Force a full /t:Rebuild instead of an incremental build.
    --clean         Remove the payload resource and exit (no embedded data).

The build format is fixed: every entry is pre-decompressed into the resource
(archive reads never touch LZMS) and the DLL is left uncompressed, so a shipped
build is a plain PE image. There is no smaller alternative to pick from.

Why a resource instead of a generated C array: a 20 MB payload becomes ~140 MB
of C source that makes cl.exe run out of compiler heap (C1060) and slows every
build. rc.exe stores the bytes verbatim, the linker embeds them, and the DLL
reads them in place at runtime with FindResource/LockResource.
"""

import argparse
import ctypes
import os
import struct
import subprocess
import sys
from pathlib import Path

SCRIPT_DIR = Path(__file__).parent.resolve()
ROOT_DIR = SCRIPT_DIR.parent
PROJECT_DIR = ROOT_DIR / "Nepgear"
PAYLOAD_RC = PROJECT_DIR / "chs_payload.rc"
PAYLOAD_DAT = PROJECT_DIR / "chs_payload.dat"

# Must match NEPGEAR_CHS_RESOURCE_ID in hooks/vfs.cpp.
RESOURCE_ID = 101

# COMPRESS_ALGORITHM_LZMS
COMPRESS_ALGORITHM_LZMS = 5


def log(message):
    print(message, flush=True)


def decompress_lzms(data, decompressed_size):
    """Decompress a single LZMS block through the Windows compression API."""
    cabinet = ctypes.WinDLL("cabinet")
    cabinet.CreateDecompressor.argtypes = [ctypes.c_ulong, ctypes.c_void_p,
                                           ctypes.POINTER(ctypes.c_void_p)]
    cabinet.CreateDecompressor.restype = ctypes.c_int
    cabinet.Decompress.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t,
                                   ctypes.c_void_p, ctypes.c_size_t,
                                   ctypes.POINTER(ctypes.c_size_t)]
    cabinet.Decompress.restype = ctypes.c_int
    cabinet.CloseDecompressor.argtypes = [ctypes.c_void_p]
    cabinet.CloseDecompressor.restype = ctypes.c_int

    handle = ctypes.c_void_p()
    if not cabinet.CreateDecompressor(COMPRESS_ALGORITHM_LZMS, None, ctypes.byref(handle)):
        raise RuntimeError("CreateDecompressor failed (LZMS unsupported?)")

    try:
        output = ctypes.create_string_buffer(decompressed_size)
        actual = ctypes.c_size_t(0)
        ok = cabinet.Decompress(handle, data, len(data), output,
                                decompressed_size, ctypes.byref(actual))
        if not ok or actual.value != decompressed_size:
            raise RuntimeError(
                "Decompress failed: ok=%s actual=%d expected=%d"
                % (bool(ok), actual.value, decompressed_size))
        return output.raw
    finally:
        cabinet.CloseDecompressor(handle)


def convert_chs_to_dat(chs_path):
    """Rewrite a .chs into the uncompressed archive layout the VFS reads."""
    data = chs_path.read_bytes()
    if len(data) < 4:
        raise RuntimeError("%s is too small to be a .chs archive" % chs_path.name)

    offset = 0
    count = struct.unpack_from("<i", data, offset)[0]
    offset += 4
    if count <= 0:
        raise RuntimeError("%s contains no entries (count=%d)" % (chs_path.name, count))
    log("  entries: %d" % count)

    total_stored = 0
    total_raw = 0
    PROJECT_DIR.mkdir(parents=True, exist_ok=True)
    with open(PAYLOAD_DAT, "wb") as out:
        out.write(struct.pack("<i", count))
        for index in range(count):
            path_len = struct.unpack_from("<i", data, offset)[0]
            offset += 4
            if path_len <= 0:
                raise RuntimeError("entry %d has an invalid path length (%d)" % (index, path_len))
            path = data[offset:offset + path_len].decode("utf-8")
            offset += path_len

            decompressed_size = struct.unpack_from("<i", data, offset)[0]
            offset += 4
            stored_size = struct.unpack_from("<i", data, offset)[0]
            offset += 4
            raw = data[offset:offset + stored_size]
            offset += stored_size
            if len(raw) != stored_size:
                raise RuntimeError("entry %d (%s) is truncated" % (index, path))

            if stored_size < decompressed_size:
                log("  decompress: %s (%d -> %d)" % (path, stored_size, decompressed_size))
                payload = decompress_lzms(raw, decompressed_size)
            else:
                log("  raw: %s (%d bytes)" % (path, stored_size))
                payload = raw

            encoded = path.encode("utf-8")
            out.write(struct.pack("<i", len(encoded)))
            out.write(encoded)
            out.write(struct.pack("<i", len(payload)))
            out.write(struct.pack("<i", len(payload)))
            out.write(payload)
            total_stored += stored_size
            total_raw += len(payload)

    log("  %s: %.2f MB stored -> %.2f MB uncompressed"
        % (PAYLOAD_DAT.name, total_stored / 1048576.0, total_raw / 1048576.0))
    return PAYLOAD_DAT


def write_payload(chs_path):
    """Materialise chs_payload.dat as a fully decompressed archive."""
    return convert_chs_to_dat(chs_path)


def write_payload_resource():
    """Emit the .rc that turns chs_payload.dat into the embedded resource."""
    lines = [
        "// Auto-generated by build_chs_embed.py - do not edit, do not commit.",
        "// Consumed by hooks/vfs.cpp through FindResourceW(%d, RT_RCDATA)." % RESOURCE_ID,
        "",
        '%d RCDATA "chs_payload.dat"' % RESOURCE_ID,
        "",
    ]
    PAYLOAD_RC.write_text("\n".join(lines), encoding="ascii", newline="\n")
    # rc.exe is tracked by timestamp only; force it even when the .rc text is
    # byte-identical after a new payload was written.
    os.utime(PAYLOAD_RC, None)
    log("  %s -> resource %d (%.2f MB)" % (PAYLOAD_RC.name, RESOURCE_ID,
                                           PAYLOAD_DAT.stat().st_size / 1048576.0))


def sanitize_env():
    """Drop case-duplicate variables.

    MSBuild keeps the child environment in a case-insensitive map, so a shell
    that exports both http_proxy and HTTP_PROXY makes every CL.exe invocation
    fail with MSB6001 before a single file is compiled.
    """
    env = {}
    seen = set()
    for key, value in os.environ.items():
        lowered = key.lower()
        if lowered in seen:
            continue
        seen.add(lowered)
        env[key] = value
    env["NEPGEAR_NO_PAUSE"] = "1"
    return env


def run_build(targets, rebuild, proxy):
    build_bat = ROOT_DIR / "build.bat"
    if not build_bat.exists():
        raise RuntimeError("build.bat not found at %s" % build_bat)

    # build.bat takes a single architecture: repeating it ("x86 x64") would just
    # let the last one win, so collapse the pair into its "all" keyword.
    arch = "all" if set(targets) == {"x86", "x64"} else targets[0]

    command = ['cmd', '/c', str(build_bat), arch, proxy]
    if rebuild:
        command.append("rebuild")

    log("  " + " ".join(command))
    result = subprocess.run(command, cwd=str(ROOT_DIR), env=sanitize_env())
    return result.returncode


def output_paths(targets, proxy):
    dll = "%s.dll" % proxy
    paths = []
    if "x86" in targets:
        paths.append(ROOT_DIR / "Release" / dll)
    if "x64" in targets:
        paths.append(ROOT_DIR / "x64" / "Release" / dll)
    return paths


def main():
    parser = argparse.ArgumentParser(
        description="Embed a .chs payload into the proxy DLL (version.dll by default) "
                    "as an RCDATA resource.",
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("chs", nargs="?", help="path to Nepgear.chs (default: ./Nepgear.chs)")
    parser.add_argument("target", nargs="?", default="x86", choices=["x86", "x64", "all"],
                        help="architecture to build (default: x86)")
    parser.add_argument("--dll", default="version", choices=["version", "winmm"],
                        help="proxy host DLL to build (default: version)")
    parser.add_argument("--no-build", action="store_true", help="only regenerate the resource")
    parser.add_argument("--rebuild", action="store_true", help="force a full /t:Rebuild")
    parser.add_argument("--clean", action="store_true",
                        help="remove the payload resource and exit (no embedded data)")
    args = parser.parse_args()

    if args.clean:
        removed = []
        for path in (PAYLOAD_RC, PAYLOAD_DAT):
            if path.exists():
                path.unlink()
                removed.append(path.name)
        if removed:
            log("Removed %s" % ", ".join(removed))
            log("Next build produces a DLL without embedded data.")
        else:
            log("Nothing to do: no payload resource present.")
        return 0

    chs_path = Path(args.chs).expanduser() if args.chs else ROOT_DIR / "Nepgear.chs"
    if not chs_path.is_absolute():
        chs_path = (Path.cwd() / chs_path).resolve()
    if not chs_path.exists():
        log("ERROR: %s not found." % chs_path)
        log("Build one with Packer.exe, or pass the path:")
        log("  python 编译\\build_chs_embed.py D:\\path\\Nepgear.chs x86")
        return 2

    targets = ["x86", "x64"] if args.target == "all" else [args.target]

    log("[1/3] Unpacking %s (%.2f MB)"
        % (chs_path.name, chs_path.stat().st_size / 1048576.0))
    write_payload(chs_path)

    log("[2/3] Writing %s" % PAYLOAD_RC.relative_to(ROOT_DIR))
    write_payload_resource()

    if args.no_build:
        log("[3/3] Skipped (--no-build). Run build.bat to compile the DLL.")
        return 0

    log("[3/3] Building %s (proxy=%s.dll)" % (" + ".join(targets), args.dll))
    code = run_build(targets, args.rebuild, args.dll)
    if code != 0:
        log("BUILD FAILED (exit %d)" % code)
        return code

    missing = []
    log("")
    for path in output_paths(targets, args.dll):
        if path.exists():
            log("  %s (%d bytes)" % (path.relative_to(ROOT_DIR), path.stat().st_size))
        else:
            missing.append(path)
    if missing:
        for path in missing:
            log("  MISSING: %s" % path)
        return 1

    log("")
    log("Done. Deploy %s.dll + Nepgear.ini; the .chs file is no longer required." % args.dll)
    log("An external Nepgear.chs still overrides the embedded payload when present.")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(130)
    except Exception as error:  # noqa: BLE001 - surface any failure with a clear message
        print("ERROR: %s" % error, file=sys.stderr)
        sys.exit(1)
