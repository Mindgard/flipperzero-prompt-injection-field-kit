#include "pifk_menu.h"

#include <gui/elements.h>
#include <gui/canvas.h>
#include <input/input.h>
#include <string.h>

/* Rows are a fixed array rather than an m-array, because every caller
 * already has a hard upper bound (payload count, channel count) and a
 * fixed array cannot fail to allocate mid-build. */
#define MENU_MAX_ITEMS 64

/* Geometry.  The screen is 128x64; the header takes the top band and
 * each row below it is MENU_ROW_H tall. */
#define MENU_SCREEN_W 128
#define MENU_SCREEN_H 64
#define MENU_HEADER_H 12
#define MENU_ROW_H    12
#define MENU_VISIBLE  ((MENU_SCREEN_H - MENU_HEADER_H) / MENU_ROW_H) /* 4 */

/* Width reserved on the right for the scrollbar and the help marker, so
 * a long label is truncated rather than drawn under them. */
#define MENU_SCROLLBAR_W 4
#define MENU_MARKER_W    7

/* Icons are 9x9 in a 12px row.  The column is reserved whether or not a
 * given row has one, because labels that start at different offsets
 * depending on their neighbours are harder to scan than a little wasted
 * space on the left.
 *
 * 9 rather than the 14 px Flipper's own app-category icons use: a 14 px
 * icon forces a 14 px row, which drops the window from four rows to
 * three.  On a 45-entry payload list that is a quarter of what the
 * operator can see, which costs more than the legibility gains.  The
 * four icons with an official counterpart are redrawn at 9x9 rather than
 * downscaled — see images/menu_icons/README.md. */
#define MENU_ICON_W   9
#define MENU_ICON_GAP 3
#define MENU_TEXT_X   (2 + MENU_ICON_W + MENU_ICON_GAP)

typedef struct {
    const char* label;
    const Icon* icon;
    const char* help;
    uint32_t index;
} PifkMenuItem;

typedef struct {
    PifkMenuItem items[MENU_MAX_ITEMS];
    uint16_t count;
    uint16_t selected;
    /* First visible row.  Kept in the model rather than derived at draw
     * time so the list does not jump when the selection wraps. */
    uint16_t window_top;
    const char* header;

    PifkMenuCallback callback;
    PifkMenuLongCallback long_callback;
    PifkMenuHelpCallback help_callback;
    void* context;
} PifkMenuModel;

struct PifkMenu {
    View* view;
};

/* ── Drawing ─────────────────────────────────────────────────── */

static void pifk_menu_draw(Canvas* canvas, void* _model) {
    PifkMenuModel* m = _model;

    canvas_clear(canvas);

    /* Header: inverted band, so the group name and its precondition read
     * as context rather than as another selectable row. */
    if(m->header) {
        canvas_set_color(canvas, ColorBlack);
        canvas_draw_box(canvas, 0, 0, MENU_SCREEN_W, MENU_HEADER_H);
        canvas_set_color(canvas, ColorWhite);
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str(canvas, 2, MENU_HEADER_H - 3, m->header);
        canvas_set_color(canvas, ColorBlack);
    }

    canvas_set_font(canvas, FontSecondary);

    size_t label_w = MENU_SCREEN_W - MENU_TEXT_X - 2 - MENU_MARKER_W;
    if(m->count > MENU_VISIBLE) label_w -= MENU_SCROLLBAR_W;

    FuriString* text = furi_string_alloc();

    for(uint16_t i = 0; i < MENU_VISIBLE; i++) {
        uint16_t row = m->window_top + i;
        if(row >= m->count) break;

        int32_t y = MENU_HEADER_H + (int32_t)i * MENU_ROW_H;
        bool selected = (row == m->selected);

        if(selected) {
            canvas_set_color(canvas, ColorBlack);
            canvas_draw_box(canvas, 0, y, MENU_SCREEN_W, MENU_ROW_H);
            canvas_set_color(canvas, ColorWhite);
        }

        /* Icon first, vertically centred in the row.  canvas_draw_icon
         * paints in the current colour, so on the selected row it comes
         * out white on black along with the label. */
        if(m->items[row].icon) {
            int32_t icon_y = y + (MENU_ROW_H - MENU_ICON_W) / 2;
            canvas_draw_icon(canvas, 2, icon_y, m->items[row].icon);
        }

        /* Truncate with the SDK's own ellipsis logic so a long payload
         * name degrades the way it does everywhere else in the firmware. */
        furi_string_set(text, m->items[row].label ? m->items[row].label : "");
        elements_string_fit_width(canvas, text, label_w);
        canvas_draw_str(canvas, MENU_TEXT_X, y + MENU_ROW_H - 3, furi_string_get_cstr(text));

        /* Help marker.  Drawn per row rather than announced once in the
         * header: the affordance has to be visible on the row it applies
         * to, or the operator has to be told it exists. */
        if(m->items[row].help) {
            int32_t marker_x = MENU_SCREEN_W - MENU_MARKER_W;
            if(m->count > MENU_VISIBLE) marker_x -= MENU_SCROLLBAR_W;
            canvas_draw_str(canvas, marker_x, y + MENU_ROW_H - 3, ">?");
        }

        if(selected) canvas_set_color(canvas, ColorBlack);
    }

    furi_string_free(text);

    if(m->count > MENU_VISIBLE) {
        elements_scrollbar(canvas, m->selected, m->count);
    }
}

