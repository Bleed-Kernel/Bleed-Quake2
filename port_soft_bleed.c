#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <devices/keyboard.h>
#include <fcntl.h>
#include <fs/file.h>
#include <graphics/display.h>
#include <sys/ioctl.h>
#include <syscalls/close.h>
#include <syscalls/femtoseconds.h>
#include <syscalls/open.h>
#include <syscalls/read.h>

#include "quake2.h"
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

#ifndef MOUSE_BTN_LEFT
#define MOUSE_BTN_LEFT   (1 << 0)
#define MOUSE_BTN_RIGHT  (1 << 1)
#define MOUSE_BTN_MIDDLE (1 << 2)

typedef struct {
    int16_t dx;
    int16_t dy;
    int8_t wheel;
    uint8_t buttons;
} mouse_event_t;
#endif

#define KEYQUEUE_SIZE 64

#define TTY_ECHO         (1 << 1)
#define TTY_CANNONICAL   (1 << 2)
#define TTY_NONBLOCK     (1 << 4)
#define TTY_FLAGS_QUAKE  (TTY_NONBLOCK)

static color_t g_colors[256];
static uint16_t g_palette_565[256];
static uint32_t g_palette_xrgb8888[256];
static uint8_t g_palette_bgr24[256][3];

static int g_fb_fd = -1;
static uint8_t *g_fb_backbuffer = NULL;
static struct fb_info g_fb_info;
static uint64_t g_fb_pitch_bytes = 0;

static int g_tty_fd = -1;
static int g_mouse_fd = -1;
static uint32_t g_tty_restore_flags = TTY_ECHO | TTY_CANNONICAL;

static int g_width = 640;
static int g_height = 480;
static char *g_screen_memory = NULL;

static unsigned short g_key_queue[KEYQUEUE_SIZE];
static unsigned int g_key_queue_write_index = 0;
static unsigned int g_key_queue_read_index = 0;

static int g_mouse_captured = 0;
static int g_mouse_dx = 0;
static int g_mouse_dy = 0;
static uint8_t g_old_mouse_buttons = 0;

