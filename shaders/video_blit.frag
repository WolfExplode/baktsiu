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

in vec2 vUV;
out vec4 oColor;

const vec4 kLetterbox = vec4(0.12, 0.14, 0.15, 1.0);

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
    return texture(tex, vec2(vidUv.x, 1.0 - vidUv.y));
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
        oColor = sampleContain(uVideo, local, uContentSize, uVideoSize);
        return;
    }

    float spx = clamp(uSplitPos, 0.02, 0.98);
    float lineW = 1.0 / max(uContentSize.x, 1.0);
    if (abs(local.x - spx) < lineW * 0.5) {
        oColor = vec4(1.0, 1.0, 1.0, 1.0);
        return;
    }

    vec4 cL = sampleContain(uVideo, local, uContentSize, uVideoSize);
    vec4 cR = sampleContain(uVideoR, local, uContentSize, uVideoSizeR);
    oColor = local.x < spx ? cL : cR;
}
