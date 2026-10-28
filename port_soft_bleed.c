#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <keyboard.h>
#include <hpet.h>
#include <fcntl.h>
#include <syscalls.h>

// the crime-scene window library, this port only runs as a window on the desktop
#include <crime/crime.h>

#include "quake2.h"
#include "quakegeneric.h"
#include "client/keys.h"
#include "ref_soft/r_local.h"

void Cbuf_AddText(char *text);
void Cbuf_Execute(void);

typedef struct {
    uint8_t b;
    uint8_t g;
    uint8_t r;
    uint8_t a;
} color_t;

#define KEYQUEUE_SIZE 64
#define HPET_DEVICE_PATH "/dev/hpet"

// the window opens at this size, the game picture is stretched to whatever it is after that
#define WINDOW_WIDTH  640
#define WINDOW_HEIGHT 480

// modes this narrow or less get a window twice their size
#define SMALL_MODE_WIDTH 400

static color_t g_colors[256];
static uint32_t g_palette_xrgb8888[256];

static cs_window_t *g_window = NULL;

static int g_width = 640;
static int g_height = 480;
static char *g_screen_memory = NULL;

static unsigned short g_key_queue[KEYQUEUE_SIZE];
static unsigned int g_key_queue_write_index = 0;
static unsigned int g_key_queue_read_index = 0;

static int g_mouse_captured = 0;
static int g_mouse_dx = 0;
static int g_mouse_dy = 0;
static uint32_t g_old_mouse_buttons = 0;
static int g_hpet_fd = -2;

static uint64_t hpet_now_femtoseconds(void) {
    uint64_t now = 0;

    if (g_hpet_fd == -2)
        g_hpet_fd = _open(HPET_DEVICE_PATH, O_RDONLY);

    if (g_hpet_fd < 0)
        return 0;

    if (_ioctl(g_hpet_fd, HPET_IOCTL_GET_FEMTOSECONDS, &now) == 0)
        return now;

    if (_read(g_hpet_fd, &now, sizeof(now)) == (long)sizeof(now))
        return now;

    return 0;
}

static void apply_new_keybinds(void) {
    Cbuf_AddText("set freelook 1\n");
    Cbuf_AddText("set lookspring 0\n");
    Cbuf_AddText("set in_mouse 1\n");
    Cbuf_AddText("bind w \"+forward\"\n");
    Cbuf_AddText("bind s \"+back\"\n");
    Cbuf_AddText("bind a \"+moveleft\"\n");
    Cbuf_AddText("bind d \"+moveright\"\n");
    Cbuf_AddText("bind UPARROW \"+lookup\"\n");
    Cbuf_AddText("bind DOWNARROW \"+lookdown\"\n");
    Cbuf_AddText("bind LEFTARROW \"+left\"\n");
    Cbuf_AddText("bind RIGHTARROW \"+right\"\n");
    Cbuf_AddText("bind CTRL \"+attack\"\n");
    Cbuf_AddText("bind MOUSE1 \"+attack\"\n");
    Cbuf_AddText("bind MOUSE2 \"+mlook\"\n");
    Cbuf_AddText("bind SPACE \"+use\"\n");
    Cbuf_AddText("bind SHIFT \"+speed\"\n");
    Cbuf_AddText("bind ALT \"+strafe\"\n");
    Cbuf_AddText("bind e \"invuse\"\n");
    Cbuf_Execute();
}

static unsigned char map_key(const keyboard_event_t *ev) {
    switch (ev->keycode)
    {
        case Escape:         return K_ESCAPE;
        case CarriageReturn: return K_ENTER;
        case LineFeed:       return K_ENTER;
        case FileSeparator:  return K_ENTER;
        case HorizontalTab:  return K_TAB;
        case Backspace:      return K_BACKSPACE;
        case ArrowLeft:      return K_LEFTARROW;
        case ArrowRight:     return K_RIGHTARROW;
        case ArrowUp:        return K_UPARROW;
        case ArrowDown:      return K_DOWNARROW;
        case Home:           return K_HOME;
        case End:            return K_END;
        case Insert:         return K_INS;
        case Delete:         return K_DEL;
        case PageUp:         return K_PGUP;
        case PageDown:       return K_PGDN;
        case F1:             return K_F1;
        case F2:             return K_F2;
        case F3:             return K_F3;
        case F4:             return K_F4;
        case F5:             return K_F5;
        case F6:             return K_F6;
        case F7:             return K_F7;
        case F8:             return K_F8;
        case F9:             return K_F9;
        case F10:            return K_F10;
        case F11:            return K_F11;
        case F12:            return K_F12;
        case GroupSeparator: return K_CTRL;
        case 42:             return K_SHIFT;  /* left shift */
        case 54:             return K_SHIFT;
        case 56:             return K_ALT;
        default:             break;
    }

    {
        char ascii = tty_key_to_ascii(ev);
        if (ascii >= 'A' && ascii <= 'Z')
            ascii = (char)(ascii + ('a' - 'A'));

        if (ascii > 0)
            return (unsigned char)ascii;
    }

    return 0;
}

