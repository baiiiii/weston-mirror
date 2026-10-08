

#include "config.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <execinfo.h>
#include <signal.h>
#include <unistd.h>
#include <unistd.h>
#include <string.h>

#include <X11/Xlib.h>
#include <xcb/xcb.h>
#include <libweston/xwayland-api.h>

#include <libweston/libweston.h>
#include <libweston/plugin-registry.h>
#include <libweston/text-input-bridge.h>

#include "shared/helpers.h"
#include "weston.h"

#define XIM_CONNECT			1
#define XIM_CONNECT_REPLY		2
#define XIM_DISCONNECT			3
#define XIM_OPEN			30
#define XIM_OPEN_REPLY			31
#define XIM_CLOSE			32
#define XIM_CLOSE_REPLY			33
#define XIM_ENCODING_NEGOTIATION	38
#define XIM_ENCODING_NEGOTIATION_REPLY	39
#define XIM_QUERY_EXTENSION		40
#define XIM_QUERY_EXTENSION_REPLY	41
#define XIM_GET_IM_VALUES		44
#define XIM_GET_IM_VALUES_REPLY		45
#define XIM_CREATE_IC			50
#define XIM_CREATE_IC_REPLY		51
#define XIM_DESTROY_IC			52
#define XIM_DESTROY_IC_REPLY		53
#define XIM_SET_IC_VALUES		54
#define XIM_SET_IC_VALUES_REPLY		55
#define XIM_GET_IC_VALUES		56
#define XIM_GET_IC_VALUES_REPLY		57
#define XIM_SET_IC_FOCUS		58
#define XIM_UNSET_IC_FOCUS		59
#define XIM_FORWARD_EVENT		60
#define XIM_SYNC			61
#define XIM_SYNC_REPLY			62
#define XIM_COMMIT			63
#define XIM_RESET_IC			64
#define XIM_RESET_IC_REPLY		65

#define XIM_BIGENDIAN			0x42
#define XIM_LITTLEENDIAN		0x6c

#define XimType_CARD32			3
#define XimType_XIMStyles		10
#define XimType_XPoint			12

#define XimLookupChars			0x0002

#define XIM_STYLE_NOTHING		(0x0008u | 0x0400u)

#define DEFAULT_SERVER_NAME		"wslg-xim"
#define XIM_PROTOCOL_MAJOR		1
#define XIM_PROTOCOL_MINOR		0
#define XIM_HEADER_SIZE			4
#define XIM_MAX_PACKET			(1024 * 1024)
#define XIM_PAD(len)			((4 - ((len) % 4)) % 4)

struct xim_server;

struct xim_ic {
	struct wl_list link;
	struct xim_client *client;
	uint16_t icid;
	xcb_window_t client_win;
	xcb_window_t focus_win;
	uint32_t input_style;
	int32_t spot_x, spot_y;
	bool has_spot;
	bool focused;
};

struct xim_client {
	struct wl_list link;
	struct xim_server *srv;
	xcb_window_t client_win;
	xcb_window_t comm_win;
	uint16_t imid;
	uint8_t order;
	bool order_known;
	bool opened;
	struct wl_list ics;
	struct xim_ic *focus_ic;
	uint8_t *rx;
	size_t rx_len, rx_cap;
};

struct xim_server {
	struct weston_compositor *ec;
	const struct weston_text_input_bridge_api *bridge;
	xcb_connection_t *conn;
	struct wl_event_source *xcb_source;
	struct wl_event_source *retry_timer;
	struct wl_event_source *efd_source;
	int efd;
	struct wl_listener destroy_listener;
	xcb_screen_t *screen;
	xcb_window_t server_win;
	xcb_atom_t a_xim_servers, a_xim_xconnect, a_xim_protocol, a_xim_moredata;
	xcb_atom_t a_comm, a_input_style, a_spot_location;
	xcb_atom_t a_client_window, a_focus_window;
	char *name;
	struct wl_list clients;
	uint16_t next_imid, next_icid;
	bool connected;
	bool connecting;
};

static struct xim_server *g_xim;

struct xim_xwm_proxy {
	xcb_connection_t *conn;
};

struct xim_xserver_proxy {
	struct wl_display *wl_display;
	struct wl_event_loop *loop;
	int abstract_fd;
	struct wl_event_source *abstract_source;
	int unix_fd;
	struct wl_event_source *unix_source;
	int display;
	pid_t pid;
	struct wl_client *client;
	struct weston_compositor *compositor;
	struct xim_xwm_proxy *wm;
	struct wl_listener destroy_listener;
};
static xcb_connection_t *
xim_wm_connection(struct weston_compositor *ec)
{
	const struct weston_xwayland_api *api;
	struct weston_xwayland *xw;
	struct xim_xserver_proxy *xsp;
	struct xim_xwm_proxy *wm;

	api = weston_xwayland_get_api(ec);
	xim_mark(api ? "XIM-wm-api-ok" : "XIM-wm-api-null");
	if (!api || !api->get)
		return NULL;
	xw = api->get(ec);
	xim_mark(xw ? "XIM-wm-get-ok" : "XIM-wm-get-null");
	if (!xw)
		return NULL;
	xsp = (struct xim_xserver_proxy *)xw;
	wm = xsp->wm;
	xim_mark(wm ? "XIM-wm-ptr-ok" : "XIM-wm-ptr-null");
	if (!wm || !wm->conn)
		return NULL;
	xim_mark(wm->conn ? "XIM-wm-conn-ok" : "XIM-wm-conn-null");
	if (xcb_connection_has_error(wm->conn))
		return NULL;
	return wm->conn;
}

