glslang = find_program('glslangValidator', native: true, required: false)
fs = import('fs')

shader_spv_files = [
    'composite.vert.spv',
    'composite.frag.spv',
    'yuv_to_rgba.comp.spv',
    'rgba_to_nv21.comp.spv',
    'export_nv21.comp.spv',
    'fullscreen.vert.spv',
    'nv21_subpass.frag.spv',
    'p010_subpass.frag.spv',
    'p010_export.comp.spv',
    'native_yuv_sdr.comp.spv',
    'native_yuv_hdr.comp.spv',
    'native_p010_direct.comp.spv',
    'native_p010_to_nv21.comp.spv',
    'native_yuv8_to_p010.comp.spv',
    'native_amly_to_p010.comp.spv',
    'native_amly_to_p010_src.comp.spv',
    'native_yuv8_to_nv21.comp.spv',
    'native_amly_to_nv21.comp.spv',
    'native_amly_to_nv21_src.comp.spv',
    'native_downscale_nv21.comp.spv',
    'native_downscale_p010.comp.spv',
]

if glslang.found() and get_option('shader_compile')
    custom_target('composite.vert.spv',
        input  : 'composite.vert',
        output : 'composite.vert.spv',
        command : [glslang, '-V', '@INPUT@', '-o', '@OUTPUT@'],
        install : true,
        install_dir : get_option('datadir') / 'sbs' / 'shaders',
    )

    custom_target('composite.frag.spv',
        input  : 'composite.frag',
        output : 'composite.frag.spv',
        command : [glslang, '-V', '@INPUT@', '-o', '@OUTPUT@'],
        install : true,
        install_dir : get_option('datadir') / 'sbs' / 'shaders',
    )

    custom_target('yuv_to_rgba.comp.spv',
        input  : 'yuv_to_rgba.comp',
        output : 'yuv_to_rgba.comp.spv',
        command : [glslang, '-V', '@INPUT@', '-o', '@OUTPUT@'],
        install : true,
        install_dir : get_option('datadir') / 'sbs' / 'shaders',
    )

    custom_target('rgba_to_nv21.comp.spv',
        input  : 'rgba_to_nv21.comp',
        output : 'rgba_to_nv21.comp.spv',
        command : [glslang, '-V', '@INPUT@', '-o', '@OUTPUT@'],
        install : true,
        install_dir : get_option('datadir') / 'sbs' / 'shaders',
    )

    custom_target('export_nv21.comp.spv',
        input  : 'export_nv21.comp',
        output : 'export_nv21.comp.spv',
        command : [glslang, '-V', '@INPUT@', '-o', '@OUTPUT@'],
        install : true,
        install_dir : get_option('datadir') / 'sbs' / 'shaders',
    )

    custom_target('fullscreen.vert.spv',
        input  : 'fullscreen.vert',
        output : 'fullscreen.vert.spv',
        command : [glslang, '-V', '@INPUT@', '-o', '@OUTPUT@'],
        install : true,
        install_dir : get_option('datadir') / 'sbs' / 'shaders',
    )

    custom_target('nv21_subpass.frag.spv',
        input  : 'nv21_subpass.frag',
        output : 'nv21_subpass.frag.spv',
        command : [glslang, '-V', '@INPUT@', '-o', '@OUTPUT@'],
        install : true,
        install_dir : get_option('datadir') / 'sbs' / 'shaders',
    )

    custom_target('p010_subpass.frag.spv',
        input  : 'p010_subpass.frag',
        output : 'p010_subpass.frag.spv',
        command : [glslang, '-V', '@INPUT@', '-o', '@OUTPUT@'],
        install : true,
        install_dir : get_option('datadir') / 'sbs' / 'shaders',
    )

    custom_target('p010_export.comp.spv',
        input  : 'p010_export.comp',
        output : 'p010_export.comp.spv',
        command : [glslang, '-V', '@INPUT@', '-o', '@OUTPUT@'],
        install : true,
        install_dir : get_option('datadir') / 'sbs' / 'shaders',
    )

    custom_target('native_yuv_sdr.comp.spv',
        input  : 'native_yuv_sdr.comp',
        output : 'native_yuv_sdr.comp.spv',
        command : [glslang, '-V', '@INPUT@', '-o', '@OUTPUT@'],
        install : true,
        install_dir : get_option('datadir') / 'sbs' / 'shaders',
    )

    custom_target('native_yuv_hdr.comp.spv',
        input  : 'native_yuv_hdr.comp',
        output : 'native_yuv_hdr.comp.spv',
        command : [glslang, '-V', '@INPUT@', '-o', '@OUTPUT@'],
        install : true,
        install_dir : get_option('datadir') / 'sbs' / 'shaders',
    )

    custom_target('native_p010_direct.comp.spv',
        input  : 'native_p010_direct.comp',
        output : 'native_p010_direct.comp.spv',
        command : [glslang, '-V', '@INPUT@', '-o', '@OUTPUT@'],
        install : true,
        install_dir : get_option('datadir') / 'sbs' / 'shaders',
    )

    custom_target('native_p010_to_nv21.comp.spv',
        input  : 'native_p010_to_nv21.comp',
        output : 'native_p010_to_nv21.comp.spv',
        command : [glslang, '-V', '@INPUT@', '-o', '@OUTPUT@'],
        install : true,
        install_dir : get_option('datadir') / 'sbs' / 'shaders',
    )

    custom_target('native_yuv8_to_p010.comp.spv',
        input  : 'native_yuv8_to_p010.comp',
        output : 'native_yuv8_to_p010.comp.spv',
        command : [glslang, '-V', '@INPUT@', '-o', '@OUTPUT@'],
        install : true,
        install_dir : get_option('datadir') / 'sbs' / 'shaders',
    )

    custom_target('native_amly_to_p010.comp.spv',
        input  : 'native_amly_to_p010.comp',
        output : 'native_amly_to_p010.comp.spv',
        command : [glslang, '-V', '@INPUT@', '-o', '@OUTPUT@'],
        install : true,
        install_dir : get_option('datadir') / 'sbs' / 'shaders',
    )

    custom_target('native_amly_to_p010_src.comp.spv',
        input  : 'native_amly_to_p010_src.comp',
        output : 'native_amly_to_p010_src.comp.spv',
        command : [glslang, '-V', '@INPUT@', '-o', '@OUTPUT@'],
        install : true,
        install_dir : get_option('datadir') / 'sbs' / 'shaders',
    )

    custom_target('native_yuv8_to_nv21.comp.spv',
        input  : 'native_yuv8_to_nv21.comp',
        output : 'native_yuv8_to_nv21.comp.spv',
        command : [glslang, '-V', '@INPUT@', '-o', '@OUTPUT@'],
        install : true,
        install_dir : get_option('datadir') / 'sbs' / 'shaders',
    )

    custom_target('native_amly_to_nv21.comp.spv',
        input  : 'native_amly_to_nv21.comp',
        output : 'native_amly_to_nv21.comp.spv',
        command : [glslang, '-V', '@INPUT@', '-o', '@OUTPUT@'],
        install : true,
        install_dir : get_option('datadir') / 'sbs' / 'shaders',
    )

    custom_target('native_amly_to_nv21_src.comp.spv',
        input  : 'native_amly_to_nv21_src.comp',
        output : 'native_amly_to_nv21_src.comp.spv',
        command : [glslang, '-V', '@INPUT@', '-o', '@OUTPUT@'],
        install : true,
        install_dir : get_option('datadir') / 'sbs' / 'shaders',
    )

    custom_target('native_downscale_nv21.comp.spv',
        input  : 'native_downscale_nv21.comp',
        output : 'native_downscale_nv21.comp.spv',
        command : [glslang, '-V', '@INPUT@', '-o', '@OUTPUT@'],
        install : true,
        install_dir : get_option('datadir') / 'sbs' / 'shaders',
    )

    custom_target('native_downscale_p010.comp.spv',
        input  : 'native_downscale_p010.comp',
        output : 'native_downscale_p010.comp.spv',
        command : [glslang, '-V', '@INPUT@', '-o', '@OUTPUT@'],
        install : true,
        install_dir : get_option('datadir') / 'sbs' / 'shaders',
    )
else
    existing_shader_spv_files = []
    foreach shader_spv : shader_spv_files
        if fs.exists(shader_spv)
            existing_shader_spv_files += shader_spv
        endif
    endforeach
    if existing_shader_spv_files.length() > 0
        install_data(
            existing_shader_spv_files,
            install_dir : get_option('datadir') / 'sbs' / 'shaders',
        )
    else
        warning('shader_compile=false and no precompiled shader .spv files found; skipping shader install')
    endif
endif
