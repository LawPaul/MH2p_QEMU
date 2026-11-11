"""Exercise the shared read-only reader with MH2P-sized blocks and old sizes."""
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch

with patch.object(sys, "path", [
        str(Path(__file__).resolve().parents[1] / "mhi2"), *sys.path]):
    from qnx6_read import Qnx6
    from qnx6_image import build
    import qnx6_image


def fixture(block_size, base=0):
    data_start = ((0x3000 + block_size - 1) // block_size) * block_size
    image = bytearray(base + data_start + 4 * block_size)
    sb = base + 0x2000
    struct.pack_into("<I", image, sb, 0x68191122)
    struct.pack_into("<I", image, sb + 48, block_size)
    struct.pack_into("<I", image, sb + 52, 2)
    struct.pack_into("<I", image, sb + 60, 4)
    struct.pack_into("<QI", image, sb + 72, 256, 0)
    data = base + data_start
    struct.pack_into("<Q", image, data, 96)
    struct.pack_into("<H", image, data + 32, 0o40755)
    struct.pack_into("<I", image, data + 36, 1)
    payload = b"MH2P read-only filesystem payload"
    inode = data + 128
    struct.pack_into("<Q", image, inode, len(payload))
    struct.pack_into("<H", image, inode + 32, 0o100644)
    struct.pack_into("<I", image, inode + 36, 2)
    for i, (name, ino) in enumerate(((b".", 1), (b"..", 1), (b"payload.bin", 2))):
        at = data + block_size + i * 32
        struct.pack_into("<IB", image, at, ino, len(name))
        image[at + 5:at + 5 + len(name)] = name
    image[data + 2 * block_size:data + 2 * block_size + len(payload)] = payload
    return bytes(image), payload


class Qnx6BlockAlignmentTests(unittest.TestCase):
    def test_builder_reserves_boot_inode(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "source"
            source.mkdir()
            (source / "!first").mkdir()
            (source / "persistence").mkdir()
            image = root / "filesystem.img"
            for existing in (False, True):
                if existing:
                    (source / ".boot").mkdir()
                    (source / ".boot" / "image").write_bytes(b"boot payload")
                with self.subTest(existing_boot=existing):
                    build(str(source), str(image))
                    fs = Qnx6(image, 0)
                    try:
                        entries = dict(fs.listdir(1))
                        self.assertEqual(entries[".boot"], 2)
                        self.assertEqual(fs.inode(2)["mode"], 0o040700)
                        self.assertGreater(entries["!first"], 2)
                        self.assertGreater(entries["persistence"], 2)
                        boot = dict(fs.listdir(2))
                        self.assertEqual(boot["."], 2)
                        self.assertEqual(boot[".."], 1)
                        if existing:
                            self.assertEqual(fs.read_file(boot["image"]),
                                             b"boot payload")
                        else:
                            self.assertFalse((source / ".boot").exists())
                    finally:
                        fs.f.close()

    def test_builder_rejects_invalid_boot_directory(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "source"
            source.mkdir()
            boot = source / ".boot"
            image = root / "filesystem.img"
            boot.write_bytes(b"not a directory")
            with self.assertRaisesRegex(ValueError,
                                        r"\.boot must be a directory"):
                build(str(source), str(image))
            boot.unlink()
            boot.symlink_to(source, target_is_directory=True)
            with self.assertRaisesRegex(ValueError,
                                        r"\.boot must be a directory"):
                build(str(source), str(image))
            self.assertFalse(image.exists())

    def test_partition_bitmap_covers_all_advertised_blocks(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "source"
            source.mkdir()
            (source / "persistence").mkdir()
            for partition_blocks in (16384, 131072):
                with self.subTest(partition_blocks=partition_blocks), \
                        patch.object(qnx6_image, "INODE_SLACK", 1024):
                    image = root / "filesystem.img"
                    build(str(source), str(image),
                          partition_blocks=partition_blocks)
                    self.assertEqual(image.stat().st_size,
                                     partition_blocks * qnx6_image.BS)
                    fs = Qnx6(image, 0)
                    try:
                        sb = fs._raw(0x2000, 512)
                        blocks, free_blocks = struct.unpack_from("<II", sb, 60)
                        bitmap_root = fs._rootnode(sb, 152)
                        self.assertEqual(bitmap_root["size"], (blocks + 7) // 8)
                        bitmap = fs._read_tree(bitmap_root)
                        self.assertEqual(len(bitmap), (blocks + 7) // 8)
                        allocated = sum(
                            bool(bitmap[index // 8] & (1 << (index % 8)))
                            for index in range(blocks))
                        self.assertEqual(blocks - allocated, free_blocks)
                        last = fs._block(fs._bmap(
                            bitmap_root, (len(bitmap) - 1) // fs.bs))
                        tail_start = blocks % (fs.bs * 8)
                        if tail_start:
                            self.assertTrue(all(
                                last[index // 8] & (1 << (index % 8))
                                for index in range(tail_start, fs.bs * 8)))
                        self.assertIn("persistence", dict(fs.listdir(1)))
                    finally:
                        fs.f.close()

    def test_builder_empty_and_mapped_files(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "source"
            source.mkdir()
            payload = bytes(range(256)) * 33
            name = "long-graphics-resource-name.bin"
            (source / name).write_bytes(payload)
            (source / "empty").write_bytes(b"")
            image = root / "filesystem.img"
            build(str(source), str(image))
            fs = Qnx6(image, 0)
            try:
                entries = dict(fs.listdir(1))
                self.assertEqual(fs.read_file(entries[name]), payload)
                self.assertEqual(fs.read_file(entries["empty"]), b"")
            finally:
                fs.f.close()

    def test_directory_and_file_across_block_sizes(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "filesystem.img"
            for size in (512, 1024, 2048, 4096, 8192, 16384):
                for base in (0, 0x200000):
                    with self.subTest(size=size, base=base):
                        image, payload = fixture(size, base)
                        path.write_bytes(image)
                        fs = Qnx6(path, base)
                        try:
                            self.assertEqual(fs.listdir(1),
                                             [(".", 1), ("..", 1), ("payload.bin", 2)])
                            self.assertTrue(fs.is_dir(1))
                            self.assertEqual(fs.read_file(2), payload)
                        finally:
                            fs.f.close()
                        self.assertEqual(path.read_bytes(), image)


if __name__ == "__main__":
    unittest.main()
