import hashlib
import struct
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts" / "mkfat32.py"
EOC_MIN = 0x0FFFFFF8


def read_sector(path: Path, sector: int) -> bytes:
    with path.open("rb") as f:
        f.seek(sector * 512)
        return f.read(512)


@dataclass(frozen=True)
class Fat32Layout:
    reserved: int
    fats: int
    fatsz: int
    total: int
    spc: int
    first_data: int
    cluster_count: int


def parse_layout(image: Path) -> Fat32Layout:
    boot = read_sector(image, 0)
    reserved = struct.unpack_from("<H", boot, 14)[0]
    fats = boot[16]
    fatsz = struct.unpack_from("<I", boot, 36)[0]
    total = struct.unpack_from("<I", boot, 32)[0]
    spc = boot[13]
    first_data = reserved + fats * fatsz
    cluster_count = (total - first_data) // spc
    return Fat32Layout(reserved, fats, fatsz, total, spc, first_data, cluster_count)


def first_sector_of_cluster(layout: Fat32Layout, cluster: int) -> int:
    return layout.first_data + (cluster - 2) * layout.spc


def read_cluster(path: Path, layout: Fat32Layout, cluster: int) -> bytes:
    return b"".join(read_sector(path, first_sector_of_cluster(layout, cluster) + i) for i in range(layout.spc))


def first_cluster(entry: bytes) -> int:
    high = struct.unpack_from("<H", entry, 20)[0]
    low = struct.unpack_from("<H", entry, 26)[0]
    return (high << 16) | low


def short_name(entry: bytes) -> bytes:
    return entry[:11]


def file_size(entry: bytes) -> int:
    return struct.unpack_from("<I", entry, 28)[0]


def directory_entries(data: bytes) -> list[bytes]:
    entries = []
    for offset in range(0, len(data), 32):
        entry = data[offset : offset + 32]
        if entry[0] == 0x00:
            break
        entries.append(entry)
    return entries


def find_short_entry(entries: list[bytes], name: bytes) -> bytes:
    for entry in entries:
        if entry[11] != 0x0F and short_name(entry) == name:
            return entry
    raise AssertionError(f"short entry {name!r} not found")


def fat_entry(image: Path, layout: Fat32Layout, cluster: int) -> int:
    with image.open("rb") as f:
        f.seek((layout.reserved * 512) + cluster * 4)
        return struct.unpack("<I", f.read(4))[0] & 0x0FFFFFFF


def lfn_checksum(short: bytes) -> int:
    checksum = 0
    for byte in short:
        checksum = (((checksum & 1) << 7) + (checksum >> 1) + byte) & 0xFF
    return checksum


def decode_lfn_payload(entry: bytes) -> str:
    chars = []
    for offset in (1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30):
        unit = struct.unpack_from("<H", entry, offset)[0]
        if unit == 0x0000:
            break
        if unit != 0xFFFF:
            chars.append(chr(unit))
    return "".join(chars)


def build_image(fsroot: Path, image: Path) -> None:
    subprocess.check_call([sys.executable, str(SCRIPT), str(fsroot), str(image)])


def test_mkfat32_writes_valid_boot_sector(tmp_path: Path) -> None:
    fsroot = tmp_path / "fsimg"
    fsroot.mkdir()
    (fsroot / "hello.txt").write_text("hello\n", encoding="ascii")
    image = tmp_path / "ramdisk.img"

    build_image(fsroot, image)

    boot = read_sector(image, 0)
    assert boot[510:512] == b"\x55\xaa"
    assert struct.unpack_from("<H", boot, 11)[0] == 512
    assert boot[13] in (1, 2, 4, 8, 16, 32, 64, 128)
    assert struct.unpack_from("<H", boot, 14)[0] == 32
    assert boot[16] == 2
    assert struct.unpack_from("<H", boot, 17)[0] == 0
    assert struct.unpack_from("<H", boot, 22)[0] == 0
    assert struct.unpack_from("<I", boot, 36)[0] > 0
    assert struct.unpack_from("<I", boot, 44)[0] == 2
    assert boot[71:82] == b"NO NAME    "


def test_mkfat32_emits_lfn_entry_for_lowercase_name(tmp_path: Path) -> None:
    fsroot = tmp_path / "fsimg"
    fsroot.mkdir()
    (fsroot / "long-name.txt").write_text("abc", encoding="ascii")
    image = tmp_path / "ramdisk.img"

    build_image(fsroot, image)

    boot = read_sector(image, 0)
    reserved = struct.unpack_from("<H", boot, 14)[0]
    fats = boot[16]
    fatsz = struct.unpack_from("<I", boot, 36)[0]
    first_data = reserved + fats * fatsz
    root = read_sector(image, first_data)
    assert root[11] == 0x0f
    assert root[32 + 11] & 0x20


def test_mkfat32_is_deterministic(tmp_path: Path) -> None:
    fsroot = tmp_path / "fsimg"
    (fsroot / "dir").mkdir(parents=True)
    (fsroot / "b.txt").write_text("b", encoding="ascii")
    (fsroot / "A.TXT").write_text("a", encoding="ascii")
    (fsroot / "dir" / "child.txt").write_text("child", encoding="ascii")
    first = tmp_path / "first.img"
    second = tmp_path / "second.img"

    build_image(fsroot, first)
    build_image(fsroot, second)

    assert hashlib.sha256(first.read_bytes()).digest() == hashlib.sha256(second.read_bytes()).digest()


