/*
 * Clipboard sync with the Snow Leopard guest agent.
 *
 * A QEMU clipboard peer, like ui/vdagent.c, but speaking to RV530Agent
 * (snowleopard/agent) through a chardev: device property clipboard=ID.
 * sl-vm.sh CLIPBOARD=1 makes that a unix socket which a slirp guestfwd
 * relay (scripts/dnd/relay.py) connects to the agent's TCP connection to
 * 10.0.2.100:7651.  The GTK UI with clipboard=on is the other peer, so
 * host copies reach the guest and guest copies reach the host.
 *
 * Text only.  Both directions use one message, "TEXT <bytes>\n" followed by
 * that many bytes of UTF-8; the agent greets with "HELLO 1\n".
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/cutils.h"
#include "rv530.h"
#include "trace.h"

#define CLIP_MAX (4 * 1024 * 1024)

static void clip_send_text(RV530State *s, QemuClipboardInfo *info)
{
    QemuClipboardContent *t = &info->types[QEMU_CLIPBOARD_TYPE_TEXT];
    g_autofree char *hdr = g_strdup_printf("TEXT %zu\n", (size_t)t->size);

    if (!qemu_chr_fe_backend_connected(&s->clip_chr)) {
        return;
    }
    trace_rv530_clip(">guest", t->size);
    qemu_chr_fe_write_all(&s->clip_chr, (uint8_t *)hdr, strlen(hdr));
    qemu_chr_fe_write_all(&s->clip_chr, t->data, t->size);
}

static void clip_update_info(RV530State *s, QemuClipboardInfo *info)
{
    QemuClipboardContent *t = &info->types[QEMU_CLIPBOARD_TYPE_TEXT];

    if (info->owner == &s->clip_peer || info == s->clip_sent ||
        info->selection != QEMU_CLIPBOARD_SELECTION_CLIPBOARD || !t->available) {
        return;
    }
    if (!t->data) {
        /* a grab without data (the GTK UI): fetch the text now, it comes
         * back with another update of the same info */
        if (!s->clip_pending) {
            s->clip_pending = true;
            qemu_clipboard_request(info, QEMU_CLIPBOARD_TYPE_TEXT);
        }
        return;
    }
    s->clip_pending = false;
    clip_send_text(s, info);
    /* remember it (with a reference, so the pointer stays unique) */
    if (s->clip_sent) {
        qemu_clipboard_info_unref(s->clip_sent);
    }
    s->clip_sent = qemu_clipboard_info_ref(info);
}

static void clip_notify(Notifier *n, void *data)
{
    RV530State *s = container_of(n, RV530State, clip_peer.notifier);
    QemuClipboardNotify *notify = data;

    if (notify->type == QEMU_CLIPBOARD_UPDATE_INFO) {
        clip_update_info(s, notify->info);
    }
}

static void clip_request(QemuClipboardInfo *info, QemuClipboardType type)
{
    /* guest text is stored complete when it arrives: nothing to fetch */
}

/* text copied in the guest becomes the clipboard content */
static void clip_from_guest(RV530State *s, const uint8_t *text, size_t len)
{
    QemuClipboardInfo *info =
        qemu_clipboard_info_new(&s->clip_peer, QEMU_CLIPBOARD_SELECTION_CLIPBOARD);

    trace_rv530_clip("<guest", len);
    qemu_clipboard_set_data(&s->clip_peer, info, QEMU_CLIPBOARD_TYPE_TEXT,
                            len, text, true);
    qemu_clipboard_info_unref(info);
}

static int clip_can_read(void *opaque)
{
    return 4096;
}

static void clip_read(void *opaque, const uint8_t *buf, int size)
{
    RV530State *s = opaque;
    GByteArray *b = s->clip_in;

    g_byte_array_append(b, buf, size);
    for (;;) {
        uint8_t *nl = memchr(b->data, '\n', b->len);
        size_t hlen;
        uint64_t n;

        if (!nl) {
            if (b->len > 64) {
                g_byte_array_set_size(b, 0);        /* garbage */
            }
            return;
        }
        hlen = nl - b->data + 1;
        *nl = 0;
        if (g_str_has_prefix((char *)b->data, "TEXT ") &&
            !qemu_strtou64((char *)b->data + 5, NULL, 10, &n) && n <= CLIP_MAX) {
            if (b->len < hlen + n) {
                *nl = '\n';
                return;                             /* wait for the rest */
            }
            clip_from_guest(s, b->data + hlen, n);
            g_byte_array_remove_range(b, 0, hlen + n);
        } else {
            g_byte_array_remove_range(b, 0, hlen);  /* HELLO or unknown */
        }
    }
}

static void clip_event(void *opaque, QEMUChrEvent event)
{
    RV530State *s = opaque;

    if (event == CHR_EVENT_OPENED || event == CHR_EVENT_CLOSED) {
        g_byte_array_set_size(s->clip_in, 0);
        s->clip_pending = false;
        if (s->clip_sent) {
            qemu_clipboard_info_unref(s->clip_sent);
            s->clip_sent = NULL;
        }
    }
    if (event == CHR_EVENT_OPENED) {
        /* the agent (re)connected: give it the current host clipboard */
        QemuClipboardInfo *info = qemu_clipboard_info(QEMU_CLIPBOARD_SELECTION_CLIPBOARD);
        if (info) {
            clip_update_info(s, info);
        }
    }
}

void rv530_clip_init(RV530State *s)
{
    if (!qemu_chr_fe_backend_connected(&s->clip_chr)) {
        return;
    }
    s->clip_in = g_byte_array_new();
    s->clip_peer.name = "rv530-agent";
    s->clip_peer.notifier.notify = clip_notify;
    s->clip_peer.request = clip_request;
    qemu_clipboard_peer_register(&s->clip_peer);
    qemu_chr_fe_set_handlers(&s->clip_chr, clip_can_read, clip_read, clip_event,
                             NULL, s, NULL, true);
}

void rv530_clip_fini(RV530State *s)
{
    if (s->clip_in) {
        qemu_clipboard_peer_unregister(&s->clip_peer);
        qemu_chr_fe_deinit(&s->clip_chr, false);
        if (s->clip_sent) {
            qemu_clipboard_info_unref(s->clip_sent);
            s->clip_sent = NULL;
        }
        g_byte_array_free(s->clip_in, true);
        s->clip_in = NULL;
    }
}
