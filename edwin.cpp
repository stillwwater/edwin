#include "edwin.h"

#include <assert.h>
#include <malloc.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <commctrl.h>

#define ED_WM_TABSTOPSETFOCUS (WM_APP + 1)

#define ed_abs(x) (x < 0 ? -(x) : x)
#define ed_min(a, b) ((a < b) ? (a) : (b))
#define ed_max(a, b) ((a > b) ? (a) : (b))
#define ed_clamp(x, a, b) (x < a ? a : (x > b ? b : x))

struct ed_tree_context {
    ed_node *parent;
    ed_node *child;
    ed_node *removed;
};

struct ed_color_picker {
    ed_node *node;
    ed_node *dialog;
    ed_node *slice;
    ed_node *hue;
    ed_node *rgba_slider;
    ed_node *rgb_hex;
    int rgb_packed;
    float *rgba; // User float4
    float hsv[3];
    float original_color[4];
};

struct ed_style ed_style;
struct ed_stats ed_stats;

ed_node ed_tree[ED_NODE_COUNT];
static unsigned used_node_count;
static unsigned active_node_count;
static ed_tree_context ed_ctx;
static ed_tree_context ed_saved_ctx;

ed_node_update ed_update_funcs[ED_UPDATE_FUNCS_COUNT];
static unsigned registered_update_count;

static HBRUSH brushes[ED_COLOR_COUNT];
static HFONT ui_font;

static ed_color_picker color_picker;

static ed_node *
ed_alloc_node()
{
    ed_node *node;
    if (ed_ctx.removed) {
        node = ed_ctx.removed;
        ed_ctx.removed = node->node_list;
        short id = node->id;
        memset(node, 0, sizeof(ed_node));
        node->id = id;
    } else {
        ++used_node_count;
        node = &ed_tree[used_node_count];
        node->id = (short)used_node_count;
    }

    ++active_node_count;
    return node;
}

static void
ed_free_node_resources(ed_node *node)
{
    if (node->value_ptr && (node->flags & ED_OWNDATA)) {
        switch (node->value_type) {
        case ED_DIB:
        case ED_BITMAP: DeleteObject((HBITMAP)node->value_ptr); break;
        case ED_ICON: DestroyIcon((HICON)node->value_ptr); break;
        default:
            assert(!"invalid use of ED_OWNDATA flag.");
        }
        node->value_ptr = NULL;
    }

    if (node->flags & ED_OWNUPDATE) {
        ed_unregister_update(node);
    }
}

static void
ed_destroy_node(ed_node *node)
{
    if (node->hwnd) {
        DestroyWindow(ed_hwnd(node));
        node->hwnd = NULL;
    }
}

static ed_node *
ed_attach(ed_node_type type, ed_rect rect)
{
    assert(ed_ctx.parent && "cannot add child node without a parent.");
    ed_node *node = ed_alloc_node();
    node->type = type;
    node->rect = rect;
    node->parent = ed_ctx.parent;

    if (!ed_ctx.parent->child) {
        ed_ctx.parent->child = node;
    } else if (ed_ctx.child) {
        if (ed_ctx.child->after) {
            ed_ctx.child->after->before = node;
            node->after = ed_ctx.child->after;
        }
        ed_ctx.child->after = node;
        node->before = ed_ctx.child;
    }
    ed_ctx.child = node;
    return node;
}

static ed_node *
ed_push(ed_node_type type, ed_rect rect)
{
    ed_node *node = ed_attach(type, rect);
    ed_ctx.parent = node;
    ed_ctx.child = NULL;
    return node;
}

static void
ed_pop()
{
    assert(ed_ctx.parent);
    int pop_parent = ed_ctx.parent->flags & ED_POPPARENT;

    ed_ctx.parent = ed_ctx.parent->parent;
    if (ed_ctx.parent) {
        ed_ctx.child = ed_ctx.parent->child;
    }

    while (ed_ctx.child->after) {
        ed_ctx.child = ed_ctx.child->after;
    }

    if (pop_parent) {
        ed_pop();
    }
}

// Returns the result of a linear search starting from `node`. The search
// does not follow the order of the UI layout.
//
// skip_first:
//   Skip the node passed in to `node`.
static ed_node *
ed_find_node_with_flags(ed_node *node, int mask, bool skip_first = false)
{
    size_t start = skip_first ? node->id + 1 : node->id;
    for (size_t i = start; i < ARRAYSIZE(ed_tree); ++i) {
        if (ed_tree[i].type && (ed_tree[i].flags & mask)) {
            return &ed_tree[i];
        }
    }
    return NULL;
}

// Returns the first node starting from `node` containing certain flags.
//
// skip_first:
//   Skip the node passed in to `node`.
static ed_node *
ed_find_node_with_flags_ordered(ed_node *node, int mask, bool skip_first = false)
{
    if ((node->flags & mask) && !skip_first) {
        return node;
    }

    for (ed_node *c = node->child; c; c = c->after) {
        ed_node *next = ed_find_node_with_flags_ordered(c, mask);
        if (next) return next;
    }

    if (node->after) {
        return ed_find_node_with_flags_ordered(node->after, mask);
    }

    while (node->parent && !node->parent->after) {
        node = node->parent;
    }

    if (node->parent) {
        return ed_find_node_with_flags_ordered(node->parent->after, mask);
    }

    return NULL;
}

// Returns the result of a linear search starting from `node`. The search
// does not follow the order of the UI layout. Searches in reverse order.
//
// skip_first:
//   Skip the node passed in to `node`.
static ed_node *
ed_rfind_node_with_flags(ed_node *node, int mask, bool skip_first = false)
{
    int start = skip_first ? node->id - 1 : node->id;
    for (int i = start; i >= 0; --i) {
        if (ed_tree[i].type && (ed_tree[i].flags & mask)) {
            return &ed_tree[i];
        }
    }
    return NULL;
}

// Returns the first node starting from `node` containing certain flags.
// Searches in reverse order.
//
// skip_first:
//   Skip the node passed in to `node`.
static ed_node *
ed_rfind_node_with_flags_ordered(ed_node *node, int mask, bool skip_first = false)
{
    if ((node->flags & mask) && !skip_first) {
        return node;
    }

    if (node->child) {
        ed_node *c = node->child;
        for (;; c = c->after) {
            if (!c->after) break;
        }
        for (; c; c = c->before) {
            ed_node *previous = ed_rfind_node_with_flags_ordered(c, mask);
            if (previous) return previous;
        }
    }

    if (node->before) {
        return ed_rfind_node_with_flags_ordered(node->before, mask);
    }

    while (node->parent && !node->parent->before) {
        node = node->parent;
    }

    if (node->parent) {
        return ed_rfind_node_with_flags_ordered(node->parent->before, mask);
    }

    return NULL;
}

static void
ed_measure_text_bounds(ed_node *node)
{
    int len = GetWindowTextLength(ed_hwnd(node));
    char *text = (char *)_malloca(len + 1);

    SIZE ext;
    HDC hdc = GetDC(ed_hwnd(node));
    GetWindowText(ed_hwnd(node), text, len + 1);

    if (GetTextExtentPoint32(hdc, text, len, &ext)) {
        node->bounds.w = (short)ext.cx;
        node->bounds.h = (short)ext.cy;
    }

    ReleaseDC(ed_hwnd(node), hdc);
    _freea(text);
}

// Measures the spacing required to layout a child node.
//
// layout:
//   Expected parent layout. If the layout does not match this function
//   returns the minimum spacing for the node.
//
// total_spacing:
//   Sum of parent node padding and spacing between all child nodes.
//
// remaining_spacing:
//   Sum of parent node padding and spacing of child nodes after `node`.
//   This will be the spacing to the right of the node for a horizontal layout,
//   or remaining spacing below the node for a vertical layout.
static void
ed_measure_spacing(ed_node *node, ed_node_layout layout,
        int *total_spacing, int *remaining_spacing)
{
    int child_count = 0;
    int child_index = 0;
    ed_node *p = node->parent;

    if (p->layout != layout) {
        *total_spacing = 2 * p->padding;
        *remaining_spacing = p->padding + node->spacing;
        return;
    }

    for (ed_node *c = p->child; c; c = c->after) {
        if (c == node) child_index = child_count;
        ++child_count;
    }

    *total_spacing = 2 * p->padding + (child_count - 1) * node->spacing;
    *remaining_spacing = p->padding + (child_count - child_index - 1) * node->spacing;
}

static void
ed_measure(ed_node *node)
{
    if (!ed_is_visible(node)) {
        node->dst = {};
        return;
    }

    ed_node *p = node->parent;
    ed_node *b = node->before;

    node->dst = {
        (short)node->rect.x, (short)node->rect.y,
        (short)node->rect.w, (short)node->rect.h,
    };

    // Parent padding
    if (p) {
        node->dst.x += p->padding;
        node->dst.y += p->padding;
    }

    // Stack layouts
    if (b) {
        if (p->layout == ED_HORZ) {
            node->dst.x = b->dst.x + b->dst.w + b->spacing;
        } else if (p->layout == ED_VERT) {
            node->dst.y = b->dst.y + b->dst.h + b->spacing;
        }
    }

    // w in (0, 1] size node relative to parent width.
    if (node->rect.w <= 1.0f && node->rect.w > 0.0f && p) {
        int total_spc, rem_spc;
        ed_measure_spacing(node, ED_HORZ, &total_spc, &rem_spc);
        if (node->rect.x > 1.0f) {
            // Left margin
            total_spc += node->dst.x;
        }
        node->dst.w = (short)((p->dst.w - total_spc) * node->rect.w);

        // Calculate space remaining in parent to make sure this will fit.
        short rest = (short)(p->dst.w - node->dst.x - rem_spc);
        if (rest <= node->dst.w && p->layout == ED_HORZ) {
            // Node doesn't fit, measure the remaining children and shrink
            // this node accordingly.
            for (ed_node *a = node->after; a; a = a->after) {
                ed_measure(a);
                rest -= a->dst.w;
            }
            node->dst.w = rest;
        }
    }

    // h in (0, 1] size node relative to parent height.
    if (node->rect.h <= 1.0f && node->rect.h > 0.0f && p) {
        int total_spc, rem_spc;
        ed_measure_spacing(node, ED_VERT, &total_spc, &rem_spc);
        if (node->rect.y > 1.0f) {
            // Top margin
            total_spc += node->dst.y;
        }
        node->dst.h = (short)((p->dst.h - total_spc) * node->rect.h);

        // Calculate space remaining in parent to make sure this will fit.
        short rest = (short)(p->dst.h - node->dst.y - rem_spc);
        if (rest <= node->dst.h && p->layout == ED_VERT) {
            for (ed_node *a = node->after; a; a = a->after) {
                ed_measure(a);
                rest -= a->dst.h;
            }
            node->dst.h = rest;
        }
    }

    // w=0 or h=0 text node size depends on bounds of the text.
    if (node->flags & ED_TEXTNODE) {
        short border = (node->flags & ED_BORDER) ? 1 : 0;
        if (node->rect.w == 0) {
            node->dst.w = (short)node->bounds.w + ed_style.text_w_spacing * border;
        }

        if (node->rect.h == 0) {
            node->dst.h = (short)node->bounds.h + ed_style.text_h_spacing * border;
        }
    }

    if (ed_node *c = node->child) {
        node->bounds = {};

        for (;; c = c->after) {
            ed_measure(c);
            node->bounds.w = ed_max(node->bounds.w, c->dst.x + c->dst.w + c->spacing);
            node->bounds.h = ed_max(node->bounds.h, c->dst.y + c->dst.h + c->spacing);

            if (node->flags & ED_COLLAPSED) {
                // Node is collapsed, size matches first child.
                break;
            }
            if (!c->after) {
                break;
            }
        }

        // w=0 size node based on total width of children.
        if (node->rect.w == 0.0f) {
            node->dst.w = node->bounds.w;
        }

        // h=0 size node based on total height of children.
        // Collapsing the parent is only supported for vertical layout.
        if (node->rect.h == 0.0f || (node->flags & ED_COLLAPSED)) {
            node->dst.h = node->bounds.h;
        }

        if (node->scroll_bar) {
            ed_node *scroll_bar = ed_index_node(node->scroll_bar);
            BOOL scroll_bar_visible = ed_is_visible(scroll_bar);
            if (node->bounds.h > node->dst.h) {
                if (!scroll_bar_visible) {
                    ShowWindow(ed_hwnd(scroll_bar), SW_SHOW);

                    // Re-measure parent to account for the scrollbar being visible
                    ed_measure(node->parent);
                    return;
                }
            } else if (scroll_bar_visible) {
                ShowWindow(ed_hwnd(scroll_bar), SW_HIDE);
                ed_measure(node->parent);
                return;
            }
        }
    }

    // x in (0, 1] position node relative to parent width.
    if (node->rect.x <= 1.0f && node->rect.x > 0.0f) {
        node->dst.x = (short)(p->dst.w * node->rect.x - node->dst.w * node->rect.x);
        node->dst.x -= p->padding * (short)node->rect.x;
    }

    // y in (0, 1] position node relative to parent height.
    if (node->rect.y <= 1.0f && node->rect.y > 0.0f) {
        node->dst.y = (short)(p->dst.h * node->rect.y - node->dst.h * node->rect.y);
        node->dst.y -= p->padding * (short)node->rect.y;
    }
}

static void
ed_layout(ed_node *node)
{
    // Don't modify root or user windows since we don't own them.
    if (node->id != ED_ID_ROOT && node->type != ED_USERWINDOW) {
        if (!ed_is_visible(node)) {
            return;
        }

        if (node->parent && node->parent->scroll_pos) {
            node->dst.y -= node->parent->scroll_pos;
        }

        ShowWindow(ed_hwnd(node), SW_SHOW);

        ed_dst dst = node->dst;
        if (dst.w == 0) dst.w = 20;
        if (dst.h == 0) dst.h = 20;

        if (node->type == ED_COMBOBOX) {
            // A quirk of the combobox api is the height should include the
            // height of the (invisible) dropdown list.
            dst.h += node->bounds.h;
        }

        SetWindowPos(ed_hwnd(node), NULL, dst.x, dst.y, dst.w, dst.h,
                SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOREDRAW);

        if (node->type == ED_SCROLLBAR) {
            assert(node->scroll_client
                    && "scrollbar is missing a client window.");
            ed_node *client = ed_index_node(node->scroll_client);

            // Pin client when size changes.
            if (client->scroll_pos + client->dst.h > client->bounds.h) {
                client->scroll_pos = client->bounds.h - client->dst.h;
            }

            SCROLLINFO si = {};
            si.cbSize = sizeof(si);
            si.fMask = SIF_ALL;
            si.nMax = client->bounds.h;
            si.nPage = client->dst.h;
            si.nPos = client->scroll_pos;
            si.nTrackPos = client->scroll_pos;
            SetScrollInfo(ed_hwnd(node), SB_CTL, &si, TRUE);
        }
    }

    for (ed_node *c = node->child; c; c = c->after) {
        ed_layout(c);
    }
}

