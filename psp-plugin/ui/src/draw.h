#ifndef DRAW_H
#define DRAW_H

#include <psptypes.h>

int draw_begin(void);
void draw_clear(u32 color);
void draw_rect(int x, int y, int w, int h, u32 color);
void draw_rect_filled(int x, int y, int w, int h, u32 color);
void draw_char(int x, int y, char c, u32 color);
void draw_text(int x, int y, const char *text, u32 color);

#endif
