#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/kd.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <drm/drm.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_mode.h>

struct logo_gradient {
	float x1, y1, x2, y2;
	int count;
	float offsets[4];
	uint32_t colors[4];
};

#include "logo.h"

#define LOGO_WIDTH 210.0f
#define BOX_WIDTH 380
#define BOX_HEIGHT 260
#define SAMPLES 4
#define MAX_CROSSINGS 64
#define BREATH_PERIOD 3.2
#define TINT_PERIOD 6.4
#define BREATH_LOW 0.1
#define BREATH_HIGH 0.9
#define TINT_HIGH 0.7
#define HALO_COLOR_R 10.0f
#define HALO_COLOR_G 132.0f
#define HALO_COLOR_B 255.0f
#define DEADLINE_SECONDS 120
#define HANDOFF_POLL_MS 100
#define EVENT_TIMEOUT_MS 1000
#define TERM_SHIFT 4
#define TERM_ONE (1 << TERM_SHIFT)
#define OPEN_ATTEMPTS 50
#define OPEN_RETRY_MS 100

enum { HALO, GLOW, LOGO, TINT, LAYERS };

struct buffer {
	uint32_t handle;
	uint32_t fb;
	uint32_t pitch;
	uint64_t size;
	uint8_t *map;
};

struct display {
	int fd;
	uint32_t connector;
	uint32_t crtc;
	struct drm_mode_modeinfo mode;
	struct buffer buffers[2];
	int front;
	int step;
	int refresh;
	double fps;
};

static volatile sig_atomic_t handoff_requested;
static volatile sig_atomic_t quit_requested;
static uint8_t layers[LAYERS][BOX_WIDTH * BOX_HEIGHT * 4];

static void prepare_terms(void);

static void on_handoff(int signal_number)
{
	(void)signal_number;
	handoff_requested = 1;
}

static void on_quit(int signal_number)
{
	(void)signal_number;
	quit_requested = 1;
}

static double boot_seconds(void)
{
	struct timespec now;

	clock_gettime(CLOCK_BOOTTIME, &now);
	return now.tv_sec + now.tv_nsec / 1e9;
}

static void add_span(float *line, float from, float to, float weight)
{
	if (from < 0)
		from = 0;
	if (to > BOX_WIDTH)
		to = BOX_WIDTH;
	if (to <= from)
		return;
	int first = (int)from;
	int last = (int)to;
	if (first == last) {
		line[first] += (to - from) * weight;
		return;
	}
	line[first] += (first + 1 - from) * weight;
	for (int x = first + 1; x < last; x++)
		line[x] += weight;
	if (last < BOX_WIDTH)
		line[last] += (to - last) * weight;
}

static void band_coverage(int band, float scale, float left, float top, float *coverage)
{
	const float *points = logo_points + 2 * logo_offsets[band];
	int count = logo_offsets[band + 1] - logo_offsets[band];
	float crossings[MAX_CROSSINGS];

	memset(coverage, 0, sizeof(float) * BOX_WIDTH * BOX_HEIGHT);
	for (int row = 0; row < BOX_HEIGHT * SAMPLES; row++) {
		float y = ((row + 0.5f) / SAMPLES - top) / scale;
		int found = 0;
		for (int i = 0; i < count && found < MAX_CROSSINGS; i++) {
			float x0 = points[2 * i], y0 = points[2 * i + 1];
			float x1 = points[2 * ((i + 1) % count)], y1 = points[2 * ((i + 1) % count) + 1];
			if ((y0 <= y && y1 > y) || (y1 <= y && y0 > y))
				crossings[found++] = (x0 + (y - y0) * (x1 - x0) / (y1 - y0) - LOGO_VIEW_X) * scale + left;
		}
		for (int i = 1; i < found; i++) {
			float value = crossings[i];
			int j = i - 1;
			for (; j >= 0 && crossings[j] > value; j--)
				crossings[j + 1] = crossings[j];
			crossings[j + 1] = value;
		}
		for (int i = 0; i + 1 < found; i += 2)
			add_span(coverage + (row / SAMPLES) * BOX_WIDTH, crossings[i], crossings[i + 1], 1.0f / SAMPLES);
	}
}