static void
ed_invalidate_scalar(ed_node *node)
{
    char buf[64];
    const char *fmt = node->value_fmt;
    if (!fmt) fmt = ed_style.value_formats[node->value_type];

    switch (node->value_type) {
    case ED_INT:
        snprintf(buf, sizeof buf, fmt, ed_read_value(int, node->value_ptr));
        SetWindowText(ed_hwnd(node), buf);
        break;
    case ED_FLOAT:
        snprintf(buf, sizeof buf, fmt, ed_read_value(float, node->value_ptr));
        SetWindowText(ed_hwnd(node), buf);
        break;
    case ED_INT64:
        snprintf(buf, sizeof buf, fmt, ed_read_value(long long, node->value_ptr));
        SetWindowText(ed_hwnd(node), buf);
        break;
    case ED_FLOAT64:
        snprintf(buf, sizeof buf, fmt, ed_read_value(double, node->value_ptr));
        SetWindowText(ed_hwnd(node), buf);
        break;
    case ED_ENUM:
        SendMessage(ed_hwnd(node), CB_SETCURSEL, ed_read_value(int, node->value_ptr), 0);
        break;
    case ED_FLAGS:
        InvalidateRect(ed_hwnd(node), NULL, TRUE);
        break;
    case ED_BOOL:
        SendMessage(ed_hwnd(node), BM_SETCHECK, ed_read_value(bool, node->value_ptr), 0);
        break;
    default:
        assert(!"ed_invalidate_scalar expected a scalar value type.");
    }
}

static void
ed_data_string(ed_node *node, void *value, size_t size)
{
    if (ed_get_focus() == node || !ed_is_visible(node)) {
        node->value_size = size;
        node->value_ptr = value;
        return;
    }

    if (value == node->value_ptr && size == node->value_size) {
        int text_size = GetWindowTextLength(ed_hwnd(node)) + 1;
        char *text = (char *)_malloca(text_size);
        GetWindowText(ed_hwnd(node), text, text_size);

        size_t max_count = node->value_size ? node->value_size : text_size;
        if (!strncmp(text, (char *)value, max_count)) {
            // No change to string, avoid redraw.
            _freea(text);
            return;
        }

        _freea(text);
    }

    node->value_size = size;
    node->value_ptr = value;

    SetWindowText(ed_hwnd(node), (char *)node->value_ptr);
}

static void
ed_data_scalar(ed_node *node, void *value, size_t size)
{
    if (size == 0) {
        switch (node->value_type) {
        case ED_INT:   size = sizeof(int); break;
        case ED_FLOAT: size = sizeof(float); break;
        case ED_INT64: size = sizeof(long long); break;
        case ED_ENUM:  size = sizeof(int); break;
        case ED_FLAGS: size = sizeof(int); break;
        case ED_BOOL:  size = sizeof(bool); break;
        default: break;
        }
    }

    if (value == node->value_ptr
            && size == node->value_size
            && !memcmp(node->value, value, node->value_size)) {
        // No change to value.
        return;
    }

    node->value_size = size;
    node->value_ptr = value;

    if (ed_get_focus() == node || !ed_is_visible(node)) {
        return;
    }

    memcpy(&node->value, value, node->value_size);
    ed_invalidate_scalar(node);
}

static void
ed_data_image(ed_node *node, void *value)
{
    if (ed_is_visible(node)) {
        ed_image_buffer_copy(node, (const unsigned char *)value);
        InvalidateRect(ed_hwnd(node), NULL, TRUE);
    }
}

static void
ed_data_color(ed_node *node, void *value, size_t size)
{
    if (size == 0) {
        size = 4 * sizeof(float);
    }

    if (value == node->value_ptr
            && size == node->value_size
            && !memcmp(node->value, value, node->value_size)) {
        // No change to value.
        return;
    }

    node->value_size = size;
    node->value_ptr = value;

    if (ed_get_focus() != node && ed_is_visible(node)) {
        memcpy(&node->value, value, node->value_size);
        InvalidateRect(ed_hwnd(node), NULL, TRUE);
    }
}

static void
ed_set_scroll_position(ed_node *client, int y)
{
    ed_node *scrollbar = ed_index_node(client->scroll_bar);
    if (ed_is_visible(scrollbar)) {
        SetScrollPos(ed_hwnd(scrollbar), SB_CTL, y, TRUE);

        // Get scroll position again, SetScrollPos might have clamped it.
        y = GetScrollPos(ed_hwnd(scrollbar), SB_CTL);
        ScrollWindow(ed_hwnd(client), 0, client->scroll_pos - y, NULL, NULL);
        client->scroll_pos = (short)y;
    }
}

static ed_node *
ed_next_tabstop(ed_node *node)
{
    bool wrapped = false;
    do {
        node = ed_find_node_with_flags_ordered(node, ED_TABSTOP, true);
        if (!node && !wrapped) {
            // Cycle to first tabstop.
            node = ed_find_node_with_flags(ed_index_node(ED_ID_ROOT), ED_TABSTOP);
            wrapped = true;
        }
    } while (node && (!ed_is_visible(node) || !ed_is_enabled(node)));

    return node;
}

static ed_node *
ed_previous_tabstop(ed_node *node)
{
    bool wrapped = false;
    do {
        node = ed_rfind_node_with_flags_ordered(node, ED_TABSTOP, true);
        if (!node && !wrapped) {
            // Cycle to last tabstop.
            node = ed_rfind_node_with_flags(
                    ed_index_node((short)used_node_count), ED_TABSTOP);
            wrapped = true;
        }
    } while (node && (!ed_is_visible(node) || !ed_is_enabled(node)));

    return node;
}

static bool
ed_is_point_over(ed_node *node, POINT p)
{
    if (ed_is_visible(node)) {
        RECT rect;
        GetWindowRect(ed_hwnd(node), &rect);
        if (p.x >= rect.left && p.x <= rect.right
                && p.y >= rect.top && p.y <= rect.bottom) {
            return true;
        }
    }

    return false;
}

static const char *
ed_file_extension(const char *filename)
{
    const char *dot = strrchr(filename, '.');
    if (!dot || dot == filename) {
        return "";
    }
    return dot + 1;
}

static HICON
ed_load_icon(const char *filename, int *w, int *h)
{
    HICON hicon = (HICON)LoadImage(NULL, filename, IMAGE_ICON, *w, *h, LR_LOADFROMFILE);
    assert(hicon && "failed to load icon.");

    if (*w == 0 || *h == 0) {
        ICONINFO info;
        if (GetIconInfo(hicon, &info)) {
            BITMAP bm;
            if (GetObject(info.hbmMask, sizeof bm, &bm) == sizeof(bm)) {
                *w = bm.bmWidth;
                *h = bm.bmHeight;

                // Monochrome bitmaps combine mask and image bitmaps into the
                // same double height bitmap.
                if (!info.hbmColor) *h /= 2;
            }

            if (info.hbmMask) DeleteObject(info.hbmMask);
            if (info.hbmColor) DeleteObject(info.hbmColor);
        }
    }

    assert((*w != 0) && (*h != 0));
    return hicon;
}

static HBITMAP
ed_load_bitmap(const char *filename, int *w, int *h)
{
    HBITMAP hbm = (HBITMAP)LoadImage(NULL, filename, IMAGE_BITMAP, *w, *h,
            LR_LOADFROMFILE);
    assert(hbm && "failed to load bitmap.");

    if (*w == 0 || *h == 0) {
        BITMAP bm;
        if (GetObject(hbm, sizeof bm, &bm) == sizeof(bm)) {
            *w = bm.bmWidth;
            *h = bm.bmHeight;
        }
    }

    assert((*w != 0) && (*h != 0));
    return hbm;
}

static void
ed_load_image(ed_node *node, const char *filename)
{
    int w = (int)node->rect.w;
    int h = (int)node->rect.h;
    const char *ext = ed_file_extension(filename);

    if (!strcmp(ext, "ico")) {
        node->value_ptr = ed_load_icon(filename, &w, &h);
        node->value_type = ED_ICON;
    } else {
        // Assume bitmap file
        node->value_ptr = ed_load_bitmap(filename, &w, &h);
        node->value_type = ED_BITMAP;
    }

    node->rect.w = (float)w;
    node->rect.h = (float)h;
}

static void
ed_alloc_bitmap_buffer(ed_node *node,
        const unsigned char *image, int w, int h, ed_pixel_format fmt)
{
    ed_bitmap_buffer buffer;
    buffer.fmt = fmt;
    buffer.w = (short)ed_abs(w);
    buffer.h = (short)ed_abs(h);
    ed_write_value(ed_bitmap_buffer, node->value, &buffer);

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize        = sizeof(bmi.bmiHeader);
    bmi.bmiHeader.biWidth       = w;
    bmi.bmiHeader.biHeight      = h;
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = ED_BITMAP_BITSPERPIXEL;
    bmi.bmiHeader.biCompression = BI_RGB;
    bmi.bmiHeader.biSizeImage   = buffer.w * buffer.h * ED_BITMAP_BYTESPERPIXEL;

    HDC hdc = GetDC(ed_hwnd(node));
    node->value_ptr = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS,
            (void **)&node->value_dib_image, NULL, 0);

    if (image) {
        ed_image_buffer_copy(node, image);
    } else {
        memset(node->value_dib_image, 0, bmi.bmiHeader.biSizeImage);
    }

    ReleaseDC(ed_hwnd(node), hdc);
}

static void
ed_hsv_from_rgb(float *hsv, const float *rgb)
{
    float h;
    float x_min = fminf(rgb[0], fminf(rgb[1], rgb[2]));
    float x_max = fmaxf(rgb[0], fmaxf(rgb[1], rgb[2]));
    float chroma = x_max - x_min;
    float v = x_max;

    if (chroma == 0 || v <= 0) {
        hsv[0] = 0, hsv[1] = 0, hsv[2] = v;
        return;
    }

    if (rgb[0] >= v)
        h = 0.0f + (rgb[1] - rgb[2]) / chroma;
    else if (rgb[1] >= v)
        h = 2.0f + (rgb[2] - rgb[0]) / chroma;
    else
        h = 4.0f + (rgb[0] - rgb[1]) / chroma;

    hsv[0] = h / 6.0f;
    hsv[1] = chroma / v;
    hsv[2] = v;

    if (hsv[0] < 0) {
        hsv[0] += 1.0f;
    }
}

static void
ed_rgb_from_hsv(float *rgb, const float *hsv)
{
    float h1 = hsv[0] * 6.0f;
    float s = hsv[1];
    float v = hsv[2];
    float t = h1 - floorf(h1);

    float x = v * (1 - s);
    float y = v * (1 - s * t);
    float z = v * (1 - s * (1 - t));

    switch ((int)h1) {
    case 6:
    case 0:  rgb[0] = v, rgb[1] = z, rgb[2] = x; break;
    case 1:  rgb[0] = y, rgb[1] = v, rgb[2] = x; break;
    case 2:  rgb[0] = x, rgb[1] = v, rgb[2] = z; break;
    case 3:  rgb[0] = x, rgb[1] = y, rgb[2] = v; break;
    case 4:  rgb[0] = z, rgb[1] = x, rgb[2] = v; break;
    default: rgb[0] = v, rgb[1] = x, rgb[2] = y; break;
    }
}

static unsigned int
ed_pack_rgb(float *rgb)
{
    unsigned char r = (unsigned char)(rgb[0] * 255);
    unsigned char g = (unsigned char)(rgb[1] * 255);
    unsigned char b = (unsigned char)(rgb[2] * 255);
    return (r << 16) | (g << 8) | b;
}

static void
ed_update_color_picker_slice()
{
    float rgb[3];
    float hsv[3];

    hsv[0] = color_picker.hsv[0];
    ed_bitmap_buffer buffer = ed_read_value(ed_bitmap_buffer, color_picker.slice->value);
    unsigned int *dst = (unsigned int *)color_picker.slice->value_dib_image;

    float one_over_w = 1.0f / buffer.w;
    float one_over_h = 1.0f / buffer.h;

    for (int y = 0; y < buffer.h; ++y) {
        for (int x = 0; x < buffer.w; ++x) {
            hsv[1] = (float)x * one_over_w;
            hsv[2] = (float)y * one_over_h;
            ed_rgb_from_hsv(rgb, hsv);
            dst[x + y * buffer.w] = ed_pack_rgb(rgb);
        }
    }
}

static void
ed_init_color_picker_hue()
{
    float rgb[3];
    float hsv[3];

    hsv[1] = 1.0f;
    hsv[2] = 1.0f;
    ed_bitmap_buffer buffer = ed_read_value(ed_bitmap_buffer, color_picker.hue->value);
    unsigned int *dst = (unsigned int *)color_picker.hue->value_dib_image;

    assert(ed_min(buffer.w, buffer.h) == 1);
    short size = ed_max(buffer.w, buffer.h);
    float one_over_size = 1.0f / size;

    for (int i = 0; i < size; ++i) {
        hsv[0] = (float)i * one_over_size;
        ed_rgb_from_hsv(rgb, hsv);
        dst[i] = ed_pack_rgb(rgb);
    }
}

static void
ed_color_picker_rgba_on_change(ed_node *)
{
    ed_hsv_from_rgb(color_picker.hsv, color_picker.rgba);
    ed_update_color_picker_slice();
    InvalidateRect(ed_hwnd(color_picker.hue), NULL, FALSE);
    InvalidateRect(ed_hwnd(color_picker.slice), NULL, FALSE);
    InvalidateRect(ed_hwnd(color_picker.node), NULL, FALSE);
    color_picker.rgb_packed = ed_pack_rgb(color_picker.rgba);
    ed_data(color_picker.rgb_hex, &color_picker.rgb_packed);
}

static void
ed_color_picker_hex_on_change(ed_node *node)
{
    if (!node->value_ptr || !color_picker.rgba) return;
    int change = ed_read_value(int, node->value);

    if (change < 0 || change > 0xFFFFFF) {
        ed_write_value(int, node->value, node->value_ptr);
        return;
    }

    constexpr float one_over_255 = 1.0f / 255.0f;
    color_picker.rgba[0] = ((change >> 16) & 0xFF) * one_over_255;
    color_picker.rgba[1] = ((change >> 8)  & 0xFF) * one_over_255;
    color_picker.rgba[2] = ((change >> 0)  & 0xFF) * one_over_255;
    ed_data(color_picker.rgba_slider, color_picker.rgba);

    ed_hsv_from_rgb(color_picker.hsv, color_picker.rgba);
    ed_update_color_picker_slice();
    InvalidateRect(ed_hwnd(color_picker.hue), NULL, FALSE);
    InvalidateRect(ed_hwnd(color_picker.slice), NULL, FALSE);
    InvalidateRect(ed_hwnd(color_picker.node), NULL, FALSE);
}

