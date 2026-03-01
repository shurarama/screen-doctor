#define _GNU_SOURCE
#include <xcb/xcb.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <png.h>
#include <sys/stat.h>
#include <time.h>
#include <stdarg.h>

/*
 * LD_PRELOAD: intercept xcb_copy_area on root window.
 * Qt's QXcbScreen::grabWindow() uses xcb_copy_area(root -> pixmap),
 * which returns black on XWayland. We intercept this and fill the
 * destination pixmap with a real screenshot via spectacle/portal.
 */

#define LOGFILE "/tmp/grab_override.log"

static void logmsg(const char *fmt, ...) {
    FILE *f = fopen(LOGFILE, "a");
    if (!f) return;
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    fprintf(f, "[%02d:%02d:%02d pid=%d] ", t->tm_hour, t->tm_min, t->tm_sec, getpid());
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fprintf(f, "\n");
    fflush(f);
    fclose(f);
}

typedef xcb_void_cookie_t (*real_xcb_copy_area_t)(
    xcb_connection_t *, xcb_drawable_t, xcb_drawable_t, xcb_gcontext_t,
    int16_t, int16_t, int16_t, int16_t, uint16_t, uint16_t);

static real_xcb_copy_area_t real_xcb_copy_area_fn = NULL;

static int is_root_window(xcb_connection_t *c, xcb_drawable_t drawable) {
    const xcb_setup_t *setup = xcb_get_setup(c);
    if (!setup) return 0;
    xcb_screen_iterator_t iter = xcb_setup_roots_iterator(setup);
    while (iter.rem) {
        if (iter.data->root == drawable) return 1;
        xcb_screen_next(&iter);
    }
    return 0;
}

static int take_screenshot(const char *output_path) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "env -u QT_QPA_PLATFORM spectacle --background --nonotify --fullscreen --output '%s' 2>/dev/null",
             output_path);
    int ret = system(cmd);
    if (ret != 0) return -1;
    struct stat st;
    if (stat(output_path, &st) != 0 || st.st_size == 0) return -1;
    return 0;
}

/* Load PNG and return BGRA pixels for the requested region */
static uint8_t *load_png_pixels(const char *path,
                                 int req_x, int req_y,
                                 int req_w, int req_h,
                                 int *out_size) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;

    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png) { fclose(fp); return NULL; }
    png_infop info = png_create_info_struct(png);
    if (!info) { png_destroy_read_struct(&png, NULL, NULL); fclose(fp); return NULL; }
    if (setjmp(png_jmpbuf(png))) {
        png_destroy_read_struct(&png, &info, NULL);
        fclose(fp);
        return NULL;
    }

    png_init_io(png, fp);
    png_read_info(png, info);

    int png_w = png_get_image_width(png, info);
    int png_h = png_get_image_height(png, info);
    png_byte color_type = png_get_color_type(png, info);
    png_byte bit_depth = png_get_bit_depth(png, info);

    if (bit_depth == 16) png_set_strip_16(png);
    if (color_type == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(png);
    if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8)
        png_set_expand_gray_1_2_4_to_8(png);
    if (png_get_valid(png, info, PNG_INFO_tRNS))
        png_set_tRNS_to_alpha(png);
    if (color_type == PNG_COLOR_TYPE_RGB ||
        color_type == PNG_COLOR_TYPE_GRAY ||
        color_type == PNG_COLOR_TYPE_PALETTE)
        png_set_filler(png, 0xFF, PNG_FILLER_AFTER);
    if (color_type == PNG_COLOR_TYPE_GRAY ||
        color_type == PNG_COLOR_TYPE_GRAY_ALPHA)
        png_set_gray_to_rgb(png);
    /* BGRA for X11 pixel format */
    png_set_bgr(png);
    png_read_update_info(png, info);

    png_bytep *rows = malloc(sizeof(png_bytep) * png_h);
    if (!rows) { png_destroy_read_struct(&png, &info, NULL); fclose(fp); return NULL; }
    for (int i = 0; i < png_h; i++)
        rows[i] = malloc(png_get_rowbytes(png, info));
    png_read_image(png, rows);

    int data_size = req_w * req_h * 4;
    uint8_t *data = calloc(1, data_size);
    if (!data) {
        for (int i = 0; i < png_h; i++) free(rows[i]);
        free(rows);
        png_destroy_read_struct(&png, &info, NULL);
        fclose(fp);
        return NULL;
    }

    for (int y = 0; y < req_h; y++) {
        int src_y = req_y + y;
        if (src_y < 0 || src_y >= png_h) continue;
        for (int x = 0; x < req_w; x++) {
            int src_x = req_x + x;
            if (src_x < 0 || src_x >= png_w) continue;
            memcpy(&data[(y * req_w + x) * 4], &rows[src_y][src_x * 4], 4);
        }
    }

    for (int i = 0; i < png_h; i++) free(rows[i]);
    free(rows);
    png_destroy_read_struct(&png, &info, NULL);
    fclose(fp);

    *out_size = data_size;
    return data;
}

