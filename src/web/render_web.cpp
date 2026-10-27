// WebGL 2: the image shader and the frame for the browser build, following render.cpp pass for pass. The shader
// is the HLSL one in GLSL ES 3.00, reading the same constants from a uniform block.

#include "viewer.h"

#include <GLES3/gl3.h>
#include <emscripten/html5.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <new>

#include "imgui.h"
#include "backends/imgui_impl_opengl3.h"
#include "ui.h"
#include "view_constants.h"

namespace htv {

GpuView::~GpuView() {
    if (texture) glDeleteTextures(1, &texture);
}

namespace {

// Compressed formats from the WebGL extensions; the GLES 3 headers name only ETC2.
constexpr GLenum kRgbaS3tcDxt1 = 0x83F1, kRgbaS3tcDxt3 = 0x83F2, kRgbaS3tcDxt5 = 0x83F3;
constexpr GLenum kSrgbAlphaS3tcDxt1 = 0x8C4D, kSrgbAlphaS3tcDxt3 = 0x8C4E, kSrgbAlphaS3tcDxt5 = 0x8C4F;
constexpr GLenum kRedRgtc1 = 0x8DBB, kSignedRedRgtc1 = 0x8DBC, kRedGreenRgtc2 = 0x8DBD, kSignedRedGreenRgtc2 = 0x8DBE;
constexpr GLenum kBptcUnorm = 0x8E8C, kBptcSrgb = 0x8E8D, kBptcSignedFloat = 0x8E8E, kBptcUnsignedFloat = 0x8E8F;

const char kVertex[] = R"(#version 300 es
void main() {
    vec2 uv = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    gl_Position = vec4(uv * vec2(2.0, -2.0) + vec2(-1.0, 1.0), 0.0, 1.0);
}
)";

// Everything up to the entry points; each program adds its main().
const char kCommon[] = R"(#version 300 es
precision highp float;
precision highp int;
precision highp sampler2D;

layout(std140) uniform View {
    vec2 imageSize;
    vec2 imageOrigin;
    float zoom;
    float exposureScale;
    uint channelMode;
    uint channels;
    uint transfer;
    uint signedValues;
    uint rotation;
    uint flips;
    float tiles;
    uint reserved;
    int forcedMip;
    float invGamma;
    float checkerSize;
    float blackPoint;
    float rangeScale;
    uint toneMap;
    uint clipping;
    float zebraWidth;
    float zebraPhase;
    float padding;
    vec4 checker1;
    vec4 checker2;
    vec4 background;
    vec4 matchTarget;
    uint matchMode;
    float matchTolerance;
    uint matchSrgb;
    uint matchOutline;
    vec2 image2Size;
    vec2 scale2;
    uint channels2;
    uint srgb1;
    uint srgb2;
    float diffGain;
    vec2 tileOrigin;
    vec2 tileSize;
    vec2 tile2Origin;
    vec2 tile2Size;
    uint firstPass;
    float padding2a, padding2b, padding2c;
};
uniform sampler2D tex;
uniform sampler2D mask;
uniform sampler2D tex2;
uniform float flipY;  // the target's height when it is the canvas, whose rows run bottom to top; 0 otherwise
out vec4 fragColor;

// Direct3D's SV_Position: pixel centres from the top-left corner.
vec2 fragPos() { return vec2(gl_FragCoord.x, flipY > 0.0 ? flipY - gl_FragCoord.y : gl_FragCoord.y); }

vec3 srgbEncode(vec3 c) {
    c = clamp(c, 0.0, 1.0);
    vec3 lo = c * 12.92;
    vec3 hi = 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055;
    return mix(hi, lo, lessThanEqual(c, vec3(0.0031308)));
}

vec3 aces(vec3 x) {
    x *= 0.6;
    return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
}

// HLSL's mul(v, M) with M written row by row is M * v here with the same numbers, column by column.
vec3 agx(vec3 v) {
    const mat3 inset = mat3(0.842479062253094, 0.0423282422610123, 0.0423756549057051,
                            0.0784335999999992, 0.878468636469772, 0.0784336,
                            0.0792237451477643, 0.0791661274605434, 0.879142973793104);
    const mat3 outset = mat3(1.19687900512017, -0.0528968517574562, -0.0529716355144438,
                             -0.0980208811401368, 1.15190312990417, -0.0980434501171241,
                             -0.0990297440797205, -0.0989611768448433, 1.15107367264116);
    const float minEv = -12.47393, maxEv = 4.026069;
    v = inset * max(v, vec3(1e-10));
    v = (clamp(log2(v), minEv, maxEv) - minEv) / (maxEv - minEv);
    vec3 v2 = v * v, v4 = v2 * v2;
    v = 15.5 * v4 * v2 - 40.14 * v4 * v + 31.96 * v4 - 6.868 * v2 * v + 0.4298 * v2 + 0.1191 * v - 0.00232;
    v = outset * v;
    return pow(clamp(v, 0.0, 1.0), vec3(2.2));
}

bool imagePos(vec2 pos, out vec2 ip) {
    vec2 shownSize = (rotation & 1u) != 0u ? imageSize.yx : imageSize;
    vec2 d = (pos - imageOrigin) / zoom;
    ip = vec2(0.0);
    if (d.x < 0.0 || d.y < 0.0 || d.x >= shownSize.x * tiles || d.y >= shownSize.y * tiles) return false;
    d = mod(d, shownSize);
    if ((flips & 1u) != 0u) d.x = shownSize.x - d.x;
    if ((flips & 2u) != 0u) d.y = shownSize.y - d.y;
    ip = d;
    if (rotation == 1u) ip = vec2(d.y, shownSize.x - d.x);
    else if (rotation == 2u) ip = imageSize - d;
    else if (rotation == 3u) ip = vec2(shownSize.y - d.y, d.x);
    return true;
}

bool inTile(ivec2 texel, vec2 origin, vec2 size) {
    ivec2 t = texel - ivec2(origin);
    return all(greaterThanEqual(t, ivec2(0))) && all(lessThan(t, ivec2(size)));
}

ivec2 texelAt(vec2 ip, vec2 size) { return ivec2(clamp(floor(ip), vec2(0.0), size - 1.0)); }

bool matchedAt(vec2 pos) {
    vec2 ip;
    if (!imagePos(pos, ip)) return false;
    ivec2 texel = texelAt(ip, imageSize) - ivec2(tileOrigin);
    return texelFetch(mask, clamp(texel, ivec2(0), ivec2(tileSize) - 1), 0).r > 0.5;
}

bool badValue(vec4 c) { return any(isnan(c)) || any(isinf(c)) || any(greaterThan(abs(c), vec4(3.402823e38))); }

vec2 hueSaturation(vec3 c) {
    float mx = max(max(c.r, c.g), c.b), mn = min(min(c.r, c.g), c.b), d = mx - mn;
    float hue = 0.0;
    if (d > 0.0) {
        if (mx == c.r) hue = (c.g - c.b) / d;
        else if (mx == c.g) hue = 2.0 + (c.b - c.r) / d;
        else hue = 4.0 + (c.r - c.g) / d;
        hue = fract(hue / 6.0 + 1.0);
    }
    return vec2(hue, mx > 0.0 ? d / mx : 0.0);
}

vec4 storedValue(vec4 c, uint ch, uint srgb) {
    if (srgb != 0u) c.rgb = srgbEncode(c.rgb);
    if (ch == 1u) c = vec4(c.rrr, 1.0);
    else if (ch == 2u) c = vec4(c.rg, 0.0, 1.0);
    return c;
}

vec3 heat(float t) { return clamp(vec3(t * 3.0, t * 3.0 - 1.0, t * 3.0 - 2.0), 0.0, 1.0); }
)";

