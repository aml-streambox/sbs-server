# sbs-worker requires GStreamer and cJSON for pipeline construction and config parsing.
# If they're not available (e.g., server-only host build), we build a minimal stub.

have_worker_deps = gst_dep.found() and gst_app_dep.found() and gst_video_dep.found() and gst_alloc_dep.found() and cjson_dep.found() and pangocairo_dep.found() and fontconfig_dep.found()

# libvfmcap is optional — only required for vfmcap direct capture path
vfmcap_dep = dependency('libvfmcap', required : false)
if not vfmcap_dep.found()
    vfmcap_dep = cc.find_library('vfmcap', required : false)
endif

if have_worker_deps
    worker_sources = files(
        'main.c',
        'worker_common.c',
        'source_worker.c',
        'output_worker.c',
    )

    worker_deps = [
        sbs_common_dep,
        glib_dep,
        gst_dep,
        gst_app_dep,
        gst_video_dep,
        gst_alloc_dep,
        pangocairo_dep,
        fontconfig_dep,
        vulkan_dep,
        cjson_dep,
    ]

    if vfmcap_dep.found()
        worker_deps += [vfmcap_dep]
        worker_c_args = ['-DSBS_HAVE_GSTREAMER=1', '-DSBS_HAVE_VFMCAP=1']
    else
        worker_c_args = ['-DSBS_HAVE_GSTREAMER=1']
    endif

    executable('sbs-worker',
        worker_sources,
        include_directories : [inc, gen_inc],
        dependencies : worker_deps,
        c_args : worker_c_args,
        install : true,
        install_dir : get_option('bindir'),
    )
else
    # Stub worker binary for builds without GStreamer (server-only development).
    warning('GStreamer or cJSON not found — building stub sbs-worker')
    executable('sbs-worker',
        files('main.c'),
        include_directories : [inc, gen_inc],
        dependencies : [
            sbs_common_dep,
            glib_dep,
        ],
        install : true,
        install_dir : get_option('bindir'),
    )
endif
