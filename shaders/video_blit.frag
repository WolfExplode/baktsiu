#version 330

uniform int uCompareMode;
uniform sampler2D uVideo;
uniform sampler2D uVideoR;
uniform vec2 uVideoSize;
uniform vec2 uVideoSizeR;
uniform vec2 uWindowSize;
uniform vec2 uContentOrigin;
uniform vec2 uContentSize;
uniform float uSplitPos;

// Match still-image present pass: AP1 working space then ODT (primaries + display gamma).
uniform float uDisplayGamma;
uniform int uOutTransformType;
// Same as color_grading uEV: exposure in stops, applied in AP1 before ODT.
uniform float uEV;
// Match present.frag: channel view + optional ACES (before ODT).
uniform int uPresentMode;
uniform bool uApplyToneMapping;
// Same bits as present.frag: 0x1 difference, 0x2 heatmap, 0x4 overflow, 0x8 underflow.
uniform int uPixelMarkerFlags;

in vec2 vUV;
out vec4 oColor;

const vec4 kLetterbox = vec4(0.12, 0.14, 0.15, 1.0);

#define mul(x, y) (y * x)

// mpv RGBA8 FBO: treat as BT.709 transfer (same primaries as sRGB); linearize → AP1 like color_grading.frag.
const mat3 BT709_2_AP1_MAT = mat3(
    0.6130973, 0.3395229, 0.0473793,
    0.0701942, 0.9163555, 0.0134523,
    0.0206156, 0.1095698, 0.8698151);

const mat3 AP1_2_BT709_MAT = mat3(
    1.7050515, -0.6217907, -0.0832587,
   -0.1302571,  1.1408029, -0.0105482,
   -0.0240033, -0.1289688,  1.1529717);

const mat3 AP1_2_P3D65_MAT = mat3(
    1.3792145, -0.3088633, -0.0703498,
   -0.0693355,  1.0822950, -0.0129618,
   -0.0021590, -0.0454592,  1.0476177);

const mat3 AP1_2_BT2020_MAT = mat3(
    1.0258249, -0.0200529, -0.0057714,
   -0.002235 ,  1.0045849, -0.0023520,
   -0.0050133, -0.0252900,  1.0303028);

const mat3 AP1_2_XYZ_MAT = mat3(
     0.6624541811, 0.1340042065, 0.1561876870,
     0.2722287168, 0.6740817658, 0.0536895174,
    -0.0055746495, 0.0040607335, 1.0103391003);

const mat3 XYZ_2_AP1_MAT = mat3(
     1.6410233797, -0.3248032942, -0.2364246952,
    -0.6636628587,  1.6153315917,  0.0167563477,
     0.0117218943, -0.0082844420,  0.9883948585);

const mat3 AP1_2_AP0_MAT = mat3(
     0.6954522414, 0.1406786965, 0.1638690622,
     0.0447945634, 0.8596711185, 0.0955343182,
    -0.0055258826, 0.0040252103, 1.0015006723);

const mat3 AP0_2_AP1_MAT = mat3(
     1.4514393161, -0.2365107469, -0.2149285693,
    -0.0765537734,  1.1762296998, -0.0996759264,
     0.0083161484, -0.0060324498,  0.9977163014);

float labf(float v)
{
    const float c1 = 0.008856451679;
    const float c2 = 7.787037037;
    const float c3 = 0.1379310345;
    return mix(c2 * v + c3, pow(v, 1.0 / 3.0), v > c1);
}

vec3 XYZtoLab(vec3 xyz)
{
    const vec3 D65WhitePoint = vec3(0.95047, 1.000, 1.08883);
    xyz /= D65WhitePoint;
    vec3 v = vec3(labf(xyz.x), labf(xyz.y), labf(xyz.z));
    return vec3((116.0 * v.y) - 16.0, 500.0 * (v.x - v.y), 200.0 * (v.y - v.z));
}

#define HALF_MIN 5.96e-08
#define HALF_MAX 65504.0
#define PI 3.14159265359

float rgb_2_saturation(vec3 rgb)
{
    const float TINY = HALF_MIN;
    float ma = max(rgb.r, max(rgb.g, rgb.b));
    float mi = min(rgb.r, min(rgb.g, rgb.b));
    return (max(ma, TINY) - max(mi, TINY)) / max(ma, 1e-2);
}

