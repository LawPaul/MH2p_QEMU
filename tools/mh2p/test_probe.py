import argparse
import unittest

from probe import read32_address


class Read32AddressTests(unittest.TestCase):
    def test_valid_addresses(self):
        for text, address in (("0", 0), ("4294967292", 0xfffffffc),
                              ("0x70019010", 0x70019010)):
            with self.subTest(text=text):
                self.assertEqual(read32_address(text), address)

    def test_invalid_addresses(self):
        for text in ("-4", "0x100000000", "1", "0x70019011", "hello", "0;quit"):
            with self.subTest(text=text), self.assertRaises(argparse.ArgumentTypeError):
                read32_address(text)


if __name__ == "__main__":
    unittest.main()
