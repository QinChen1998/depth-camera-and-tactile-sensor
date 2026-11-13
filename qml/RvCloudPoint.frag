VARYING vec2 pointOffset;
VARYING vec4 pointColor;

void MAIN()
{
    if (dot(pointOffset, pointOffset) > 1.0)
        discard;
    FRAGCOLOR = pointColor;
}
