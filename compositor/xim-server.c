#include "config.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <xcb/xcb.h>

#include <libweston/libweston.h>
#include <libweston/plugin-registry.h>
#include <libweston/text-input-bridge.h>
#include <libweston/xwayland-api.h>

#include "shared/helpers.h"
#include "weston.h"

#define XIM_CONNECT			1
#define XIM_CONNECT_REPLY		2
#define XIM_DISCONNECT			3
#define XIM_DISCONNECT_REPLY		4
#define XIM_AUTH_REQUIRED		10
#define XIM_AUTH_REPLY			11
#define XIM_AUTH_NEXT			12
#define XIM_ERROR			20
#define XIM_OPEN			30
#define XIM_OPEN_REPLY			31
#define XIM_CLOSE			32
#define XIM_CLOSE_REPLY			33
#define XIM_SET_EVENT_MASK		37
#define XIM_ENCODING_NEGOTIATION	38
#define XIM_ENCODING_NEGOTIATION_REPLY	39
#define XIM_QUERY_EXTENSION		40
#define XIM_QUERY_EXTENSION_REPLY	41
#define XIM_SET_IM_VALUES		42
#define XIM_SET_IM_VALUES_REPLY		43
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
#define XIM_PREEDIT_START		73
#define XIM_PREEDIT_START_REPLY		74
#define XIM_PREEDIT_DRAW		75
#define XIM_PREEDIT_CARET		76
#define XIM_PREEDIT_CARET_REPLY		77
#define XIM_PREEDIT_DONE		78

#define XIM_BIGENDIAN			0x42
#define XIM_LITTLEENDIAN		0x6c

#define XimType_SeparatorOfNestedList	0
#define XimType_CARD32			3
#define XimType_Window			5
#define XimType_XIMStyles		10
#define XimType_NEST			0x7fff

#define XimLookupChars			0x0002
#define XimSYNCHRONUS			0x0001
#define XIM_IMID_VALID			0x0001
#define XIM_BadProtocol			13

#define XIM_STYLE_PREEDIT_AREA		0x0001
#define XIM_STYLE_PREEDIT_CALLBACKS	0x0002
#define XIM_STYLE_PREEDIT_POSITION	0x0004
#define XIM_STYLE_PREEDIT_NOTHING	0x0008
#define XIM_STYLE_PREEDIT_NONE		0x0010
#define XIM_STYLE_STATUS_AREA		0x0100
#define XIM_STYLE_STATUS_CALLBACKS	0x0200
#define XIM_STYLE_STATUS_NOTHING	0x0400
#define XIM_STYLE_STATUS_NONE		0x0800
#define XIM_STYLE_NOTHING \
	(XIM_STYLE_PREEDIT_NOTHING | XIM_STYLE_STATUS_NOTHING)

static const uint32_t xim_advertised_styles[] = {
	XIM_STYLE_PREEDIT_CALLBACKS | XIM_STYLE_STATUS_CALLBACKS,
	XIM_STYLE_PREEDIT_CALLBACKS | XIM_STYLE_STATUS_NOTHING,
	XIM_STYLE_PREEDIT_CALLBACKS | XIM_STYLE_STATUS_NONE,
	XIM_STYLE_PREEDIT_CALLBACKS | XIM_STYLE_STATUS_AREA,
	XIM_STYLE_NOTHING,
};

#define XIM_FEEDBACK_UNDERLINE		0x00000002

#define XIM_IM_ATTR_QUERY_INPUT_STYLE	0
#define XIM_IM_ATTR_SEPARATOR		1

#define XIM_IC_ATTR_INPUT_STYLE		0
#define XIM_IC_ATTR_CLIENT_WINDOW	1
#define XIM_IC_ATTR_FOCUS_WINDOW	2
#define XIM_IC_ATTR_PREEDIT_ATTRS	3
#define XIM_IC_ATTR_STATUS_ATTRS	4
#define XIM_IC_ATTR_SEPARATOR		5
#define XIM_IC_ATTR_FILTER_EVENTS	6

#define DEFAULT_SERVER_NAME		"wslgxim"
#define XIM_PROTOCOL_MAJOR		1
#define XIM_PROTOCOL_MINOR		0
#define XIM_TRANSPORT_MAJOR		0
#define XIM_TRANSPORT_MINOR		0
#define XIM_HEADER_SIZE			4
#define XIM_CM_DATA_SIZE		20
#define XIM_MAX_PACKET			(0xffff * 4)
#define XIM_PAD(len)			((4 - ((len) % 4)) % 4)

#define XIM_LOCALES_VALUE \
	"@locale=C,POSIX," \
	"en,en_US,en_US.UTF-8,en.UTF-8,en_GB,en_GB.UTF-8," \
	"zh,zh_CN,zh_CN.UTF-8,zh.UTF-8,zh_TW,zh_TW.UTF-8,zh_HK.UTF-8," \
	"ja,ja_JP,ja_JP.UTF-8,ja.UTF-8,ko,ko_KR,ko_KR.UTF-8,ko.UTF-8," \
	"de,de_DE.UTF-8,fr,fr_FR.UTF-8,es,es_ES.UTF-8,it,it_IT.UTF-8," \
	"pt,pt_BR.UTF-8,pt_PT.UTF-8,ru,ru_RU.UTF-8,pl,pl_PL.UTF-8," \
	"nl,nl_NL.UTF-8,sv,sv_SE.UTF-8,da,da_DK.UTF-8,nb,no,fi," \
	"cs,cs_CZ.UTF-8,sk,hu,hu_HU.UTF-8,ro,ro_RO.UTF-8," \
	"tr,tr_TR.UTF-8,el,el_GR.UTF-8,he,iw_IL.UTF-8,ar,ar_SA.UTF-8," \
	"fa,fa_IR.UTF-8,hi,hi_IN.UTF-8,th,th_TH.UTF-8,vi,vi_VN.UTF-8," \
	"id,id_ID.UTF-8,ms,ms_MY.UTF-8,uk,uk_UA.UTF-8"
#define XIM_TRANSPORT_VALUE		"@transport=X/"

struct xim_server;
struct xim_client;

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
	bool preedit_active;
	bool has_preedit_attrs;
	uint32_t preedit_chars;
	uint8_t *last_key;
	size_t last_key_len;
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
	struct wl_event_source *efd_source;
	int efd;
	struct wl_listener destroy_listener;
	xcb_screen_t *screen;
	xcb_window_t server_win;
	xcb_atom_t a_xim_servers, a_xim_xconnect, a_xim_protocol, a_xim_moredata;
	xcb_atom_t a_locales, a_transport, a_server;
	char *name;
	struct wl_list clients;
	uint16_t next_imid, next_icid;
	bool connected;
	bool listener_attached;
};

struct xim_xwm_proxy {
	xcb_connection_t *conn;
};

struct xim_xserver_proxy {
	struct wl_display *wl_display;
	struct wl_event_loop *loop;
	int abstract_fd;
	void *abstract_source;
	int unix_fd;
	void *unix_source;
	int display;
	pid_t pid;
	void *client;
	struct weston_compositor *compositor;
	struct xim_xwm_proxy *wm;
};

struct weston_xim_server_api {
	int (*init)(struct weston_compositor *ec);
	void (*request)(struct weston_compositor *ec);
};

