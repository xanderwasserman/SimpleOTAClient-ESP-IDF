/**
 * @file
 * @brief What sota_engine.c and sota_apply.c share. Not for the
 * API layer.
 */

#ifndef SOTA_ENGINE_PRIV_H
#define SOTA_ENGINE_PRIV_H

#include "sota_app.h"
#include "sota_engine.h"

/** A flashed update waits for its reboot. Describes this boot, so it is
 *  shared by every engine and survives sota_engine_deinit(). */
extern bool sota_engine_waiting;

/** Deliver an event through sota_app_event(). */
void sota_engine_emit(sota_engine_t *e, sota_event_id_t id, const char *reason,
                      uint32_t build, size_t done, size_t total);

/** Post one status event. -1 when there is no deployment or no response. */
int sota_engine_post_status(sota_engine_t *e, const char *event, const char *reason,
                            const char *deployment, uint32_t build);

/** Forget the current offer and free its URL. */
void sota_engine_clear_offer(sota_engine_t *e);

/** Load the stored state into e->st again. */
void sota_engine_reload(sota_engine_t *e);

#endif /* SOTA_ENGINE_PRIV_H */