static void
xim_mark(const char *s)
{
	write(STDERR_FILENO, s, strlen(s));
	write(STDERR_FILENO, "\n", 1);
}

struct weston_xim_server_api {

	int (*init)(struct weston_compositor *ec);

	void (*request)(struct weston_compositor *ec);
};

static bool xim_display_socket_ready(const char *display);
static xcb_connection_t *xim_connect_display(const char *display);

#define WESTON_XIM_SERVER_API_NAME "weston_xim_server_v1"

static int
xim_efd_cb(int fd, uint32_t mask, void *data)
{
	struct xim_server *srv = data;
	uint64_t v;

	if (mask & WL_EVENT_READABLE) {
		while (read(fd, &v, sizeof v) == (ssize_t)sizeof v)
			;
		if (!srv->connected)
			xim_server_init(srv->ec);	
	}
	return 0;
}

static void
xim_server_request(struct weston_compositor *ec)
{
	struct xim_server *srv = g_xim; 	uint64_t v = 1;  	weston_log("xim-server: REQ srv=%p efd=%d connected=%d conn=%p\n", 			(void *)srv, srv ? srv->efd : -1, 			srv ? srv->connected : 0, srv ? (void *)srv->conn : NULL); 	if (!srv || !srv->efd || srv->connected) 		return;  	if (write(srv->efd, &v, sizeof v) != (ssize_t)sizeof v) { 	} 	weston_log("xim-server: REQ poked eventfd\n");
}

int xim_server_init(struct weston_compositor *ec);

static uint16_t
rd16(const uint8_t *p, uint8_t order)
{
	if (order == XIM_BIGENDIAN)
		return (uint16_t)((p[0] << 8) | p[1]);
	return (uint16_t)((p[1] << 8) | p[0]);
}

static uint32_t
rd32(const uint8_t *p, uint8_t order)
{
	if (order == XIM_BIGENDIAN)
		return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
		       ((uint32_t)p[2] << 8) | p[3];
	return ((uint32_t)p[3] << 24) | ((uint32_t)p[2] << 16) |
	       ((uint32_t)p[1] << 8) | p[0];
}

static void
wr16(uint8_t *p, uint16_t v, uint8_t order)
{
	if (order == XIM_BIGENDIAN) {
		p[0] = (uint8_t)(v >> 8);
		p[1] = (uint8_t)v;
	} else {
		p[0] = (uint8_t)v;
		p[1] = (uint8_t)(v >> 8);
	}
}

static void
wr32(uint8_t *p, uint32_t v, uint8_t order)
{
	if (order == XIM_BIGENDIAN) {
		p[0] = (uint8_t)(v >> 24);
		p[1] = (uint8_t)(v >> 16);
		p[2] = (uint8_t)(v >> 8);
		p[3] = (uint8_t)v;
	} else {
		p[0] = (uint8_t)v;
		p[1] = (uint8_t)(v >> 8);
		p[2] = (uint8_t)(v >> 16);
		p[3] = (uint8_t)(v >> 24);
	}
}

struct xbuf {
	uint8_t *data;
	size_t len, cap;
	uint8_t order;
};

static bool
xb_reserve(struct xbuf *b, size_t extra)
{
	size_t need = b->len + extra, cap;
	uint8_t *n;

	if (need <= b->cap)
		return true;
	cap = b->cap ? b->cap : 256;
	while (cap < need)
		cap *= 2;
	n = realloc(b->data, cap);
	if (!n)
		return false;
	b->data = n;
	b->cap = cap;
	return true;
}

static void xb_init(struct xbuf *b, uint8_t order)
{
	b->data = NULL; b->len = b->cap = 0; b->order = order;
}

static void xb_u8(struct xbuf *b, uint8_t v)
{
	if (xb_reserve(b, 1)) b->data[b->len++] = v;
}

static void xb_u16(struct xbuf *b, uint16_t v)
{
	if (xb_reserve(b, 2)) { wr16(b->data + b->len, v, b->order); b->len += 2; }
}

static void xb_u32(struct xbuf *b, uint32_t v)
{
	if (xb_reserve(b, 4)) { wr32(b->data + b->len, v, b->order); b->len += 4; }
}

static void xb_bytes(struct xbuf *b, const void *p, size_t n)
{
	if (xb_reserve(b, n)) { memcpy(b->data + b->len, p, n); b->len += n; }
}

static void xb_pad(struct xbuf *b)
{
	size_t p = XIM_PAD(b->len), i;
	for (i = 0; i < p; i++) xb_u8(b, 0);
}

