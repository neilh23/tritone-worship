/* Stand-in for the header LMMS generates at build time */
#define LMMS_EXPORT __attribute__((visibility("default")))
#define LMMS_NO_EXPORT __attribute__((visibility("hidden")))
#define LMMS_DEPRECATED __attribute__((__deprecated__))
#define LMMS_DEPRECATED_EXPORT LMMS_EXPORT LMMS_DEPRECATED