#define WESTON_XIM_SERVER_API_NAME "weston_xim_server_v1"

static struct xim_server *g_xim;

static bool xim_display_socket_ready(const char *display);
static xcb_connection_t *xim_connect_display(const char *display);
static void xim_on_unhandled(struct xim_client *c, uint8_t major,
			     const uint8_t *body, size_t len);

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
	bool overflow;
};

static bool
xb_reserve(struct xbuf *b, size_t extra)
{
	size_t need = b->len + extra, cap;
	uint8_t *n;

	if (need <= b->cap)
		return true;
	if (b->overflow)
		return false;
	cap = b->cap ? b->cap : 256;
	while (cap < need)
		cap *= 2;
	n = realloc(b->data, cap);
	if (!n) {
		b->overflow = true;
		return false;
	}
	b->data = n;
	b->cap = cap;
	return true;
}

static void
xb_init(struct xbuf *b, uint8_t order)
{
	b->data = NULL;
	b->len = 0;
	b->cap = 0;
	b->order = order;
	b->overflow = false;
}

static void
xb_u16(struct xbuf *b, uint16_t v)
{
	if (xb_reserve(b, 2)) {
		wr16(b->data + b->len, v, b->order);
		b->len += 2;
	}
}

static void
xb_u32(struct xbuf *b, uint32_t v)
{
	if (xb_reserve(b, 4)) {
		wr32(b->data + b->len, v, b->order);
		b->len += 4;
	}
}

static void
xb_bytes(struct xbuf *b, const void *p, size_t n)
{
	if (xb_reserve(b, n)) {
		memcpy(b->data + b->len, p, n);
		b->len += n;
	}
}

static void
xb_zero(struct xbuf *b, size_t n)
{
	if (xb_reserve(b, n)) {
		memset(b->data + b->len, 0, n);
		b->len += n;
	}
}

static void
xb_pad(struct xbuf *b)
{
	xb_zero(b, XIM_PAD(b->len));
}

static void
xb_imattr(struct xbuf *b, uint16_t id, uint16_t type, const char *name)
{
	size_t n = strlen(name);

	xb_u16(b, id);
	xb_u16(b, type);
	xb_u16(b, (uint16_t)n);
	xb_bytes(b, name, n);
	xb_zero(b, XIM_PAD(n + 2));
}

static void
xb_attr_value(struct xbuf *b, uint16_t id, const void *value, size_t len)
{
	xb_u16(b, id);
	xb_u16(b, (uint16_t)len);
	xb_bytes(b, value, len);
	xb_zero(b, XIM_PAD(len));
}

static void
xim_send_packet(struct xim_client *c, const uint8_t *pkt, size_t len)
{
	size_t off;

	for (off = 0; off < len; off += XIM_CM_DATA_SIZE) {
		xcb_client_message_event_t ev;
		size_t left = len - off;
		bool last = left <= XIM_CM_DATA_SIZE;

		memset(&ev, 0, sizeof ev);
		ev.response_type = XCB_CLIENT_MESSAGE;
		ev.window = c->client_win;
		ev.type = last ? c->srv->a_xim_protocol : c->srv->a_xim_moredata;
		ev.format = 8;
		memcpy(ev.data.data8, pkt + off,
		       last ? left : XIM_CM_DATA_SIZE);

		xcb_send_event(c->srv->conn, 0, c->client_win,
			       XCB_EVENT_MASK_NO_EVENT, (const char *)&ev);
	}

	xcb_flush(c->srv->conn);
}

static void
xim_send(struct xim_client *c, uint8_t major, uint8_t minor,
	 const struct xbuf *body)
{
	size_t blen = body ? body->len : 0;
	size_t total = XIM_HEADER_SIZE + blen;
	uint8_t *pkt;

	if (body && body->overflow)
		return;
	if (blen % 4 || blen > XIM_MAX_PACKET) {
		weston_log("xim-server: refusing malformed packet %u/%zu\n",
			   major, blen);
		return;
	}

	pkt = malloc(total ? total : 1);
	if (!pkt)
		return;

	pkt[0] = major;
	pkt[1] = minor;
	wr16(pkt + 2, (uint16_t)(blen / 4), c->order);
	if (blen)
		memcpy(pkt + XIM_HEADER_SIZE, body->data, blen);

	xim_send_packet(c, pkt, total);
	free(pkt);
}

static xcb_atom_t
xim_atom(struct xim_server *srv, const char *name)
{
	xcb_intern_atom_cookie_t ck;
	xcb_intern_atom_reply_t *rep;
	xcb_atom_t atom = XCB_ATOM_NONE;

	ck = xcb_intern_atom(srv->conn, 0, (uint16_t)strlen(name), name);
	rep = xcb_intern_atom_reply(srv->conn, ck, NULL);
	if (rep) {
		atom = rep->atom;
		free(rep);
	}
	return atom;
}

static bool
xim_register_server(struct xim_server *srv)
{
	char sel_name[128];
	xcb_get_property_cookie_t ck;
	xcb_get_property_reply_t *rep = NULL;
	xcb_atom_t *atoms = NULL;
	xcb_atom_t *list;
	uint32_t n = 0, i;
	bool have = false;

	snprintf(sel_name, sizeof sel_name, "@server=%s", srv->name);
	srv->a_server = xim_atom(srv, sel_name);
	if (srv->a_server == XCB_ATOM_NONE)
		return false;

	srv->a_xim_servers = xim_atom(srv, "XIM_SERVERS");
	srv->a_xim_xconnect = xim_atom(srv, "_XIM_XCONNECT");
	srv->a_xim_protocol = xim_atom(srv, "_XIM_PROTOCOL");
	srv->a_xim_moredata = xim_atom(srv, "_XIM_MOREDATA");
	srv->a_locales = xim_atom(srv, "LOCALES");
	srv->a_transport = xim_atom(srv, "TRANSPORT");

	if (srv->a_xim_servers == XCB_ATOM_NONE ||
	    srv->a_xim_xconnect == XCB_ATOM_NONE ||
	    srv->a_xim_protocol == XCB_ATOM_NONE ||
	    srv->a_xim_moredata == XCB_ATOM_NONE ||
	    srv->a_locales == XCB_ATOM_NONE ||
	    srv->a_transport == XCB_ATOM_NONE)
		return false;

	ck = xcb_get_property(srv->conn, 0, srv->screen->root,
			      srv->a_xim_servers, XCB_ATOM_ATOM, 0, 64);
	rep = xcb_get_property_reply(srv->conn, ck, NULL);
	if (rep) {
		int bytes = xcb_get_property_value_length(rep);
		xcb_atom_t *v = xcb_get_property_value(rep);

		n = (uint32_t)(bytes / 4);
		for (i = 0; i < n; i++) {
			if (v[i] == srv->a_server)
				have = true;
		}
		atoms = calloc(n + 1, sizeof *atoms);
		if (!atoms) {
			free(rep);
			return false;
		}
		for (i = 0; i < n; i++)
			atoms[i] = v[i];
		free(rep);
	} else {
		atoms = calloc(1, sizeof *atoms);
		if (!atoms)
			return false;
	}

	xcb_set_selection_owner(srv->conn, srv->server_win, srv->a_server,
				XCB_CURRENT_TIME);

	if (!have) {
		list = calloc(n + 1, sizeof *list);
		if (!list) {
			free(atoms);
			return false;
		}
		for (i = 0; i < n; i++)
			list[i] = atoms[i];
		list[n] = srv->a_server;

		xcb_change_property(srv->conn, XCB_PROP_MODE_REPLACE,
				    srv->screen->root, srv->a_xim_servers,
				    XCB_ATOM_ATOM, 32, n + 1, list);
		free(list);
	}
	free(atoms);

	xcb_flush(srv->conn);

	{
		xcb_get_selection_owner_cookie_t ock;
		xcb_get_selection_owner_reply_t *orep;
		bool ok;

		ock = xcb_get_selection_owner(srv->conn, srv->a_server);
		orep = xcb_get_selection_owner_reply(srv->conn, ock, NULL);
		ok = orep && orep->owner == srv->server_win;
		free(orep);
		if (!ok)
			return false;
	}

	return true;
}