xcb_void_cookie_t
xcb_copy_area(xcb_connection_t *c,
              xcb_drawable_t src_drawable,
              xcb_drawable_t dst_drawable,
              xcb_gcontext_t gc,
              int16_t src_x, int16_t src_y,
              int16_t dst_x, int16_t dst_y,
              uint16_t width, uint16_t height) {

    if (!real_xcb_copy_area_fn)
        real_xcb_copy_area_fn =
            (real_xcb_copy_area_t)dlsym(RTLD_NEXT, "xcb_copy_area");

    if (!is_root_window(c, src_drawable)) {
        /* Normal copy - pass through */
        return real_xcb_copy_area_fn(c, src_drawable, dst_drawable, gc,
                                      src_x, src_y, dst_x, dst_y,
                                      width, height);
    }

    logmsg("xcb_copy_area from ROOT: src(%d,%d) dst(%d,%d) %dx%d",
            src_x, src_y, dst_x, dst_y, width, height);

    /* First do the real copy (will copy black) */
    xcb_void_cookie_t cookie =
        real_xcb_copy_area_fn(c, src_drawable, dst_drawable, gc,
                               src_x, src_y, dst_x, dst_y,
                               width, height);

    /* Now take a real screenshot and overwrite the pixmap */
    char tmppath[256];
    snprintf(tmppath, sizeof(tmppath), "/tmp/td_grab_%d.png", getpid());

    if (take_screenshot(tmppath) != 0) {
        logmsg("Screenshot failed");
        return cookie;
    }

    int data_size = 0;
    uint8_t *pixels = load_png_pixels(tmppath, src_x, src_y,
                                       width, height, &data_size);
    unlink(tmppath);

    if (!pixels) {
        logmsg("Failed to load PNG pixels");
        return cookie;
    }

    /* Get screen depth */
    const xcb_setup_t *setup = xcb_get_setup(c);
    xcb_screen_iterator_t iter = xcb_setup_roots_iterator(setup);
    uint8_t depth = iter.data ? iter.data->root_depth : 24;

    /* Write screenshot data to the destination pixmap using xcb_put_image.
     * We send in chunks because xcb has a max request size. */
    uint32_t max_req = xcb_get_maximum_request_length(c);
    /* max data per request in bytes (leave room for header) */
    uint32_t max_data = (max_req * 4) - 64;
    int row_bytes = width * 4;
    int rows_per_chunk = max_data / row_bytes;
    if (rows_per_chunk < 1) rows_per_chunk = 1;
    if (rows_per_chunk > height) rows_per_chunk = height;

    for (int y_off = 0; y_off < height; y_off += rows_per_chunk) {
        int chunk_rows = rows_per_chunk;
        if (y_off + chunk_rows > height)
            chunk_rows = height - y_off;

        xcb_put_image(c, XCB_IMAGE_FORMAT_Z_PIXMAP,
                       dst_drawable, gc,
                       width, chunk_rows,       /* width, height of data */
                       dst_x, dst_y + y_off,    /* dst position */
                       0, depth,
                       chunk_rows * row_bytes,
                       &pixels[y_off * row_bytes]);
    }

    xcb_flush(c);
    free(pixels);

    logmsg("Wrote %dx%d screenshot to pixmap via xcb_put_image", width, height);
    return cookie;
}

__attribute__((constructor))
static void init(void) {
    logmsg("=== grab_override.so loaded (xcb_copy_area intercept) ===");
}
