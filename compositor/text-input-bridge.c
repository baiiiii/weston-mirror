

#include "config.h"

#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include <libweston/libweston.h>
#include <libweston/plugin-registry.h>
#include <libweston/text-input-bridge.h>

#include "weston.h"
#include "text-input-unstable-v3-server-protocol.h"
#include "shared/helpers.h"

struct ti_bridge {
	struct weston_compositor *ec;
	struct wl_global *manager_global;
	struct wl_listener destroy_listener;

	struct wl_list text_inputs; 

	void (*state_cb)(bool active, struct weston_surface *surface,
			 void *user_data);
	void *state_cb_user_data;

	
	void (*xim_commit_cb)(const char *text, void *user_data);
	void *xim_commit_user_data;
	bool xim_focus;
	int32_t xim_x, xim_y;
	bool remote_composing;
};

struct bridge_text_input {
	struct wl_resource *resource;
	struct ti_bridge *bridge;

	struct weston_seat *seat;
	struct weston_surface *surface; 

	
	bool enabled;
	int32_t cursor_x, cursor_y, cursor_w, cursor_h;
	bool have_cursor_rect;
	uint32_t content_hint;
	uint32_t content_purpose;

	
	bool pending_enabled;
	bool have_pending_cursor_rect;
	int32_t pending_cursor_x, pending_cursor_y;
	int32_t pending_cursor_w, pending_cursor_h;

	uint32_t done_serial;   

	bool entered; 

	
	struct weston_keyboard *keyboard_listener_attached_to;
	struct wl_listener keyboard_focus_listener;
	bool seat_listener_attached;
	struct wl_listener seat_destroy_listener;

	
	struct weston_surface *surface_listener_attached_to;
	struct wl_listener surface_destroy_listener;

	struct wl_list link;
};

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

	
	if (ti->surface && ti->surface->resource)
		zwp_text_input_v3_send_leave(ti->resource,
					     ti->surface->resource);
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

	
	ti->surface_listener_attached_to = NULL;
	ti->entered = false;
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
ti_request_commit(struct wl_client *client, struct wl_resource *resource)
{
	struct bridge_text_input *ti = ti_from_resource(resource);

	ti->enabled = ti->pending_enabled;

	if (ti->have_pending_cursor_rect) {
		ti->cursor_x = ti->pending_cursor_x;
		ti->cursor_y = ti->pending_cursor_y;
		ti->cursor_w = ti->pending_cursor_w;
		ti->cursor_h = ti->pending_cursor_h;
		ti->have_cursor_rect = true;
		ti->have_pending_cursor_rect = false;
	}

	
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
		       uint32_t id,
		       struct wl_resource *seat_resource)
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

	
	ti_update_focus(ti);
}

static void
manager_destroy(struct wl_client *client, struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

static const struct zwp_text_input_manager_v3_interface
manager_implementation = {
	.destroy = manager_destroy,
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

	if (!text)
		text = "";

	
	g_bridge->remote_composing = (text[0] != '\0');

	ti = bridge_find_active(g_bridge);
	if (!ti)
		return;   

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

	if (!text)
		text = "";

	
	g_bridge->remote_composing = false;

	ti = bridge_find_active(g_bridge);
	if (!ti) {
		
		if (g_bridge->xim_commit_cb)
			g_bridge->xim_commit_cb(text,
						g_bridge->xim_commit_user_data);
		return;
	}

	
	zwp_text_input_v3_send_preedit_string(ti->resource, "", 0, 0);
	zwp_text_input_v3_send_commit_string(ti->resource, text);
	bridge_send_done(ti);
}

static void
bridge_api_set_xim_sink(struct weston_compositor *ec,
			void (*cb)(const char *text, void *user_data),
			void *user_data)
{
	if (!g_bridge || g_bridge->ec != ec)
		return;

	g_bridge->xim_commit_cb = cb;
	g_bridge->xim_commit_user_data = user_data;
}

static void
bridge_api_set_xim_focus(struct weston_compositor *ec,
			 bool focused, int32_t x, int32_t y)
{
	if (!g_bridge || g_bridge->ec != ec)
		return;

	g_bridge->xim_focus = focused;
	g_bridge->xim_x = x;
	g_bridge->xim_y = y;
}

static bool
bridge_api_get_remote_composing(struct weston_compositor *ec)
{
	if (!g_bridge || g_bridge->ec != ec)
		return false;

	return g_bridge->remote_composing;
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
	.set_xim_sink = bridge_api_set_xim_sink,
	.set_xim_focus = bridge_api_set_xim_focus,
	.get_remote_composing = bridge_api_get_remote_composing,
};

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
