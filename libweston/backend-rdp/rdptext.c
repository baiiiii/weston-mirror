/*
 * Copyright 2026 Weston-mirror contributors
 *
 * MS-RDPETXT: Remote Desktop Protocol: Text Input Virtual Channel Extension
 * (server side, per [MS-RDPETXT] v20241119)
 *
 * Enables the host-side Windows IME (e.g. Microsoft Pinyin running inside
 * msrdc.exe) to compose text and inject the committed string into the
 * focused Wayland surface, mirroring what WSA does for Android.  The
 * client side is the WSL::TextBridge component of WSLDVCPlugin.dll
 * (our fork of the open-source WSLg plugin), which creates a
 * RemoteTextConnection with IsEnabled=true; msrdc's built-in
 * remotetextplugin would keep integration disabled (product gate) and
 * already holds the spec channel names, so the bridge listens on
 * custom names instead.
 *
 * Transport: two dynamic virtual channels opened by this server via
 * WTSVirtualChannelOpenEx(WTS_CHANNEL_OPTION_DYNAMIC):
 *   - "WSL::TextBridge::ServerToClient": server -> client
 *   - "WSL::TextBridge::ClientToServer": client -> server
 *
 * Handshake sequence (MS-RDPETXT section 1.7 / 3.1.5):
 *
 *   weston                                   msrdc.exe
 *     |-- DYNVC CREATE "WSL::...S2C" ------->|
 *     |-- DYNVC CREATE "WSL::...C2S" ------->|
 *     |<-- CREATE_RESPONSE (join) -----------|   (dvc_open_state SUCCEEDED)
 *     |-- NOTIFY_SERVER_VERSION 0x031A ----->|
 *     |<-- NOTIFY_CLIENT_VERSION 0x0604 -----|
 *     |-- REGISTER_REMOTE_* / HOST_* ------->|   (one text target, one
 *     |                                         key target, one edit ctl)
 *     |<-> EDIT_CONTROL_FOCUS on activation -|
 *     |<-- UPDATE_COMPOSITION (preedit) -----|
 *     |<-- UPDATE_TEXT / SET_COMPOSITION_INFO  (commit)
 *     |<-- KEY_EVENT (unconsumed keys) ------|
 *
 * IMPORTANT: WTSVirtualChannelWrite() on a DVC does NOT buffer until the
 * client confirms the channel: libfreerdp sends the DYNVC DATA PDU
 * synchronously as soon as drdynvc is READY. Data written before the
 * CREATE_RESPONSE arrives would reach an unknown channelId, so PDUs are
 * only sent after WTSVirtualChannelQuery(WTSVirtualChannelReady) reports
 * both channels open (same pattern as FreeRDP's audin/rdpsnd server).
 *
 * Every PDU is preceded by a 6-byte header:
 *   size  (4 bytes, UINT32 LE): size of the PDU excluding this field,
 *         i.e. 2 + payload length
 *   pduId (2 bytes, UINT16 LE)
 *
 * Text is UTF-16LE on the wire; converted to/from UTF-8 for Wayland.
 */

#include <config.h>

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <freerdp/freerdp.h>
#include <freerdp/channels/wtsvc.h>

#include <libweston/libweston.h>
#include <libweston/plugin-registry.h>
#include <libweston/text-input-bridge.h>

#include "rdp.h"

/* 客户端侧的文本输入桥（WSLDVCPlugin 的 WSL::TextBridge 组件）监听
 * 这两个自定义 DVC。规格点名的 TextInput_*DVC 两名已被 msrdc 内置
 * remotetextplugin 占用（其连接处于禁用状态），故改用本名；
 * 通道内 PDU 逐字节为 MS-RDPETXT 协议。 */
#define RDPTXT_CHANNEL_S2C "WSL::TextBridge::ServerToClient"
#define RDPTXT_CHANNEL_C2S "WSL::TextBridge::ClientToServer"

#define RDPTXT_HEADER_SIZE 6
#define RDPTXT_VERSION_MAJOR 1
/* minor is a bitmask of protocol updates (MS-RDPETXT 1.3.5).  The live
 * client reports (1,7); report the same set so the client's InputService
 * does not treat us as a pre-update peer and keep remote integration
 * disabled (it announces REMOTE_INTEGRATION_STATUS right after the
 * version exchange). */
#define RDPTXT_VERSION_MINOR 7

/* Give the client ~10 s to confirm both DYNVC CREATEs before assuming it
 * has no TextInput plugin (process() is pumped once per VCM wake). */
#define RDPTXT_READY_TIMEOUT_TICKS 600

/* Sanity limits: a single IME operation is never this large. */
#define RDPTXT_MAX_TEXT_UNITS 65536
#define RDPTXT_MAX_CLAUSES 64
#define RDPTXT_MAX_KEYSTATS 1024
#define RDPTXT_MAX_PDU_SIZE (1024 * 1024)

/* Fixed object ids used on this connection. The client only needs the ids
 * to be consistent between registration and focus/content PDUs. */
#define RDPTXT_TEXT_TARGET_ID 1  /* TextInputClient (text target) */
#define RDPTXT_HOST_ID 1         /* TextInputHost (key target) */
#define RDPTXT_EDIT_CONTROL_ID 1 /* the (single) virtual edit control */

/* window type "Legacy": a traditional Win32 window, which is what the
 * client's InputService expects for a RAIL server app */
#define RDPTXT_HOST_TYPE_LEGACY 3

/* PDU identifiers, MS-RDPETXT section 2.2.2 */
#define RDPTXT_PDU_KEY_EVENT                       0x0100
#define RDPTXT_PDU_ACKNOWLEDGE_HOST_OPERATION      0x0101
#define RDPTXT_PDU_CHARACTER_EVENT                 0x0102
#define RDPTXT_PDU_ENABLE_WINDOW                   0x0105
#define RDPTXT_PDU_UPDATE_TEXT                     0x0200
#define RDPTXT_PDU_UPDATE_TEXT_AND_SELECTION       0x0201
#define RDPTXT_PDU_SET_SELECTION                   0x0202
#define RDPTXT_PDU_UPDATE_FORMAT                   0x0203
#define RDPTXT_PDU_UPDATE_COMPOSITION              0x0204
#define RDPTXT_PDU_SET_COMPOSITION_INFO            0x0205
#define RDPTXT_PDU_RECONVERSION_CANDIDATES         0x0206
#define RDPTXT_PDU_UPDATE_INPUT_PROFILE            0x0208
#define RDPTXT_PDU_UPDATE_MODE                     0x0209
#define RDPTXT_PDU_SET_CONVERSION_MODE             0x020A
#define RDPTXT_PDU_ACKNOWLEDGE_OPERATION           0x020B
#define RDPTXT_PDU_ERROR_REPORT                    0x020C
#define RDPTXT_PDU_REGISTER_REMOTE_TEXT_TARGET     0x0300
#define RDPTXT_PDU_REGISTER_REMOTE_KEY_TARGET      0x0301
#define RDPTXT_PDU_REGISTER_REMOTE_EDIT_CONTROL    0x0302
#define RDPTXT_PDU_UNREGISTER_REMOTE_EDIT_CONTROL  0x0306
#define RDPTXT_PDU_EDIT_CONTROL_FOCUS              0x0308
#define RDPTXT_PDU_HOST_FOCUS                      0x0309
#define RDPTXT_PDU_HOST_FOREGROUND                 0x030A
#define RDPTXT_PDU_SELECTION_CHANGED               0x030B
#define RDPTXT_PDU_TEXT_CHANGED                    0x030C
#define RDPTXT_PDU_GEOMETRY_CHANGED                0x030F
#define RDPTXT_PDU_NOTIFY_SERVER_VERSION           0x031A
#define RDPTXT_PDU_ACKNOWLEDGE_REMOTE_OPERATION    0x0312
#define RDPTXT_PDU_ACKNOWLEDGE_KEY_EVENT           0x0313
#define RDPTXT_PDU_INPUT_PROFILE_CHANGED           0x0314
#define RDPTXT_PDU_REMOTE_TEXT_TARGET_THREAD_PROPS 0x0322
#define RDPTXT_PDU_OCCLUDING_VIEWS                 0x0400
#define RDPTXT_PDU_REMOTE_INTEGRATION_STATUS       0x0602
#define RDPTXT_PDU_REREGISTRATION_REQUEST          0x0603
#define RDPTXT_PDU_NOTIFY_CLIENT_VERSION           0x0604
#define RDPTXT_PDU_REPORT_CLIENT_OPTIONS           0x0605

/* RDPTXT_UPDATE_MODE_PDU TextInputFeature bits (spec 2.2.2.19) */
#define RDPTXT_FEATURE_PREDICTION_MODE        0x00000001
#define RDPTXT_FEATURE_LAYOUT_CHANGE_TRACKING 0x00000004
#define RDPTXT_FEATURE_SELECTION_TRACKING     0x00000008

/* RDPTXT_UPDATE_COMPOSITION_PDU compositionAction values */
#define RDPTXT_COMPOSITION_ENTER  0x01
#define RDPTXT_COMPOSITION_LEAVE  0x02
#define RDPTXT_COMPOSITION_UPDATE 0x03

/* RDPTXT_KEY_EVENT_PDU routingStage values */
#define RDPTXT_KEY_ROUTING_NONE 0x02

/* KeyEventHostInfo EventFlags */
#define RDPTXT_KEY_FLAG_DOWN 0x0001
#define RDPTXT_KEY_FLAG_UP   0x0004

/* TextInputAcknowledgementType for RDPTXT_ACKNOWLEDGE_KEY_EVENT_PDU */
#define RDPTXT_KEY_ACK_COMPLETED 0x00000001

struct rdptext_state {
	RdpPeerContext *peer_ctx;
	HANDLE s2c_channel;   /* server -> client */
	HANDLE c2s_channel;   /* client -> server */
	DWORD session_id;     /* real session id for WTSVirtualChannelOpenEx */

	bool version_sent;
	bool client_version_received;
	bool ready_failed;    /* CREATE confirmation timed out */
	uint32_t ready_ticks;
	uint32_t client_version_major;
	uint32_t client_version_minor;

	bool registered;      /* registration PDUs sent */
	bool integration_enabled;    /* our gate, kept true (see handler) */
	bool client_integration_status; /* REMOTE_INTEGRATION_STATUS seen */
	bool window_input_enabled; /* ENABLE_WINDOW for our host id */
	bool focus_notified;  /* an edit control currently has focus */
	struct weston_surface *focused_surface;

	uint32_t host_focus_ordinal;
	uint32_t window_id;   /* RAIL window id of the focused surface */
	uint32_t notify_op_id; /* operationId for server->client notifications */
	uint32_t client_buf_len; /* 客户端编辑缓冲副本的 UTF-16 长度 */