static void
ed_color_picker_revert(ed_node *)
{
    memcpy(color_picker.rgba, color_picker.original_color, 4 * sizeof(float));
    color_picker.rgb_packed = ed_pack_rgb(color_picker.rgba);
    ed_hsv_from_rgb(color_picker.hsv, color_picker.rgba);
    ed_update_color_picker_slice();

    ed_data(color_picker.rgb_hex, &color_picker.rgb_packed);
    ed_data(color_picker.rgba_slider, color_picker.rgba);
    InvalidateRect(ed_hwnd(color_picker.hue), NULL, FALSE);
    InvalidateRect(ed_hwnd(color_picker.slice), NULL, FALSE);
    InvalidateRect(ed_hwnd(color_picker.node), NULL, FALSE);
}

static void
ed_open_color_picker(ed_node *node)
{
    auto color_rgba = [] (const char *label) {
        ed_node *input;
        ed_begin({0, 0, 1, 0}, ED_HORZ)->spacing = ed_style.spacing / 2;
        {
            ed_label(label, {0, 0, 16, 24})->spacing = 0;
            input = ed_float(NULL, 0, 1, NULL, {0, 0, 1, 24});
            input->spacing = 0;
            input->onchange = ed_color_picker_rgba_on_change;
        }
        ed_end();

        return input;
    };

    RECT rect = {0, 0, 280, 406};
    AdjustWindowRect(&rect, WS_CAPTION | WS_SYSMENU, FALSE);

    RECT node_rect;
    GetWindowRect(ed_hwnd(node), &node_rect);

    if (!color_picker.dialog || color_picker.dialog->type == ED_NONE) {
        HWND hwnd = CreateWindow("ED_USERWINDOW", "Color Picker",
                WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
                node_rect.right - rect.right, node_rect.top + node->dst.h,
                rect.right - rect.left, rect.bottom - rect.top,
                NULL, NULL, GetModuleHandle(NULL), NULL);

        ed_begin_context(ed_index_node(ED_ID_ROOT));
        color_picker.dialog = ed_begin_hwnd(hwnd, ED_VERT);
        {
            ed_begin({0, 0, 280, 256}, ED_HORZ);
            {
                color_picker.hue = ed_attach(ED_IMAGE, {0, 0, 24, 256});
                ed_attach_hwnd(color_picker.hue, "ED_COLOR_HUE",
                        NULL, WS_CHILD | WS_VISIBLE);
                color_picker.hue->value_type = ED_DIB;
                color_picker.hue->flags = ED_OWNDATA;
                ed_alloc_bitmap_buffer(color_picker.hue, NULL, 1, 256, ED_BGRA);
                ed_init_color_picker_hue();

                color_picker.slice = ed_attach(ED_IMAGE, {0, 0, 256, 256});
                ed_attach_hwnd(color_picker.slice, "ED_COLOR_SLICE",
                        NULL, WS_CHILD | WS_VISIBLE);
                color_picker.slice->value_type = ED_DIB;
                color_picker.slice->flags = ED_OWNDATA;
                ed_alloc_bitmap_buffer(color_picker.slice, NULL, 256, 256, ED_BGRA);
            }
            ed_end();

            ed_begin({0, 0, 280, 300}, ED_VERT)->padding = ed_style.padding;
            {
                ed_node *r_node = color_rgba("R");
                ed_node *g_node = color_rgba("G");
                ed_node *b_node = color_rgba("B");
                ed_node *a_node = color_rgba("A");

                r_node->node_list = g_node;
                g_node->node_list = b_node;
                b_node->node_list = a_node;
                color_picker.rgba_slider = r_node;

                ed_begin({1, 0, 160, 0}, ED_HORZ)->spacing = ed_style.spacing / 2;
                {
                    ed_node *revert = ed_button("Revert", NULL, {0, 0, 0, 24});
                    revert->onclick = ed_color_picker_revert;

                    ed_label("#", {0, 0, 12, 24})->spacing = 0;
                    color_picker.rgb_hex = ed_int(NULL, 0, 0, "%06X", 16, {0, 0, 1, 24});
                    color_picker.rgb_hex->spacing = 0;
                    color_picker.rgb_hex->onchange = ed_color_picker_hex_on_change;
                }
                ed_end();
            }
            ed_end();
        }
        ed_end();
        ed_end_context();
    } else {
        SetWindowPos(ed_hwnd(color_picker.dialog), NULL,
                node_rect.right - rect.right,
                node_rect.top + node->dst.h, 0, 0,
                SWP_NOZORDER | SWP_NOOWNERZORDER
                | SWP_NOREDRAW | SWP_NOSIZE);

        ShowWindow(ed_hwnd(color_picker.dialog), SW_SHOW);
    }

    assert(node->value_ptr);
    color_picker.node = node;
    color_picker.rgba = (float *)node->value_ptr;
    color_picker.rgb_packed = ed_pack_rgb(color_picker.rgba);
    memcpy(color_picker.original_color, color_picker.rgba, 4 * sizeof(float));

    ed_hsv_from_rgb(color_picker.hsv, color_picker.rgba);
    ed_update_color_picker_slice();
    ed_data(color_picker.rgba_slider, color_picker.rgba);
    ed_data(color_picker.rgb_hex, &color_picker.rgb_packed);
    ed_invalidate(color_picker.dialog);
}

static LRESULT __stdcall
ed_window_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    ed_node *node = (ed_node *)GetWindowLongPtr(hwnd, GWLP_USERDATA);

    switch (msg) {
    case WM_DRAWITEM: {
        DRAWITEMSTRUCT *dis = (DRAWITEMSTRUCT *)lparam;
        if (dis->CtlType == ODT_COMBOBOX) {
            assert(wparam);
            ed_node *item = ed_index_node((short)wparam);
            assert(item->type == ED_COMBOBOX);

            SelectObject(dis->hDC, ui_font);
            SetTextColor(dis->hDC, ed_style.colors[ED_COLOR_WINDOWTEXT]);
            SetBkColor(dis->hDC, ed_style.colors[ED_COLOR_WINDOW]);

            if (item->value_type == ED_FLAGS) {
                unsigned long long value = *(unsigned long long *)&item->value;
                if (dis->itemState & ODS_COMBOBOXEDIT) {
                    // This is the value shown above the dropdown.
                    char buf[19] = {0};
                    snprintf(buf, sizeof buf, "0x%llX", value);

                    DrawText(dis->hDC, buf, (int)(sizeof(buf) - 1), &dis->rcItem,
                            DT_SINGLELINE | DT_VCENTER);
                    return 1;
                }

                if (dis->itemID == (unsigned)-1) break;

                assert(dis->itemID < 64);
                unsigned long long mask = 1ULL << dis->itemID;
                const char *text = (const char *)SendMessage(ed_hwnd(item),
                        CB_GETITEMDATA, dis->itemID, 0);
                size_t len = strlen(text);

                RECT cb_rect = dis->rcItem;
                cb_rect.right = dis->rcItem.left + 20;
                dis->rcItem.left += 25;

                if (value & mask) {
                    DrawFrameControl(dis->hDC, &cb_rect, DFC_BUTTON, DFCS_CHECKED);
                } else {
                    DrawFrameControl(dis->hDC, &cb_rect, DFC_BUTTON, DFCS_BUTTONCHECK);
                }

                DrawText(dis->hDC, text, (int)len, &dis->rcItem,
                        DT_SINGLELINE | DT_VCENTER);
            }
        }
        break;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rect;
        GetClientRect(hwnd, &rect);
        FillRect(hdc, &rect, brushes[ED_COLOR_3DFACE]);
        if (node && (node->flags & ED_BORDER)) {
            FrameRect(hdc, &rect, brushes[ED_COLOR_GRAYTEXT]);
        }
        EndPaint(hwnd, &ps);
        return 1;
    }
    case WM_MOUSEWHEEL: {
        if (node && node->type == ED_SCROLLBLOCK) {
            ed_node *client = node->child;
            int scroll_lines;
            SystemParametersInfo(SPI_GETWHEELSCROLLLINES, 0, &scroll_lines, 0);

            int delta = (GET_WHEEL_DELTA_WPARAM(wparam) / WHEEL_DELTA) * scroll_lines;
            ed_set_scroll_position(client,
                    client->scroll_pos - delta * ed_style.scroll_sensitivity);
            return 1;
        }
        break;
    }
    case WM_VSCROLL: {
        if (node && node->type == ED_SCROLLBLOCK) {
            ed_node *client = node->child;
            short scroll_pos = client->scroll_pos;

            switch (LOWORD(wparam)) {
            case SB_LINEUP:        scroll_pos -= ed_style.scroll_unit; break;
            case SB_LINEDOWN:      scroll_pos += ed_style.scroll_unit; break;
            case SB_THUMBPOSITION: scroll_pos = HIWORD(wparam); break;
            case SB_THUMBTRACK:    scroll_pos = HIWORD(wparam); break;
            }
            ed_set_scroll_position(client, scroll_pos);
            return 1;
        }
        break;
    }
    case WM_COMMAND: {
        ed_node *c = (ed_node *)GetWindowLongPtr((HWND)lparam, GWLP_USERDATA);

        switch (HIWORD(wparam)) {
        case CBN_SELCHANGE: {
            int selected = (int)SendMessage(ed_hwnd(c), CB_GETCURSEL, 0, 0);
            if (c->value_ptr && selected != -1) {
                if (c->value_type == ED_ENUM) {
                    ed_write_value(int, c->value, &selected);
                } else if (c->value_type == ED_FLAGS) {
                    assert(selected < 64);
                    unsigned long long value = ed_read_value(unsigned long long,
                            &c->value);
                    value ^= 1ULL << selected;
                    ed_write_value(unsigned long long, c->value, &value);
                    InvalidateRect(ed_hwnd(c), NULL, FALSE);
                }

                if (c->onchange) c->onchange(c);
                memcpy(c->value_ptr, &c->value, c->value_size);
            }
            break;
        }
        case BN_CLICKED: {
            if (c->type == ED_CHECKBOX && c->value_ptr) {
                int state = (int)SendMessage(ed_hwnd(c), BM_GETCHECK, 0, 0);

                // Call ed_data to initialize the node.
                assert(state != BST_INDETERMINATE);

                bool current_value = !(state == BST_CHECKED);
                ed_write_value(bool, c->value, &current_value);

                if (c->onchange) c->onchange(c);
                memcpy(c->value_ptr, c->value, c->value_size);
                SendMessage(ed_hwnd(c), BM_SETCHECK, current_value, 0);
            } else if (c->type == ED_BUTTON) {
                if (c->onclick) c->onclick(c);
            }
            break;
        }
        }
    }
    }

    return DefWindowProc(hwnd, msg, wparam, lparam);
}

static LRESULT __stdcall
ed_user_window_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    ed_node *node = (ed_node *)GetWindowLongPtr(hwnd, GWLP_USERDATA);

    switch (msg) {
    case WM_CLOSE:
        if (node) {
            ed_remove(node);
            return 1;
        }
        break;

    case WM_SIZE:
        if (node) {
            ed_invalidate(node);
        }
        break;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        FillRect(hdc, &ps.rcPaint, brushes[ED_COLOR_WINDOW]);
        EndPaint(hwnd, &ps);
        break;
    }
    }

    return DefWindowProc(hwnd, msg, wparam, lparam);
}

static LRESULT __stdcall
ed_caption_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    ed_node *node = (ed_node *)GetWindowLongPtr(hwnd, GWLP_USERDATA);
    ed_node *p = node ? node->parent : NULL;

    switch (msg) {
    case WM_SETFOCUS:
        InvalidateRect(hwnd, NULL, TRUE);
        break;
    case WM_KILLFOCUS:
        InvalidateRect(hwnd, NULL, TRUE);
        break;
    case WM_PAINT: {
        char name[256];
        int len_name = GetWindowText(hwnd, name, sizeof(name));

        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        SetBkMode(hdc, TRANSPARENT);
        RECT rect;
        GetClientRect(hwnd, &rect);
        FillRect(hdc, &rect, brushes[ED_COLOR_HIGHLIGHT]);

        if (GetFocus() == hwnd) {
            FrameRect(hdc, &rect, (HBRUSH)(COLOR_WINDOWFRAME + 1));
        }
        rect.left += ed_style.spacing;

        SelectObject(hdc, ui_font);
        SetTextColor(hdc, ed_style.colors[ED_COLOR_HIGHLIGHTTEXT]);
        if (p && (p->flags & ED_EXPAND)) {
            if (p->flags & ED_COLLAPSED) {
                DrawText(hdc, "+", 1, &rect, DT_SINGLELINE | DT_VCENTER);
            } else {
                DrawText(hdc, "-", 1, &rect, DT_SINGLELINE | DT_VCENTER);
            }
            rect.left += ed_style.spacing + ed_style.spacing / 2;
        }
        DrawText(hdc, name, len_name, &rect, DT_SINGLELINE | DT_VCENTER);
        EndPaint(hwnd, &ps);
        return 1;
    }
    case WM_KEYDOWN:
        if (!((wparam == VK_RETURN || wparam == VK_SPACE) && GetFocus() == hwnd)) {
            break;
        }
        [[fallthrough]];
    case WM_LBUTTONDOWN: {
        if (p && (p->flags & ED_EXPAND)) {
            p->flags ^= ED_COLLAPSED;

            // Node size changed, parent may need to reflow
            ed_invalidate(p->parent);
        }
        break;
    }
    }

    return DefWindowProc(hwnd, msg, wparam, lparam);
}

