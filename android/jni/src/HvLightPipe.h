/**
 * Copyright (c) 2016 Enzien Audio Ltd.
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH
 * REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT,
 * INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR
 * OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */



#ifndef _HEAVY_LIGHTPIPE_H_
#define _HEAVY_LIGHTPIPE_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * This pipe assumes that there is only one producer thread and one consumer
 * thread. This data structure does not support any other configuration.
 * Note that there is no mechanism implemented to know when the pipe is full;
 * old data will simply be overwritten.
 */
typedef struct HvLightPipe {
  char *buffer;
  char *writeHead;
  char *readHead;
  uint32_t len;
  uint32_t remainingBytes; // total bytes from write head to end
} HvLightPipe;

// initialise the pipe with a given length, in bytes.
void hLp_init(HvLightPipe *q, uint32_t numBytes);

// free the internal buffer
void hLp_free(HvLightPipe *q);

// returns zero if no data is available, otherwise returns the number of bytes
// available for reading
uint32_t hLp_hasData(HvLightPipe *q);

/**
 * Returns a pointer to a location in the pipe where numBytes can be written.
 *
 * @param numBytes  The number of bytes to be written.
 * @returns  A pointer to a location where those bytes can be written. Returns
 *           NULL if no more space is available. Successive calls to this
 *           function may eventually return a valid pointer because the readhead
 *           has been advanced.
 */
char *hLp_getWriteBuffer(HvLightPipe *q, uint32_t numBytes);

// indicate to the pipe how many bytes have been written.
void hLp_produce(HvLightPipe *q, uint32_t numBytes);

// returns the current read buffer, indicating the number of bytes available
// for reading.
char *hLp_getReadBuffer(HvLightPipe *q, uint32_t *numBytes);

void hLp_consume(HvLightPipe *q);

// resets the queue to it's initialised state
// This should be done when only one thread is accessing the pipe.
void hLp_reset(HvLightPipe *q);

#ifdef __cplusplus
}
#endif

#endif // _HEAVY_LIGHTPIPE_H_
