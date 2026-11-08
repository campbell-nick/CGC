#ifndef CGC_PROGRESS_H
#define CGC_PROGRESS_H

#include <stddef.h>
#include <stdint.h>

/* Git-style status output. Everything here is silent when --quiet is set.
 * Live meters are only drawn on a terminal; a final summary line is always
 * printed (unless quiet) so redirected output still records what happened. */

/* True when live status output (meters and remote progress) should be drawn. */
int progress_live_enabled(void);

void progress_remote(const void *data, size_t len);
void progress_remote_end(void);

void progress_receive_begin(void);
void progress_receive_update(size_t bytes);
void progress_receive_end(uint32_t objects, size_t bytes);

void progress_deltas_update(uint32_t done, uint32_t total);
void progress_deltas_end(uint32_t total);

void progress_checkout_begin(size_t total);
void progress_checkout_update(size_t done, size_t total);
void progress_checkout_end(size_t total);

/* Erase an in-progress meter line (used before warnings/errors). */
void progress_clear(void);

#endif
