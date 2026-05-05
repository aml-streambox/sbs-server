sbs_common_sources = files(
    'cjson_stub.c',
    'dmabuf_alloc.c',
    'uuid.c',
    'log.c',
    'ipc_proto.c',
    'ipc_transport.c',
    'time_utils.c',
    'spsc_queue.c',
)

sbs_common_deps = [glib_dep]
if systemd_dep.found()
    sbs_common_deps += [systemd_dep]
endif

sbs_common_lib = static_library('sbs-common',
    sbs_common_sources,
    include_directories : [inc, gen_inc],
    dependencies : sbs_common_deps,
)

sbs_common_dep = declare_dependency(
    link_with : sbs_common_lib,
    include_directories : [inc, gen_inc],
)
