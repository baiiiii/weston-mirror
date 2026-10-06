/*
 * Copyright © 2026
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
 */

/*
 * XIM server for the MS-RDPETXT bridge.
 *
 * Wayland clients get text input through zwp_text_input_v3, which the
 * text-input-bridge implements.  X11 clients (running through XWayland) never
 * speak that protocol: they use XIM, and XIM servers are normally provided by
 * a Linux input method framework.  In WSLg there is no such framework - the
 * input method lives on the Windows side - so this module provides a
 * minimal XIM server that relays to the existing bridge:
 *
 *   X11 app --XIM--> this module --in-process--> bridge --> rdptext
 *                                                        --> WSLDVCPlugin
 *                                                        --> Windows IME
 *
 * and the committed text travels back along the same path into
 * xcb_im_commit_string().
 *
 * Only the "root window" input style is offered
 * (XIMPreeditNothing | XIMStatusNothing): the application does not draw the
 * preedit and does not own a status area, so the remote IME's own window shows
 * the candidates and composition, exactly as it does for Wayland clients.
 *
 * The XIM wire protocol itself is handled by xcb-imdkit (LGPL-2.1-only,
 * dynamically linked); this file is only glue.
 *
 * Load with:  weston --modules=...,xim-server.so
 * The server name defaults to "wslg-xim" and can be overridden with the
 * XIM_SERVER_NAME environment variable; clients select it with
 *   XMODIFIERS=@im=wslg-xim
 */

#include "config.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <xcb/xcb.h>
#include <xcb-imdkit/imdkit.h>

#include <libweston/libweston.h>
#include <libweston/text-input-bridge.h>

#include "shared/helpers.h"
#include "weston.h"

/* XIMPreeditNothing | XIMStatusNothing -- the values come from X11/Xlib.h
 * (XIMPreeditNothing 0x0008, XIMStatusNothing 0x0400); xcb-imdkit uses the
 * same protocol bit values.  This is the "root window" style: the client
 * neither draws the preedit nor owns a status area. */
#define XIM_STYLE_NOTHING (0x0008u | 0x0400u)

#define DEFAULT_SERVER_NAME "wslg-xim"

struct xim_server {
	struct weston_compositor *ec;
	const struct weston_text_input_bridge_api *bridge;

	xcb_connection_t *conn;
	int screen_num;
	xcb_screen_t *screen;
	xcb_window_t server_window;
	xcb_im_t *im;

	struct wl_event_source *xcb_source;
	struct wl_listener destroy_listener;

	char *server_name;

	/* current input context that owns the keyboard focus */
	xcb_im_input_context_t *focus_ic;
	bool warned_no_encoding;
};

static struct xim_server *g_xim;

/* ------------------------------------------------------------------ */
/* helpers                                                            */
/* ------------------------------------------------------------------ */

static void
xim_update_focus(struct xim_server *srv)
{
	struct xcb_im_preedit_attr_t *attr;

	if (!srv->focus_ic) {
		srv->bridge->set_xim_focus(srv->ec, false, 0, 0);
		return;
	}

	attr = (struct xcb_im_preedit_attr_t *)
		xcb_im_input_context_get_preedit_attr(srv->focus_ic);
	if (!attr ||
	    !(xcb_im_input_context_get_preedit_attr_mask(srv->focus_ic) &
	      XCB_XIM_XNSpotLocation_MASK)) {
		/* The client did not report a spot yet: report focus without a
		 * usable position so the remote IME keeps its previous anchor
		 * rather than jumping to a corner. */
		srv->bridge->set_xim_focus(srv->ec, true, 0, 0);
		return;
	}

	/* XNSpotLocation is in X11 screen (root window) coordinates; WSLg
	 * runs XWayland at the same size as the weston output, so for the
	 * single-output, unscaled case the values can be handed to the bridge
	 * as compositor coordinates.  See the design document for the
	 * multi-output / HiDPI follow-up. */
	srv->bridge->set_xim_focus(srv->ec, true,
				   (int32_t)attr->spot_location.x,
				   (int32_t)attr->spot_location.y);
}

/* Called by the bridge (from rdptext) when the remote IME commits text. */
static void
xim_bridge_commit(const char *text, void *user_data)
{
	struct xim_server *srv = user_data;

	if (!srv || !srv->im || !text || !text[0])
		return;

	if (!srv->focus_ic) {
		/* No X11 client has the focus: nothing to deliver to. */
		return;
	}

	/* Committing UTF-8 requires the client to have negotiated UTF-8; we
	 * advertise it in the encoding list below. */
	xcb_im_commit_string(srv->im, srv->focus_ic,
			     XCB_XIM_LOOKUP_CHARS,
			     text, (uint32_t)strlen(text), 0);
	xcb_flush(srv->conn);
}