static void add_key_to_queue(int pressed, unsigned char key) {
    unsigned int next_write_index = (g_key_queue_write_index + 1U) % KEYQUEUE_SIZE;
    unsigned short key_data = (unsigned short)((pressed << 8) | key);

    if (next_write_index == g_key_queue_read_index)
    {
        /* Keep the newest event when the queue overflows. */
        g_key_queue_read_index = (g_key_queue_read_index + 1U) % KEYQUEUE_SIZE;
    }

    g_key_queue[g_key_queue_write_index] = key_data;
    g_key_queue_write_index = next_write_index;
}

static void setup_video_buffer(void) {
    if (g_screen_memory)
    {
        free(g_screen_memory);
        g_screen_memory = NULL;
    }

    g_screen_memory = (char *)malloc((size_t)g_width * (size_t)g_height);
    if (!g_screen_memory)
        Sys_Error("out of memory allocating software framebuffer\n");

    vid.rowbytes = g_width;
    vid.buffer = (pixel_t *)g_screen_memory;
}

static void mouse_buttons(uint32_t buttons) {
    static const uint32_t button_masks[] = {
        CS_BUTTON_LEFT,
        CS_BUTTON_RIGHT,
        CS_BUTTON_MIDDLE
    };
    uint32_t changed = buttons ^ g_old_mouse_buttons;
    int i;

    for (i = 0; i < (int)(sizeof(button_masks) / sizeof(button_masks[0])); ++i)
    {
        if (changed & button_masks[i])
            Quake2_SendKey(K_MOUSE1 + i, (buttons & button_masks[i]) ? true : false);
    }

    g_old_mouse_buttons = buttons;
}

// take everything the desktop sent since last time. keys are queued for handle_input
static void pump_events(void) {
    cs_event_t ev;

    if (!g_window)
        return;

    while (cs_event_wait(&ev, 0))
    {
        switch (ev.type)
        {
            case CS_EV_KEY: {
                keyboard_event_t key_event = {
                    .keycode = ev.keycode,
                    .action  = ev.key_action,
                    .keymod  = ev.keymod,
                };
                unsigned char key = map_key(&key_event);
                if (key)
                    add_key_to_queue(key_event.action == KEY_DOWN ? 1 : 0, key);
                break;
            }

            // only sent while the pointer is locked to us, how far the mouse moved
            case CS_EV_MOUSE_RELATIVE:
                if (g_mouse_captured)
                {
                    g_mouse_dx += ev.x;
                    g_mouse_dy += ev.y;
                    mouse_buttons(ev.buttons);
                }
                break;

            case CS_EV_CLOSE_REQUEST:
                cs_window_close(g_window);
                exit(0);
                break;

            // a resize needs nothing doing here, the next frame is stretched to the new size
            default:
                break;
        }
    }
}

rserr_t SWimp_SetMode(int *pwidth, int *pheight, int mode, qboolean fullscreen) {
    int width = 0;
    int height = 0;

    (void)fullscreen;

    ri.Con_Printf(PRINT_ALL, "Initializing software display\n");
    ri.Con_Printf(PRINT_ALL, "setting mode %d:", mode);

    if (!ri.Vid_GetModeInfo(&width, &height, mode))
    {
        ri.Con_Printf(PRINT_ALL, " invalid mode\n");
        return rserr_invalid_mode;
    }

    ri.Con_Printf(PRINT_ALL, " %d %d\n", width, height);

    g_width = width;
    g_height = height;
    *pwidth = width;
    *pheight = height;

    setup_video_buffer();

    if (g_window)
    {
        int scale = (width <= SMALL_MODE_WIDTH) ? 2 : 1;
        cs_window_resize(g_window, width * scale, height * scale);
    }

    ri.Vid_NewWindow(width, height);
    return rserr_ok;
}

