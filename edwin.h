#include <stddef.h>

#ifndef EDWIN_H_
#define EDWIN_H_

#define ED_VERSION 2302

#ifndef ED_TREE_MEMORY
#define ED_TREE_MEMORY (1024 << 10)
#endif

#ifndef ED_NODE_COUNT
#define ED_NODE_COUNT (ED_TREE_MEMORY / sizeof(ed_node))
#endif

#ifndef ED_UPDATE_FUNCS_COUNT
#define ED_UPDATE_FUNCS_COUNT 256
#endif

#define ED_ID_ROOT 1

#define ED_BITMAP_BYTESPERPIXEL 4
#define ED_BITMAP_BITSPERPIXEL 32

#define ed_hwnd(node) ((HWND)node->hwnd)

#define ed_read_value(Type, value) (*(Type *)(value))
#define ed_write_value(Type, dst, src) (*(Type *)(dst) = *(Type *)src)

enum ed_node_layout {
    ED_VERT,
    ED_HORZ,
    ED_ABS,
};

enum ed_node_type {
    ED_NONE,
    ED_BLOCK,
    ED_WINDOW,
    ED_USERWINDOW,
    ED_GROUP,
    ED_LABEL,
    ED_BUTTON,
    ED_SPACE,
    ED_SEPARATOR,
    ED_INPUT,
    ED_COMBOBOX,
    ED_CHECKBOX,
    ED_CAPTION,
    ED_SCROLLBLOCK,
    ED_SCROLLBAR,
    ED_IMAGE,
    ED_COLORPICKER,

    ED_NODE_TYPE_USER = 0x8000,
};

enum ed_value_type {
    ED_STRING,
    ED_INT,
    ED_FLOAT,
    ED_INT64,
    ED_FLOAT64,
    ED_ENUM,
    ED_FLAGS,
    ED_BOOL,
    ED_BITMAP,
    ED_ICON,
    ED_DIB,
    ED_COLOR,

    ED_VALUE_TYPE_NUMBER_MIN = ED_INT,
    ED_VALUE_TYPE_NUMBER_MAX = ED_FLOAT64,
    ED_VALUE_TYPE_SCALAR_MIN = ED_INT,
    ED_VALUE_TYPE_SCALAR_MAX = ED_BOOL,
    ED_VALUE_TYPE_COUNT,
};

enum ed_node_flags {
    ED_ROOT        = 0x00000001,
    ED_BORDER      = 0x00000002,
    ED_TEXTNODE    = 0x00000004,
    ED_EXPAND      = 0x00000008,
    ED_POPPARENT   = 0x00000010,
    ED_READONLY    = 0x00000020,
    ED_COLLAPSED   = 0x00000040,
    ED_EDITING     = 0x00000080,
    ED_TABSTOP     = 0x00000100,
    ED_OWNDATA     = 0x00000200,
    ED_OWNUPDATE   = 0x00000400,
};

enum ed_color {
    ED_COLOR_WINDOW,
    ED_COLOR_WINDOWTEXT,
    ED_COLOR_3DFACE,
    ED_COLOR_BTNTEXT,
    ED_COLOR_HIGHLIGHT,
    ED_COLOR_GRAYTEXT,
    ED_COLOR_HIGHLIGHTTEXT,
    ED_COLOR_COUNT,
};

enum ed_pixel_format {
    ED_RGB,
    ED_BGR,
    ED_ARGB,
    ED_RGBA,
    ED_ABGR,
    ED_BGRA,
};

struct ed_style {
    short spacing = 8;
    short padding = 8;
    short border_size = 1;
    short scrollbar_size = 15;
    short text_w_spacing = 20;
    short text_h_spacing = 10;
    short caption_height = 26;
    short scroll_sensitivity = 8;
    short scroll_unit = 15;
    short number_input_deadzone = 3;
    float label_width = 0.4f;
    float label_height = 20.0f;
    float input_height = 20.0f;
    float number_input_float_increment = 0.01f;
    double number_input_float64_increment = 0.01;

    int colors[ED_COLOR_COUNT]; // Colors are in BGR format
    const char *value_formats[ED_VALUE_TYPE_COUNT];
};