	/* IME state assembled from client PDUs */
	bool composition_active;
	bool skip_identical_update_text;
	char *pending_commit;   /* SET_COMPOSITION_INFO determinedText */
	char *last_commit_text; /* last text we committed, for dedup */

	/* text-input bridge (zwp_text_input_v3, compositor/text-input-bridge.c) */
	const struct weston_text_input_bridge_api *bridge_api;
	bool bridge_listener_attached;

	bool verbose;
	int c2s_dump_count;
};

/* ------------------------------------------------------------------ */
/* Logging                                                             */
/* ------------------------------------------------------------------ */

static void
rdptext_log(struct rdptext_state *t, const char *fmt, ...)
{
	va_list ap;
	char buf[512];

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);

	/* weston_log lands in the always-on "log" scope, which the WSLg
	 * daemon collects into wlog.log - vital for first-time debugging. */
	(void)t;
	weston_log("rdptext: %s\n", buf);
}

#define rdptext_verbose(t, ...) \
	do { \
		if ((t) && (t)->verbose) \
			rdptext_log(t, __VA_ARGS__); \
	} while (0)

/* Hex dump of the first bytes of a message - vital to diagnose
 * client-side framing mismatches (log goes to weston.log). */
static void
rdptext_hex_dump(struct rdptext_state *t, const unsigned char *data,
		 size_t len)
{
	char buf[3 * 32 + 8];
	size_t i, n = len < 32 ? len : 32;

	(void)t;
	for (i = 0; i < n; i++)
		snprintf(buf + 3 * i, 4, "%02X ", data[i]);
	buf[3 * n] = '\0';
	weston_log("rdptext:   bytes: %s (%zu bytes total)\n", buf, len);
}

/* ------------------------------------------------------------------ */
/* UTF-16LE <-> UTF-8                                                  */
/* ------------------------------------------------------------------ */

static char *
rdptext_utf16_to_utf8(const UINT16 *wstr, size_t wchars)
{
	size_t i, out = 0;
	unsigned char *buf;

	/* Worst case: 3 bytes per UTF-16 unit (surrogate pairs emit 4 bytes
	 * for 2 units, so 3/unit never overflows). */
	buf = malloc(wchars * 3 + 1);
	if (!buf)
		return NULL;

	for (i = 0; i < wchars; i++) {
		uint32_t cp = wstr[i];

		if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < wchars &&
		    wstr[i + 1] >= 0xDC00 && wstr[i + 1] <= 0xDFFF) {
			/* surrogate pair */
			cp = 0x10000 + ((cp - 0xD800) << 10) + (wstr[++i] - 0xDC00);
		} else if (cp >= 0xD800 && cp <= 0xDFFF) {
			cp = 0xFFFD; /* unpaired surrogate */
		}

		if (cp < 0x80) {
			buf[out++] = (unsigned char)cp;
		} else if (cp < 0x800) {
			buf[out++] = 0xC0 | (cp >> 6);
			buf[out++] = 0x80 | (cp & 0x3F);
		} else if (cp < 0x10000) {
			buf[out++] = 0xE0 | (cp >> 12);
			buf[out++] = 0x80 | ((cp >> 6) & 0x3F);
			buf[out++] = 0x80 | (cp & 0x3F);
		} else {
			buf[out++] = 0xF0 | (cp >> 18);
			buf[out++] = 0x80 | ((cp >> 12) & 0x3F);
			buf[out++] = 0x80 | ((cp >> 6) & 0x3F);
			buf[out++] = 0x80 | (cp & 0x3F);
		}
	}
	buf[out] = '\0';
	return (char *)buf;
}

static UINT16 *
rdptext_utf8_to_utf16(const char *utf8, size_t *out_wchars)
{
	unsigned char *s = (unsigned char *)utf8;
	UINT16 *buf;
	size_t len = strlen(utf8);
	size_t i = 0, out = 0;

	buf = malloc((len + 1) * sizeof(UINT16));
	if (!buf)
		return NULL;

	while (i < len) {
		uint32_t cp;
		unsigned char c = s[i];

		if (c < 0x80) {
			cp = c;
			i += 1;
		} else if ((c & 0xE0) == 0xC0 && i + 1 < len) {
			cp = ((c & 0x1F) << 6) | (s[i + 1] & 0x3F);
			i += 2;
		} else if ((c & 0xF0) == 0xE0 && i + 2 < len) {
			cp = ((c & 0x0F) << 12) | ((s[i + 1] & 0x3F) << 6) |
			     (s[i + 2] & 0x3F);
			i += 3;
		} else if ((c & 0xF8) == 0xF0 && i + 3 < len) {
			cp = ((c & 0x07) << 18) | ((s[i + 1] & 0x3F) << 12) |
			     ((s[i + 2] & 0x3F) << 6) | (s[i + 3] & 0x3F);
			i += 4;
		} else {
			cp = 0xFFFD;
			i += 1;
		}

		if (cp >= 0x10000) {
			cp -= 0x10000;
			buf[out++] = 0xD800 | (cp >> 10);
			buf[out++] = 0xDC00 | (cp & 0x3FF);
		} else {
			buf[out++] = (UINT16)cp;
		}
	}

	buf[out] = 0;
	if (out_wchars)
		*out_wchars = out;
	return buf;
}

/* ------------------------------------------------------------------ */
/* Channel state helpers                                               */
/* ------------------------------------------------------------------ */

/* TRUE once the client answered our DYNVC CREATE_REQUEST.
 * FALSE on failure or while still pending. */
static BOOL
rdptext_channel_is_ready(HANDLE channel)
{
	BOOL ready = FALSE;
	PVOID buffer = NULL;
	DWORD bytes_returned = 0;

	if (!channel)
		return FALSE;

	if (!WTSVirtualChannelQuery(channel, WTSVirtualChannelReady,
				    &buffer, &bytes_returned))
		return FALSE;

	if (buffer && bytes_returned == sizeof(BOOL))
		ready = *((BOOL *)buffer);

	WTSFreeMemory(buffer);
	return ready;
}

/* ------------------------------------------------------------------ */
/* PDU send                                                            */
/* ------------------------------------------------------------------ */

static BOOL
rdptext_send_pdu(struct rdptext_state *t, HANDLE channel, UINT16 pdu_id,
		 const void *payload, UINT32 payload_len)
{
	wStream *s;
	ULONG written;
	BOOL ret;

	s = Stream_New(NULL, RDPTXT_HEADER_SIZE + payload_len);
	if (!s)
		return FALSE;

	/* size excludes the 4-byte size field itself, but includes pduId */
	Stream_Write_UINT32(s, 2 + payload_len);
	Stream_Write_UINT16(s, pdu_id);
	if (payload_len)
		Stream_Write(s, payload, payload_len);

	ret = WTSVirtualChannelWrite(channel, (PCHAR)Stream_Buffer(s),
				     Stream_GetPosition(s), &written);
	Stream_Free(s, TRUE);

	if (!ret)
		rdptext_log(t, "failed to send PDU 0x%04X", pdu_id);

	return ret;
}

static BOOL
rdptext_s2c_ok(struct rdptext_state *t)
{
	return t->integration_enabled && t->window_input_enabled &&
	       t->s2c_channel;
}

static void
rdptext_send_server_version(struct rdptext_state *t)
{
	wStream *payload;
	unsigned char zeros[16] = { 0 };
	BOOL ret;

	/* RDPTXT_NOTIFY_SERVER_VERSION_PDU payload:
	 *   containerId   (16 bytes, all zeros)
	 *   versionMajor  (4 bytes, UINT32 LE)
	 *   versionMinor  (4 bytes, UINT32 LE)
	 */
	payload = Stream_New(NULL, 24);
	if (!payload)
		return;

	Stream_Write(payload, zeros, sizeof(zeros));
	Stream_Write_UINT32(payload, RDPTXT_VERSION_MAJOR);
	Stream_Write_UINT32(payload, RDPTXT_VERSION_MINOR);

	ret = rdptext_send_pdu(t, t->s2c_channel,
			       RDPTXT_PDU_NOTIFY_SERVER_VERSION,
			       Stream_Buffer(payload), 24);
	Stream_Free(payload, TRUE);

	rdptext_log(t, "sent NOTIFY_SERVER_VERSION %u.%u (ret=%d)",
		    RDPTXT_VERSION_MAJOR, RDPTXT_VERSION_MINOR, ret);
}

/* REGISTER_REMOTE_TEXT_TARGET_PDU: textTargetId (4 bytes) */
static void
rdptext_send_register_text_target(struct rdptext_state *t)
{
	wStream *s;

	if (!rdptext_s2c_ok(t))
		return;

	s = Stream_New(NULL, 4);
	if (!s)
		return;
	Stream_Write_UINT32(s, RDPTXT_TEXT_TARGET_ID);

	rdptext_send_pdu(t, t->s2c_channel,
			 RDPTXT_PDU_REGISTER_REMOTE_TEXT_TARGET,
			 Stream_Buffer(s), 4);
	Stream_Free(s, TRUE);
}

/* REGISTER_REMOTE_KEY_TARGET_PDU:
 *   objectId (4), textTargetId (4), textInputHostSettings (7),
 *   viewInstanceId (8), windowInstanceId (8)
 */
static void
rdptext_send_register_key_target(struct rdptext_state *t)
{
	wStream *s;

	if (!rdptext_s2c_ok(t))
		return;

	s = Stream_New(NULL, 31);
	if (!s)
		return;
	Stream_Write_UINT32(s, RDPTXT_HOST_ID);
	Stream_Write_UINT32(s, RDPTXT_TEXT_TARGET_ID);
	/* TextInputHostSettings: Type (4), InputEnabledOnWindowByApp (1),
	 * IsOwnerWin32 (1), IsOwnerAppFrame (1) */
	Stream_Write_UINT32(s, RDPTXT_HOST_TYPE_LEGACY);
	Stream_Write_UINT8(s, 1); /* input enabled on window by app */
	Stream_Write_UINT8(s, 1); /* owner is a Win32 window */
	Stream_Write_UINT8(s, 0); /* not an application frame */
	Stream_Write_UINT64(s, t->window_id); /* viewInstanceId */
	Stream_Write_UINT64(s, t->window_id); /* windowInstanceId */

	rdptext_send_pdu(t, t->s2c_channel,
			 RDPTXT_PDU_REGISTER_REMOTE_KEY_TARGET,
			 Stream_Buffer(s), 31);
	Stream_Free(s, TRUE);
}