void SWimp_Shutdown(void) {
    if (g_screen_memory)
    {
        free(g_screen_memory);
        g_screen_memory = NULL;
    }

    g_key_queue_read_index = 0;
    g_key_queue_write_index = 0;
    g_mouse_dx = 0;
    g_mouse_dy = 0;
    g_old_mouse_buttons = 0;
    g_mouse_captured = 0;
}

// the window itself is opened in main before any of this, it outlives a renderer restart
int SWimp_Init(void *hInstance, void *wndProc) {
    (void)hInstance;
    (void)wndProc;

    if (!g_window)
    {
        Sys_Error("no window to draw into\n");
        return false;
    }

    setup_video_buffer();
    return true;
}

void SWimp_SetPalette(const unsigned char *palette) {
    int i;

    for (i = 0; i < 256; ++i)
    {
        g_colors[i].r = *palette++;
        g_colors[i].g = *palette++;
        g_colors[i].b = *palette++;
        g_colors[i].a = 255;
        palette++;

        g_palette_xrgb8888[i] =
            0xFF000000u |
            ((uint32_t)g_colors[i].r << 16) |
            ((uint32_t)g_colors[i].g << 8) |
            (uint32_t)g_colors[i].b;
    }
}

void SWimp_BeginFrame(float camera_seperation) {
    (void)camera_seperation;
}

// one row of the game picture through the palette, each pixel repeated scale times
static void convert_row(uint32_t *dst, const uint8_t *src, int width, int scale) {
    int x;

    if (scale == 1)
    {
        for (x = 0; x < width; ++x)
            dst[x] = g_palette_xrgb8888[src[x]];
        return;
    }

    for (x = 0; x < width; ++x)
    {
        uint32_t colour = g_palette_xrgb8888[src[x]];
        int i;
        for (i = 0; i < scale; ++i)
            *dst++ = colour;
    }
}

void SWimp_EndFrame(void) {
    static uint32_t *x_map = NULL;
    static int x_map_width = 0;
    static int x_map_source = 0;
    int x;
    int y;
    int out_w;
    int out_h;
    int scale;
    size_t row_bytes;

    if (!g_window || !g_screen_memory)
        return;

    out_w = g_window->width;
    out_h = g_window->height;
    row_bytes = (size_t)out_w * sizeof(uint32_t);

    scale = out_w / g_width;
    if (scale >= 1 && out_w == g_width * scale && out_h == g_height * scale)
    {
        for (y = 0; y < g_height; ++y)
        {
            const uint8_t *src = (const uint8_t *)g_screen_memory + (size_t)y * (size_t)g_width;
            uint32_t *dst = g_window->pixels + (size_t)(y * scale) * (size_t)g_window->pitch;
            int i;

            // the first of the repeated rows is worked out, the rest are just copies of it
            convert_row(dst, src, g_width, scale);
            for (i = 1; i < scale; ++i)
                memcpy(dst + (size_t)i * (size_t)g_window->pitch, dst, row_bytes);
        }
    }
    else
    {
        int last_src_y = -1;

        if (out_w != x_map_width || g_width != x_map_source)
        {
            if (x_map)
                free(x_map);
            x_map = (uint32_t *)malloc((size_t)out_w * sizeof(uint32_t));
            if (!x_map)
                return;

            for (x = 0; x < out_w; ++x)
                x_map[x] = (uint32_t)(((uint64_t)x * (uint64_t)g_width) / (uint64_t)out_w);

            x_map_width = out_w;
            x_map_source = g_width;
        }

        for (y = 0; y < out_h; ++y)
        {
            int src_y = (int)(((int64_t)y * g_height) / out_h);
            const uint8_t *src = (const uint8_t *)g_screen_memory + (size_t)src_y * (size_t)g_width;
            uint32_t *dst = g_window->pixels + (size_t)y * (size_t)g_window->pitch;

            // stretched taller than the picture, so this row is the one above it again
            if (src_y == last_src_y)
            {
                memcpy(dst, dst - g_window->pitch, row_bytes);
                continue;
            }
            last_src_y = src_y;

            for (x = 0; x < out_w; ++x)
                dst[x] = g_palette_xrgb8888[src[x_map[x]]];
        }
    }

    cs_window_commit(g_window, 0, 0, out_w, out_h);
    pump_events();
}

