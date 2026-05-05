#version 450

/* OPT-6: Mali G52 — vertex shader keeps highp for transform (needs full
 * float32 precision for matrix multiply), but outputs mediump UVs since
 * interpolated texture coordinates are fine at fp16 precision. */

vec2 positions[6] = vec2[](
    vec2(0.0, 0.0),
    vec2(1.0, 0.0),
    vec2(0.0, 1.0),
    vec2(0.0, 1.0),
    vec2(1.0, 0.0),
    vec2(1.0, 1.0)
);

vec2 uvs[6] = vec2[](
    vec2(0.0, 0.0),
    vec2(1.0, 0.0),
    vec2(0.0, 1.0),
    vec2(0.0, 1.0),
    vec2(1.0, 0.0),
    vec2(1.0, 1.0)
);

/* Push constants — must match sbs_push_constants_t exactly (176 bytes) */
layout(push_constant) uniform PushConstants {
    mat4 transform;         /*   0: 64 bytes */
    vec4 crop;              /*  64: 16 bytes */
    float opacity;          /*  80:  4 bytes */
    float pad0;             /*  84:  4 bytes */
    vec2 source_size;       /*  88:  8 bytes */
    vec4 tint;              /*  96: 16 bytes */
    uint filter_flags;      /* 112:  4 bytes */
    float filter_pad[3];    /* 116: 12 bytes */
    vec4 filter_params_a;   /* 128: 16 bytes */
    vec4 filter_params_b;   /* 144: 16 bytes */
    uint has_texture;       /* 160:  4 bytes */
    uint _pad1[3];          /* 164: 12 bytes → total 176 */
} pc;

layout(location = 0) out mediump vec2 frag_uv;

void main()
{
    vec2 pos = positions[gl_VertexIndex];
    frag_uv = vec2(
        mix(pc.crop.x, 1.0 - pc.crop.z, pos.x),
        mix(pc.crop.y, 1.0 - pc.crop.w, pos.y)
    );
    gl_Position = pc.transform * vec4(pos, 0.0, 1.0);
}
