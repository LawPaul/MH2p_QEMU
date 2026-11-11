"""Exact-identity NVvp5/NVfp5 transcriptions of P2865 EAL materials."""
import hashlib


_VERTEX = b"""
attribute vec3 kzPosition;
attribute vec2 kzTextureCoordinate0;
uniform mat4 kzProjectionCameraWorldMatrix;
varying highp vec2 vTexCoord;
"""
_POSITION = b"gl_Position = kzProjectionCameraWorldMatrix * vec4(kzPosition, 1.0);"


def _vertex(uniforms, coordinates, color):
    return (_VERTEX + b"""
uniform vec4 ealModulateColor;
varying highp vec4 vModulate;
""" + uniforms + b"\nvoid main() {\n" + _POSITION +
            b"\nvTexCoord = " + coordinates + b";\nvModulate = " + color + b";\n}\n")


def _fragment(sampler, *, external=False, alpha_only=False):
    declaration = b"samplerExternalOES" if external else b"sampler2D"
    extension = b"#extension GL_OES_EGL_image_external : require\n" if external else b""
    result = (b"vec4(vModulate.rgb, vModulate.a * sampled.a)" if alpha_only else
              b"sampled * vModulate")
    return (extension + b"precision highp float;\nuniform " + declaration + b" " +
            sampler + b";\nvarying highp vec2 vTexCoord;\nvarying highp vec4 vModulate;\n"
            b"void main() {\nvec4 sampled = texture2D(" + sampler + b", vTexCoord);\n"
            b"gl_FragColor = " + result + b";\n}\n")


_TEXTURED = _vertex(
    b"uniform float BlendIntensity;", b"kzTextureCoordinate0",
    b"vec4(ealModulateColor.rgb, ealModulateColor.a * BlendIntensity)")
_LAYER = _vertex(
    b"uniform vec4 ealAtlasCoords;\nuniform float LayerRenderOpacity;",
    b"kzTextureCoordinate0 * ealAtlasCoords.zw + ealAtlasCoords.xy",
    b"vec4(ealModulateColor.rgb, ealModulateColor.a * LayerRenderOpacity)")
_FAST_LAYER = _vertex(
    b"uniform vec4 ealAtlasCoords;",
    b"kzTextureCoordinate0 * ealAtlasCoords.zw + ealAtlasCoords.xy",
    b"ealModulateColor")
_PREMULTIPLIED = _vertex(
    b"uniform float BlendIntensity;", b"kzTextureCoordinate0",
    b"vec4(ealModulateColor.rgb * ealModulateColor.a * BlendIntensity, "
    b"ealModulateColor.a * BlendIntensity)")
_FONT_VERTEX = (_VERTEX + b"void main() {\n" + _POSITION +
                b"\nvTexCoord = kzTextureCoordinate0;\n}\n")
_FONT_FRAGMENT = b"""
precision highp float;
uniform sampler2D LayerRenderTexture;
uniform float BlendIntensity;
uniform vec4 ealFontColor;
varying highp vec2 vTexCoord;
void main() {
    float coverage = texture2D(LayerRenderTexture, vTexCoord).r
                     * ealFontColor.a * BlendIntensity;
    gl_FragColor = vec4(ealFontColor.rgb * coverage, coverage);
}
"""
_MULTICOLOR_VERTEX = (_VERTEX + b"""
attribute vec4 kzColor0;
uniform vec4 ealFontColor;
varying highp vec4 vGlyphColor;
void main() {
""" + _POSITION + b"""
    vTexCoord = kzTextureCoordinate0;
    vGlyphColor = kzColor0 * ealFontColor;
}
""")
_MULTICOLOR_FRAGMENT = b"""
precision highp float;
uniform sampler2D LayerRenderTexture;
uniform float BlendIntensity;
varying highp vec2 vTexCoord;
varying highp vec4 vGlyphColor;
void main() {
    float coverage = texture2D(LayerRenderTexture, vTexCoord).r
                     * vGlyphColor.a * BlendIntensity;
    gl_FragColor = vec4(vGlyphColor.rgb * coverage, coverage);
}
"""
_SIMPLE_VERTEX = b"""
attribute vec2 kzPosition;
uniform mat4 kzWorldMatrix;
void main() {
    gl_Position = kzWorldMatrix * vec4(kzPosition, 0.0, 1.0);
}
"""
_QUICK_VERTEX = b"""
attribute vec2 kzPosition;
attribute vec4 kzColor0;
uniform mat4 kzWorldMatrix;
varying highp vec4 varColor;
void main() {
    gl_Position = kzWorldMatrix * vec4(kzPosition, 0.0, 1.0);
    varColor = kzColor0;
}
"""

