#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Loopback-only view of an original HMI renderer frame with diagnostic taps."""
import argparse
import hmac
from http.server import BaseHTTPRequestHandler, HTTPServer
import json
from pathlib import Path
import secrets
import struct
import zlib

from hmi_diagnosis import DiagnosisError, HmiDiagnosis


def frame_pixels(frame):
    header = b"P6\n1920 720\n255\n"
    if not frame.startswith(header) or len(frame) != len(header) + 1920 * 720 * 3:
        raise ValueError("Renderer frame is incomplete or not 1920x720 RGB PPM")
    return memoryview(frame)[len(header):]


def frame_png(frame):
    pixels = frame_pixels(frame)
    rows = b"".join(b"\0" + pixels[y * 5760:(y + 1) * 5760]
                    for y in range(720))

    def chunk(kind, payload):
        return (struct.pack(">I", len(payload)) + kind + payload +
                struct.pack(">I", zlib.crc32(kind + payload)))

    return (b"\x89PNG\r\n\x1a\n" +
            chunk(b"IHDR", struct.pack(">IIBBBBB", 1920, 720, 8, 2, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(rows, 1)) + chunk(b"IEND", b""))


PAGE = """<!doctype html><meta charset="utf-8">
<title>MH2P original HMI development input</title>
<style>
body { background:#181818; color:#eee; font:16px sans-serif; margin:16px }
img { width:100%; max-width:1920px; touch-action:manipulation }
pre { white-space:pre-wrap } button { margin:8px }
</style>
<h2>Original MH2P HMI</h2>
<p>Experimental graphics bridge and original developer touch service.
Not native HID, RCC or BAP readiness.</p>
<img id="frame" alt="Waiting for original renderer frame">
<button id="screen">Query original screen state</button><pre id="status"></pre>
<script>
const token = TOKEN;
const image = document.getElementById('frame');
const status = document.getElementById('status');
let busy = false;
async function request(path, body) {
  const response = await fetch(path, body === undefined ? {} : {
    method:'POST', headers:{'Content-Type':'application/json','X-HMI-Token':token},
    body:JSON.stringify(body)});
  if (!response.ok) throw new Error(await response.text());
  return response;
}
async function refresh() {
  try {
    const response = await request('/frame.png');
    const url = URL.createObjectURL(await response.blob());
    const previous = image.src;
    image.onload = () => { if (previous.startsWith('blob:')) URL.revokeObjectURL(previous); };
    image.src = url;
  } catch (error) { status.textContent = error.message; }
  setTimeout(refresh, 1000);
}
image.addEventListener('click', async event => {
  if (busy || !image.naturalWidth) return;
  const rect = image.getBoundingClientRect();
  const x = Math.floor((event.clientX - rect.left) * 1920 / rect.width);
  const y = Math.floor((event.clientY - rect.top) * 720 / rect.height);
  busy = true;
  status.textContent = `Sending original typed touch (${x},${y})...`;
  try { status.textContent = await (await request('/tap', {x,y})).text(); }
  catch (error) { status.textContent = error.message; }
  finally { busy = false; }
});
document.getElementById('screen').onclick = async () => {
  try { status.textContent = await (await request('/screen', {})).text(); }
  catch (error) { status.textContent = error.message; }
};
refresh();
</script>
"""


class DevelopmentServer(HTTPServer):
    def __init__(self, port, frame, tap, screen):
        self.frame = Path(frame)
        self.tap = tap
        self.screen = screen
        self.token = secrets.token_hex(24)
        super().__init__(("127.0.0.1", port), DevelopmentHandler)


class DevelopmentHandler(BaseHTTPRequestHandler):
    def setup(self):
        super().setup()
        self.connection.settimeout(10)

    def respond(self, code, content, kind="text/plain; charset=utf-8"):
        data = content.encode() if isinstance(content, str) else content
        self.send_response(code)
        self.send_header("Content-Type", kind)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("Content-Security-Policy",
                         "default-src 'self'; img-src 'self' blob:; "
                         "script-src 'unsafe-inline'; style-src 'unsafe-inline'; "
                         "frame-ancestors 'none'")
        self.end_headers()
        self.wfile.write(data)

    def allowed_host(self):
        return self.headers.get("Host") == (
            f"127.0.0.1:{self.server.server_port}")

    def do_GET(self):
        if not self.allowed_host():
            self.respond(403, "Use the loopback URL printed by the server")
        elif self.path == "/":
            self.respond(200, PAGE.replace("TOKEN", json.dumps(self.server.token)),
                         "text/html; charset=utf-8")
        elif self.path == "/frame.png":
            try:
                image = frame_png(self.server.frame.read_bytes())
            except (OSError, ValueError) as exc:
                self.respond(503, f"Original renderer frame unavailable: {exc}")
            else:
                self.respond(200, image, "image/png")
        else:
            self.respond(404, "Not found")

    def do_POST(self):
        if not self.allowed_host() or not hmac.compare_digest(
                self.headers.get("X-HMI-Token", "").encode("utf-8"),
                self.server.token.encode("ascii")):
            self.respond(403, "Invalid development input token or host")
            return
        try:
            length = int(self.headers.get("Content-Length", "0"))
            if not 0 < length <= 512:
                raise ValueError("Request must be 1..512 bytes")
            value = json.loads(self.rfile.read(length))
            if not isinstance(value, dict):
                raise ValueError("Expected a JSON object")
            if self.path == "/tap":
                x, y = value.get("x"), value.get("y")
                if (set(value) != {"x", "y"} or type(x) is not int or
                        type(y) is not int or not 0 <= x < 1920 or
                        not 0 <= y < 720):
                    raise ValueError("Expected integer x,y within 1920x720")
                reply = self.server.tap(x, y)
            elif self.path == "/screen" and not value:
                reply = self.server.screen()
            else:
                self.respond(404, "Unknown development input action")
                return
        except (ValueError, UnicodeError) as exc:
            self.respond(400, str(exc))
        except (OSError, DiagnosisError) as exc:
            self.respond(502, f"Original HMI diagnosis failed: {exc}")
        else:
            self.respond(200, reply)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--frame", type=Path, required=True,
                        help="Live original-renderer 1920x720 PPM file")
    parser.add_argument("--host", required=True,
                        help="Reachable guest diagnosis address or isolated relay")
    parser.add_argument("--diagnosis-port", type=int, default=1501)
    parser.add_argument("--port", type=int, default=8080)
    args = parser.parse_args()

    def call(action, *values):
        with HmiDiagnosis(args.host, args.diagnosis_port) as diagnosis:
            return (diagnosis.screen() if action == "screen" else
                    diagnosis.touch("tap", *values))

    with DevelopmentServer(args.port, args.frame,
                           lambda x, y: call("tap", x, y),
                           lambda: call("screen")) as server:
        print(f"Original HMI development view: "
              f"http://127.0.0.1:{server.server_port}/", flush=True)
        try:
            server.serve_forever()
        except KeyboardInterrupt:
            pass


if __name__ == "__main__":
    main()