static void gradient_color(const struct logo_gradient *gradient, float x, float y, float rgb[3])
{
	float dx = gradient->x2 - gradient->x1;
	float dy = gradient->y2 - gradient->y1;
	float t = ((x - gradient->x1) * dx + (y - gradient->y1) * dy) / (dx * dx + dy * dy);
	int last = gradient->count - 1;
	int segment = 0;

	if (t <= gradient->offsets[0])
		t = gradient->offsets[0];
	if (t >= gradient->offsets[last])
		t = gradient->offsets[last];
	while (segment < last - 1 && t > gradient->offsets[segment + 1])
		segment++;
	float span = gradient->offsets[segment + 1] - gradient->offsets[segment];
	float f = span > 0 ? (t - gradient->offsets[segment]) / span : 0;
	for (int channel = 0; channel < 3; channel++) {
		int shift = 16 - 8 * channel;
		float a = (gradient->colors[segment] >> shift) & 0xff;
		float b = (gradient->colors[segment + 1] >> shift) & 0xff;
		rgb[channel] = a + (b - a) * f;
	}
}

static void render_logo(int palette, float scale, float left, float top, float *rgba, float *coverage)
{
	memset(rgba, 0, sizeof(float) * BOX_WIDTH * BOX_HEIGHT * 4);
	for (int band = 0; band < LOGO_BANDS; band++) {
		band_coverage(band, scale, left, top, coverage);
		for (int y = 0; y < BOX_HEIGHT; y++) {
			for (int x = 0; x < BOX_WIDTH; x++) {
				float alpha = coverage[y * BOX_WIDTH + x];
				if (alpha <= 0)
					continue;
				if (alpha > 1)
					alpha = 1;
				float rgb[3];
				gradient_color(&logo_gradients[palette][band], (x + 0.5f - left) / scale + LOGO_VIEW_X,
					(y + 0.5f - top) / scale, rgb);
				float *pixel = rgba + 4 * (y * BOX_WIDTH + x);
				for (int channel = 0; channel < 3; channel++)
					pixel[channel] = rgb[channel] * alpha + pixel[channel] * (1 - alpha);
				pixel[3] = 255 * alpha + pixel[3] * (1 - alpha);
			}
		}
	}
}

static void box_blur(float *rgba, float *scratch, int radius, int horizontal)
{
	int length = horizontal ? BOX_WIDTH : BOX_HEIGHT;
	int lines = horizontal ? BOX_HEIGHT : BOX_WIDTH;
	int stride = horizontal ? 4 : 4 * BOX_WIDTH;
	float width = 2 * radius + 1;

	for (int line = 0; line < lines; line++) {
		float *base = rgba + (horizontal ? 4 * line * BOX_WIDTH : 4 * line);
		for (int channel = 0; channel < 4; channel++) {
			float sum = 0;
			for (int i = 0; i < radius && i < length; i++)
				sum += base[i * stride + channel];
			for (int i = 0; i < length; i++) {
				if (i + radius < length)
					sum += base[(i + radius) * stride + channel];
				if (i - radius - 1 >= 0)
					sum -= base[(i - radius - 1) * stride + channel];
				scratch[i] = sum / width;
			}
			for (int i = 0; i < length; i++)
				base[i * stride + channel] = scratch[i];
		}
	}
}

static void store_layer(int layer, const float *rgba)
{
	for (int i = 0; i < BOX_WIDTH * BOX_HEIGHT * 4; i++) {
		float value = rgba[i] + 0.5f;
		layers[layer][i] = value < 0 ? 0 : value > 255 ? 255 : (uint8_t)value;
	}
}