/* REGISTER_REMOTE_EDIT_CONTROL_PDU:
 *   appNameLength (4), appName (UTF-16), editClientOperationId (4),
 *   editControlId (4), textInputClientId (4)
 */
static void
rdptext_send_register_edit_control(struct rdptext_state *t)
{
	wStream *s;
	UINT16 *app_name;
	size_t name_wchars = 0;
	UINT32 payload_len;
	static const char app_name_utf8[] = "weston";

	if (!rdptext_s2c_ok(t))
		return;

	app_name = rdptext_utf8_to_utf16(app_name_utf8, &name_wchars);
	if (!app_name)
		return;

	payload_len = 4 + (UINT32)(name_wchars * 2) + 12;

	s = Stream_New(NULL, payload_len);
	if (!s) {
		free(app_name);
		return;
	}

	Stream_Write_UINT32(s, (UINT32)name_wchars);
	Stream_Write(s, app_name, name_wchars * 2);
	Stream_Write_UINT32(s, ++t->host_focus_ordinal); /* editClientOperationId */
	Stream_Write_UINT32(s, RDPTXT_EDIT_CONTROL_ID);
	Stream_Write_UINT32(s, RDPTXT_TEXT_TARGET_ID);

	rdptext_send_pdu(t, t->s2c_channel,
			 RDPTXT_PDU_REGISTER_REMOTE_EDIT_CONTROL,
			 Stream_Buffer(s), payload_len);
	Stream_Free(s, TRUE);
	free(app_name);
}

/* REMOTE_TEXT_TARGET_THREAD_PROPERTIES_PDU:
 *   textInputClientId (4), threadProperties (4)
 */
static void
rdptext_send_thread_properties(struct rdptext_state *t)
{
	wStream *s;

	if (!rdptext_s2c_ok(t))
		return;

	s = Stream_New(NULL, 8);
	if (!s)
		return;
	Stream_Write_UINT32(s, RDPTXT_TEXT_TARGET_ID);
	Stream_Write_UINT32(s, 0x00000001); /* IsWin32App */

	rdptext_send_pdu(t, t->s2c_channel,
			 RDPTXT_PDU_REMOTE_TEXT_TARGET_THREAD_PROPS,
			 Stream_Buffer(s), 8);
	Stream_Free(s, TRUE);
}

/* HOST_FOREGROUND_PDU: objectId (4), windowInstanceId (8) */
static void
rdptext_send_host_foreground(struct rdptext_state *t)
{
	wStream *s;

	if (!rdptext_s2c_ok(t))
		return;

	s = Stream_New(NULL, 12);
	if (!s)
		return;
	Stream_Write_UINT32(s, RDPTXT_HOST_ID);
	Stream_Write_UINT64(s, t->window_id);

	rdptext_send_pdu(t, t->s2c_channel, RDPTXT_PDU_HOST_FOREGROUND,
			 Stream_Buffer(s), 12);
	Stream_Free(s, TRUE);
}

/* HOST_FOCUS_PDU: textInputHostId (4), ordinal (4), gainingFocus (1),
 * override (1) */
static void
rdptext_send_host_focus(struct rdptext_state *t, bool gaining)
{
	wStream *s;

	if (!rdptext_s2c_ok(t))
		return;

	s = Stream_New(NULL, 10);
	if (!s)
		return;
	Stream_Write_UINT32(s, RDPTXT_HOST_ID);
	Stream_Write_UINT32(s, ++t->host_focus_ordinal);
	Stream_Write_UINT8(s, gaining ? 1 : 0);
	Stream_Write_UINT8(s, 0); /* override */

	rdptext_send_pdu(t, t->s2c_channel, RDPTXT_PDU_HOST_FOCUS,
			 Stream_Buffer(s), 10);
	Stream_Free(s, TRUE);
}

/* Map the focused surface's text cursor rectangle from weston surface
 * coordinates into RDP client desktop coordinates. Returns false when the
 * rectangle is unknown. */
static bool
rdptext_get_cursor_bounds(struct rdptext_state *t, struct weston_surface *surface,
			  pixman_box32_t *bounds)
{
	struct rdp_backend *b = t->peer_ctx->rdpBackend;
	struct weston_view *view, *iter;
	struct weston_output *output;
	int32_t x, y, w, h;
	float gx1, gy1, gx2, gy2;

	if (!t->bridge_api ||
	    !t->bridge_api->get_cursor_rect(b->compositor, &x, &y, &w, &h))
		return false;
	/* GTK（gtktextview.c 的 update_im_spot_location）把光标矩形的宽度固定
	 * 报成 0：宽度只用来表示方向，真正有效的是 x/y/height。这里不能因为
	 * w == 0 就放弃，否则永远拿不到应用的光标位置，只能退回整个窗口，
	 * 客户端据此摆的候选窗就落在窗口角落。 */
	if (w < 0 || h <= 0)
		return false;
	if (w == 0)
		w = 1;

	/* the surface's primary view maps surface-local to global */
	output = rdp_output_get_primary(b->compositor);
	view = NULL;
	wl_list_for_each(iter, &surface->views, surface_link) {
		if (iter->output) {
			view = iter;
			output = iter->output;
			break;
		}
	}
	if (!view || !output)
		return false;

	/* 应用的矩形是**表面本地**坐标（协议规定如此）。Wayland 客户端拿不到
	 * 自己的全局位置——GTK 的 gdk_window_get_root_coords 在 Wayland 下返回
	 * 的也只是 toplevel 相对坐标——所以必须叠加视图的全局位置，再换算到
	 * 客户端坐标。实测（窗口 real 277,95 + 光标 real 52,90）不叠加时算出
	 * 的插入符会落在窗口外面，候选窗因此跑到屏幕底部。 */
	weston_view_to_global_float(view, (float)x, (float)y, &gx1, &gy1);
	weston_view_to_global_float(view, (float)(x + w), (float)(y + h),
				    &gx2, &gy2);

	bounds->x1 = (int32_t)gx1;
	bounds->y1 = (int32_t)gy1;
	bounds->x2 = (int32_t)gx2;
	bounds->y2 = (int32_t)gy2;

	to_client_coordinate(t->peer_ctx, output,
			     &bounds->x1, &bounds->y1, NULL, NULL);
	to_client_coordinate(t->peer_ctx, output,
			     &bounds->x2, &bounds->y2, NULL, NULL);

	return true;
}

/* 聚焦编辑控件的客户端坐标矩形 = 整个窗口区域（controlBounds）。
 *
 * 规格（MS-RDPETXT 2.2.2.39）里 controlBounds 是"编辑控件"矩形、rangeBounds
 * 才是光标/选区矩形，本函数只负责前者；光标矩形由 rdptext_get_cursor_bounds()
 * 单独取，在 GEOMETRY_CHANGED 里用作 rangeBounds。
 *
 * 注：曾怀疑"两个字段都填成 1px 宽光标细条导致 Windows 输入法把候选窗退化成
 * 屏幕底部横条"，客户端改写两字段后逐档对照（0/1/2/3）截图与像素比对显示
 * 候选条位置完全不变，该假设已证伪；此处仅按规格对齐字段含义。 */
static bool
	rdptext_get_control_bounds(struct rdptext_state *t,
				  struct weston_surface *surface,
				  pixman_box32_t *bounds)
{
	struct rdp_backend *b = t->peer_ctx->rdpBackend;
	struct weston_view *view = NULL;
	struct weston_output *output;
	float gx1, gy1, gx2, gy2;

	if (!surface || surface->width <= 0 || surface->height <= 0)
		return false;

	output = rdp_output_get_primary(b->compositor);
	wl_list_for_each(view, &surface->views, surface_link) {
		if (view->output) {
			output = view->output;
			break;
		}
	}
	if (!view || !output)
		return false;

	weston_view_to_global_float(view, 0.f, 0.f, &gx1, &gy1);
	weston_view_to_global_float(view, (float)surface->width,
				    (float)surface->height, &gx2, &gy2);
	bounds->x1 = (int32_t)gx1;
	bounds->y1 = (int32_t)gy1;
	bounds->x2 = (int32_t)gx2;
	bounds->y2 = (int32_t)gy2;
	to_client_coordinate(t->peer_ctx, output,
			     &bounds->x1, &bounds->y1, NULL, NULL);
	to_client_coordinate(t->peer_ctx, output,
			     &bounds->x2, &bounds->y2, NULL, NULL);
	return true;
}

/* 候选窗的摆放依据（实测结论，2026-10-06）：
 *
 *   1) Windows 输入法把候选窗摆在 **controlBounds 的"底边中点"** 上；
 *   2) 该矩形还必须足够"像个编辑控件"才会被采纳——几十到几百像素的小矩形
 *      一律被判无效，候选窗退化成屏幕右下角；整窗量级的矩形才会被采纳；
 *   3) EDIT_CONTROL_FOCUS 与 GEOMETRY_CHANGED 两个 PDU 必须给出**同一个**矩形。
 *
 * Wayland 的 text-input 协议只提供光标矩形、不提供编辑控件矩形，所以这里构造
 * "宽度取窗口宽度、水平以光标为中心、上沿取窗口上沿、底边压光标下沿"的矩形：
 * 尺寸够大（会被采纳），而它的底边中点正好落在光标上，候选窗因此出现在光标旁。
 */
static bool
rdptext_get_caret_control_bounds(struct rdptext_state *t,
				 struct weston_surface *surface,
				 pixman_box32_t *bounds)
{
	pixman_box32_t caret;
	int32_t w, top;

	if (!surface)
		return false;
	if (!rdptext_get_cursor_bounds(t, surface, &caret))
		return rdptext_get_control_bounds(t, surface, bounds);
	if (!rdptext_get_control_bounds(t, surface, bounds))
		return false;

	w = bounds->x2 - bounds->x1;	/* 窗口宽度（与已被采纳的量级一致） */
	if (w < 800)
		w = 800;
	top = bounds->y1;

	bounds->x1 = caret.x1 - w / 2;
	bounds->x2 = caret.x1 + w / 2;
	bounds->y1 = top;
	bounds->y2 = caret.y2;

	if (bounds->x2 <= bounds->x1 || bounds->y2 <= bounds->y1)
		return false;

	return true;
}

/* EDIT_CONTROL_FOCUS_PDU:
 *   textInputClientId (4), controlBounds (16 TextInputRect),
 *   editInfo (36 EditControlInfo), gainingFocus (1),
 *   losingFocusControlId (4), losingFocusTextInputHostId (4),
 *   override (1)
 */
static void
	rdptext_send_edit_control_focus(struct rdptext_state *t, bool gaining,
					struct weston_surface *surface)
{
	wStream *s;
	pixman_box32_t bounds;
	bool have_bounds = false;

	if (!rdptext_s2c_ok(t))
		return;

