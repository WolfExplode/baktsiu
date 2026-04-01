#version 330

uniform int uCompareMode;
uniform sampler2D uVideo;
uniform sampler2D uVideoR;
uniform vec2 uVideoSize;
uniform vec2 uVideoSizeR;
uniform vec2 uWindowSize;
uniform float uSplitPos;

in vec2 vUV;
out vec4 oColor;

vec4 sampleLetterbox(sampler2D tex, vec2 vCoord, vec2 panelPx, vec2 vidSize)
{
    float winAspect = panelPx.x / max(panelPx.y, 1.0);
    float vidAspect = vidSize.x / max(vidSize.y, 1.0);
    vec2 uv = vCoord;
    if (winAspect > vidAspect) {
        float scale = vidAspect / winAspect;
        uv.x = (uv.x - 0.5) * scale + 0.5;
    } else {
        float scale = winAspect / vidAspect;
        uv.y = (uv.y - 0.5) * scale + 0.5;
    }
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {
        return vec4(0.12, 0.14, 0.15, 1.0);
    }
    return texture(tex, vec2(uv.x, 1.0 - uv.y));
}

void main()
{
    if (uCompareMode == 0) {
        oColor = sampleLetterbox(uVideo, vUV, uWindowSize, uVideoSize);
        return;
    }

    float sp = clamp(uSplitPos, 0.02, 0.98);
    float lineW = 1.0 / max(uWindowSize.x, 1.0);
    if (abs(vUV.x - sp) < lineW * 0.5) {
        oColor = vec4(1.0, 1.0, 1.0, 1.0);
        return;
    }

    // Full-window letterbox for each video (same as single-video mode), then mask by divider —
    // avoids per-panel UV remap glitches at the split.
    vec4 cL = sampleLetterbox(uVideo, vUV, uWindowSize, uVideoSize);
    vec4 cR = sampleLetterbox(uVideoR, vUV, uWindowSize, uVideoSizeR);
    oColor = vUV.x < sp ? cL : cR;
}
