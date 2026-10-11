"""Run clang-tidy on the component's C++ sources using an ESPHome build's flags.

Usage: python script/clang_tidy.py [--fix] [example.yaml]

The YAML must have been compiled first (``esphome compile``) so PlatformIO has
fetched the toolchain and framework. Only ESP8266 (xtensa) builds are handled.
clang has no usable ESP8266 target, so, like ESPHome's own script/clang-tidy,
this analyses the code as 32-bit x86 while pretending to be xtensa.
"""

import argparse
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parent.parent
COMPONENT_DIR = ROOT / "components" / "senseair_sunrise"

# GCC flags clang doesn't understand.
OMIT_FLAGS = {
    "-free",
    "-fipa-pta",
    "-fstrict-volatile-bitfields",
    "-mlongcalls",
    "-mtext-section-literals",
    "-fno-tree-switch-conversion",
}


def load_idedata(yaml_file: str) -> dict:
    result = subprocess.run(
        [sys.executable, "-m", "esphome", "idedata", yaml_file],
        check=True,
        capture_output=True,
        text=True,
        cwd=ROOT,
    )
    return json.loads(result.stdout)


def clang_args(idedata: dict, build_src: Path) -> list[str]:
    args = [
        "-m32",
        "-U__i386__",
        "-U__x86_64__",
        "-D__XTENSA__",
        "-D__XTENSA_EL__",
        "-D_LIBC",
        "-nostdinc",
        "-nostdinc++",
        "-DCLANG_TIDY",
        "-Ddeprecated(x)=",
        # pgmspace.h uses GNU extensions clang can't parse; stub it out.
        "-D_PGMSPACE_H_",
        "-Dpgm_read_byte(s)=(*(const uint8_t *)(s))",
        "-Dpgm_read_byte_near(s)=(*(const uint8_t *)(s))",
        "-Dpgm_read_word(s)=(*(const uint16_t *)(s))",
        "-Dpgm_read_dword(s)=(*(const uint32_t *)(s))",
        "-Dpgm_read_ptr(s)=(*(const void *const *)(s))",
        "-DPROGMEM=",
        "-DPGM_P=const char *",
        "-DPSTR(s)=(s)",
        "-DPSTRN(s, n)=(s)",
    ]
    args += [
        flag
        for flag in idedata["cxx_flags"]
        if flag not in OMIT_FLAGS
        and not flag.startswith(("-Werror", "-std=", "-falign-functions"))
    ]
    args.append("-std=gnu++20")
    args += [f"-D{define}" for define in idedata["defines"]]

    # Toolchain and framework headers are -isystem so their warnings are suppressed.
    for directory in idedata["includes"]["toolchain"]:
        args += ["-isystem", directory]
    for directory in idedata["includes"]["build"]:
        if Path(directory) != build_src:
            args += ["-isystem", directory]
    args += ["-I", str(build_src)]
    return args


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("yaml", nargs="?", default="example.yaml")
    parser.add_argument("--fix", action="store_true", help="apply suggested fixes")
    opts = parser.parse_args()

    idedata = load_idedata(opts.yaml)
    if not Path(idedata["cxx_path"]).name.startswith("xtensa-lx106-"):
        print(f"Unsupported toolchain: {idedata['cxx_path']}", file=sys.stderr)
        return 1
    # ESPHome's generated sources, which provide the esphome/... headers.
    build_src = next(
        Path(d) for d in idedata["includes"]["build"] if Path(d).name == "src"
    )

    cmd = [
        "clang-tidy",
        f"--config-file={ROOT / '.clang-tidy'}",
        f"--header-filter=^{COMPONENT_DIR}/",
        "--quiet",
    ]
    if opts.fix:
        cmd.append("--fix")
    cmd += [str(p) for p in sorted(COMPONENT_DIR.glob("*.cpp"))]
    cmd += ["--", *clang_args(idedata, build_src)]
    return subprocess.run(cmd, check=False, cwd=ROOT).returncode


if __name__ == "__main__":
    sys.exit(main())
