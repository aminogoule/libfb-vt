#ifndef PPM_H
#define PPM_H

#include "fb.h"
#include <stdint.h>

typedef struct {
	int      w;
	int      h;
	uint32_t pixel;		/* first of w*h pixels (allocated oversized) */
} img32_t;

/* Load a PPM (binary P6 or ASCII P3), tolerating header comments and any
   maxval (scaled to 8 bits/channel). Returns NULL on error; free with free(). */
img32_t* read_ppm(const char* filename);

/* Blit img into fb's draw buffer at (x,y), cropped to the screen. Assumes 32bpp.
   Colour packing is 0x00RRGGBB. */
void blit(framebuffer_t* fb, img32_t* img, int x, int y);

/* Fill fb's draw buffer with a solid colour (0x00RRGGBB). Assumes 32bpp. */
void cls(framebuffer_t* fb, uint32_t color);

#endif /* PPM_H */