	s = Stream_New(NULL, 66);
	if (!s)
		return;

	if (gaining && surface)
		have_bounds = rdptext_get_caret_control_bounds(t, surface, &bounds);

	Stream_Write_UINT32(s, RDPTXT_TEXT_TARGET_ID);
	if (have_bounds) {
		Stream_Write_UINT32(s, (UINT32)bounds.x1); /* left */
		Stream_Write_UINT32(s, (UINT32)bounds.y1); /* top */
		Stream_Write_UINT32(s, (UINT32)bounds.x2); /* right */
		Stream_Write_UINT32(s, (UINT32)bounds.y2); /* bottom */
	} else {
		Stream_Write_UINT32(s, 0);
		Stream_Write_UINT32(s, 0);
		Stream_Write_UINT32(s, 0);
		Stream_Write_UINT32(s, 0);
	}

	/* EditControlInfo (36 bytes):
	 *   bufferLength (4) -1 = unlimited
	 *   editSettings (4)
	 *   frameworkType (4) Legacy —— 与 REGISTER_REMOTE_KEY_TARGET 的
	 *     TextInputHostSettings.Type=Legacy 保持一致；0 不在枚举表内，
	 *     InputService 处理焦点时可能因此拒绝关联。
	 *   frameworkVersion (4)
	 *   id (4)
	 *   inputScope (4) IS_DEFAULT
	 *   inputSettings (4)
	 *   visualReferenceId (8)
	 */
	Stream_Write_UINT32(s, 0xFFFFFFFF);
	Stream_Write_UINT32(s, 0);
	Stream_Write_UINT32(s, RDPTXT_HOST_TYPE_LEGACY);
	Stream_Write_UINT32(s, 0);
	Stream_Write_UINT32(s, RDPTXT_EDIT_CONTROL_ID);
	Stream_Write_UINT32(s, 0);
	Stream_Write_UINT32(s, 0);
	Stream_Write_UINT64(s, 0);

	Stream_Write_UINT8(s, gaining ? 1 : 0);
	Stream_Write_UINT32(s, 0); /* losingFocusControlId */
	Stream_Write_UINT32(s, 0); /* losingFocusTextInputHostId */
	Stream_Write_UINT8(s, 0);  /* override */

	rdptext_send_pdu(t, t->s2c_channel, RDPTXT_PDU_EDIT_CONTROL_FOCUS,
			 Stream_Buffer(s), 66);
	Stream_Free(s, TRUE);

	rdptext_log(t, "EDIT_CONTROL_FOCUS %s bounds:%d",
		    gaining ? "gained" : "lost", have_bounds);
}

/* TEXT_CHANGED_PDU (server -> client), MS-RDPETXT 2.2.2.36.  The client
 * keeps a local edit buffer per registered edit control and the TSF3 IMEs
 * read it; prime it with the current (empty) text and caret after focus
 * gain so the integration has a defined state to work from.  The client
 * acknowledges this with ACKNOWLEDGE_OPERATION (TextChange). */
static void
	rdptext_send_text_changed(struct rdptext_state *t,
				  const UINT16 *text16, UINT32 text16_len)
{
	wStream *s;
	const size_t keystates_size = 256;
	size_t len;
	UINT32 caret = t->client_buf_len + text16_len;

	if (!rdptext_s2c_ok(t))
		return;

	/* ids + ranges + opId + textLength + originKey + override +
	 * noConflict + region1 + region2(empty) + keyStates */
	len = 4 + 4 + 8 + 8 + 4 + 4 + 44 + 1 + 1 +
	      4 + 4 + text16_len * 2 + 4 + 4 + 4 + keystates_size;
	s = Stream_New(NULL, len);
	if (!s)
		return;

	Stream_Write_UINT32(s, RDPTXT_TEXT_TARGET_ID);
	Stream_Write_UINT32(s, RDPTXT_EDIT_CONTROL_ID);
	/* replacedTextRange: 客户端缓冲中在本会话被替换的范围——
	 * 提交文本插入在客户端缓冲末尾（无覆盖） */
	Stream_Write_UINT32(s, t->client_buf_len);
	Stream_Write_UINT32(s, t->client_buf_len);
	/* newSelectionRange: 提交/更新后的光标位置 */
	Stream_Write_UINT32(s, caret);
	Stream_Write_UINT32(s, caret);
	Stream_Write_UINT32(s, ++t->notify_op_id);
	Stream_Write_UINT32(s, text16_len); /* textLength */
	Stream_Zero(s, 44);        /* originKey: KeyEventHostInfo */
	Stream_Write_UINT8(s, 0);  /* override */
	Stream_Write_UINT8(s, 0);  /* noConflict */
	Stream_Write_UINT32(s, 0); /* offset1 */
	Stream_Write_UINT32(s, text16_len); /* updatedTextRegion1Length */
	if (text16_len > 0)
		Stream_Write(s, text16, text16_len * 2);
	Stream_Write_UINT32(s, 0xFFFFFFFF); /* offset2: -1, no 2nd region */
	Stream_Write_UINT32(s, 0); /* updatedTextRegion2Length */
	Stream_Write_UINT32(s, (UINT32)keystates_size);
	Stream_Zero(s, keystates_size);

	rdptext_send_pdu(t, t->s2c_channel, RDPTXT_PDU_TEXT_CHANGED,
			 Stream_Buffer(s), len);
	Stream_Free(s, TRUE);
	rdptext_log(t, "TEXT_CHANGED sent (opId %u, text %u, caret %u)",
		    t->notify_op_id, text16_len, caret);
}

/* GEOMETRY_CHANGED_PDU (server -> client), MS-RDPETXT 2.2.2.39:
 *   textInputClientId (4), editControlId (4), controlBounds (16),
 *   selection range (8), rangeBounds (16)
 * 供 IME 定位候选窗；LayoutChangeTracking/SelectionTracking 模式下
 * 控件移动/选区变化时也要发送。 */
static void
	rdptext_send_geometry_changed(struct rdptext_state *t,
				      struct weston_surface *surface)
{
	wStream *s;
	pixman_box32_t bounds;
	pixman_box32_t caret;
	bool have_bounds;
	bool have_caret;

	if (!rdptext_s2c_ok(t))
		return;

	s = Stream_New(NULL, 48);
	if (!s)
		return;

	have_bounds = surface ? rdptext_get_caret_control_bounds(t, surface,
								 &bounds) : false;
	have_caret = surface ? rdptext_get_cursor_bounds(t, surface, &caret)
			     : false;

	Stream_Write_UINT32(s, RDPTXT_TEXT_TARGET_ID);
	Stream_Write_UINT32(s, RDPTXT_EDIT_CONTROL_ID);
	if (have_bounds) {
		Stream_Write_UINT32(s, (UINT32)bounds.x1);
		Stream_Write_UINT32(s, (UINT32)bounds.y1);
		Stream_Write_UINT32(s, (UINT32)bounds.x2);
		Stream_Write_UINT32(s, (UINT32)bounds.y2);
	} else {
		Stream_Write_UINT32(s, 0);
		Stream_Write_UINT32(s, 0);
		Stream_Write_UINT32(s, 0);
		Stream_Write_UINT32(s, 0);
	}
	Stream_Write_UINT32(s, 0); /* selection range begin (caret) */
	Stream_Write_UINT32(s, 0); /* selection range end */
	if (have_caret) {
		Stream_Write_UINT32(s, (UINT32)caret.x1);
		Stream_Write_UINT32(s, (UINT32)caret.y1);
		Stream_Write_UINT32(s, (UINT32)caret.x2);
		Stream_Write_UINT32(s, (UINT32)caret.y2);
	} else if (have_bounds) {
		Stream_Write_UINT32(s, (UINT32)bounds.x1);
		Stream_Write_UINT32(s, (UINT32)bounds.y1);
		Stream_Write_UINT32(s, (UINT32)bounds.x2);
		Stream_Write_UINT32(s, (UINT32)bounds.y2);
	} else {
		Stream_Write_UINT32(s, 0);
		Stream_Write_UINT32(s, 0);
		Stream_Write_UINT32(s, 0);
		Stream_Write_UINT32(s, 0);
	}

	rdptext_send_pdu(t, t->s2c_channel, RDPTXT_PDU_GEOMETRY_CHANGED,
			 Stream_Buffer(s), 48);
	Stream_Free(s, TRUE);
	rdptext_log(t, "GEOMETRY_CHANGED sent bounds:%d caret:%d",
		    have_bounds, have_caret);

	/* 实测（2026-10-06）：EDIT_CONTROL_FOCUS 与 GEOMETRY_CHANGED 的
	 * controlBounds **必须一致**，否则输入法把整组判无效、候选窗退回屏幕
	 * 右下角。而焦点 PDU 是在激活时发送的，那时应用还没报光标、只能退回
	 * 整窗矩形——第一组由此不一致，输入法又恰好在第一个按键时读一次，
	 * 于是永远失败。这里在几何 PDU 发出后补发一条焦点 PDU（走同一个辅助
	 * 函数，取值必然相同），保证两者始终成对一致。 */
	if (have_bounds) {
		static int32_t last_ctrl[4];

		if (last_ctrl[0] != bounds.x1 || last_ctrl[1] != bounds.y1 ||
		    last_ctrl[2] != bounds.x2 || last_ctrl[3] != bounds.y2) {
			last_ctrl[0] = bounds.x1;
			last_ctrl[1] = bounds.y1;
			last_ctrl[2] = bounds.x2;
			last_ctrl[3] = bounds.y2;
			rdptext_send_edit_control_focus(t, true, surface);
		}
	}
}

/* UPDATE_MODE_PDU (client -> server), MS-RDPETXT 2.2.2.19:
 *   textInputHostId (4), textInputClientId (4), editControlId (4),
 *   features (4), enabled (1), customRange (8),
 *   predictionModeTriggerLength (4), triggers (variable),
 *   operationId (4)
 * PredictionMode 启用时规格要求服务端"发送上下文信息以启用候选生成"
 * —— 响应 TEXT_CHANGED（编辑缓冲内容）+ GEOMETRY_CHANGED（控件矩形，
 * 候选窗定位）。LayoutChangeTracking/SelectionTracking 启用时后续
 * 控件移动/选区变化也发 GEOMETRY_CHANGED。 */
