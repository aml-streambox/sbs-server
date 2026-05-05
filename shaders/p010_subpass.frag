#version 450

layout(input_attachment_index = 0, set = 0, binding = 0) uniform subpassInput u_color;
layout(std430, set = 0, binding = 1) buffer P010Output {
    uint data[];
} p010;

layout(push_constant) uniform PushConstants {
    uint width;
    uint height;
    uint y_stride;
    uint uv_stride;
    uint uv_offset;
} pc;

const vec3 kRgbToY  = vec3(0.2627, 0.6780, 0.0593);
const vec3 kRgbToCb = vec3(-0.1396, -0.3604, 0.5);
const vec3 kRgbToCr = vec3(0.5, -0.4598, -0.0402);

void write_u16(uint byte_addr, uint val) {
    uint word_idx = byte_addr >> 2u;
    uint byte_off = byte_addr & 2u;
    uint shift = byte_off * 8u;
    atomicOr(p010.data[word_idx], (val & 0xFFFFu) << shift);
}

void main() {
    ivec2 coord = ivec2(gl_FragCoord.xy);
    uint x = uint(coord.x);
    uint y = uint(coord.y);

    if (x >= pc.width || y >= pc.height)
        return;

    vec4 rgba = subpassLoad(u_color);

    /* DEBUG: write test pattern to verify shader execution.
     * Y = 100 + x%16, UV = (200, 300) to make it obvious if shader runs. */
    uint y_byte = (y * pc.y_stride + x) * 2u;
    uint y10 = 100u + (x % 16u);
    write_u16(y_byte, y10);

    if ((x & 1u) == 0u && (y & 1u) == 0u) {
        uint uv_row = y >> 1u;
        uint uv_col = x >> 1u;
        uint uv_byte = pc.uv_offset + (uv_row * pc.uv_stride + uv_col * 2u) * 2u;
        write_u16(uv_byte, 200u);
        write_u16(uv_byte + 2u, 300u);
    }
}
