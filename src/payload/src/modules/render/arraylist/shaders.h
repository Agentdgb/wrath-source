// shaders.h — GLSL sources for the arraylist glass pipeline, embedded as
// string literals so the DLL doesn't need to ship a resources/ folder.
//
// The four shaders (fullscreen vsh, kawase down, kawase up, composite) are
// ports of Breeze/Vape's originals under resources/shader/arraylist/. The
// math is identical; only the #version line was pinned to 330 core (rather
// than 430) so the pipeline also loads on the older core contexts some game
// launchers land on.

#pragma once

namespace vanta::al::shaders {

// Fullscreen triangle-pair vertex shader — clip-space in, UV out.
inline constexpr const char* fullscreen_vsh = R"GLSL(
#version 330 core
precision highp float;
layout(location = 0) in vec2 aPos;
out vec2 vUv;
void main() {
    vUv = aPos * 0.5 + 0.5;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)GLSL";

// Dual-Kawase downsample step.
inline constexpr const char* kawase_down_fsh = R"GLSL(
#version 330 core
precision highp float;
uniform sampler2D uTex;
uniform vec2  uTexel;
uniform float uOffset;
in  vec2 vUv;
out vec4 frag;
void main() {
    vec2 o = uTexel * uOffset;
    vec4 c =
        texture(uTex, vUv + vec2(-o.x, -o.y)) +
        texture(uTex, vUv + vec2( o.x, -o.y)) +
        texture(uTex, vUv + vec2(-o.x,  o.y)) +
        texture(uTex, vUv + vec2( o.x,  o.y));
    frag = c * 0.25;
}
)GLSL";

// Dual-Kawase upsample step.
inline constexpr const char* kawase_up_fsh = R"GLSL(
#version 330 core
precision highp float;
uniform sampler2D uTex;
uniform vec2  uTexel;
uniform float uOffset;
in  vec2 vUv;
out vec4 frag;
void main() {
    vec2 o = uTexel * uOffset;
    vec4 c =
        texture(uTex, vUv + vec2(-o.x,  0.0)) +
        texture(uTex, vUv + vec2( o.x,  0.0)) +
        texture(uTex, vUv + vec2( 0.0, -o.y)) +
        texture(uTex, vUv + vec2( 0.0,  o.y)) +
        texture(uTex, vUv + vec2(-o.x, -o.y)) +
        texture(uTex, vUv + vec2( o.x, -o.y)) +
        texture(uTex, vUv + vec2(-o.x,  o.y)) +
        texture(uTex, vUv + vec2( o.x,  o.y));
    frag = c * (1.0 / 8.0);
}
)GLSL";

// Composite: blurred scene through the union of rounded rects + drop shadow.
// MAX_RECTS matches al_glass.cpp — bump both together if you ever raise it.
inline constexpr const char* composite_fsh = R"GLSL(
#version 330 core
precision highp float;
uniform sampler2D uBlur;
uniform vec2  uResolution;
uniform vec2  uBboxMin;
uniform vec2  uBboxSize;
const int MAX_RECTS = 64;
uniform int   uRectCount;
uniform vec4  uRects[MAX_RECTS];
uniform vec4  uRadii[MAX_RECTS];
uniform float uBrightness;
uniform vec4  uTint;
uniform float uEdge;
uniform float uShadowRadius;
uniform float uShadowStrength;
uniform float uShadowGamma;
uniform float uOpacity;
out vec4 frag;

float roundedBoxSDF4(vec2 p, vec2 halfSize, vec4 r) {
    float rad;
    if (p.x < 0.0) rad = (p.y > 0.0) ? r.x : r.w;
    else           rad = (p.y > 0.0) ? r.y : r.z;
    rad = min(rad, min(halfSize.x, halfSize.y));
    vec2 q = abs(p) - halfSize + vec2(rad);
    return length(max(q, 0.0)) - rad;
}

float unionSDF(vec2 pixelPos) {
    float d = 1e9;
    for (int i = 0; i < MAX_RECTS; i++) {
        if (i >= uRectCount) break;
        vec4 rc = uRects[i];
        vec2 halfSize = rc.zw * 0.5;
        vec2 center   = rc.xy + halfSize;
        d = min(d, roundedBoxSDF4(pixelPos - center, halfSize, uRadii[i]));
    }
    return d;
}

void main() {
    vec2  p    = gl_FragCoord.xy;
    float dist = unionSDF(p);

    float shapeA  = 1.0 - smoothstep(0.0, uEdge, dist);
    float shadowA = 0.0;
    if (uShadowRadius > 0.001 && uShadowStrength > 0.001) {
        float outside = 1.0 - smoothstep(0.0, uShadowRadius, max(dist, 0.0));
        shadowA = pow(clamp(outside, 0.0, 1.0), uShadowGamma) * uShadowStrength * (1.0 - shapeA);
    }
    float coverA = shapeA + shadowA;
    if (coverA <= 0.001) discard;

    vec2 uv    = (p - uBboxMin) / uBboxSize;
    vec4 b     = texture(uBlur, uv);
    vec3 glass = mix(b.rgb, uTint.rgb, clamp(uTint.a, 0.0, 1.0)) * uBrightness;

    vec3 rgb = mix(vec3(0.0), glass, shapeA / max(coverA, 0.001));

    /* @a pane that can actually fade out.

       uTint.a is NOT an opacity. It says how much of the tint colour is mixed
       into the blurred scene, so a caller fading a pane by winding it down to
       zero does not fade the pane at all — it strips the colour out and leaves
       the blurred scene composited at full coverage. That is an opaque frosted
       rectangle with nothing in it, which is exactly the bright smear a fading
       panel left standing for the whole length of its fade.

       Applied to the coverage, after the colour has been resolved, so the
       shape-versus-shadow ratio inside the pane is untouched by it. */
    float outA = coverA * clamp(uOpacity, 0.0, 1.0);
    if (outA <= 0.001) discard;

    frag = vec4(rgb * outA, outA);
}
)GLSL";

} // namespace vanta::al::shaders
