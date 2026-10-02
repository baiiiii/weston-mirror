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
 * Compositor side of zwp_text_input_v3 plus a small in-process bridge API
 * for an IME provider (e.g. the MS-RDPETXT server in the RDP backend).
 *
 * The bridge does not process keys itself; it only tracks which client is
 * ready to receive text and forwards preedit/commit strings that an IME
 * provider pushes through struct weston_text_input_bridge_api.
 *
 * Protocol notes (text-input-unstable-v3, interface version 1 is
 * advertised):
 *  - enter(surface) is sent when the text input's surface gains keyboard
 *    focus, leave() when it loses it.
 *  - The client activates text input by sending enable followed by
 *    commit(); we then consider it active while its surface keeps focus.
 *  - Preedit/commit pushed by the IME provider are sent as
 *    preedit_string/commit_string events followed by done(serial) with an
 *    internally increasing serial, mirroring the compositor-initiated
 *    update model of the protocol.
 */

#include "config.h"

#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include <libweston/libweston.h>
#include <libweston/plugin-registry.h>

#include "weston.h"
#include "text-input-unstable-v3-server-protocol.h"
#include "shared/helpers.h"

struct ti_bridge {
	struct weston_compositor *ec;
	struct wl_global *manager_global;
	struct wl_listener destroy_listener;

	struct wl_list text_inputs; /* struct bridge_text_input::link */

	void (*state_cb)(bool active, struct weston_surface *surface,
			 void *user_data);
	void *state_cb_user_data;
};

struct bridge_text_input {
	struct wl_resource *resource;
	struct ti_bridge *bridge;

	struct weston_seat *seat;
	struct weston_surface *surface; /* surface entered, or NULL */

	/* state committed by the client */
	bool enabled;
	int32_t cursor_x, cursor_y, cursor_w, cursor_h;
	bool have_cursor_rect;
	uint32_t content_hint;
	uint32_t content_purpose;

	/* state pending until the next commit request */
	bool pending_enabled;
	bool have_pending_cursor_rect;
	int32_t pending_cursor_x, pending_cursor_y;
	int32_t pending_cursor_w, pending_cursor_h;

	uint32_t commit_serial; /* last serial the client committed with */
	uint32_t done_serial;   /* serial of our last done() event */

	bool entered; /* we sent enter() and no leave() yet */

	/* keyboard_listener_attached_to doubles as the guard for removing
	 * keyboard_focus_listener.  weston_keyboard has no destroy signal;
	 * its object lives as long as the seat (release_keyboard only
	 * resets it), so death is detected through the seat's
	 * destroy_signal, which weston_seat_release emits after destroying
	 * the keyboard. */
	struct weston_keyboard *keyboard_listener_attached_to;
	struct wl_listener keyboard_focus_listener;
	bool seat_listener_attached;
	struct wl_listener seat_destroy_listener;

	/* same pattern for the entered surface */
	struct weston_surface *surface_listener_attached_to;
	struct wl_listener surface_destroy_listener;

	struct wl_list link;
};

/* One compositor per weston process; the API callbacks only receive the
 * struct weston_compositor, so keep the single instance here. */
static struct ti_bridge *g_bridge;

static void bridge_notify_state(struct ti_bridge *bridge);
static void ti_update_focus(struct bridge_text_input *ti);
static void ti_keyboard_focus_handler(struct wl_listener *listener,
				      void *data);
static void ti_seat_destroy_handler(struct wl_listener *listener,
				    void *data);

static struct bridge_text_input *
ti_from_resource(struct wl_resource *resource)
{
	return wl_resource_get_user_data(resource);
}

/* ------------------------------------------------------------------ */
/* enter/leave driving                                                 */
/* ------------------------------------------------------------------ */

static void
bridge_send_done(struct bridge_text_input *ti)
{
	zwp_text_input_v3_send_done(ti->resource, ++ti->done_serial);
}