static void
	rdptext_handle_update_mode(struct rdptext_state *t, wStream *s)
{
	UINT32 host_id, client_id, control_id, features, trigger_len, op_id;
	BYTE enabled;
	struct weston_surface *surface = t->focused_surface;

	if (Stream_GetRemainingLength(s) < 21)
		return;

	Stream_Read_UINT32(s, host_id);
	Stream_Read_UINT32(s, client_id);
	Stream_Read_UINT32(s, control_id);
	Stream_Read_UINT32(s, features);
	Stream_Read_UINT8(s, enabled);
	/* customRange (8) + predictionModeTriggerLength (4) */
	if (Stream_GetRemainingLength(s) < 12)
		return;
	Stream_Seek(s, 8);
	Stream_Read_UINT32(s, trigger_len);
	/* predictionModeTriggers (2 * trigger_len) + operationId (4) */
	if (Stream_GetRemainingLength(s) < (UINT64)trigger_len * 2 + 4)
		return;
	Stream_Seek(s, (UINT64)trigger_len * 2);
	Stream_Read_UINT32(s, op_id);

	rdptext_log(t, "UPDATE_MODE features=0x%08X enabled=%u control=%u "
		    "op=%u", features, enabled, control_id, op_id);

	if (!t->focused_surface) {
		/* 无聚焦表面则无上下文可给；记录即可 */
		return;
	}

	if ((features & RDPTXT_FEATURE_PREDICTION_MODE) && enabled) {
		/* 候选生成上下文：编辑缓冲 + 控件矩形 */
		rdptext_send_text_changed(t, NULL, 0);
		rdptext_send_geometry_changed(t, surface);
	} else if ((features & (RDPTXT_FEATURE_LAYOUT_CHANGE_TRACKING |
				RDPTXT_FEATURE_SELECTION_TRACKING)) && enabled) {
		rdptext_send_geometry_changed(t, surface);
	}
}

/* ACKNOWLEDGE_REMOTE_OPERATION_PDU (server -> client):
 *   textInputClientId (4), editControlId (4), operationId (4),
 *   errorCode (4)
 */
static void
rdptext_send_ack_remote_operation(struct rdptext_state *t,
				  UINT32 text_input_client_id,
				  UINT32 edit_control_id,
				  UINT32 operation_id, UINT32 error_code)
{
	wStream *s;

	if (!rdptext_s2c_ok(t))
		return;

	s = Stream_New(NULL, 16);
	if (!s)
		return;
	Stream_Write_UINT32(s, text_input_client_id);
	Stream_Write_UINT32(s, edit_control_id);
	Stream_Write_UINT32(s, operation_id);
	Stream_Write_UINT32(s, error_code);

	rdptext_send_pdu(t, t->s2c_channel,
			 RDPTXT_PDU_ACKNOWLEDGE_REMOTE_OPERATION,
			 Stream_Buffer(s), 16);
	Stream_Free(s, TRUE);
	rdptext_verbose(t, "acked remote operation %u (err %u)",
			operation_id, error_code);
}

/* ACKNOWLEDGE_KEY_EVENT_PDU (server -> client):
 *   textInputHostId (4), keyEventId (4), acknowledgementType (4)
 */
static void
rdptext_send_ack_key_event(struct rdptext_state *t, UINT32 text_input_host_id,
			   UINT32 key_event_id)
{
	wStream *s;

	if (!rdptext_s2c_ok(t))
		return;

	s = Stream_New(NULL, 12);
	if (!s)
		return;
	Stream_Write_UINT32(s, text_input_host_id);
	Stream_Write_UINT32(s, key_event_id);
	Stream_Write_UINT32(s, RDPTXT_KEY_ACK_COMPLETED);

	rdptext_send_pdu(t, t->s2c_channel, RDPTXT_PDU_ACKNOWLEDGE_KEY_EVENT,
			 Stream_Buffer(s), 12);
	Stream_Free(s, TRUE);
}

/* ------------------------------------------------------------------ */
/* Registration / focus notifications                                  */
/* ------------------------------------------------------------------ */

static void
rdptext_send_registrations(struct rdptext_state *t)
{
	if (!rdptext_s2c_ok(t) || t->registered)
		return;

	rdptext_send_register_text_target(t);
	rdptext_send_register_key_target(t);
	rdptext_send_register_edit_control(t);
	rdptext_send_thread_properties(t);
	rdptext_send_host_foreground(t);
	rdptext_send_host_focus(t, true);

	t->registered = true;
	rdptext_log(t, "registered text target/key target/edit control");
}

/* Re-evaluate whether an IME-enabled client is focused, and mirror the
 * transition to msrdc via EDIT_CONTROL_FOCUS. Called from the bridge state
 * listener and after the peer becomes active. */
static void
rdptext_update_edit_focus(struct rdptext_state *t)
{
	bool active = false;
	struct weston_surface *surface = NULL;

	if (t->bridge_api && t->integration_enabled &&
	    (t->peer_ctx->item.flags & RDP_PEER_ACTIVATED))
		active = t->bridge_api->get_active(t->peer_ctx->rdpBackend->compositor,
						   &surface);

	if (active && !t->registered && surface) {
		/* first activation: register with the focused surface's
		 * real RAIL window id */
		struct weston_surface_rail_state *rail_state =
			surface->backend_state;

		t->window_id = rail_state ? rail_state->window_id : 0;
		if (!t->window_id)
			t->window_id = RDPTXT_HOST_ID;
		rdptext_log(t, "first text input on surface %p, RAIL window "
			    "id 0x%X; registering", (void *)surface,
			    t->window_id);
		rdptext_send_registrations(t);
	}

	if (active == t->focus_notified &&
	    (!active || surface == t->focused_surface))
		return;

	if (t->focus_notified)
		rdptext_send_edit_control_focus(t, false, NULL);

	if (active) {
		rdptext_send_edit_control_focus(t, true, surface);
		rdptext_send_text_changed(t, NULL, 0);
		rdptext_log(t, "edit control focused: surface %p",
			    (void *)surface);
	}

	t->focus_notified = active;
	t->focused_surface = active ? surface : NULL;
}

static void
rdptext_bridge_state_cb(bool active, struct weston_surface *surface,
			void *user_data)
{
	struct rdptext_state *t = user_data;

	rdptext_verbose(t, "bridge state: active=%d surface=%p",
			active, (void *)surface);
	rdptext_update_edit_focus(t);
}

/* ------------------------------------------------------------------ */
/* Composition / commit injection                                      */
/* ------------------------------------------------------------------ */

static void
rdptext_record_commit(struct rdptext_state *t, const char *utf8)
{
	char *dup;

	dup = strdup(utf8);
	if (!dup)
		return;

	free(t->last_commit_text);
	t->last_commit_text = dup;
}

static void
rdptext_commit_text(struct rdptext_state *t, const char *utf8)
{
	struct rdp_backend *b = t->peer_ctx->rdpBackend;

	if (!utf8 || !*utf8)
		return;

	if (!t->bridge_api) {
		rdptext_log(t, "commit (no bridge, not injected): \"%s\"", utf8);
		return;
	}

	rdptext_log(t, "commit: \"%s\"", utf8);

	t->bridge_api->send_commit(b->compositor, utf8);
	rdptext_record_commit(t, utf8);
}

static void
rdptext_flush_pending_commit(struct rdptext_state *t)
{
	if (!t->pending_commit)
		return;

	rdptext_commit_text(t, t->pending_commit);
	free(t->pending_commit);
	t->pending_commit = NULL;

	/* clients that follow SET_COMPOSITION_INFO with an identical
	 * UPDATE_TEXT must not commit twice */
	t->skip_identical_update_text = true;
}

static void
rdptext_set_preedit(struct rdptext_state *t, const char *utf8)
{
	struct rdp_backend *b = t->peer_ctx->rdpBackend;

	if (!t->bridge_api)
		return;

	t->bridge_api->send_preedit(b->compositor, utf8, -1, -1);
	rdptext_verbose(t, "preedit: \"%s\"", utf8 ? utf8 : "");
}

/* ------------------------------------------------------------------ */
/* PDU parse: content updates from the client                          */
/* ------------------------------------------------------------------ */

/* UPDATE_TEXT / UPDATE_TEXT_AND_SELECTION:
 *   textInputClientId (4), editControlId (4), textInputHostId (4),
 *   operationId (4), replaceBegin (4), replaceEnd (4),
 *   newTextLength (4), newText (UTF-16)
 *   [UPDATE_TEXT_AND_SELECTION: selectionBegin (4), selectionEnd (4)]
 */
static void
rdptext_handle_update_text(struct rdptext_state *t, UINT16 pdu_id,
			   wStream *s)
{
	UINT32 client_id, control_id, host_id, operation_id;
	UINT32 replace_begin, replace_end;
	UINT32 text_len;
	char *utf8;

	(void)pdu_id;
	(void)host_id;

	if (Stream_GetRemainingLength(s) < 28)
		return;

	Stream_Read_UINT32(s, client_id);
	Stream_Read_UINT32(s, control_id);
	Stream_Read_UINT32(s, host_id);
	Stream_Read_UINT32(s, operation_id);
	Stream_Read_UINT32(s, replace_begin);
	Stream_Read_UINT32(s, replace_end);
	Stream_Read_UINT32(s, text_len);

	if (text_len > RDPTXT_MAX_TEXT_UNITS ||
	    Stream_GetRemainingLength(s) < (UINT64)text_len * 2) {
		rdptext_log(t, "UPDATE_TEXT: bad text length %u", text_len);
		return;
	}

	utf8 = rdptext_utf16_to_utf8((const UINT16 *)Stream_Pointer(s),
				     text_len);
	Stream_Seek(s, text_len * 2);
	if (!utf8)
		return;

	rdptext_verbose(t, "UPDATE_TEXT op=%u [%u,%d): \"%s\"",
			operation_id, replace_begin, (int32_t)replace_end,
			utf8);

	/* weston text-input inserts at the caret; the replacement range is
	 * not representable, so just commit the new text. */
	if (t->skip_identical_update_text && t->last_commit_text &&
	    strcmp(t->last_commit_text, utf8) == 0) {
		rdptext_verbose(t, "UPDATE_TEXT: duplicate of the determined "
				"text just committed, skipped");
	} else {
		rdptext_commit_text(t, utf8);
	}
	t->skip_identical_update_text = false;

	/* 提交已插入 gedit：同步客户端编辑缓冲副本（TEXT_CHANGED 携带
	 * 提交文本与新光标），InputService 的组合引擎依赖缓冲一致才能
	 * 启动下一次组合。 */
	{
		size_t wchars = 0;
		UINT16 *text16 = rdptext_utf8_to_utf16(utf8, &wchars);
		if (text16)
		{
			rdptext_send_text_changed(t, text16,
						  (UINT32)wchars);
			t->client_buf_len += (UINT32)wchars;
			free(text16);
		}
	}

	free(t->pending_commit);
	t->pending_commit = NULL;
	free(utf8);

	rdptext_send_ack_remote_operation(t, client_id, control_id,
					  operation_id, 0);
}

