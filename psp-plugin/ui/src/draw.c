#include <pspdisplay.h>
#include <string.h>

#include "draw.h"
#include "font8x8.h"

#define SCREEN_WIDTH 480
#define SCREEN_HEIGHT 272

static void *g_vram = NULL;
static int g_stride = 0;
static int g_pixfmt = PSP_DISPLAY_PIXEL_FORMAT_8888;

static u16 pack565(u32 color) {
    u32 r = (color >> 16) & 0xFF;
    u32 g = (color >> 8) & 0xFF;
    u32 b = color & 0xFF;
    return (u16)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

static u16 pack5551(u32 color) {
    u32 a = (color >> 24) & 0xFF;
    u32 r = (color >> 16) & 0xFF;
    u32 g = (color >> 8) & 0xFF;
    u32 b = color & 0xFF;
    u16 alpha = (a >= 0x80) ? 1 : 0;
    return (u16)((alpha << 15) | ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3));
}

static u16 pack4444(u32 color) {
    u32 a = (color >> 24) & 0xFF;
    u32 r = (color >> 16) & 0xFF;
    u32 g = (color >> 8) & 0xFF;
    u32 b = color & 0xFF;
    return (u16)(((a >> 4) << 12) | ((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4));
}

static void set_pixel(int x, int y, u32 color) {
    if (x < 0 || x >= SCREEN_WIDTH || y < 0 || y >= SCREEN_HEIGHT) {
        return;
    }
    if (g_vram == NULL) {
        return;
    }
    if (g_pixfmt == PSP_DISPLAY_PIXEL_FORMAT_8888) {
        u32 *vram32 = (u32 *)g_vram;
        vram32[y * g_stride + x] = color;
    } else {
        u16 *vram16 = (u16 *)g_vram;
        u16 packed = 0;
        if (g_pixfmt == PSP_DISPLAY_PIXEL_FORMAT_565) {
            packed = pack565(color);
        } else if (g_pixfmt == PSP_DISPLAY_PIXEL_FORMAT_5551) {
            packed = pack5551(color);
        } else if (g_pixfmt == PSP_DISPLAY_PIXEL_FORMAT_4444) {
            packed = pack4444(color);
        }
        vram16[y * g_stride + x] = packed;
    }
}

int draw_begin(void) {
    void *vram = NULL;
    int buffer_width = 0;
    int pixfmt = 0;

    if (sceDisplayGetFrameBuf(&vram, &buffer_width, &pixfmt, 0) < 0) {
        return -1;
    }
    if (vram == NULL) {
        return -1;
    }
    if (pixfmt != PSP_DISPLAY_PIXEL_FORMAT_8888 &&
        pixfmt != PSP_DISPLAY_PIXEL_FORMAT_565 &&
        pixfmt != PSP_DISPLAY_PIXEL_FORMAT_5551 &&
        pixfmt != PSP_DISPLAY_PIXEL_FORMAT_4444) {
        return -2;
    }

    g_vram = vram;
    g_stride = buffer_width;
    g_pixfmt = pixfmt;
    return 0;
}

void draw_clear(u32 color) {
    int x;
    int y;
    if (g_vram == NULL) {
        return;
    }
    if (g_pixfmt == PSP_DISPLAY_PIXEL_FORMAT_8888) {
        for (y = 0; y < SCREEN_HEIGHT; y++) {
            u32 *row32 = (u32 *)g_vram + y * g_stride;
            for (x = 0; x < SCREEN_WIDTH; x++) {
                row32[x] = color;
            }
        }
    } else {
        u16 packed = pack565(color);
        if (g_pixfmt == PSP_DISPLAY_PIXEL_FORMAT_5551) {
            packed = pack5551(color);
        } else if (g_pixfmt == PSP_DISPLAY_PIXEL_FORMAT_4444) {
            packed = pack4444(color);
        }
        for (y = 0; y < SCREEN_HEIGHT; y++) {
            u16 *row16 = (u16 *)g_vram + y * g_stride;
            for (x = 0; x < SCREEN_WIDTH; x++) {
                row16[x] = packed;
            }
        }
    }
}

void draw_rect(int x, int y, int w, int h, u32 color) {
    int i;
    if (g_vram == NULL) {
        return;
    }
    if (w <= 0 || h <= 0) {
        return;
    }
    for (i = 0; i < w; i++) {
        set_pixel(x + i, y, color);
        set_pixel(x + i, y + h - 1, color);
    }
    for (i = 0; i < h; i++) {
        set_pixel(x, y + i, color);
        set_pixel(x + w - 1, y + i, color);
    }
}

void draw_rect_filled(int x, int y, int w, int h, u32 color) {
    int i;
    int j;
    if (g_vram == NULL) {
        return;
    }
    if (w <= 0 || h <= 0) {
        return;
    }
    for (j = 0; j < h; j++) {
        int yy = y + j;
        if (yy < 0 || yy >= SCREEN_HEIGHT) {
            continue;
        }
        for (i = 0; i < w; i++) {
            int xx = x + i;
            if (xx < 0 || xx >= SCREEN_WIDTH) {
                continue;
            }
            set_pixel(xx, yy, color);
        }
    }
}

void draw_char(int x, int y, char c, u32 color) {
    int row;
    int col;
    unsigned char bits;
    if (g_vram == NULL) {
        return;
    }
    if ((unsigned char)c >= 128) {
        c = '?';
    }
    if (c >= 'a' && c <= 'z') {
        c = (char)(c - 1);
    }
    for (row = 0; row < 8; row++) {
        bits = font8x8_basic[(int)c][row];
        for (col = 0; col < 8; col++) {
            if (bits & (1 << col)) {
                int xx = x + col;
                int yy = y + row;
                set_pixel(xx, yy, color);
            }
        }
    }
}

void draw_text(int x, int y, const char *text, u32 color) {
    int cx = x;
    int cy = y;
    int i;
    int len;
    if (g_vram == NULL || text == NULL) {
        return;
    }
    len = (int)strlen(text);
    for (i = 0; i < len; i++) {
        char c = text[i];
        if (c == '\n') {
            cy += 10;
            cx = x;
            continue;
        }
        draw_char(cx, cy, c, color);
        cx += 8;
    }
}