float rgb_2_yc(vec3 rgb)
{
    const float ycRadiusWeight = 1.75;
    float r = rgb.x;
    float g = rgb.y;
    float b = rgb.z;
    float chroma = sqrt(b * (b - g) + g * (g - r) + r * (r - b));
    return (b + g + r + ycRadiusWeight * chroma) / 3.0;
}

float rgb_2_hue(vec3 rgb)
{
    float hue;
    if (rgb.x == rgb.y && rgb.y == rgb.z) {
        hue = 0.0;
    } else {
        hue = (180.0 / PI) * atan(sqrt(3.0) * (rgb.y - rgb.z), 2.0 * rgb.x - rgb.y - rgb.z);
    }
    if (hue < 0.0) {
        hue = hue + 360.0;
    }
    return hue;
}

float center_hue(float hue, float centerH)
{
    float hueCentered = hue - centerH;
    if (hueCentered < -180.0) {
        hueCentered = hueCentered + 360.0;
    } else if (hueCentered > 180.0) {
        hueCentered = hueCentered - 360.0;
    }
    return hueCentered;
}

float sigmoid_shaper(float x)
{
    float t = max(1.0 - abs(x / 2.0), 0.0);
    float y = 1.0 + sign(x) * (1.0 - t * t);
    return y / 2.0;
}

float glow_fwd(float ycIn, float glowGainIn, float glowMid)
{
    if (ycIn <= 2.0 / 3.0 * glowMid) {
        return glowGainIn;
    }
    if (ycIn >= 2.0 * glowMid) {
        return 0.0;
    }
    return glowGainIn * (glowMid / ycIn - 1.0 / 2.0);
}

vec3 XYZ_2_xyY(vec3 XYZ)
{
    float divisor = max(dot(XYZ, vec3(1.0)), HALF_MIN);
    return vec3(XYZ.xy / divisor, XYZ.y);
}

vec3 xyY_2_XYZ(vec3 xyY)
{
    float m = xyY.z / max(xyY.y, HALF_MIN);
    vec3 XYZ = vec3(xyY.xz, (1.0 - xyY.x - xyY.y));
    XYZ.xz *= m;
    return XYZ;
}

const float DIM_SURROUND_GAMMA = 0.9811;
const float RRT_GLOW_GAIN = 0.05;
const float RRT_GLOW_MID = 0.08;
const float RRT_RED_SCALE = 0.82;
const float RRT_RED_PIVOT = 0.03;
const float RRT_RED_HUE = 0.0;
const float RRT_RED_WIDTH = 135.0;

const mat3 RRT_SAT_MAT = mat3(
    0.9708890, 0.0269633, 0.00214758,
    0.0108892, 0.9869630, 0.00214758,
    0.0108892, 0.0269633, 0.96214800);

const mat3 ODT_SAT_MAT = mat3(
    0.949056, 0.0471857, 0.00375827,
    0.019056, 0.9771860, 0.00375827,
    0.019056, 0.0471857, 0.93375800);

vec3 darkSurround_to_dimSurround(vec3 linearCV)
{
    vec3 XYZ = mul(AP1_2_XYZ_MAT, linearCV);
    vec3 xyY = XYZ_2_xyY(XYZ);
    xyY.z = clamp(xyY.z, 0.0, HALF_MAX);
    xyY.z = pow(xyY.z, DIM_SURROUND_GAMMA);
    XYZ = xyY_2_XYZ(xyY);
    return mul(XYZ_2_AP1_MAT, XYZ);
}

vec3 AcesToneMapping(vec3 aces)
{
    float saturation = rgb_2_saturation(aces);
    float ycIn = rgb_2_yc(aces);
    float s = sigmoid_shaper((saturation - 0.4) / 0.2);
    float addedGlow = 1.0 + glow_fwd(ycIn, RRT_GLOW_GAIN * s, RRT_GLOW_MID);
    aces *= addedGlow;

    float hue = rgb_2_hue(aces);
    float centeredHue = center_hue(hue, RRT_RED_HUE);
    float hueWeight = smoothstep(0.0, 1.0, 1.0 - abs(2.0 * centeredHue / RRT_RED_WIDTH));
    hueWeight *= hueWeight;

    aces.r += hueWeight * saturation * (RRT_RED_PIVOT - aces.r) * (1.0 - RRT_RED_SCALE);

    aces = max(vec3(0.0), aces);
    vec3 rgbPre = mul(AP0_2_AP1_MAT, aces);
    rgbPre = clamp(rgbPre, vec3(0.0), vec3(HALF_MAX));
    rgbPre = mul(RRT_SAT_MAT, rgbPre);

    const float a = 180.08877305;
    const float b = 5.82507674;
    const float c = 190.14106451;
    const float d = 56.89654471;
    const float e = 53.22517853;
    vec3 rgbPost = (rgbPre * (a * rgbPre + b)) / (rgbPre * (c * rgbPre + d) + e);
    vec3 linearCV = darkSurround_to_dimSurround(rgbPost);
    return mul(ODT_SAT_MAT, linearCV);
}

