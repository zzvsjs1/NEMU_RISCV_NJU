#!/usr/bin/env python3
"""Build a small deterministic FAT32 disk image from a directory tree."""

from __future__ import annotations

import argparse
import math
import os
import struct
import sys
from dataclasses import dataclass, field
from pathlib import Path


BYTES_PER_SECTOR = 512
SECTORS_PER_CLUSTER = 1
CLUSTER_SIZE = BYTES_PER_SECTOR * SECTORS_PER_CLUSTER
RESERVED_SECTORS = 32
FAT_COUNT = 2
ROOT_CLUSTER = 2
MIN_FAT32_CLUSTERS = 65525
RUNTIME_FREE_BYTES = 64 * 1024 * 1024
EOC = 0x0FFFFFFF


@dataclass
class Node:
    # Source path in fsimg.  Symlinked directories keep this link-side path so
    # the on-disk FAT name matches Navy's expected pathname.
    path: Path
    # Single path component stored in the parent directory entry.
    name: str
    # True for directories, false for regular files.  FAT32 has no symlink
    # entry type here because scan_tree() resolves build-time symlinks.
    is_dir: bool
    # Regular-file length in bytes.  Directory entries store zero here because
    # the FAT specification requires DIR_FileSize to be zero for directories.
    size: int = 0
    # Child nodes sorted by encoded byte name, giving deterministic directory
    # entry order independent of the host locale.
    children: list["Node"] = field(default_factory=list)
    # Allocated FAT32 cluster numbers.  Directories receive enough clusters to
    # hold their directory entries; files receive ceil(size / CLUSTER_SIZE).
    clusters: list[int] = field(default_factory=list)
    # Generated 11-byte short 8.3 alias stored in DIR_Name.
    short_name: bytes = b""
    # True when the real name cannot be represented exactly by short_name and
    # therefore needs one or more preceding long-file-name entries.
    needs_lfn: bool = False


def die(message: str) -> None:
    raise SystemExit(f"mkfat32.py: {message}")


def path_sort_key(path: Path) -> bytes:
    # Sorting on encoded bytes keeps traversal independent from locale settings.
    return os.fsencode(path.name)


def validate_name(name: str) -> None:
    if name in ("", ".", ".."):
        die(f"unsupported name {name!r}")
    if any(ord(ch) < 0x20 or ch in '/\\:*?"<>|' for ch in name):
        die(f"unsupported FAT name {name!r}")
    try:
        name.encode("utf-16le")
    except UnicodeEncodeError:
        die(f"name {name!r} cannot be encoded as a FAT long filename")


def utf16_units(text: str) -> list[int]:
    data = text.encode("utf-16le")
    return [struct.unpack_from("<H", data, offset)[0] for offset in range(0, len(data), 2)]


def scan_tree(root: Path) -> Node:
    if not root.is_dir():
        die(f"{root} is not a directory")

    def resolve_existing(path: Path) -> Path:
        try:
            return path.resolve(strict=True)
        except FileNotFoundError:
            die(f"broken symbolic link: {path}")
        except RuntimeError:
            die(f"symbolic link loop: {path}")

    def scan(path: Path, ancestors: set[Path]) -> Node:
        validate_name(path.name)
        real_path = resolve_existing(path)
        if real_path.is_dir():
            if real_path in ancestors:
                die(f"directory symlink loop: {path}")
            # Build-time symlinks are metadata used to avoid copying large game
            # trees into fsimg.  FAT32 has no symlink entry type, so follow the
            # target while keeping the link name as the on-disk directory name.
            children = [scan(child, ancestors | {real_path}) for child in sorted(path.iterdir(), key=path_sort_key)]
            return Node(path=path, name=path.name, is_dir=True, children=children)
        if real_path.is_file():
            return Node(path=path, name=path.name, is_dir=False, size=real_path.stat().st_size)
        die(f"unsupported file type: {path}")

    root_real_path = resolve_existing(root)
    children = [scan(child, {root_real_path}) for child in sorted(root.iterdir(), key=path_sort_key)]
    return Node(path=root, name="", is_dir=True, children=children)