static int prepare_layers(void)
{
	float scale = LOGO_WIDTH / LOGO_VIEW_WIDTH;
	float left = (BOX_WIDTH - LOGO_WIDTH) / 2;
	float top = (BOX_HEIGHT - LOGO_VIEW_HEIGHT * scale) / 2;
	float *rgba = malloc(sizeof(float) * BOX_WIDTH * BOX_HEIGHT * 4);
	float *coverage = malloc(sizeof(float) * BOX_WIDTH * BOX_HEIGHT);
	float scratch[BOX_WIDTH > BOX_HEIGHT ? BOX_WIDTH : BOX_HEIGHT];

	if (!rgba || !coverage)
		return -1;
	render_logo(1, scale, left, top, rgba, coverage);
	store_layer(TINT, rgba);
	render_logo(0, scale, left, top, rgba, coverage);
	store_layer(LOGO, rgba);
	float sigma = LOGO_GLOW_BLUR * scale;
	int radius = (int)lroundf((sqrtf(4 * sigma * sigma + 1) - 1) / 2);
	for (int pass = 0; pass < 3; pass++) {
		box_blur(rgba, scratch, radius, 1);
		box_blur(rgba, scratch, radius, 0);
	}
	store_layer(GLOW, rgba);
	float rx = BOX_WIDTH / 2.0f, ry = BOX_HEIGHT / 2.0f;
	for (int y = 0; y < BOX_HEIGHT; y++) {
		for (int x = 0; x < BOX_WIDTH; x++) {
			float dx = (x + 0.5f - rx) / rx, dy = (y + 0.5f - ry) / ry;
			float d = sqrtf(dx * dx + dy * dy);
			float alpha = d < 0.5f ? 0.55f - (0.55f - 0.16f) * d / 0.5f : d < 1 ? 0.16f * (1 - d) / 0.5f : 0;
			float *pixel = rgba + 4 * (y * BOX_WIDTH + x);
			pixel[0] = HALO_COLOR_R * alpha;
			pixel[1] = HALO_COLOR_G * alpha;
			pixel[2] = HALO_COLOR_B * alpha;
			pixel[3] = 255 * alpha;
		}
	}
	store_layer(HALO, rgba);
	free(rgba);
	free(coverage);
	prepare_terms();
	return 0;
}

static double breath_at(double t)
{
	return BREATH_LOW + (BREATH_HIGH - BREATH_LOW) * (0.5 - 0.5 * cos(2 * M_PI * t / BREATH_PERIOD));
}

static double tint_at(double t)
{
	return TINT_HIGH * (0.5 - 0.5 * cos(2 * M_PI * t / TINT_PERIOD));
}

struct term {
	int16_t base[3];
	int16_t linear[3];
	int16_t square[3];
	int16_t alt[3];
	int16_t alt_alpha;
};

static struct term terms[BOX_WIDTH * BOX_HEIGHT];
static int16_t span_first[BOX_HEIGHT];
static int16_t span_last[BOX_HEIGHT];

static int16_t fixed(float value)
{
	return (int16_t)lroundf(value * TERM_ONE);
}

static void prepare_terms(void)
{
	for (int y = 0; y < BOX_HEIGHT; y++) {
		span_first[y] = BOX_WIDTH;
		span_last[y] = 0;
		for (int x = 0; x < BOX_WIDTH; x++) {
			int i = y * BOX_WIDTH + x;
			const uint8_t *halo = layers[HALO] + 4 * i;
			const uint8_t *glow = layers[GLOW] + 4 * i;
			const uint8_t *logo = layers[LOGO] + 4 * i;
			const uint8_t *alt = layers[TINT] + 4 * i;
			float logo_rest = 1 - logo[3] / 255.0f;
			float glow_alpha = glow[3] / 255.0f;
			struct term *term = &terms[i];
			int varies = alt[3] != 0;
			for (int channel = 0; channel < 3; channel++) {
				term->base[channel] = fixed(logo[channel]);
				term->linear[channel] = fixed(logo_rest * (glow[channel] + halo[channel]));
				term->square[channel] = fixed(-logo_rest * halo[channel] * glow_alpha);
				term->alt[channel] = fixed(alt[channel]);
				varies |= term->linear[channel] || term->square[channel];
			}
			term->alt_alpha = (int16_t)lroundf(alt[3] * 256 / 255.0f);
			if (varies) {
				if (x < span_first[y])
					span_first[y] = x;
				span_last[y] = x + 1;
			}
		}
	}
}

