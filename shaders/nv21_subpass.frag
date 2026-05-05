#version 450

/* NV21 subpass conversion — reads RGBA from tile memory via input attachment,
 * converts to NV21, writes to SSBO using atomicOr.
 *
 * This runs as subpass 1 of the composition render pass. The color attachment
 * from subpass 0 is accessed as an input attachment, which on tile-based GPUs
 * (Mali G52) reads directly from tile memory — zero external memory bandwidth.
 *
 * The SSBO layout is NV21: width*height Y bytes, then width*height/2 VU bytes.
 * We pack bytes into uint32 elements using atomicOr to handle the fact that
 * multiple fragment invocations may write to the same uint32 word.
 *
 * Push constants provide output dimensions (may differ from render target for
 * same-resolution case; for different-resolution preview, use compute path). */

layout(input_attachment_index = 0, set = 0, binding = 0) uniform subpassInput u_color;
layout(std430, set = 0, binding = 1) buffer NV21Output {
    uint data[];
} nv21;

layout(push_constant) uniform PushConstants {
    uint width;
    uint height;
} pc;

void main()
{
    ivec2 coord = ivec2(gl_FragCoord.xy);
    uint x = uint(coord.x);
    uint y = uint(coord.y);

    if (x >= pc.width || y >= pc.height)
        return;

    /* Read RGBA from tile memory (subpass input) */
    vec4 rgba = subpassLoad(u_color);

    /* BT.601 RGBA → YVU conversion (matching existing NV21 compute shader) */
    float R = rgba.r;
    float G = rgba.g;
    float B = rgba.b;

    /* Y plane */
    float Y = clamp(16.0 + 65.481 * R + 128.553 * G + 24.966 * B, 0.0, 255.0);
    uint y_byte = uint(Y);

    /* Write Y byte into SSBO — pack into uint32 at correct byte offset */
    uint y_offset = y * pc.width + x;
    uint y_word = y_offset >> 2;
    uint y_shift = (y_offset & 3u) << 3;
    atomicOr(nv21.data[y_word], y_byte << y_shift);

    /* UV plane — only write for even coordinates (2x2 subsampling) */
    if ((x & 1u) == 0u && (y & 1u) == 0u) {
        float V = clamp(128.0 + 112.0 * R - 93.786 * G - 18.214 * B, 0.0, 255.0);
        float U = clamp(128.0 - 37.797 * R - 74.203 * G + 112.0 * B, 0.0, 255.0);

        uint uv_base = pc.width * pc.height;
        uint uv_offset = uv_base + (y >> 1) * pc.width + x;
        /* NV21: V first, then U */
        uint v_word = uv_offset >> 2;
        uint v_shift = (uv_offset & 3u) << 3;
        uint u_word = (uv_offset + 1u) >> 2;
        uint u_shift = ((uv_offset + 1u) & 3u) << 3;

        atomicOr(nv21.data[v_word], uint(V) << v_shift);
        atomicOr(nv21.data[u_word], uint(U) << u_shift);
    }
}