def cleaned_short_part(text: str) -> str:
    allowed = set("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789$%'-_@~`!(){}^#&")
    out = []
    for ch in text.upper():
        out.append(ch if ch in allowed else "_")
    return "".join(out)


def split_name(name: str) -> tuple[str, str]:
    if "." in name:
        base, ext = name.rsplit(".", 1)
    else:
        base, ext = name, ""
    return cleaned_short_part(base), cleaned_short_part(ext)


def render_short_name(short_name: bytes) -> str:
    base = short_name[:8].decode("ascii").rstrip()
    ext = short_name[8:].decode("ascii").rstrip()
    return f"{base}.{ext}" if ext else base


def short_alias(name: str, used: set[bytes]) -> bytes:
    base, ext = split_name(name)
    base = base or "_"
    ext = ext[:3]

    if len(base) <= 8:
        candidate = f"{base:<8}{ext:<3}".encode("ascii")
        if candidate not in used:
            used.add(candidate)
            return candidate

    for index in range(1, 1000000):
        suffix = f"~{index}"
        prefix = base[: 8 - len(suffix)] or "_"
        candidate = f"{prefix + suffix:<8}{ext:<3}".encode("ascii")
        if candidate not in used:
            used.add(candidate)
            return candidate

    die(f"could not allocate short alias for {name!r}")


def assign_short_names(directory: Node) -> None:
    used: set[bytes] = set()
    for child in directory.children:
        child.short_name = short_alias(child.name, used)
        child.needs_lfn = child.name != render_short_name(child.short_name)
        if child.is_dir:
            assign_short_names(child)


def lfn_entry_count(name: str) -> int:
    return (len(utf16_units(name)) + 12) // 13


def directory_entry_count(node: Node, is_root: bool) -> int:
    entries = 0 if is_root else 2
    for child in node.children:
        entries += 1 + (lfn_entry_count(child.name) if child.needs_lfn else 0)
    return entries


def directory_cluster_count(node: Node, is_root: bool) -> int:
    return max(1, math.ceil(directory_entry_count(node, is_root) * 32 / CLUSTER_SIZE))


def allocate_clusters(root: Node) -> int:
    next_cluster = ROOT_CLUSTER + 1
    root.clusters = [ROOT_CLUSTER]

    def take(count: int) -> list[int]:
        nonlocal next_cluster
        clusters = list(range(next_cluster, next_cluster + count))
        next_cluster += count
        return clusters

    # The root directory must start at cluster 2, but it may still need a
    # normal FAT chain when many entries spill past the first cluster.
    root.clusters.extend(take(directory_cluster_count(root, True) - 1))

    def walk(node: Node) -> None:
        for child in node.children:
            if child.is_dir:
                child.clusters = take(directory_cluster_count(child, False))
                walk(child)
            else:
                count = math.ceil(child.size / CLUSTER_SIZE)
                child.clusters = take(count) if count else []

    walk(root)
    return next_cluster


def lfn_checksum(short_name: bytes) -> int:
    total = 0
    for byte in short_name:
        total = (((total & 1) << 7) + (total >> 1) + byte) & 0xFF
    return total


def make_lfn_entries(name: str, short_name: bytes) -> bytes:
    checksum = lfn_checksum(short_name)
    chars = utf16_units(name)
    chunks = [chars[i : i + 13] for i in range(0, len(chars), 13)]
    out = bytearray()

    # FAT stores LFN slots immediately before the short entry, with the final
    # name chunk first on disk. Each slot carries the same short-name checksum.
    for disk_index, chunk in reversed(list(enumerate(chunks, start=1))):
        units = chunk + [0x0000] + [0xFFFF] * 13
        units = units[:13]
        entry = bytearray(32)
        entry[0] = disk_index | (0x40 if disk_index == len(chunks) else 0)
        entry[11] = 0x0F
        entry[12] = 0
        entry[13] = checksum
        entry[26:28] = b"\x00\x00"
        for i, unit in enumerate(units[:5]):
            struct.pack_into("<H", entry, 1 + i * 2, unit)
        for i, unit in enumerate(units[5:11]):
            struct.pack_into("<H", entry, 14 + i * 2, unit)
        for i, unit in enumerate(units[11:13]):
            struct.pack_into("<H", entry, 28 + i * 2, unit)
        out.extend(entry)

    return bytes(out)