static LRESULT __stdcall
ed_image_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    ed_node *node = (ed_node *)GetWindowLongPtr(hwnd, GWLP_USERDATA);

    switch (msg) {
    case WM_NCHITTEST:
        return HTTRANSPARENT;
    case WM_PAINT:
        if (node && node->value_ptr) {
            PAINTSTRUCT ps;
            HDC hdc_dst = BeginPaint(hwnd, &ps);
            RECT rect;
            GetClientRect(ed_hwnd(node), &rect);

            HDC hdc = CreateCompatibleDC(hdc_dst);
            SelectObject(hdc, (HBITMAP)node->value_ptr);
            ed_bitmap_buffer buffer = ed_read_value(ed_bitmap_buffer, node->value);

            BLENDFUNCTION blendfn;
            blendfn.BlendOp = AC_SRC_OVER;
            blendfn.BlendFlags = 0;
            blendfn.SourceConstantAlpha = 255;
            blendfn.AlphaFormat = AC_SRC_ALPHA;

            int dst_w = rect.right - rect.left;
            int dst_h = rect.bottom - rect.top;

            BOOL blend_status = AlphaBlend(hdc_dst, 0, 0, dst_w, dst_h,
                    hdc, 0, 0, buffer.w, buffer.h, blendfn);

            if (!blend_status) {
                StretchBlt(hdc_dst, 0, 0, dst_w, dst_h,
                    hdc, 0, 0, buffer.w, buffer.h, SRCCOPY);
            }

            if (node->flags & ED_BORDER) {
                DrawEdge(hdc_dst, &rect, BDR_SUNKEN, BF_RECT);
            }

            DeleteDC(hdc);
            EndPaint(hwnd, &ps);
            return 1;
        }
        break;
    }

    return DefWindowProc(hwnd, msg, wparam, lparam);
}

static LRESULT __stdcall
ed_color_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    ed_node *node = (ed_node *)GetWindowLongPtr(hwnd, GWLP_USERDATA);

    switch (msg) {
    case WM_LBUTTONUP: {
        if (node && node->value_ptr) {
            ed_open_color_picker(node);
            return 1;
        }
        break;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        RECT rect;
        GetClientRect(hwnd, &rect);
        HDC hdc = BeginPaint(hwnd, &ps);

        DrawEdge(hdc, &rect, BDR_SUNKEN, BF_RECT);
        InflateRect(&rect, -2, -2);

        float *rgba = (float *)node->value_ptr;
        if (rgba) {
            unsigned char r = (unsigned char)(rgba[0] * 255.0f);
            unsigned char g = (unsigned char)(rgba[1] * 255.0f);
            unsigned char b = (unsigned char)(rgba[2] * 255.0f);

            HBRUSH hbr = CreateSolidBrush(RGB(r, g, b));
            FillRect(hdc, &rect, hbr);
            DeleteObject(hbr);
        }

        EndPaint(hwnd, &ps);
        return 1;
    }
    }

    return DefWindowProc(hwnd, msg, wparam, lparam);
}

static LRESULT __stdcall
ed_color_slice_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    ed_node *node = (ed_node *)GetWindowLongPtr(hwnd, GWLP_USERDATA);

    switch (msg) {
    case WM_LBUTTONDOWN: {
        SetCapture(hwnd);
        ed_reset_focus(node);
        [[fallthrough]];
    }
    case WM_MOUSEMOVE: {
        if (GetKeyState(VK_LBUTTON) & 0x80) {
            ed_bitmap_buffer buffer = ed_read_value(ed_bitmap_buffer, node->value);
            short cursor_x = LOWORD(lparam);
            short cursor_y = HIWORD(lparam);
            cursor_x = ed_clamp(cursor_x, 0, buffer.w);
            cursor_y = ed_clamp(cursor_y, 0, buffer.h);

            color_picker.hsv[1] = (float)cursor_x / buffer.w;
            color_picker.hsv[2] = (float)(buffer.h - cursor_y) / buffer.h;
            color_picker.rgb_packed = ed_pack_rgb(color_picker.rgba);

            InvalidateRect(hwnd, NULL, FALSE);

            ed_rgb_from_hsv(color_picker.rgba, color_picker.hsv);
            ed_data(color_picker.rgba_slider, color_picker.rgba);
            InvalidateRect(ed_hwnd(color_picker.node), NULL, FALSE);
            ed_data(color_picker.rgb_hex, &color_picker.rgb_packed);
            break;
        }
    }
    case WM_LBUTTONUP: {
        SetCapture(NULL);
        break;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        RECT rect;
        GetClientRect(hwnd, &rect);
        HDC hdc = BeginPaint(hwnd, &ps);

        HDC slice_hdc = CreateCompatibleDC(hdc);
        SelectObject(slice_hdc, (HBITMAP)node->value_ptr);
        ed_bitmap_buffer buffer = ed_read_value(ed_bitmap_buffer, node->value);

        int dst_w = rect.right - rect.left;
        int dst_h = rect.bottom - rect.top;

        StretchBlt(hdc, 0, 0, dst_w, dst_h,
                slice_hdc, 0, 0, buffer.w, buffer.h, SRCCOPY);

        int cursor_x = (int)(color_picker.hsv[1] * buffer.w);
        int cursor_y = (int)((1 - color_picker.hsv[2]) * buffer.h);

        HPEN outer_border = CreatePen(PS_SOLID, 1, RGB(0, 0, 0));
        HPEN inner_border = CreatePen(PS_SOLID, 1, RGB(255, 255, 255));

        SelectObject(hdc, GetStockObject(NULL_BRUSH));
        SelectObject(hdc, outer_border);
        Ellipse(hdc, cursor_x - 8, cursor_y - 8, cursor_x + 8, cursor_y + 8);

        SelectObject(hdc, inner_border);
        Ellipse(hdc, cursor_x - 7, cursor_y - 7, cursor_x + 7, cursor_y + 7);

        DeleteDC(slice_hdc);
        DeleteObject(outer_border);
        DeleteObject(inner_border);
        EndPaint(hwnd, &ps);
        return 1;
    }
    }

    return DefWindowProc(hwnd, msg, wparam, lparam);
}

static LRESULT __stdcall
ed_color_hue_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    ed_node *node = (ed_node *)GetWindowLongPtr(hwnd, GWLP_USERDATA);

    switch (msg) {
    case WM_LBUTTONDOWN: {
        SetCapture(hwnd);
        ed_reset_focus(node);
        [[fallthrough]];
    }
    case WM_MOUSEMOVE: {
        if (GetKeyState(VK_LBUTTON) & 0x80) {
            ed_bitmap_buffer buffer = ed_read_value(ed_bitmap_buffer, node->value);

            if (buffer.h > buffer.w) {
                short cursor_y = HIWORD(lparam);
                cursor_y = ed_clamp(cursor_y, 0, buffer.h);
                color_picker.hsv[0] = (float)(buffer.h - cursor_y) / buffer.h;
            } else {
                short cursor_x = LOWORD(lparam);
                cursor_x = ed_clamp(cursor_x, 0, buffer.w);
                color_picker.hsv[0] = (float)cursor_x / buffer.w;
            }

            ed_rgb_from_hsv(color_picker.rgba, color_picker.hsv);
            color_picker.rgb_packed = ed_pack_rgb(color_picker.rgba);
            ed_data(color_picker.rgba_slider, color_picker.rgba);
            InvalidateRect(hwnd, NULL, FALSE);

            // Updating hue changes which hue slice to show.
            ed_update_color_picker_slice();

            InvalidateRect(ed_hwnd(color_picker.slice), NULL, FALSE);
            InvalidateRect(ed_hwnd(color_picker.node), NULL, FALSE);
            ed_data(color_picker.rgb_hex, &color_picker.rgb_packed);
            break;
        }
    }
    case WM_LBUTTONUP: {
        SetCapture(NULL);
        break;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        RECT rect;
        GetClientRect(hwnd, &rect);
        HDC hdc = BeginPaint(hwnd, &ps);

        HDC hue_hdc = CreateCompatibleDC(hdc);
        SelectObject(hue_hdc, (HBITMAP)node->value_ptr);
        ed_bitmap_buffer buffer = ed_read_value(ed_bitmap_buffer, node->value);

        int dst_w = rect.right - rect.left;
        int dst_h = rect.bottom - rect.top;

        StretchBlt(hdc, 0, 0, dst_w, dst_h,
                hue_hdc, 0, 0, buffer.w, buffer.h, SRCCOPY);

        HBRUSH tri_fill = CreateSolidBrush(RGB(0, 0, 0));
        POINT tris[6];
        INT index[] = {3, 3};

        if (buffer.h > buffer.w) {
            // Drawing this |> <|
            int cursor_y = (int)((1 - color_picker.hsv[0]) * buffer.h);
            tris[0] = {0, cursor_y - 6};
            tris[1] = {6, cursor_y};
            tris[2] = {0, cursor_y + 6};
            tris[3] = {rect.right - 1, cursor_y - 6};
            tris[4] = {rect.right - 7, cursor_y};
            tris[5] = {rect.right - 1, cursor_y + 6};
        } else {
            int cursor_x = (int)(color_picker.hsv[0] * buffer.w);
            tris[0] = {cursor_x - 6, 0};
            tris[1] = {cursor_x, 6};
            tris[2] = {cursor_x + 6, 0};
            tris[3] = {cursor_x - 6, rect.bottom - 1};
            tris[4] = {cursor_x, rect.bottom - 7};
            tris[5] = {cursor_x + 6, rect.bottom - 1};
        }

        SelectObject(hdc, tri_fill);
        PolyPolygon(hdc, tris, index, ARRAYSIZE(index));

        DeleteDC(hue_hdc);
        DeleteObject(tri_fill);
        EndPaint(hwnd, &ps);
        return 1;
    }
    }

    return DefWindowProc(hwnd, msg, wparam, lparam);
}

static LRESULT __stdcall
ed_edit_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam,
        UINT_PTR /* id */, DWORD_PTR /* data */)
{
    ed_node *node = (ed_node *)GetWindowLongPtr(hwnd, GWLP_USERDATA);

    auto write_number_value = [] (ed_node *node, auto value, bool valid) {
        if (valid) {
            auto val_min = ed_read_value(decltype(value), node->value_min);
            auto val_max = ed_read_value(decltype(value), node->value_max);

            if (val_min != val_max) value = ed_clamp(value, val_min, val_max);
            ed_write_value(decltype(value), node->value, &value);

            if (node->onchange) node->onchange(node);
            ed_write_value(decltype(value), node->value_ptr, node->value);
        }
        ed_invalidate_data(node);
    };

    switch (msg) {
    case WM_CHAR:
        if (wparam == VK_ESCAPE) {
            ed_reset_focus(node);
            return 1;
        }
        if (!(wparam == VK_RETURN)) {
            break;
        }

        [[fallthrough]];
    case WM_KILLFOCUS:
        if (node->value_ptr && !(node->flags & ED_READONLY)) {
            if (node->value_type == ED_STRING) {
                if (node->value_size > 0) {
                    size_t min_size = (size_t)(GetWindowTextLength(ed_hwnd(node)) + 1);
                    ed_write_value(size_t, node->value, &min_size);

                    if (node->onchange) node->onchange(node);
                    GetWindowText(ed_hwnd(node),
                            (char *)node->value_ptr, (int)node->value_size);
                }
                break;
            }

            char buf[64];
            char *end;
            int len = GetWindowText(ed_hwnd(node), buf, sizeof buf);
            int base = ed_read_value(int, &node->value[8]);

            switch (node->value_type) {
            case ED_INT: {
                int value = (int)strtol(buf, &end, base);
                write_number_value(node, value, (end - buf) == len);
                break;
            }
            case ED_FLOAT: {
                float value = strtof(buf, &end);
                write_number_value(node, value, (end - buf) == len);
                break;
            }
            case ED_INT64: {
                long long value = strtoll(buf, &end, base);
                write_number_value(node, value, (end - buf) == len);
                break;
            }
            case ED_FLOAT64: {
                double value = strtof(buf, &end);
                write_number_value(node, value, (end - buf) == len);
                break;
            }
            default:
                assert(!"unsupported value_type for input node.");
            }
        }
        break;
    }

    if (msg == WM_CHAR && wparam == VK_RETURN) {
        LONG style = GetWindowLong(hwnd, GWL_STYLE);
        if (!(style & ES_MULTILINE)) {
            return 1;
        }
    }

    return DefSubclassProc(hwnd, msg, wparam, lparam);
}

