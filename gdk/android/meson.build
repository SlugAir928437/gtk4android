gdk_android_sources = files([
  'gdkandroidinit.c',
  'gdkandroiddisplay.c',
  'gdkandroidmonitor.c',
  'gdkandroidevents.c',
  'gdkandroidsurface.c',
  'gdkandroidtoplevel.c',
  'gdkandroidpopup.c',
  'gdkandroidchoreographersource.c',
  'gdkandroidcairocontext.c',
  'gdkandroidclipboard.c',
  'gdkandroiddnd.c',
  'gdkandroidglcontext.c',
  'gdkandroidseat.c',
  'gdkandroiddevice.c',
  'gdkandroidkeymap.c',

  'gdkandroidcontentfile.c',

  'gdkandroidutils.c'
])

gdk_android_public_headers = files([
  'gdkandroidinit.h',
  'gdkandroiddisplay.h',
  'gdkandroidmonitor.h',
  'gdkandroidsurface.h',
  'gdkandroidtoplevel.h',
  'gdkandroidpopup.h',
  'gdkandroidseat.h',
  'gdkandroiddevice.h'
])

install_headers(gdk_android_public_headers, 'gdkandroid.h', subdir: 'gtk-4.0/gdk/android/')

if not cc.has_header('jni.h')
  error('the android backend requires a properly configured ndk')
endif

gdk_android_deps = [
  #dependency('jni'),
  cc.find_library('android'),
  cc.find_library('jnigraphics'),
  # NOTE: libnativewindow only exists at API 26+ and we target android-24
  # (minSdk).  All ANativeWindow_* symbols used by this backend are part of
  # the long-standing libandroid ABI (API 9+) and link through libandroid.
  # The only API-26-only call (ANativeWindow_fromSurface) is replaced by a
  # JNI read of Surface.mNativeObject, so no nativewindow dependency remains.
]

android_runtime = get_option('android-runtime')
if android_runtime.allowed()
  gdk_android_sources += files(['gdkandroidruntime.c'])
  gdk_android_deps += cc.find_library('log')
endif

libgdk_android = static_library('gdk-android',
  sources: [ gdk_android_sources, gdk_gen_headers ],
  include_directories: [ confinc, gdkinc, ],
  c_args: [ libgdk_c_args, common_cflags, ],
  dependencies: [ gdk_deps, gdk_android_deps ],
  gnu_symbol_visibility: symbol_visibility,
)