static void
xim_send_raw(struct xim_client *c, const uint8_t *data, size_t len)
{
	xcb_client_message_event_t ev;

	xcb_change_property(c->srv->conn, XCB_PROP_MODE_REPLACE, c->client_win,
			    c->srv->a_comm, XCB_ATOM_STRING, 8,
			    (uint32_t)len, data);

	memset(&ev, 0, sizeof ev);
	ev.response_type = XCB_CLIENT_MESSAGE;
	ev.window = c->client_win;
	ev.type = c->srv->a_xim_protocol;
	ev.format = 32;
	ev.data.data32[0] = (uint32_t)len;
	ev.data.data32[1] = c->srv->a_comm;

	xcb_send_event(c->srv->conn, 0, c->client_win, XCB_EVENT_MASK_NO_EVENT,
		       (const char *)&ev);
	xcb_flush(c->srv->conn);
}

static void
xim_send(struct xim_client *c, uint8_t major, uint8_t minor,
	 const struct xbuf *body)
{
	size_t blen = body ? body->len : 0;
	size_t total = XIM_HEADER_SIZE + blen;
	uint8_t *pkt = malloc(total);

	if (!pkt)
		return;
	pkt[0] = major;
	pkt[1] = minor;
	wr16(pkt + 2, (uint16_t)blen, c->order);
	if (blen)
		memcpy(pkt + XIM_HEADER_SIZE, body->data, blen);
	xim_send_raw(c, pkt, total);
	free(pkt);
}

static void
xim_report_focus(struct xim_server *srv)
{
	struct xim_client *c;
	struct xim_ic *ic = NULL;

	wl_list_for_each(c, &srv->clients, link) {
		if (c->focus_ic) { ic = c->focus_ic; break; }
	}
	if (!ic || !ic->has_spot) {
		srv->bridge->set_xim_focus(srv->ec, ic != NULL, 0, 0);
		return;
	}

	srv->bridge->set_xim_focus(srv->ec, true, ic->spot_x, ic->spot_y);
}

static void
xim_bridge_commit(const char *text, void *user_data)
{
	struct xim_server *srv = user_data;
	struct xim_client *c;
	struct xim_ic *ic = NULL;
	struct xbuf b;

	if (!srv || !text || !text[0])
		return;

	wl_list_for_each(c, &srv->clients, link) {
		if (c->focus_ic && c->opened) { ic = c->focus_ic; break; }
	}
	if (!ic)
		return;
	c = ic->client;

	xb_init(&b, c->order);
	xb_u16(&b, ic->icid);
	xb_u16(&b, XimLookupChars);
	xb_bytes(&b, text, strlen(text));
	xb_pad(&b);
	xim_send(c, XIM_COMMIT, 0, &b);
	free(b.data);
	weston_log("xim-server: commit \"%s\" -> ic %u\n", text, ic->icid);
}

static struct xim_ic *
xim_ic_find(struct xim_client *c, uint16_t icid)
{
	struct xim_ic *ic;
	wl_list_for_each(ic, &c->ics, link)
		if (ic->icid == icid)
			return ic;
	return NULL;
}

static void
xim_parse_values(struct xim_client *c, struct xim_ic *ic,
		 const uint8_t *p, size_t len)
{
	size_t off = 0;

	while (off + 6 <= len) {
		uint16_t id = rd16(p + off, c->order);
		uint16_t type = rd16(p + off + 2, c->order);
		int16_t alen = (int16_t)rd16(p + off + 4, c->order);
		const uint8_t *val = p + off + 6;

		if (id == 0 || alen < 0 || off + 6 + (size_t)alen > len)
			break;

		if (ic) {
			if (id == c->srv->a_spot_location &&
			    type == XimType_XPoint && alen >= 8) {
				ic->spot_x = (int32_t)rd32(val, c->order);
				ic->spot_y = (int32_t)rd32(val + 4, c->order);
				ic->has_spot = true;
			} else if (id == c->srv->a_input_style &&
				   type == XimType_CARD32 && alen >= 4) {
				ic->input_style = rd32(val, c->order);
			} else if (id == c->srv->a_client_window &&
				   type == XimType_CARD32 && alen >= 4) {
				ic->client_win = (xcb_window_t)rd32(val, c->order);
			} else if (id == c->srv->a_focus_window &&
				   type == XimType_CARD32 && alen >= 4) {
				ic->focus_win = (xcb_window_t)rd32(val, c->order);
			}
		}
		off += 6 + (size_t)alen;
		off += XIM_PAD(6 + (size_t)alen);
	}

	if (ic && ic->focused)
		xim_report_focus(c->srv);
}

static void
xim_on_connect(struct xim_client *c, const uint8_t *body, size_t len)
{
	struct xbuf b;
	(void)body; (void)len;

	xb_init(&b, c->order);
	xb_u16(&b, XIM_PROTOCOL_MAJOR);
	xb_u16(&b, XIM_PROTOCOL_MINOR);
	xim_send(c, XIM_CONNECT_REPLY, 0, &b);
	free(b.data);
}