static void
bridge_send_enter(struct bridge_text_input *ti)
{
	if (ti->entered || !ti->surface)
		return;

	zwp_text_input_v3_send_enter(ti->resource, ti->surface->resource);
	ti->entered = true;
}

static void
bridge_send_leave(struct bridge_text_input *ti)
{
	if (!ti->entered)
		return;

	zwp_text_input_v3_send_leave(ti->resource);
	ti->entered = false;
}

static struct weston_keyboard *
ti_get_keyboard(struct bridge_text_input *ti)
{
	return ti->seat ? weston_seat_get_keyboard(ti->seat) : NULL;
}

static void
ti_attach_keyboard_listener(struct bridge_text_input *ti)
{
	struct weston_keyboard *keyboard = ti_get_keyboard(ti);

	if (ti->keyboard_listener_attached_to || !keyboard)
		return;

	ti->keyboard_focus_listener.notify = ti_keyboard_focus_handler;
	wl_signal_add(&keyboard->focus_signal,
		      &ti->keyboard_focus_listener);
	if (!ti->seat_listener_attached) {
		ti->seat_destroy_listener.notify = ti_seat_destroy_handler;
		wl_signal_add(&ti->seat->destroy_signal,
			      &ti->seat_destroy_listener);
		ti->seat_listener_attached = true;
	}
	ti->keyboard_listener_attached_to = keyboard;
}

static void
ti_seat_destroy_handler(struct wl_listener *listener, void *data)
{
	struct bridge_text_input *ti =
		container_of(listener, struct bridge_text_input,
			     seat_destroy_listener);

	/* weston_seat_release() destroyed the keyboard before emitting this
	 * signal, so both signal lists are gone; forget about them without
	 * removing any links. */
	ti->keyboard_listener_attached_to = NULL;
	ti->seat_listener_attached = false;
	bridge_notify_state(ti->bridge);
}

static void
ti_surface_destroy_handler(struct wl_listener *listener, void *data)
{
	struct bridge_text_input *ti =
		container_of(listener, struct bridge_text_input,
			     surface_destroy_listener);

	/* Same as above: do not remove the listener link. */
	ti->surface_listener_attached_to = NULL;
	ti->surface = NULL;
	bridge_notify_state(ti->bridge);
}

static void
ti_keyboard_focus_handler(struct wl_listener *listener, void *data)
{
	struct bridge_text_input *ti =
		container_of(listener, struct bridge_text_input,
			     keyboard_focus_listener);

	ti_update_focus(ti);
	bridge_notify_state(ti->bridge);
}

/* Point the text input at a surface (or away from one) and keep the
 * surface destroy listener in sync. Sends leave() when moving away from
 * an entered surface. */
static void
ti_set_surface(struct bridge_text_input *ti, struct weston_surface *surface)
{
	if (ti->surface == surface)
		return;

	if (ti->surface_listener_attached_to) {
		wl_list_remove(&ti->surface_destroy_listener.link);
		ti->surface_listener_attached_to = NULL;
	}

	bridge_send_leave(ti);

	ti->surface = surface;
	ti->entered = false;

	if (ti->surface) {
		ti->surface_destroy_listener.notify =
			ti_surface_destroy_handler;
		wl_signal_add(&ti->surface->destroy_signal,
			      &ti->surface_destroy_listener);
		ti->surface_listener_attached_to = ti->surface;
	}
}

/* Re-evaluate which surface has keyboard focus for this text input and
 * drive the enter/leave events. */
static void
ti_update_focus(struct bridge_text_input *ti)
{
	struct weston_keyboard *keyboard = ti_get_keyboard(ti);
	struct weston_surface *focus = keyboard ? keyboard->focus : NULL;

	ti_attach_keyboard_listener(ti);

	if (focus &&
	    (!focus->resource ||
	     wl_resource_get_client(focus->resource) !=
	     wl_resource_get_client(ti->resource)))
		focus = NULL;

	if (ti->surface != focus)
		ti_set_surface(ti, focus);

	if (focus)
		bridge_send_enter(ti);
}

