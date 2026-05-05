project('sbs', ['c'],
    version : '0.0.1',
    license : 'GPL-2.0-or-later',
    default_options : [
        'c_std=c11',
        'warning_level=2',
        'b_ndebug=if-release',
        'buildtype=debugoptimized',
    ],
    meson_version : '>= 0.60',
)

# ── Version header ────────────────────────────────────────────
version_data = configuration_data()
version_data.set('SBS_VERSION_MAJOR', 0)
version_data.set('SBS_VERSION_MINOR', 0)
version_data.set('SBS_VERSION_PATCH', 1)
version_data.set_quoted('SBS_VERSION_STRING', meson.project_version())
version_data.set_quoted('SBS_BUILD_DATE',
    run_command('date', '-u', '+%Y-%m-%dT%H:%M:%SZ', check: true).stdout().strip(),
)
configure_file(
    input  : 'src/include/sbs_version.h.in',
    output : 'sbs_version.h',
    configuration : version_data,
)

# ── Build options ─────────────────────────────────────────────
platform = get_option('platform')

# ── Dependencies ──────────────────────────────────────────────
glib_dep = dependency('glib-2.0', version : '>= 2.56')
gio_dep = dependency('gio-2.0', version : '>= 2.56')

# GStreamer — required for the worker binary (source/output pipelines).
# Declared required:false at project level so server-only builds work on hosts
# without GStreamer dev packages. Worker meson.build will error if missing.
gst_dep        = dependency('gstreamer-1.0',           required : false)
gst_app_dep    = dependency('gstreamer-app-1.0',       required : false)
gst_audio_dep  = dependency('gstreamer-audio-1.0',     required : false)
gst_video_dep  = dependency('gstreamer-video-1.0',     required : false)
gst_alloc_dep  = dependency('gstreamer-allocators-1.0', required : false)
gst_webrtc_dep = dependency('gstreamer-webrtc-1.0',    required : false)
gst_sdp_dep    = dependency('gstreamer-sdp-1.0',       required : false)
gst_rtp_dep    = dependency('gstreamer-rtp-1.0',       required : false)
pangocairo_dep = dependency('pangocairo',              required : false)
fontconfig_dep = dependency('fontconfig',              required : false)
vulkan_dep     = dependency('vulkan',                   required : true)
lws_dep        = dependency('libwebsockets',            required : false)
cjson_dep      = dependency('libcjson',                 required : false)
jpeg_dep       = dependency('libjpeg',              required : false)
png_dep        = dependency('libpng',               required : false)
udev_dep       = dependency('libudev',              required : false)
alsa_dep       = dependency('alsa',                 required : false)
systemd_dep    = dependency('libsystemd',           required : false)
srt_dep        = dependency('srt',                  required : false)

# ── Compiler flags ────────────────────────────────────────────
cc = meson.get_compiler('c')
m_dep = cc.find_library('m', required : false)
dl_dep = cc.find_library('dl', required : false)
ion_dep = cc.find_library('ion', required : false)
ge2d_dep = cc.find_library('ge2d', required : false)
common_flags = [
    '-D_GNU_SOURCE',
    '-DSBS_PLATFORM_@0@'.format(platform.to_upper()),
]

if alsa_dep.found()
    common_flags += ['-DSBS_HAVE_ALSA']
endif
if systemd_dep.found()
    common_flags += ['-DSBS_HAVE_SYSTEMD']
endif

add_project_arguments(common_flags, language : 'c')

# ── Include directories ──────────────────────────────────────
inc = include_directories('src/include')
# Generated headers (sbs_version.h) land in the build root.
# configure_file() output dir is the current build dir (project root → build root).
# We reference it via '.' so Meson resolves both source and build dirs.
gen_inc = include_directories('.')

# ── Subdirectories ────────────────────────────────────────────
subdir('shaders')
subdir('src/common')
subdir('src/server')
subdir('src/worker')
subdir('src/cli')

if get_option('tests')
    subdir('tests')
endif