/* UPDATE_COMPOSITION:
 *   textInputClientId (4), editControlId (4), textInputHostId (4),
 *   operationId (4), compositionAction (1), clausesCount (4),
 *   clauses: { preConversionStringLen (4), preConversionString (UTF-16),
 *              range (8) } *
 */
static void
rdptext_handle_update_composition(struct rdptext_state *t, wStream *s)
{
	UINT32 client_id, control_id, host_id, operation_id;
	UINT8 composition_action;
	UINT32 clauses_count, i;
	wStream *composition;
	char *utf8;

	if (Stream_GetRemainingLength(s) < 21)
		return;

	Stream_Read_UINT32(s, client_id);
	Stream_Read_UINT32(s, control_id);
	Stream_Read_UINT32(s, host_id);
	Stream_Read_UINT32(s, operation_id);
	Stream_Read_UINT8(s, composition_action);
	Stream_Read_UINT32(s, clauses_count);

	rdptext_verbose(t, "UPDATE_COMPOSITION op=%u action=%u clauses=%u",
			operation_id, composition_action, clauses_count);

	if (clauses_count > RDPTXT_MAX_CLAUSES) {
		rdptext_log(t, "UPDATE_COMPOSITION: too many clauses (%u)",
			    clauses_count);
		return;
	}

	/* Reassemble the composition string from the clauses. The clause
	 * ranges carry formatting information which plain preedit cannot
	 * express, so the strings are simply concatenated in PDU order. */
	composition = Stream_New(NULL, 256);
	if (!composition)
		return;

	for (i = 0; i < clauses_count; i++) {
		UINT32 clause_len;

		if (Stream_GetRemainingLength(s) < 4)
			goto out;

		Stream_Read_UINT32(s, clause_len);
		if (clause_len > RDPTXT_MAX_TEXT_UNITS ||
		    Stream_GetRemainingLength(s) < (UINT64)clause_len * 2 + 8)
			goto out;

		utf8 = rdptext_utf16_to_utf8((const UINT16 *)Stream_Pointer(s),
					     clause_len);
		Stream_Seek(s, clause_len * 2);
		Stream_Seek(s, 8); /* EditControlRange */

		if (utf8 && *utf8) {
			if (!Stream_EnsureRemainingCapacity(composition,
							    strlen(utf8) + 1))
				goto out;
			Stream_Write(composition, utf8, strlen(utf8));
		}
		free(utf8);
	}

	if (!Stream_EnsureRemainingCapacity(composition, 1))
		goto out;
	Stream_Write(composition, "", 1);

	utf8 = (char *)Stream_Buffer(composition);

	switch (composition_action) {
	case RDPTXT_COMPOSITION_ENTER:
	case RDPTXT_COMPOSITION_UPDATE:
		t->composition_active = true;
		t->skip_identical_update_text = false;
		rdptext_set_preedit(t, utf8);
		break;
	case RDPTXT_COMPOSITION_LEAVE:
		t->composition_active = false;
		/* the determined text (if any) replaces the composition */
		rdptext_flush_pending_commit(t);
		rdptext_set_preedit(t, "");
		break;
	default:
		rdptext_log(t, "UPDATE_COMPOSITION: unknown action %u",
			    composition_action);
		break;
	}

out:
	Stream_Free(composition, TRUE);

	/* the server must acknowledge composition start and end */
	if (composition_action == RDPTXT_COMPOSITION_ENTER ||
	    composition_action == RDPTXT_COMPOSITION_LEAVE)
		rdptext_send_ack_remote_operation(t, client_id, control_id,
						  operation_id, 0);
}

/* SET_COMPOSITION_INFO:
 *   textInputClientId (4), editControlId (4), textInputHostId (4),
 *   operationId (4), compositionRange (8), determinedTextLength (4),
 *   determinedText (UTF-16)
 */
static void
rdptext_handle_set_composition_info(struct rdptext_state *t, wStream *s)
{
	UINT32 text_len;
	char *utf8;

	if (Stream_GetRemainingLength(s) < 28)
		return;

	Stream_Seek(s, 16); /* ids + operationId */
	Stream_Seek(s, 8);  /* compositionRange */
	Stream_Read_UINT32(s, text_len);

	if (text_len > RDPTXT_MAX_TEXT_UNITS ||
	    Stream_GetRemainingLength(s) < (UINT64)text_len * 2)
		return;

	utf8 = rdptext_utf16_to_utf8((const UINT16 *)Stream_Pointer(s),
				     text_len);
	if (!utf8)
		return;

	rdptext_verbose(t, "SET_COMPOSITION_INFO: determined \"%s\"", utf8);

	/* The determined text replaces the composition when the
	 * composition ends; stash it and let UPDATE_COMPOSITION(Leave)
	 * flush it. Some clients send it without ending the composition,
	 * in which case the text is already final - commit it right away
	 * (the UPDATE_TEXT dedup guard catches double delivery). */
	free(t->pending_commit);
	t->pending_commit = utf8;

	if (!t->composition_active)
		rdptext_flush_pending_commit(t);
}

/* KeyEventHostInfo (44 bytes):
 *   ModifierFlags (2), EventFlags (2), EventFlags2 (4), VirtualKey (2),
 *   Character (2), TranslationFlags (2), DeviceId (8), RepeatCount (2),
 *   ScanCode (2), IsExtendedKey (1), IsMenuKey (1), WasKeyDown (1),
 *   IsKeyReleased (1), TimestampInMs (4), MessageId (4),
 *   KeyEventAttributes (6)
 */
struct rdptext_key_event_info {
	UINT16 modifier_flags;
	UINT16 event_flags;
	UINT16 virtual_key;
	UINT16 character;
	UINT16 repeat_count;
	UINT16 scan_code;
	UINT8 is_extended_key;
	UINT8 was_key_down;
	UINT8 is_key_released;
};

static BOOL
rdptext_read_key_event_info(wStream *s, struct rdptext_key_event_info *info)
{
	UINT32 event_flags2;

	(void)event_flags2;

	if (Stream_GetRemainingLength(s) < 44)
		return FALSE;

	Stream_Read_UINT16(s, info->modifier_flags);
	Stream_Read_UINT16(s, info->event_flags);
	Stream_Read_UINT32(s, event_flags2);
	Stream_Read_UINT16(s, info->virtual_key);
	Stream_Read_UINT16(s, info->character);
	Stream_Seek(s, 2); /* TranslationFlags */
	Stream_Seek(s, 8); /* DeviceId */
	Stream_Read_UINT16(s, info->repeat_count);
	Stream_Read_UINT16(s, info->scan_code);
	Stream_Read_UINT8(s, info->is_extended_key);
	Stream_Seek(s, 1); /* IsMenuKey */
	Stream_Read_UINT8(s, info->was_key_down);
	Stream_Read_UINT8(s, info->is_key_released);
	Stream_Seek(s, 4); /* TimestampInMs */
	Stream_Seek(s, 4); /* MessageId */
	Stream_Seek(s, 6); /* KeyEventAttributes */

	return TRUE;
}

/* Inject a key event into weston the same way the legacy RDP keyboard
 * channel does (see xf_input_keyboard_event in rdp.c). */
static void
rdptext_inject_key(struct rdptext_state *t,
		   const struct rdptext_key_event_info *info)
{
	RdpPeerContext *peer_ctx = t->peer_ctx;
	struct weston_keyboard *keyboard;
	uint32_t vk_code, scan_code;
	enum wl_keyboard_key_state key_state;
	bool send_key = false;
	struct timespec time;

	keyboard = weston_seat_get_keyboard(peer_ctx->item.seat);
	if (!keyboard)
		return;

	vk_code = info->virtual_key;
	if (info->is_extended_key)
		vk_code |= KBDEXT;

	scan_code = GetKeycodeFromVirtualKeyCode(vk_code, KEYCODE_TYPE_EVDEV);
	if (scan_code == 0 || scan_code <= 8)
		return;

	if (info->event_flags & RDPTXT_KEY_FLAG_UP)
		key_state = WL_KEYBOARD_KEY_STATE_RELEASED;
	else
		key_state = WL_KEYBOARD_KEY_STATE_PRESSED;

	/* Ignore release if the key is not previously pressed, mirroring
	 * the stale-release guard of the legacy keyboard path. */
	if (key_state == WL_KEYBOARD_KEY_STATE_RELEASED) {
		uint32_t *k, *end;

		end = keyboard->keys.data + keyboard->keys.size;
		for (k = keyboard->keys.data; k < end; k++) {
			if (*k == (scan_code - 8)) {
				send_key = true;
				break;
			}
		}
	} else {
		send_key = true;
	}

	if (!send_key)
		return;

	weston_compositor_get_time(&time);
	notify_key(peer_ctx->item.seat, &time, scan_code - 8, key_state,
		   STATE_UPDATE_AUTOMATIC);
	rdptext_verbose(t, "key vk=0x%x ext=%d flags=0x%x -> keycode %u %s",
			info->virtual_key, info->is_extended_key,
			info->event_flags, scan_code - 8,
			key_state == WL_KEYBOARD_KEY_STATE_PRESSED ?
			"press" : "release");
}

/* KEY_EVENT:
 *   textInputHostId (4), keyEventId (4), routingStage (1),
 *   lastSeenKeyEventId (4), editControlId (4), notifyFramework (1),
 *   keyEventInfo (44), keyStatesSize (4), keyStates (var),
 *   keyTextLength (4), keyText (var), deadChar (2),
 *   keyNameTextLength (4), keyNameText (var)
 */