static LRESULT __stdcall
ed_number_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam,
        UINT_PTR /* id */, DWORD_PTR /* data */)
{
    static short last_mouse_x = 0;
    static int mouse_acc = 0;

    ed_node *node = (ed_node *)GetWindowLongPtr(hwnd, GWLP_USERDATA);

    if ((node->flags & ED_READONLY) || !IsWindowEnabled(hwnd)) {
        return DefSubclassProc(hwnd, msg, wparam, lparam);
    }

    auto set_editing = [] (ed_node *node, bool editing) {
        if ((bool)(node->flags & ED_EDITING) != editing) {
            if (editing) {
                node->flags |= ED_EDITING;
                ShowCaret(ed_hwnd(node));
            } else {
                node->flags &= ~ED_EDITING;
                HideCaret(ed_hwnd(node));
            }
            InvalidateRect(ed_hwnd(node), NULL, TRUE);
        }
    };

    auto update_slider = [] (ed_node *node, short mouse_x, int mouse_acc, auto delta_x) {
        auto value = ed_read_value(decltype(delta_x), node->value);
        auto val_min = ed_read_value(decltype(delta_x), node->value_min);
        auto val_max = ed_read_value(decltype(delta_x), node->value_max);

        if (mouse_acc > ed_style.number_input_deadzone) {
            if (val_min != val_max) {
                // Slider
                RECT rc;
                GetWindowRect(ed_hwnd(node), &rc);

                double t = mouse_x / (double)(rc.right - rc.left);
                auto slider_x = (decltype(delta_x))(val_min + ((double)(val_max - val_min) * t));
                value = ed_clamp(slider_x, val_min, val_max);
            } else {
                // Unbound slider
                value += delta_x;
            }
        }

        ed_write_value(decltype(delta_x), node->value, &value);
        if (node->onchange) node->onchange(node);

        ed_write_value(decltype(delta_x), node->value_ptr, &value);
        ed_invalidate_data(node);
    };

    auto fill_slider = [] (ed_node *node, HDC hdc, RECT *rc_slider, auto value) {
        auto val_min = ed_read_value(decltype(value), node->value_min);
        auto val_max = ed_read_value(decltype(value), node->value_max);

        if (val_min != val_max) {
            double t = (value - val_min) / (double)(val_max - val_min);
            double w = (double)(rc_slider->right - rc_slider->left) * t;
            int right_max = rc_slider->right;
            rc_slider->right = (int)(rc_slider->left + w);
            rc_slider->right = ed_min(rc_slider->right, right_max);
            FillRect(hdc, rc_slider, brushes[ED_COLOR_HIGHLIGHT]);
            return;
        }
        rc_slider->right = rc_slider->left;
    };

    switch (msg) {
    case WM_LBUTTONDOWN: {
        last_mouse_x = LOWORD(lparam);
        mouse_acc = 0;
        InvalidateRect(hwnd, NULL, TRUE);
        SetCapture(hwnd);
        break;
    }
    case WM_LBUTTONUP: {
        SetCapture(NULL);
        if (!(node->flags & ED_EDITING)) {
            if (mouse_acc <= ed_style.number_input_deadzone) {
                // Click on input node without sliding mouse over deadzone to edit.
                set_editing(node, true);
                SendMessage(hwnd, EM_SETSEL, 0, -1);
            } else {
                // Outside of edit mode, the number slider only has temporary
                // focus while active.
                ed_reset_focus(node);
                return 1;
            }
        }
        break;
    }
    case WM_RBUTTONUP: {
        if (!(GetKeyState(VK_LBUTTON) & 0x80)) {
            // Right click to exit out of edit mode.
            set_editing(node, false);
            SendMessage(hwnd, EM_SETSEL, 0, 0);
            SetCursor(LoadCursor(NULL, IDC_ARROW));
            ed_reset_focus(node);
            return 1;
        }
        break;
    }
    case WM_SETFOCUS: {
        DefSubclassProc(hwnd, msg, wparam, lparam);
        HideCaret(hwnd);
        return 1;
    }
    case WM_KILLFOCUS: {
        set_editing(node, false);
        break;
    }
    case ED_WM_TABSTOPSETFOCUS: {
        set_editing(node, true);
        SendMessage(hwnd, EM_SETSEL, 0, -1);
        InvalidateRect(hwnd, NULL, TRUE);
        break;
    }
    case WM_SETCURSOR: {
        if (!(node->flags & ED_EDITING)) {
            SetCursor(LoadCursor(NULL, IDC_ARROW));
            return 0;
        }
        break;
    }
    case WM_MOUSEMOVE: {
        if (!node->value_ptr) {
            break;
        }
        if (!(node->flags & ED_EDITING) && (GetKeyState(VK_LBUTTON) & 0x80)) {
            short mouse_x = LOWORD(lparam);
            int mouse_delta = mouse_x - last_mouse_x;
            mouse_acc += ed_abs(mouse_delta);
            last_mouse_x = mouse_x;

            switch (node->value_type) {
            case ED_INT:
                update_slider(node, mouse_x, mouse_acc, mouse_delta);
                break;
            case ED_FLOAT:
                update_slider(node, mouse_x, mouse_acc,
                        (float)mouse_delta * ed_style.number_input_float_increment);
                break;
            case ED_INT64:
                update_slider(node, mouse_x, mouse_acc, (long long)mouse_delta);
                break;
            case ED_FLOAT64:
                update_slider(node, mouse_x, mouse_acc,
                        (double)mouse_delta * ed_style.number_input_float64_increment);
                break;
            default:
                break;
            }
            return 1;
        }
        break;
    }
    case WM_PAINT: {
        if (!(node->flags & ED_EDITING)) {
            char buf[64];
            PAINTSTRUCT ps;
            RECT rect;
            GetClientRect(hwnd, &rect);

            HDC hdc = BeginPaint(hwnd, &ps);
            HDC buf_hdc = CreateCompatibleDC(hdc);
            int width = rect.right - rect.left;
            int height = rect.bottom - rect.top;
            HBITMAP hbm = CreateCompatibleBitmap(hdc, width, height);
            SelectObject(buf_hdc, hbm);

            FillRect(buf_hdc, &rect, (HBRUSH)(COLOR_WINDOW + 1));
            RECT rc_slider = rect;

            switch (node->value_type) {
            case ED_INT:
                fill_slider(node, buf_hdc, &rc_slider,
                        ed_read_value(int, node->value));
                break;
            case ED_FLOAT:
                fill_slider(node, buf_hdc, &rc_slider,
                        ed_read_value(float, node->value));
                break;
            case ED_INT64:
                fill_slider(node, buf_hdc, &rc_slider,
                        ed_read_value(long long, node->value));
                break;
            case ED_FLOAT64:
                fill_slider(node, buf_hdc, &rc_slider,
                        ed_read_value(double, node->value));
                break;
            default:
                break;
            }

            FrameRect(buf_hdc, &rect, brushes[ED_COLOR_GRAYTEXT]);

            SelectObject(buf_hdc, ui_font);
            SetTextColor(buf_hdc, ed_style.colors[ED_COLOR_WINDOWTEXT]);
            SetBkColor(buf_hdc, ed_style.colors[ED_COLOR_WINDOW]);

            int len = GetWindowText(hwnd, buf, sizeof buf);
            DrawText(buf_hdc, buf, len, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

            if (rc_slider.right > rect.left) {
                // Text covered by slider (invert color)
                HRGN slider_rgn = CreateRectRgn(rc_slider.left, rc_slider.top,
                        rc_slider.right, rc_slider.bottom);

                SelectClipRgn(buf_hdc, slider_rgn);
                SetTextColor(buf_hdc, ed_style.colors[ED_COLOR_HIGHLIGHTTEXT]);
                SetBkColor(buf_hdc, ed_style.colors[ED_COLOR_HIGHLIGHT]);

                DrawText(buf_hdc, buf, len, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                DeleteObject(slider_rgn);
            }

            BitBlt(hdc, 0, 0, width, height, buf_hdc, 0, 0, SRCCOPY);
            DeleteObject(hbm);
            DeleteDC(buf_hdc);
            EndPaint(hwnd, &ps);

            return 1;
        }
        break;
    }
    }

    return DefSubclassProc(hwnd, msg, wparam, lparam);
}

static LRESULT __stdcall
ed_tabstop_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam,
        UINT_PTR /* id */, DWORD_PTR /* data */)
{
    static bool vk_shift_down = false;

    ed_node *node = (ed_node *)GetWindowLongPtr(hwnd, GWLP_USERDATA);

    switch (msg) {
    case WM_KEYDOWN: {
        if (wparam == VK_SHIFT) vk_shift_down = true;
        if (wparam == VK_RETURN || wparam == VK_SPACE) {
            if (node->type == ED_CHECKBOX) {
                SendMessage(hwnd, BM_CLICK, 0, 0);
            } else if (node->type == ED_COMBOBOX) {
                SendMessage(hwnd, CB_SHOWDROPDOWN, 1, 0);
            }
            return 1;
        }
        break;
    }
    case WM_KEYUP: {
        if (wparam == VK_SHIFT) vk_shift_down = false;
        break;
    }
    case WM_CHAR: {
        if (wparam == VK_TAB) {
            ed_node *tabstop;
            if (vk_shift_down) {
                tabstop = ed_previous_tabstop(node);
            } else {
                tabstop = ed_next_tabstop(node);
            }

            if (tabstop) {
                ed_set_focus(tabstop);
                SendMessage(ed_hwnd(tabstop), ED_WM_TABSTOPSETFOCUS, 0, 0);
            }
            return 1;
        }
        break;
    }
    }
    return DefSubclassProc(hwnd, msg, wparam, lparam);
}

static LRESULT __stdcall
ed_text_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam,
        UINT_PTR /* id */, DWORD_PTR /* data */)
{
    ed_node *node = (ed_node *)GetWindowLongPtr(hwnd, GWLP_USERDATA);
    LRESULT result = DefSubclassProc(hwnd, msg, wparam, lparam);

    if (msg == WM_SETTEXT) {
        if (node->rect.w == 0 || node->rect.h == 0) {
            ed_measure_text_bounds(node);
            ed_invalidate(node);
        }
    }
    return result;
}

static ATOM
ed_register_class(const char *name, WNDPROC proc)
{
    WNDCLASS wnd_class = {};
    wnd_class.lpfnWndProc   = proc;
    wnd_class.hInstance     = GetModuleHandle(NULL);
    wnd_class.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wnd_class.lpszClassName = name;
    return RegisterClass(&wnd_class);
}

// Returns a node given a unique id.
//
// Valid ids are in range:
//
//     1 <= id < ED_NODE_COUNT <= 0x7FFF
//
// A node with id=ED_ID_ROOT is the root window. The root window also has a
// null parent, all other valid nodes have a parent node.
ed_node *
ed_index_node(short id)
{
    assert((size_t)id < ARRAYSIZE(ed_tree));
    return &ed_tree[id];
}

// Initializes the library.
//   hwnd: the root window of the application.
void
ed_init(void *hwnd)
{
    if (ed_index_node(ED_ID_ROOT)->id == ED_ID_ROOT) {
        // Already initialized
        return;
    }

    ed_style = {};
    ed_stats = {};
    ed_ctx = {};
    used_node_count = 0;
    registered_update_count = 0;

    ed_apply_system_colors();
    ed_allocate_colors();

    // Default UI font
    NONCLIENTMETRICS ncmetrics = {};
    ncmetrics.cbSize = sizeof(ncmetrics);
    SystemParametersInfo(SPI_GETNONCLIENTMETRICS, sizeof(ncmetrics), &ncmetrics, 0);
    ui_font = CreateFontIndirect(&ncmetrics.lfStatusFont);

    ed_style.value_formats[ED_STRING]  = "%s";
    ed_style.value_formats[ED_INT]     = "%d";
    ed_style.value_formats[ED_FLOAT]   = "%.3f";
    ed_style.value_formats[ED_INT64]   = "%lld";
    ed_style.value_formats[ED_FLOAT64] = "%.3f";

    // Parent window class
    ed_register_class("ED_WINDOW", ed_window_proc);
    ed_register_class("ED_USERWINDOW", ed_user_window_proc);
    ed_register_class("ED_CAPTION", ed_caption_proc);
    ed_register_class("ED_IMAGE", ed_image_proc);
    ed_register_class("ED_COLOR", ed_color_proc);
    ed_register_class("ED_COLOR_SLICE", ed_color_slice_proc);
    ed_register_class("ED_COLOR_HUE", ed_color_hue_proc);

    ed_node *root = ed_index_node(ED_ID_ROOT);
    used_node_count = ED_ID_ROOT;
    root->id = ED_ID_ROOT;
    root->type = ED_WINDOW;
    root->flags = ED_ROOT;
    root->layout = ED_ABS;
    root->hwnd = hwnd;
    ed_ctx.parent = root;
    ed_ctx.child = NULL;

    ed_resize(hwnd);
}

// Frees resources used by the library.
void
ed_deinit()
{
    for (size_t i = ED_ID_ROOT; i < used_node_count; ++i) {
        ed_free_node_resources(&ed_tree[i]);
    }

    ed_node *root = ed_index_node(ED_ID_ROOT);
    for (ed_node *c = root->child; c; c = c->after) {
        ed_destroy_node(c);
    }

    for (size_t color = 0; color < ED_COLOR_COUNT; ++color) {
        DeleteObject(brushes[color]);
    }

    if (ui_font) {
        DeleteObject(ui_font);
    }

    memset(ed_tree, 0, sizeof ed_tree);
}

// Registers an update function to be run during `ed_update`.
//
// node:
//   The node associated with the update function. The update function will be
//   unregistered when the node is removed and `ed_update` only calls the given
//   update function if the node is visible at the time.
//
//   If NULL, the update function is always called.
void
ed_register_update(ed_node *node, void (*update)())
{
    assert(registered_update_count < ED_UPDATE_FUNCS_COUNT
            && "too many registered update functions.");

    ed_update_funcs[registered_update_count] = {node, update};
    ++registered_update_count;

    if (node) {
        node->flags |= ED_OWNUPDATE;
    }
}

// Unregisters update functions associated with the given node.
//
// node:
//   If NULL, all functions are unregistered.
void
ed_unregister_update(ed_node *node)
{
    if (!node) {
        registered_update_count = 0;
        memset(ed_update_funcs, 0, sizeof(ed_update_funcs));
        return;
    }

    node->flags &= ~ED_OWNUPDATE;

    for (size_t i = registered_update_count; i-- > 0;) {
        if (ed_update_funcs[i].node == node) {
            --registered_update_count;
            ed_update_funcs[i] = ed_update_funcs[registered_update_count];
            ed_update_funcs[registered_update_count] = {};
        }
    }
}

// Performs a full update by calling update functions in ed_update_funcs every
// `n` frames. If `n > 1` this tries to spread update calls to subsequent
// frames.
//
//     ed_register_update([] { printf("A "); });
//     ed_register_update([] { printf("B "); });
//     ed_register_update([] { printf("C "); });
//
//     ed_update(2); // A B
//     ed_update(2); // C
//     ed_update(2); // A B
//     // ...
//
// update_every_n_frames:
//   How many frames until all update calls registered with
//   `ed_register_update` are called.
//
//   If `ed_update(4)` is called 60 times per
//   second, each registered update call gets called 15 times per second.
void
ed_update(unsigned update_every_n_frames)
{
    static unsigned offset = 0;

    LARGE_INTEGER start, end;
    QueryPerformanceCounter(&start);
    ed_stats.data_calls = 0;

    unsigned groups = ed_max(update_every_n_frames, 1);
    unsigned chunk = registered_update_count / groups;

    if (chunk <= 0 && (ed_stats.update_calls % groups) != 0) {
        // If the chunk size is too small, run all update calls in the same
        // frame.
        goto next_frame;
    }
    if (offset >= registered_update_count) {
        offset = 0;
    }
    if (offset == 0) {
        chunk += registered_update_count % groups;
    }

    for (unsigned i = offset, chunk_end = offset + chunk; i < chunk_end; ++i) {
        ed_node_update *node_update = &ed_update_funcs[i];
        if (node_update->node && !ed_is_visible(node_update->node)) {
            // Hidden nodes are not updated.
            continue;
        }
        if (node_update->update) node_update->update();
    }
    offset += chunk;

next_frame:
    ++ed_stats.update_calls;
    QueryPerformanceCounter(&end);
    ed_stats.update_ticks = end.QuadPart - start.QuadPart;
}

// Set colors to the default system theme.
void
ed_apply_system_colors()
{
    ed_style.colors[ED_COLOR_WINDOW]        = GetSysColor(COLOR_WINDOW);
    ed_style.colors[ED_COLOR_WINDOWTEXT]    = GetSysColor(COLOR_WINDOWTEXT);
    ed_style.colors[ED_COLOR_3DFACE]        = GetSysColor(COLOR_3DFACE);
    ed_style.colors[ED_COLOR_HIGHLIGHT]     = GetSysColor(COLOR_HIGHLIGHT);
    ed_style.colors[ED_COLOR_GRAYTEXT]      = GetSysColor(COLOR_GRAYTEXT);
    ed_style.colors[ED_COLOR_HIGHLIGHTTEXT] = GetSysColor(COLOR_HIGHLIGHTTEXT);
    ed_style.colors[ED_COLOR_BTNTEXT]       = GetSysColor(COLOR_BTNTEXT);
}

