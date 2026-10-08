

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
	
	bool (*get_active)(struct weston_compositor *ec,
			   struct weston_surface **out_surface);

	
	bool (*get_cursor_rect)(struct weston_compositor *ec,
				int32_t *x, int32_t *y,
				int32_t *width, int32_t *height);

	
	void (*send_preedit)(struct weston_compositor *ec,
			     const char *text,
			     int32_t cursor_begin, int32_t cursor_end);

	
	void (*send_commit)(struct weston_compositor *ec, const char *text);

	
	void (*set_state_listener)(struct weston_compositor *ec,
				   void (*cb)(bool active,
					      struct weston_surface *surface,
					      void *user_data),
				   void *user_data);

	

	
	void (*set_xim_sink)(struct weston_compositor *ec,
			     void (*cb)(const char *text, void *user_data),
			     void *user_data);

	
	void (*set_xim_focus)(struct weston_compositor *ec,
			      bool focused, int32_t x, int32_t y);

	
	bool (*get_remote_composing)(struct weston_compositor *ec);
};

#ifdef __cplusplus
}
#endif

#endif 