# Each name, stage constant slot and attribute type was checked against native
# reflection. A layer named "premultiplied" has the same instructions as its
# ordinary variant; only the textured variant premultiplies modulation RGB.
PROGRAMS = {
    "eal_font": (
        "98b7cb0c079bbb96bf4c38e7ac1257af80f20696c0f3c117f4565de4ebc9d43e",
        _FONT_VERTEX, _FONT_FRAGMENT),
    "eal_font_multicolor": (
        "969b3cbc1a802555508170c8583e1996bba8b11ff95d6ef731238fae8f4b28e3",
        _MULTICOLOR_VERTEX, _MULTICOLOR_FRAGMENT),
    "eal_textured": (
        "67b8985d364220f992ad32e3e6c775fb0a444744b59f80c6de677d6992bb5d1c",
        _TEXTURED, _fragment(b"Texture")),
    "eal_textured_multi": (
        "84bdf8fed700e588cd55db30d62582708a3521a32e9d8a96d25bfa96af5e997f",
        _TEXTURED, _fragment(b"LayerRenderTexture")),
    "eal_textured_alpha": (
        "2e6ef894fb36bc3cbdf266bca1539f08c9c30ff40a9dd8f21db47870d5bbc239",
        _TEXTURED, _fragment(b"Texture", alpha_only=True)),
    "eal_textured_premultiplied": (
        "1376089fc39f09bc81281c5bbaa4d35799831ab349fe6acf91b32fd47d63e3e3",
        _PREMULTIPLIED, _fragment(b"Texture")),
    "eal_textured_oes": (
        "811aa546aac6bc73034335f7410b22bf8d914fca64bca006c8fdde74dc76ab43",
        _TEXTURED, _fragment(b"Texture", external=True)),
    "eal_layer_texture": (
        "3d7dad9fedbb563422952ed795dc306050c7b0171940d36e5e635079d3d73e5c",
        _LAYER, _fragment(b"LayerRenderTexture")),
    "eal_layer_texture_alpha": (
        "0a28811b3b4186028cb50d7e2d3dfdccd9b2f635f6b06a28155b23026b3e52b7",
        _LAYER, _fragment(b"LayerRenderTexture", alpha_only=True)),
    "eal_layer_texture_premultiplied": (
        "7b9810f9f98cfc8d713777f66e8e586c81037251360060be15514ecbb9f678f4",
        _LAYER, _fragment(b"LayerRenderTexture")),
    "eal_layer_texture_fast": (
        "7db79f764c33829c26d8936ab0d3ea6e68b2061de65d1d45692e29fcf7f93c6d",
        _FAST_LAYER, _fragment(b"LayerRenderTexture")),
    "eal_layer_texture_fast_oes": (
        "97007634922711e522f3d5da20d5169aca33ce95f8615213e3c99c6b49b1c579",
        _FAST_LAYER, _fragment(b"LayerRenderTexture", external=True)),
    "eal_simple": (
        "37a054ce034ef1766f6f7a4137541e3b1a009d4f8ae0c28cf478e13af67bd6d0",
        _SIMPLE_VERTEX,
        b"precision highp float;\nvoid main() { gl_FragColor = vec4(1.0); }\n"),
    "eal_quicknode_ext": (
        "5db40fd5c0304f58a3863b57c73922cad6567c9dc27b5b0095cfaae9b068d68c",
        _QUICK_VERTEX,
        b"precision highp float;\nvarying highp vec4 varColor;\n"
        b"void main() { gl_FragColor = varColor; }\n"),
}


def eal_program(name, binary):
    if name not in PROGRAMS:
        raise ValueError(f"Unverified EAL material: {name}")
    digest, vertex, fragment = PROGRAMS[name]
    if hashlib.sha256(binary).hexdigest() != digest:
        raise ValueError(f"EAL binary identity mismatch: {name}")
    return [vertex, fragment]
