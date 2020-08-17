#include "config.h"

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <stdbool.h>
#include <drm_fourcc.h>

#include <libweston/libweston.h>
#include <libweston/backend.h>
#include <libweston/backend-headless.h>
#include "shared/helpers.h"
#include "linux-explicit-synchronization.h"
#include "linux-dmabuf.h"
#include "presentation-time-server-protocol.h"
#include <libweston/windowed-output-api.h>

enum headless_renderer_type {
	HEADLESS_NOOP,
	HEADLESS_PIXMAN,
	HEADLESS_GL,
};

struct headless_backend {
	struct weston_backend base;
	struct weston_compositor *compositor;

	struct weston_seat fake_seat;
	enum headless_renderer_type renderer_type;

	struct gl_renderer_interface *glri;
};

struct headless_head {
	struct weston_head base;
};

struct headless_output {
	struct weston_output base;

	struct weston_mode mode;
	struct wl_event_source *finish_frame_timer;
	uint32_t *image_buf;
	pixman_image_t *image;
};

static const uint32_t headless_formats[] = {
	DRM_FORMAT_XRGB8888,
	DRM_FORMAT_ARGB8888,
};

static inline struct headless_head *
to_headless_head(struct weston_head *base)
{
	return container_of(base, struct headless_head, base);
}

static inline struct headless_output *
to_headless_output(struct weston_output *base)
{
	return container_of(base, struct headless_output, base);
}

static inline struct headless_backend *
to_headless_backend(struct weston_compositor *base)
{
	return container_of(base->backend, struct headless_backend, base);
}

