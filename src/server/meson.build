# Platform identifier string
if platform == 'amlogic'
    platform_define = '-DSBS_PLATFORM="amlogic"'
else
    platform_define = '-DSBS_PLATFORM="generic"'
endif

server_sources = files(
    'main.c',
    'instance_manager.c',
)

subdir('engine')
subdir('supervisor')
subdir('config')
subdir('api')

server_deps = [
    sbs_common_dep,
    glib_dep,
    gio_dep,
    vulkan_dep,
    jpeg_dep,
]

if m_dep.found()
    server_deps += [m_dep]
endif

if dl_dep.found()
    server_deps += [dl_dep]
endif

server_c_args = [platform_define]

if gst_dep.found() and gst_app_dep.found() and gst_webrtc_dep.found() and gst_sdp_dep.found() and gst_rtp_dep.found()
    server_deps += [gst_dep, gst_app_dep, gst_webrtc_dep, gst_sdp_dep, gst_rtp_dep]
    server_c_args += ['-DSBS_HAVE_GSTREAMER_PREVIEW=1']
endif

if gst_video_dep.found()
    server_deps += [gst_video_dep]
endif

if gst_alloc_dep.found()
    server_deps += [gst_alloc_dep]
endif

if gst_dep.found() and gst_app_dep.found() and gst_audio_dep.found()
    server_deps += [gst_dep, gst_app_dep, gst_audio_dep]
    server_c_args += ['-DSBS_HAVE_GSTREAMER_AUDIO=1']
endif

if lws_dep.found()
    server_deps += [lws_dep]
endif

if platform == 'amlogic' and ge2d_dep.found()
    if ion_dep.found()
        server_deps += [ion_dep]
    endif
    server_deps += [ge2d_dep]
    server_c_args += ['-DSBS_HAVE_GE2D=1']
endif

if systemd_dep.found()
    server_deps += [systemd_dep]
endif

if srt_dep.found()
    server_deps += [srt_dep]
else
    error('libsrt dependency is required for SRT outputs')
endif

if not get_option('tests')
    executable('sbs-server',
        server_sources + engine_sources + supervisor_sources + config_sources + api_sources,
        include_directories : [inc, gen_inc],
        dependencies : server_deps,
        c_args : server_c_args,
        link_args : ['-rdynamic'],
        install : true,
        install_dir : get_option('bindir'),
    )
endif