static uint32_t compose_pixel(const struct term *term, int32_t breath, int32_t breath_squared, int32_t tint)
{
	uint32_t rgb[3];

	for (int channel = 0; channel < 3; channel++) {
		int32_t value = term->base[channel] + ((breath * term->linear[channel] + breath_squared * term->square[channel]) >> 8);
		value += tint * (term->alt[channel] - (term->alt_alpha * value >> 8)) >> 8;
		value = (value + TERM_ONE / 2) >> TERM_SHIFT;
		rgb[channel] = value < 0 ? 0 : value > 255 ? 255 : (uint32_t)value;
	}
	return rgb[0] << 16 | rgb[1] << 8 | rgb[2];
}

static void compose_region(uint8_t *map, uint32_t pitch, int left, int top, double t, int whole)
{
	int32_t breath = (int32_t)lround(breath_at(t) * 256);
	int32_t breath_squared = breath * breath >> 8;
	int32_t tint = (int32_t)lround(tint_at(t) * 256);

	for (int y = 0; y < BOX_HEIGHT; y++) {
		uint32_t *row = (uint32_t *)(map + (size_t)(top + y) * pitch) + left;
		int first = whole ? 0 : span_first[y];
		int last = whole ? BOX_WIDTH : span_last[y];
		const struct term *term = terms + y * BOX_WIDTH;
		for (int x = first; x < last; x++)
			row[x] = compose_pixel(term + x, breath, breath_squared, tint);
	}
}

static void compose(uint8_t *map, uint32_t pitch, int left, int top, double t)
{
	compose_region(map, pitch, left, top, t, 0);
}

static int dump_frames(const char *directory, int count, int fps)
{
	uint32_t pitch = 480 * 4;
	uint8_t *map = calloc(480, pitch);

	if (!map)
		return 1;
	for (int n = 0; n < count; n++) {
		char path[512];
		compose_region(map, pitch, (480 - BOX_WIDTH) / 2, (480 - BOX_HEIGHT) / 2, (double)n / fps, n == 0);
		snprintf(path, sizeof(path), "%s/frame-%04d.ppm", directory, n);
		FILE *file = fopen(path, "wb");
		if (!file)
			return 1;
		fprintf(file, "P6\n480 480\n255\n");
		for (int i = 0; i < 480 * 480; i++) {
			uint32_t pixel = ((uint32_t *)map)[i];
			uint8_t rgb[3] = { pixel >> 16 & 0xff, pixel >> 8 & 0xff, pixel & 0xff };
			fwrite(rgb, 1, 3, file);
		}
		fclose(file);
	}
	free(map);
	return 0;
}

static int get_resources(int fd, struct drm_mode_card_res *resources, uint32_t **connectors, uint32_t **crtcs)
{
	memset(resources, 0, sizeof(*resources));
	if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, resources) || !resources->count_connectors || !resources->count_crtcs)
		return -1;
	*connectors = calloc(resources->count_connectors, sizeof(uint32_t));
	*crtcs = calloc(resources->count_crtcs, sizeof(uint32_t));
	if (!*connectors || !*crtcs)
		return -1;
	resources->count_fbs = 0;
	resources->count_encoders = 0;
	resources->connector_id_ptr = (uintptr_t)*connectors;
	resources->crtc_id_ptr = (uintptr_t)*crtcs;
	return ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, resources);
}