// Values between 0.0 and 1.0 represent a relative position or size, and values
// greater than 1.0 represent an exact pixel value. Most controls have sensible
// defaults assuming you are working with a vertical layout.
//
// `ed_rect{.x = 10, .y = 10, .w = 100, .h = 20`}: A 100px by 20px element
// placed at x=10px, y=10px.
//
// `{0, 0, 1.0f, 10}`: A 10px tall element taking up 100% of width of its
// parent.
//
// `{1, 0, 0.5f, 10}:` A 10px tall, right aligned element taking up 50% of
// the its parent.
//
// `{0.5f, 0.5f, 100, 0}`: A 100px wide centered element. The height will be
// determined by this element's children, or text size in the case of buttons,
// labels and inputs.
struct ed_rect {
    float x, y, w, h;
};

struct ed_dst {
    short x, y, w, h;
};

struct ed_bounds {
    short w, h;
};

struct ed_bitmap_buffer {
    ed_pixel_format fmt;
    short w, h;
};

struct ed_node {
    ed_node *parent, *child;
    ed_node *before, *after;

    void *hwnd;            // Window handle

    short id;
    short scroll_pos;      // For scroll clients, position of vertical scrollbar
    short spacing, padding;

    ed_rect rect;          // User specified position and size, may be relative to parent
    ed_dst dst;            // Final window position and size
    ed_bounds bounds;      // Space required by children, may be larger than dst
    ed_node_layout layout;
    ed_node_type type;
    int flags;

    short scroll_bar;      // For scroll clients, id of scrollbar node
    short scroll_client;   // For scrollbars, id of client node
    ed_value_type value_type;

    char value[16];        // Current number value displayed, size of string buffer, or bitmap buffer
    void *value_ptr;       // Pointer to value storage owned by the user or by the library if ED_OWNDATA is set
    size_t value_size;     // Size of storage pointed to by value_ptr
    char value_min[8];
    char value_max[8];

    union {
        // Number values: printf style format
        const char *value_fmt;

        // Image buffer values: pointer to dib image
        unsigned char *value_dib_image;
    };

    ed_node *node_list;    // List of nodes used to chain related nodes

    union {
        void (*onclick)(ed_node *node);
        void (*onchange)(ed_node *node);
    };

    void *user_data;
};

struct ed_node_update {
    ed_node *node;
    void (*update)();
};

struct ed_stats {
    // Number of calls to ed_invalidate.
    unsigned invalidate_calls;

    // Number of calls to ed_update.
    unsigned update_calls;

    // Number of calls to ed_data. Resets during each call to ed_update.
    unsigned data_calls;

    // Number of ticks (from QueryPerformanceCounter) used by calls to ed_measure
    // during the last call to ed_invalidate.
    long long measure_ticks;

    // Number of ticks (from QueryPerformanceCounter) used by calls to ed_layout
    // during the last call to ed_invalidate.
    long long layout_ticks;

    // Number of ticks (from QueryPerformanceCounter) used by calls to ed_data
    // during the last call to ed_update.
    long long update_ticks;
};

ed_node *ed_index_node(short id);
void ed_init(void *hwnd);
void ed_deinit();
void ed_register_update(ed_node *node, void (*update)());
void ed_unregister_update(ed_node *node);
void ed_update(unsigned update_every_n_frames);
void ed_apply_system_colors();
void ed_allocate_colors();
void ed_resize(void *hwnd);
void ed_invalidate(ed_node *node);
void ed_invalidate_data(ed_node *node);
void ed_data(ed_node *node, void *data, size_t size = 0);

void ed_begin_context(ed_node *node);
void ed_end_context();
void ed_insert_after(ed_node *node);
void ed_remove(ed_node *node);
void ed_attach_hwnd(ed_node *node, const char *class_name, const char *name, int flags);

// Parent controls

ed_node *ed_begin(ed_rect rect = {}, ed_node_layout layout = ED_VERT);
ed_node *ed_begin_border(ed_rect rect = {}, ed_node_layout layout = ED_VERT);
ed_node *ed_begin_scroll(ed_rect rect = {0, 0, 1.0f, 1.0f}, ed_node_layout layout = ED_VERT);
ed_node *ed_begin_window(const char *name, ed_rect rect = {}, ed_node_layout = ED_VERT);
ed_node *ed_begin_group(const char *name, ed_rect rect = {0, 0, 1.0f, 0}, ed_node_layout layout = ED_VERT);
ed_node *ed_begin_button(void (*onclick)(ed_node *node), ed_rect rect = ed_rect{});
ed_node *ed_begin_hwnd(void *hwnd, ed_node_layout layout = ED_VERT);
void ed_end();