static void
xim_on_open(struct xim_client *c, const uint8_t *body, size_t len)
{
	struct xbuf b;
	char loc[64] = "-";
	if (len > 0) {
		size_t n = strnlen((const char *)body, len);
		if (n >= sizeof loc) n = sizeof loc - 1;
		memcpy(loc, body, n);
		loc[n] = '\0';
	}
	c->imid = c->srv->next_imid++;
	c->opened = true;
	weston_log("xim-server: XIM_OPEN from 0x%x locale=%s imid=%u\n",
		   c->client_win, loc, c->imid);

	xb_init(&b, c->order);
	xb_bytes(&b, c->srv->name, strlen(c->srv->name));
	xb_pad(&b);
	xim_send(c, XIM_OPEN_REPLY, 0, &b);
	free(b.data);
}

static void
xim_on_close(struct xim_client *c)
{
	struct xbuf b;
	c->opened = false;
	xb_init(&b, c->order);
	xim_send(c, XIM_CLOSE_REPLY, 0, &b);
	free(b.data);
}

static void
xim_on_encoding(struct xim_client *c)
{
	struct xbuf b;
	const char *enc = "UTF-8";
	size_t elen = strlen(enc);

	xb_init(&b, c->order);
	xb_u16(&b, 1);
	xb_u16(&b, (uint16_t)elen);
	xb_u16(&b, 0);
	xb_bytes(&b, enc, elen);
	xb_pad(&b);
	xim_send(c, XIM_ENCODING_NEGOTIATION_REPLY, 0, &b);
	free(b.data);
}

static void
xim_on_query_extension(struct xim_client *c, const uint8_t *body, size_t len)
{
	struct xbuf b;
	uint16_t idx = 0;
	if (len >= 2) idx = rd16(body, c->order);
	xb_init(&b, c->order);
	xb_u16(&b, idx);
	xim_send(c, XIM_QUERY_EXTENSION_REPLY, 0, &b);
	free(b.data);
}

static void
xim_on_get_im_values(struct xim_client *c)
{
	struct xbuf b;

	xb_init(&b, c->order);
	xb_u16(&b, c->srv->a_input_style);
	xb_u16(&b, XimType_XIMStyles);
	xb_u16(&b, 6);
	xb_u16(&b, 1);
	xb_u32(&b, XIM_STYLE_NOTHING);
	xb_u16(&b, 0);
	xim_send(c, XIM_GET_IM_VALUES_REPLY, 0, &b);
	free(b.data);
}

static void
xim_on_create_ic(struct xim_client *c, const uint8_t *body, size_t len)
{
	struct xim_ic *ic;
	struct xbuf b;

	ic = zalloc(sizeof *ic);
	if (!ic) return;
	ic->client = c;
	ic->icid = c->srv->next_icid++;
	ic->client_win = c->client_win;
	ic->input_style = XIM_STYLE_NOTHING;
	wl_list_insert(&c->ics, &ic->link);
	if (len > 0)
		xim_parse_values(c, ic, body, len);

	weston_log("xim-server: IC %u created (style=0x%x spot=%d,%d)\n",
		   ic->icid, ic->input_style, ic->spot_x, ic->spot_y);

	xb_init(&b, c->order);
	xb_u16(&b, ic->icid);
	xim_send(c, XIM_CREATE_IC_REPLY, 0, &b);
	free(b.data);
}

static void
xim_on_destroy_ic(struct xim_client *c, const uint8_t *body, size_t len)
{
	struct xbuf b;
	uint16_t icid = (len >= 2) ? rd16(body, c->order) : 0;
	struct xim_ic *ic = xim_ic_find(c, icid);

	if (ic) {
		if (c->focus_ic == ic) c->focus_ic = NULL;
		wl_list_remove(&ic->link);
		free(ic);
		xim_report_focus(c->srv);
	}
	xb_init(&b, c->order);
	xb_u16(&b, icid);
	xim_send(c, XIM_DESTROY_IC_REPLY, 0, &b);
	free(b.data);
}

static void
xim_on_set_ic_values(struct xim_client *c, const uint8_t *body, size_t len)
{
	struct xbuf b;
	uint16_t icid;
	struct xim_ic *ic;

	if (len < 2) return;
	icid = rd16(body, c->order);
	ic = xim_ic_find(c, icid);
	if (ic)
		xim_parse_values(c, ic, body + 2, len - 2);

	xb_init(&b, c->order);
	xb_u16(&b, icid);
	xim_send(c, XIM_SET_IC_VALUES_REPLY, 0, &b);
	free(b.data);
}

static void
xim_on_get_ic_values(struct xim_client *c, const uint8_t *body, size_t len)
{
	struct xbuf b;
	uint16_t icid = (len >= 2) ? rd16(body, c->order) : 0;
	struct xim_ic *ic = xim_ic_find(c, icid);

	xb_init(&b, c->order);
	xb_u16(&b, icid);
	if (ic) {
		xb_u16(&b, c->srv->a_input_style);
		xb_u16(&b, XimType_CARD32);
		xb_u16(&b, 4);
		xb_u32(&b, ic->input_style);
	}
	xb_u16(&b, 0);
	xim_send(c, XIM_GET_IC_VALUES_REPLY, 0, &b);
	free(b.data);
}