static int pick_connector(struct display *display, uint32_t connector_id, const uint32_t *crtcs, uint32_t crtc_count)
{
	struct drm_mode_get_connector connector = { .connector_id = connector_id };

	if (ioctl(display->fd, DRM_IOCTL_MODE_GETCONNECTOR, &connector))
		return -1;
	if (connector.connection != 1 || !connector.count_modes || !connector.count_encoders)
		return -1;
	struct drm_mode_modeinfo *modes = calloc(connector.count_modes, sizeof(*modes));
	uint32_t *encoders = calloc(connector.count_encoders, sizeof(uint32_t));
	if (!modes || !encoders)
		return -1;
	uint32_t mode_count = connector.count_modes, encoder_count = connector.count_encoders;
	connector.modes_ptr = (uintptr_t)modes;
	connector.encoders_ptr = (uintptr_t)encoders;
	connector.count_props = 0;
	if (ioctl(display->fd, DRM_IOCTL_MODE_GETCONNECTOR, &connector) || connector.count_modes > mode_count ||
		connector.count_encoders > encoder_count)
		return -1;
	display->mode = modes[0];
	for (uint32_t i = 0; i < connector.count_modes; i++) {
		if (modes[i].type & DRM_MODE_TYPE_PREFERRED) {
			display->mode = modes[i];
			break;
		}
	}
	uint32_t encoder_id = connector.encoder_id ? connector.encoder_id : encoders[0];
	struct drm_mode_get_encoder encoder = { .encoder_id = encoder_id };
	if (ioctl(display->fd, DRM_IOCTL_MODE_GETENCODER, &encoder))
		return -1;
	display->crtc = encoder.crtc_id;
	for (uint32_t i = 0; !display->crtc && i < crtc_count; i++) {
		if (encoder.possible_crtcs & (1u << i))
			display->crtc = crtcs[i];
	}
	display->connector = connector_id;
	free(modes);
	free(encoders);
	return display->crtc ? 0 : -1;
}

static int open_display(struct display *display)
{
	for (int card = 0; card < 8; card++) {
		char path[32];
		snprintf(path, sizeof(path), "dri/card%d", card);
		display->fd = open(path, O_RDWR | O_CLOEXEC);
		if (display->fd < 0) {
			snprintf(path, sizeof(path), "/dev/dri/card%d", card);
			display->fd = open(path, O_RDWR | O_CLOEXEC);
		}
		if (display->fd < 0)
			continue;
		struct drm_mode_card_res resources;
		uint32_t *connectors = NULL, *crtcs = NULL;
		if (!get_resources(display->fd, &resources, &connectors, &crtcs)) {
			for (uint32_t i = 0; i < resources.count_connectors; i++) {
				if (!pick_connector(display, connectors[i], crtcs, resources.count_crtcs)) {
					free(connectors);
					free(crtcs);
					return 0;
				}
			}
		}
		free(connectors);
		free(crtcs);
		close(display->fd);
	}
	return -1;
}

static int create_buffer(struct display *display, struct buffer *buffer)
{
	struct drm_mode_create_dumb create = {
		.width = display->mode.hdisplay,
		.height = display->mode.vdisplay,
		.bpp = 32,
	};

	if (ioctl(display->fd, DRM_IOCTL_MODE_CREATE_DUMB, &create))
		return -1;
	buffer->handle = create.handle;
	buffer->pitch = create.pitch;
	buffer->size = create.size;
	struct drm_mode_fb_cmd2 framebuffer = {
		.width = display->mode.hdisplay,
		.height = display->mode.vdisplay,
		.pixel_format = DRM_FORMAT_XRGB8888,
		.handles = { create.handle },
		.pitches = { create.pitch },
	};
	if (ioctl(display->fd, DRM_IOCTL_MODE_ADDFB2, &framebuffer))
		return -1;
	buffer->fb = framebuffer.fb_id;
	struct drm_mode_map_dumb map = { .handle = create.handle };
	if (ioctl(display->fd, DRM_IOCTL_MODE_MAP_DUMB, &map))
		return -1;
	buffer->map = mmap(NULL, create.size, PROT_READ | PROT_WRITE, MAP_SHARED, display->fd, map.offset);
	if (buffer->map == MAP_FAILED)
		return -1;
	memset(buffer->map, 0, create.size);
	return 0;
}

