/*
 * Copyright 2026 Weston-mirror contributors
 *
 * MS-RDPETXT: Remote Desktop Protocol: Text Input Virtual Channel Extension
 * (server side, per [MS-RDPETXT] v20241119)
 *
 * Enables the host-side Windows IME (e.g. Microsoft Pinyin running inside
 * msrdc.exe) to compose text and inject the committed string into the
 * focused Wayland surface, mirroring what WSA does for Android.
 *
 * Transport: two dynamic virtual channels opened by this server via
 * WTSVirtualChannelOpenEx(WTS_CHANNEL_OPTION_DYNAMIC):
 *   - "TextInput_ServerToClientDVC": server -> client (version notify, ...)
 *   - "TextInput_ClientToServerDVC": client -> server (key/text/composition)
 *
 * Handshake sequence (MS-RDPETXT section 3.1.4 / 3.2.4):
 *
 *   weston                                msrdc.exe
 *     |-- DYNVC CREATE "TextInput_...S2C" -->|
 *     |-- DYNVC CREATE "TextInput_...C2S" -->|
 *     |<-- CREATE_RESPONSE (join) ----------|   (dvc_open_state SUCCEEDED)
 *     |-- NOTIFY_SERVER_VERSION 0x031A ----->|
 *     |<-- NOTIFY_CLIENT_VERSION 0x0604 -----|
 *     |<-- UPDATE_TEXT 0x0200 / 0x0201 ------|   (committed strings)
 *
 * IMPORTANT: WTSVirtualChannelWrite() on a DVC does NOT buffer until the
 * client confirms the channel: libfreerdp sends the DYNVC DATA PDU
 * synchronously as soon as drdynvc is READY. Data written before the
 * CREATE_RESPONSE arrives would reach an unknown channelId, so the version
 * PDU is only sent after WTSVirtualChannelQuery(WTSVirtualChannelReady)
 * reports the channel open (same pattern as FreeRDP's audin/rdpsnd server).
 *
 * Every PDU is preceded by a 6-byte header:
 *   size  (4 bytes, UINT32 LE): payload size, excluding the size field itself
 *   pduId (2 bytes, UINT16 LE)
 */

#include <config.h>

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <freerdp/freerdp.h>
#include <freerdp/channels/wtsvc.h>

#include "rdp.h"

#define RDPTXT_CHANNEL_S2C "TextInput_ServerToClientDVC"
#define RDPTXT_CHANNEL_C2S "TextInput_ClientToServerDVC"

#define RDPTXT_HEADER_SIZE 6
#define RDPTXT_VERSION_MAJOR 1
#define RDPTXT_VERSION_MINOR 0

/* Give the client ~10 s to confirm both DYNVC CREATEs before assuming it
 * has no TextInput plugin (process() is pumped once per frame). */
#define RDPTXT_READY_TIMEOUT_TICKS 600

/* Sanity limit for UPDATE_TEXT payloads: 64k UTF-16 units is far beyond
 * anything an IME should commit in a single update. */
#define RDPTXT_MAX_TEXT_UNITS 65536

/* PDU identifiers, MS-RDPETXT section 2.2.2 */
#define RDPTXT_PDU_KEY_EVENT                       0x0100
#define RDPTXT_PDU_CHARACTER_EVENT                 0x0102
#define RDPTXT_PDU_ENABLE_WINDOW                   0x0105
#define RDPTXT_PDU_UPDATE_TEXT                     0x0200
#define RDPTXT_PDU_UPDATE_TEXT_AND_SELECTION       0x0201
#define RDPTXT_PDU_UPDATE_COMPOSITION              0x0204
#define RDPTXT_PDU_SET_COMPOSITION_INFO            0x0205
#define RDPTXT_PDU_EDIT_CONTROL_FOCUS              0x0308
#define RDPTXT_PDU_HOST_FOCUS                      0x0309
#define RDPTXT_PDU_NOTIFY_SERVER_VERSION           0x031A
#define RDPTXT_PDU_NOTIFY_CLIENT_VERSION           0x0604
#define RDPTXT_PDU_REPORT_CLIENT_OPTIONS           0x0605

struct rdptext_state {
	RdpPeerContext *peer_ctx;
	HANDLE s2c_channel;   /* server -> client */
	HANDLE c2s_channel;   /* client -> server */
	DWORD session_id;     /* real session id for WTSVirtualChannelOpenEx */
	bool version_sent;
	bool ready_failed;    /* CREATE confirmation timed out */
	uint32_t ready_ticks;
	uint32_t client_version_major;
	uint32_t client_version_minor;
};

static void
rdptext_log(struct rdptext_state *t, const char *fmt, ...)
{
	va_list ap;
	char buf[512];

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	weston_log("rdptext: %s\n", buf);
}

/* ------------------------------------------------------------------ */
/* UTF-16LE -> UTF-8 (with surrogate pairs)                            */
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

/* ------------------------------------------------------------------ */
/* Commit injection (MVP: log only; see rdptext_notify_commit)         */
/* ------------------------------------------------------------------ */

/*
 * Injection pipeline, implemented incrementally:
 *
 *   MS-RDPETXT UPDATE_TEXT  ->  UTF-8 string  ->  focused surface
 *
 *   Step 1 (this commit): log the committed string, so channel-level
 *                         behaviour can be verified end-to-end.
 *   Step 2 (planned):     port upstream weston text-input-v3 compositor
 *                         support and call commit_string() on the
 *                         focused surface's text-input object.
 *   Step 3 (planned):     route UPDATE_COMPOSITION as preedit_string().
 */
