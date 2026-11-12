// A quad over x in [-1, -0.375] of an 8-pixel-wide target: its right edge crosses pixel 2 at its middle.
[shader("vertex")]
float4 vertexMain(uint vertex_index : SV_VertexID) : SV_Position
{
    const uint corner = vertex_index < 3 ? vertex_index : 6 - vertex_index;
    return float4((corner & 1) != 0 ? -0.375f : -1.0f, (corner & 2) != 0 ? 1.0f : -1.0f, 0.5f, 1.0f);
}

[shader("fragment")]
float4 fragmentMain() : SV_Target0
{
    return float4(1.0f, 1.0f, 1.0f, 1.0f);
}