void SWimp_AppActivate(qboolean active) {
    (void)active;
}

static void handle_input(void) {
    pump_events();

    while (g_key_queue_read_index != g_key_queue_write_index) {
        unsigned short key_data = g_key_queue[g_key_queue_read_index];
        int pressed = key_data >> 8;
        int quake_key = key_data & 0xFF;

        g_key_queue_read_index = (g_key_queue_read_index + 1U) % KEYQUEUE_SIZE;

        Quake2_SendKey(quake_key, pressed ? true : false);
    }
}

int QG_Milliseconds(void) {
    static uint64_t start_fs = 0;
    static int fallback_ms = 0;
    uint64_t now_fs = hpet_now_femtoseconds();

    if (!now_fs)
        return ++fallback_ms;

    if (!start_fs || now_fs < start_fs)
        start_fs = now_fs;

    return (int)((now_fs - start_fs) / femtosecondsPerMillisecond);
}

void QG_GetMouseDiff(int *dx, int *dy) {
    pump_events();

    *dx = g_mouse_dx;
    *dy = g_mouse_dy;
    g_mouse_dx = 0;
    g_mouse_dy = 0;
}

// the engine captures the mouse while the game is being played and lets go for the console
void QG_CaptureMouse(void) {
    if (g_mouse_captured)
        return;
    g_mouse_captured = 1;
    if (g_window)
        cs_pointer_lock(g_window, 1);
}

void QG_ReleaseMouse(void) {
    if (!g_mouse_captured)
        return;
    g_mouse_captured = 0;
    g_mouse_dx = 0;
    g_mouse_dy = 0;
    mouse_buttons(0);
    if (g_window)
        cs_pointer_lock(g_window, 0);
}

#define BASEDIR_FALLBACK "/mnt/data/quake2"
static char g_basedir[256];

const char *QG_BaseDir(void) {
    if (g_basedir[0])
        return g_basedir;

    char *slash;
    if (_taskpath((uint64_t)_getpid(), g_basedir, sizeof(g_basedir)) == 0 &&
        (slash = strrchr(g_basedir, '/')) != NULL && slash != g_basedir)
        *slash = '\0';
    else
        strcpy(g_basedir, BASEDIR_FALLBACK);
    return g_basedir;
}

int main(int argc, char **argv) {
    int i;
    int time;
    int oldtime;
    int newtime;
    int has_basedir = 0;
    int basedir_index = -1;
    char *argv_ext[128];

    if (argc + 2 < (int)(sizeof(argv_ext) / sizeof(argv_ext[0])))
    {
        for (i = 1; i < argc; ++i)
        {
            if (!strcmp(argv[i], "-basedir"))
            {
                has_basedir = 1;
                basedir_index = i + 1;
                break;
            }
        }

        if (!has_basedir)
        {
            for (i = 0; i < argc; ++i)
                argv_ext[i] = argv[i];
            argv_ext[argc] = "-basedir";
            argv_ext[argc + 1] = (char *)QG_BaseDir();
            argv_ext[argc + 2] = NULL;
            argc += 2;
            argv = argv_ext;
        }
        else if (basedir_index > 0 && basedir_index < argc) {
            argv[basedir_index] = (char *)QG_BaseDir();
        }
    }

    // before anything is loaded, theres no point reading the pak just to find nowhere to draw
    g_window = cs_window_open(WINDOW_WIDTH, WINDOW_HEIGHT, "Quake II");
    if (!g_window)
    {
        printf("CrimeScene Display Server is not running\n");
        return 1;
    }

    Quake2_Init(argc, argv);
    apply_new_keybinds();

    oldtime = Quake2_Milliseconds();
    while (1)
    {
        handle_input();

        // asleep until the next millisecond instead of spinning, the desktop has other windows to draw
        for (;;) {
            newtime = Quake2_Milliseconds();
            time = newtime - oldtime;
            if (time >= 1)
                break;
            poll(NULL, 0, 1);
        }

        Quake2_Frame(time);
        oldtime = newtime;
    }

    return 0;
}