vec3 colorTransform(vec3 color, int mode)
{
    if (uApplyToneMapping) {
        color = AcesToneMapping(mul(AP1_2_AP0_MAT, color));
    }
    if (mode == 1) {
        color = color.rrr;
    } else if (mode == 2) {
        color = color.ggg;
    } else if (mode == 3) {
        color = color.bbb;
    } else if (mode == 4) {
        color = mul(AP1_2_XYZ_MAT, color).yyy;
    } else if (mode >= 5 && mode < 8) {
        const vec3 labMin = vec3(0, -128, -128);
        const vec3 labMax = vec3(100, 128, 128);
        vec3 lab = XYZtoLab(mul(AP1_2_XYZ_MAT, color));
        color = ((lab - labMin) / (labMax - labMin));
        color = vec3(color[mode - 5]);
    }
    return color;
}

vec3 decode709(vec3 color)
{
    bvec3 isSmall = lessThanEqual(color, vec3(0.081));
    return mix(pow((color + vec3(0.099)) / 1.099, vec3(1.0 / 0.45)), color / 4.5, isSmall);
}

vec3 videoRgbToAp1(vec3 encRgb)
{
    vec3 linear709 = decode709(encRgb);
    return mul(BT709_2_AP1_MAT, linear709);
}

vec3 outputTransform(vec3 color, int type, float gamma)
{
    if (type == 0) {
        color = mul(AP1_2_BT709_MAT, color);
    } else if (type == 1) {
        color = mul(AP1_2_P3D65_MAT, color);
    } else if (type == 2) {
        color = mul(AP1_2_BT2020_MAT, color);
    }
    color = max(color, vec3(0.0));
    return pow(color, vec3(1.0 / gamma));
}

vec3 getHeatColor(float value)
{
    float r = clamp(value, 0, 1.0);
    float g = sin(180.0 * radians(value));
    float b = cos(60.0 * radians(value));
    return vec3(r, g, b);
}

float getColorDistance(vec3 color1, vec3 color2)
{
    vec3 lab1 = XYZtoLab(mul(AP1_2_XYZ_MAT, color1));
    vec3 lab2 = XYZtoLab(mul(AP1_2_XYZ_MAT, color2));
    vec3 diff = lab1 - lab2;
    return dot(diff, diff);
}

vec3 getCheckerColor(vec2 uv, vec2 windowSize)
{
    vec2 bgUV = uv * vec2(1.0, windowSize.y / windowSize.x);
    ivec2 iwh = ivec2(round(fract(bgUV * windowSize.y * 0.08)));
    return vec3(iwh.x ^ iwh.y) * 0.18;
}

vec4 overlayAp1PixelMarker(vec4 color, int markerFlags)
{
    bool isOverflow = all(greaterThan(color.rgb, vec3(1.0))) && ((markerFlags & 0x4) != 0);
    color = mix(color, vec4(1.0, 0.0, 0.0, 1.0), vec4(isOverflow));
    bool isUnderflow = all(lessThan(color.rgb, vec3(1e-5))) && ((markerFlags & 0x8) != 0);
    color = mix(color, vec4(0.0, 0.0, 1.0, 1.0), vec4(isUnderflow));
    return color;
}

bool videoContainFetchEnc(sampler2D tex, vec2 localBL, vec2 panelPx, vec2 vidSize, out vec3 encRgb)
{
    float vw = max(vidSize.x, 1.0);
    float vh = max(vidSize.y, 1.0);
    float pw = max(panelPx.x, 1.0);
    float ph = max(panelPx.y, 1.0);
    float s = min(pw / vw, ph / vh);
    vec2 disp = vec2(vw * s / pw, vh * s / ph);
    vec2 off = (vec2(1.0) - disp) * 0.5;
    vec2 vidUv = (localBL - off) / max(disp, vec2(1e-5));
    if (vidUv.x < 0.0 || vidUv.x > 1.0 || vidUv.y < 0.0 || vidUv.y > 1.0) {
        return false;
    }
    encRgb = texture(tex, vec2(vidUv.x, 1.0 - vidUv.y)).rgb;
    return true;
}