const char kMain[] = R"(
void main() {
    vec2 pos = fragPos();
    vec2 ip;
    if (!imagePos(pos, ip)) {
        if (firstPass != 3u) discard;
        fragColor = background;
        return;
    }
    ivec2 texel = texelAt(ip, imageSize);
    if (!inTile(texel, tileOrigin, tileSize)) discard;
    ivec2 local = texel - ivec2(tileOrigin);

    if (matchOutline != 0u && matchedAt(pos) &&
        (!matchedAt(pos + vec2(1, 0)) || !matchedAt(pos - vec2(1, 0)) ||
         !matchedAt(pos + vec2(0, 1)) || !matchedAt(pos - vec2(0, 1)))) {
        int dash = int(floor((pos.x + pos.y) / max(zebraWidth, 1.0)));
        fragColor = (dash & 1) != 0 ? vec4(1) : vec4(0, 0, 0, 1);
        return;
    }

    vec4 c;
    if (forcedMip >= 0) c = texelFetch(tex, local >> forcedMip, forcedMip);
    else if (zoom >= 1.0) c = texelFetch(tex, local, 0);
    else c = textureLod(tex, (ip - tileOrigin) / tileSize, log2(1.0 / zoom));
    if (signedValues != 0u) c.rgb = c.rgb * 0.5 + 0.5;
    if (channels == 1u) c = vec4(c.rrr, 1.0);
    else if (channels == 2u) c = vec4(c.rg, 0.0, 1.0);

    if (badValue(c)) {
        fragColor = clipping != 0u ? vec4(1, 0, 1, 1) : vec4(0, 0, 0, 1);
        return;
    }

    vec3 rgb = c.rgb;
    float a = c.a;
    if (channelMode == 1u) rgb = c.rrr;
    else if (channelMode == 2u) rgb = c.ggg;
    else if (channelMode == 3u) rgb = c.bbb;
    else if (channelMode == 4u) rgb = c.aaa;
    else if (channelMode == 6u) rgb = vec3(dot(c.rgb, vec3(0.2126, 0.7152, 0.0722)));
    if (channelMode != 0u) a = 1.0;

    bool clipped = false;
    if (channelMode != 4u) {
        rgb = (rgb - blackPoint) * rangeScale * exposureScale;
        clipped = clipping != 0u && any(greaterThan(rgb, vec3(1.0)));
        rgb = max(rgb, vec3(0.0));
        if (toneMap == 1u) rgb = rgb / (1.0 + rgb);
        else if (toneMap == 2u) rgb = aces(rgb);
        else if (toneMap == 3u) rgb = agx(rgb);
    }
    if (transfer >= 1u) rgb = srgbEncode(rgb);
    if (transfer >= 2u) rgb = srgbEncode(rgb);
    rgb = pow(clamp(rgb, 0.0, 1.0), vec3(invGamma));
    if (clipped) {
        int stripe = int(floor((pos.x + pos.y + zebraPhase) / max(zebraWidth, 1.0)));
        rgb = (stripe & 1) != 0 ? vec3(1) : vec3(0);
    }

    vec2 cell = floor((pos - imageOrigin) / checkerSize);
    vec3 under = mod(cell.x + cell.y, 2.0) < 1.0 ? checker1.rgb : checker2.rgb;
    fragColor = vec4(mix(under, rgb, clamp(a, 0.0, 1.0)), 1.0);
}
)";

const char kMatch[] = R"(
void main() {
    vec4 c = texelFetch(tex, ivec2(fragPos()), 0);
    if (matchSrgb != 0u) c.rgb = srgbEncode(c.rgb);
    if (channels == 1u) c = vec4(c.rrr, 1.0);
    else if (channels == 2u) c = vec4(c.rg, 0.0, 1.0);
    vec4 t = matchTarget;
    float d;
    if (matchMode == 0u) d = max(max(abs(c.r - t.r), abs(c.g - t.g)), abs(c.b - t.b));
    else if (matchMode == 1u) d = max(max(abs(c.r - t.r), abs(c.g - t.g)), max(abs(c.b - t.b), abs(c.a - t.a)));
    else if (matchMode <= 5u)
        d = abs(dot(c - t, vec4(matchMode == 2u, matchMode == 3u, matchMode == 4u, matchMode == 5u)));
    else if (matchMode == 6u) d = abs(dot(c.rgb - t.rgb, vec3(0.2126, 0.7152, 0.0722)));
    else {
        vec2 hc = hueSaturation(c.rgb), ht = hueSaturation(t.rgb);
        float dh = abs(hc.x - ht.x);
        d = matchMode == 7u ? min(dh, 1.0 - dh) : abs(hc.y - ht.y);
    }
    fragColor = vec4(d <= matchTolerance ? 1.0 : 0.0);
}
)";

const char kDiff[] = R"(
void main() {
    vec2 pos = fragPos();
    vec2 ip;
    if (!imagePos(pos, ip)) {
        if (firstPass != 3u) discard;
        fragColor = background;
        return;
    }
    ivec2 texel = texelAt(ip, imageSize);
    if (!inTile(texel, tileOrigin, tileSize)) discard;
    vec2 ip2 = ip * scale2;
    if (ip2.x >= image2Size.x || ip2.y >= image2Size.y) {
        if ((firstPass & 2u) == 0u) discard;
        fragColor = background;
        return;
    }
    ivec2 texel2 = texelAt(ip2, image2Size);
    if (!inTile(texel2, tile2Origin, tile2Size)) discard;
    vec4 a, b;
    if (zoom >= 1.0) {
        a = texelFetch(tex, texel - ivec2(tileOrigin), 0);
        b = texelFetch(tex2, texel2 - ivec2(tile2Origin), 0);
    } else {
        a = textureLod(tex, (ip - tileOrigin) / tileSize, log2(1.0 / zoom));
        b = textureLod(tex2, (ip2 - tile2Origin) / tile2Size, log2(scale2.x / zoom));
    }
    vec4 sa = storedValue(a, channels, srgb1), sb = storedValue(b, channels2, srgb2);
    vec4 d = abs(sa - sb);
    float v = max(max(d.r, d.g), d.b);
    if (channelMode >= 1u && channelMode <= 4u)
        v = dot(d, vec4(channelMode == 1u, channelMode == 2u, channelMode == 3u, channelMode == 4u));
    else if (channelMode == 6u) v = abs(dot(sa.rgb - sb.rgb, vec3(0.2126, 0.7152, 0.0722)));
    fragColor = isnan(v) ? vec4(1, 0, 1, 1) : vec4(heat(v * diffGain), 1.0);
}
)";

