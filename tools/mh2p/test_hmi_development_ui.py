# SPDX-License-Identifier: GPL-2.0-or-later
from http.client import HTTPConnection
import json
from pathlib import Path
import struct
import tempfile
import threading
import unittest
import zlib

from hmi_development_ui import DevelopmentServer, frame_pixels, frame_png
from hmi_diagnosis import DiagnosisError


class DevelopmentUiTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.frame = Path(self.temporary.name) / "frame.ppm"
        self.taps = []
        self.error = False

        def tap(x, y):
            if self.error:
                raise DiagnosisError("original command rejected")
            self.taps.append((x, y))
            return "response_processcommand   Call executed successfully."

        self.server = DevelopmentServer(
            0, self.frame, tap, lambda: "response_getvalueScreenData(id:1)")
        self.thread = threading.Thread(target=self.server.serve_forever)
        self.thread.start()
        self.addCleanup(self.stop)

    def stop(self):
        self.server.shutdown()
        self.thread.join(timeout=5)
        self.server.server_close()
        self.assertFalse(self.thread.is_alive())

    def request(self, method, path, value=None, *, token=True, host=None):
        connection = HTTPConnection("127.0.0.1", self.server.server_port,
                                    timeout=3)
        headers = {}
        if token:
            headers["X-HMI-Token"] = self.server.token
        if host is not None:
            headers["Host"] = host
        body = json.dumps(value).encode() if value is not None else None
        try:
            connection.request(method, path, body=body, headers=headers)
            response = connection.getresponse()
            return response.status, response.read(), dict(response.getheaders())
        finally:
            connection.close()

    def test_original_frame_png_has_correct_pixels_and_chunks(self):
        row = bytes(range(256)) * 22 + bytes(range(128))
        self.assertEqual(len(row), 5760)
        pixels = row * 720
        ppm = b"P6\n1920 720\n255\n" + pixels
        self.frame.write_bytes(ppm)
        code, png, headers = self.request("GET", "/frame.png")
        self.assertEqual(code, 200)
        self.assertEqual(headers["Content-Type"], "image/png")
        self.assertEqual(png[:8], b"\x89PNG\r\n\x1a\n")
        offset = 8
        compressed = b""
        kinds = []
        while offset < len(png):
            size = struct.unpack_from(">I", png, offset)[0]
            kind = png[offset + 4:offset + 8]
            data = png[offset + 8:offset + 8 + size]
            crc = struct.unpack_from(">I", png, offset + 8 + size)[0]
            self.assertEqual(crc, zlib.crc32(kind + data))
            if kind == b"IDAT":
                compressed += data
            kinds.append(kind)
            offset += 12 + size
        self.assertEqual(kinds, [b"IHDR", b"IDAT", b"IEND"])
        self.assertEqual(zlib.decompress(compressed), (b"\0" + row) * 720)
        self.assertEqual(headers["Cache-Control"], "no-store")

    def test_frame_unavailable_or_partial_fails_explicitly(self):
        self.assertEqual(self.request("GET", "/frame.png")[0], 503)
        self.frame.write_bytes(b"P6\n1920 720\n255\n")
        self.assertEqual(self.request("GET", "/frame.png")[0], 503)
        with self.assertRaises(ValueError):
            frame_png(b"P6\n1 1\n255\n123")

    def test_pixel_validation_retains_exact_bytes_without_encoding(self):
        pixels = bytes(range(256)) * (1920 * 720 * 3 // 256)
        frame = b"P6\n1920 720\n255\n" + pixels
        view = frame_pixels(frame)
        self.assertIs(view.obj, frame)
        self.assertEqual(view, pixels)
        for invalid in (frame[:-1], frame + b"\0",
                        frame.replace(b"1920 720", b"720 1920", 1)):
            with self.subTest(header=invalid[:20]), self.assertRaises(ValueError):
                frame_pixels(invalid)

    def test_page_and_authenticated_tap_and_screen(self):
        code, body, _ = self.request("GET", "/")
        self.assertEqual(code, 200)
        self.assertIn(self.server.token.encode(), body)
        code, body, _ = self.request("POST", "/tap", {"x": 1919, "y": 719})
        self.assertEqual(code, 200)
        self.assertIn(b"successfully", body)
        self.assertEqual(self.taps, [(1919, 719)])
        self.assertEqual(self.request("POST", "/screen", {})[:2],
                         (200, b"response_getvalueScreenData(id:1)"))

    def test_rejects_cross_origin_hosts_tokens_and_invalid_coordinates(self):
        self.assertEqual(self.request("GET", "/", host="outside.example")[0],
                         403)
        self.assertEqual(self.request("POST", "/tap", {"x": 1, "y": 1},
                                      token=False)[0], 403)
        for value in ({"x": -1, "y": 0}, {"x": 1920, "y": 0},
                      {"x": 0, "y": 720}, {"x": True, "y": 0},
                      {"x": 0, "y": 0, "extra": 1}, []):
            self.assertEqual(self.request("POST", "/tap", value)[0], 400)
        self.assertEqual(self.taps, [])

    def test_original_failure_is_not_reported_as_success(self):
        self.error = True
        code, body, _ = self.request("POST", "/tap", {"x": 1, "y": 1})
        self.assertEqual(code, 502)
        self.assertIn(b"original command rejected", body)
        self.assertEqual(self.taps, [])


if __name__ == "__main__":
    unittest.main()
