uniform sampler2D tex;
uniform sampler2D worldPalette;
uniform sampler2D displayPalette;
uniform sampler2D refTex;
uniform int useRef;
uniform sampler2D blendTex;
uniform int useBlend;
in vec4 f_color;
in vec2 f_uv;
out vec4 fragColor;

void main(void)
{
    vec4 sampled = texture(tex, f_uv);
    vec4 sampled_color = vec4(0.0, 0.0, 0.0, 0.0);
    vec4 vertex_color = f_color;
    float index = sampled.r;
    vec4 palval = texture(worldPalette, vec2(index, 0.5));
    vec4 palvald = texture(displayPalette, vec2(index, 0.5));
    vec4 blend = vec4(1.0, 1.0, 1.0, 1.0);

    float transparency = 1.0;
    if (index == 0.0)
        discard;
    // DroidWorks hardware views: a GUI pixel still equal to the quantized view pixel
    // the view composited (refTex) is shown from the full-resolution view layer
    // underneath; anything drawn over it since (markers, dialogs, tints) differs.
    if (useRef != 0)
    {
        float ref = texture(refTex, f_uv).r;
        if (ref != 0.0 && ref == index)
            discard;
    }
    // DroidWorks hardware mission scene: a GUI pixel still equal to its blend-mask entry
    // was colormap-blended onto the scene hole by the 8-bit code (dimmed inventory icons,
    // translucent panels, font edges): draw it at half alpha over the GL scene beneath.
    if (useBlend != 0)
    {
        float m = texture(blendTex, f_uv).r;
        if (m != 0.0 && m == index)
            transparency = 0.5;
    }
    sampled_color = vec4(palvald.r, palvald.g, palvald.b, transparency);

    fragColor = sampled_color * vertex_color * blend;
}