const char kDiffRead[] = R"(
void main() {
    ivec2 p = clamp(ivec2(imageOrigin) + ivec2(fragPos()), ivec2(0), ivec2(imageSize) - 1);
    if (!inTile(p, tileOrigin, tileSize)) discard;
    vec2 p2 = floor((vec2(p) + 0.5) * scale2);
    if (p2.x >= image2Size.x || p2.y >= image2Size.y) {
        if ((firstPass & 2u) == 0u) discard;
        fragColor = vec4(-1);
        return;
    }
    if (!inTile(ivec2(p2), tile2Origin, tile2Size)) discard;
    vec4 a = storedValue(texelFetch(tex, p - ivec2(tileOrigin), 0), channels, srgb1);
    vec4 b = storedValue(texelFetch(tex2, ivec2(p2) - ivec2(tile2Origin), 0), channels2, srgb2);
    fragColor = abs(a - b);
}
)";

const char kRead[] = R"(
void main() {
    ivec2 p = clamp(ivec2(imageOrigin) + ivec2(fragPos()), ivec2(0), ivec2(imageSize) - 1);
    if (!inTile(p, tileOrigin, tileSize)) discard;
    fragColor = texelFetch(tex, p - ivec2(tileOrigin), 0);
}
)";

struct Program {
    GLuint id = 0;
    GLint flipY = -1;
};

struct Gl {
    Program main, read, match, diff, diffRead;
    GLuint ubo = 0, vao = 0, sampler = 0;
    GLint maxTexture = 4096;
    bool floatLinear = false;   // 32-bit float textures filter (OES_texture_float_linear)
    bool floatTargets = false;  // and float formats can be drawn into (EXT_color_buffer_float)
    bool s3tc = false, s3tcSrgb = false, rgtc = false, bptc = false;
} gl;

GLuint compile(GLenum type, const std::string& source, std::string& error) {
    GLuint s = glCreateShader(type);
    const char* text = source.c_str();
    glShaderSource(s, 1, &text, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048] = {};
        glGetShaderInfoLog(s, sizeof log, nullptr, log);
        error = log;
        glDeleteShader(s);
        return 0;
    }
    return s;
}

bool link(Program& p, const char* entry, std::string& error) {
    static GLuint vs = 0;
    if (!vs && !(vs = compile(GL_VERTEX_SHADER, kVertex, error))) return false;
    GLuint fs = compile(GL_FRAGMENT_SHADER, std::string(kCommon) + entry, error);
    if (!fs) return false;
    p.id = glCreateProgram();
    glAttachShader(p.id, vs);
    glAttachShader(p.id, fs);
    glLinkProgram(p.id);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(p.id, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048] = {};
        glGetProgramInfoLog(p.id, sizeof log, nullptr, log);
        error = log;
        return false;
    }
    glUniformBlockBinding(p.id, glGetUniformBlockIndex(p.id, "View"), 0);
    glUseProgram(p.id);
    glUniform1i(glGetUniformLocation(p.id, "tex"), 0);
    glUniform1i(glGetUniformLocation(p.id, "mask"), 1);
    glUniform1i(glGetUniformLocation(p.id, "tex2"), 2);
    p.flipY = glGetUniformLocation(p.id, "flipY");
    return true;
}

// A render target: the canvas (fbo 0) or a texture with its framebuffer.
struct Target {
    GLuint fbo = 0;
    int width = 0, height = 0;
    bool canvas = false;
};

// One full-target pass of `p` with these textures in units 0, 1 and 2; the scissor box (in top-down pixels)
// clips it, as Direct3D's viewport does.
void drawPass(const Target& target, int sx, int sy, int sw, int sh, const ViewConstants& c, const Program& p,
              GLuint t0, GLuint t1 = 0, GLuint t2 = 0) {
    glBindFramebuffer(GL_FRAMEBUFFER, target.fbo);
    glViewport(0, 0, target.width, target.height);
    glEnable(GL_SCISSOR_TEST);
    glScissor(sx, target.canvas ? target.height - sy - sh : sy, sw, sh);
    glUseProgram(p.id);
    glUniform1f(p.flipY, target.canvas ? float(target.height) : 0.0f);
    glBindBuffer(GL_UNIFORM_BUFFER, gl.ubo);
    glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof c, &c);
    glBindBufferBase(GL_UNIFORM_BUFFER, 0, gl.ubo);
    const GLuint units[3] = {t0, t1, t2};
    for (int i = 0; i < 3; ++i) {
        glActiveTexture(GL_TEXTURE0 + i);
        glBindTexture(GL_TEXTURE_2D, units[i]);
        glBindSampler(i, i == 0 || i == 2 ? gl.sampler : 0);
    }
    glBindVertexArray(gl.vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
    for (int i = 0; i < 3; ++i) glBindSampler(i, 0);
    glActiveTexture(GL_TEXTURE0);
    glDisable(GL_SCISSOR_TEST);
}

GLuint tex(const GpuTile& t) { return t.srv ? t.srv->texture : 0; }

// A pass for each tile (each pair of tiles with a second image), as render.cpp's drawImage.
void drawImage(const Target& target, int sx, int sy, int sw, int sh, const ViewConstants& c, const Program& p,
               const GpuImage* mask = nullptr, const GpuImage* image = nullptr, const GpuImage* second = nullptr) {
    const GpuImage& first = image ? *image : app.srv;
    const size_t secondTiles = second ? second->tiles.size() : 1;
    for (size_t i = 0; i < first.tiles.size(); ++i)
        for (size_t j = 0; j < secondTiles; ++j) {
            const GpuTile& t = first.tiles[i];
            ViewConstants pass = c;
            pass.tileOrigin[0] = float(t.x);
            pass.tileOrigin[1] = float(t.y);
            pass.tileSize[0] = float(t.width);
            pass.tileSize[1] = float(t.height);
            pass.firstPass = (i == 0 ? 1u : 0u) | (j == 0 ? 2u : 0u);
            GLuint t2 = 0;
            if (second) {
                const GpuTile& s = second->tiles[j];
                pass.tile2Origin[0] = float(s.x);
                pass.tile2Origin[1] = float(s.y);
                pass.tile2Size[0] = float(s.width);
                pass.tile2Size[1] = float(s.height);
                t2 = tex(s);
            }
            drawPass(target, sx, sy, sw, sh, pass, p, tex(t), mask && i < mask->tiles.size() ? tex(mask->tiles[i]) : 0, t2);
        }
}

// A texture to draw into and read back.
struct Readback {
    GLuint texture = 0, fbo = 0;
    int width = 0, height = 0;
    bool floats = false;

    ~Readback() { release(); }
    void release() {
        if (fbo) glDeleteFramebuffers(1, &fbo);
        if (texture) glDeleteTextures(1, &texture);
        fbo = texture = 0;
        width = height = 0;
    }
    bool create(int w, int h, bool floatTarget) {
        if (width == w && height == h && floats == floatTarget && fbo) return true;
        release();
        if (floatTarget && !gl.floatTargets) return false;
        glGenTextures(1, &texture);
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexStorage2D(GL_TEXTURE_2D, 1, floatTarget ? GL_RGBA32F : GL_RGBA8, w, h);
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
        const bool ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        if (!ok) {
            release();
            return false;
        }
        width = w;
        height = h;
        floats = floatTarget;
        return true;
    }
    Target target() const { return {fbo, width, height, false}; }
    void clear() {
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        const float zero[4] = {};
        glClearBufferfv(GL_COLOR, 0, zero);
    }
    // Rows from the top, as Direct3D reads them: the shader drew them from the bottom of the texture up.
    void read(void* out) {
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, width, height, GL_RGBA, floats ? GL_FLOAT : GL_UNSIGNED_BYTE, out);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }
};