static void close_fd_if_open(int *fd) {
    if (*fd >= 0)
    {
        _close(*fd);
        *fd = -1;
    }
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

static void handle_keyboard_input(void) {
    keyboard_event_t ev;
    long read_count;

    for (;;)
    {
        read_count = _read(0, &ev, sizeof(ev));
        if (read_count != (long)sizeof(ev))
            break;

        {
            unsigned char key = map_key(&ev);
            if (key)
                add_key_to_queue(ev.action == KEY_DOWN ? 1 : 0, key);
        }
    }
}

static void handle_mouse_input(void) {
    mouse_event_t ev;
    long read_count;

    if (!g_mouse_captured || g_mouse_fd < 0)
        return;

    for (;;)
    {
        read_count = _read(g_mouse_fd, &ev, sizeof(ev));
        if (read_count != (long)sizeof(ev))
            break;

        g_mouse_dx += (int)ev.dx;
        g_mouse_dy += (int)ev.dy;

        {
            uint8_t changed = (uint8_t)(ev.buttons ^ g_old_mouse_buttons);

            if (changed & MOUSE_BTN_LEFT)
                Quake2_SendKey(K_MOUSE1, (ev.buttons & MOUSE_BTN_LEFT) ? true : false);
            if (changed & MOUSE_BTN_RIGHT)
                Quake2_SendKey(K_MOUSE2, (ev.buttons & MOUSE_BTN_RIGHT) ? true : false);
            if (changed & MOUSE_BTN_MIDDLE)
                Quake2_SendKey(K_MOUSE3, (ev.buttons & MOUSE_BTN_MIDDLE) ? true : false);

            g_old_mouse_buttons = ev.buttons;
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

    /* vbox is sensitive to larger software modes on a smaller boot FB. */
    if (g_fb_info.width && g_fb_info.height &&
        (width > (int)g_fb_info.width || height > (int)g_fb_info.height))
    {
        ri.Con_Printf(PRINT_ALL, " mode %dx%d exceeds framebuffer %llux%llu\n",
            width, height,
            (unsigned long long)g_fb_info.width,
            (unsigned long long)g_fb_info.height);
        return rserr_invalid_mode;
    }

    ri.Con_Printf(PRINT_ALL, " %d %d\n", width, height);

    g_width = width;
    g_height = height;
    *pwidth = width;
    *pheight = height;

    setup_video_buffer();

    ri.Vid_NewWindow(width, height);
    return rserr_ok;
}

void SWimp_Shutdown(void) {
    _ioctl(0, TTY_IOCTL_SET_FLAGS, &g_tty_restore_flags);
    if (g_tty_fd >= 0)
        _ioctl(g_tty_fd, TTY_IOCTL_SET_FLAGS, &g_tty_restore_flags);

    close_fd_if_open(&g_mouse_fd);
    close_fd_if_open(&g_tty_fd);
    close_fd_if_open(&g_fb_fd);

    if (g_screen_memory)
    {
        free(g_screen_memory);
        g_screen_memory = NULL;
    }

    if (g_fb_backbuffer)
    {
        free(g_fb_backbuffer);
        g_fb_backbuffer = NULL;
    }

    g_fb_pitch_bytes = 0;
    memset(&g_fb_info, 0, sizeof(g_fb_info));
    g_key_queue_read_index = 0;
    g_key_queue_write_index = 0;
    g_mouse_dx = 0;
    g_mouse_dy = 0;
    g_old_mouse_buttons = 0;
    g_mouse_captured = 0;
}

int SWimp_Init(void *hInstance, void *wndProc) {
    uint32_t tty_flags = TTY_FLAGS_QUAKE;

    (void)hInstance;
    (void)wndProc;

    g_fb_fd = _open("/dev/fb0", O_RDWR);
    if (g_fb_fd < 0)
    {
        Sys_Error("failed to open /dev/fb0\n");
        goto fail;
    }

    if (_ioctl(g_fb_fd, FB_IOC_GET_INFO, &g_fb_info) < 0)
    {
        Sys_Error("failed FB_IOC_GET_INFO on /dev/fb0\n");
        goto fail;
    }

    g_fb_pitch_bytes = g_fb_info.pitch;
    g_fb_backbuffer = (uint8_t *)malloc((size_t)g_fb_info.height * (size_t)g_fb_pitch_bytes);
    if (!g_fb_backbuffer)
    {
        Sys_Error("failed to allocate framebuffer backbuffer\n");
        goto fail;
    }
    memset(g_fb_backbuffer, 0, (size_t)g_fb_info.height * (size_t)g_fb_pitch_bytes);

    _ioctl(0, TTY_IOCTL_SET_FLAGS, &tty_flags);
    g_tty_fd = _open("/dev/tty0", O_RDWR);
    if (g_tty_fd >= 0)
        _ioctl(g_tty_fd, TTY_IOCTL_SET_FLAGS, &tty_flags);

    g_mouse_fd = _open("/dev/mouse0", O_RDONLY);

    setup_video_buffer();
    return true;

fail:
    SWimp_Shutdown();
    return false;
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

        g_palette_565[i] =
            (uint16_t)(((uint16_t)(g_colors[i].r >> 3) << 11) |
                       ((uint16_t)(g_colors[i].g >> 2) << 5)  |
                       ((uint16_t)(g_colors[i].b >> 3)));

        g_palette_xrgb8888[i] =
            0xFF000000u |
            ((uint32_t)g_colors[i].r << 16) |
            ((uint32_t)g_colors[i].g << 8) |
            (uint32_t)g_colors[i].b;

        g_palette_bgr24[i][0] = g_colors[i].b;
        g_palette_bgr24[i][1] = g_colors[i].g;
        g_palette_bgr24[i][2] = g_colors[i].r;
    }
}

void SWimp_BeginFrame(float camera_seperation) {
    (void)camera_seperation;
}

void SWimp_EndFrame(void) {
    int x;
    int y;
    int fbw;
    int fbh;
    int copy_width;
    int copy_height;
    int ox;
    int oy;
    int bytes_per_pixel;
    int max_pitch_pixels;
    const uint8_t *src_base;
    qboolean needs_clear;

    if (!g_fb_backbuffer || !g_screen_memory)
        return;

    fbw = (int)g_fb_info.width;
    fbh = (int)g_fb_info.height;
    bytes_per_pixel = (int)(g_fb_info.bpp / 8);
    if (bytes_per_pixel <= 0)
        bytes_per_pixel = 4;
    max_pitch_pixels = (int)(g_fb_pitch_bytes / (uint64_t)bytes_per_pixel);
    if (max_pitch_pixels < fbw)
        fbw = max_pitch_pixels;

    copy_width = g_width < fbw ? g_width : fbw;
    copy_height = g_height < fbh ? g_height : fbh;
    ox = (fbw > copy_width) ? (fbw - copy_width) / 2 : 0;
    oy = (fbh > copy_height) ? (fbh - copy_height) / 2 : 0;
    src_base = (const uint8_t *)g_screen_memory;
    needs_clear = (copy_width != fbw || copy_height != fbh) ? true : false;

    if (needs_clear)
        memset(g_fb_backbuffer, 0, (size_t)fbh * (size_t)g_fb_pitch_bytes);

    if (g_fb_info.bpp == 16)
    {
        for (y = 0; y < copy_height; ++y)
        {
            const uint8_t *src = src_base + (size_t)y * (size_t)g_width;
            uint16_t *dst = (uint16_t *)(g_fb_backbuffer + (size_t)(y + oy) * (size_t)g_fb_pitch_bytes);
            dst += ox;
            for (x = 0; x < copy_width; ++x)
            {
                dst[x] = g_palette_565[src[x]];
            }
        }
    }
    else if (g_fb_info.bpp == 24)
    {
        for (y = 0; y < copy_height; ++y)
        {
            const uint8_t *src = src_base + (size_t)y * (size_t)g_width;
            uint8_t *dst = g_fb_backbuffer + (size_t)(y + oy) * (size_t)g_fb_pitch_bytes + (size_t)ox * 3u;
            for (x = 0; x < copy_width; ++x)
            {
                const uint8_t *bgr = g_palette_bgr24[src[x]];
                dst[0] = bgr[0];
                dst[1] = bgr[1];
                dst[2] = bgr[2];
                dst += 3;
            }
        }
    }
    else
    {
        for (y = 0; y < copy_height; ++y)
        {
            const uint8_t *src = src_base + (size_t)y * (size_t)g_width;
            uint32_t *dst = (uint32_t *)(g_fb_backbuffer + (size_t)(y + oy) * (size_t)g_fb_pitch_bytes);
            dst += ox;
            for (x = 0; x < copy_width; ++x)
            {
                dst[x] = g_palette_xrgb8888[src[x]];
            }
        }
    }

    if (g_fb_fd >= 0)
        _ioctl(g_fb_fd, FB_IOC_FLIP, g_fb_backbuffer);

    handle_keyboard_input();
    handle_mouse_input();
}

void SWimp_AppActivate(qboolean active) {
    (void)active;
}

static void handle_input(void) {
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
    uint64_t now_fs = _femtoseconds();

    if (!start_fs)
        start_fs = now_fs;

    return (int)((now_fs - start_fs) / femtosecondsPerMillisecond);
}

void QG_GetMouseDiff(int *dx, int *dy) {
    *dx = g_mouse_dx;
    *dy = g_mouse_dy;
    g_mouse_dx = 0;
    g_mouse_dy = 0;
}

void QG_CaptureMouse(void) {
    g_mouse_captured = 1;
}

void QG_ReleaseMouse(void) {
    g_mouse_captured = 0;
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
            argv_ext[argc + 1] = "/initrd";
            argv_ext[argc + 2] = NULL;
            argc += 2;
            argv = argv_ext;
        }
        else if (basedir_index > 0 && basedir_index < argc) {
            argv[basedir_index] = "/initrd";
        }
    }

    Quake2_Init(argc, argv);
    apply_new_keybinds();

    oldtime = Quake2_Milliseconds();
    while (1)
    {
        handle_input();
        do {
            newtime = Quake2_Milliseconds();
            time = newtime - oldtime;
        } while (time < 1);

        Quake2_Frame(time);
        oldtime = newtime;
    }

    return 0;
}
