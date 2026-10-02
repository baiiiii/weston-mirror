/*
 * Copyright © 2026 Weston-mirror contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial
 * portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT.  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * weston-text-input-test: minimal zwp_text_input_v3 test client.
 *
 * Binds zwp_text_input_v3, enables text input and prints every preedit /
 * commit / key event it receives on stdout.  Use it to verify the
 * MS-RDPETXT -> text-input-bridge pipeline without a real toolkit:
 *
 *   1. click into the window (so it gains keyboard focus),
 *   2. type with the Windows IME in the WSLg window,
 *   3. expect "preedit" lines while composing and "commit" lines on
 *      candidate selection.
 */

#include "config.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <wayland-client.h>
#include "shared/os-compatibility.h"
#include "xdg-shell-client-protocol.h"
#include "text-input-unstable-v3-client-protocol.h"

struct display {
	struct wl_display *display;
	struct wl_registry *registry;
	struct wl_compositor *compositor;
	struct wl_seat *seat;
	struct wl_shm *shm;
	struct xdg_wm_base *xdg_wm_base;
	struct zwp_text_input_manager_v3 *text_input_manager;
	bool text_input_manager_seen;
};

struct buffer {
	struct wl_buffer *buffer;
	void *shm_data;
	size_t size;
};

struct keyboard {
	struct wl_keyboard *keyboard;
	bool pressed[256];
};

struct test {
	struct display display;
	struct wl_surface *surface;
	struct xdg_surface *xdg_surface;
	struct xdg_toplevel *xdg_toplevel;
	struct zwp_text_input_v3 *text_input;
	struct keyboard keyboard;
	struct buffer buffer;
	int32_t width, height;
	bool configured;
	bool text_input_enabled;
	bool running;
	int exit_code;
};

static void
buffer_release(void *data, struct wl_buffer *buffer)
{
}

static const struct wl_buffer_listener buffer_listener = {
	buffer_release
};

static int
create_shm_buffer(struct test *test, int32_t width, int32_t height)
{
	struct display *display = &test->display;
	struct wl_shm_pool *pool;
	int fd, stride;
	void *data;

	stride = width * 4;
	fd = os_create_anonymous_file(height * stride);
	if (fd < 0) {
		fprintf(stderr, "creating a buffer file failed: %s\n",
			strerror(errno));
		return -1;
	}

	data = mmap(NULL, height * stride, PROT_READ | PROT_WRITE,
		    MAP_SHARED, fd, 0);
	if (data == MAP_FAILED) {
		fprintf(stderr, "mmap failed: %s\n", strerror(errno));
		close(fd);
		return -1;
	}

	pool = wl_shm_create_pool(display->shm, fd, height * stride);
	test->buffer.buffer =
		wl_shm_pool_create_buffer(pool, 0, width, height, stride,
					  WL_SHM_FORMAT_ARGB8888);
	wl_buffer_add_listener(test->buffer.buffer, &buffer_listener,
			       test);
	wl_shm_pool_destroy(pool);

	/* paint a solid dark blue */
	for (int32_t y = 0; y < height; y++) {
		uint32_t *row = (uint32_t *)((char *)data + y * stride);
		for (int32_t x = 0; x < width; x++)
			row[x] = 0xFF205080;
	}

	munmap(data, height * stride);
	close(fd);

	test->buffer.size = (size_t)height * stride;
	test->width = width;
	test->height = height;

	return 0;
}

static void
redraw(struct test *test)
{
	if (!test->buffer.buffer &&
	    create_shm_buffer(test, 480, 240) < 0) {
		test->exit_code = EXIT_FAILURE;
		return;
	}

	wl_surface_attach(test->surface, test->buffer.buffer, 0, 0);
	wl_surface_damage(test->surface, 0, 0, test->width, test->height);
	wl_surface_commit(test->surface);
}

/* ------------------------------------------------------------------ */
/* zwp_text_input_v3 events                                            */
/* ------------------------------------------------------------------ */

static void
text_input_enter(void *data, struct zwp_text_input_v3 *text_input,
		 struct wl_surface *surface)
{
	printf("[ti] enter (surface %p) - now enabling text input\n",
	       (void *)surface);
}

static void
text_input_leave(void *data, struct zwp_text_input_v3 *text_input,
		 struct wl_surface *surface)
{
	printf("[ti] leave (surface %p)\n", (void *)surface);
}