static void
bridge_notify_state(struct ti_bridge *bridge)
{
	struct bridge_text_input *ti, *active = NULL;
	struct weston_surface *surface = NULL;

	wl_list_for_each(ti, &bridge->text_inputs, link) {
		struct weston_keyboard *keyboard = ti_get_keyboard(ti);

		if (!ti->enabled || !ti->surface)
			continue;

		if (keyboard && keyboard->focus == ti->surface) {
			active = ti;
			surface = ti->surface;
			break;
		}
	}

	if (bridge->state_cb)
		bridge->state_cb(active != NULL, surface,
				 bridge->state_cb_user_data);
}

/* ------------------------------------------------------------------ */
/* zwp_text_input_v3 requests                                          */
/* ------------------------------------------------------------------ */

static void
ti_request_destroy(struct wl_client *client, struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

static void
ti_request_enable(struct wl_client *client, struct wl_resource *resource)
{
	struct bridge_text_input *ti = ti_from_resource(resource);

	ti->pending_enabled = true;
}

static void
ti_request_disable(struct wl_client *client, struct wl_resource *resource)
{
	struct bridge_text_input *ti = ti_from_resource(resource);

	ti->pending_enabled = false;
}

static void
ti_request_set_surrounding_text(struct wl_client *client,
				struct wl_resource *resource,
				const char *text, int32_t cursor,
				int32_t anchor)
{
	/* The remote IME does not consume surrounding text; ignored. */
}

static void
ti_request_set_text_change_cause(struct wl_client *client,
				 struct wl_resource *resource,
				 uint32_t cause)
{
}

static void
ti_request_set_content_type(struct wl_client *client,
			    struct wl_resource *resource,
			    uint32_t hint, uint32_t purpose)
{
	struct bridge_text_input *ti = ti_from_resource(resource);

	ti->content_hint = hint;
	ti->content_purpose = purpose;
}

static void
ti_request_set_cursor_rectangle(struct wl_client *client,
				struct wl_resource *resource,
				int32_t x, int32_t y,
				int32_t width, int32_t height)
{
	struct bridge_text_input *ti = ti_from_resource(resource);

	ti->pending_cursor_x = x;
	ti->pending_cursor_y = y;
	ti->pending_cursor_w = width;
	ti->pending_cursor_h = height;
	ti->have_pending_cursor_rect = true;
}

static void
ti_request_commit(struct wl_client *client, struct wl_resource *resource,
		  uint32_t serial)
{
	struct bridge_text_input *ti = ti_from_resource(resource);

	ti->commit_serial = serial;
	ti->enabled = ti->pending_enabled;

	if (ti->have_pending_cursor_rect) {
		ti->cursor_x = ti->pending_cursor_x;
		ti->cursor_y = ti->pending_cursor_y;
		ti->cursor_w = ti->pending_cursor_w;
		ti->cursor_h = ti->pending_cursor_h;
		ti->have_cursor_rect = true;
		ti->have_pending_cursor_rect = false;
	}

	/* re-check the keyboard in case it appeared after this text input
	 * was bound (RDP seats create the keyboard after the seat) */
	ti_update_focus(ti);

	bridge_notify_state(ti->bridge);
}

static const struct zwp_text_input_v3_interface ti_implementation = {
	.destroy = ti_request_destroy,
	.enable = ti_request_enable,
	.disable = ti_request_disable,
	.set_surrounding_text = ti_request_set_surrounding_text,
	.set_text_change_cause = ti_request_set_text_change_cause,
	.set_content_type = ti_request_set_content_type,
	.set_cursor_rectangle = ti_request_set_cursor_rectangle,
	.commit = ti_request_commit,
};

static void
ti_unbind(struct wl_resource *resource)
{
	struct bridge_text_input *ti = ti_from_resource(resource);

	bridge_send_leave(ti);

	if (ti->keyboard_listener_attached_to &&
	    ti->keyboard_listener_attached_to == ti_get_keyboard(ti))
		wl_list_remove(&ti->keyboard_focus_listener.link);

	if (ti->seat_listener_attached)
		wl_list_remove(&ti->seat_destroy_listener.link);

	if (ti->surface_listener_attached_to)
		wl_list_remove(&ti->surface_destroy_listener.link);

	wl_list_remove(&ti->link);
	bridge_notify_state(ti->bridge);
	free(ti);
}

static void
manager_get_text_input(struct wl_client *client,
		       struct wl_resource *manager_resource,
		       struct wl_resource *seat_resource, uint32_t id)
{
	struct ti_bridge *bridge = wl_resource_get_user_data(manager_resource);
	struct weston_seat *seat = wl_resource_get_user_data(seat_resource);
	struct bridge_text_input *ti;

	ti = zalloc(sizeof *ti);
	if (!ti) {
		wl_client_post_no_memory(client);
		return;
	}

	ti->bridge = bridge;
	ti->seat = seat;
	wl_list_init(&ti->link);
	wl_list_insert(bridge->text_inputs.prev, &ti->link);

	ti->resource = wl_resource_create(client,
					  &zwp_text_input_v3_interface,
					  wl_resource_get_version(manager_resource),
					  id);
	if (!ti->resource) {
		wl_list_remove(&ti->link);
		free(ti);
		wl_client_post_no_memory(client);
		return;
	}

	wl_resource_set_implementation(ti->resource, &ti_implementation, ti,
				       ti_unbind);

	/* The seat's keyboard may already have focus on the surface this
	 * client is about to enter; catch up and attach the listener. */
	ti_update_focus(ti);
}

static const struct zwp_text_input_manager_v3_interface
manager_implementation = {
	.get_text_input = manager_get_text_input,
};

static void
bind_text_input_manager(struct wl_client *client, void *data, uint32_t version,
			uint32_t id)
{
	struct ti_bridge *bridge = data;
	struct wl_resource *resource;

	resource = wl_resource_create(client,
				      &zwp_text_input_manager_v3_interface,
				      version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}

	wl_resource_set_implementation(resource, &manager_implementation,
				       bridge, NULL);
}

static void
bridge_destroy(struct wl_listener *listener, void *data)
{
	struct ti_bridge *bridge =
		container_of(listener, struct ti_bridge, destroy_listener);

	wl_list_remove(&bridge->destroy_listener.link);

	if (bridge->manager_global)
		wl_global_destroy(bridge->manager_global);

	if (g_bridge == bridge)
		g_bridge = NULL;

	free(bridge);
}

/* ------------------------------------------------------------------ */
/* Bridge API                                                          */
/* ------------------------------------------------------------------ */

static struct bridge_text_input *
bridge_find_active(struct ti_bridge *bridge)
{
	struct bridge_text_input *ti;

	wl_list_for_each(ti, &bridge->text_inputs, link) {
		struct weston_keyboard *keyboard = ti_get_keyboard(ti);

		if (ti->enabled && ti->surface && keyboard &&
		    keyboard->focus == ti->surface)
			return ti;
	}

	return NULL;
}

static bool
bridge_api_get_active(struct weston_compositor *ec,
		      struct weston_surface **out_surface)
{
	struct bridge_text_input *ti;

	if (!g_bridge || g_bridge->ec != ec)
		return false;

	ti = bridge_find_active(g_bridge);
	if (out_surface)
		*out_surface = ti ? ti->surface : NULL;

	return ti != NULL;
}

static bool
bridge_api_get_cursor_rect(struct weston_compositor *ec,
			   int32_t *x, int32_t *y,
			   int32_t *width, int32_t *height)
{
	struct bridge_text_input *ti;

	if (!g_bridge || g_bridge->ec != ec)
		return false;

	ti = bridge_find_active(g_bridge);
	if (!ti || !ti->have_cursor_rect)
		return false;

	*x = ti->cursor_x;
	*y = ti->cursor_y;
	*width = ti->cursor_w;
	*height = ti->cursor_h;
	return true;
}

static void
bridge_api_send_preedit(struct weston_compositor *ec, const char *text,
			int32_t cursor_begin, int32_t cursor_end)
{
	struct bridge_text_input *ti;
	size_t len;

	if (!g_bridge || g_bridge->ec != ec)
		return;

	ti = bridge_find_active(g_bridge);
	if (!ti)
		return;

	if (!text)
		text = "";

	if (cursor_begin < 0 || cursor_end < 0) {
		len = strlen(text);
		cursor_begin = (int32_t)len;
		cursor_end = (int32_t)len;
	}

	zwp_text_input_v3_send_preedit_string(ti->resource, text,
					      cursor_begin, cursor_end);
	bridge_send_done(ti);
}

static void
bridge_api_send_commit(struct weston_compositor *ec, const char *text)
{
	struct bridge_text_input *ti;

	if (!g_bridge || g_bridge->ec != ec)
		return;

	ti = bridge_find_active(g_bridge);
	if (!ti)
		return;

	if (!text)
		text = "";

	/* Clear any preedit, then commit. Both apply on the done() below. */
	zwp_text_input_v3_send_preedit_string(ti->resource, "", 0, 0);
	zwp_text_input_v3_send_commit_string(ti->resource, text);
	bridge_send_done(ti);
}

static void
bridge_api_set_state_listener(struct weston_compositor *ec,
			      void (*cb)(bool active,
					 struct weston_surface *surface,
					 void *user_data),
			      void *user_data)
{
	if (!g_bridge || g_bridge->ec != ec)
		return;

	g_bridge->state_cb = cb;
	g_bridge->state_cb_user_data = user_data;
}

static const struct weston_text_input_bridge_api bridge_api = {
	.get_active = bridge_api_get_active,
	.get_cursor_rect = bridge_api_get_cursor_rect,
	.send_preedit = bridge_api_send_preedit,
	.send_commit = bridge_api_send_commit,
	.set_state_listener = bridge_api_set_state_listener,
};

/* ------------------------------------------------------------------ */
/* Init                                                                */
/* ------------------------------------------------------------------ */

int
text_input_bridge_init(struct weston_compositor *ec)
{
	struct ti_bridge *bridge;
	int ret;

	if (g_bridge) {
		weston_log("text-input-bridge: already initialized\n");
		return 0;
	}

	bridge = zalloc(sizeof *bridge);
	if (!bridge)
		return -1;

	bridge->ec = ec;
	wl_list_init(&bridge->text_inputs);
	bridge->destroy_listener.notify = bridge_destroy;
	wl_signal_add(&ec->destroy_signal, &bridge->destroy_listener);

	/* advertise interface version 1; version 2 only adds APIs this
	 * bridge does not implement */
	bridge->manager_global =
		wl_global_create(ec->wl_display,
				 &zwp_text_input_manager_v3_interface, 1,
				 bridge, bind_text_input_manager);
	if (!bridge->manager_global) {
		wl_list_remove(&bridge->destroy_listener.link);
		free(bridge);
		return -1;
	}

	ret = weston_plugin_api_register(ec,
					 WESTON_TEXT_INPUT_BRIDGE_API_NAME,
					 &bridge_api, sizeof(bridge_api));
	if (ret < 0) {
		weston_log("text-input-bridge: failed to register plugin API\n");
		wl_global_destroy(bridge->manager_global);
		wl_list_remove(&bridge->destroy_listener.link);
		free(bridge);
		return -1;
	}

	g_bridge = bridge;

	weston_log("text-input-bridge: zwp_text_input_v3 support enabled\n");
	return 0;
}
