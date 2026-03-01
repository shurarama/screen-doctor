#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <stdio.h>
#include <stdlib.h>
#include <png.h>

/* Small test: grabs root window via XGetImage and saves to PNG */

int main() {
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) { fprintf(stderr, "Cannot open display\n"); return 1; }

    Window root = DefaultRootWindow(dpy);
    int w = 200, h = 200; /* small test area */

    XImage *img = XGetImage(dpy, root, 100, 100, w, h, AllPlanes, ZPixmap);
    if (!img) { fprintf(stderr, "XGetImage failed\n"); XCloseDisplay(dpy); return 1; }

    /* Check if image is all black */
    int non_black = 0;
    for (int y = 0; y < h && !non_black; y++)
        for (int x = 0; x < w && !non_black; x++)
            if (XGetPixel(img, x, y) != 0) non_black = 1;

    printf("Image %dx%d captured. All black: %s\n", w, h, non_black ? "NO (has content!)" : "YES (black)");

    /* Save as PNG for visual inspection */
    FILE *fp = fopen("/tmp/test_grab_result.png", "wb");
    if (fp) {
        png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
        png_infop info = png_create_info_struct(png);
        png_init_io(png, fp);
        png_set_IHDR(png, info, w, h, 8, PNG_COLOR_TYPE_RGB, PNG_INTERLACE_NONE,
                     PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
        png_write_info(png, info);

        png_bytep row = malloc(3 * w);
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                unsigned long px = XGetPixel(img, x, y);
                row[x*3+0] = (px >> 16) & 0xFF;
                row[x*3+1] = (px >> 8) & 0xFF;
                row[x*3+2] = px & 0xFF;
            }
            png_write_row(png, row);
        }
        png_write_end(png, NULL);
        free(row);
        png_destroy_write_struct(&png, &info);
        fclose(fp);
        printf("Saved to /tmp/test_grab_result.png\n");
    }

    XDestroyImage(img);
    XCloseDisplay(dpy);
    return 0;
}