// As render.cpp's readRows: strips of whole rows, column blocks for wide images.
template <class Draw, class Row>
bool readRows(int width, int height, Draw draw, Row row) {
    const int block = std::min(width, int(gl.maxTexture));
    const int strip = std::clamp((1 << 20) / std::max(width, 1), 1, height);
    Readback rb;
    if (!rb.create(block, strip, true)) return false;
    std::vector<float> pixels(size_t(block) * strip * 4);
    std::vector<float> rows(block < width ? size_t(width) * strip * 4 : 0);
    for (int y0 = 0; y0 < height; y0 += strip) {
        const int n = std::min(strip, height - y0);
        for (int x0 = 0; x0 < width; x0 += block) {
            const int columns = std::min(block, width - x0);
            rb.clear();
            draw(rb.target(), x0, y0, block, strip);
            rb.read(pixels.data());
            for (int y = 0; y < n; ++y) {
                const float* p = &pixels[size_t(y) * block * 4];
                if (rows.empty()) row(p, width);
                else std::memcpy(&rows[(size_t(y) * width + x0) * 4], p, size_t(columns) * 16);
            }
        }
        if (!rows.empty())
            for (int y = 0; y < n; ++y) row(&rows[size_t(y) * width * 4], width);
    }
    return true;
}

// ---- textures ----

struct GlFormat {
    GLenum internal = 0, format = 0, type = 0;
    bool compressed = false;
    bool mipmappable = false;  // glGenerateMipmap works on it
    // The pixels need converting first: BGRA to RGBA, 16-bit integers to floats, 32-bit floats to halves.
    enum class Convert { None, Bgra, BgrX, Unorm16, FloatToHalf } convert = Convert::None;
    int channels = 4;
};

bool glFormat(htv::GpuFormat f, GlFormat& out, std::string& error) {
    using G = htv::GpuFormat;
    using C = GlFormat::Convert;
    const bool floatFilter = gl.floatLinear && gl.floatTargets;
    auto plain = [&](GLenum internal, GLenum format, GLenum type, bool mips, int channels) {
        out = {internal, format, type, false, mips, C::None, channels};
        return true;
    };
    auto block = [&](bool available, GLenum internal, const char* extension) {
        if (!available) {
            error = std::string(htv::formatInfo(f).name) + " needs a browser with " + extension +
                    " (Chrome or Edge on a desktop)";
            return false;
        }
        out = {internal, 0, 0, true, false, C::None, 4};
        return true;
    };
    switch (f) {
        case G::RGBA8: return plain(GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, true, 4);
        case G::RGBA8Srgb: return plain(GL_SRGB8_ALPHA8, GL_RGBA, GL_UNSIGNED_BYTE, true, 4);
        case G::BGRA8: plain(GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, true, 4); out.convert = C::Bgra; return true;
        case G::BGRX8: plain(GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, true, 4); out.convert = C::BgrX; return true;
        case G::BGRA8Srgb: plain(GL_SRGB8_ALPHA8, GL_RGBA, GL_UNSIGNED_BYTE, true, 4); out.convert = C::Bgra; return true;
        case G::RG8: return plain(GL_RG8, GL_RG, GL_UNSIGNED_BYTE, true, 2);
        case G::R8: return plain(GL_R8, GL_RED, GL_UNSIGNED_BYTE, true, 1);
        case G::RGB10A2: return plain(GL_RGB10_A2, GL_RGBA, GL_UNSIGNED_INT_2_10_10_10_REV, true, 4);
        case G::RG11B10F:
            return plain(GL_R11F_G11F_B10F, GL_RGB, GL_UNSIGNED_INT_10F_11F_11F_REV, gl.floatTargets, 3);
        case G::RGBA16F: return plain(GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT, gl.floatTargets, 4);
        case G::RG16F: return plain(GL_RG16F, GL_RG, GL_HALF_FLOAT, gl.floatTargets, 2);
        case G::R16F: return plain(GL_R16F, GL_RED, GL_HALF_FLOAT, gl.floatTargets, 1);
        // 32-bit floats filter only with an extension; without it they go up as halves.
        case G::RGBA32F:
            if (floatFilter) return plain(GL_RGBA32F, GL_RGBA, GL_FLOAT, true, 4);
            plain(GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT, gl.floatTargets, 4);
            out.convert = C::FloatToHalf;
            return true;
        case G::RG32F:
            if (floatFilter) return plain(GL_RG32F, GL_RG, GL_FLOAT, true, 2);
            plain(GL_RG16F, GL_RG, GL_HALF_FLOAT, gl.floatTargets, 2);
            out.convert = C::FloatToHalf;
            return true;
        case G::R32F:
            if (floatFilter) return plain(GL_R32F, GL_RED, GL_FLOAT, true, 1);
            plain(GL_R16F, GL_RED, GL_HALF_FLOAT, gl.floatTargets, 1);
            out.convert = C::FloatToHalf;
            return true;
        // WebGL has no 16-bit normalised formats: the values go up as floats, still in [0, 1].
        case G::RGBA16Unorm:
            plain(floatFilter ? GL_RGBA32F : GL_RGBA16F, GL_RGBA, floatFilter ? GL_FLOAT : GL_HALF_FLOAT,
                  gl.floatTargets, 4);
            out.convert = C::Unorm16;
            return true;
        case G::RG16Unorm:
            plain(floatFilter ? GL_RG32F : GL_RG16F, GL_RG, floatFilter ? GL_FLOAT : GL_HALF_FLOAT, gl.floatTargets, 2);
            out.convert = C::Unorm16;
            return true;
        case G::R16Unorm:
            plain(floatFilter ? GL_R32F : GL_R16F, GL_RED, floatFilter ? GL_FLOAT : GL_HALF_FLOAT, gl.floatTargets, 1);
            out.convert = C::Unorm16;
            return true;
        case G::BC1: return block(gl.s3tc, kRgbaS3tcDxt1, "S3TC");
        case G::BC1Srgb: return block(gl.s3tcSrgb, kSrgbAlphaS3tcDxt1, "sRGB S3TC");
        case G::BC2: return block(gl.s3tc, kRgbaS3tcDxt3, "S3TC");
        case G::BC2Srgb: return block(gl.s3tcSrgb, kSrgbAlphaS3tcDxt3, "sRGB S3TC");
        case G::BC3: return block(gl.s3tc, kRgbaS3tcDxt5, "S3TC");
        case G::BC3Srgb: return block(gl.s3tcSrgb, kSrgbAlphaS3tcDxt5, "sRGB S3TC");
        case G::BC4: return block(gl.rgtc, kRedRgtc1, "RGTC");
        case G::BC4Snorm: return block(gl.rgtc, kSignedRedRgtc1, "RGTC");
        case G::BC5: return block(gl.rgtc, kRedGreenRgtc2, "RGTC");
        case G::BC5Snorm: return block(gl.rgtc, kSignedRedGreenRgtc2, "RGTC");
        case G::BC6HUf16: return block(gl.bptc, kBptcUnsignedFloat, "BPTC");
        case G::BC6HSf16: return block(gl.bptc, kBptcSignedFloat, "BPTC");
        case G::BC7: return block(gl.bptc, kBptcUnorm, "BPTC");
        case G::BC7Srgb: return block(gl.bptc, kBptcSrgb, "BPTC");
        case G::Unknown: break;
    }
    error = "unknown texture format";
    return false;
}

