project(
    'CMedian',
    'cpp',
    default_options : [
        'buildtype=release',
        'b_ndebug=if-release',
        'warning_level=3',
        'cpp_std=c++17',
    ],
    version : '2',
)

add_project_arguments('-mfpmath=sse', '-msse2', language : 'cpp')

if get_option('buildtype').startswith('debug')
    if meson.get_compiler('cpp').get_argument_syntax() == 'gcc'
        add_project_arguments('-Wconversion', language : 'cpp')
    endif
endif

py = import('python').find_installation(pure : false)

incdir = include_directories(
    run_command(py, '-c', 'import vapoursynth as vs; print(vs.get_include())', check : true).stdout().strip(),
)

shared_module(
    'cmedian',
    'src/cmedian.cpp',
    include_directories : incdir,
    gnu_symbol_visibility : 'hidden',
    install_dir : py.get_install_dir() / 'vapoursynth/plugins',
    install : true,
)