static void set_console_mode(int mode)
{
	int tty = open("tty0", O_RDWR | O_CLOEXEC);

	if (tty < 0)
		tty = open("/dev/tty0", O_RDWR | O_CLOEXEC);
	if (tty < 0)
		return;
	ioctl(tty, KDSETMODE, mode);
	close(tty);
}

static int request_vblank(struct display *display, unsigned int count)
{
	union drm_wait_vblank wait = { 0 };

	wait.request.type = _DRM_VBLANK_RELATIVE | _DRM_VBLANK_EVENT;
	wait.request.sequence = count;
	return ioctl(display->fd, DRM_IOCTL_WAIT_VBLANK, &wait);
}

static void prepare_back(struct display *display, int left, int top)
{
	int back = !display->front;
	double ahead = (double)display->step / display->refresh;
	double t = round((boot_seconds() + ahead) * display->fps) / display->fps;

	compose(display->buffers[back].map, display->buffers[back].pitch, left, top, t);
}

enum flip_result { FLIPPED, RETRY, FAILED };

static enum flip_result flip_back(struct display *display)
{
	struct drm_mode_crtc_page_flip flip = {
		.crtc_id = display->crtc,
		.fb_id = display->buffers[!display->front].fb,
		.flags = DRM_MODE_PAGE_FLIP_EVENT,
	};

	if (!ioctl(display->fd, DRM_IOCTL_MODE_PAGE_FLIP, &flip))
		return FLIPPED;
	return errno == EBUSY ? RETRY : FAILED;
}

static void log_kernel(const char *text)
{
	int kmsg = open("kmsg", O_WRONLY | O_CLOEXEC);

	if (kmsg < 0)
		kmsg = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
	if (kmsg < 0)
		return;
	dprintf(kmsg, "%s: %s\n", text, strerror(errno));
	close(kmsg);
}

static uint32_t current_fb(struct display *display)
{
	struct drm_mode_crtc crtc = { .crtc_id = display->crtc };

	if (ioctl(display->fd, DRM_IOCTL_MODE_GETCRTC, &crtc))
		return 0;
	return crtc.fb_id;
}

static void hand_off(struct display *display)
{
	uint32_t shown = current_fb(display);

	if (ioctl(display->fd, DRM_IOCTL_DROP_MASTER, 0))
		log_kernel("wallview-splash: dropping DRM master failed");
	while (!quit_requested && current_fb(display) == shown)
		usleep(HANDOFF_POLL_MS * 1000);
}

struct schedule {
	int flip_pending;
	int waiting_for_vblank;
};

static int advance(struct display *display, struct schedule *schedule, int wait_first)
{
	if (wait_first && !request_vblank(display, wait_first)) {
		schedule->waiting_for_vblank = 1;
		return 0;
	}
	switch (flip_back(display)) {
	case FLIPPED:
		schedule->flip_pending = 1;
		return 0;
	case RETRY:
		if (!request_vblank(display, 1)) {
			schedule->waiting_for_vblank = 1;
			return 0;
		}
		return -1;
	default:
		return -1;
	}
}

