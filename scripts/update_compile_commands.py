#!/usr/bin/env python3

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
from contextlib import contextmanager
from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterable


SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parent
DB_PATH = REPO_ROOT / "compile_commands.json"
WORK_DIR = REPO_ROOT / ".compile-commands"
FRAGMENT_DIR = WORK_DIR / "fragments"
LOCK_PATH = WORK_DIR / "compile_commands.lock"


@dataclass(frozen=True)
class CaptureSpec:
    label: str
    command: tuple[str, ...]
    env: dict[str, str] = field(default_factory=dict)
    setup: tuple[tuple[str, ...], ...] = ()


def repo_env(extra: dict[str, str] | None = None) -> dict[str, str]:
    env = os.environ.copy()
    if extra:
        env.update(extra)
    return env


def mkdirs() -> None:
    FRAGMENT_DIR.mkdir(parents=True, exist_ok=True)


def load_entries(path: Path) -> list[dict]:
    if not path.exists():
        return []
    with path.open("r", encoding="utf-8") as f:
        data = json.load(f)
    if not isinstance(data, list):
        raise ValueError(f"{path} does not contain a JSON array")
    return data


def write_entries(path: Path, entries: list[dict]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(path.suffix + ".tmp")
    with tmp.open("w", encoding="utf-8") as f:
        json.dump(entries, f, indent=2)
        f.write("\n")
    tmp.replace(path)


def normalise_optional_path(value: object) -> str:
    if not isinstance(value, str) or value == "":
        return ""
    return os.path.normpath(value)


def entry_key(entry: dict) -> tuple[str, str, str]:
    return (
        normalise_optional_path(entry.get("directory")),
        normalise_optional_path(entry.get("file")),
        normalise_optional_path(entry.get("output")),
    )


def merge_entries(existing: Iterable[dict], incoming: Iterable[dict]) -> list[dict]:
    merged: dict[tuple[str, str, str], dict] = {}
    for entry in existing:
        merged[entry_key(entry)] = entry
    for entry in incoming:
        merged[entry_key(entry)] = entry
    return list(merged.values())


def fragment_name(label: str) -> str:
    safe = re.sub(r"[^A-Za-z0-9]+", "-", label.strip().lower()).strip("-")
    safe = safe or "capture"
    digest = hashlib.sha1(label.encode("utf-8")).hexdigest()[:12]
    return f"{safe[:80]}-{digest}.json"


def merge_database(fragment_paths: list[Path]) -> int:
    entries = load_entries(DB_PATH)
    if fragment_paths:
        fragments = fragment_paths
    else:
        fragments = sorted(FRAGMENT_DIR.glob("*.json"))

    for fragment in fragments:
        entries = merge_entries(entries, load_entries(fragment))

    write_entries(DB_PATH, entries)
    print(f"merged {len(entries)} compile command entries into {DB_PATH.relative_to(REPO_ROOT)}")
    return 0


@contextmanager
def compile_db_lock():
    mkdirs()
    with LOCK_PATH.open("w", encoding="utf-8") as lock:
        try:
            import fcntl

            fcntl.flock(lock, fcntl.LOCK_EX)
            yield
            fcntl.flock(lock, fcntl.LOCK_UN)
        except ImportError:
            yield


def add_entry(entry: dict) -> int:
    with compile_db_lock():
        entries = merge_entries(load_entries(DB_PATH), [entry])
        write_entries(DB_PATH, entries)
    return 0


def run_checked(command: tuple[str, ...], env: dict[str, str] | None = None) -> None:
    printable = " ".join(command)
    print(f"+ {printable}", flush=True)
    subprocess.run(command, cwd=REPO_ROOT, env=repo_env(env), check=True)


def capture(label: str, command: tuple[str, ...], env: dict[str, str] | None = None) -> Path:
    bear = shutil.which("bear")
    if bear is None:
        raise RuntimeError("bear is required for capture mode, but it is not in PATH")

    mkdirs()
    fragment = FRAGMENT_DIR / fragment_name(label)
    bear_cmd = (bear, "--output", str(fragment), "--", *command)
    run_checked(bear_cmd, env=env)
    return fragment


def capture_and_merge(label: str, command: tuple[str, ...], env: dict[str, str] | None = None) -> int:
    fragment = capture(label, command, env)
    return merge_database([fragment])


def project_env() -> dict[str, str]:
    return {
        "AM_HOME": str(REPO_ROOT / "abstract-machine"),
        "NAVY_HOME": str(REPO_ROOT / "navy-apps"),
        "NEMU_HOME": str(REPO_ROOT / "nemu"),
        "FCEUX_PATH": str(REPO_ROOT / "fceux-am"),
    }


def nemu_specs() -> list[CaptureSpec]:
    return [
        CaptureSpec(
            label="nemu-rv32",
            setup=(("make", "-C", "nemu", "riscv32-am-headless-jit_defconfig"),),
            command=("make", "-C", "nemu", "-B"),
            env=project_env(),
        ),
        CaptureSpec(
            label="nemu-rv64",
            setup=(("make", "-C", "nemu", "riscv64-am-headless-jit-stats_defconfig"),),
            command=("make", "-C", "nemu", "-B"),
            env=project_env(),
        ),
    ]


def am_specs() -> list[CaptureSpec]:
    env = project_env()
    return [
        CaptureSpec(
            label="am-cpu-tests-rv32",
            command=(
                "make",
                "-C",
                "am-kernels/tests/cpu-tests",
                "ARCH=riscv32-nemu",
                "NEMU_DEFCONFIG=riscv32-am-headless-jit_defconfig",
                "clean",
                "run",
            ),
            env=env,
        ),
        CaptureSpec(
            label="am-cpu-tests-rv64",
            command=(
                "make",
                "-C",
                "am-kernels/tests/cpu-tests",
                "ARCH=riscv64-nemu",
                "NEMU_DEFCONFIG=riscv64-am-headless-jit-stats_defconfig",
                "clean",
                "run",
            ),
            env=env,
        ),
    ]


def navy_specs() -> list[CaptureSpec]:
    env = project_env()
    libs = (
        "compiler-rt",
        "libc",
        "libos",
        "libndl",
        "libminiSDL",
        "libbmp",
        "libbdf",
        "libfixedptc",
        "libvorbis",
        "libSDL_image",
        "libSDL_ttf",
        "libSDL_mixer",
        "libam",
    )
    apps = (
        "am-kernels",
        "bird",
        "busybox",
        "doom",
        "exec-test",
        "fceux",
        "lua",
        "menu",
        "nplayer",
        "nslider",
        "nterm",
        "nwm",
        "onscripter",
        "oslab0",
        "pal",
    )
    tests = (
        "bmp-test",
        "cpp-test",
        "dummy",
        "event-test",
        "exec-test",
        "file-test",
        "float-test",
        "hello",
        "ons-mixer-test",
        "ons-sdl-test",
        "time-test",
        "timer-test",
    )

    specs: list[CaptureSpec] = []
    for lib in libs:
        specs.append(
            CaptureSpec(
                label=f"navy-lib-{lib}-riscv64",
                command=("make", "-C", f"navy-apps/libs/{lib}", "ISA=riscv64", "-B", "archive"),
                env=env,
            )
        )

    specs.append(
        CaptureSpec(
            label="navy-lib-libos-native",
            command=("make", "-C", "navy-apps/libs/libos", "ISA=native", "-B"),
            env=env,
        )
    )

    for app in apps:
        specs.append(
            CaptureSpec(
                label=f"navy-app-{app}-riscv64",
                command=("make", "-C", f"navy-apps/apps/{app}", "ISA=riscv64", "-B", "app"),
                env=env,
            )
        )

    for test in tests:
        specs.append(
            CaptureSpec(
                label=f"navy-test-{test}-riscv64",
                command=("make", "-C", f"navy-apps/tests/{test}", "ISA=riscv64", "-B", "app"),
                env=env,
            )
        )

    return specs


def nanos_ramdisk_setup() -> tuple[tuple[str, ...], ...]:
    ramdisk = REPO_ROOT / "nanos-lite" / "build" / "ramdisk.img"
    if ramdisk.exists():
        return ()
    return (("make", "-C", "nanos-lite", "ARCH=riscv64-nemu", "FS_MODE=fat32", "update"),)


def nanos_specs() -> list[CaptureSpec]:
    env = project_env()
    setup = nanos_ramdisk_setup()
    return [
        CaptureSpec(
            label="nanos-lite-rv32-fat32",
            setup=setup,
            command=("make", "-C", "nanos-lite", "ARCH=riscv32-nemu", "FS_MODE=fat32", "-B", "image"),
            env=env,
        ),
        CaptureSpec(
            label="nanos-lite-rv64-fat32",
            command=("make", "-C", "nanos-lite", "ARCH=riscv64-nemu", "FS_MODE=fat32", "-B", "image"),
            env=env,
        ),
        CaptureSpec(
            label="nanos-lite-fat32-host-tests",
            command=("make", "-C", "nanos-lite/tests/fat32", "-B", "all"),
            env=env,
        ),
    ]


def specs_for_profiles(profiles: list[str]) -> list[CaptureSpec]:
    selected = set()
    for profile in profiles:
        if profile == "all":
            selected.update(("nemu", "am", "navy", "nanos"))
        else:
            selected.add(profile)

    specs: list[CaptureSpec] = []
    if "nemu" in selected:
        specs.extend(nemu_specs())
    if "am" in selected:
        specs.extend(am_specs())
    if "navy" in selected:
        specs.extend(navy_specs())
    if "nanos" in selected:
        specs.extend(nanos_specs())
    return specs


def run_baseline(args: argparse.Namespace) -> int:
    specs = specs_for_profiles(args.profile)
    if args.list:
        for spec in specs:
            print(f"{spec.label}: {' '.join(spec.command)}")
        return 0

    fragments: list[Path] = []
    failures: list[str] = []
    for index, spec in enumerate(specs, start=1):
        print(f"\n[{index}/{len(specs)}] {spec.label}", flush=True)
        try:
            for setup_cmd in spec.setup:
                run_checked(setup_cmd, spec.env)
            fragments.append(capture(spec.label, spec.command, spec.env))
        except subprocess.CalledProcessError as exc:
            failures.append(f"{spec.label}: command exited with {exc.returncode}")
            if not args.keep_going:
                raise
        except Exception as exc:
            failures.append(f"{spec.label}: {exc}")
            if not args.keep_going:
                raise

    merge_database(fragments)
    if failures:
        print("\nBaseline completed with failures:", file=sys.stderr)
        for failure in failures:
            print(f"  {failure}", file=sys.stderr)
        return 1
    return 0


def run_add(args: argparse.Namespace) -> int:
    command = args.arguments
    if command[:1] == ["--"]:
        command = command[1:]
    if not command:
        raise SystemExit("add mode needs compiler arguments after --")
    entry = {
        "directory": str(Path(args.directory).resolve()),
        "file": str(Path(args.file).resolve()),
        "arguments": command,
    }
    if args.output:
        entry["output"] = str(Path(args.output).resolve())
    return add_entry(entry)


def run_capture(args: argparse.Namespace) -> int:
    if not args.command:
        raise SystemExit("capture mode needs a command after --")
    label = args.label or " ".join(args.command)
    return capture_and_merge(label, tuple(args.command), project_env())


def run_merge(args: argparse.Namespace) -> int:
    paths = [Path(p) for p in args.fragments]
    return merge_database(paths)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Capture and merge compile_commands.json entries for this repository."
    )
    sub = parser.add_subparsers(dest="command_name", required=True)

    baseline = sub.add_parser("baseline", help="Run an explicit full capture profile.")
    baseline.add_argument(
        "--profile",
        choices=("all", "nemu", "am", "navy", "nanos"),
        action="append",
        default=None,
        help="Capture profile to run. Defaults to all.",
    )
    baseline.add_argument("--keep-going", action="store_true", help="Continue after a failed target.")
    baseline.add_argument("--list", action="store_true", help="Print selected capture commands without running them.")
    baseline.set_defaults(func=run_baseline)

    add = sub.add_parser("add", help="Add one compile command entry. Used by Make hooks.")
    add.add_argument("--directory", required=True)
    add.add_argument("--file", required=True)
    add.add_argument("--output")
    add.add_argument("arguments", nargs=argparse.REMAINDER)
    add.set_defaults(func=run_add)

    capture_parser = sub.add_parser("capture", aliases=("run",), help="Capture one build command and merge it.")
    capture_parser.add_argument("--label", help="Stable label for the captured command fragment.")
    capture_parser.add_argument("command", nargs=argparse.REMAINDER, help="Command to run after --.")
    capture_parser.set_defaults(func=run_capture)

    merge = sub.add_parser("merge", help="Merge captured fragments into compile_commands.json.")
    merge.add_argument("fragments", nargs="*", help="Specific fragment files. Defaults to every stored fragment.")
    merge.set_defaults(func=run_merge)

    return parser


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    if getattr(args, "profile", None) is None:
        args.profile = ["all"]
    if getattr(args, "command", None) and args.command[:1] == ["--"]:
        args.command = args.command[1:]
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