static void
xim_answer_selection(struct xim_server *srv,
		     xcb_selection_request_event_t *sr)
{
	const char *value = NULL;
	xcb_atom_t prop = sr->property ? sr->property : sr->target;
	xcb_selection_notify_event_t ev;

	if (sr->target == srv->a_locales)
		value = XIM_LOCALES_VALUE;
	else if (sr->target == srv->a_transport)
		value = XIM_TRANSPORT_VALUE;

	if (value) {
		xcb_change_property(srv->conn, XCB_PROP_MODE_REPLACE,
				    sr->requestor, prop, sr->target, 8,
				    (uint32_t)strlen(value), value);
	}

	memset(&ev, 0, sizeof ev);
	ev.response_type = XCB_SELECTION_NOTIFY;
	ev.time = sr->time;
	ev.requestor = sr->requestor;
	ev.selection = sr->selection;
	ev.target = sr->target;
	ev.property = value ? prop : XCB_ATOM_NONE;

	xcb_send_event(srv->conn, 0, sr->requestor, XCB_EVENT_MASK_NO_EVENT,
		       (const char *)&ev);
	xcb_flush(srv->conn);
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

static struct xim_ic *
xim_ic_focused(struct xim_server *srv)
{
	struct xim_client *c;

	wl_list_for_each(c, &srv->clients, link)
		if (c->focus_ic && c->opened)
			return c->focus_ic;

	return NULL;
}

static void
xim_report_focus(struct xim_server *srv)
{
	struct xim_ic *ic = xim_ic_focused(srv);

	if (!srv->bridge)
		return;

	if (!ic) {
		srv->bridge->set_xim_focus(srv->ec, false, 0, 0);
		return;
	}

	srv->bridge->set_xim_focus(srv->ec, true,
				   ic->has_spot ? ic->spot_x : 0,
				   ic->has_spot ? ic->spot_y : 0);
}

static bool
xim_needs_ct_utf8(const char *text)
{
	for (; *text; text++) {
		if ((unsigned char)*text >= 0x80)
			return true;
	}

	return false;
}

static void
xim_preedit_done(struct xim_ic *ic)
{
	struct xim_client *c = ic->client;
	struct xbuf b;

	if (!ic->preedit_active)
		return;

	xb_init(&b, c->order);
	xb_u16(&b, c->imid);
	xb_u16(&b, ic->icid);
	xim_send(c, XIM_PREEDIT_DONE, 0, &b);
	free(b.data);

	ic->preedit_active = false;
	ic->preedit_chars = 0;
}

static void
xim_bridge_preedit(const char *text, int32_t cursor_begin, int32_t cursor_end,
		   void *user_data)
{
	struct xim_server *srv = user_data;
	struct xim_ic *ic;
	struct xim_client *c;
	struct xbuf b;
	size_t len, i;
	uint32_t chars = 0;
	uint16_t str_len;
	bool utf8;

	(void)cursor_begin;
	(void)cursor_end;

	if (!srv)
		return;

	ic = xim_ic_focused(srv);
	if (!ic)
		return;

	if (!(ic->input_style & XIM_STYLE_PREEDIT_CALLBACKS) &&
	    !ic->has_preedit_attrs) {
		xim_preedit_done(ic);
		return;
	}

	if (!text || !text[0]) {
		xim_preedit_done(ic);
		return;
	}

	c = ic->client;

	if (!ic->preedit_active) {
		xb_init(&b, c->order);
		xb_u16(&b, c->imid);
		xb_u16(&b, ic->icid);
		xim_send(c, XIM_PREEDIT_START, 0, &b);
		free(b.data);
		ic->preedit_active = true;
		ic->preedit_chars = 0;
	}

	utf8 = xim_needs_ct_utf8(text);
	len = strlen(text);
	str_len = (uint16_t)(len + (utf8 ? 3 : 0));

	for (i = 0; i < len; i++) {
		if (((unsigned char)text[i] & 0xc0) != 0x80)
			chars++;
	}

	xb_init(&b, c->order);
	xb_u16(&b, c->imid);
	xb_u16(&b, ic->icid);
	xb_u32(&b, chars);
	xb_u32(&b, 0);
	xb_u32(&b, ic->preedit_chars);
	xb_u32(&b, 0);
	xb_u16(&b, str_len);
	if (utf8)
		xb_bytes(&b, "\033%G", 3);
	xb_bytes(&b, text, len);
	xb_pad(&b);
	xb_u16(&b, (uint16_t)(4 * chars));
	xb_u16(&b, 0);
	for (i = 0; i < chars; i++)
		xb_u32(&b, XIM_FEEDBACK_UNDERLINE);
	xim_send(c, XIM_PREEDIT_DRAW, 0, &b);
	free(b.data);

	ic->preedit_chars = chars;
	weston_log("xim-server: preedit \"%s\" (%u chars) to ic %u\n",
		   text, chars, ic->icid);
}

static void
xim_bridge_commit(const char *text, void *user_data)
{
	struct xim_server *srv = user_data;
	struct xim_ic *ic;
	struct xim_client *c;
	struct xbuf b;
	size_t len;
	bool utf8;

	if (!srv || !text || !text[0])
		return;

	ic = xim_ic_focused(srv);
	if (!ic)
		return;
	c = ic->client;

	xim_preedit_done(ic);

	utf8 = xim_needs_ct_utf8(text);

	len = strlen(text);
	xb_init(&b, c->order);
	xb_u16(&b, c->imid);
	xb_u16(&b, ic->icid);
	xb_u16(&b, XimLookupChars);
	xb_u16(&b, (uint16_t)(len + (utf8 ? 3 : 0)));
	if (utf8)
		xb_bytes(&b, "\033%G", 3);
	xb_bytes(&b, text, len);
	xb_pad(&b);
	xim_send(c, XIM_COMMIT, 0, &b);
	free(b.data);

	if (ic->last_key) {
		xb_init(&b, c->order);
		xb_bytes(&b, ic->last_key, ic->last_key_len);
		xb_pad(&b);
		xim_send(c, XIM_FORWARD_EVENT, 0, &b);
		free(b.data);

		free(ic->last_key);
		ic->last_key = NULL;
		ic->last_key_len = 0;
	}

	weston_log("xim-server: committed %zu bytes to ic %u\n", len, ic->icid);
}

static void
xim_parse_ic_values(struct xim_client *c, struct xim_ic *ic,
		    const uint8_t *p, size_t len)
{
	size_t off = 0;

	while (off + 4 <= len) {
		uint16_t id = rd16(p + off, c->order);
		uint16_t alen = rd16(p + off + 2, c->order);
		const uint8_t *val = p + off + 4;

		if (off + 4 + alen > len)
			break;

		if (id == XIM_IC_ATTR_INPUT_STYLE && alen >= 4 &&
		    ic->input_style == XIM_STYLE_NOTHING)
			ic->input_style = rd32(val, c->order);
		else if (id == XIM_IC_ATTR_CLIENT_WINDOW && alen >= 4)
			ic->client_win = (xcb_window_t)rd32(val, c->order);
		else if (id == XIM_IC_ATTR_FOCUS_WINDOW && alen >= 4)
			ic->focus_win = (xcb_window_t)rd32(val, c->order);
		else if (id == XIM_IC_ATTR_PREEDIT_ATTRS) {
			ic->has_preedit_attrs = true;
			weston_log("xim-server: IC preeditAttributes, %u bytes\n",
				   alen);
		}

		off += 4 + alen + XIM_PAD(alen);
	}
}

static void
xim_on_connect(struct xim_client *c, const uint8_t *body, size_t len)
{
	struct xbuf b;

	if (len >= 1) {
		c->order = (body[0] == XIM_BIGENDIAN) ? XIM_BIGENDIAN
						      : XIM_LITTLEENDIAN;
		c->order_known = true;
	}

	xb_init(&b, c->order);
	xb_u16(&b, XIM_PROTOCOL_MAJOR);
	xb_u16(&b, XIM_PROTOCOL_MINOR);
	xim_send(c, XIM_CONNECT_REPLY, 0, &b);
	free(b.data);

	weston_log("xim-server: client 0x%x connected (protocol %u.%u)\n",
		   c->client_win, XIM_PROTOCOL_MAJOR, XIM_PROTOCOL_MINOR);
}

static void
xim_on_open(struct xim_client *c, const uint8_t *body, size_t len)
{
	struct xbuf b;
	char loc[128] = "";

	if (len > 1) {
		size_t n = body[0];

		if (n > len - 1)
			n = len - 1;
		if (n >= sizeof loc)
			n = sizeof loc - 1;
		memcpy(loc, body + 1, n);
		loc[n] = '\0';
	}

	c->imid = c->srv->next_imid++;
	c->opened = true;

	xb_init(&b, c->order);
	xb_u16(&b, c->imid);
	{
		struct xbuf im;

		xb_init(&im, c->order);
		xb_imattr(&im, XIM_IM_ATTR_QUERY_INPUT_STYLE,
			  XimType_XIMStyles, "queryInputStyle");
		xb_imattr(&im, XIM_IM_ATTR_SEPARATOR,
			  XimType_SeparatorOfNestedList,
			  "separatorofNestedList");
		xb_u16(&b, (uint16_t)im.len);
		xb_bytes(&b, im.data, im.len);
		free(im.data);
	}

	{
		struct xbuf ic;

		xb_init(&ic, c->order);
		xb_imattr(&ic, XIM_IC_ATTR_INPUT_STYLE, XimType_CARD32,
			  "inputStyle");
		xb_imattr(&ic, XIM_IC_ATTR_CLIENT_WINDOW, XimType_Window,
			  "clientWindow");
		xb_imattr(&ic, XIM_IC_ATTR_FOCUS_WINDOW, XimType_Window,
			  "focusWindow");
		xb_imattr(&ic, XIM_IC_ATTR_PREEDIT_ATTRS, XimType_NEST,
			  "preeditAttributes");
		xb_imattr(&ic, XIM_IC_ATTR_STATUS_ATTRS, XimType_NEST,
			  "statusAttributes");
		xb_imattr(&ic, XIM_IC_ATTR_SEPARATOR,
			  XimType_SeparatorOfNestedList,
			  "separatorofNestedList");
		xb_imattr(&ic, XIM_IC_ATTR_FILTER_EVENTS, XimType_CARD32,
			  "filterEvents");
		xb_u16(&b, (uint16_t)ic.len);
		xb_u16(&b, 0);
		xb_bytes(&b, ic.data, ic.len);
		free(ic.data);
	}
	xim_send(c, XIM_OPEN_REPLY, 0, &b);
	free(b.data);

	weston_log("xim-server: XIM_OPEN from 0x%x (locale \"%s\"), imid %u\n",
		   c->client_win, loc, c->imid);

	xb_init(&b, c->order);
	xb_u16(&b, c->imid);
	xb_u16(&b, 0);
	xb_u32(&b, XCB_EVENT_MASK_KEY_PRESS);
	xb_u32(&b, 0);
	xim_send(c, XIM_SET_EVENT_MASK, 0, &b);
	free(b.data);
}

static void
xim_on_close(struct xim_client *c, const uint8_t *body, size_t len)
{
	struct xbuf b;
	uint16_t imid = (len >= 2) ? rd16(body, c->order) : c->imid;

	c->opened = false;
	c->focus_ic = NULL;
	xim_report_focus(c->srv);

	xb_init(&b, c->order);
	xb_u16(&b, imid);
	xb_u16(&b, 0);
	xim_send(c, XIM_CLOSE_REPLY, 0, &b);
	free(b.data);
}

static void
xim_on_encoding(struct xim_client *c, const uint8_t *body, size_t len)
{
	struct xbuf b;
	int index = 0;
	uint16_t name_len;

	if (len >= 4) {
		const uint8_t *names = body + 4;
		size_t total, off = 0;
		bool found = false;

		name_len = rd16(body + 2, c->order);
		total = (name_len <= len - 4) ? name_len : len - 4;

		while (off + 1 <= total) {
			size_t n = names[off];

			if (off + 1 + n > total)
				break;
			if ((n == 5 && !strncasecmp((const char *)names + off + 1,
						    "UTF-8", 5)) ||
			    (n == 4 && !strncasecmp((const char *)names + off + 1,
						    "utf8", 4))) {
				found = true;
				break;
			}
			off += 1 + n;
			index++;
		}
		if (!found)
			index = 0;
	}

	xb_init(&b, c->order);
	xb_u16(&b, c->imid);
	xb_u16(&b, 0);
	xb_u16(&b, (uint16_t)index);
	xb_u16(&b, 0);
	xim_send(c, XIM_ENCODING_NEGOTIATION_REPLY, 0, &b);
	free(b.data);

	weston_log("xim-server: encoding negotiation -> index %d%s\n", index,
		   index ? "" : " (client did not offer UTF-8)");
}

static void
xim_on_query_extension(struct xim_client *c, const uint8_t *body, size_t len)
{
	struct xbuf b;
	uint16_t imid = (len >= 2) ? rd16(body, c->order) : c->imid;

	xb_init(&b, c->order);
	xb_u16(&b, imid);
	xb_u16(&b, 0);
	xim_send(c, XIM_QUERY_EXTENSION_REPLY, 0, &b);
	free(b.data);
}

static void
xim_on_get_im_values(struct xim_client *c, const uint8_t *body, size_t len)
{
	struct xbuf b;
	uint8_t value[4 + sizeof xim_advertised_styles];
	size_t i, n = sizeof xim_advertised_styles /
		      sizeof xim_advertised_styles[0];
	uint16_t imid = (len >= 2) ? rd16(body, c->order) : c->imid;

	if (len >= 4) {
		char ids[96];
		size_t o = 0, cnt = rd16(body + 2, c->order) / 2, k;

		ids[0] = '\0';
		for (k = 0; k < cnt && 4 + 2 * k + 2 <= len && o + 8 < sizeof ids;
		     k++)
			o += (size_t)snprintf(ids + o, sizeof ids - o, "%s%u",
					      k ? "," : "",
					      rd16(body + 4 + 2 * k, c->order));
		weston_log("xim-server: GET_IM_VALUES ids [%s]\n", ids);
	}

	wr16(value, (uint16_t)n, c->order);
	wr16(value + 2, 0, c->order);
	for (i = 0; i < n; i++)
		wr32(value + 4 + 4 * i, xim_advertised_styles[i], c->order);

	xb_init(&b, c->order);
	xb_u16(&b, imid);
	xb_u16(&b, (uint16_t)(sizeof value + 4));
	xb_attr_value(&b, XIM_IM_ATTR_QUERY_INPUT_STYLE, value, sizeof value);
	xim_send(c, XIM_GET_IM_VALUES_REPLY, 0, &b);
	free(b.data);
}

static void
xim_on_set_im_values(struct xim_client *c, const uint8_t *body, size_t len)
{
	struct xbuf b;
	uint16_t imid = (len >= 2) ? rd16(body, c->order) : c->imid;

	xb_init(&b, c->order);
	xb_u16(&b, imid);
	xb_u16(&b, 0);
	xim_send(c, XIM_SET_IM_VALUES_REPLY, 0, &b);
	free(b.data);
}

static void
xim_on_create_ic(struct xim_client *c, const uint8_t *body, size_t len)
{
	struct xim_ic *ic;
	struct xbuf b;
	uint16_t imid = (len >= 2) ? rd16(body, c->order) : c->imid;
	uint16_t bytes = (len >= 4) ? rd16(body + 2, c->order) : 0;
	size_t avail = (len > 4) ? len - 4 : 0;

	ic = zalloc(sizeof *ic);
	if (!ic) {
		xb_init(&b, c->order);
		xb_u16(&b, imid);
		xb_u16(&b, 0);
		xim_send(c, XIM_ERROR, 0, &b);
		free(b.data);
		return;
	}

	ic->client = c;
	ic->icid = c->srv->next_icid++;
	ic->client_win = c->client_win;
	ic->input_style = XIM_STYLE_NOTHING;
	wl_list_insert(&c->ics, &ic->link);

	if (bytes && bytes <= avail)
		xim_parse_ic_values(c, ic, body + 4, bytes);

	xb_init(&b, c->order);
	xb_u16(&b, imid);
	xb_u16(&b, ic->icid);
	xim_send(c, XIM_CREATE_IC_REPLY, 0, &b);
	free(b.data);

	weston_log("xim-server: IC %u created (style 0x%x, preeditAttrs %d, "
		   "window 0x%x)\n", ic->icid, ic->input_style,
		   ic->has_preedit_attrs, ic->client_win);

	if (bytes && bytes <= avail) {
		char ids[96];
		size_t o = 0, off = 0, n = 0;

		ids[0] = '\0';
		while (off + 4 <= bytes && n < 10) {
			uint16_t id = rd16(body + 4 + off, c->order);
			uint16_t alen = rd16(body + 4 + off + 2, c->order);

			if (off + 4 + alen > bytes)
				break;
			o += (size_t)snprintf(ids + o, sizeof ids - o, "%s%u/%u",
					      n ? "," : "", id, alen);
			off += 4 + alen + XIM_PAD(alen);
			n++;
		}
		weston_log("xim-server: CREATE_IC attrs [%s]\n", ids);
	}
}

static void
xim_ic_free(struct xim_ic *ic)
{
	if (!ic)
		return;

	free(ic->last_key);
	free(ic);
}

static void
xim_on_destroy_ic(struct xim_client *c, const uint8_t *body, size_t len)
{
	struct xbuf b;
	uint16_t imid = (len >= 2) ? rd16(body, c->order) : c->imid;
	uint16_t icid = (len >= 4) ? rd16(body + 2, c->order) : 0;
	struct xim_ic *ic = xim_ic_find(c, icid);

	if (ic) {
		if (c->focus_ic == ic)
			c->focus_ic = NULL;
		wl_list_remove(&ic->link);
		xim_ic_free(ic);
		xim_report_focus(c->srv);
	}

	xb_init(&b, c->order);
	xb_u16(&b, imid);
	xb_u16(&b, icid);
	xim_send(c, XIM_DESTROY_IC_REPLY, 0, &b);
	free(b.data);
}

static void
xim_on_set_ic_values(struct xim_client *c, const uint8_t *body, size_t len)
{
	struct xbuf b;
	uint16_t imid = (len >= 2) ? rd16(body, c->order) : c->imid;
	uint16_t icid = (len >= 4) ? rd16(body + 2, c->order) : 0;
	uint16_t bytes = (len >= 6) ? rd16(body + 4, c->order) : 0;
	struct xim_ic *ic = xim_ic_find(c, icid);

	if (ic && bytes) {
		size_t off = 8;

		if (off + bytes > len)
			off = 6;
		if (off + bytes <= len)
			xim_parse_ic_values(c, ic, body + off, bytes);
		if (ic->focused)
			xim_report_focus(c->srv);
	}

	xb_init(&b, c->order);
	xb_u16(&b, imid);
	xb_u16(&b, icid);
	xim_send(c, XIM_SET_IC_VALUES_REPLY, 0, &b);
	free(b.data);
}

static void
xim_on_get_ic_values(struct xim_client *c, const uint8_t *body, size_t len)
{
	struct xbuf b;
	uint8_t value[8];
	uint16_t imid = (len >= 2) ? rd16(body, c->order) : c->imid;
	uint16_t icid = (len >= 4) ? rd16(body + 2, c->order) : 0;
	struct xim_ic *ic = xim_ic_find(c, icid);

	wr32(value, ic ? ic->input_style : XIM_STYLE_NOTHING, c->order);
	wr32(value + 4, XCB_EVENT_MASK_KEY_PRESS, c->order);

	xb_init(&b, c->order);
	xb_u16(&b, imid);
	xb_u16(&b, icid);
	xb_u16(&b, 16);
	xb_u16(&b, 0);
	xb_attr_value(&b, XIM_IC_ATTR_INPUT_STYLE, value, 4);
	xb_attr_value(&b, XIM_IC_ATTR_FILTER_EVENTS, value + 4, 4);
	xim_send(c, XIM_GET_IC_VALUES_REPLY, 0, &b);
	free(b.data);
}

static void
xim_on_set_ic_focus(struct xim_client *c, const uint8_t *body, size_t len,
		    bool focused)
{
	struct xim_ic *ic;
	uint16_t icid = (len >= 4) ? rd16(body + 2, c->order) :
			((len >= 2) ? rd16(body, c->order) : 0);

	if (len < 4)
		return;

	ic = xim_ic_find(c, icid);
	if (!ic)
		return;

	ic->focused = focused;
	if (focused)
		c->focus_ic = ic;
	else if (c->focus_ic == ic)
		c->focus_ic = NULL;
	if (!focused)
		xim_preedit_done(ic);

	xim_report_focus(c->srv);

	weston_log("xim-server: IC %u %s (host IME %s)\n", ic->icid,
		   focused ? "focused" : "unfocused",
		   focused ? "engaged" : "released");
}

static void
xim_on_forward_event(struct xim_client *c, const uint8_t *body, size_t len)
{
	struct xbuf b;
	struct xim_ic *ic;

	if (len < 40) {
		xim_on_unhandled(c, XIM_FORWARD_EVENT, body, len);
		return;
	}

	if (len >= 4) {
		ic = xim_ic_find(c, rd16(body + 2, c->order));
		if (ic) {
			free(ic->last_key);
			ic->last_key = malloc(len);
			if (ic->last_key) {
				memcpy(ic->last_key, body, len);
				ic->last_key_len = len;
			} else {
				ic->last_key_len = 0;
			}

			if (ic->preedit_active)
				return;
		}
	}

	xb_init(&b, c->order);
	xb_bytes(&b, body, len);
	xb_pad(&b);
	xim_send(c, XIM_FORWARD_EVENT, 0, &b);
	free(b.data);
}

static void
xim_on_sync(struct xim_client *c, const uint8_t *body, size_t len)
{
	struct xbuf b;

	xb_init(&b, c->order);
	xb_bytes(&b, body, len);
	xb_pad(&b);
	xim_send(c, XIM_SYNC_REPLY, 0, &b);
	free(b.data);
}

static void
xim_on_reset_ic(struct xim_client *c, const uint8_t *body, size_t len)
{
	struct xbuf b;
	uint16_t imid = (len >= 2) ? rd16(body, c->order) : c->imid;
	uint16_t icid = (len >= 4) ? rd16(body + 2, c->order) : 0;
	struct xim_ic *ic = xim_ic_find(c, icid);

	if (ic)
		xim_preedit_done(ic);

	xb_init(&b, c->order);
	xb_u16(&b, imid);
	xb_u16(&b, icid);
	xb_u16(&b, 0);
	xb_u16(&b, 0);
	xim_send(c, XIM_RESET_IC_REPLY, 0, &b);
	free(b.data);
}

static void
xim_on_unhandled(struct xim_client *c, uint8_t major, const uint8_t *body,
		 size_t len)
{
	struct xbuf b;
	uint16_t imid = (len >= 2) ? rd16(body, c->order) : 0;

	if (major == 0 || major == XIM_ERROR)
		return;

	xb_init(&b, c->order);
	xb_u16(&b, imid);
	xb_u16(&b, 0);
	xb_u16(&b, XIM_IMID_VALID);
	xb_u16(&b, XIM_BadProtocol);
	xb_u16(&b, 0);
	xb_u16(&b, 0);
	xim_send(c, XIM_ERROR, 0, &b);
	free(b.data);

	weston_log("xim-server: unhandled opcode %u from 0x%x\n", major,
		   c->client_win);
}

static void
xim_dispatch(struct xim_client *c, const uint8_t *pkt, size_t len)
{
	uint8_t major = pkt[0];
	const uint8_t *body = pkt + XIM_HEADER_SIZE;
	size_t body_len = len - XIM_HEADER_SIZE;

	if (!c->order_known)
		c->order = XIM_LITTLEENDIAN;

	{
		uint32_t declared = (uint32_t)rd16(pkt + 2, c->order) * 4;

		if (declared < body_len)
			body_len = declared;
	}

	switch (major) {
	case XIM_CONNECT:
		xim_on_connect(c, body, body_len);
		break;
	case XIM_DISCONNECT: {
		struct xbuf b;

		xb_init(&b, c->order);
		xb_u16(&b, (body_len >= 2) ? rd16(body, c->order) : c->imid);
		xb_u16(&b, 0);
		xim_send(c, XIM_DISCONNECT_REPLY, 0, &b);
		free(b.data);

		c->opened = false;
		c->focus_ic = NULL;
		xim_report_focus(c->srv);
		break;
	}
	case XIM_OPEN:
		xim_on_open(c, body, body_len);
		break;
	case XIM_CLOSE:
		xim_on_close(c, body, body_len);
		break;
	case XIM_ENCODING_NEGOTIATION:
		xim_on_encoding(c, body, body_len);
		break;
	case XIM_QUERY_EXTENSION:
		xim_on_query_extension(c, body, body_len);
		break;
	case XIM_GET_IM_VALUES:
		xim_on_get_im_values(c, body, body_len);
		break;
	case XIM_SET_IM_VALUES:
		xim_on_set_im_values(c, body, body_len);
		break;
	case XIM_CREATE_IC:
		xim_on_create_ic(c, body, body_len);
		break;
	case XIM_DESTROY_IC:
		xim_on_destroy_ic(c, body, body_len);
		break;
	case XIM_SET_IC_VALUES:
		xim_on_set_ic_values(c, body, body_len);
		break;
	case XIM_GET_IC_VALUES:
		xim_on_get_ic_values(c, body, body_len);
		break;
	case XIM_SET_IC_FOCUS:
		xim_on_set_ic_focus(c, body, body_len, true);
		break;
	case XIM_UNSET_IC_FOCUS:
		xim_on_set_ic_focus(c, body, body_len, false);
		break;
	case XIM_FORWARD_EVENT:
		xim_on_forward_event(c, body, body_len);
		break;
	case XIM_SYNC:
		xim_on_sync(c, body, body_len);
		break;
	case XIM_RESET_IC:
		xim_on_reset_ic(c, body, body_len);
		break;
	case XIM_PREEDIT_START_REPLY:
	case XIM_PREEDIT_CARET_REPLY:
		break;
	default:
		xim_on_unhandled(c, major, body, body_len);
		break;
	}
}

static void
xim_client_reset(struct xim_client *c)
{
	struct xim_ic *ic, *tmp;

	wl_list_for_each_safe(ic, tmp, &c->ics, link) {
		wl_list_remove(&ic->link);
		xim_ic_free(ic);
	}
	c->rx_len = 0;
	c->order_known = false;
	c->order = XIM_LITTLEENDIAN;
	c->opened = false;
	c->focus_ic = NULL;
	xim_report_focus(c->srv);
}

static void
xim_client_feed(struct xim_client *c, const uint8_t *data, size_t len)
{
	if (c->rx_len + len > XIM_MAX_PACKET) {
		xim_client_reset(c);
		return;
	}

	if (c->rx_len + len > c->rx_cap) {
		size_t cap = c->rx_cap ? c->rx_cap : 512;
		uint8_t *n;

		while (cap < c->rx_len + len)
			cap *= 2;
		n = realloc(c->rx, cap);
		if (!n) {
			xim_client_reset(c);
			return;
		}
		c->rx = n;
		c->rx_cap = cap;
	}

	memcpy(c->rx + c->rx_len, data, len);
	c->rx_len += len;

	if (!c->order_known && c->rx_len >= 5) {
		c->order = (c->rx[4] == XIM_BIGENDIAN) ? XIM_BIGENDIAN
						      : XIM_LITTLEENDIAN;
		c->order_known = true;
	}

	for (;;) {
		size_t total;

		if (c->rx_len < XIM_HEADER_SIZE)
			return;
		if (!c->order_known)
			return;

		total = XIM_HEADER_SIZE + (size_t)rd16(c->rx + 2, c->order) * 4;
		if (total < XIM_HEADER_SIZE || total > XIM_MAX_PACKET) {
			xim_client_reset(c);
			return;
		}
		if (c->rx_len < total)
			return;

		xim_dispatch(c, c->rx, total);
		memmove(c->rx, c->rx + total, c->rx_len - total);
		c->rx_len -= total;

		{
			size_t skip = 0;

			while (skip < c->rx_len && c->rx[skip] == 0)
				skip++;
			if (skip) {
				memmove(c->rx, c->rx + skip, c->rx_len - skip);
				c->rx_len -= skip;
			}
		}
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
	if (!rep)
		return;

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
xim_send_xconnect_reply(struct xim_server *srv, struct xim_client *c)
{
	xcb_client_message_event_t reply;

	memset(&reply, 0, sizeof reply);
	reply.response_type = XCB_CLIENT_MESSAGE;
	reply.window = c->client_win;
	reply.type = srv->a_xim_xconnect;
	reply.format = 32;
	reply.data.data32[0] = c->comm_win;
	reply.data.data32[1] = XIM_TRANSPORT_MAJOR;
	reply.data.data32[2] = XIM_TRANSPORT_MINOR;
	reply.data.data32[3] = 0;
	reply.data.data32[4] = 0;

	xcb_send_event(srv->conn, 0, c->client_win, XCB_EVENT_MASK_NO_EVENT,
		       (const char *)&reply);
	xcb_flush(srv->conn);
}

static void
xim_new_connection(struct xim_server *srv, xcb_client_message_event_t *ev)
{
	struct xim_client *c;
	uint32_t evmask = XCB_EVENT_MASK_STRUCTURE_NOTIFY;

	if (!ev->data.data32[0])
		return;

	wl_list_for_each(c, &srv->clients, link) {
		if (c->client_win == ev->data.data32[0]) {
			xim_send_xconnect_reply(srv, c);
			return;
		}
	}

	c = zalloc(sizeof *c);
	if (!c)
		return;

	c->srv = srv;
	c->client_win = ev->data.data32[0];
	c->order = XIM_LITTLEENDIAN;
	c->comm_win = xcb_generate_id(srv->conn);
	wl_list_init(&c->ics);
	wl_list_insert(&srv->clients, &c->link);

	xcb_create_window(srv->conn, XCB_COPY_FROM_PARENT, c->comm_win,
			  srv->screen->root, 0, 0, 1, 1, 0,
			  XCB_WINDOW_CLASS_INPUT_OUTPUT,
			  srv->screen->root_visual, 0, NULL);

	xcb_change_window_attributes(srv->conn, c->client_win,
				     XCB_CW_EVENT_MASK, &evmask);

	xim_send_xconnect_reply(srv, c);

	weston_log("xim-server: client 0x%x -> protocol window 0x%x\n",
		   c->client_win, c->comm_win);
}

static void
xim_client_gone(struct xim_server *srv, xcb_window_t win)
{
	struct xim_client *c, *ctmp;

	wl_list_for_each_safe(c, ctmp, &srv->clients, link) {
		struct xim_ic *ic, *ictmp;

		if (c->client_win != win)
			continue;

		wl_list_for_each_safe(ic, ictmp, &c->ics, link) {
			wl_list_remove(&ic->link);
			xim_ic_free(ic);
		}
		if (c->comm_win)
			xcb_destroy_window(srv->conn, c->comm_win);
		wl_list_remove(&c->link);
		free(c->rx);
		free(c);

		xim_report_focus(srv);
		weston_log("xim-server: client 0x%x went away\n", win);
		break;
	}
}

static void
xim_free_clients(struct xim_server *srv)
{
	struct xim_client *c, *ctmp;

	wl_list_for_each_safe(c, ctmp, &srv->clients, link) {
		struct xim_ic *ic, *ictmp;

		wl_list_for_each_safe(ic, ictmp, &c->ics, link) {
			wl_list_remove(&ic->link);
			xim_ic_free(ic);
		}
		wl_list_remove(&c->link);
		free(c->rx);
		free(c);
	}
}

static void
xim_drop_connection(struct xim_server *srv)
{
	if (srv->xcb_source) {
		wl_event_source_remove(srv->xcb_source);
		srv->xcb_source = NULL;
	}

	xim_free_clients(srv);
	xim_report_focus(srv);

	if (srv->conn) {
		xcb_disconnect(srv->conn);
		srv->conn = NULL;
	}

	srv->connected = false;
}

static int
xim_handle_events(int fd, uint32_t mask, void *data)
{
	struct xim_server *srv = data;
	xcb_generic_event_t *ev;

	(void)fd;
	if (!(mask & WL_EVENT_READABLE))
		return 0;

	while ((ev = xcb_poll_for_event(srv->conn))) {
		uint8_t type = ev->response_type & ~0x80;

		switch (type) {
		case XCB_CLIENT_MESSAGE: {
			xcb_client_message_event_t *cm =
				(xcb_client_message_event_t *)ev;

			if (cm->type == srv->a_xim_xconnect) {
				xim_new_connection(srv, cm);
			} else if (cm->type == srv->a_xim_protocol ||
				   cm->type == srv->a_xim_moredata) {
				struct xim_client *c =
					xim_client_find(srv, cm->window);

				if (!c)
					break;

				if (cm->format == 8)
					xim_client_feed(c, cm->data.data8,
							XIM_CM_DATA_SIZE);
				else if (cm->format == 32)
					xim_read_property(c, cm->window,
							  cm->data.data32[1]);
			}
			break;
		}
		case XCB_SELECTION_REQUEST:
			xim_answer_selection(srv,
					     (xcb_selection_request_event_t *)ev);
			break;
		case XCB_DESTROY_NOTIFY:
			xim_client_gone(srv,
					((xcb_destroy_notify_event_t *)ev)->window);
			break;
		default:
			break;
		}

		free(ev);
	}

	if (xcb_connection_has_error(srv->conn)) {
		weston_log("xim-server: X connection lost, will reconnect\n");
		xim_drop_connection(srv);
		return 0;
	}

	xcb_flush(srv->conn);
	return 0;
}

static bool
xim_display_socket_ready(const char *display)
{
	const char *colon;
	char path[64];
	struct stat st;

	colon = strrchr(display, ':');
	if (!colon)
		return false;

	snprintf(path, sizeof path, "/tmp/.X11-unix/X%d", atoi(colon + 1));
	return stat(path, &st) == 0;
}

static xcb_connection_t *
xim_connect_display(const char *display)
{
	const char *colon;
	struct sockaddr_un addr;
	xcb_connection_t *conn;
	int fd, screen;

	colon = strrchr(display, ':');
	if (!colon)
		return NULL;
	screen = atoi(colon + 1);

	memset(&addr, 0, sizeof addr);
	addr.sun_family = AF_UNIX;
	snprintf(addr.sun_path, sizeof addr.sun_path, "/tmp/.X11-unix/X%d",
		 screen);

	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return NULL;

	if (connect(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
		close(fd);
		return NULL;
	}

	conn = xcb_connect_to_fd(fd, NULL);
	if (!conn)
		return NULL;
	if (xcb_connection_has_error(conn)) {
		xcb_disconnect(conn);
		return NULL;
	}

	return conn;
}

static xcb_connection_t *
xim_wm_connection(struct weston_compositor *ec)
{
	const struct weston_xwayland_api *api;
	struct weston_xwayland *xw;
	struct xim_xserver_proxy *xsp;
	struct xim_xwm_proxy *wm;

	api = weston_xwayland_get_api(ec);
	if (!api || !api->get)
		return NULL;
	xw = api->get(ec);
	if (!xw)
		return NULL;

	xsp = (struct xim_xserver_proxy *)xw;
	wm = xsp->wm;
	if (!wm || !wm->conn)
		return NULL;
	if (xcb_connection_has_error(wm->conn))
		return NULL;

	return wm->conn;
}

static void
xim_destroy(struct xim_server *srv)
{
	if (srv->xcb_source)
		wl_event_source_remove(srv->xcb_source);
	if (srv->efd_source)
		wl_event_source_remove(srv->efd_source);
	if (srv->efd >= 0)
		close(srv->efd);
	if (srv->bridge)
		srv->bridge->set_xim_sink(srv->ec, NULL, NULL, NULL);

	xim_free_clients(srv);

	if (srv->conn)
		xcb_disconnect(srv->conn);
	free(srv->name);
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
	struct xim_server *srv = g_xim;
	uint64_t v = 1;

	if (!srv || srv->efd < 0 || srv->connected)
		return;

	if (write(srv->efd, &v, sizeof v) != (ssize_t)sizeof v)
		return;
}

int
xim_server_init(struct weston_compositor *ec)
{
	struct xim_server *srv = g_xim;
	struct wl_event_loop *loop;
	const char *display, *name;
	const xcb_setup_t *setup;
	xcb_screen_iterator_t it;
	uint32_t evmask = XCB_EVENT_MASK_PROPERTY_CHANGE;

	if (srv && srv->connected)
		return 0;

	if (!srv) {
		srv = zalloc(sizeof *srv);
		if (!srv)
			return -1;

		srv->ec = ec;
		srv->efd = -1;
		wl_list_init(&srv->clients);
		srv->next_imid = 1;
		srv->next_icid = 1;
		srv->destroy_listener.notify = xim_compositor_destroy;

		srv->efd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
		if (srv->efd >= 0) {
			loop = wl_display_get_event_loop(ec->wl_display);
			srv->efd_source = wl_event_loop_add_fd(loop, srv->efd,
					WL_EVENT_READABLE, xim_efd_cb, srv);
		}

		srv->bridge = weston_plugin_api_get(ec,
				WESTON_TEXT_INPUT_BRIDGE_API_NAME,
				sizeof(struct weston_text_input_bridge_api));
		if (!srv->bridge) {
			weston_log("xim-server: no text-input bridge; "
				   "X11 input disabled\n");
			if (srv->efd_source)
				wl_event_source_remove(srv->efd_source);
			if (srv->efd >= 0)
				close(srv->efd);
			free(srv);
			return 0;
		}

		name = getenv("XIM_SERVER_NAME");
		if (!name || !name[0])
			name = DEFAULT_SERVER_NAME;
		srv->name = strdup(name);
		if (!srv->name) {
			if (srv->efd_source)
				wl_event_source_remove(srv->efd_source);
			if (srv->efd >= 0)
				close(srv->efd);
			free(srv);
			return -1;
		}

		g_xim = srv;

		{
			static const struct weston_xim_server_api api = {
				.init = xim_server_init,
				.request = xim_server_request,
			};

			weston_plugin_api_register(ec, WESTON_XIM_SERVER_API_NAME,
						   &api, sizeof api);
		}
	}

	display = getenv("DISPLAY");
	if (!display || !display[0]) {
		weston_log("xim-server: DISPLAY unset, will retry\n");
		return 0;
	}

	if (!xim_wm_connection(ec)) {
		weston_log("xim-server: XWayland is not up yet, will retry\n");
		return 0;
	}

	if (!xim_display_socket_ready(display)) {
		weston_log("xim-server: X socket for %s not there yet, "
			   "will retry\n", display);
		return 0;
	}

	if (srv->conn) {
		xcb_disconnect(srv->conn);
		srv->conn = NULL;
	}

	srv->conn = xim_connect_display(display);
	if (!srv->conn) {
		weston_log("xim-server: cannot connect to %s, will retry\n",
			   display);
		return 0;
	}

	setup = xcb_get_setup(srv->conn);
	it = xcb_setup_roots_iterator(setup);
	if (!it.rem) {
		weston_log("xim-server: %s reports no screen, will retry\n",
			   display);
		xcb_disconnect(srv->conn);
		srv->conn = NULL;
		return 0;
	}
	srv->screen = it.data;

	srv->server_win = xcb_generate_id(srv->conn);
	xcb_create_window(srv->conn, XCB_COPY_FROM_PARENT, srv->server_win,
			  srv->screen->root, 0, 0, 1, 1, 0,
			  XCB_WINDOW_CLASS_INPUT_OUTPUT,
			  srv->screen->root_visual, 0, NULL);
	{
		uint32_t ovr = 1;

		xcb_change_window_attributes(srv->conn, srv->server_win,
					     XCB_CW_OVERRIDE_REDIRECT, &ovr);
	}
	xcb_map_window(srv->conn, srv->server_win);
	xcb_change_window_attributes(srv->conn, srv->server_win,
				     XCB_CW_EVENT_MASK, &evmask);

	if (!xim_register_server(srv)) {
		weston_log("xim-server: cannot register '@server=%s', "
			   "will retry\n", srv->name);
		xcb_destroy_window(srv->conn, srv->server_win);
		xcb_disconnect(srv->conn);
		srv->conn = NULL;
		return 0;
	}

	{
		int fd = xcb_get_file_descriptor(srv->conn);

		loop = wl_display_get_event_loop(ec->wl_display);
		srv->xcb_source = wl_event_loop_add_fd(loop, fd,
				WL_EVENT_READABLE, xim_handle_events, srv);
		if (!srv->xcb_source) {
			weston_log("xim-server: cannot watch the X connection, "
				   "will retry\n");
			xcb_disconnect(srv->conn);
			srv->conn = NULL;
			return 0;
		}
	}

	if (!srv->listener_attached) {
		wl_signal_add(&ec->destroy_signal, &srv->destroy_listener);
		srv->listener_attached = true;
	}
	srv->bridge->set_xim_sink(ec, xim_bridge_commit, xim_bridge_preedit, srv);
	srv->connected = true;

	weston_log("xim-server: serving XIM on %s as '@server=%s' "
		   "(XIMPreeditCallbacks preferred, Compound Text); "
		   "use XMODIFIERS=@im=%s\n", display, srv->name, srv->name);

	return 0;
}