static void
rdptext_handle_key_event(struct rdptext_state *t, wStream *s)
{
	UINT32 host_id, key_event_id, last_seen_key_event_id, control_id;
	UINT32 key_states_size, key_text_len, key_name_text_len;
	UINT8 routing_stage, notify_framework;
	struct rdptext_key_event_info info = { 0 };

	(void)control_id;
	(void)last_seen_key_event_id;

	if (Stream_GetRemainingLength(s) < 19)
		return;

	Stream_Read_UINT32(s, host_id);
	Stream_Read_UINT32(s, key_event_id);
	Stream_Read_UINT8(s, routing_stage);
	Stream_Read_UINT32(s, last_seen_key_event_id);
	Stream_Read_UINT32(s, control_id);
	Stream_Read_UINT8(s, notify_framework);

	if (!rdptext_read_key_event_info(s, &info))
		return;

	if (Stream_GetRemainingLength(s) < 4)
		return;
	Stream_Read_UINT32(s, key_states_size);
	if (key_states_size > RDPTXT_MAX_KEYSTATS ||
	    Stream_GetRemainingLength(s) < key_states_size)
		return;
	Stream_Seek(s, key_states_size);

	if (Stream_GetRemainingLength(s) < 4)
		return;
	Stream_Read_UINT32(s, key_text_len);
	if (key_text_len > RDPTXT_MAX_TEXT_UNITS ||
	    Stream_GetRemainingLength(s) < (UINT64)key_text_len * 2)
		return;
	Stream_Seek(s, key_text_len * 2);

	if (Stream_GetRemainingLength(s) < 6)
		return;
	Stream_Seek(s, 2); /* deadChar */
	Stream_Read_UINT32(s, key_name_text_len);
	if (key_name_text_len > RDPTXT_MAX_TEXT_UNITS ||
	    Stream_GetRemainingLength(s) < (UINT64)key_name_text_len * 2)
		return;
	Stream_Seek(s, key_name_text_len * 2);

	rdptext_verbose(t, "KEY_EVENT id=%u stage=%u notify=%d vk=0x%x "
			"flags=0x%x", key_event_id, routing_stage,
			notify_framework, info.virtual_key, info.event_flags);

	/* notifyFramework tells us the key still has to reach the
	 * application; during composition the IME consumed the key and the
	 * flag is typically FALSE. */
	if (notify_framework && t->focus_notified)
		rdptext_inject_key(t, &info);

	if (routing_stage == RDPTXT_KEY_ROUTING_NONE)
		rdptext_send_ack_key_event(t, host_id, key_event_id);
}

/* CHARACTER_EVENT:
 *   textInputHostId (4), keyEventId (4), editControlId (4),
 *   keyDownEventId (4), keyEventInfo (44)
 */
static void
rdptext_handle_character_event(struct rdptext_state *t, wStream *s)
{
	struct rdptext_key_event_info info = { 0 };

	if (Stream_GetRemainingLength(s) < 16)
		return;

	Stream_Seek(s, 16);
	if (!rdptext_read_key_event_info(s, &info))
		return;

	rdptext_verbose(t, "CHARACTER_EVENT character=0x%x (ignored: keys "
			"arrive via KEY_EVENT / UPDATE_TEXT)", info.character);
}

/* ------------------------------------------------------------------ */
/* C2S pump                                                            */
/* ------------------------------------------------------------------ */

static void
rdptext_handle_client_version(struct rdptext_state *t, wStream *s)
{
	/* RDPTXT_NOTIFY_CLIENT_VERSION_PDU: versionMajor (4),
	 * versionMinor (4); note there is no containerId here. */
	if (Stream_GetRemainingLength(s) < 8)
		return;

	Stream_Read_UINT32(s, t->client_version_major);
	Stream_Read_UINT32(s, t->client_version_minor);
	t->client_version_received = true;

	rdptext_log(t, "client text-input version %u.%u (minor is a "
		    "capability bitmask)",
		    t->client_version_major, t->client_version_minor);

	/* Registration is deferred until a text input actually activates,
	 * so the key target can carry the real RAIL window instance id of
	 * the focused surface - the client only engages the IME for
	 * windows it knows from the RAIL channel. */
}

static void
rdptext_handle_report_client_options(struct rdptext_state *t, wStream *s)
{
	UINT32 options;

	if (Stream_GetRemainingLength(s) < 4)
		return;

	Stream_Read_UINT32(s, options);
	rdptext_verbose(t, "REPORT_CLIENT_OPTIONS 0x%x%s", options,
			(options & 0x1) ? " (EnablePredictedKeyReporting)" : "");
}

static void
rdptext_handle_remote_integration_status(struct rdptext_state *t, wStream *s)
{
	UINT8 enabled;

	if (Stream_GetRemainingLength(s) < 1)
		return;

	Stream_Read_UINT8(s, enabled);
	t->client_integration_status = (enabled != 0);

	/* Live msrdc sends isEnabled=false as its very first message, before
	 * even the version response; gating our sends on it would deadlock
	 * the handshake, so it is recorded but not enforced. */
	rdptext_log(t, "REMOTE_INTEGRATION_STATUS: %s",
		    t->client_integration_status ? "enabled" : "disabled");

	if (!t->client_integration_status && t->focus_notified) {
		rdptext_send_edit_control_focus(t, false, NULL);
		t->focus_notified = false;
		t->focused_surface = NULL;
	}
}

static void
rdptext_handle_enable_window(struct rdptext_state *t, wStream *s)
{
	UINT32 host_id;
	UINT8 input_enabled;

	if (Stream_GetRemainingLength(s) < 5)
		return;

	Stream_Read_UINT32(s, host_id);
	Stream_Read_UINT8(s, input_enabled);

	if (host_id != RDPTXT_HOST_ID) {
		rdptext_verbose(t, "ENABLE_WINDOW for unknown host %u",
				host_id);
		return;
	}

	t->window_input_enabled = (input_enabled != 0);
	rdptext_log(t, "ENABLE_WINDOW: input %s",
		    t->window_input_enabled ? "enabled" : "disabled");
}

static void
rdptext_handle_reregistration_request(struct rdptext_state *t)
{
	rdptext_log(t, "REREGISTRATION_REQUEST: re-registering targets");
	t->registered = false;
	if (t->focus_notified) {
		t->focus_notified = false;
		t->focused_surface = NULL;
	}
	rdptext_send_registrations(t);
	rdptext_update_edit_focus(t);
}

/* UPDATE_INPUT_PROFILE_PDU (client -> server), MS-RDPETXT 2.2.2.18:
 *   textInputClientId (4), profile (82 CoreInputProfile), initializing (1)
 * The client activates the input profile it wants on this thread.  Per
 * MS-RDPETXT 3.1.5.3 the server does its best to honor the request and
 * MUST respond with INPUT_PROFILE_CHANGED; without the confirmation the
 * client's InputService never finishes enabling remote integration.
 * We accept the requested profile verbatim (weston has no TSF state). */
static void
rdptext_handle_update_input_profile(struct rdptext_state *t, wStream *s)
{
	UINT32 client_id, initializing;
	const size_t profile_size = 82;
	BYTE profile[82];
	UINT16 langid;

	if (Stream_GetRemainingLength(s) < 4 + profile_size + 1)
		return;

	Stream_Read_UINT32(s, client_id);
	Stream_Read(s, profile, profile_size);
	Stream_Read_UINT8(s, initializing);

	langid = (UINT16)(profile[0] | ((UINT16)profile[1] << 8));
	rdptext_log(t, "UPDATE_INPUT_PROFILE client=%u langid=0x%04X "
		    "initializing=%u", client_id, langid, initializing);

	/* Confirm with the exact profile the client asked for even when
	 * initializing=TRUE: weston keeps no TSF state, so accepting the
	 * request verbatim is the closest possible "honored" reply and
	 * lets the client's InputService finish enabling integration. */
	if (rdptext_s2c_ok(t)) {
		wStream *r = Stream_New(NULL, 4 + profile_size);
		if (!r)
			return;
		Stream_Write_UINT32(r, client_id);
		Stream_Write(r, profile, profile_size);
		rdptext_send_pdu(t, t->s2c_channel,
				 RDPTXT_PDU_INPUT_PROFILE_CHANGED,
				 Stream_Buffer(r), 4 + profile_size);
		Stream_Free(r, TRUE);
		rdptext_log(t, "INPUT_PROFILE_CHANGED sent (accepted client "
			    "profile)");
	}
}

static void
rdptext_handle_acknowledge_operation(struct rdptext_state *t, wStream *s)
{
	UINT32 client_id, control_id, ack_type, operation_id;

	if (Stream_GetRemainingLength(s) < 16)
		return;

	Stream_Read_UINT32(s, client_id);
	Stream_Read_UINT32(s, control_id);
	Stream_Read_UINT32(s, ack_type);
	Stream_Read_UINT32(s, operation_id);

	rdptext_log(t, "ACKNOWLEDGE_OPERATION type=%u client=%u control=%u "
		    "op=%u", ack_type, client_id, control_id, operation_id);
}

static void
rdptext_process_pdu(struct rdptext_state *t, wStream *s)
{
	UINT32 size, start;
	UINT16 pdu_id;

	start = Stream_GetPosition(s);

	if (Stream_GetRemainingLength(s) < RDPTXT_HEADER_SIZE)
		return;

	Stream_Read_UINT32(s, size);
	Stream_Read_UINT16(s, pdu_id);

	if (size < 2 || Stream_GetRemainingLength(s) < size - 2) {
		rdptext_log(t, "truncated PDU 0x%04X (declared size %u)", pdu_id,
			    size);
		/* the stream is positioned at the message start here */
		rdptext_hex_dump(t, (const unsigned char *)Stream_Pointer(s) - 6,
				 32);
		return;
	}

	switch (pdu_id) {
	case RDPTXT_PDU_UPDATE_TEXT:
	case RDPTXT_PDU_UPDATE_TEXT_AND_SELECTION:
		rdptext_handle_update_text(t, pdu_id, s);
		break;
	case RDPTXT_PDU_UPDATE_COMPOSITION:
		rdptext_handle_update_composition(t, s);
		break;
	case RDPTXT_PDU_SET_COMPOSITION_INFO:
		rdptext_handle_set_composition_info(t, s);
		break;
	case RDPTXT_PDU_KEY_EVENT:
		rdptext_handle_key_event(t, s);
		break;
	case RDPTXT_PDU_CHARACTER_EVENT:
		rdptext_handle_character_event(t, s);
		break;
	case RDPTXT_PDU_NOTIFY_CLIENT_VERSION:
		rdptext_handle_client_version(t, s);
		break;
	case RDPTXT_PDU_REPORT_CLIENT_OPTIONS:
		rdptext_handle_report_client_options(t, s);
		break;
	case RDPTXT_PDU_REMOTE_INTEGRATION_STATUS:
		rdptext_handle_remote_integration_status(t, s);
		break;
	case RDPTXT_PDU_ENABLE_WINDOW:
		rdptext_handle_enable_window(t, s);
		break;
	case RDPTXT_PDU_UPDATE_INPUT_PROFILE:
		rdptext_handle_update_input_profile(t, s);
		break;
	case RDPTXT_PDU_UPDATE_MODE:
		rdptext_handle_update_mode(t, s);
		break;
	case RDPTXT_PDU_SET_CONVERSION_MODE:
		rdptext_log(t, "SET_CONVERSION_MODE received (ignored)");
		break;
	case RDPTXT_PDU_OCCLUDING_VIEWS:
		rdptext_log(t, "OCCLUDING_VIEWS received (ignored)");
		break;
	case RDPTXT_PDU_REREGISTRATION_REQUEST:
		rdptext_handle_reregistration_request(t);
		break;
	case RDPTXT_PDU_ACKNOWLEDGE_OPERATION:
	case RDPTXT_PDU_ACKNOWLEDGE_HOST_OPERATION:
		rdptext_handle_acknowledge_operation(t, s);
		break;
	case RDPTXT_PDU_ERROR_REPORT:
		rdptext_log(t, "client reported ERROR_REPORT: edit buffer out "
			    "of sync, client resyncs itself");
		break;
	case RDPTXT_PDU_SET_SELECTION:
	case RDPTXT_PDU_UPDATE_FORMAT:
	case RDPTXT_PDU_RECONVERSION_CANDIDATES:
		rdptext_log(t, "C2S PDU 0x%04X ignored", pdu_id);
		break;
	default:
		rdptext_log(t, "UNKNOWN C2S PDU 0x%04X (%u bytes)", pdu_id,
			    size);
		if (Stream_GetRemainingLength(s) >= 32)
			rdptext_hex_dump(t, Stream_Pointer(s), 32);
		break;
	}

	/* handlers may consume only part of the payload; skip to the end of
	 * this PDU (total on wire = 4-byte size field + size) so the pump
	 * loop resumes at the next message */
	{
		UINT32 pos = Stream_GetPosition(s);
		UINT32 end = start + 4 + size;

		if (end > pos && end - pos <= Stream_GetRemainingLength(s))
			Stream_Seek(s, end - pos);
	}
}