// The rows of an area of a surface, tightly packed and converted for glTexImage2D.
std::vector<uint8_t> packRows(const htv::Surface& s, const htv::FormatInfo& info, const GlFormat& g, uint32_t x,
                              uint32_t y, uint32_t w, uint32_t h) {
    using C = GlFormat::Convert;
    std::vector<uint8_t> out;
    if (info.compressed) {
        const uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
        out.resize(size_t(bw) * bh * info.bytes);
        for (uint32_t r = 0; r < bh; ++r)
            std::memcpy(&out[size_t(r) * bw * info.bytes], &s.data[size_t(y / 4 + r) * s.rowPitch + size_t(x / 4) * info.bytes],
                        size_t(bw) * info.bytes);
        return out;
    }
    const size_t inBytes = info.bytes;
    const size_t outBytes = g.convert == C::Unorm16 ? (g.type == GL_FLOAT ? inBytes * 2 : inBytes)
                            : g.convert == C::FloatToHalf ? inBytes / 2
                                                          : inBytes;
    out.resize(size_t(w) * h * outBytes);
    for (uint32_t r = 0; r < h; ++r) {
        const uint8_t* src = &s.data[size_t(y + r) * s.rowPitch + size_t(x) * inBytes];
        uint8_t* dst = &out[size_t(r) * w * outBytes];
        switch (g.convert) {
            case C::None: std::memcpy(dst, src, size_t(w) * inBytes); break;
            case C::Bgra:
            case C::BgrX:
                for (uint32_t i = 0; i < w; ++i, src += 4, dst += 4) {
                    dst[0] = src[2];
                    dst[1] = src[1];
                    dst[2] = src[0];
                    dst[3] = g.convert == C::BgrX ? 255 : src[3];
                }
                break;
            case C::Unorm16:
                for (size_t i = 0; i < size_t(w) * inBytes / 2; ++i) {
                    uint16_t v;
                    std::memcpy(&v, src + 2 * i, 2);
                    const float f = float(v) / 65535.0f;
                    if (g.type == GL_FLOAT) std::memcpy(dst + 4 * i, &f, 4);
                    else {
                        const uint16_t half = htv::floatToHalf(f);
                        std::memcpy(dst + 2 * i, &half, 2);
                    }
                }
                break;
            case C::FloatToHalf:
                for (size_t i = 0; i < size_t(w) * inBytes / 4; ++i) {
                    float f;
                    std::memcpy(&f, src + 4 * i, 4);
                    const uint16_t half = htv::floatToHalf(f);
                    std::memcpy(dst + 2 * i, &half, 2);
                }
                break;
        }
    }
    return out;
}

TextureRef makeView(GLuint texture) {
    auto view = std::make_unique<GpuView>();
    view->texture = texture;
    return view;
}

void drawUi() {
    ImGui_ImplOpenGL3_NewFrame();
    ImGui::NewFrame();
    uiFrame();
    ImGui::Render();
}

}  // namespace