// Creates brushes for colors.
void
ed_allocate_colors()
{
    for (size_t color = 0; color < ED_COLOR_COUNT; ++color) {
        if (brushes[color]) {
            DeleteObject(brushes[color]);
        }
        brushes[color] = CreateSolidBrush((COLORREF)ed_style.colors[color]);
    }
}

// Call when the root window is resized.
void
ed_resize(void *hwnd)
{
    ed_node *root = ed_index_node(ED_ID_ROOT);
    RECT rc_root;
    GetClientRect((HWND)hwnd, &rc_root);

    root->rect.x = (float)rc_root.left;
    root->rect.y = (float)rc_root.top;
    root->rect.w = (float)rc_root.right - rc_root.left;
    root->rect.h = (float)rc_root.bottom - rc_root.top;

    ed_invalidate(root);
}

// Lays out and draws a node.
void
ed_invalidate(ed_node *node)
{
    LARGE_INTEGER start, end;

    QueryPerformanceCounter(&start);
    ed_measure(node);
    QueryPerformanceCounter(&end);
    ed_stats.measure_ticks = end.QuadPart - start.QuadPart;

    QueryPerformanceCounter(&start);
    ed_layout(node);
    QueryPerformanceCounter(&end);
    ed_stats.layout_ticks = end.QuadPart - start.QuadPart;

    InvalidateRect(ed_hwnd(node), NULL, TRUE);
    ++ed_stats.invalidate_calls;
}

// Updates the window text or image with the node value.
void
ed_invalidate_data(ed_node *node)
{
    if (node->value_type >= ED_VALUE_TYPE_SCALAR_MIN &&
            node->value_type <= ED_VALUE_TYPE_SCALAR_MAX) {
        ed_invalidate_scalar(node);
    } else if (node->value_type == ED_STRING) {
        SetWindowText(ed_hwnd(node), (char *)node->value_ptr);
    } else if (node->value_type == ED_DIB) {
        InvalidateRect(ed_hwnd(node), NULL, TRUE);
    } else if (node->value_type == ED_COLOR) {
        InvalidateRect(ed_hwnd(node), NULL, TRUE);
    }
}

// Updates the value for a node and invalidates the node if the value has
// changed since the last call to `ed_data`. If the data spans multiple nodes,
// this function recurses while `next_value_node` is not NULL.
//
// size:
//   The size in bytes of the data pointed to by `value`. If 0, the default
//   size is chosen based on the node value type, unless the type is ED_STRING.
//   If the data spans multiple nodes, the size should correspond to the size
//   of a single element in the array, not the size of the entire array.
//
//   For strings:
//   This argument is the size of the string buffer pointed to by `value`. If
//   the user input exceeds the given buffer size, the input is truncated.
void
ed_data(ed_node *node, void *value, size_t size)
{
    assert(node->type != ED_NONE
            && "invalid node, it's possible this node was previously removed.");

    if (node->value_type >= ED_VALUE_TYPE_SCALAR_MIN &&
            node->value_type <= ED_VALUE_TYPE_SCALAR_MAX) {
        ed_data_scalar(node, value, size);
    } else if (node->value_type == ED_STRING) {
        ed_data_string(node, value, size);
    } else if (node->value_type == ED_DIB) {
        ed_data_image(node, value);
    } else if (node->value_type == ED_COLOR) {
        ed_data_color(node, value, size);
    } else {
        // Bitmaps and icons can only be initialized once, for a writable image
        // buffer use ed_image_buffer (ED_BITMAPBUFFER) instead.
        assert(!"node value type not supported by ed_data.");
    }

    ++ed_stats.data_calls;

    if (node->node_list) {
        // Data spans multiple nodes, only the first node should be passed to ed_data
        // by the user.
        assert(node->value_size);
        ed_data(node->node_list, (char *)value + node->value_size);
    }
}

// Saves the current active parent and child, and sets the active parent to the
// given node. The next control will be inserted after the last child of
// `node`.
//
// `ed_end_context` should be called to restore the previous context.
//
// Calls to `ed_begin_context` may not be nested. Only one context can be saved
// at a time.
void
ed_begin_context(ed_node *node)
{
    assert(node);
    assert(!ed_saved_ctx.parent
            && "expected ed_end_context before next call to ed_begin_context.");

    ed_saved_ctx = ed_ctx;
    ed_ctx.parent = node;
    ed_ctx.child = node->child;

    if (ed_ctx.child) {
        while (ed_ctx.child->after) {
            ed_ctx.child = ed_ctx.child->after;
        }
    }
}

// Restores active context with context saved with `ed_begin_context`.
void
ed_end_context()
{
    assert(ed_saved_ctx.parent
            && "expected ed_begin_context before ed_end_context.");

    ed_invalidate(ed_ctx.parent);

    ed_ctx.parent = ed_saved_ctx.parent;
    ed_ctx.child = ed_saved_ctx.child;
    memset(&ed_saved_ctx, 0, sizeof(ed_saved_ctx));
}

// Sets the active child node to the given node. The next control created will
// be inserted after `node` in the UI tree.
void
ed_insert_after(ed_node *node)
{
    assert(node);
    assert(node->parent && "cannot insert a new root node.");

    ed_ctx.child = node;
    ed_ctx.parent = node->parent;
}

// Destroys a node window and removes the node from the UI tree.
void
ed_remove(ed_node *node)
{
    assert(node);
    assert(node->parent && "cannot remove root node.");

    ed_free_node_resources(node);
    --active_node_count;

    if (ed_ctx.child == node) ed_ctx.child = node->before;
    if (ed_ctx.parent == node) ed_ctx.parent = node->parent;

    node->type = ED_NONE;
    node->node_list = ed_ctx.removed;
    ed_ctx.removed = node;

    if (node->parent->type != ED_NONE) {
        if (node->before) {
            node->before->after = node->after;
            if (node->after) {
                node->after->before = node->before;
            }
        } else {
            node->parent->child = node->after;
            if (node->after) {
                node->after->before = NULL;
            }
        }

        ed_destroy_node(node);
        ed_invalidate(node->parent);
    }

    for (ed_node *c = node->child; c; c = c->after) {
        ed_remove(c);
    }
}

// Creates a window handle for a node.
//
// flags:
//   Style flags.
void
ed_attach_hwnd(ed_node *node, const char *class_name, const char *name, int flags)
{
    assert(node->parent);
    size_t id = node->id;
    HMENU hmenu = NULL;
    if (flags & WS_CHILD) hmenu = (HMENU)id;

    node->hwnd = CreateWindow(
            class_name, name, flags,
            0, 0, 100, 20, // Set during ed_layout
            ed_hwnd(node->parent),
            hmenu, NULL, NULL);

    SetWindowLongPtr(ed_hwnd(node), GWLP_USERDATA, (LONG_PTR)node);

    if ((node->flags & ED_TEXTNODE) || node->type == ED_COMBOBOX) {
        SendMessage(ed_hwnd(node), WM_SETFONT, (WPARAM)ui_font, FALSE);
    }
}

// Creates a parent block.
//
// All functions starting with `ed_begin` must eventually be followed by
// `ed_end`.
ed_node *
ed_begin(ed_rect rect, ed_node_layout layout)
{
    ed_node *node = ed_push(ED_BLOCK, rect);
    node->layout = layout;
    ed_attach_hwnd(node, "ED_WINDOW", NULL, WS_CHILD | WS_VISIBLE);
    return node;
}

// Creates a block with padding and an outline border.
ed_node *
ed_begin_border(ed_rect rect, ed_node_layout layout)
{
    ed_node *node = ed_push(ED_BLOCK, rect);
    node->layout = layout;
    node->padding = ed_style.padding;
    node->spacing = ed_style.spacing;
    node->flags = ED_BORDER;

    ed_attach_hwnd(node, "ED_WINDOW", NULL, WS_CHILD | WS_VISIBLE);
    return node;
}

// Creates a vertical scroll block. The scrollbar is hidden until the content
// overflows the scroll client.
ed_node *
ed_begin_scroll(ed_node_layout layout)
{
    ed_node *scrollblock = ed_push(ED_SCROLLBLOCK, {0, 0, 1.0f, 1.0f});
    scrollblock->layout = ED_HORZ;

    ed_node *client = ed_attach(ED_BLOCK, {0, 0, 1.0f, 1.0f});
    client->layout = layout;
    client->padding = ed_style.padding;
    client->flags = ED_POPPARENT;

    int sb_width = GetSystemMetrics(SM_CXVSCROLL);
    ed_node *scroll_bar = ed_attach(ED_SCROLLBAR, {1.0f, 0, (float)sb_width, 1.0f});
    scroll_bar->scroll_client = client->id;
    client->scroll_bar = scroll_bar->id;

    ed_attach_hwnd(scrollblock, "ED_WINDOW", NULL, WS_CHILD | WS_VISIBLE);
    ed_attach_hwnd(client, "ED_WINDOW", NULL, WS_CHILD | WS_VISIBLE);
    ed_attach_hwnd(scroll_bar, "SCROLLBAR", NULL, WS_CHILD | SBS_VERT);
    ed_ctx.parent = client; // push client node
    return scrollblock;
}

// Creates a new child window with a title bar and vertical scroll bar.
ed_node *
ed_begin_window(const char *name, ed_rect rect, ed_node_layout layout)
{
    ed_node *node = ed_push(ED_WINDOW, rect);
    node->layout = ED_VERT;
    node->flags = ED_BORDER;
    node->padding = 1;

    ed_node *cap = ed_attach(ED_CAPTION, {0, 0, 1.0f, (float)ed_style.caption_height});
    ed_attach_hwnd(node, "ED_WINDOW", name, WS_CHILD | WS_VISIBLE);
    ed_attach_hwnd(cap, "ED_CAPTION", name, WS_CHILD | WS_VISIBLE);

    ed_node *scrollblock = ed_begin_scroll(layout);
    scrollblock->flags |= ED_POPPARENT;
    return node;
}

// Creates a collapsible group.
//
// Use `ed_collapse` and `ed_expand` to programatically control the group.
ed_node *
ed_begin_group(const char *name, ed_rect rect, ed_node_layout layout)
{
    ed_node *node = ed_push(ED_GROUP, rect);
    node->layout = layout;
    node->flags = ED_EXPAND;

    ed_node *cap = ed_attach(ED_CAPTION, {0, 0, 1.0f, (float)ed_style.caption_height});
    cap->spacing = ed_style.spacing;
    cap->flags = ED_TABSTOP;

    ed_attach_hwnd(node, "ED_WINDOW", name, WS_CHILD | WS_VISIBLE);
    ed_attach_hwnd(cap, "ED_CAPTION", name, WS_CHILD | WS_VISIBLE);
    SetWindowSubclass(ed_hwnd(cap), ed_tabstop_proc, 1, 0);
    return node;
}

// Creates a parent button node. For a simple text button, use `ed_button`
// instead.
//
// onclick:
//   Event called when the button is clicked, or NULL.
ed_node *
ed_begin_button(void (*onclick)(ed_node *node), ed_rect rect)
{
    ed_node *node = ed_push(ED_BUTTON, rect);
    node->flags = ED_TABSTOP;
    node->onclick = onclick;
    node->spacing = ed_style.spacing;

    ed_attach_hwnd(node, "BUTTON", NULL, WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON);
    SetWindowSubclass(ed_hwnd(node), ed_tabstop_proc, 1, 0);
    return node;
}

// Create an editor node from an existing window handle. The editor will not
// modify the windows size or position. To use the default window handler, set
// the window class to ED_WINDOW. The window will not be parented to the
// current layout.
//
// To create a window and attach it to the root window:
//
//     ed_begin_context(ed_index_node(ED_ID_ROOT));
//     ed_begin_hwnd(hwnd, ED_ABS);
//     ed_end_context();
ed_node *
ed_begin_hwnd(void *hwnd, ed_node_layout layout)
{
    assert(hwnd);
    ed_node *node = ed_push(ED_USERWINDOW, {});
    node->layout = layout;
    node->hwnd = (HWND)hwnd;

    LONG style = GetWindowLong(ed_hwnd(node), GWL_STYLE);
    if (style & WS_CHILD) {
        assert(node->parent);
        SetParent(ed_hwnd(node), ed_hwnd(node->parent));
    }

    SetWindowLongPtr(ed_hwnd(node), GWLP_USERDATA, (LONG_PTR)node);
    return node;
}

// Ends the parent context.
void
ed_end()
{
    ed_pop();
    if (ed_ctx.parent->id == ED_ID_ROOT) {
        ed_invalidate(ed_ctx.parent);
    }
}

// Creates a static label. To dynamically set the label content, use `ed_data`
// to modify the label. Set the node `value_type` and `value_fmt` to use a
// number format.
ed_node *
ed_label(const char *label, ed_rect rect)
{
    ed_node *node = ed_attach(ED_LABEL, rect);
    node->spacing = ed_style.spacing;
    node->flags = ED_TEXTNODE;

    ed_attach_hwnd(node, "STATIC", label, WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE);
    ed_measure_text_bounds(node);
    SetWindowSubclass(ed_hwnd(node), ed_text_proc, 2, 0);
    return node;
}

// Creates a push button.
//
// onclick:
//   Event called when the button is clicked, or NULL.
ed_node *
ed_button(const char *label, void (*onclick)(ed_node *node), ed_rect rect)
{
    ed_node *node = ed_attach(ED_BUTTON, rect);
    node->flags = ED_BORDER | ED_TABSTOP | ED_TEXTNODE;
    node->onclick = onclick;
    node->spacing = ed_style.spacing;

    ed_attach_hwnd(node, "BUTTON", label, WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON);
    ed_measure_text_bounds(node);
    SetWindowSubclass(ed_hwnd(node), ed_tabstop_proc, 1, 0);
    SetWindowSubclass(ed_hwnd(node), ed_text_proc, 2, 0);
    return node;
}

// Creates a blank space in a vertical or horizontal layout.
ed_node *
ed_space(float size)
{
    assert(ed_ctx.parent && ed_ctx.parent->layout != ED_ABS &&
            "space node can only be used with a horizontal or vertical layout.");
    ed_node *node;

    if (ed_ctx.parent->layout == ED_VERT) {
        node = ed_attach(ED_SPACE, {0, 0, 1.0f, size});
    } else {
        node = ed_attach(ED_SPACE, {0, 0, size, 1.0f});
    }

    ed_attach_hwnd(node, "ED_WINDOW", "", WS_CHILD | WS_VISIBLE);
    return node;
}