static void
text_input_preedit_string(void *data, struct zwp_text_input_v3 *text_input,
			  const char *text, int32_t cursor_begin,
			  int32_t cursor_end)
{
	printf("[ti] PREEDIT \"%s\" (cursor %d..%d)\n", text, cursor_begin,
	       cursor_end);
}

static void
text_input_commit_string(void *data, struct zwp_text_input_v3 *text_input,
			 const char *text)
{
	printf("[ti] COMMIT \"%s\"\n", text);
}

static void
text_input_delete_surrounding_text(void *data,
				   struct zwp_text_input_v3 *text_input,
				   uint32_t before_length,
				   uint32_t after_length)
{
	printf("[ti] delete_surrounding_text %u %u\n", before_length,
	       after_length);
}

static void
text_input_done(void *data, struct zwp_text_input_v3 *text_input,
		uint32_t serial)
{
	printf("[ti] done (serial %u)\n", serial);
}

static const struct zwp_text_input_v3_listener text_input_listener = {
	text_input_enter,
	text_input_leave,
	text_input_preedit_string,
	text_input_commit_string,
	text_input_delete_surrounding_text,
	text_input_done,
};

/* ------------------------------------------------------------------ */
/* wl_keyboard                                                         */
/* ------------------------------------------------------------------ */

static void
keyboard_keymap(void *data, struct wl_keyboard *keyboard, uint32_t format,
		int32_t fd, uint32_t size)
{
	close(fd);
}

static void
keyboard_enter(void *data, struct wl_keyboard *keyboard, uint32_t serial,
	       struct wl_surface *surface, struct wl_array *keys)
{
	printf("[kbd] enter\n");
}

static void
keyboard_leave(void *data, struct wl_keyboard *keyboard, uint32_t serial,
	       struct wl_surface *surface)
{
	printf("[kbd] leave\n");
}

static void
keyboard_key(void *data, struct wl_keyboard *keyboard, uint32_t serial,
	     uint32_t time, uint32_t key, uint32_t state)
{
	struct test *test = data;

	printf("[kbd] key %u %s\n", key,
	       state == WL_KEYBOARD_KEY_STATE_PRESSED ? "press" :
	       "release");

	if (state == WL_KEYBOARD_KEY_STATE_PRESSED && key < 256)
		test->keyboard.pressed[key] = true;
	else if (state == WL_KEYBOARD_KEY_STATE_RELEASED && key < 256)
		test->keyboard.pressed[key] = false;
}

static void
keyboard_modifiers(void *data, struct wl_keyboard *keyboard, uint32_t serial,
		   uint32_t mods_depressed, uint32_t mods_latched,
		   uint32_t mods_locked, uint32_t group)
{
}

static void
keyboard_repeat_info(void *data, struct wl_keyboard *keyboard,
		     int32_t rate, int32_t delay)
{
}

static const struct wl_keyboard_listener keyboard_listener = {
	keyboard_keymap,
	keyboard_enter,
	keyboard_leave,
	keyboard_key,
	keyboard_modifiers,
	keyboard_repeat_info,
};

/* ------------------------------------------------------------------ */
/* xdg_shell                                                           */
/* ------------------------------------------------------------------ */

static void
xdg_surface_configure(void *data, struct xdg_surface *xdg_surface,
		      uint32_t serial)
{
	struct test *test = data;

	xdg_surface_ack_configure(xdg_surface, serial);

	if (!test->configured) {
		test->configured = true;
		redraw(test);

		/* Activate text input once the window is on screen. The
		 * compositor answers with enter() when the surface gains
		 * keyboard focus. */
		if (test->text_input && !test->text_input_enabled) {
			zwp_text_input_v3_enable(test->text_input);
			zwp_text_input_v3_set_content_type(
				test->text_input,
				ZWP_TEXT_INPUT_V3_CONTENT_HINT_NONE,
				ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_NORMAL);
			zwp_text_input_v3_commit(test->text_input);
			test->text_input_enabled = true;
			printf("[ti] enable+commit sent\n");
		}
	}
}

static const struct xdg_surface_listener xdg_surface_listener = {
	xdg_surface_configure
};

static void
xdg_toplevel_configure(void *data, struct xdg_toplevel *xdg_toplevel,
		       int32_t width, int32_t height,
		       struct wl_array *states)
{
}

static void
xdg_toplevel_close(void *data, struct xdg_toplevel *xdg_toplevel)
{
	struct test *test = data;

	test->running = false;
}

static const struct xdg_toplevel_listener xdg_toplevel_listener = {
	xdg_toplevel_configure,
	xdg_toplevel_close,
};