// Markers after colorTransform (AP1). encRgb is raw mpv FBO (non-linear, ~BT.709/sRGB-ish, 8-bit).
// AP1-only tests match present.frag but rarely fire on SDR 8-bit; add encoded clip checks for video.
vec4 overlayVideoPixelMarker(vec4 ap1Color, vec3 encRgb, int markerFlags)
{
    bool ovAp1 = all(greaterThan(ap1Color.rgb, vec3(1.0)));
    const float kEncWhite = 253.0 / 255.0;
    bool ovEnc = any(greaterThan(encRgb, vec3(kEncWhite)));
    bool isOverflow = (ovAp1 || ovEnc) && ((markerFlags & 0x4) != 0);
    vec4 c = mix(ap1Color, vec4(1.0, 0.0, 0.0, 1.0), vec4(isOverflow));

    bool ufAp1 = all(lessThan(ap1Color.rgb, vec3(1e-5)));
    const float kEncBlack = 2.0 / 255.0;
    bool ufEnc = all(lessThan(encRgb, vec3(kEncBlack)));
    bool isUnderflow = (ufAp1 || ufEnc) && ((markerFlags & 0x8) != 0);
    c = mix(c, vec4(0.0, 0.0, 1.0, 1.0), vec4(isUnderflow));
    return c;
}

vec3 mapVideoToDisplay(vec3 encRgb)
{
    vec3 ap1 = videoRgbToAp1(encRgb);
    ap1 *= pow(2.0, uEV);
    ap1 = colorTransform(ap1, uPresentMode);
    vec4 c = overlayVideoPixelMarker(vec4(ap1, 1.0), encRgb, uPixelMarkerFlags);
    return outputTransform(c.rgb, uOutTransformType, uDisplayGamma);
}

// localBL: [0,1]^2 with (0,0) bottom-left, (1,1) top-right of the panel.
vec4 sampleContain(sampler2D tex, vec2 localBL, vec2 panelPx, vec2 vidSize)
{
    float vw = max(vidSize.x, 1.0);
    float vh = max(vidSize.y, 1.0);
    float pw = max(panelPx.x, 1.0);
    float ph = max(panelPx.y, 1.0);
    float s = min(pw / vw, ph / vh);
    vec2 disp = vec2(vw * s / pw, vh * s / ph);
    vec2 off = (vec2(1.0) - disp) * 0.5;
    vec2 vidUv = (localBL - off) / max(disp, vec2(1e-5));
    if (vidUv.x < 0.0 || vidUv.x > 1.0 || vidUv.y < 0.0 || vidUv.y > 1.0) {
        return kLetterbox;
    }
    vec4 t = texture(tex, vec2(vidUv.x, 1.0 - vidUv.y));
    t.rgb = mapVideoToDisplay(t.rgb);
    return t;
}

vec2 screenPxFromVUV(vec2 vUVin)
{
    return vec2(vUVin.x * uWindowSize.x, (1.0 - vUVin.y) * uWindowSize.y);
}