// Creates a vertical or horizontal separator.
ed_node *
ed_separator()
{
    assert(ed_ctx.parent && ed_ctx.parent->layout != ED_ABS &&
            "separator node can only be used with a horizontal or vertical layout.");
    ed_node *node;

    if (ed_ctx.parent->layout == ED_VERT) {
        node = ed_attach(ED_SEPARATOR, {0, 0, 1.0f, 1.1f});
        node->spacing = ed_style.spacing;
        ed_attach_hwnd(node, "STATIC", "", WS_CHILD | WS_VISIBLE | SS_ETCHEDHORZ);
    } else {
        node = ed_attach(ED_SEPARATOR, {0, 0, 1.1f, 1.0f});
        node->spacing = ed_style.spacing;
        ed_attach_hwnd(node, "STATIC", "", WS_CHILD | WS_VISIBLE | SS_ETCHEDVERT);
    }

    return node;
}

// Creates a single input node. The value of the input node can be updated
// using `ed_data`.
//
//     float x = 1.0f;
//     ED_Node *node = ed_input(ED_FLOAT);
//     ed_data(node, &x);
ed_node *
ed_input(ed_value_type value_type, ed_rect rect)
{
    ed_node *node = ed_attach(ED_INPUT, rect);
    node->spacing = ed_style.spacing;
    node->flags = ED_TABSTOP | ED_TEXTNODE;
    node->value_type = value_type;
    memset(&node->value, 0, sizeof node->value);

    ed_attach_hwnd(node, "EDIT", "", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL);
    ed_measure_text_bounds(node);
    SetWindowSubclass(ed_hwnd(node), ed_edit_proc, 0, 0);
    SetWindowSubclass(ed_hwnd(node), ed_tabstop_proc, 1, 0);
    SetWindowSubclass(ed_hwnd(node), ed_text_proc, 2, 0);

    if (node->value_type >= ED_VALUE_TYPE_NUMBER_MIN
            && node->value_type <= ED_VALUE_TYPE_NUMBER_MAX) {
        SetWindowSubclass(ed_hwnd(node), ed_number_proc, 3, 0);
    }

    // node->value_ptr starts by pointing to the internal value storage.
    ed_data(node, node->value);
    return node;
}

// Creates an input node with a corresponding label. The width of the label
// column is determined by `ed_style.label_width`.
//
//     Label    [          ]
//
// Returns a pointer to the input node.
ed_node *
ed_input(const char *label, ed_value_type value_type, ed_rect rect)
{
    ed_begin(rect, ED_HORZ);

    if (label) {
        ed_label(label, {0, 0, ed_style.label_width, ed_style.label_height});
    }

    ed_node *node = ed_input(value_type, {0, 0, 1.0f, ed_style.input_height});
    ed_end();

    return node;
}

// Creates a string input node.
//
//     char str[256];
//     ED_Node *node = ed_string("String");
//     ed_data(node, str, sizeof(str));
ed_node *
ed_string(const char *label, ed_rect rect)
{
    return ed_input(label, ED_STRING, rect);
}

// Creates an int input node.
//
// value_min, value_max:
//   If `value_min == value_max` the input range is not constrained.
//
// fmt:
//   printf format used when formatting the number. Can be set to NULL to use
//   the default value in `ed_style.value_formats`.
//
// base:
//   Used when parsing the number. Use 8 for octal, 10 for decimal and 16 for
//   hexadecimal. If 0, the base is determined based on whether the input has a
//   "0" prefix (octal), "0x" prefix (hexadecimal), or no prefix (decimal).
ed_node *
ed_int(const char *label, int value_min, int value_max,
        const char *fmt, int base, ed_rect rect)
{
    ed_node *node = ed_input(label, ED_INT, rect);
    node->value_fmt = fmt;
    ed_write_value(int, &node->value_min, &value_min);
    ed_write_value(int, &node->value_max, &value_max);
    ed_write_value(int, &node->value[8], &base);

    return node;
}

// Creates an float input node.
//
// value_min, value_max:
//   If `value_min == value_max` the input range is not constrained.
//
// fmt:
//   printf format used when formatting the number. Can be set to NULL to use
//   the default value in `ed_style.value_formats`.
ed_node *
ed_float(const char *label, float value_min, float value_max,
        const char *fmt, ed_rect rect)
{
    ed_node *node = ed_input(label, ED_FLOAT, rect);
    node->value_fmt = fmt;
    ed_write_value(float, &node->value_min, &value_min);
    ed_write_value(float, &node->value_max, &value_max);

    return node;
}

// Creates a 64 bit int input node.
//
// value_min, value_max:
//   If `value_min == value_max` the input range is not constrained.
//
// fmt:
//   printf format used when formatting the number. Can be set to NULL to use
//   the default value in `ed_style.value_formats`.
//
// base:
//   Used when parsing the number. Use 8 for octal, 10 for decimal and 16 for
//   hexadecimal. If 0, the base is determined based on whether the input has a
//   "0" prefix (octal), "0x" prefix (hexadecimal), or no prefix (decimal).
ed_node *
ed_int64(const char *label, long long value_min, long long value_max,
        const char *fmt, int base, ed_rect rect)
{
    ed_node *node = ed_input(label, ED_INT64, rect);
    node->value_fmt = fmt;
    ed_write_value(long long, &node->value_min, &value_min);
    ed_write_value(long long, &node->value_max, &value_max);
    ed_write_value(int, &node->value[8], &base);

    return node;
}

// Creates a double input node.
//
// value_min, value_max:
//   If `value_min == value_max` the input range is not constrained.
//
// fmt:
//   printf format used when formatting the number. Can be set to NULL to use
//   the default value in `ed_style.value_formats`.
ed_node *
ed_float64(const char *label, double value_min, double value_max,
        const char *fmt, ed_rect rect)
{
    ed_node *node = ed_input(label, ED_FLOAT64, rect);
    node->value_fmt = fmt;
    ed_write_value(double, &node->value_min, &value_min);
    ed_write_value(double, &node->value_max, &value_max);

    return node;
}

// Creates a dropdown combobox. The result of `ed_data` corresponds to the
// index of the selected item in the dropdown list.
ed_node *
ed_enum(const char *label, const char **items, size_t items_count, ed_rect rect)
{
    ed_begin(rect, ED_HORZ);
    ed_label(label, {0, 0, ed_style.label_width, ed_style.label_height});

    ed_node *node = ed_attach(ED_COMBOBOX, {0, 0, 1.0f - ed_style.label_width, 0});
    node->flags = ED_TABSTOP;
    node->value_type = ED_ENUM;
    node->bounds.h = (short)(ed_style.label_height * items_count);

    int value = -1;
    ed_write_value(int, &node->value, &value);
    ed_end();

    ed_attach_hwnd(node, "COMBOBOX", label, WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST);
    SetWindowSubclass(ed_hwnd(node), ed_tabstop_proc, 1, 0);

    for (size_t i = 0; i < items_count; ++i) {
        SendMessage(ed_hwnd(node), CB_ADDSTRING, 0, (LPARAM)items[i]);
    }

    return node;
}

// Creates a dropdown combobox where each element toggles a bit in a int flag.
ed_node *
ed_flags(const char *label, const char **items, size_t items_count, ed_rect rect)
{
    ed_begin(rect, ED_HORZ);
    ed_label(label, {0, 0, ed_style.label_width, ed_style.label_height});

    ed_node *node = ed_attach(ED_COMBOBOX, {0, 0, 1.0f - ed_style.label_width, 0});
    node->flags = ED_TABSTOP;
    node->value_type = ED_FLAGS;
    node->bounds.h = (short)(ed_style.label_height * items_count);

    int value = 0;
    ed_write_value(int, &node->value, &value);
    ed_end();

    ed_attach_hwnd(node, "COMBOBOX", label,
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED);
    SetWindowSubclass(ed_hwnd(node), ed_tabstop_proc, 1, 0);

    for (size_t i = 0; i < items_count; ++i) {
        SendMessage(ed_hwnd(node), CB_ADDSTRING, 0, (LPARAM)items[i]);
    }

    return node;
}

// Creates a checkbox node. Initially the checkbox will be in an indeterminate
// state until `ed_data` is called to initialize the checkbox with a value.
ed_node *
ed_bool(const char *label, ed_rect rect)
{
    ed_begin(rect, ED_HORZ);
    ed_label(label, {0, 0, ed_style.label_width, ed_style.label_height});

    ed_node *node = ed_attach(ED_CHECKBOX, {0, 0, 1.0f - ed_style.label_width, 0});
    node->flags = ED_TABSTOP;
    node->value_type = ED_BOOL;
    memset(&node->value, BST_INDETERMINATE, 1);
    ed_end();

    ed_attach_hwnd(node, "BUTTON", NULL, WS_CHILD | WS_VISIBLE | BS_3STATE);
    SetWindowSubclass(ed_hwnd(node), ed_tabstop_proc, 1, 0);
    SendMessage(ed_hwnd(node), BM_SETCHECK, BST_INDETERMINATE, 0);

    return node;
}

// Creates a multiline textbox.
ed_node *
ed_text(const char *label, ed_rect rect)
{
    ed_node *vert = ed_begin(rect, ED_HORZ);
    vert->spacing = ed_style.spacing;
    if (label) {
        ed_label(label, {0, 0, ed_style.label_width, ed_style.label_height});
    }

    ed_node *node = ed_attach(ED_INPUT, {0, 0, 1.0f, 1.0f});
    node->flags = ED_TABSTOP | ED_TEXTNODE;
    node->value_type = ED_STRING;
    node->spacing = ed_style.spacing;
    ed_end();

    ed_attach_hwnd(node, "EDIT", "", WS_CHILD | WS_VISIBLE | WS_VSCROLL
            | WS_BORDER | ES_AUTOVSCROLL | ES_MULTILINE | ES_WANTRETURN);

    SetWindowSubclass(ed_hwnd(node), ed_edit_proc, 0, 0);
    SetWindowSubclass(ed_hwnd(node), ed_tabstop_proc, 1, 0);

    ed_data(node, node->value);
    return node;
}

// Creates a vector with n input fields.
//
//     float vec[] = {1, 2, 3, 4};
//     ed_data(ed_vector("A", ED_FLOAT, 4), vec);
//
// A      X [1.000]
//        Y [2.000]
//        Z [3.000]
//        W [4.000]
//
// If n>4 the the labels correspond to the element index in the array.
//
//     int nums = {1, 2, 3, 4, 5};
//     ed_data(ed_vector("B", ED_INT, 5), nums);
//
// B      0 [1]
//        1 [2]
//        2 [3]
//        3 [4]
//        4 [5]
//
// Returns a pointer to the first input node (X or element 0), which is a list
// of subsequent nodes (`next_value_node`). Only the node returned should be
// used as an argument to `ed_data`.
ed_node *
ed_vector(const char *label, ed_value_type value_type, size_t n, ed_rect rect)
{
    static const char* item_labels[4] = {"X", "Y", "Z", "W"};
    const int font_width = 10;
    float item_label_width = (float)((int)(log10f((float)n) + 1.0f) * font_width);
    ed_node *first = NULL;
    ed_node *last = NULL;

    ed_begin(rect, ED_HORZ);
    if (label) ed_label(label, {0, 0, ed_style.label_width, ed_style.label_height});
    ed_begin({0, 0, 1.0f, 0}, ED_VERT);

    for (size_t i = 0; i < n; ++i) {
        ed_begin({0, 0, 1.0f, 0}, ED_HORZ);

        if (n < ARRAYSIZE(item_labels)) {
            ed_label(item_labels[i], {0, 0, item_label_width, ed_style.label_height});
        } else {
            char item_label[16];
            snprintf(item_label, sizeof item_label, "%zu", i);
            ed_label(item_label, {0, 0, item_label_width, ed_style.label_height});
        }

        ed_node *node = ed_input(value_type, {0, 0, 1.0f, ed_style.input_height});
        if (i == 0) {
            first = node;
            last = node;
        } else {
            last->node_list = node;
            last = node;
        }
        ed_end();
    }

    ed_end();
    ed_end();
    return first;
}

// Creates a grid of m rows and n columns of input fields. `ed_data` populates
// the matrix in column-major order.
//
// transpose:
//   Uses the transpose of the matrix producing a nxm matrix. This can be used
//   to switch to a row-major layout instead of the default column-major order
//   when populating the matrix with `ed_data`.
//
//     float mat[] = {1, 2, 3, 4}
//
//     ed_data(ed_matrix("A", ED_FLOAT, 2, 2), mat);
//     // [ 1, 3,
//     //   2, 4 ]
//
//     ed_data(ed_matrix("B", ED_FLOAT, 2, 2, /* transpose: */ true), mat);
//     // [ 1, 2,
//     //   3, 4 ]
//
// value_size:
//   Size in bytes of each element. If 0, a default size is chosen depending
//   on the `value_type` argument.
//
// Returns a pointer to the first input node, which is a list of subsequent
// nodes (`next_value_node`). Only the node returned should be used as an
// argument to `ed_data`.
ed_node *
ed_matrix(const char *label, ed_value_type value_type, size_t m, size_t n,
        bool transpose, ed_rect rect)
{
    ed_node *first = NULL;
    ed_node *last = NULL;

    ed_begin(rect, ED_HORZ);
    if (label) ed_label(label, {0, 0, ed_style.label_width, ed_style.label_height});

    if (transpose) {
        float input_w = 1.0f / (float)m;
        ed_begin(rect, ED_VERT);
        for (size_t i = 0; i < n; ++i) {
            ed_begin({0, 0, 1.0f, 0}, ED_HORZ);
            for (size_t j = 0; j < m; ++j) {
                ed_node *node = ed_input(value_type, {0, 0, input_w, ed_style.input_height});

                if (first) {
                    last->node_list = node;
                    last = node;
                } else {
                    first = node;
                    last = node;
                }
            }
            ed_end();
        }
    } else {
        float input_w = 1.0f / (float)n;
        ed_begin(rect, ED_HORZ);
        for (size_t i = 0; i < n; ++i) {
            ed_begin({0, 0, input_w, 0}, ED_VERT)->spacing = ed_style.spacing;
            for (size_t j = 0; j < m; ++j) {
                ed_node *node = ed_input(value_type, {0, 0, 1.0f, ed_style.input_height});

                if (first) {
                    last->node_list = node;
                    last = node;
                } else {
                    first = node;
                    last = node;
                }
            }
            ed_end();
        }
    }

    ed_end();
    ed_end();
    return first;
}

ed_node *
ed_color(const char *label, ed_rect rect)
{
    ed_begin(rect, ED_HORZ);

    if (label) {
        ed_label(label, {0, 0, ed_style.label_width, ed_style.label_height});
    }

    ed_node *node = ed_attach(ED_BUTTON, {0, 0, 1.0f, ed_style.input_height});
    node->spacing = ed_style.spacing;
    node->flags = ED_TABSTOP;
    node->value_type = ED_COLOR;
    node->value_ptr = node->value;

    ed_attach_hwnd(node, "ED_COLOR", "", WS_CHILD | WS_VISIBLE);
    SetWindowSubclass(ed_hwnd(node), ed_tabstop_proc, 1, 0);

    ed_end();
    return node;
}

