project('CMedian', 'cpp',
    default_options: ['buildtype=release', 'b_ndebug=if-release', 'cpp_std=c++11'],
    version: '1')

add_project_arguments('-mfpmath=sse', '-msse2', language : 'cpp')

sources = ['src/cmedian.cpp']

deps = [dependency('vapoursynth').partial_dependency(includes: true, compile_args: true)]

shared_module('cmedian',
              sources,
              dependencies: deps,
              install: true)