static int run(int fps)
{
	struct display display = { .fd = -1 };

	for (int attempt = 0; open_display(&display); attempt++) {
		if (attempt >= OPEN_ATTEMPTS || quit_requested || handoff_requested)
			return 1;
		usleep(OPEN_RETRY_MS * 1000);
	}
	int width = display.mode.hdisplay, height = display.mode.vdisplay;
	if (width < BOX_WIDTH || height < BOX_HEIGHT)
		return 1;
	int left = (width - BOX_WIDTH) / 2, top = (height - BOX_HEIGHT) / 2;
	display.refresh = display.mode.vrefresh ? (int)display.mode.vrefresh : 60;
	display.step = (int)lround((double)display.refresh / fps);
	if (display.step < 1)
		display.step = 1;
	display.fps = (double)display.refresh / display.step;
	for (int i = 0; i < 2; i++) {
		if (create_buffer(&display, &display.buffers[i]))
			return 1;
	}
	if (prepare_layers())
		return 1;
	double deadline = boot_seconds() + DEADLINE_SECONDS;
	for (int i = 0; i < 2; i++)
		compose_region(display.buffers[i].map, display.buffers[i].pitch, left, top, boot_seconds(), 1);
	struct drm_mode_crtc crtc = {
		.crtc_id = display.crtc,
		.fb_id = display.buffers[0].fb,
		.set_connectors_ptr = (uintptr_t)&display.connector,
		.count_connectors = 1,
		.mode = display.mode,
		.mode_valid = 1,
	};
	if (ioctl(display.fd, DRM_IOCTL_MODE_SETCRTC, &crtc))
		return 1;
	set_console_mode(KD_GRAPHICS);
	display.front = 0;
	struct schedule schedule = { 0 };
	prepare_back(&display, left, top);
	if (advance(&display, &schedule, display.step - 1))
		goto done;
	while (!quit_requested) {
		if (boot_seconds() > deadline)
			handoff_requested = 1;
		if (handoff_requested && !schedule.flip_pending)
			break;
		struct pollfd poll_fd = { .fd = display.fd, .events = POLLIN };
		int ready = poll(&poll_fd, 1, EVENT_TIMEOUT_MS);
		if (ready < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (ready == 0) {
			schedule.flip_pending = 0;
			schedule.waiting_for_vblank = 0;
			if (handoff_requested)
				break;
			if (advance(&display, &schedule, 0))
				break;
			continue;
		}
		if (poll_fd.revents & (POLLERR | POLLHUP | POLLNVAL))
			break;
		char events[1024] __attribute__((aligned(8)));
		ssize_t length = read(display.fd, events, sizeof(events));
		for (ssize_t offset = 0; offset + (ssize_t)sizeof(struct drm_event) <= length;) {
			struct drm_event *event = (struct drm_event *)(events + offset);
			if (event->length < sizeof(struct drm_event))
				break;
			offset += event->length;
			if (event->type == DRM_EVENT_FLIP_COMPLETE && schedule.flip_pending) {
				schedule.flip_pending = 0;
				display.front = !display.front;
				if (handoff_requested)
					continue;
				prepare_back(&display, left, top);
				if (advance(&display, &schedule, display.step - 1))
					goto done;
			} else if (event->type == DRM_EVENT_VBLANK && schedule.waiting_for_vblank) {
				schedule.waiting_for_vblank = 0;
				if (handoff_requested)
					continue;
				if (advance(&display, &schedule, 0))
					goto done;
			}
		}
	}
done:
	if (!quit_requested)
		hand_off(&display);
	return 0;
}

int main(int argc, char **argv)
{
	int fps = 30;
	struct sigaction handoff = { .sa_handler = on_handoff };
	struct sigaction quit = { .sa_handler = on_quit };

	sigaction(SIGUSR1, &handoff, NULL);
	sigaction(SIGTERM, &quit, NULL);
	sigaction(SIGINT, &quit, NULL);
	if (argc >= 3 && !strcmp(argv[1], "--fps")) {
		fps = atoi(argv[2]);
		argv += 2;
		argc -= 2;
	}
	if (fps < 1 || fps > 60)
		return 2;
	if (argc == 4 && !strcmp(argv[1], "--dump"))
		return prepare_layers() ? 1 : dump_frames(argv[2], atoi(argv[3]), fps);
	return run(fps);
}
