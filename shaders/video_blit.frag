#version 330

uniform int uCompareMode;
uniform sampler2D uVideo;
uniform sampler2D uVideoR;
uniform sampler2D uVideoUv;
uniform sampler2D uVideoUvR;
uniform int uVideoIsNv12;
uniform int uVideoRIsNv12;
uniform vec2 uVideoYTexSize;
uniform vec2 uVideoChromaTexSize;
uniform vec2 uVideoRYTexSize;
uniform vec2 uVideoRChromaTexSize;
uniform vec2 uVideoSize;
uniform vec2 uVideoSizeR;
// Coded picture size in luma pixels (MF width/height), not display-aspect width.
uniform vec2 uVideoPicPx;
uniform vec2 uVideoRPicPx;
uniform int uNv12Debug;
uniform vec2 uWindowSize;
uniform vec2 uContentOrigin;
uniform vec2 uContentSize;
uniform float uSplitPos;

in vec2 vUV;
out vec4 oColor;

const vec4 kLetterbox = vec4(0.12, 0.14, 0.15, 1.0);

vec4 sampleContainRgba(sampler2D tex, vec2 localBL, vec2 panelPx, vec2 vidSize)
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
    return texture(tex, vec2(vidUv.x, 1.0 - vidUv.y));
}

// MF NV12 limited-range to RGB (matches CPU nv12ToRgba in mf_video_reader.cpp).
// picPx = coded luma size (width,height). yTexSize.y may be larger (decoder padding rows): map using
// picture row indices only, same vertical sense as sampleContainRgba (v = 1 - vidUv.y).
vec4 sampleContainNv12(sampler2D yTex, sampler2D uvTex, vec2 localBL, vec2 panelPx, vec2 vidSize,
    vec2 picPx, vec2 yTexSize, vec2 chromaTexSize, int dbgMode)
{
    float vw = max(vidSize.x, 1.0);
    float vh = max(vidSize.y, 1.0);
    float pw = max(panelPx.x, 1.0);
    float ph = max(panelPx.y, 1.0);
    float sc = min(pw / vw, ph / vh);
    vec2 disp = vec2(vw * sc / pw, vh * sc / ph);
    vec2 off = (vec2(1.0) - disp) * 0.5;
    vec2 vidUv = (localBL - off) / max(disp, vec2(1e-5));
    if (vidUv.x < 0.0 || vidUv.x > 1.0 || vidUv.y < 0.0 || vidUv.y > 1.0) {
        return kLetterbox;
    }
    float W = max(picPx.x, 1.0);
    float H = max(picPx.y, 1.0);
    float xN = vidUv.x * (W - 1.0);
    // Row 0 = top of picture (first row in buffer); vidUv.y=1 top of image, vidUv.y=0 bottom.
    float yRowTop = (1.0 - vidUv.y) * (H - 1.0);
    vec2 yTc = vec2((xN + 0.5) / max(yTexSize.x, 1.0), (yRowTop + 0.5) / max(yTexSize.y, 1.0));
    float chromaCol = floor(xN * 0.5);
    float chromaRow = floor(yRowTop * 0.5);
    vec2 uvTc = vec2((chromaCol + 0.5) / max(chromaTexSize.x, 1.0), (chromaRow + 0.5) / max(chromaTexSize.y, 1.0));
    float yNorm = texture(yTex, yTc).r;
    if (dbgMode == 1) {
        return vec4(yNorm, yNorm, yNorm, 1.0);
    }
    vec2 uvRg = texture(uvTex, uvTc).rg;
    if (dbgMode == 2) {
        return vec4(uvRg, 0.0, 1.0);
    }
    // Match nv12ToRgba: int r = (c + 409*V + 128) >> 8 then /255 for display; not (c+...)/255.
    float Ys = yNorm * 255.0 - 16.0;
    vec2 UVs = uvRg * 255.0 - vec2(128.0);
    float c = 298.0 * Ys;
    float rByte = clamp((c + 409.0 * UVs.y + 128.0) / 256.0, 0.0, 255.0);
    float gByte = clamp((c - 100.0 * UVs.x - 208.0 * UVs.y + 128.0) / 256.0, 0.0, 255.0);
    float bByte = clamp((c + 516.0 * UVs.x + 128.0) / 256.0, 0.0, 255.0);
    return vec4(rByte / 255.0, gByte / 255.0, bByte / 255.0, 1.0);
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

    if (uCompareMode == 0) {
        if (uVideoIsNv12 != 0) {
            oColor = sampleContainNv12(uVideo, uVideoUv, local, uContentSize, uVideoSize, uVideoPicPx,
                uVideoYTexSize, uVideoChromaTexSize, uNv12Debug);
        } else {
            oColor = sampleContainRgba(uVideo, local, uContentSize, uVideoSize);
        }
        return;
    }

    float spx = clamp(uSplitPos, 0.02, 0.98);
    float lineW = 1.0 / max(uContentSize.x, 1.0);
    if (abs(local.x - spx) < lineW * 0.5) {
        oColor = vec4(1.0, 1.0, 1.0, 1.0);
        return;
    }

    vec4 cL;
    vec4 cR;
    if (uVideoIsNv12 != 0) {
        cL = sampleContainNv12(uVideo, uVideoUv, local, uContentSize, uVideoSize, uVideoPicPx, uVideoYTexSize,
            uVideoChromaTexSize, uNv12Debug);
    } else {
        cL = sampleContainRgba(uVideo, local, uContentSize, uVideoSize);
    }
    if (uVideoRIsNv12 != 0) {
        cR = sampleContainNv12(uVideoR, uVideoUvR, local, uContentSize, uVideoSizeR, uVideoRPicPx, uVideoRYTexSize,
            uVideoRChromaTexSize, uNv12Debug);
    } else {
        cR = sampleContainRgba(uVideoR, local, uContentSize, uVideoSizeR);
    }
    oColor = local.x < spx ? cL : cR;
}
