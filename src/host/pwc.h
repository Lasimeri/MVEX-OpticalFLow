/* pwc.h: frames of one PipeWire video node, in plain memory (memfd), at a capped rate. */
#ifndef MF_PWC_H
#define MF_PWC_H

#include <stdint.h>

struct pwc;

/* Called on PipeWire's own thread, the frame valid only during the call. pts_ns: the source's stamp (0 when it gives none); t_ns: CLOCK_MONOTONIC on arrival. */
typedef void (*pwc_frame_fn)(void *user, const uint8_t *data, int stride, int w, int h, uint64_t pts_ns, uint64_t t_ns);

/* node: the PipeWire id. Rate: num / den frames a second at most. NULL with a message. */
struct pwc *pwc_open(uint32_t node, int w, int h, uint32_t num, uint32_t den, pwc_frame_fn fn, void *user);
void pwc_close(struct pwc *c);
/* After negotiation: "BGRx 1366x768, max 29895/1000", or empty. */
const char *pwc_format(const struct pwc *c);

#endif
