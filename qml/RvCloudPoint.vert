VARYING vec2 pointOffset;
VARYING vec4 pointColor;

void MAIN()
{
    pointOffset = UV0 * 2.0 - vec2(1.0);
    pointColor = INSTANCE_COLOR;
    vec4 center = INSTANCE_MODELVIEWPROJECTION_MATRIX * vec4(0.0, 0.0, 0.0, 1.0);
    // Expand around the projected centre. The diameter is in framebuffer pixels.
    center.xy += pointOffset * diameterToClip * center.w;
    POSITION = center;
}