def test_mkfat32_layout_has_task2_compatible_fat_capacity(tmp_path: Path) -> None:
    fsroot = tmp_path / "fsimg"
    fsroot.mkdir()
    (fsroot / "hello.txt").write_text("hello\n", encoding="ascii")
    image = tmp_path / "ramdisk.img"

    build_image(fsroot, image)

    layout = parse_layout(image)
    assert layout.reserved == 32
    assert layout.fats == 2
    assert layout.first_data == layout.reserved + layout.fats * layout.fatsz
    assert layout.cluster_count >= 65525
    assert layout.fatsz * 512 // 4 >= layout.cluster_count + 2


def test_root_level_directory_dotdot_points_to_cluster_zero(tmp_path: Path) -> None:
    fsroot = tmp_path / "fsimg"
    (fsroot / "EMPTYDIR").mkdir(parents=True)
    image = tmp_path / "ramdisk.img"

    build_image(fsroot, image)

    layout = parse_layout(image)
    root_entries = directory_entries(read_cluster(image, layout, 2))
    emptydir = find_short_entry(root_entries, b"EMPTYDIR   ")
    emptydir_entries = directory_entries(read_cluster(image, layout, first_cluster(emptydir)))
    assert first_cluster(find_short_entry(emptydir_entries, b".          ")) == first_cluster(emptydir)
    assert first_cluster(find_short_entry(emptydir_entries, b"..         ")) == 0


def test_nested_directory_dotdot_points_to_parent_cluster(tmp_path: Path) -> None:
    fsroot = tmp_path / "fsimg"
    (fsroot / "PARENT" / "CHILD").mkdir(parents=True)
    image = tmp_path / "ramdisk.img"

    build_image(fsroot, image)

    layout = parse_layout(image)
    root_entries = directory_entries(read_cluster(image, layout, 2))
    parent = find_short_entry(root_entries, b"PARENT     ")
    parent_entries = directory_entries(read_cluster(image, layout, first_cluster(parent)))
    child = find_short_entry(parent_entries, b"CHILD      ")
    child_entries = directory_entries(read_cluster(image, layout, first_cluster(child)))
    assert first_cluster(find_short_entry(child_entries, b"..         ")) == first_cluster(parent)


def test_mkfat32_multi_slot_lfn_order_checksum_and_payload(tmp_path: Path) -> None:
    fsroot = tmp_path / "fsimg"
    fsroot.mkdir()
    long_name = "very-long-name.txt"
    (fsroot / long_name).write_text("abc", encoding="ascii")
    image = tmp_path / "ramdisk.img"

    build_image(fsroot, image)

    layout = parse_layout(image)
    entries = directory_entries(read_cluster(image, layout, 2))
    lfn_entries = [entry for entry in entries if entry[11] == 0x0F]
    short = entries[len(lfn_entries)]
    checksum = lfn_checksum(short_name(short))

    assert len(lfn_entries) > 1
    assert all(entry[11] == 0x0F for entry in lfn_entries)
    assert all(entry[13] == checksum for entry in lfn_entries)
    assert lfn_entries[0][0] & 0x40
    assert not any(entry[0] & 0x40 for entry in lfn_entries[1:])
    reconstructed = "".join(
        decode_lfn_payload(entry) for entry in sorted(lfn_entries, key=lambda entry: entry[0] & 0x1F)
    )
    assert reconstructed == long_name


def test_zero_length_file_has_no_cluster(tmp_path: Path) -> None:
    fsroot = tmp_path / "fsimg"
    fsroot.mkdir()
    (fsroot / "EMPTY.TXT").write_bytes(b"")
    image = tmp_path / "ramdisk.img"

    build_image(fsroot, image)

    layout = parse_layout(image)
    entries = directory_entries(read_cluster(image, layout, 2))
    empty = find_short_entry(entries, b"EMPTY   TXT")
    assert first_cluster(empty) == 0
    assert file_size(empty) == 0


def test_multi_cluster_file_has_valid_fat_chain(tmp_path: Path) -> None:
    fsroot = tmp_path / "fsimg"
    fsroot.mkdir()
    payload = b"a" * 1300
    (fsroot / "BIG.BIN").write_bytes(payload)
    image = tmp_path / "ramdisk.img"

    build_image(fsroot, image)

    layout = parse_layout(image)
    entries = directory_entries(read_cluster(image, layout, 2))
    big = find_short_entry(entries, b"BIG     BIN")
    first = first_cluster(big)
    second = fat_entry(image, layout, first)
    third = fat_entry(image, layout, second)
    end = fat_entry(image, layout, third)
    assert first >= 3
    assert second == first + 1
    assert third == second + 1
    assert end >= EOC_MIN
    assert file_size(big) == len(payload)


def test_colliding_long_names_get_distinct_short_aliases(tmp_path: Path) -> None:
    fsroot = tmp_path / "fsimg"
    fsroot.mkdir()
    (fsroot / "longfilename-one.txt").write_text("one", encoding="ascii")
    (fsroot / "longfilename-two.txt").write_text("two", encoding="ascii")
    image = tmp_path / "ramdisk.img"

    build_image(fsroot, image)

    layout = parse_layout(image)
    entries = directory_entries(read_cluster(image, layout, 2))
    short_entries = [entry for entry in entries if entry[11] != 0x0F]
    aliases = {short_name(entry) for entry in short_entries}
    assert len(aliases) == 2
    assert aliases == {b"LONGFI~1TXT", b"LONGFI~2TXT"}
