#version 450

/* Keep compositor color/filter math highp on Mali G52.  The mediump/fp16
 * path can drop dynamic filter operations on sampled source colors. */

layout(location = 0) in mediump vec2 frag_uv;

layout(location = 0) out highp vec4 out_color;

/* Push constants — must match sbs_push_constants_t exactly (176 bytes) */
layout(push_constant) uniform PushConstants {
    mat4 transform;         /*   0: 64 bytes — kept highp (vertex stage) */
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

/* Source texture — binding 0 from the per-item descriptor set.
 * When has_texture == 0, this is bound to a 1x1 magenta placeholder. */
layout(set = 0, binding = 0) uniform mediump sampler2D source_tex;
layout(set = 0, binding = 1) uniform mediump sampler3D lut_tex;

highp vec3 rotate_hue(highp vec3 color, highp float degrees)
{
    highp float angle = radians(degrees);
    highp float s = sin(angle);
    highp float c = cos(angle);
    highp mat3 rgb_to_yiq = mat3(
        0.299, 0.596, 0.211,
        0.587, -0.274, -0.523,
        0.114, -0.322, 0.312);
    highp mat3 yiq_to_rgb = mat3(
        1.0, 1.0, 1.0,
        0.956, -0.272, -1.106,
        0.621, -0.647, 1.703);
    highp vec3 yiq = color * rgb_to_yiq;
    highp vec2 iq = vec2(c * yiq.y - s * yiq.z, s * yiq.y + c * yiq.z);
    return yiq_to_rgb * vec3(yiq.x, iq);
}

highp vec3 apply_color_correction(highp vec3 color)
{
    highp float gray = dot(color, vec3(0.299, 0.587, 0.114));
    highp float saturation = clamp(pc.filter_params_a.x, 0.0, 3.0);
    highp float brightness = clamp(pc.filter_params_a.y, -1.0, 1.0);
    highp float contrast = clamp(pc.filter_params_a.z, 0.0, 4.0);
    highp float gamma = clamp(pc.filter_params_a.w, 0.1, 4.0);
    highp float hue = clamp(pc.filter_params_b.w, -180.0, 180.0);
    color = vec3(gray) + (color - vec3(gray)) * saturation;
    color = ((color - 0.5) * contrast) + 0.5 + brightness;
    color = pow(clamp(color, vec3(0.0), vec3(1.0)), vec3(1.0 / gamma));
    if (abs(hue) > 0.001)
        color = rotate_hue(color, hue);
    return clamp(color, vec3(0.0), vec3(1.0));
}

highp float apply_luma_key_alpha(highp vec3 color)
{
    highp float luma = dot(color, vec3(0.299, 0.587, 0.114));
    highp float min_luma = clamp(pc.filter_params_a.w, 0.0, 1.0);
    highp float max_luma = clamp(pc.filter_params_b.z, min_luma, 1.0);
    highp float smoothness = clamp(pc.filter_params_b.w, 0.001, 1.0);
    highp float low = smoothstep(min_luma, min_luma + smoothness, luma);
    highp float high = 1.0 - smoothstep(max_luma - smoothness, max_luma, luma);
    return clamp(low * high, 0.0, 1.0);
}

highp float apply_chroma_key_alpha(inout highp vec3 color)
{
    highp vec3 key_color = clamp(pc.filter_params_a.rgb, vec3(0.0), vec3(1.0));
    highp float similarity = clamp(pc.filter_params_a.w, 0.0, 1.0);
    highp float smoothness = clamp(pc.filter_params_b.z, 0.001, 1.0);
    highp float spill = clamp(pc.filter_params_b.w, 0.0, 1.0);
    highp float dist = distance(color, key_color);
    highp float keep = smoothstep(similarity, similarity + smoothness, dist);
    highp float gray = dot(color, vec3(0.299, 0.587, 0.114));
    color = mix(color, vec3(gray), spill * (1.0 - keep));
    return keep;
}

void main()
{
    mediump vec2 uv = clamp(frag_uv, 0.0, 1.0);

    /* Branchless source selection: mix between tinted placeholder and
     * live texture based on has_texture (0 or 1). */
    highp float shade = 0.85 + 0.15 * (1.0 - uv.y);
    highp vec3 placeholder = pc.tint.rgb * shade;
    highp vec4 tex_color = texture(source_tex, uv);
    highp float has_tex = float(pc.has_texture);
    highp vec3 color = mix(placeholder, tex_color.rgb, has_tex);
    highp float alpha = pc.opacity * pc.tint.a * mix(1.0, tex_color.a, has_tex);

    highp float f_desat    = float((pc.filter_flags >> 0u) & 1u);
    highp float f_bright   = float((pc.filter_flags >> 1u) & 1u);
    highp float f_contrast = float((pc.filter_flags >> 2u) & 1u);
    highp float f_reduce   = float((pc.filter_flags >> 3u) & 1u);
    highp float f_warmth   = float((pc.filter_flags >> 4u) & 1u);
    highp float f_hdr_lut  = float((pc.filter_flags >> 5u) & 1u);
    highp float f_color    = float((pc.filter_flags >> 6u) & 1u);
    highp float f_luma_key = float((pc.filter_flags >> 7u) & 1u);
    highp float f_chroma_key = float((pc.filter_flags >> 8u) & 1u);
    highp float f_lut      = float((pc.filter_flags >> 9u) & 1u);

    if (f_desat > 0.5) {
        highp float gray = dot(color, vec3(0.299, 0.587, 0.114));
        highp float desat_amt = clamp(pc.filter_params_a.x, 0.0, 1.0);
        color = mix(color, vec3(gray), desat_amt);
    }

    if (f_bright > 0.5) {
        color = clamp(color + vec3(pc.filter_params_a.y), 0.0, 1.0);
    }

    if (f_contrast > 0.5) {
        color = ((color - 0.5) * pc.filter_params_a.z) + 0.5;
    }

    if (f_reduce > 0.5) {
        highp float reduce_gray = dot(color, vec3(0.3333));
        highp float reduce_amt = clamp(pc.filter_params_a.w, 0.0, 1.0) * 0.35;
        color = mix(color, vec3(reduce_gray), reduce_amt);
    }

    if (f_warmth > 0.5) {
        highp vec3 warmed = color * 1.15;
        highp float warmth_amt = clamp(pc.filter_params_b.x, 0.0, 1.0);
        color = mix(color, warmed, warmth_amt);
    }

    if (f_color > 0.5) {
        color = apply_color_correction(color);
    }

    if (f_hdr_lut > 0.5 || f_lut > 0.5) {
        highp vec3 lut_color = texture(lut_tex, clamp(color, 0.0, 1.0)).rgb;
        highp float lut_amt = clamp(pc.filter_params_b.y, 0.0, 1.0);
        color = mix(color, lut_color, lut_amt);
    }

    if (f_luma_key > 0.5) {
        alpha *= apply_luma_key_alpha(color);
    }

    if (f_chroma_key > 0.5) {
        alpha *= apply_chroma_key_alpha(color);
    }

    out_color = vec4(clamp(color, vec3(0.0), vec3(1.0)), alpha);
}