void
rdptext_notify_commit(struct rdp_backend *b, const char *utf8,
		      uint32_t replace_begin, int32_t replace_end)
{
	if (!utf8 || !*utf8)
		return;

	weston_log("rdptext: commit [%u,%d): \"%s\"\n",
		   replace_begin, replace_end, utf8);

	/* TODO(step 2): forward to weston text-input-v3 once merged. */
	(void)b;
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
/* PDU send / parse                                                    */
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
	return ret;
}

static BOOL
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
		return FALSE;

	Stream_Write(payload, zeros, sizeof(zeros));
	Stream_Write_UINT32(payload, RDPTXT_VERSION_MAJOR);
	Stream_Write_UINT32(payload, RDPTXT_VERSION_MINOR);

	ret = rdptext_send_pdu(t, t->s2c_channel,
			       RDPTXT_PDU_NOTIFY_SERVER_VERSION,
			       Stream_Buffer(payload), 24);
	Stream_Free(payload, TRUE);

	rdptext_log(t, "sent NOTIFY_SERVER_VERSION %u.%u (ret=%d)",
		    RDPTXT_VERSION_MAJOR, RDPTXT_VERSION_MINOR, ret);
	return ret;
}

static void
rdptext_handle_update_text(struct rdptext_state *t, UINT16 pdu_id,
			   wStream *s)
{
	UINT32 client_id, control_id, host_id, operation_id;
	UINT32 replace_begin, replace_end;
	UINT32 text_len;
	char *utf8;

	(void)pdu_id;

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
	    Stream_GetRemainingLength(s) < (UINT64)text_len * 2)
		return;

	/* UPDATE_TEXT_AND_SELECTION carries selectionBegin/selectionEnd
	 * after the text; we ignore it for commit purposes. */
	utf8 = rdptext_utf16_to_utf8((const UINT16 *)Stream_Pointer(s), text_len);
	if (!utf8)
		return;

	rdptext_notify_commit(t->peer_ctx->rdpBackend, utf8,
			      replace_begin, (int32_t)replace_end);
	free(utf8);
}

static void
rdptext_handle_client_version(struct rdptext_state *t, wStream *s)
{
	if (Stream_GetRemainingLength(s) < 24)
		return;

	Stream_Seek(s, 16); /* containerId */
	Stream_Read_UINT32(s, t->client_version_major);
	Stream_Read_UINT32(s, t->client_version_minor);
	rdptext_log(t, "client text-input version %u.%u",
		    t->client_version_major, t->client_version_minor);
}

static void
rdptext_process_pdu(struct rdptext_state *t, wStream *s)
{
	UINT32 size;
	UINT16 pdu_id;

	if (Stream_GetRemainingLength(s) < RDPTXT_HEADER_SIZE)
		return;

	Stream_Read_UINT32(s, size);
	Stream_Read_UINT16(s, pdu_id);

	if (size < 2 || Stream_GetRemainingLength(s) < size - 2) {
		rdptext_log(t, "truncated PDU 0x%04X", pdu_id);
		return;
	}

	switch (pdu_id) {
	case RDPTXT_PDU_UPDATE_TEXT:
	case RDPTXT_PDU_UPDATE_TEXT_AND_SELECTION:
		rdptext_handle_update_text(t, pdu_id, s);
		break;
	case RDPTXT_PDU_NOTIFY_CLIENT_VERSION:
		rdptext_handle_client_version(t, s);
		break;
	case RDPTXT_PDU_KEY_EVENT:
	case RDPTXT_PDU_CHARACTER_EVENT:
		/* Raw keys: only meaningful once key-replay is implemented;
		 * the Windows IME consumes these before composing. */
		break;
	default:
		rdptext_log(t, "PDU 0x%04X (%u bytes) ignored", pdu_id, size);
		break;
	}
}

/* Drain whatever the client pushed into the C2S channel.
 *
 * MVP simplification: each WTSVirtualChannelRead() returns exactly one
 * message (one client-side WTSVirtualChannelWrite == one PDU), so PDUs are
 * never split across messages here. */
static void
rdptext_pump_c2s(struct rdptext_state *t)
{
	unsigned char buf[8192];
	ULONG read = 0;

	while (t->c2s_channel) {
		wStream *s;

		if (!WTSVirtualChannelRead(t->c2s_channel, 0, (PCHAR)buf,
					   sizeof(buf), &read))
			break;   /* no more data (or error) */
		if (read == 0)
			break;

		/* Stream_New does not copy an external buffer; allocate our
		 * own so the stream owns its storage. */
		s = Stream_New(NULL, read);
		if (!s)
			return;
		memcpy(Stream_Buffer(s), buf, read);
		Stream_SetLength(s, read);

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

	/* Both channels are opened server-side; msrdc's TextInput plugin
	 * answers the DYNVC CREATE requests when present. */
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
	struct rdptext_state *t;

	assert_compositor_thread(peer_ctx->rdpBackend);

	if (!peer_ctx->vcm)
		return -1;

	t = calloc(1, sizeof(*t));
	if (!t)
		return -1;
	t->peer_ctx = peer_ctx;
	peer_ctx->rdptext = t;

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
				    "without TextInput plugin?)");
		}
	}

	rdptext_pump_c2s(t);
}

void
rdp_rdptext_destroy(RdpPeerContext *peer_ctx)
{
	struct rdptext_state *t = peer_ctx->rdptext;

	if (!t)
		return;

	if (t->s2c_channel)
		WTSVirtualChannelClose(t->s2c_channel);
	if (t->c2s_channel)
		WTSVirtualChannelClose(t->c2s_channel);
	free(t);
	peer_ctx->rdptext = NULL;
}