/* ── Input ───────────────────────────────────────────────────── */

/* Keep the selection inside the visible window, scrolling by the
 * smallest amount that does so. */
static void pifk_menu_scroll_into_view(PifkMenuModel* m) {
    if(m->selected < m->window_top) {
        m->window_top = m->selected;
    } else if(m->selected >= m->window_top + MENU_VISIBLE) {
        m->window_top = (uint16_t)(m->selected - MENU_VISIBLE + 1);
    }
}

static bool pifk_menu_input(InputEvent* event, void* context) {
    PifkMenu* menu = context;
    bool consumed = false;

    /* Callbacks must not run with the model locked: a callback switches
     * views and may re-enter this widget.  Copy what is needed, release,
     * then call. */
    PifkMenuCallback cb = NULL;
    PifkMenuLongCallback long_cb = NULL;
    PifkMenuHelpCallback help_cb = NULL;
    void* cb_context = NULL;
    uint32_t cb_index = 0;

    with_view_model(
        menu->view,
        PifkMenuModel * m,
        {
            /* An empty menu has no selection to act on.  A sentinel key
             * rather than an early break: with_view_model expands to a
             * plain block, so a break here would bind to nothing and
             * fail to compile — or worse, to an enclosing construct if
             * one were ever added. */
            InputKey key = (m->count > 0) ? event->key : InputKeyMAX;

            switch(key) {
            case InputKeyUp:
                if(event->type == InputTypeShort || event->type == InputTypeRepeat) {
                    /* Wrap, as the stock module does. */
                    m->selected = (m->selected == 0) ? (uint16_t)(m->count - 1) :
                                                       (uint16_t)(m->selected - 1);
                    pifk_menu_scroll_into_view(m);
                    consumed = true;
                }
                break;

            case InputKeyDown:
                if(event->type == InputTypeShort || event->type == InputTypeRepeat) {
                    m->selected = (uint16_t)((m->selected + 1) % m->count);
                    pifk_menu_scroll_into_view(m);
                    consumed = true;
                }
                break;

            case InputKeyRight:
                /* Only a row with help responds, so Right on a plain row
                 * is inert rather than opening an empty screen. */
                if(event->type == InputTypeShort && m->items[m->selected].help) {
                    help_cb = m->help_callback;
                    cb_context = m->context;
                    cb_index = m->items[m->selected].index;
                    consumed = true;
                }
                break;

            case InputKeyOk:
                /* Short and Long only.  InputTypePress, Release and
                 * Repeat all arrive for the same physical press, so
                 * anything less specific fires the action several
                 * times — and Repeat fires on a period while held. */
                if(event->type == InputTypeShort) {
                    cb = m->callback;
                    cb_context = m->context;
                    cb_index = m->items[m->selected].index;
                    consumed = true;
                } else if(event->type == InputTypeLong) {
                    long_cb = m->long_callback;
                    cb_context = m->context;
                    cb_index = m->items[m->selected].index;
                    consumed = true;
                }
                break;

            default:
                /* Back stays unconsumed: the ViewDispatcher's only
                 * fallback is for Back, and navigation depends on it. */
                break;
            }
        },
        consumed);

    if(help_cb) help_cb(cb_context, cb_index);
    if(cb) cb(cb_context, cb_index);
    if(long_cb) long_cb(cb_context, cb_index);

    return consumed;
}

