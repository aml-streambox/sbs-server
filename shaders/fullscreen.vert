#version 450

/* Fullscreen triangle — 3 vertices, no VBO.
 * gl_VertexIndex 0 → (-1,-1), 1 → (3,-1), 2 → (-1,3)
 * Covers the entire clip-space quad with a single triangle. */
void main()
{
    vec2 pos = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);
}
