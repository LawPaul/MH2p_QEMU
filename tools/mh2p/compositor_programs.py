"""Bounded NVvp5/NVfp5 transcriptions for the original P2865 compositor."""
import hashlib


DISPLAYMANAGER_SHA256 = "47c21029e2dcfd25423cf076d0906b3d938db290f2db7151cd3fe746e034af63"
ANIMATION_SHA256 = "8e2dd1997d1b1c3a371b19a2d1092309a26778ed4250f34d6af538d15db66dde"

# Bindings come from the binaries' reflected GL types and per-stage c[] slots.
# Coordinates, reciprocal scaling, Y inversion and alpha follow the NV code.
PROGRAMS = (
    (
        "DisplayableDebugTexture", 0x82208, 10085,
        "4a2d2b6424c0f3014c731909c60bda0962191f31c30066f6f83b7819b4f680d4",
        b"""#version 300 es
in vec2 position;
uniform vec4 uSize, uSource, uTarget;
out highp vec2 texout;
void main() {
    vec2 destination = (position * uTarget.zw + uTarget.xy) / uSize.zw;
    texout = (position * uSource.zw + uSource.xy) / uSize.xy;
    gl_Position = vec4(destination * vec2(2.0, -2.0) + vec2(-1.0, 1.0),
                       0.0, 1.0);
}
""",
        b"""#version 300 es
#extension GL_OES_EGL_image_external_essl3 : require
precision highp float;
precision highp int;
uniform samplerExternalOES uTex;
uniform vec4 uTarget;
uniform int uDigits[10], uDisplayable;
uniform float uOpacity;
in highp vec2 texout;
out vec4 color;
void main() {
    vec4 sampled = texture(uTex, texout);
    float cursor = 10.0;
    int marked = 0;
    for (int exponent = 2; exponent >= 0; --exponent) {
        float place = pow(10.0, float(exponent));
        float quotient = float(uDisplayable) / place;
        int digit = int(quotient - floor(quotient * 0.1) * 10.0);
        vec2 cell = floor((texout * uTarget.zw
                          - vec2(cursor, uTarget.w - 59.0)) * 0.14285715);
        // Native SGT.S returns -1; its negation gates both ink and advance.
        int enabled = uDisplayable > int(place) ? 1 : 0;
        if (cell.x >= 0.0 && cell.x < 3.0 && cell.y >= 0.0 && cell.y < 7.0) {
            int bit = int(cell.y * 3.0 + cell.x);
            marked += (uDigits[digit] & (1 << bit)) * enabled;
        }
        cursor += float(enabled) * 28.0;
    }
    if (marked > 0) {
        color = vec4(1.0);
    } else {
        color = vec4(sampled.rgb, sampled.a * uOpacity);
    }
}
""",
    ),
    (
        "DisplayableTexture", 0x7fa54, 8288,
        "f26d1ad280c3bf938dd159dee4fe333aa8da002b4626cac5812e7334eb97871d",
        b"""
attribute vec2 position;
uniform vec4 uSize, uSource, uTarget;
varying highp vec2 texout;
void main() {
    vec2 destination = (position * uTarget.zw + uTarget.xy) / uSize.zw;
    texout = (position * uSource.zw + uSource.xy) / uSize.xy;
    gl_Position = vec4(destination * vec2(2.0, -2.0) + vec2(-1.0, 1.0),
                       0.0, 1.0);
}
""",
        b"""#extension GL_OES_EGL_image_external : require
precision highp float;
uniform samplerExternalOES uTex;
uniform float uOpacity;
varying highp vec2 texout;
void main() {
    gl_FragColor = texture2D(uTex, texout) * uOpacity;
}
""",
    ),
    (
        "DisplayableFill", 0x84984, 7703,
        "6e98ac312ba3de2fc6ebd9895b212e3b0b1116791e846cd56892106de11156e3",
        b"""
attribute vec2 position;
uniform vec2 uStart, uEnd, uDisplaySize;
void main() {
    vec2 destination = (position * (uEnd - uStart) + uStart) / uDisplaySize;
    gl_Position = vec4(destination * vec2(2.0, -2.0) + vec2(-1.0, 1.0),
                       0.0, 1.0);
}
""",
        b"""
precision highp float;
uniform vec4 uColor;
void main() {
    gl_FragColor = uColor;
}
""",
    ),
    (
        "DisplayableOpaqueTexture", 0x88084, 7335,
        "9b642968b40cb13087eb5c7b4f6d4e798996b70a801e58f78cdc03d3be4a349f",
        b"""
attribute vec2 attrVertex, attrTex;
varying highp vec2 var_tex;
void main() {
    var_tex = attrTex;
    gl_Position = vec4(attrVertex, 0.0, 1.0);
}
""",
        b"""
precision highp float;
uniform sampler2D texture1;
varying highp vec2 var_tex;
void main() {
    gl_FragColor = vec4(texture2D(texture1, var_tex).rgb, 1.0);
}
""",
    ),
)


ANIMATION_PROGRAMS = (
    (
        "AnimationMaskedTexture", 0x171bc, 10117,
        "e5885a40c5400dee91a04a92d036fc77201b785c44567bcb2249b148eea2692a",
        b"""
attribute vec2 position;
uniform vec4 uSource, uTarget;
uniform vec2 uResolution, maskOffset;
uniform float uAngle, maskAngle;
varying highp vec2 texout, maskout;
void main() {
    vec2 center = uTarget.xy / uResolution * 2.0 - 1.0;
    // The native program adjusts X for odd height and Y for odd width.
    if (uTarget.w * 0.5 > floor(uTarget.w * 0.5))
        center.x -= 1.0 / uResolution.x;
    if (uTarget.z * 0.5 > floor(uTarget.z * 0.5))
        center.y -= 1.0 / uResolution.y;
    vec2 rotated = vec2(position.x * cos(uAngle) - position.y * sin(uAngle),
                        position.x * sin(uAngle) + position.y * cos(uAngle));
    gl_Position = vec4(rotated * uTarget.zw / uResolution + center, 0.0, 1.0);
    texout = (position * 0.5 + 0.5) * uSource.zw + uSource.xy;
    vec2 delta = texout - maskOffset / uResolution;
    maskout = vec2(delta.x * cos(maskAngle) - delta.y * sin(maskAngle),
                   delta.x * sin(maskAngle) + delta.y * cos(maskAngle));
}
""",
        b"""
precision highp float;
uniform sampler2D tex, mask;
uniform float opacity;
varying highp vec2 texout, maskout;
void main() {
    vec4 sampled = texture2D(tex, texout);
    gl_FragColor = vec4(sampled.rgb,
                       (sampled.a - texture2D(mask, maskout).r) * opacity);
}
""",
    ),
)


def _verified_programs(data, expected, entries, description):
    if hashlib.sha256(data).hexdigest() != expected:
        raise ValueError(f"Unverified {description} version")
    for name, offset, length, digest, vertex, fragment in entries:
        binary = data[offset:offset + length]
        if hashlib.sha256(binary).hexdigest() != digest:
            raise ValueError(f"Compositor binary identity mismatch: {name}")
        yield name, binary, [vertex, fragment]


def compositor_programs(data):
    yield from _verified_programs(
        data, DISPLAYMANAGER_SHA256, PROGRAMS, "DisplayManagement executable")


def animation_programs(data):
    yield from _verified_programs(
        data, ANIMATION_SHA256, ANIMATION_PROGRAMS, "DisplayManagement animation library")