def first_cluster(node: Node) -> int:
    return node.clusters[0] if node.clusters else 0


def short_entry(short_name: bytes, attr: int, cluster: int, size: int) -> bytes:
    entry = bytearray(32)
    entry[:11] = short_name
    entry[11] = attr
    struct.pack_into("<H", entry, 20, (cluster >> 16) & 0xFFFF)
    struct.pack_into("<H", entry, 26, cluster & 0xFFFF)
    struct.pack_into("<I", entry, 28, size)
    return bytes(entry)


def dot_short_name(name: str) -> bytes:
    return f"{name:<11}".encode("ascii")


def serialise_directory(node: Node, parent: Node | None) -> bytes:
    out = bytearray()
    if parent is not None:
        # FAT represents the root directory as cluster 0 in a root child's
        # ".." entry, while nested directories point to their real parent.
        parent_cluster = 0 if parent.name == "" else first_cluster(parent)
        out.extend(short_entry(dot_short_name("."), 0x10, first_cluster(node), 0))
        out.extend(short_entry(dot_short_name(".."), 0x10, parent_cluster, 0))

    for child in node.children:
        if child.needs_lfn:
            out.extend(make_lfn_entries(child.name, child.short_name))
        attr = 0x10 if child.is_dir else 0x20
        size = 0 if child.is_dir else child.size
        out.extend(short_entry(child.short_name, attr, first_cluster(child), size))

    capacity = len(node.clusters) * CLUSTER_SIZE
    if len(out) > capacity:
        die(f"directory {node.path} grew beyond its allocated clusters")
    return bytes(out).ljust(capacity, b"\x00")


def build_boot_sector(total_sectors: int, fat_size_sectors: int) -> bytes:
    sector = bytearray(BYTES_PER_SECTOR)
    sector[0:3] = b"\xeb\x58\x90"
    sector[3:11] = b"NEMUFS  "
    struct.pack_into("<H", sector, 11, BYTES_PER_SECTOR)
    sector[13] = SECTORS_PER_CLUSTER
    struct.pack_into("<H", sector, 14, RESERVED_SECTORS)
    sector[16] = FAT_COUNT
    struct.pack_into("<H", sector, 17, 0)
    struct.pack_into("<H", sector, 19, 0)
    sector[21] = 0xF8
    struct.pack_into("<H", sector, 22, 0)
    struct.pack_into("<H", sector, 24, 32)
    struct.pack_into("<H", sector, 26, 64)
    struct.pack_into("<I", sector, 28, 0)
    struct.pack_into("<I", sector, 32, total_sectors)
    struct.pack_into("<I", sector, 36, fat_size_sectors)
    struct.pack_into("<H", sector, 40, 0)
    struct.pack_into("<H", sector, 42, 0)
    struct.pack_into("<I", sector, 44, ROOT_CLUSTER)
    struct.pack_into("<H", sector, 48, 1)
    struct.pack_into("<H", sector, 50, 6)
    sector[64] = 0x80
    sector[66] = 0x29
    struct.pack_into("<I", sector, 67, 0x4E454D55)
    sector[71:82] = b"NO NAME    "
    sector[82:90] = b"FAT32   "
    sector[510:512] = b"\x55\xaa"
    return bytes(sector)


def build_fsinfo(free_clusters: int, next_free: int) -> bytes:
    sector = bytearray(BYTES_PER_SECTOR)
    struct.pack_into("<I", sector, 0, 0x41615252)
    struct.pack_into("<I", sector, 484, 0x61417272)
    struct.pack_into("<I", sector, 488, free_clusters)
    struct.pack_into("<I", sector, 492, next_free)
    struct.pack_into("<I", sector, 508, 0xAA550000)
    return bytes(sector)