/* ------------------------------------------------------------------ */
/* XIM callback                                                       */
/* ------------------------------------------------------------------ */

static void
xim_callback(xcb_im_t *im, xcb_im_client_t *client,
	     xcb_im_input_context_t *ic,
	     const xcb_im_packet_header_fr_t *hdr,
	     void *frame, void *arg, void *user_data)
{
	struct xim_server *srv = user_data;

	(void)client;
	(void)frame;
	(void)arg;

	if (!srv || !ic || !hdr)
		return;

	switch (hdr->major_code) {
	case XCB_XIM_CREATE_IC:
		weston_log("xim-server: input context created (style=0x%x)\n",
			   xcb_im_input_context_get_input_style(ic));
		break;

	case XCB_XIM_DESTROY_IC:
		if (srv->focus_ic == ic) {
			srv->focus_ic = NULL;
			srv->bridge->set_xim_focus(srv->ec, false, 0, 0);
		}
		break;

	case XCB_XIM_SET_IC_VALUES:
		/* The application moved the caret. */
		if (srv->focus_ic == ic)
			xim_update_focus(srv);
		break;

	case XCB_XIM_SET_IC_FOCUS:
		srv->focus_ic = ic;
		weston_log("xim-server: focus window=0x%x\n",
			   xcb_im_input_context_get_focus_window(ic));
		xim_update_focus(srv);
		break;

	case XCB_XIM_UNSET_IC_FOCUS:
		if (srv->focus_ic == ic) {
			srv->focus_ic = NULL;
			srv->bridge->set_xim_focus(srv->ec, false, 0, 0);
		}
		break;

	case XCB_XIM_FORWARD_EVENT: {
		xcb_key_press_event_t *ev = frame;

		/* The keys already reach both sides (the physical keyboard is
		 * on Windows and weston forwards them to X), so the remote IME
		 * is composing on its own.  All we decide here is whether the
		 * application may see the key:
		 *
		 *  - remote IME composing  -> swallow it, so letters do not
		 *    get inserted while the composition is running;
		 *  - otherwise             -> re-inject it, so ordinary typing
		 *    (ASCII, shortcuts) keeps working.
		 */
		if (srv->bridge->get_remote_composing(srv->ec)) {
			/* consumed: do not forward back to the client */
			break;
		}
		if (ev)
			xcb_im_forward_event(im, ic, ev);
		break;
	}

	case XCB_XIM_RESET_IC:
		break;

	default:
		break;
	}
}

/* ------------------------------------------------------------------ */
/* xcb event pump, driven by the weston event loop                    */
/* ------------------------------------------------------------------ */

static int
xim_handle_xcb_events(int fd, uint32_t mask, void *data)
{
	struct xim_server *srv = data;
	xcb_generic_event_t *ev;

	if (!(mask & WL_EVENT_READABLE))
		return 0;

	while ((ev = xcb_poll_for_event(srv->conn))) {
		xcb_im_filter_event(srv->im, ev);
		free(ev);
	}

	if (xcb_connection_has_error(srv->conn)) {
		weston_log("xim-server: X connection lost\n");
		return 0;
	}

	xcb_flush(srv->conn);
	return 0;
}

static void
xim_destroy(struct xim_server *srv)
{
	if (srv->xcb_source) {
		wl_event_source_remove(srv->xcb_source);
		srv->xcb_source = NULL;
	}

	if (srv->bridge)
		srv->bridge->set_xim_sink(srv->ec, NULL, NULL);

	if (srv->im) {
		xcb_im_close_im(srv->im);
		xcb_im_destroy(srv->im);
		srv->im = NULL;
	}

	if (srv->conn) {
		xcb_disconnect(srv->conn);
		srv->conn = NULL;
	}

	free(srv->server_name);
	free(srv);
}

static void
xim_compositor_destroy(struct wl_listener *listener, void *data)
{
	struct xim_server *srv =
		container_of(listener, struct xim_server, destroy_listener);

	(void)data;

	if (g_xim == srv)
		g_xim = NULL;

	xim_destroy(srv);
}

/* ------------------------------------------------------------------ */
/* module entrypoint                                                  */
/* ------------------------------------------------------------------ */

