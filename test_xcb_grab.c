#include <xcb/xcb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Test: grab root window via xcb_get_image_unchecked (same as Qt does) */
int main() {
    xcb_connection_t *c = xcb_connect(NULL, NULL);
    if (xcb_connection_has_error(c)) {
        fprintf(stderr, "Cannot connect to X\n");
        return 1;
    }

    const xcb_setup_t *setup = xcb_get_setup(c);
    xcb_screen_t *screen = xcb_setup_roots_iterator(setup).data;

    int w = 200, h = 200;
    xcb_get_image_cookie_t cookie =
        xcb_get_image_unchecked(c, XCB_IMAGE_FORMAT_Z_PIXMAP,
                                 screen->root, 100, 100, w, h, ~0);

    xcb_generic_error_t *err = NULL;
    xcb_get_image_reply_t *reply = xcb_get_image_reply(c, cookie, &err);

    if (err) {
        printf("Error: code=%d\n", err->error_code);
        free(err);
    }
    if (!reply) {
        printf("Reply is NULL (grab failed)\n");
        xcb_disconnect(c);
        return 1;
    }

    uint8_t *data = xcb_get_image_data(reply);
    int len = xcb_get_image_data_length(reply);
    printf("Got reply: depth=%d, data_len=%d\n", reply->depth, len);

    /* Check if all black */
    int non_black = 0;
    for (int i = 0; i < len && !non_black; i += 4) {
        if (data[i] != 0 || data[i+1] != 0 || data[i+2] != 0)
            non_black = 1;
    }
    printf("All black: %s\n", non_black ? "NO (has content!)" : "YES");

    free(reply);
    xcb_disconnect(c);
    return 0;
}
