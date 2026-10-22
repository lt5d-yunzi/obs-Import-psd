#define QT_FEATURE_android  -1
#define QT_FEATURE_win32  -1
#define QT_FEATURE_darwin  1
#define QT_FEATURE_accessibility  1
#define QT_FEATURE_animation  1
#define QT_FEATURE_dbus -1
#define QT_FEATURE_sharedmemory  1
#define QT_FEATURE_systemsemaphore  1
#define QT_FEATURE_futures  1
#define QT_FEATURE_concurrent  1
#define QT_FEATURE_opengl  1
#define QT_FEATURE_opengles2 -1
#define QT_FEATURE_vulkan  1
#define QT_FEATURE_metal  1
#define QT_FEATURE_imageformat_png 1
#define QT_FEATURE_imageformat_jpeg 1
#define QT_FEATURE_styles 1
#define QT_FEATURE_style_fusion 1
#define QT_FEATURE_style_mac 1
#define QT_FEATURE_textcodec 1
#define QT_FEATURE_thread 1
#define QT_FEATURE_shortcut 1
#define QT_FEATURE_wheelevent 1
#define QT_FEATURE_tabletevent 1
#define QT_FEATURE_clipboard 1
#define QT_FEATURE_draganddrop 1
#define QT_FEATURE_action 1
#define QT_FEATURE_validator 1
#define QT_FEATURE_imgui -1

/* Qt version the vendored headers were taken from (qtbase v6.9.0).
   The installed OBS ships Qt 6.11, which is ABI compatible for the public API. */
#define QT_VERSION_MAJOR 6
#define QT_VERSION_MINOR 9
#define QT_VERSION_PATCH 0

/* Deprecation levels baked into the header snapshot. */
#define QT_DISABLE_DEPRECATED_UP_TO QT_VERSION_CHECK(6, 0, 0)
#define QT_WARN_DEPRECATED_UP_TO QT_VERSION_CHECK(6, 9, 0)