/* ── Lifecycle ───────────────────────────────────────────────── */

PifkMenu* pifk_menu_alloc(void) {
    PifkMenu* menu = malloc(sizeof(PifkMenu));
    menu->view = view_alloc();
    view_set_context(menu->view, menu);
    view_allocate_model(menu->view, ViewModelTypeLocking, sizeof(PifkMenuModel));
    view_set_draw_callback(menu->view, pifk_menu_draw);
    view_set_input_callback(menu->view, pifk_menu_input);

    with_view_model(
        menu->view, PifkMenuModel * m, { memset(m, 0, sizeof(PifkMenuModel)); }, false);

    return menu;
}

void pifk_menu_free(PifkMenu* menu) {
    furi_assert(menu);
    view_free_model(menu->view);
    view_free(menu->view);
    free(menu);
}

View* pifk_menu_get_view(PifkMenu* menu) {
    furi_assert(menu);
    return menu->view;
}

void pifk_menu_reset(PifkMenu* menu) {
    furi_assert(menu);
    with_view_model(
        menu->view,
        PifkMenuModel * m,
        {
            /* Callbacks and context survive a reset, so a scene can
             * rebuild its rows without re-registering them. */
            PifkMenuCallback cb = m->callback;
            PifkMenuLongCallback lcb = m->long_callback;
            PifkMenuHelpCallback hcb = m->help_callback;
            void* ctx = m->context;
            memset(m, 0, sizeof(PifkMenuModel));
            m->callback = cb;
            m->long_callback = lcb;
            m->help_callback = hcb;
            m->context = ctx;
        },
        true);
}

void pifk_menu_set_header(PifkMenu* menu, const char* header) {
    furi_assert(menu);
    with_view_model(menu->view, PifkMenuModel * m, { m->header = header; }, true);
}

void pifk_menu_add_item(
    PifkMenu* menu,
    const char* label,
    const Icon* icon,
    const char* help,
    uint32_t index) {
    furi_assert(menu);
    with_view_model(
        menu->view,
        PifkMenuModel * m,
        {
            if(m->count < MENU_MAX_ITEMS) {
                m->items[m->count].label = label;
                m->items[m->count].icon = icon;
                m->items[m->count].help = help;
                m->items[m->count].index = index;
                m->count++;
            }
        },
        true);
}

void pifk_menu_set_callback(PifkMenu* menu, PifkMenuCallback cb, void* context) {
    furi_assert(menu);
    with_view_model(
        menu->view,
        PifkMenuModel * m,
        {
            m->callback = cb;
            m->context = context;
        },
        false);
}

void pifk_menu_set_long_callback(PifkMenu* menu, PifkMenuLongCallback cb) {
    furi_assert(menu);
    with_view_model(menu->view, PifkMenuModel * m, { m->long_callback = cb; }, false);
}

void pifk_menu_set_help_callback(PifkMenu* menu, PifkMenuHelpCallback cb) {
    furi_assert(menu);
    with_view_model(menu->view, PifkMenuModel * m, { m->help_callback = cb; }, false);
}

uint16_t pifk_menu_get_selected_row(PifkMenu* menu) {
    furi_assert(menu);
    uint16_t row = 0;
    with_view_model(menu->view, PifkMenuModel * m, { row = m->selected; }, false);
    return row;
}

void pifk_menu_set_selected_row(PifkMenu* menu, uint16_t row) {
    furi_assert(menu);
    with_view_model(
        menu->view,
        PifkMenuModel * m,
        {
            if(row < m->count) {
                m->selected = row;
                pifk_menu_scroll_into_view(m);
            }
        },
        true);
}

const char* pifk_menu_get_help(PifkMenu* menu, uint16_t row) {
    furi_assert(menu);
    const char* help = NULL;
    with_view_model(
        menu->view,
        PifkMenuModel * m,
        {
            if(row < m->count) help = m->items[row].help;
        },
        false);
    return help;
}

const char* pifk_menu_get_label(PifkMenu* menu, uint16_t row) {
    furi_assert(menu);
    const char* label = NULL;
    with_view_model(
        menu->view,
        PifkMenuModel * m,
        {
            if(row < m->count) label = m->items[row].label;
        },
        false);
    return label;
}