void main()
{
    vec2 sp = screenPxFromVUV(vUV);
    if (sp.x < uContentOrigin.x || sp.y < uContentOrigin.y
        || sp.x >= uContentOrigin.x + uContentSize.x || sp.y >= uContentOrigin.y + uContentSize.y) {
        oColor = kLetterbox;
        return;
    }

    vec2 local = (sp - uContentOrigin) / max(uContentSize, vec2(1.0));
    local.y = 1.0 - local.y;

    bool inDiffMode = (uPixelMarkerFlags & 0x3) != 0;
    bool enableHeatMap = ((uPixelMarkerFlags & 0x2) >> 1) != 0;
    vec3 checkerBg = getCheckerColor(vUV, uWindowSize);

    if (uCompareMode == 0) {
        oColor = sampleContain(uVideo, local, uContentSize, uVideoSize);
        return;
    }

    float spx = clamp(uSplitPos, 0.02, 0.98);
    float lineW = 1.0 / max(uContentSize.x, 1.0);
    bool showSplitter = uSplitPos != 1.0 && (uPixelMarkerFlags & 0x3) == 0;
    bool onSplit = abs(local.x - spx) < lineW * 0.5;

    // Side-by-side: each stream letterboxed in its column (split position = column boundary).
    if (uCompareMode == 2) {
        float wr = max(1.0 - spx, 1e-5);
        vec2 panelL = vec2(uContentSize.x * spx, uContentSize.y);
        vec2 panelR = vec2(uContentSize.x * wr, uContentSize.y);

        if (inDiffMode) {
            vec3 encL, encR;
            bool okL;
            bool okR;
            if (local.x < spx) {
                vec2 norm = vec2(local.x / max(spx, 1e-5), local.y);
                okL = videoContainFetchEnc(uVideo, norm, panelL, uVideoSize, encL);
                okR = videoContainFetchEnc(uVideoR, norm, panelR, uVideoSizeR, encR);
            } else {
                vec2 norm = vec2((local.x - spx) / wr, local.y);
                okL = videoContainFetchEnc(uVideo, norm, panelL, uVideoSize, encL);
                okR = videoContainFetchEnc(uVideoR, norm, panelR, uVideoSizeR, encR);
            }
            if (!okL || !okR) {
                oColor = vec4(checkerBg, 1.0);
            } else {
                vec3 ap1L = videoRgbToAp1(encL) * pow(2.0, uEV);
                vec3 ap1R = videoRgbToAp1(encR) * pow(2.0, uEV);
                float squareError = getColorDistance(ap1L, ap1R);
                bool leftSide = local.x < spx;
                vec3 baseRaw = mix(ap1R, ap1L, float(leftSide));
                vec3 rgb = mix(baseRaw, vec3(1.0, 0.0, 1.0), clamp(squareError, 0.0, 1.0));
                rgb = mix(rgb, getHeatColor(squareError), vec3(enableHeatMap));
                oColor = overlayAp1PixelMarker(vec4(rgb, 1.0), uPixelMarkerFlags);
                oColor.rgb = outputTransform(oColor.rgb, uOutTransformType, mix(uDisplayGamma, 1.0, enableHeatMap));
            }
        } else {
            if (local.x < spx) {
                vec2 localL = vec2(local.x / max(spx, 1e-5), local.y);
                oColor = sampleContain(uVideo, localL, panelL, uVideoSize);
            } else {
                vec2 localR = vec2((local.x - spx) / wr, local.y);
                oColor = sampleContain(uVideoR, localR, panelR, uVideoSizeR);
            }
        }
        if (onSplit) {
            oColor = vec4(1.0, 1.0, 1.0, 1.0);
        }
        return;
    }

    // uCompareMode == 1: vertical wipe / split line
    if (inDiffMode) {
        if (onSplit && showSplitter) {
            oColor = vec4(1.0, 1.0, 1.0, 1.0);
            return;
        }
        vec3 encL;
        vec3 encR;
        bool okL = videoContainFetchEnc(uVideo, local, uContentSize, uVideoSize, encL);
        bool okR = videoContainFetchEnc(uVideoR, local, uContentSize, uVideoSizeR, encR);
        if (!okL || !okR) {
            oColor = vec4(checkerBg, 1.0);
            return;
        }
        vec3 ap1L = videoRgbToAp1(encL) * pow(2.0, uEV);
        vec3 ap1R = videoRgbToAp1(encR) * pow(2.0, uEV);
        float squareError = getColorDistance(ap1L, ap1R);
        bool leftSide = local.x < spx;
        vec3 baseRaw = mix(ap1R, ap1L, float(leftSide));
        vec3 rgb = mix(baseRaw, vec3(1.0, 0.0, 1.0), clamp(squareError, 0.0, 1.0));
        rgb = mix(rgb, getHeatColor(squareError), vec3(enableHeatMap));
        oColor = overlayAp1PixelMarker(vec4(rgb, 1.0), uPixelMarkerFlags);
        oColor.rgb = outputTransform(oColor.rgb, uOutTransformType, mix(uDisplayGamma, 1.0, enableHeatMap));
        return;
    }

    if (onSplit) {
        oColor = vec4(1.0, 1.0, 1.0, 1.0);
        return;
    }

    vec4 cL = sampleContain(uVideo, local, uContentSize, uVideoSize);
    vec4 cR = sampleContain(uVideoR, local, uContentSize, uVideoSizeR);
    oColor = local.x < spx ? cL : cR;
}