WL_EXPORT int
wet_module_init(struct weston_compositor *ec, int *argc, char *argv[])
{
	struct xim_server *srv;
	struct wl_event_loop *loop;
	const char *display;
	const char *name;
	xcb_im_styles_t styles;
	uint32_t style_list[1];
	xcb_im_encodings_t encodings;
	char *encoding_list[2];
	int fd;

	(void)argc;
	(void)argv;

	if (g_xim) {
		weston_log("xim-server: already initialised\n");
		return 0;
	}

	srv = zalloc(sizeof *srv);
	if (!srv)
		return -1;

	srv->ec = ec;

	srv->bridge = weston_plugin_api_get(ec,
					    WESTON_TEXT_INPUT_BRIDGE_API_NAME,
					    sizeof(struct weston_text_input_bridge_api));
	if (!srv->bridge) {
		weston_log("xim-server: text-input bridge API not available; "
			   "X11 input method disabled\n");
		free(srv);
		return 0;   /* not fatal: Wayland clients keep working */
	}

	display = getenv("DISPLAY");
	if (!display || !display[0]) {
		weston_log("xim-server: DISPLAY is not set; X11 input method "
			   "disabled\n");
		free(srv);
		return 0;
	}

	name = getenv("XIM_SERVER_NAME");
	if (!name || !name[0])
		name = DEFAULT_SERVER_NAME;
	srv->server_name = strdup(name);
	if (!srv->server_name) {
		free(srv);
		return -1;
	}

	srv->conn = xcb_connect(display, &srv->screen_num);
	if (!srv->conn || xcb_connection_has_error(srv->conn)) {
		weston_log("xim-server: cannot connect to X display %s\n",
			   display);
		if (srv->conn)
			xcb_disconnect(srv->conn);
		free(srv->server_name);
		free(srv);
		return 0;
	}

	srv->screen = xcb_setup_roots_iterator(xcb_get_setup(srv->conn)).data;
	if (!srv->screen) {
		weston_log("xim-server: X display %s has no screen\n", display);
		xim_destroy(srv);
		return 0;
	}

	/* A tiny input-only window is enough: xcb-imdkit uses it to receive the
	 * client's ClientMessage handshake. */
	srv->server_window = xcb_generate_id(srv->conn);
	xcb_create_window(srv->conn, XCB_COPY_FROM_PARENT, srv->server_window,
			  srv->screen->root, 0, 0, 1, 1, 0,
			  XCB_WINDOW_CLASS_INPUT_OUTPUT,
			  srv->screen->root_visual, 0, NULL);

	style_list[0] = XIM_STYLE_NOTHING;
	styles.nStyles = 1;
	styles.styles = style_list;

	encoding_list[0] = (char *)"UTF-8";
	encoding_list[1] = NULL;
	encodings.nEncodings = 1;
	encodings.encodings = encoding_list;

	srv->im = xcb_im_create(srv->conn, srv->screen_num, srv->server_window,
				srv->server_name, XCB_IM_ALL_LOCALES,
				&styles, NULL, NULL, &encodings, 0,
				xim_callback, srv);
	if (!srv->im) {
		weston_log("xim-server: xcb_im_create failed\n");
		xim_destroy(srv);
		return 0;
	}

	if (!xcb_im_open_im(srv->im)) {
		weston_log("xim-server: cannot claim the XIM server name '%s' "
			   "(another input method may own it)\n",
			   srv->server_name);
		xim_destroy(srv);
		return 0;
	}

	xcb_flush(srv->conn);

	fd = xcb_get_file_descriptor(srv->conn);
	loop = wl_display_get_event_loop(ec->wl_display);
	srv->xcb_source = wl_event_loop_add_fd(loop, fd, WL_EVENT_READABLE,
					       xim_handle_xcb_events, srv);
	if (!srv->xcb_source) {
		weston_log("xim-server: cannot add the X connection to the "
			   "event loop\n");
		xim_destroy(srv);
		return 0;
	}

	srv->destroy_listener.notify = xim_compositor_destroy;
	wl_signal_add(&ec->destroy_signal, &srv->destroy_listener);

	/* From now on the bridge routes commits that have no Wayland target
	 * here. */
	srv->bridge->set_xim_sink(ec, xim_bridge_commit, srv);

	g_xim = srv;

	weston_log("xim-server: serving XIM on %s as '%s' "
		   "(style=XIMPreeditNothing|XIMStatusNothing, encoding=UTF-8)\n",
		   display, srv->server_name);
	weston_log("xim-server: clients should use XMODIFIERS=@im=%s\n",
		   srv->server_name);

	return 0;
}
