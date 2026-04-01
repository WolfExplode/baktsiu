#version 330

uniform sampler2D uVideo;
uniform vec2 uWindowSize;
uniform vec2 uVideoSize;

in vec2 vUV;
out vec4 oColor;

void main()
{
    float winAspect = uWindowSize.x / max(uWindowSize.y, 1.0);
    float vidAspect = uVideoSize.x / max(uVideoSize.y, 1.0);
    vec2 uv = vUV;
    if (winAspect > vidAspect) {
        float scale = vidAspect / winAspect;
        uv.x = (uv.x - 0.5) * scale + 0.5;
    } else {
        float scale = winAspect / vidAspect;
        uv.y = (uv.y - 0.5) * scale + 0.5;
    }
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {
        oColor = vec4(0.12, 0.14, 0.15, 1.0);
    } else {
        oColor = texture(uVideo, vec2(uv.x, 1.0 - uv.y));
    }
}
