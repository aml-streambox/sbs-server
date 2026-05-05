executable('sbs-cli',
    files('main.c'),
    include_directories : [inc, gen_inc],
    dependencies : [
        sbs_common_dep,
        glib_dep,
    ],
    install : true,
    install_dir : get_option('bindir'),
)