static void
xim_on_set_ic_focus(struct xim_client *c, const uint8_t *body, size_t len)
{
	struct xim_ic *ic;
	if (len < 2) return;
	ic = xim_ic_find(c, rd16(body, c->order));
	if (!ic) return;
	ic->focused = true;
	c->focus_ic = ic;
	xim_report_focus(c->srv);
}

static void
xim_on_unset_ic_focus(struct xim_client *c, const uint8_t *body, size_t len)
{
	struct xim_ic *ic;
	if (len < 2) return;
	ic = xim_ic_find(c, rd16(body, c->order));
	if (!ic) return;
	ic->focused = false;
	if (c->focus_ic == ic) c->focus_ic = NULL;
	xim_report_focus(c->srv);
}

static void
xim_on_forward_event(struct xim_client *c, const uint8_t *body, size_t len)
{
	struct xbuf b;
	bool composing = c->srv->bridge->get_remote_composing(c->srv->ec);

	xb_init(&b, c->order);
	xb_bytes(&b, body, len);
	if (composing) {

		if (b.len >= 6) {
			uint16_t fl = rd16(b.data + 4, c->order);
			fl &= (uint16_t)~0x0001;
			wr16(b.data + 4, fl, c->order);
		}
	}
	xim_send(c, XIM_FORWARD_EVENT, 0, &b);
	free(b.data);
}

static void
xim_on_sync(struct xim_client *c, const uint8_t *body, size_t len)
{
	struct xbuf b;
	xb_init(&b, c->order);
	xb_bytes(&b, body, len);
	xim_send(c, XIM_SYNC_REPLY, 0, &b);
	free(b.data);
}

static void
xim_on_reset_ic(struct xim_client *c, const uint8_t *body, size_t len)
{
	struct xbuf b;
	uint16_t icid = (len >= 2) ? rd16(body, c->order) : 0;
	xb_init(&b, c->order);
	xb_u16(&b, icid);
	xim_send(c, XIM_RESET_IC_REPLY, 0, &b);
	free(b.data);
}

static void
xim_dispatch(struct xim_client *c, const uint8_t *pkt, size_t len)
{
	uint8_t major, minor;
	uint16_t blen;
	const uint8_t *body;
	size_t body_len;

	if (len < XIM_HEADER_SIZE) return;
	major = pkt[0];
	minor = pkt[1];
	blen = rd16(pkt + 2, c->order);

	if (!c->order_known) {
		if ((size_t)blen == len - XIM_HEADER_SIZE) {
			c->order = XIM_BIGENDIAN;
		} else {
			c->order = XIM_LITTLEENDIAN;
			blen = rd16(pkt + 2, c->order);
		}
		c->order_known = true;
		weston_log("xim-server: client byte order = %s\n",
			   c->order == XIM_BIGENDIAN ? "big" : "little");
	}

	body = pkt + XIM_HEADER_SIZE;
	body_len = len - XIM_HEADER_SIZE;
	if (blen < body_len) body_len = blen;
	(void)minor;

	switch (major) {
	case XIM_CONNECT: xim_on_connect(c, body, body_len); break;
	case XIM_DISCONNECT: c->opened = false; break;
	case XIM_OPEN: xim_on_open(c, body, body_len); break;
	case XIM_CLOSE: xim_on_close(c); break;
	case XIM_ENCODING_NEGOTIATION: xim_on_encoding(c); break;
	case XIM_QUERY_EXTENSION: xim_on_query_extension(c, body, body_len); break;
	case XIM_GET_IM_VALUES: xim_on_get_im_values(c); break;
	case XIM_CREATE_IC: xim_on_create_ic(c, body, body_len); break;
	case XIM_DESTROY_IC: xim_on_destroy_ic(c, body, body_len); break;
	case XIM_SET_IC_VALUES: xim_on_set_ic_values(c, body, body_len); break;
	case XIM_GET_IC_VALUES: xim_on_get_ic_values(c, body, body_len); break;
	case XIM_SET_IC_FOCUS: xim_on_set_ic_focus(c, body, body_len); break;
	case XIM_UNSET_IC_FOCUS: xim_on_unset_ic_focus(c, body, body_len); break;
	case XIM_FORWARD_EVENT: xim_on_forward_event(c, body, body_len); break;
	case XIM_SYNC: xim_on_sync(c, body, body_len); break;
	case XIM_RESET_IC: xim_on_reset_ic(c, body, body_len); break;
	default:
		weston_log("xim-server: unhandled opcode %u\n", major);
		break;
	}
}

static void
xim_client_feed(struct xim_client *c, const uint8_t *data, size_t len)
{
	if (c->rx_len + len > XIM_MAX_PACKET) { c->rx_len = 0; return; }
	if (c->rx_len + len > c->rx_cap) {
		size_t cap = c->rx_cap ? c->rx_cap : 512;
		uint8_t *n;
		while (cap < c->rx_len + len) cap *= 2;
		n = realloc(c->rx, cap);
		if (!n) { c->rx_len = 0; return; }
		c->rx = n; c->rx_cap = cap;
	}
	memcpy(c->rx + c->rx_len, data, len);
	c->rx_len += len;

	for (;;) {
		uint16_t blen;
		size_t total;
		if (c->rx_len < XIM_HEADER_SIZE) return;
		blen = rd16(c->rx + 2, c->order_known ? c->order : XIM_BIGENDIAN);
		total = XIM_HEADER_SIZE + blen;
		if (c->rx_len < total) return;
		xim_dispatch(c, c->rx, total);
		memmove(c->rx, c->rx + total, c->rx_len - total);
		c->rx_len -= total;
	}
}