// Creates a static image node by loading a .bmp or .ico image from a file.
//
// `node->value_ptr` points to the created HBITMAP or HICON.
//
// filename:
//   Bitmap (.bmp) or icon (.ico) file. The image type is determined from the
//   file extension (defaults to bitmap).
//
// rect:
//   If `w` or `h` is zero they will be set to match the loaded image
//   dimensions. Otherwise the image is scaled to fit the rectangle.
ed_node *
ed_image(const char *filename, ed_rect rect)
{
    ed_node *node = ed_attach(ED_IMAGE, rect);
    node->spacing = ed_style.spacing;
    node->flags = ED_OWNDATA;
    ed_load_image(node, filename);

    if (node->value_type == ED_ICON) {
        ed_attach_hwnd(node, "STATIC", NULL, WS_CHILD | WS_VISIBLE | SS_ICON);
        SendMessage(ed_hwnd(node), STM_SETIMAGE, IMAGE_ICON, (LPARAM)node->value_ptr);
    } else {
        ed_attach_hwnd(node, "STATIC", NULL, WS_CHILD | WS_VISIBLE | SS_BITMAP);
        SendMessage(ed_hwnd(node), STM_SETIMAGE, IMAGE_BITMAP, (LPARAM)node->value_ptr);
    }

    return node;
}

// Creates a dynamic image from a pixel buffer. The image can be updated by
// passing the new buffer to `ed_data`. The modified buffer must be the same
// dimensions and format of the allocated buffer.
//
// `node->value_ptr` points to the created HBITMAP.
//
// image:
//   A buffer with format `fmt` or NULL. The size of the buffer must be
//   w * h * 3 for RGB buffers and w * h * 4 for buffers with an alpha
//   component.
//
//   If NULL the image buffer is allocated but nothing will be drawn.
//
// w, h:
//   Dimensions of the source image. If negative the image will be mirrored
//   (0, 0) is on the bottom-left. For a top-down image, use a negative value
//   for the height.
//
// fmt:
//   Format of the source buffer.
//
// rect:
//   If `w` or `h` is zero they will be set to match the loaded image
//   dimensions. Otherwise the image is scaled to fit the rectangle.
ed_node *
ed_image(const unsigned char *image, int w, int h, ed_pixel_format fmt, ed_rect rect)
{
    if (rect.w == 0) rect.w = (float)ed_abs(w);
    if (rect.h == 0) rect.h = (float)ed_abs(h);

    ed_node *node = ed_attach(ED_IMAGE, rect);
    node->spacing = ed_style.spacing;
    node->flags = ED_OWNDATA;
    node->value_type = ED_DIB;
    ed_alloc_bitmap_buffer(node, image, w, h, fmt);

    ed_attach_hwnd(node, "ED_IMAGE", NULL, WS_CHILD | WS_VISIBLE);
    return node;
}

// Creates a static image button node by loading a .bmp or .ico image from a
// file.
//
// `node->value_ptr` points to the created HBITMAP or HICON.
//
// filename:
//   Bitmap (.bmp) or icon (.ico) file. The image type is determined from the
//   file extension (defaults to bitmap).
//
// rect:
//   If `w` or `h` is zero they will be set to match the loaded image
//   dimensions. Otherwise the image is scaled to fit the rectangle.
ed_node *ed_image_button(const char *filename,
        void (*onclick)(ed_node *node), ed_rect rect)
{
    ed_node *node = ed_attach(ED_BUTTON, rect);
    node->onclick = onclick;
    node->spacing = ed_style.spacing;
    node->flags = ED_OWNDATA | ED_TABSTOP;
    ed_load_image(node, filename);

    if (node->value_type == ED_ICON) {
        ed_attach_hwnd(node, "BUTTON", NULL,
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_ICON);
        SendMessage(ed_hwnd(node), BM_SETIMAGE, IMAGE_ICON, (LPARAM)node->value_ptr);
    } else {
        ed_attach_hwnd(node, "BUTTON", NULL,
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_BITMAP);
        SendMessage(ed_hwnd(node), BM_SETIMAGE, IMAGE_BITMAP, (LPARAM)node->value_ptr);
    }

    SetWindowSubclass(ed_hwnd(node), ed_tabstop_proc, 1, 0);
    return node;
}

// Creates a dynamic image button from a pixel buffer. The image can be updated
// by passing the new buffer to `ed_data`. The modified buffer must be the same
// dimensions and format of the allocated buffer.
//
// `node->value_ptr` points to the created HBITMAP.
//
// image:
//   A buffer with format `fmt` or NULL. The size of the buffer must be
//   w * h * 3 for RGB buffers and w * h * 4 for buffers with an alpha
//   component.
//
//   If NULL the image buffer is allocated but nothing will be drawn.
//
// w, h:
//   Dimensions of the source image. If negative the image will be mirrored
//   (0, 0) is on the bottom-left. For a top-down image, use a negative value
//   for the height.
//
// fmt:
//   Format of the source buffer.
//
// rect:
//   If `w` or `h` is zero they will be set to match the loaded image
//   dimensions. Otherwise the image is scaled to fit the rectangle.
ed_node *
ed_image_button(const unsigned char *image, int w, int h, ed_pixel_format fmt,
        void (*onclick)(ed_node *node), ed_rect rect)
{
    ed_node *node = ed_begin_button(onclick, rect);
    ed_image(image, w, h, fmt, rect);
    ed_end();
    return node;
}

// Copies an image from a source bitmap to a premultiplied alpha BGRA bitmap.
//
// node:
//   Image node with allocated DIB.
//
// src:
//   A buffer with format `fmt`. The size of the buffer must be w * h * 3 for
//   RGB buffers and w * h * 4 for buffers with an alpha component.
void
ed_image_buffer_copy(ed_node *node, const unsigned char *src)
{
    constexpr float one_over_255 = 1.0f / 255.0f;

    assert(node->value_type == ED_DIB);
    assert(node->value_dib_image);

    ed_bitmap_buffer buffer = ed_read_value(ed_bitmap_buffer, node->value);
    unsigned char *dst = node->value_dib_image;
    size_t buffer_size = buffer.w * buffer.h * ED_BITMAP_BYTESPERPIXEL;

    switch (buffer.fmt) {
    case ED_RGB:
        for (size_t i = 0, j = 0; i < buffer_size; i += 4, j += 3) {
            dst[i + 0] = src[j + 2];
            dst[i + 1] = src[j + 1];
            dst[i + 2] = src[j + 0];
            dst[i + 3] = 0xFF;
        }
        break;
    case ED_BGR:
        for (size_t i = 0, j = 0; i < buffer_size; i += 4, j += 3) {
            dst[i + 0] = src[j + 0];
            dst[i + 1] = src[j + 1];
            dst[i + 2] = src[j + 2];
            dst[i + 3] = 0xFF;
        }
        break;
    case ED_ARGB:
        for (size_t i = 0; i < buffer_size; i += 4) {
            unsigned char a = src[i];
            float af = a * one_over_255;
            dst[i + 0] = (unsigned char)(src[i + 3] * af);
            dst[i + 1] = (unsigned char)(src[i + 2] * af);
            dst[i + 2] = (unsigned char)(src[i + 1] * af);
            dst[i + 3] = a;
        }
        break;
    case ED_RGBA:
        for (size_t i = 0; i < buffer_size; i += 4) {
            unsigned char a = src[i + 3];
            float af = a * one_over_255;
            dst[i + 0] = (unsigned char)(src[i + 2] * af);
            dst[i + 1] = (unsigned char)(src[i + 1] * af);
            dst[i + 2] = (unsigned char)(src[i + 0] * af);
            dst[i + 3] = a;
        }
        break;
    case ED_ABGR:
        for (size_t i = 0; i < buffer_size; i += 4) {
            unsigned char a = src[i];
            float af = a * one_over_255;
            dst[i + 0] = (unsigned char)(src[i + 1] * af);
            dst[i + 1] = (unsigned char)(src[i + 2] * af);
            dst[i + 2] = (unsigned char)(src[i + 3] * af);
            dst[i + 3] = a;
        }
        break;
    case ED_BGRA:
        for (size_t i = 0; i < buffer_size; i += 4) {
            unsigned char a = src[i + 3];
            float af = a * one_over_255;
            dst[i + 0] = (unsigned char)(src[i + 0] * af);
            dst[i + 1] = (unsigned char)(src[i + 1] * af);
            dst[i + 2] = (unsigned char)(src[i + 2] * af);
            dst[i + 3] = a;
        }
        break;
    }
}

// Clears a premultipled alpha BGRA bitmap.
//
// node:
//   Image node with allocated DIB.
void
ed_image_buffer_clear(ed_node *node,
        unsigned char r, unsigned char g, unsigned char b, unsigned char a)
{
    assert(node->value_type == ED_DIB);
    assert(node->value_dib_image);

    ed_bitmap_buffer buffer = ed_read_value(ed_bitmap_buffer, node->value);
    unsigned int *dst = (unsigned int *)node->value_dib_image;
    size_t buffer_size_pixels = buffer.w * buffer.h;

    float af = a / 255.0f;
    r = (unsigned char)(r * af);
    g = (unsigned char)(g * af);
    b = (unsigned char)(b * af);

    unsigned int bgra = (a << 24) | (r << 16) | (g << 8) | b;
    for (size_t i = 0; i < buffer_size_pixels; ++i) {
        dst[i] = bgra;
    }
}

// Returns the currently focused node, or NULL if no node has focus.
ed_node *
ed_get_focus()
{
    HWND hwnd = GetFocus();
    if (hwnd) {
        return (ed_node *)GetWindowLongPtr(hwnd, GWLP_USERDATA);
    }

    return NULL;
}

// Returns true if the mouse is over an editor node.
//
// node:
//   if NULL, this function returns true if the mouse is over *any* editor
//   node.
bool
ed_is_mouse_over(ed_node *node)
{
    POINT cursor;
    GetCursorPos(&cursor);

    if (node) {
        return ed_is_point_over(node, cursor);
    }

    ed_node *root = ed_index_node(ED_ID_ROOT);
    for (ed_node *c = root->child; c; c = c->after) {
        if (ed_is_point_over(c, cursor)) {
            return true;
        }
    }

    return false;
}

bool
ed_is_visible(ed_node *node)
{
    return IsWindowVisible(ed_hwnd(node));
}

bool
ed_is_enabled(ed_node *node)
{
    return IsWindowEnabled(ed_hwnd(node));
}

// Focuses on a node. If the node is inside a scroll block, the block is
// scrolled to make the node visible.
void
ed_set_focus(ed_node *node)
{
    ed_node *client = NULL;
    for (ed_node *p = node->parent; p; p = p->parent) {
        if (p->scroll_bar) {
            client = p;
            break;
        }
    }

    if (client) {
        RECT rect;
        GetWindowRect(ed_hwnd(node), &rect);
        MapWindowPoints(HWND_DESKTOP, ed_hwnd(client), (LPPOINT)&rect, 2);

        if (client->scroll_bar) {
            if (rect.top < 0) {
                int delta = rect.top - ed_style.spacing;
                ed_set_scroll_position(client, client->scroll_pos + delta);
            } else if (rect.bottom > client->dst.h) {
                int delta = (rect.bottom + ed_style.spacing) - client->dst.h;
                ed_set_scroll_position(client, client->scroll_pos + delta);
            }
        }
    }

    SetFocus(ed_hwnd(node));
}

// Removes focus by setting focus to the nearest parent window.
void
ed_reset_focus(ed_node *node)
{
    ed_node *window = ed_index_node(ED_ID_ROOT);
    for (ed_node *p = node->parent; p; p = p->parent) {
        if (p->type == ED_WINDOW || p->type == ED_USERWINDOW) {
            window = p;
            break;
        }
    }

    SetFocus(ed_hwnd(window));
}

// Shows a window. The state of the window can be queried with IsWindowVisible.
void
ed_show(ed_node *node)
{
    ShowWindow(ed_hwnd(node), SW_SHOW);
}

// Hides a window. The state of the window can be queried with IsWindowVisible.
void
ed_hide(ed_node *node)
{
    ShowWindow(ed_hwnd(node), SW_HIDE);
}

// Enables the node and its children. The state of the window can be queried
// with IsWindowEnabled.
void
ed_enable(ed_node *node)
{
    EnableWindow(ed_hwnd(node), TRUE);
    for (ed_node *c = node->child; c; c = c->after) {
        ed_enable(c);
    }
}

// Disables the node and its children. The state of the window can be queried
// with IsWindowEnabled.
void
ed_disable(ed_node *node)
{
    EnableWindow(ed_hwnd(node), FALSE);
    for (ed_node *c = node->child; c; c = c->after) {
        ed_disable(c);
    }
}

// Expands a collapsed node by making all of its children visible.
//
// node:
//   Must have a ED_HORZ or ED_VERT layout.
void
ed_expand(ed_node *node)
{
    assert(node->layout != ED_ABS
            && "node must have a vertical or horizontal layout to be expanded.");

    node->flags &= ~ED_COLLAPSED;
    if (node->child) {
        for (ed_node *c = node->child->after; c; c = c->after) {
            ShowWindow(ed_hwnd(c), SW_SHOW);
        }

        ed_invalidate(node->parent);
    }
}

// Collapses a node by hiding all of its children, except the first child.
//
// node:
//   Must have a ED_HORZ or ED_VERT layout.
void
ed_collapse(ed_node *node)
{
    assert(node->layout != ED_ABS
            && "node must have a vertical or horizontal layout to be collapsed.");

    node->flags |= ED_COLLAPSED;
    if (node->child) {
        for (ed_node *c = node->child->after; c; c = c->after) {
            ShowWindow(ed_hwnd(c), SW_HIDE);
        }

        ed_invalidate(node->parent);
    }
}

// Makes an node readonly. The pointer passed to ed_data is guaranteed to not
// be written to by the library if the node has the ED_READONLY flag.
void
ed_readonly(ed_node *node)
{
    node->flags |= ED_READONLY;
    SendMessage(ed_hwnd(node), EM_SETREADONLY, TRUE, 0);

    if (node->node_list) {
        ed_readonly(node->node_list);
    }
}

// Makes a node writable. The pointer passed to ed_data may be written to using
// user input.
void
ed_readwrite(ed_node *node)
{
    node->flags &= ~ED_READONLY;
    SendMessage(ed_hwnd(node), EM_SETREADONLY, FALSE, 0);

    if (node->node_list) {
        ed_readwrite(node->node_list);
    }
}