bool createPipeline(std::string& error) {
    gl.floatLinear = emscripten_webgl_enable_extension(emscripten_webgl_get_current_context(), "OES_texture_float_linear");
    gl.floatTargets = emscripten_webgl_enable_extension(emscripten_webgl_get_current_context(), "EXT_color_buffer_float");
    const auto ctx = emscripten_webgl_get_current_context();
    gl.s3tc = emscripten_webgl_enable_extension(ctx, "WEBGL_compressed_texture_s3tc");
    gl.s3tcSrgb = emscripten_webgl_enable_extension(ctx, "WEBGL_compressed_texture_s3tc_srgb");
    gl.rgtc = emscripten_webgl_enable_extension(ctx, "EXT_texture_compression_rgtc");
    gl.bptc = emscripten_webgl_enable_extension(ctx, "EXT_texture_compression_bptc");
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &gl.maxTexture);
    if (!link(gl.main, kMain, error) || !link(gl.read, kRead, error) || !link(gl.match, kMatch, error) ||
        !link(gl.diff, kDiff, error) || !link(gl.diffRead, kDiffRead, error))
        return false;
    glGenBuffers(1, &gl.ubo);
    glBindBuffer(GL_UNIFORM_BUFFER, gl.ubo);
    glBufferData(GL_UNIFORM_BUFFER, sizeof(ViewConstants), nullptr, GL_DYNAMIC_DRAW);
    glGenVertexArrays(1, &gl.vao);
    glGenSamplers(1, &gl.sampler);
    glSamplerParameteri(gl.sampler, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glSamplerParameteri(gl.sampler, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glSamplerParameteri(gl.sampler, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glSamplerParameteri(gl.sampler, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return true;
}

bool createImageTexture(const htv::Image& img, GpuImage& out, int& mips, std::string& error) {
    const htv::FormatInfo& info = htv::formatInfo(img.format);
    GlFormat g;
    if (!glFormat(img.format, g, error)) return false;
    if (info.compressed && (img.width % 4 || img.height % 4)) {
        error = "compressed textures must be a multiple of 4 px for now";
        return false;
    }
    // Tiles as in render.cpp, at most the browser's texture size and 256 MB each.
    const uint32_t maxSize = uint32_t(std::min(gl.maxTexture, 16384));
    const double bytesPerPixel = info.compressed ? info.bytes / 16.0 : double(info.bytes);
    constexpr double kTileBytes = 256.0 * 1024 * 1024;
    uint32_t tilesX = (img.width + maxSize - 1) / maxSize, tilesY = (img.height + maxSize - 1) / maxSize;
    uint32_t tileW = img.width, tileH = img.height;
    auto split = [](uint32_t size, uint32_t& count) {
        if (count <= 1) return size;
        const uint32_t tile = ((size + count - 1) / count + 255) / 256 * 256;
        count = (size + tile - 1) / tile;
        return tile;
    };
    for (;;) {
        tileW = split(img.width, tilesX);
        tileH = split(img.height, tilesY);
        if (double(tileW) * tileH * bytesPerPixel <= kTileBytes || (tileW <= 256 && tileH <= 256)) break;
        if (tileW >= tileH) ++tilesX;
        else ++tilesY;
    }
    const bool tiled = tilesX * tilesY > 1;
    const bool autoMips = (img.mips.size() == 1 || tiled) && (img.width > 1 || img.height > 1) && g.mipmappable;
    const size_t fileMips = tiled ? 1 : img.mips.size();

    GpuImage result;
    mips = 0;
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    for (uint32_t ty = 0; ty < tilesY; ++ty)
        for (uint32_t tx = 0; tx < tilesX; ++tx) {
            GpuTile tile;
            tile.x = tx * tileW;
            tile.y = ty * tileH;
            tile.width = std::min(tileW, img.width - tile.x);
            tile.height = std::min(tileH, img.height - tile.y);
            GLuint texture = 0;
            glGenTextures(1, &texture);
            tile.srv = makeView(texture);
            glBindTexture(GL_TEXTURE_2D, texture);
            int levels = int(fileMips);
            if (autoMips) {
                levels = 1;
                for (uint32_t s = std::max(tile.width, tile.height); s > 1; s >>= 1) ++levels;
            }
            while (glGetError() != GL_NO_ERROR) {
            }
            glTexStorage2D(GL_TEXTURE_2D, levels, g.internal, GLsizei(tile.width), GLsizei(tile.height));
            for (size_t i = 0; i < (autoMips ? 1 : fileMips); ++i) {
                const htv::Surface& s = img.mips[i];
                const uint32_t w = i == 0 ? tile.width : s.width, h = i == 0 ? tile.height : s.height;
                const std::vector<uint8_t> px = packRows(s, info, g, i == 0 ? tile.x : 0, i == 0 ? tile.y : 0, w, h);
                if (g.compressed)
                    glCompressedTexSubImage2D(GL_TEXTURE_2D, GLint(i), 0, 0, GLsizei(w), GLsizei(h), g.internal,
                                              GLsizei(px.size()), px.data());
                else
                    glTexSubImage2D(GL_TEXTURE_2D, GLint(i), 0, 0, GLsizei(w), GLsizei(h), g.format, g.type, px.data());
            }
            if (autoMips) glGenerateMipmap(GL_TEXTURE_2D);
            const GLenum err = glGetError();
            if (err != GL_NO_ERROR) {
                error = err == GL_OUT_OF_MEMORY ? "not enough video memory for this image" : "cannot create texture";
                return false;
            }
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, levels - 1);
            mips = mips ? std::min(mips, levels) : levels;
            result.tiles.push_back(std::move(tile));
        }
    out = std::move(result);
    return true;
}

bool uploadImage(const htv::Image& img, std::string& error) {
    GpuImage srv;
    int mips = 1;
    if (!createImageTexture(img, srv, mips, error)) return false;
    app.srv = std::move(srv);
    app.textureMips = mips;
    ++app.imageGeneration;
    return true;
}

bool uploadSurface(const htv::Surface& surface, htv::GpuFormat format, std::string& error) {
    htv::Image one;
    one.width = surface.width;
    one.height = surface.height;
    one.format = format;
    one.mips.push_back(surface);
    GpuImage srv;
    int mips = 1;
    if (!createImageTexture(one, srv, mips, error)) return false;
    app.srv = std::move(srv);
    app.textureMips = mips;
    ++app.imageGeneration;
    return true;
}

void releaseImage() {
    app.srv.reset();
    app.hasImage = false;
    app.image = htv::Image();
    app.stats = htv::ImageStats();
    app.pixelLocked = false;
    ++app.imageGeneration;
}

TextureRef createTexture(const uint8_t* rgba, int width, int height) {
    GLuint texture = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, width, height);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    return makeView(texture);
}

void resizeBuffers() {}
void queryDisplayPeak() {}

// ---- Compare ----

namespace {

struct CompareImage {
    const htv::Image* image = nullptr;
    const GpuImage* srv = nullptr;
    int generation = 0;
};

CompareImage layer(int i) {
    if (i == 0) return {&app.image, app.hasImage && app.srv ? &app.srv : nullptr, app.imageGeneration};
    const CompareSlot& s = app.compareSlots[i - 1];
    return {&s.image, s.srv ? &s.srv : nullptr, s.generation};
}

void layerView(const CompareImage& l, float& zoom, float& x0, float& y0) {
    const float w = float(app.rotation & 1 ? l.image->height : l.image->width);
    const float h = float(app.rotation & 1 ? l.image->width : l.image->height);
    float aw, ah;
    imageSizeOnScreen(aw, ah);
    zoom = app.zoom;
    if (l.image != &app.image && app.compareSameSize && w > 0) zoom = app.zoom * aw / w;
    const float around = app.tiling != Tiling::Off ? 1.0f : 0.0f;
    x0 = app.originX - around * w * zoom;
    y0 = app.originY - around * h * zoom;
}

ViewConstants layerConstants(int i, const UiColors& colors) {
    const CompareImage l = layer(i);
    float zoom, x0, y0;
    layerView(l, zoom, x0, y0);
    ViewConstants c = makeConstants(*l.image, x0, y0, zoom, app.tiling == Tiling::Off ? 1.0f : 3.0f,
                                    std::round(kCheckerSizes[app.checkerSize] * app.scale), colors);
    if (i != 0) {
        c.matchOutline = 0;
        c.forcedMip = -1;
    }
    return c;
}

const int kPairs[3][2] = {{0, 1}, {0, 2}, {1, 2}};

void setSecond(ViewConstants& c, const CompareImage& first, const CompareImage& second) {
    c.image2Size[0] = float(second.image->width);
    c.image2Size[1] = float(second.image->height);
    const bool stretch = app.compareSameSize && first.image->width && first.image->height;
    c.scale2[0] = stretch ? float(second.image->width) / float(first.image->width) : 1.0f;
    c.scale2[1] = stretch ? float(second.image->height) / float(first.image->height) : 1.0f;
    c.channels2 = uint32_t(htv::formatInfo(second.image->format).channels);
    c.srgb1 = isSrgbFormat(first.image->format) ? 1u : 0u;
    c.srgb2 = isSrgbFormat(second.image->format) ? 1u : 0u;
    c.diffGain = app.compareGain;
}

Target canvasTarget() { return {0, app.winW, app.winH, true}; }

void drawCompare(const UiColors& colors) {
    const Target target = canvasTarget();
    const float vx0 = app.vpX, vx1 = app.vpX + app.vpW;
    auto draw = [&](int i, float left, float right, const Program& p, const GpuImage* second, const ViewConstants& c) {
        left = std::max(left, vx0);
        right = std::min(right, vx1);
        if (right <= left) return;
        const int x0 = int(std::round(left)), x1 = int(std::round(right));
        drawImage(target, x0, int(app.vpY), x1 - x0, int(app.vpH), c, p, c.matchOutline ? &app.matchMask : nullptr,
                  layer(i).srv, second);
    };
    if (app.compareMode == CompareMode::Split) {
        int ids[3];
        const int n = compareLayers(ids);
        float aw, ah;
        imageSizeOnScreen(aw, ah);
        for (int k = 0; k < n; ++k) {
            const float left = k == 0 ? vx0 : app.originX + app.compareShutters[k - 1] * aw * app.zoom;
            const float right = k == n - 1 ? vx1 : app.originX + app.compareShutters[k] * aw * app.zoom;
            draw(ids[k], left, right, gl.main, nullptr, layerConstants(ids[k], colors));
        }
        return;
    }
    const int first = kPairs[app.comparePair][0], second = kPairs[app.comparePair][1];
    const CompareImage a = layer(first), b = layer(second);
    if (!a.srv || !b.srv) {
        draw(0, vx0, vx1, gl.main, nullptr, layerConstants(0, colors));
        return;
    }
    if (app.compareMode == CompareMode::Blend) {
        draw(first, vx0, vx1, gl.main, nullptr, layerConstants(first, colors));
        const float f = std::clamp(app.compareBlend, 0.0f, 1.0f);
        glEnable(GL_BLEND);
        glBlendColor(f, f, f, f);
        glBlendFunc(GL_CONSTANT_COLOR, GL_ONE_MINUS_CONSTANT_COLOR);
        ViewConstants c = layerConstants(second, colors);
        c.matchOutline = 0;
        draw(second, vx0, vx1, gl.main, nullptr, c);
        glDisable(GL_BLEND);
        return;
    }
    ViewConstants c = layerConstants(first, colors);
    c.matchOutline = 0;
    setSecond(c, a, b);
    draw(first, vx0, vx1, gl.diff, b.srv, c);
}

}  // namespace

int compareLayers(int* out) {
    int n = 0;
    if (!app.hasImage) return 0;
    out[n++] = 0;
    for (int i = 0; i < 2; ++i)
        if (app.compareSlots[i].srv) out[n++] = i + 1;
    return n;
}

void updateCompareStats() {
    static int key[5] = {-1, -1, -1, -1, -1};
    if (!app.compare || app.compareMode != CompareMode::Difference) {
        key[0] = -1;
        return;
    }
    const int first = kPairs[app.comparePair][0], second = kPairs[app.comparePair][1];
    const CompareImage a = layer(first), b = layer(second);
    if (!a.srv || !b.srv) {
        app.compareStats = CompareStats();
        key[0] = -1;
        return;
    }
    const int now[5] = {app.comparePair, a.generation, b.generation, app.compareSameSize ? 1 : 0, 1};
    if (std::equal(now, now + 5, key) && app.compareStats.valid) return;
    std::copy(now, now + 5, key);
    app.compareStats = CompareStats();
    const int w = int(a.image->width), h = int(a.image->height);
    CompareStats st;
    double sum = 0, sumSq = 0;
    const bool ok = readRows(
        w, h,
        [&](const Target& t, int x0, int y0, int bw, int bh) {
            ViewConstants c{};
            c.imageSize[0] = float(w);
            c.imageSize[1] = float(h);
            c.imageOrigin[0] = float(x0);
            c.imageOrigin[1] = float(y0);
            c.channels = uint32_t(htv::formatInfo(a.image->format).channels);
            setSecond(c, a, b);
            drawImage(t, 0, 0, bw, bh, c, gl.diffRead, nullptr, a.srv, b.srv);
        },
        [&](const float* d, int width) {
            for (int x = 0; x < width; ++x, d += 4) {
                if (d[0] < 0) continue;
                const double m = std::max({d[0], d[1], d[2]});
                if (!(m == m)) continue;
                st.maxDelta = std::max(st.maxDelta, m);
                sum += double(d[0]) + d[1] + d[2];
                sumSq += double(d[0]) * d[0] + double(d[1]) * d[1] + double(d[2]) * d[2];
                ++st.pixels;
                if (m > 2.5 / 255.0) ++st.pixelsOver;
            }
        });
    if (!ok) return;
    if (st.pixels) {
        st.meanDelta = sum / (3.0 * double(st.pixels));
        st.mse = sumSq / (3.0 * double(st.pixels));
    }
    st.valid = true;
    app.compareStats = st;
}

void render() {
    updateColorMatch();
    updateCompareStats();
    drawUi();

    const UiColors colors = uiColors();
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, app.winW, app.winH);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(colors.viewport[0], colors.viewport[1], colors.viewport[2], 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    int layers[3];
    if (app.compare && app.hasImage && app.srv && compareLayers(layers) > 1) {
        drawCompare(colors);
    } else if (app.hasImage && app.srv) {
        float x0, y0, x1, y1;
        shownArea(x0, y0, x1, y1);
        const ViewConstants c = makeConstants(app.image, x0, y0, app.zoom, app.tiling == Tiling::Off ? 1.0f : 3.0f,
                                              std::round(kCheckerSizes[app.checkerSize] * app.scale), colors);
        drawImage(canvasTarget(), int(app.vpX), int(app.vpY), int(std::ceil(app.vpW)), int(std::ceil(app.vpH)), c,
                  gl.main, c.matchOutline ? &app.matchMask : nullptr);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, app.winW, app.winH);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

namespace {

std::vector<uint8_t> renderThumbnailOf(const htv::Image& image, const GpuImage& srv, int size, bool contain = false) {
    std::vector<uint8_t> out;
    static Readback rb;
    if (!srv || !rb.create(size, size, false)) return out;
    const float w = float(app.rotation & 1 ? image.height : image.width);
    const float h = float(app.rotation & 1 ? image.width : image.height);
    const float zoom = float(size) / (contain ? std::max(w, h) : std::min(w, h));
    ViewConstants c =
        makeConstants(image, (size - w * zoom) * 0.5f, (size - h * zoom) * 0.5f, zoom, 1.0f, 4.0f, uiColors());
    c.clipping = 0;
    c.matchOutline = 0;
    c.forcedMip = -1;
    rb.clear();
    drawImage(rb.target(), 0, 0, size, size, c, gl.main, nullptr, &srv);
    out.resize(size_t(size) * size * 4);
    rb.read(out.data());
    return out;
}

}  // namespace

std::vector<uint8_t> renderThumbnail(int size) {
    if (!app.hasImage) return {};
    return renderThumbnailOf(app.image, app.srv, size);
}

GpuView* compareThumbnail(int slot) {
    constexpr int kSize = 64;
    static TextureRef a;
    static int aGeneration = -1, aRotation = -1;
    if (slot < 0) {
        if (!app.hasImage) return nullptr;
        if (aGeneration != app.imageGeneration || aRotation != app.rotation) {
            const std::vector<uint8_t> px = renderThumbnailOf(app.image, app.srv, kSize);
            a = px.empty() ? TextureRef() : createTexture(px.data(), kSize, kSize);
            aGeneration = app.imageGeneration;
            aRotation = app.rotation;
        }
        return a.get();
    }
    CompareSlot& s = app.compareSlots[slot];
    if (s.srv && !s.thumbnail) {
        const std::vector<uint8_t> px = renderThumbnailOf(s.image, s.srv, kSize);
        if (!px.empty()) s.thumbnail = createTexture(px.data(), kSize, kSize);
    }
    return s.thumbnail.get();
}

GpuView* layerThumbnail(int index) {
    constexpr int kSize = 64;
    static std::vector<TextureRef> thumbs;
    static int fileGeneration = -1, rotation = -1, madeInFrame = -1, made = 0;
    if (!app.hasImage || index < 0 || index >= int(app.image.layers.size())) return nullptr;
    if (fileGeneration != app.fileGeneration || rotation != app.rotation) {
        thumbs.clear();
        fileGeneration = app.fileGeneration;
        rotation = app.rotation;
    }
    thumbs.resize(app.image.layers.size());
    if (thumbs[size_t(index)]) return thumbs[size_t(index)].get();
    if (app.image.layers[size_t(index)].group) return nullptr;
    const int frame = ImGui::GetFrameCount();
    if (madeInFrame != frame) {
        madeInFrame = frame;
        made = 0;
    }
    if (made >= 3) {
        requestRedraw();
        return nullptr;
    }
    ++made;
    std::vector<uint8_t> px;
    const htv::Layer& l = app.image.layers[size_t(index)];
    const bool onScreen = l.mips.empty() && app.image.layerKind != htv::LayerKind::PsdLayers;
    if (onScreen && index == app.image.shownLayer) {
        px = renderThumbnailOf(app.image, app.srv, kSize);
    } else if (!l.mips.empty()) {
        size_t level = 0;
        while (level + 1 < l.mips.size() && std::min(l.mips[level + 1].width, l.mips[level + 1].height) >= uint32_t(kSize))
            ++level;
        htv::Image one;
        one.width = l.mips[level].width;
        one.height = l.mips[level].height;
        one.format = l.format;
        one.maxValue = l.maxValue;
        one.mips.push_back(l.mips[level]);
        GpuImage srv;
        int mips = 1;
        std::string error;
        if (createImageTexture(one, srv, mips, error))
            px = renderThumbnailOf(one, srv, kSize, app.image.layerKind == htv::LayerKind::PsdLayers);
    }
    TextureRef texture;
    if (!px.empty()) texture = createTexture(px.data(), kSize, kSize);
    if (!texture) texture = createTexture(std::vector<uint8_t>(size_t(kSize) * kSize * 4, 0).data(), kSize, kSize);
    thumbs[size_t(index)] = std::move(texture);
    return thumbs[size_t(index)].get();
}

bool readPixelsGpu(int x, int y, int n, float* rgba) {
    static Readback rb;
    if (!app.hasImage || !app.srv || !rb.create(5, 5, true)) return false;
    static int cachedX = -1, cachedY = -1, cachedN = 0, cachedGeneration = -1;
    static float cached[25 * 4];
    if (x != cachedX || y != cachedY || n != cachedN || app.imageGeneration != cachedGeneration) {
        ViewConstants c{};
        c.imageSize[0] = float(app.image.width);
        c.imageSize[1] = float(app.image.height);
        c.imageOrigin[0] = float(x - n / 2);
        c.imageOrigin[1] = float(y - n / 2);
        rb.clear();
        drawImage(rb.target(), 0, 0, n, n, c, gl.read);
        float all[25 * 4];
        rb.read(all);
        for (int row = 0; row < n; ++row) std::memcpy(cached + row * n * 4, all + row * 5 * 4, size_t(n) * 16);
        cachedX = x;
        cachedY = y;
        cachedN = n;
        cachedGeneration = app.imageGeneration;
    }
    std::memcpy(rgba, cached, size_t(n) * n * 16);
    return true;
}

bool scanImageGpu(const std::function<void(const float* rgba, int width)>& row) {
    if (!app.hasImage || !app.srv) return false;
    const int w = int(app.image.width), h = int(app.image.height);
    return readRows(
        w, h,
        [&](const Target& t, int x0, int y0, int bw, int bh) {
            ViewConstants c{};
            c.imageSize[0] = float(w);
            c.imageSize[1] = float(h);
            c.imageOrigin[0] = float(x0);
            c.imageOrigin[1] = float(y0);
            drawImage(t, 0, 0, bw, bh, c, gl.read);
        },
        row);
}

namespace {

float srgbEncode(float c) {
    c = std::clamp(c, 0.0f, 1.0f);
    return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}

}  // namespace

bool setMatchTarget(int x, int y) {
    float c[4];
    // Uncompressed images are read on the CPU, exactly as stored; compressed ones through the GPU.
    if (!htv::readPixel(app.image, uint32_t(x), uint32_t(y), c)) {
        if (!readPixelsGpu(x, y, 1, c)) return false;
        if (isSrgbFormat(app.image.format))
            for (int i = 0; i < 3; ++i) c[i] = srgbEncode(c[i]);
    }
    const int channels = htv::formatInfo(app.image.format).channels;
    if (channels == 1) c[1] = c[2] = c[0], c[3] = 1.0f;
    else if (channels == 2) c[2] = 0.0f, c[3] = 1.0f;
    std::copy(c, c + 4, app.matchTarget);
    app.matchHasTarget = true;
    app.matchValid = false;
    return true;
}

void updateColorMatch() {
    static int generation = -1, mode = -1, tolerance = -1;
    static float target[4];
    if (!app.colorMatch || !app.matchHasTarget || !app.hasImage || !app.srv) {
        app.matchValid = false;
        generation = -1;
        return;
    }
    if (generation == app.imageGeneration && mode == app.matchMode && tolerance == app.matchTolerance &&
        std::equal(target, target + 4, app.matchTarget) && app.matchValid)
        return;
    if (generation != app.imageGeneration) app.matchRegion = -1;
    generation = app.imageGeneration;
    mode = app.matchMode;
    tolerance = app.matchTolerance;
    std::copy(app.matchTarget, app.matchTarget + 4, target);
    app.matchValid = false;

    // One byte per pixel in the image's tiles, drawn on the GPU and read back for the count and the regions.
    static std::vector<GLuint> fbos;
    auto sameLayout = [] {
        if (app.matchMask.tiles.size() != app.srv.tiles.size() || fbos.size() != app.srv.tiles.size()) return false;
        for (size_t i = 0; i < fbos.size(); ++i) {
            const GpuTile &m = app.matchMask.tiles[i], &t = app.srv.tiles[i];
            if (m.x != t.x || m.y != t.y || m.width != t.width || m.height != t.height) return false;
        }
        return true;
    };
    if (!sameLayout()) {
        app.matchMask.reset();
        if (!fbos.empty()) glDeleteFramebuffers(GLsizei(fbos.size()), fbos.data());
        fbos.clear();
        for (const GpuTile& t : app.srv.tiles) {
            GpuTile m;
            m.x = t.x;
            m.y = t.y;
            m.width = t.width;
            m.height = t.height;
            GLuint texture = 0, fbo = 0;
            glGenTextures(1, &texture);
            m.srv = makeView(texture);
            glBindTexture(GL_TEXTURE_2D, texture);
            glTexStorage2D(GL_TEXTURE_2D, 1, GL_R8, GLsizei(t.width), GLsizei(t.height));
            glGenFramebuffers(1, &fbo);
            glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
            fbos.push_back(fbo);
            app.matchMask.tiles.push_back(std::move(m));
        }
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }
    const int w = int(app.image.width), h = int(app.image.height);
    ViewConstants c{};
    c.imageSize[0] = float(w);
    c.imageSize[1] = float(h);
    c.channels = uint32_t(htv::formatInfo(app.image.format).channels);
    std::copy(app.matchTarget, app.matchTarget + 4, c.matchTarget);
    c.matchMode = uint32_t(app.matchMode);
    c.matchTolerance = (float(app.matchTolerance) + 0.5f) / 255.0f;
    c.matchSrgb = isSrgbFormat(app.image.format) ? 1u : 0u;
    std::vector<uint8_t> all, rgba;
    try {
        all.resize(size_t(w) * h);
    } catch (const std::bad_alloc&) {
        return;
    }
    for (size_t i = 0; i < fbos.size(); ++i) {
        const GpuTile& t = app.srv.tiles[i];
        const Target target{fbos[i], int(t.width), int(t.height), false};
        drawPass(target, 0, 0, int(t.width), int(t.height), c, gl.match, tex(t));
        rgba.resize(size_t(t.width) * t.height * 4);
        glBindFramebuffer(GL_FRAMEBUFFER, fbos[i]);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, GLsizei(t.width), GLsizei(t.height), GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
        for (uint32_t y = 0; y < t.height; ++y)
            for (uint32_t x = 0; x < t.width; ++x)
                all[(size_t(t.y) + y) * w + t.x + x] = rgba[(size_t(y) * t.width + x) * 4];
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    app.matchRegions = htv::findRegions(all.data(), w, h, size_t(w));
    if (app.matchRegion >= int(app.matchRegions.boxes.size())) app.matchRegion = -1;
    app.matchValid = true;
}

}  // namespace htv
