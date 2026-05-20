#!/usr/bin/env python3

import unittest
from pathlib import Path
from tempfile import TemporaryDirectory

import update_compile_commands as ucc


class CompileCommandsMergeTest(unittest.TestCase):
    def test_merge_replaces_same_output_and_keeps_other_arch_outputs(self):
        old_entries = [
            {
                "directory": "/repo/nemu",
                "file": "/repo/nemu/src/cpu.c",
                "output": "/repo/nemu/build/obj-rv32/src/cpu.o",
                "arguments": ["gcc", "-O2", "-c", "src/cpu.c"],
            }
        ]
        new_entries = [
            {
                "directory": "/repo/nemu",
                "file": "/repo/nemu/src/cpu.c",
                "output": "/repo/nemu/build/obj-rv32/src/cpu.o",
                "arguments": ["gcc", "-O3", "-c", "src/cpu.c"],
            },
            {
                "directory": "/repo/nemu",
                "file": "/repo/nemu/src/cpu.c",
                "output": "/repo/nemu/build/obj-rv64/src/cpu.o",
                "arguments": ["gcc", "-O2", "-D__GUEST_ISA__=riscv64", "-c", "src/cpu.c"],
            },
        ]

        merged = ucc.merge_entries(old_entries, new_entries)

        self.assertEqual(len(merged), 2)
        self.assertEqual(merged[0]["arguments"][1], "-O3")
        self.assertEqual(merged[1]["output"], "/repo/nemu/build/obj-rv64/src/cpu.o")

    def test_fragment_name_is_stable_and_path_safe(self):
        name = ucc.fragment_name("navy apps/menu:riscv64")

        self.assertTrue(name.startswith("navy-apps-menu-riscv64-"))
        self.assertTrue(name.endswith(".json"))
        self.assertNotIn("/", name)
        self.assertNotIn(":", name)

    def test_add_entry_writes_root_database_with_lock(self):
        with TemporaryDirectory() as td:
            old_db = ucc.DB_PATH
            old_work = ucc.WORK_DIR
            old_fragments = ucc.FRAGMENT_DIR
            old_lock = ucc.LOCK_PATH
            try:
                ucc.DB_PATH = Path(td) / "compile_commands.json"
                ucc.WORK_DIR = Path(td) / ".compile-commands"
                ucc.FRAGMENT_DIR = ucc.WORK_DIR / "fragments"
                ucc.LOCK_PATH = ucc.WORK_DIR / "compile_commands.lock"

                ucc.add_entry(
                    {
                        "directory": td,
                        "file": str(Path(td) / "hello.c"),
                        "output": str(Path(td) / "hello.o"),
                        "arguments": ["gcc", "-c", "hello.c"],
                    }
                )

                loaded = ucc.load_entries(ucc.DB_PATH)
                self.assertEqual(len(loaded), 1)
                self.assertEqual(loaded[0]["arguments"], ["gcc", "-c", "hello.c"])
            finally:
                ucc.DB_PATH = old_db
                ucc.WORK_DIR = old_work
                ucc.FRAGMENT_DIR = old_fragments
                ucc.LOCK_PATH = old_lock


if __name__ == "__main__":
    unittest.main()
