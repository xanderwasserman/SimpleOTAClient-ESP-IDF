/**
 * @file
 * @brief What the engine tells the application: events, log lines,
 * and a wake-up when the confirm timeout fires. Implemented by the public
 * API layer (port/esp32/simpleota.c) and by the host tests' fake.
 */

#ifndef SOTA_APP_H
#define SOTA_APP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** In the order of simpleota_event_id_t, so the API layer can pass
 *  them through. */
typedef enum {
    SOTA_EVENT_CHECK_DONE,        /**< A check completed; no update offered. */
    SOTA_EVENT_UPDATE_AVAILABLE,  /**< An update was offered. */
    SOTA_EVENT_DOWNLOAD_PROGRESS, /**< done / total advanced. */
    SOTA_EVENT_UPDATE_FAILED,     /**< The update was dropped; reason says why. */
    SOTA_EVENT_BEFORE_REBOOT,     /**< Flashed and verified; it runs from the next reboot. */
    SOTA_EVENT_TRIAL_BOOT,        /**< Running an image that is not confirmed yet. */
    SOTA_EVENT_CONFIRMED,         /**< The trial image's confirmed report was delivered. */
    SOTA_EVENT_ROLLING_BACK,      /**< The trial failed; the previous image starts next. */
} sota_event_id_t;

/** One event, valid only during the sota_app_event() call. */
typedef struct {
    sota_event_id_t id; /**< What happened. */
    const char *reason; /**< A reason token or server status, or NULL. */
    uint32_t build;     /**< The build the event is about, 0 when none. */
    size_t done;        /**< DOWNLOAD_PROGRESS: bytes written. */
    size_t total;       /**< DOWNLOAD_PROGRESS: bytes expected, 0 when unknown.
                             UPDATE_AVAILABLE: the offer's size, 0 when unknown. */
} sota_event_t;

/** Deliver an event to the application, on the task doing the work.
 *  user is sota_config_t::user. */
void sota_app_event(void *user, const sota_event_t *event);

/** The confirm timeout fired, possibly in a timer task: arrange for
 *  sota_engine_service() to run soon on a task that may block. */
void sota_app_wake(void *user);

/** Log levels, numbered as esp_log_level_t. */
enum { SOTA_LOG_NONE, SOTA_LOG_ERROR, SOTA_LOG_WARN, SOTA_LOG_INFO, SOTA_LOG_DEBUG };

/** Write one log line at level, printf-style, without a trailing newline.
 *  The core calls it through the SOTA_LOG* macros. */
void sota_app_log(int level, const char *fmt, ...)
#ifdef __GNUC__
    __attribute__((format(printf, 2, 3)))
#endif
    ;

#ifndef SOTA_LOG_LEVEL
/** The most detailed level compiled in; lines above it are left out. The
 *  ESP-IDF component sets it to CONFIG_LOG_MAXIMUM_LEVEL. */
#define SOTA_LOG_LEVEL SOTA_LOG_DEBUG
#endif

/** Log at level when SOTA_LOG_LEVEL includes it. */
#define SOTA_LOG_AT(level, ...) \
    do { if (SOTA_LOG_LEVEL >= (level)) sota_app_log((level), __VA_ARGS__); } while (0)
#define SOTA_LOGE(...) SOTA_LOG_AT(SOTA_LOG_ERROR, __VA_ARGS__) /**< Log an error. */
#define SOTA_LOGW(...) SOTA_LOG_AT(SOTA_LOG_WARN, __VA_ARGS__)  /**< Log a warning. */
#define SOTA_LOGI(...) SOTA_LOG_AT(SOTA_LOG_INFO, __VA_ARGS__)  /**< Log at info level. */
#define SOTA_LOGD(...) SOTA_LOG_AT(SOTA_LOG_DEBUG, __VA_ARGS__) /**< Log at debug level. */

#ifdef __cplusplus
}
#endif

#endif /* SOTA_APP_H */
