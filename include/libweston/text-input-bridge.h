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
 * Text input bridge API.
 *
 * The bridge lives in the weston executable (compositor/text-input-bridge.c)
 * and implements the compositor side of zwp_text_input_v3.  It exposes a
 * small in-process API, registered through the weston plugin-registry, that
 * lets an IME provider living elsewhere in the compositor - such as the
 * MS-RDPETXT server in the RDP backend (libweston/backend-rdp/rdptext.c) -
 * observe which client is ready to receive text and push preedit/commit
 * strings into it.
 *
 * Retrieve with:
 *     weston_plugin_api_get(compositor, WESTON_TEXT_INPUT_BRIDGE_API_NAME,
 *                           sizeof(struct weston_text_input_bridge_api));
 */

#ifndef WESTON_TEXT_INPUT_BRIDGE_H
#define WESTON_TEXT_INPUT_BRIDGE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct weston_compositor;
struct weston_surface;

#define WESTON_TEXT_INPUT_BRIDGE_API_NAME "weston_text_input_bridge_v1"
#define WESTON_TEXT_INPUT_BRIDGE_API_VERSION 1

struct weston_text_input_bridge_api {
	/**
	 * Return true when a text input client is active, i.e. it enabled
	 * text input via zwp_text_input_v3 and its surface currently has
	 * keyboard focus.  When true, the focused surface is stored in
	 * *out_surface (may be NULL if the caller is not interested).
	 */
	bool (*get_active)(struct weston_compositor *ec,
			   struct weston_surface **out_surface);

	/**
	 * Cursor rectangle, in surface-local coordinates, as last reported
	 * by the active text input client (zwp_text_input_v3
	 * set_cursor_rectangle).  Returns false when there is no active
	 * text input or the client did not report a rectangle.
	 */
	bool (*get_cursor_rect)(struct weston_compositor *ec,
				int32_t *x, int32_t *y,
				int32_t *width, int32_t *height);

	/**
	 * Push a preedit (in-progress composition) string to the active
	 * text input client.  cursor_begin/cursor_end are character
	 * offsets into text; pass -1, -1 to place the cursor at the end.
	 * An empty text clears the preedit.  No-op when no text input is
	 * active.
	 */
	void (*send_preedit)(struct weston_compositor *ec,
			     const char *text,
			     int32_t cursor_begin, int32_t cursor_end);

	/**
	 * Push a committed string to the active text input client and
	 * clear its preedit.  No-op when no text input is active.
	 */
	void (*send_commit)(struct weston_compositor *ec, const char *text);

	/**
	 * Register a callback fired whenever the active text input state
	 * changes (a client enabled/disabled text input on the focused
	 * surface, or keyboard focus moved).  cb() receives true and the
	 * focused surface when a text input became active, false with a
	 * NULL surface when it went away.  Only one listener is supported;
	 * pass NULL to remove.
	 */
	void (*set_state_listener)(struct weston_compositor *ec,
				   void (*cb)(bool active,
					      struct weston_surface *surface,
					      void *user_data),
				   void *user_data);

	/* ---------------- X11 (XIM) support ---------------- */

	/**
	 * Register a sink for the X11/XIM path.
	 *
	 * The Wayland path delivers preedit/commit to the active
	 * zwp_text_input_v3 client; X11 applications never speak that
	 * protocol, so an XIM server has to relay the text instead.  When no
	 * Wayland text input client is active, send_commit()/send_preedit()
	 * fall back to this sink.  cb() is called with the committed UTF-8
	 * text.  Pass NULL to unregister.  Only one sink is supported.
	 */
	void (*set_xim_sink)(struct weston_compositor *ec,
			     void (*cb)(const char *text, void *user_data),
			     void *user_data);

	/**
	 * Tell the bridge where the caret of the focused X11 window is, in
	 * compositor (logical) coordinates.  This becomes the candidate
	 * window anchor for the remote IME, exactly like the cursor
	 * rectangle reported by a Wayland client.  focused=false clears it.
	 */
	void (*set_xim_focus)(struct weston_compositor *ec,
			      bool focused, int32_t x, int32_t y);

	/**
	 * True while the remote (Windows) IME is composing: preedit text has
	 * been pushed and no commit has arrived yet.  An XIM server uses this
	 * to decide whether to consume a key (keep it away from the
	 * application) or let it through.
	 */
	bool (*get_remote_composing)(struct weston_compositor *ec);
};

#ifdef __cplusplus
}
#endif

#endif /* WESTON_TEXT_INPUT_BRIDGE_H */
