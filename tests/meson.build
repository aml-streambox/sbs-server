if get_option('tests')
    test_deps = [sbs_common_dep, glib_dep, gio_dep, jpeg_dep]
    if m_dep.found()
        test_deps += [m_dep]
    endif

    test_uuid = executable('test-uuid',
        'test_uuid.c',
        include_directories : [inc, gen_inc],
        dependencies : test_deps,
    )
    test('uuid', test_uuid)

    test_log = executable('test-log',
        'test_log.c',
        include_directories : [inc, gen_inc],
        dependencies : test_deps,
    )
    test('log', test_log)

    test_time = executable('test-time',
        'test_time.c',
        include_directories : [inc, gen_inc],
        dependencies : test_deps,
    )
    test('time-utils', test_time)

    test_ipc = executable('test-ipc',
        'test_ipc.c',
        include_directories : [inc, gen_inc],
        dependencies : test_deps,
    )
    test('ipc-protocol', test_ipc)

    test_spsc = executable('test-spsc',
        'test_spsc.c',
        include_directories : [inc, gen_inc],
        dependencies : test_deps,
    )
    test('spsc-queue', test_spsc)

    test_frame_slot = executable('test-frame-slot',
        'test_frame_slot.c',
        '../src/server/engine/frame_slot.c',
        include_directories : [inc, gen_inc],
        dependencies : test_deps,
    )
    test('frame-slot', test_frame_slot)

    test_ipc_transport = executable('test-ipc-transport',
        'test_ipc_transport.c',
        include_directories : [inc, gen_inc],
        dependencies : test_deps,
    )
    test('ipc-transport-layer', test_ipc_transport)

    test_scene_graph = executable('test-scene-graph',
        'test_scene_graph.c',
        '../src/server/engine/scene_graph.c',
        '../src/server/engine/compositor_scene.c',
        '../src/server/engine/frame_slot.c',
        include_directories : [inc, gen_inc],
        dependencies : test_deps,
    )
    test('scene-graph', test_scene_graph)

    api_test_sources = [
        'test_api_stubs.c',
        '../src/server/engine/scene_graph.c',
        '../src/server/engine/audio_mixer.c',
        '../src/server/engine/compositor_scene.c',
        '../src/server/engine/frame_slot.c',
        '../src/server/engine/preview.c',
        '../src/server/engine/snapshot.c',
        '../src/server/instance_manager.c',
        '../src/server/config/config_manager.c',
        '../src/server/config/source_start_config.c',
        '../src/server/api/ws_server.c',
        '../src/server/api/telemetry_utils.c',
        '../src/server/api/v4l2_discovery.c',
        '../src/server/api/jsonrpc.c',
        '../src/server/api/pubsub.c',
        '../src/server/api/handlers/command.c',
        '../src/server/api/handlers/audio.c',
        '../src/server/api/handlers/config.c',
        '../src/server/api/handlers/preview.c',
        '../src/server/api/handlers/snapshot.c',
        '../src/server/api/handlers/system.c',
        '../src/server/api/handlers/source.c',
        '../src/server/api/handlers/scene.c',
        '../src/server/api/handlers/output.c',
        '../src/server/api/handlers/canvas.c',
        '../src/server/api/handlers/encoder.c',
        '../src/server/api/handlers/pubsub.c',
        '../src/server/api/handlers/instance.c',
    ]

    test_jsonrpc = executable('test-jsonrpc',
        'test_jsonrpc.c',
        api_test_sources,
        include_directories : [inc, gen_inc],
        dependencies : test_deps,
    )
    test('jsonrpc', test_jsonrpc)

    test_api_engine = executable('test-api-engine',
        'test_api_engine.c',
        api_test_sources,
        include_directories : [inc, gen_inc],
        dependencies : test_deps,
    )
    test('api-engine', test_api_engine)

    test_config_restore = executable('test-config-restore',
        'test_config_restore.c',
        api_test_sources,
        include_directories : [inc, gen_inc],
        dependencies : test_deps,
    )
    test('config-restore', test_config_restore)

    # Integration test -- requires Vulkan GPU (Mali-G52 on target, any on host)
    # Not run by default in CI; use `meson test -C build --suite integration`
    integ_vulkan = executable('integ-vulkan',
        'integration/integ_vulkan.c',
        '../src/server/engine/compositor.c',
        '../src/server/engine/compositor_scene.c',
        '../src/server/engine/compositor_thread.c',
        '../src/server/engine/export_dest.c',
        '../src/server/engine/frame_slot.c',
        'test_ge2d_stubs.c',
        include_directories : [inc, gen_inc],
        dependencies : [sbs_common_dep, glib_dep, vulkan_dep, m_dep],
    )
    test('integ-vulkan', integ_vulkan,
         suite : 'integration',
         is_parallel : false,
         timeout : 60,
    )

    # Integration test -- IPC frame delivery pipeline
    # Tests: socketpair, send/recv with SCM_RIGHTS, frame slot, fork'd mock worker
    integ_ipc = executable('integ-ipc',
        'integration/integ_ipc.c',
        '../src/server/engine/frame_slot.c',
        include_directories : [inc, gen_inc],
        dependencies : [sbs_common_dep, glib_dep],
    )
    test('integ-ipc', integ_ipc,
         suite : 'integration',
         is_parallel : false,
         timeout : 30,
    )

    # Integration test -- end-to-end pipeline (compositor + frame distribution)
    # Tests compositor lifecycle, frame export, IPC distribution, fd leaks
    integ_pipeline = executable('integ-pipeline',
        'integration/integ_pipeline.c',
        '../src/server/engine/compositor.c',
        '../src/server/engine/compositor_scene.c',
        '../src/server/engine/compositor_thread.c',
        '../src/server/engine/export_dest.c',
        '../src/server/engine/frame_slot.c',
        'test_ge2d_stubs.c',
        include_directories : [inc, gen_inc],
        dependencies : [sbs_common_dep, glib_dep, vulkan_dep, m_dep],
    )
    test('integ-pipeline', integ_pipeline,
         suite : 'integration',
         is_parallel : false,
         timeout : 60,
    )
endif