/* Drain whatever the client pushed into the C2S channel.
 *
 * Each WTSVirtualChannelRead() returns exactly one message (one
 * client-side WTSVirtualChannelWrite == one PDU), so PDUs are never split
 * across messages here. */
static void
rdptext_pump_c2s(struct rdptext_state *t)
{
	while (t->c2s_channel) {
		ULONG read = 0;
		wStream *s;

		/* first call with a NULL buffer: returns the pending size
		 * without consuming the message */
		if (!WTSVirtualChannelRead(t->c2s_channel, 0, NULL, 0, &read))
			break;   /* no more data (or error) */
		if (read == 0)
			break;
		if (read > RDPTXT_MAX_PDU_SIZE) {
			rdptext_log(t, "oversized C2S message (%lu bytes), "
				    "dropped", (unsigned long)read);
			/* consume in chunks to unblock the channel */
			read = RDPTXT_MAX_PDU_SIZE;
		}

		s = Stream_New(NULL, read);
		if (!s)
			return;

		if (!WTSVirtualChannelRead(t->c2s_channel, 0,
					   (PCHAR)Stream_Buffer(s), read,
					   &read) || read == 0) {
			Stream_Free(s, TRUE);
			break;
		}
		Stream_SetLength(s, read);

		/* dump the first messages verbatim: the client-side framing
		 * has not been verified against a live msrdc yet */
		if (t->c2s_dump_count < 5 || t->verbose) {
			rdptext_log(t, "C2S message #%d:", t->c2s_dump_count);
			rdptext_hex_dump(t, Stream_Buffer(s), read);
		}
		t->c2s_dump_count++;

		/* live msrdc clients prefix every message with a 4-byte
		 * outer length (bytes following the prefix); the spec PDU
		 * ([size][pduId][payload]) starts after it */
		if (read >= 8) {
			const unsigned char *b = Stream_Buffer(s);
			UINT32 outer_len = (UINT32)b[0] | ((UINT32)b[1] << 8) |
					   ((UINT32)b[2] << 16) |
					   ((UINT32)b[3] << 24);

			if (outer_len == read - 4) {
				memmove(Stream_Buffer(s), Stream_Buffer(s) + 4,
					read - 4);
				read -= 4;
				Stream_SetLength(s, read);
				Stream_SetPosition(s, 0);
			} else {
				rdptext_log(t, "C2S outer length mismatch: "
					    "%u vs message %lu; parsing from "
					    "offset 0", outer_len,
					    (unsigned long)read - 4);
			}
		}

		while (Stream_GetRemainingLength(s) >= RDPTXT_HEADER_SIZE)
			rdptext_process_pdu(t, s);

		Stream_Free(s, TRUE);
	}
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

static void
rdptext_open_channels(struct rdptext_state *t)
{
	RdpPeerContext *peer_ctx = t->peer_ctx;
	LPSTR p_session_id = NULL;
	DWORD bytes_returned = 0;

	/* libfreerdp's WTSVirtualChannelOpenEx() rejects
	 * WTS_CURRENT_SESSION and looks the VCM up in a hash table keyed
	 * by session id, so resolve the real session id first (same
	 * pattern as FreeRDP's disp/rdpsnd server modules). */
	t->session_id = WTS_CURRENT_SESSION;
	if (WTSQuerySessionInformationA(peer_ctx->vcm, WTS_CURRENT_SESSION,
					WTSSessionId, &p_session_id,
					&bytes_returned) &&
	    bytes_returned >= sizeof(DWORD)) {
		t->session_id = *((DWORD *)p_session_id);
	}
	if (p_session_id)
		WTSFreeMemory(p_session_id);

	/* Both channels are opened server-side; msrdc's remotetextplugin
	 * answers the DYNVC CREATE requests when text processing
	 * redirection is enabled on the connection. */
	t->s2c_channel = WTSVirtualChannelOpenEx(t->session_id,
						 (LPSTR)RDPTXT_CHANNEL_S2C,
						 WTS_CHANNEL_OPTION_DYNAMIC);
	t->c2s_channel = WTSVirtualChannelOpenEx(t->session_id,
						 (LPSTR)RDPTXT_CHANNEL_C2S,
						 WTS_CHANNEL_OPTION_DYNAMIC);

	rdptext_log(t, "session %lu channels open: s2c=%p c2s=%p",
		    (unsigned long)t->session_id,
		    (void *)t->s2c_channel, (void *)t->c2s_channel);
}

int
rdp_rdptext_init(freerdp_peer *client)
{
	RdpPeerContext *peer_ctx = (RdpPeerContext *)client->context;
	struct rdp_backend *b = peer_ctx->rdpBackend;
	struct rdptext_state *t;
	const char *env;

	assert_compositor_thread(b);

	if (!peer_ctx->vcm)
		return -1;

	env = getenv("WESTON_RDPETXT");
	if (env && strcmp(env, "0") == 0) {
		rdptext_log(NULL, "disabled via WESTON_RDPETXT=0");
		return -1;
	}

	t = calloc(1, sizeof(*t));
	if (!t)
		return -1;
	t->peer_ctx = peer_ctx;
	t->integration_enabled = true;
	t->window_input_enabled = true;
	t->verbose = getenv("WESTON_RDPETXT_DEBUG") != NULL;
	peer_ctx->rdptext = t;

	t->bridge_api =
		weston_plugin_api_get(b->compositor,
				      WESTON_TEXT_INPUT_BRIDGE_API_NAME,
				      sizeof(struct weston_text_input_bridge_api));
	if (t->bridge_api) {
		t->bridge_api->set_state_listener(b->compositor,
						  rdptext_bridge_state_cb, t);
		t->bridge_listener_attached = true;
	} else {
		rdptext_log(t, "text-input bridge API not available; "
			    "was the shell initialized with "
			    "text_backend_init()? committed text will be "
			    "logged but not injected");
	}

	rdptext_open_channels(t);
	return 0;
}

void
rdp_rdptext_process(RdpPeerContext *peer_ctx)
{
	struct rdptext_state *t = peer_ctx->rdptext;

	if (!t)
		return;

	/* Wait for the client to confirm both DYNVC CREATEs before sending
	 * the version PDU (writes on an unconfirmed DVC are sent straight
	 * out and would hit an unknown channelId). */
	if (!t->version_sent && !t->ready_failed && t->s2c_channel) {
		if (rdptext_channel_is_ready(t->s2c_channel) &&
		    rdptext_channel_is_ready(t->c2s_channel)) {
			t->version_sent = TRUE;
			rdptext_send_server_version(t);
		} else if (++t->ready_ticks > RDPTXT_READY_TIMEOUT_TICKS) {
			t->ready_failed = TRUE;
			rdptext_log(t, "client never confirmed the TextInput "
				    "DVCs; MS-RDPETXT unavailable (msrdc "
				    "without redirecttextprocessing:i:1?)");
		}
	}

	rdptext_pump_c2s(t);

	/* The seat/keyboard only exists after the peer activation finished;
	 * catch up on the text input state once it did. */
	if (t->version_sent && !t->bridge_listener_attached &&
	    (peer_ctx->item.flags & RDP_PEER_ACTIVATED)) {
		struct rdp_backend *b = peer_ctx->rdpBackend;

		t->bridge_api =
			weston_plugin_api_get(b->compositor,
					      WESTON_TEXT_INPUT_BRIDGE_API_NAME,
					      sizeof(struct weston_text_input_bridge_api));
		if (t->bridge_api) {
			t->bridge_api->set_state_listener(b->compositor,
							  rdptext_bridge_state_cb,
							  t);
			t->bridge_listener_attached = true;
		}
	}

	if (t->registered)
		rdptext_update_edit_focus(t);
}

void
rdp_rdptext_destroy(RdpPeerContext *peer_ctx)
{
	struct rdptext_state *t = peer_ctx->rdptext;

	if (!t)
		return;

	if (t->bridge_api) {
		if (t->focus_notified)
			rdptext_send_edit_control_focus(t, false, NULL);
		if (t->registered)
			rdptext_send_host_focus(t, false);
		if (t->bridge_listener_attached)
			t->bridge_api->set_state_listener(
				t->peer_ctx->rdpBackend->compositor, NULL, NULL);
	}

	if (t->s2c_channel)
		WTSVirtualChannelClose(t->s2c_channel);
	if (t->c2s_channel)
		WTSVirtualChannelClose(t->c2s_channel);

	free(t->pending_commit);
	free(t->last_commit_text);
	free(t);
	peer_ctx->rdptext = NULL;
}

/* ------------------------------------------------------------------ */
/* Legacy keyboard suppression                                         */
/* ------------------------------------------------------------------ */

/* When the client routes all keys through MS-RDPETXT, the legacy RDP
 * keyboard channel may deliver duplicates for IME-unconsumed keys. Test
 * runs can force suppression with WESTON_RDPETXT_SUPPRESS_LEGACY_KEYS=1. */
bool
rdp_rdptext_suppress_legacy_keys(RdpPeerContext *peer_ctx)
{
	static int lookup_done = -1;
	struct rdptext_state *t;

	if (lookup_done < 0)
		lookup_done = getenv("WESTON_RDPETXT_SUPPRESS_LEGACY_KEYS") ?
			      1 : 0;

	t = peer_ctx ? peer_ctx->rdptext : NULL;

	return lookup_done == 1 && t && t->focus_notified;
}