static void
xdg_wm_base_ping(void *data, struct xdg_wm_base *xdg_wm_base,
		 uint32_t serial)
{
	xdg_wm_base_pong(xdg_wm_base, serial);
}

static const struct xdg_wm_base_listener xdg_wm_base_listener = {
	xdg_wm_base_ping
};

/* ------------------------------------------------------------------ */
/* registry                                                            */
/* ------------------------------------------------------------------ */

static void
seat_handle_capabilities(void *data, struct wl_seat *seat,
			 enum wl_seat_capability caps)
{
	struct test *test = data;

	if (caps & WL_SEAT_CAPABILITY_KEYBOARD &&
	    !test->keyboard.keyboard) {
		test->keyboard.keyboard = wl_seat_get_keyboard(seat);
		wl_keyboard_add_listener(test->keyboard.keyboard,
					 &keyboard_listener, test);
	}
}

static void
seat_handle_name(void *data, struct wl_seat *seat, const char *name)
{
}

static const struct wl_seat_listener seat_listener = {
	seat_handle_capabilities,
	seat_handle_name,
};

static void
registry_handle_global(void *data, struct wl_registry *registry,
		       uint32_t name, const char *interface,
		       uint32_t version)
{
	struct test *test = data;
	struct display *d = &test->display;

	if (strcmp(interface, "wl_compositor") == 0) {
		d->compositor =
			wl_registry_bind(registry, name,
					 &wl_compositor_interface, 1);
	} else if (strcmp(interface, "wl_seat") == 0) {
		d->seat = wl_registry_bind(registry, name,
					   &wl_seat_interface, 1);
		wl_seat_add_listener(d->seat, &seat_listener, test);
	} else if (strcmp(interface, "wl_shm") == 0) {
		d->shm = wl_registry_bind(registry, name,
					  &wl_shm_interface, 1);
	} else if (strcmp(interface, "xdg_wm_base") == 0) {
		d->xdg_wm_base = wl_registry_bind(registry, name,
						  &xdg_wm_base_interface, 1);
		xdg_wm_base_add_listener(d->xdg_wm_base,
					 &xdg_wm_base_listener, test);
	} else if (strcmp(interface, "zwp_text_input_manager_v3") == 0) {
		d->text_input_manager_seen = true;
		d->text_input_manager =
			wl_registry_bind(registry, name,
					 &zwp_text_input_manager_v3_interface,
					 1);
	}
}

static void
registry_handle_global_remove(void *data, struct wl_registry *registry,
			      uint32_t name)
{
}

static const struct wl_registry_listener registry_listener = {
	registry_handle_global,
	registry_handle_global_remove
};

/* ------------------------------------------------------------------ */

int
main(int argc, char *argv[])
{
	struct test test = { 0 };
	struct display *d = &test.display;

	test.width = 480;
	test.height = 240;

	d->display = wl_display_connect(NULL);
	if (!d->display) {
		fprintf(stderr, "failed to connect to a wayland compositor\n");
		return EXIT_FAILURE;
	}

	d->registry = wl_display_get_registry(d->display);
	wl_registry_add_listener(d->registry, &registry_listener, &test);
	wl_display_roundtrip(d->display);

	if (!d->compositor || !d->shm || !d->xdg_wm_base || !d->seat) {
		fprintf(stderr, "required globals missing\n");
		return EXIT_FAILURE;
	}

	if (!d->text_input_manager) {
		fprintf(stderr, "zwp_text_input_manager_v3 not advertised by "
			"the compositor - text-input-bridge is not active\n");
		return EXIT_FAILURE;
	}

	test.surface = wl_compositor_create_surface(d->compositor);

	test.xdg_surface = xdg_wm_base_get_xdg_surface(d->xdg_wm_base,
						       test.surface);
	xdg_surface_add_listener(test.xdg_surface, &xdg_surface_listener,
				 &test);

	test.xdg_toplevel = xdg_surface_get_toplevel(test.xdg_surface);
	xdg_toplevel_add_listener(test.xdg_toplevel, &xdg_toplevel_listener,
				  &test);
	xdg_toplevel_set_title(test.xdg_toplevel, "text-input-test");

	test.text_input =
		zwp_text_input_manager_v3_get_text_input(
			d->text_input_manager, d->seat);
	zwp_text_input_v3_add_listener(test.text_input,
				       &text_input_listener, &test);

	wl_surface_commit(test.surface);

	test.running = true;
	while (test.running && wl_display_dispatch(d->display) >= 0)
		;

	fprintf(stderr, "exiting (code %d)\n", test.exit_code);
	return test.exit_code;
}