def build_fat(fat_size_sectors: int, nodes: list[Node]) -> bytes:
    fat = bytearray(fat_size_sectors * BYTES_PER_SECTOR)
    struct.pack_into("<I", fat, 0, 0x0FFFFFF8)
    struct.pack_into("<I", fat, 4, EOC)

    def mark_chain(clusters: list[int]) -> None:
        for index, cluster in enumerate(clusters):
            value = clusters[index + 1] if index + 1 < len(clusters) else EOC
            struct.pack_into("<I", fat, cluster * 4, value)

    for node in nodes:
        mark_chain(node.clusters)
    return bytes(fat)


def flatten_nodes(root: Node) -> list[Node]:
    out = [root]
    for child in root.children:
        out.extend(flatten_nodes(child))
    return out


def sector_offset(sector: int) -> int:
    return sector * BYTES_PER_SECTOR


def cluster_sector(first_data_sector: int, cluster: int) -> int:
    return first_data_sector + (cluster - ROOT_CLUSTER) * SECTORS_PER_CLUSTER


def write_cluster_chain(image, first_data_sector: int, clusters: list[int], data: bytes) -> None:
    for index, cluster in enumerate(clusters):
        chunk = data[index * CLUSTER_SIZE : (index + 1) * CLUSTER_SIZE]
        image.seek(sector_offset(cluster_sector(first_data_sector, cluster)))
        image.write(chunk.ljust(CLUSTER_SIZE, b"\x00"))


def write_image(root: Node, output: Path) -> None:
    next_free = allocate_clusters(root)
    used_clusters = next_free - ROOT_CLUSTER
    # Keep writable space for runtime state.  Large ONScripter images can be
    # bigger than the FAT32 minimum data area; without this reserve the image is
    # valid but completely full, so save2.dat cannot allocate its first cluster.
    runtime_free_clusters = math.ceil(RUNTIME_FREE_BYTES / CLUSTER_SIZE)
    data_clusters = max(MIN_FAT32_CLUSTERS, used_clusters + runtime_free_clusters)
    # Keep the advertised data area and FAT length self-consistent: Task 2's
    # parser requires at least cluster_count + 2 FAT entries.
    fat_size_sectors = math.ceil((data_clusters + 2) * 4 / BYTES_PER_SECTOR)
    total_sectors = RESERVED_SECTORS + FAT_COUNT * fat_size_sectors + data_clusters * SECTORS_PER_CLUSTER
    first_data_sector = RESERVED_SECTORS + FAT_COUNT * fat_size_sectors

    nodes = flatten_nodes(root)
    fat = build_fat(fat_size_sectors, nodes)
    boot = build_boot_sector(total_sectors, fat_size_sectors)
    fsinfo = build_fsinfo(data_clusters - used_clusters, next_free if next_free <= data_clusters + 1 else EOC)

    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("wb") as image:
        image.truncate(total_sectors * BYTES_PER_SECTOR)
        image.seek(0)
        image.write(boot)
        image.seek(sector_offset(1))
        image.write(fsinfo)
        image.seek(sector_offset(6))
        image.write(boot)
        image.seek(sector_offset(7))
        image.write(fsinfo)
        for fat_index in range(FAT_COUNT):
            image.seek(sector_offset(RESERVED_SECTORS + fat_index * fat_size_sectors))
            image.write(fat)

        def write_dirs(node: Node, parent: Node | None) -> None:
            write_cluster_chain(image, first_data_sector, node.clusters, serialise_directory(node, parent))
            for child in node.children:
                if child.is_dir:
                    write_dirs(child, node)

        write_dirs(root, None)

        for node in nodes:
            if not node.is_dir and node.clusters:
                with node.path.open("rb") as src:
                    write_cluster_chain(image, first_data_sector, node.clusters, src.read())


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="Build a deterministic FAT32 image")
    parser.add_argument("fsroot", type=Path)
    parser.add_argument("image", type=Path)
    args = parser.parse_args(argv)

    root = scan_tree(args.fsroot)
    assign_short_names(root)
    write_image(root, args.image)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
