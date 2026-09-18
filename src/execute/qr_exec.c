/*
 * QR Code display for payload delivery.
 *
 * Renders the payload text as a QR code on the Flipper's 128×64
 * screen.  The QR code is centered and scaled to fill the display
 * as much as possible.  Press Back to dismiss.
 */

#include "qr_exec.h"
#include "qrcode.h"
#include <gui/view.h>
#include <gui/canvas.h>
#include <stdlib.h>

/* ── View model ───────────────────────────────────────────────── */

typedef struct {
    QrCode qr;
} QrViewModel;

/* ── Draw callback ────────────────────────────────────────────── */

static void qr_draw_cb(Canvas* canvas, void* model) {
    QrViewModel* m = model;
    if(!m->qr.ok) {
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str_aligned(canvas, 64, 32, AlignCenter, AlignCenter, "QR: text too long");
        return;
    }

    int qr_size = (int)m->qr.size;

    /* Force 2× scale for readability on the 128×64 screen.
     * The QR is centered; modules that fall outside the display
     * are clipped by the canvas — scanners tolerate minor clipping
     * of the quiet zone and outer modules. */
    int scale = 2;

    /* Center on screen (ox/oy may be negative if QR overflows) */
    int drawn = qr_size * scale;
    int ox = (128 - drawn) / 2;
    int oy = (64 - drawn) / 2;

    /* White background — fill entire screen as quiet zone */
    canvas_set_color(canvas, ColorWhite);
    canvas_draw_box(canvas, 0, 0, 128, 64);

    /* Draw QR modules, skipping any fully off-screen */
    canvas_set_color(canvas, ColorBlack);
    for(int y = 0; y < qr_size; y++) {
        int py = oy + y * scale;
        if(py + scale <= 0 || py >= 64) continue; /* entirely off-screen */
        for(int x = 0; x < qr_size; x++) {
            if(m->qr.modules[y][x]) {
                int px = ox + x * scale;
                if(px + scale <= 0 || px >= 128) continue; /* off-screen */
                canvas_draw_box(canvas, px, py, scale, scale);
            }
        }
    }
}

/* ── Public API ───────────────────────────────────────────────── */

void pifk_qr_view_alloc(PifkApp* app) {
    app->qr_view = view_alloc();
    view_allocate_model(app->qr_view, ViewModelTypeLocking, sizeof(QrViewModel));
    view_set_draw_callback(app->qr_view, qr_draw_cb);
    view_set_context(app->qr_view, app);
    /* No input callback: Back falls through to the ViewDispatcher's
     * navigation handler, which unwinds the scene stack correctly. */
    view_dispatcher_add_view(app->view_dispatcher, PifkViewQrCode, app->qr_view);
}

void pifk_qr_view_free(PifkApp* app) {
    view_dispatcher_remove_view(app->view_dispatcher, PifkViewQrCode);
    view_free(app->qr_view);
    app->qr_view = NULL;
}

bool pifk_execute_qr(PifkApp* app, const PifkPayload* payload) {
    if(!payload || !payload->text || !payload->text[0]) return false;

    size_t len = strlen(payload->text);

    /* Encode the payload text (heap-allocated to avoid stack overflow) */
    QrCode* qr = qrcode_encode((const uint8_t*)payload->text, len);
    if(!qr) return false;

    /* Don't switch to the QR view on failure — the caller reports the
     * reason, which is more useful than a blank "too long" screen. */
    if(!qr->ok) {
        free(qr);
        return false;
    }

    /* Copy into view model, then free the encoder's allocation */
    with_view_model(app->qr_view, QrViewModel * model, { model->qr = *qr; }, true);

    free(qr);

    view_dispatcher_switch_to_view(app->view_dispatcher, PifkViewQrCode);

    return true;
}
