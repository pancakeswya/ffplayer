project('ff_player', 'c', version : '1.0.0', default_options : ['c_std=c11', 'default_library=static'])

include_dirs = [include_directories('include')]
sources = files(
  'include/ff_clock.h',
  'src/ff_clock.c',
  'include/ff_decoder.h',
  'src/ff_decoder.c',
  'include/ff_frame.h',
  'include/ff_frame_queue.h',
  'src/ff_frame_queue.c',
  'include/ff_packet_queue.h',
  'src/ff_packet_queue.c',
  'include/ff_player.h',
  'src/ff_player.c'
)

deps = [
  dependency('libavcodec'),
  dependency('libavdevice'),
  dependency('libavfilter'),
  dependency('libavformat'),
  dependency('libavutil'),
  dependency('libswresample'),
  dependency('libswscale')
]

if not meson.get_compiler('c').has_header('thread.h')
  sources += files(
    'third_party/tinycthread/tinycthread.h',
    'third_party/tinycthread/tinycthread.c'
  )
  include_dirs += include_directories('third_party')
endif

ff_player_lib = library(meson.project_name(), sources, dependencies: deps, include_directories: include_dirs)
ff_player_dep = declare_dependency(link_with : ff_player_lib, dependencies: deps, include_directories : include_dirs)

if get_option('build_examples')
  subdir('examples')
endif