// Basic controls

ed_node *ed_label(const char *label, ed_rect rect = {});
ed_node *ed_button(const char *label, void (*onclick)(ed_node *node), ed_rect rect = {});
ed_node *ed_space(float size);
ed_node *ed_separator();

// Input controls

ed_node *ed_input(ed_value_type type, ed_rect rect = {0, 0, 1.0f, 0});
ed_node *ed_input(const char *label, ed_value_type type, ed_rect rect = {0, 0, 1.0f, 0});
ed_node *ed_string(const char *label, ed_rect rect = {0, 0, 1.0f, 0});
ed_node *ed_int(const char *label, int value_min = 0, int value_max = 0, const char *fmt = NULL, int base = 0, ed_rect rect = {0, 0, 1.0f, 0});
ed_node *ed_float(const char *label, float value_min = 0, float value_max = 0, const char *fmt = NULL, ed_rect rect = {0, 0, 1.0f, 0});
ed_node *ed_int64(const char *label, long long value_min = 0, long long value_max = 0, const char *fmt = NULL, int base = 0, ed_rect rect = {0, 0, 1.0f, 0});
ed_node *ed_float64(const char *label, double value_min = 0.0, double value_max = 0.0, const char *fmt = NULL, ed_rect rect = {0, 0, 1.0f, 0});
ed_node *ed_enum(const char *label, const char **items, size_t items_count, ed_rect rect = {0, 0, 1.0f, 0});
ed_node *ed_flags(const char *label, const char **items, size_t items_count, ed_rect rect = {0, 0, 1.0f, 0});
ed_node *ed_bool(const char *label, ed_rect rect = {0, 0, 1.0f, 0});
ed_node *ed_text(const char *label, ed_rect rect = {0, 0, 1.0f, 150});
ed_node *ed_vector(const char *label, ed_value_type value_type, size_t n, ed_rect rect = {0, 0, 1.0f, 0});
ed_node *ed_matrix(const char *label, ed_value_type value_type, size_t m, size_t n, bool transpose = false, ed_rect rect = {0, 0, 1.0f, 0});
ed_node *ed_color(const char *label, ed_rect rect = {0, 0, 1.0f, 0});

// Image controls

ed_node *ed_image(const char *filename, ed_rect rect = {});
ed_node *ed_image(const unsigned char *image, int w, int h, ed_pixel_format fmt, ed_rect rect = {});
ed_node *ed_image_button(const char *filename, void (*onclick)(ed_node *node), ed_rect rect = {});
ed_node *ed_image_button(const unsigned char *image, int w, int h, ed_pixel_format fmt, void (*onclick)(ed_node *node), ed_rect rect = {});

void ed_image_buffer_copy(ed_node *node, const unsigned char *src);
void ed_image_buffer_clear(ed_node *node, unsigned char r, unsigned char g, unsigned char b, unsigned char a);

// Node state

ed_node *ed_get_focus();
bool ed_is_mouse_over(ed_node *node = NULL);
bool ed_is_visible(ed_node *node);
bool ed_is_enabled(ed_node *node);

// Node state changes

void ed_set_focus(ed_node *node);
void ed_reset_focus(ed_node *node);
void ed_show(ed_node *node);
void ed_hide(ed_node *node);
void ed_enable(ed_node *node);
void ed_disable(ed_node *node);
void ed_expand(ed_node *node);
void ed_collapse(ed_node *node);
void ed_readonly(ed_node *node);
void ed_readwrite(ed_node *node);

extern struct ed_style ed_style;
extern struct ed_stats ed_stats;

extern ed_node ed_tree[ED_NODE_COUNT];
extern ed_node_update ed_update_funcs[ED_UPDATE_FUNCS_COUNT];

static_assert(ED_NODE_COUNT < 0x7FFF);

#endif // EDWIN_H_