static void
xim_read_property(struct xim_client *c, xcb_window_t win, xcb_atom_t prop)
{
	xcb_get_property_cookie_t ck;
	xcb_get_property_reply_t *rep;
	uint8_t *val;
	int vlen;

	ck = xcb_get_property(c->srv->conn, 1, win, prop,
			      XCB_GET_PROPERTY_TYPE_ANY, 0, XIM_MAX_PACKET / 4);
	rep = xcb_get_property_reply(c->srv->conn, ck, NULL);
	if (!rep) return;
	vlen = xcb_get_property_value_length(rep);
	val = xcb_get_property_value(rep);
	if (vlen > 0 && val)
		xim_client_feed(c, val, (size_t)vlen);
	free(rep);
}

static struct xim_client *
xim_client_find(struct xim_server *srv, xcb_window_t comm_win)
{
	struct xim_client *c;
	wl_list_for_each(c, &srv->clients, link)
		if (c->comm_win == comm_win)
			return c;
	return NULL;
}

static void
xim_new_connection(struct xim_server *srv, xcb_client_message_event_t *ev)
{
	struct xim_client *c;
	xcb_client_message_event_t reply;

	c = zalloc(sizeof *c);
	if (!c) return;
	c->srv = srv;
	c->client_win = ev->data.data32[0];
	c->order = XIM_BIGENDIAN;
	wl_list_init(&c->ics);
	wl_list_insert(&srv->clients, &c->link);

	c->comm_win = xcb_generate_id(srv->conn);
	xcb_create_window(srv->conn, XCB_COPY_FROM_PARENT, c->comm_win,
			  srv->screen->root, 0, 0, 1, 1, 0,
			  XCB_WINDOW_CLASS_INPUT_OUTPUT,
			  srv->screen->root_visual, 0, NULL);

	memset(&reply, 0, sizeof reply);
	reply.response_type = XCB_CLIENT_MESSAGE;
	reply.window = c->client_win;
	reply.type = srv->a_xim_xconnect;
	reply.format = 32;
	reply.data.data32[0] = c->comm_win;
	reply.data.data32[1] = 0;
	reply.data.data32[2] = 2;
	reply.data.data32[3] = 4;
	xcb_send_event(srv->conn, 0, c->client_win, XCB_EVENT_MASK_NO_EVENT,
		       (const char *)&reply);
	xcb_flush(srv->conn);

	weston_log("xim-server: client 0x%x -> comm 0x%x\n",
		   c->client_win, c->comm_win);
}

