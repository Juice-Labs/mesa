
inc_zink = include_directories('../../../drivers/zink')

libzinkwinsys = static_library(
  'zinkwinsys',
  files('zink_wgl_framebuffer.c'),
  include_directories : [
    inc_src,
    inc_wgl,
    inc_include,
    inc_gallium,
    inc_gallium_aux,
    inc_gallium_drivers,
    inc_zink,
    inc_vulkan_util,
    'D:/source/mesa/src/mapi/glapi/tracy'
  ],
  dependencies: [
    dep_vulkan, idep_nir_headers
  ],
  c_args : ['-DTRACY_ENABLE', '-DTRACY_ON_DEMAND', '-DTRACY_DELAYED_INIT'],
  cpp_args : ['-DTRACY_ENABLE', '-DTRACY_ON_DEMAND', '-DTRACY_DELAYED_INIT'],
  gnu_symbol_visibility : 'hidden',
)

winsys_zink_wgl = declare_dependency(
  link_with : [libzinkwinsys],
)