static int
xim_handle_events(int fd, uint32_t mask, void *data)
{
	struct xim_server *srv = data;
	xcb_generic_event_t *ev;

	(void)fd;
	if (!(mask & WL_EVENT_READABLE)) return 0;

	while ((ev = xcb_poll_for_event(srv->conn))) {
		if ((ev->response_type & ~0x80) == XCB_CLIENT_MESSAGE) {
			xcb_client_message_event_t *cm =
				(xcb_client_message_event_t *)ev;
			if (cm->type == srv->a_xim_xconnect) {
				xim_new_connection(srv, cm);
			} else if (cm->type == srv->a_xim_protocol ||
				   cm->type == srv->a_xim_moredata) {
				struct xim_client *c =
					xim_client_find(srv, cm->window);
				if (c)
					xim_read_property(c, cm->window,
							  cm->data.data32[1]);
			}
		}
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
	struct xim_client *c, *ctmp;

	if (srv->xcb_source) wl_event_source_remove(srv->xcb_source);
	if (srv->retry_timer) wl_event_source_remove(srv->retry_timer);
	if (srv->efd_source) wl_event_source_remove(srv->efd_source);
	if (srv->efd >= 0) close(srv->efd);
	if (srv->bridge) srv->bridge->set_xim_sink(srv->ec, NULL, NULL);

	wl_list_for_each_safe(c, ctmp, &srv->clients, link) {
		struct xim_ic *ic, *ictmp;
		wl_list_for_each_safe(ic, ictmp, &c->ics, link) {
			wl_list_remove(&ic->link);
			free(ic);
		}
		wl_list_remove(&c->link);
		free(c->rx);
		free(c);
	}
	if (srv->conn) xcb_disconnect(srv->conn);
	free(srv->name);
	free(srv);
}

static void
xim_compositor_destroy(struct wl_listener *listener, void *data)
{
	struct xim_server *srv =
		container_of(listener, struct xim_server, destroy_listener);
	(void)data;
	if (g_xim == srv) g_xim = NULL;
	xim_destroy(srv);
}

static bool
xim_display_socket_ready(const char *display)
{
	const char *colon;
	char path[64];
	struct stat st;

	colon = strrchr(display, ':');
	if (!colon || !colon[1])
		return false;
	snprintf(path, sizeof path, "/tmp/.X11-unix/X%d", atoi(colon + 1));
	return stat(path, &st) == 0;
}

static xcb_connection_t *
xim_connect_display(const char *display)
{
	struct sockaddr_un addr;
	const char *colon;
	xcb_connection_t *conn;
	int fd, dnum;

	colon = strrchr(display, ':');
	if (!colon || !colon[1])
		return NULL;
	dnum = atoi(colon + 1);

	weston_log("xim-server: [dbg] socket()...\n");
	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	weston_log("xim-server: [dbg] socket fd=%d\n", fd);
	if (fd < 0)
		return NULL;

	memset(&addr, 0, sizeof addr);
	addr.sun_family = AF_UNIX;
	snprintf(addr.sun_path, sizeof addr.sun_path,
		 "/tmp/.X11-unix/X%d", dnum);

	weston_log("xim-server: [dbg] connect(%s)...\n", addr.sun_path);
	if (connect(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
		weston_log("xim-server: connect() to %s failed: %s\n",
			   addr.sun_path, strerror(errno));
		close(fd);
		return NULL;
	}

	weston_log("xim-server: [dbg] connect() ok, xcb handshake...\n");
	xcb_connection_t *wmconn = g_xim ? xim_wm_connection(g_xim->ec) : NULL;
	xim_mark("XIM-w-checked-wm-connection");
	if (wmconn) {
		xim_mark("XIM-x-reusing-wm-connection");
		close(fd);
		return wmconn;
	}
	xim_mark("XIM-a-before-xcb_connect_to_fd");
	conn = xcb_connect_to_fd(fd, NULL);
	xim_mark("XIM-b-after-xcb_connect_to_fd");
	weston_log("xim-server: [dbg] handshake returned %p\n", (void *)conn);
	if (!conn || xcb_connection_has_error(conn)) {
		weston_log("xim-server: xcb_connect_to_fd failed\n");
		if (conn)
			xcb_disconnect(conn);
		close(fd);
		return NULL;
	}
	return conn;
}

static xcb_atom_t
xim_atom(struct xim_server *srv, const char *name)
{
	xcb_intern_atom_cookie_t ck;
	xcb_intern_atom_reply_t *rep;
	xcb_atom_t atom = XCB_ATOM_NONE;

	ck = xcb_intern_atom(srv->conn, 0, (uint16_t)strlen(name), name);
	rep = xcb_intern_atom_reply(srv->conn, ck, NULL);
	if (rep) { atom = rep->atom; free(rep); }
	return atom;
}

static bool
xim_register_server(struct xim_server *srv)
{
	xcb_atom_t self = xim_atom(srv, srv->name);
	xcb_get_property_cookie_t ck;
	xcb_get_property_reply_t *rep;
	xcb_atom_t *atoms;
	int n = 0, i;

	if (self == XCB_ATOM_NONE) return false;

	ck = xcb_get_property(srv->conn, 0, srv->screen->root,
			      srv->a_xim_servers, XCB_ATOM_ATOM, 0, 64);
	rep = xcb_get_property_reply(srv->conn, ck, NULL);
	if (rep) {
		int bytes = xcb_get_property_value_length(rep);
		xcb_atom_t *v = xcb_get_property_value(rep);
		n = bytes / 4;
		atoms = calloc((size_t)n + 2, sizeof *atoms);
		if (!atoms) { free(rep); return false; }
		for (i = 0; i < n; i++) {
			if (v[i] == self) { free(atoms); free(rep); return true; }
			atoms[i] = v[i];
		}
		free(rep);
	} else {
		atoms = calloc(2, sizeof *atoms);
		if (!atoms) return false;
	}
	atoms[n++] = self;

	xcb_change_property(srv->conn, XCB_PROP_MODE_REPLACE,
			    srv->screen->root, srv->a_xim_servers,
			    XCB_ATOM_ATOM, 32, (uint32_t)n, atoms);
	free(atoms);
	xcb_flush(srv->conn);
	return true;
}

int
xim_server_init(struct weston_compositor *ec)
{
	struct xim_server *srv = g_xim;
	struct wl_event_loop *loop;
	const char *display, *name;
	int fd;

	if (srv && srv->connected)
		return 0;			

	weston_log("xim-server: [dbg] init enter (srv=%p)\n", (void *)srv);

	if (!srv) {
		srv = zalloc(sizeof *srv);
		if (!srv)
			return -1;

		srv->ec = ec;
		wl_list_init(&srv->clients);

		weston_log("xim-server: [dbg] allocating eventfd\n");
		srv->efd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
		weston_log("xim-server: [dbg] eventfd=%d, adding to loop\n", srv->efd);
		if (srv->efd >= 0) {
			loop = wl_display_get_event_loop(ec->wl_display);
			srv->efd_source = wl_event_loop_add_fd(loop, srv->efd,
					WL_EVENT_READABLE, xim_efd_cb, srv);
		}

		{
			static const struct weston_xim_server_api api = {
				.init = xim_server_init,
				.request = xim_server_request,
			};
			weston_plugin_api_register(ec, WESTON_XIM_SERVER_API_NAME,
						   &api, sizeof api);
			weston_log("xim-server: [dbg] plugin API registered\n");
		}
		srv->next_imid = 1;
		srv->next_icid = 1;
		srv->destroy_listener.notify = xim_compositor_destroy;

		srv->bridge = weston_plugin_api_get(ec,
				WESTON_TEXT_INPUT_BRIDGE_API_NAME,
				sizeof(struct weston_text_input_bridge_api));
		if (!srv->bridge) {
			weston_log("xim-server: no text-input bridge; "
				   "X11 input disabled\n");
			free(srv);
			return 0;
		}

		name = getenv("XIM_SERVER_NAME");
		if (!name || !name[0])
			name = DEFAULT_SERVER_NAME;
		srv->name = strdup(name);
		if (!srv->name) {
			free(srv);
			return -1;
		}

		g_xim = srv;
	}

	display = getenv("DISPLAY");
	if (!display || !display[0]) {
		weston_log("xim-server: DISPLAY unset, will retry\n");
		goto retry;
	}

	if (srv->conn) {
		xcb_disconnect(srv->conn);
		srv->conn = NULL;
	}
	if (!xim_display_socket_ready(display)) {
		weston_log("xim-server: X socket for %s not there yet, "
			   "will retry\n", display);
		goto retry;
	}

	xim_mark("XIM-1-enter-connect");
	if (!srv->conn) {
		weston_log("xim-server: [dbg] connecting on compositor thread\n");
	xim_mark("XIM-2-calling-helper");
		srv->conn = xim_connect_display(display);
	xim_mark("XIM-3-helper-returned");
		weston_log("xim-server: [dbg] connect done conn=%p\n",
			   (void *)srv->conn);
	} else {
		weston_log("xim-server: [dbg] reusing RDP-thread connection\n");
	}
	if (!srv->conn || xcb_connection_has_error(srv->conn)) {
		weston_log("xim-server: X server %s not up yet, will retry\n",
			   display);
		if (srv->conn) {
			xcb_disconnect(srv->conn);
			srv->conn = NULL;
		}
		goto retry;
	}

	weston_log("xim-server: [dbg] getting screen\n");
	xim_mark("XIM-c-before-get-setup");
	srv->screen = xcb_setup_roots_iterator(xcb_get_setup(srv->conn)).data;
	xim_mark("XIM-d-after-get-setup");
	weston_log("xim-server: [dbg] screen=%p\n", (void *)srv->screen);
	if (!srv->screen) {
		weston_log("xim-server: %s reports no screen, will retry\n",
			   display);
		goto retry;
	}

	xim_mark("XIM-e-before-create-window");
	srv->server_win = xcb_generate_id(srv->conn);
	xcb_create_window(srv->conn, XCB_COPY_FROM_PARENT, srv->server_win,
			  srv->screen->root, 0, 0, 1, 1, 0,
			  XCB_WINDOW_CLASS_INPUT_OUTPUT,
			  srv->screen->root_visual, 0, NULL);

	xim_mark("XIM-f-after-create-window");
	srv->a_xim_servers = xim_atom(srv, "XIM_SERVERS");
	srv->a_xim_xconnect = xim_atom(srv, "_XIM_XCONNECT");
	srv->a_xim_protocol = xim_atom(srv, "_XIM_PROTOCOL");
	srv->a_xim_moredata = xim_atom(srv, "_XIM_MOREDATA");
	srv->a_comm = xim_atom(srv, "WSLG_XIM_COMM");
	srv->a_input_style = xim_atom(srv, "inputStyle");
	srv->a_spot_location = xim_atom(srv, "spotLocation");
	srv->a_client_window = xim_atom(srv, "clientWindow");
	srv->a_focus_window = xim_atom(srv, "focusWindow");

	xim_mark("XIM-g-before-register-server");
	if (!xim_register_server(srv)) {
		weston_log("xim-server: cannot register '%s', will retry\n",
			   srv->name);
		goto retry;
	}

	xim_mark("XIM-h-after-register-server");
	fd = xcb_get_file_descriptor(srv->conn);
	loop = wl_display_get_event_loop(ec->wl_display);
	srv->xcb_source = wl_event_loop_add_fd(loop, fd, WL_EVENT_READABLE,
					       xim_handle_events, srv);
	if (!srv->xcb_source) {
		weston_log("xim-server: cannot watch the X connection, "
			   "will retry\n");
		goto retry;
	}

	wl_signal_add(&ec->destroy_signal, &srv->destroy_listener);
	srv->bridge->set_xim_sink(ec, xim_bridge_commit, srv);
	xim_mark("XIM-i-before-connected-true");
	srv->connected = true;
	xim_mark("XIM-j-CONNECTED");

	weston_log("xim-server: serving XIM on %s as '%s' "
		   "(XIMPreeditNothing|XIMStatusNothing, UTF-8); "
		   "use XMODIFIERS=@im=%s\n", display, srv->name, srv->name);
	return 0;

retry:

	return 0;
}