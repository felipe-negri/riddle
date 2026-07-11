// color_probe: experimental Paper Pro color-path probe for quill takeover.
//
// Usage: color_probe [mode] [full_refresh] [content_type]
//   mode:         EP screen mode number; try 4 first, then 3/5/1/0
//   full_refresh: 1 = CompleteRefresh, 0 = NoRefresh
//   content_type: 1 = EPContentType::Color, 0 = Mono
//
// Draws saturated RGB/CMY/YK bars plus gradients into quill's RGB-ish aux
// framebuffer, then calls quill_swap_ex(..., content_type). Exit with power
// button, five-finger tap, SIGTERM, or Ctrl-C when run under a shell.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <poll.h>

extern int quill_init(void);
extern int quill_width(void);
extern int quill_height(void);
extern int quill_stride(void);
extern int quill_format(void);
extern unsigned char *quill_buffer(void);
extern unsigned long quill_swap_ex(int x, int y, int w, int h, int mode, int full, int content_type);
extern void quill_process_events(void);

#define EV_KEY 1
#define EV_ABS 3
#define ABS_MT_SLOT 47
#define ABS_MT_TRACKING_ID 57
#define KEY_POWER 116
#define EVIOCGRAB 0x40044590
#define MAX_SLOTS 16

struct input_event { struct timeval time; uint16_t type; uint16_t code; int32_t value; };

static volatile sig_atomic_t g_quit = 0;
static void on_term(int sig) { (void)sig; g_quit = 1; }

static int W, H, STRIDE, BPP;
static unsigned char *FB;

static unsigned char clamp8(int v) { return v < 0 ? 0 : (v > 255 ? 255 : (unsigned char)v); }

static void put_rgb(int x, int y, unsigned char r, unsigned char g, unsigned char b) {
    if (x < 0 || y < 0 || x >= W || y >= H) return;
    unsigned char *p = FB + (size_t)y * STRIDE + (size_t)x * BPP;
    if (BPP == 4) {
        // QImage::Format_RGB32 on little-endian Qt: bytes are B,G,R,0xFF.
        p[0] = b; p[1] = g; p[2] = r; p[3] = 0xFF;
    } else if (BPP == 2) {
        uint16_t v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
        p[0] = (unsigned char)(v & 0xff); p[1] = (unsigned char)(v >> 8);
    } else {
        // Unknown format: fall back to luma so the probe still paints something.
        unsigned char yv = (unsigned char)((30 * r + 59 * g + 11 * b) / 100);
        memset(p, yv, BPP);
    }
}

static void fill_rect_rgb(int x0, int y0, int w, int h, unsigned char r, unsigned char g, unsigned char b) {
    for (int y = y0; y < y0 + h && y < H; y++)
        for (int x = x0; x < x0 + w && x < W; x++)
            put_rgb(x, y, r, g, b);
}

static void draw_pattern(void) {
    // White background.
    fill_rect_rgb(0, 0, W, H, 255, 255, 255);

    // Top: saturated color bars with black/white references.
    struct C { unsigned char r, g, b; } bars[] = {
        {255,255,255}, {0,0,0}, {255,0,0}, {0,255,0}, {0,0,255},
        {255,255,0}, {255,0,255}, {0,255,255}, {128,128,128}
    };
    int nb = (int)(sizeof bars / sizeof bars[0]);
    int bar_h = H * 38 / 100;
    int bw = W / nb;
    for (int i = 0; i < nb; i++) {
        int x0 = i * bw;
        int x1 = (i == nb - 1) ? W : (i + 1) * bw;
        fill_rect_rgb(x0, 0, x1 - x0, bar_h, bars[i].r, bars[i].g, bars[i].b);
    }

    // Middle: RGB ramps. If true color works, these should differ strongly;
    // if mono conversion dominates, they collapse by luminance.
    int y0 = bar_h + 40;
    int ramp_h = H * 15 / 100;
    for (int y = 0; y < ramp_h; y++) {
        for (int x = 0; x < W; x++) {
            int v = x * 255 / (W - 1);
            if (y < ramp_h / 3) put_rgb(x, y0 + y, clamp8(v), 0, 0);
            else if (y < 2 * ramp_h / 3) put_rgb(x, y0 + y, 0, clamp8(v), 0);
            else put_rgb(x, y0 + y, 0, 0, clamp8(v));
        }
    }

    // Lower: hue-ish 2D field. Useful for spotting channel swaps and whether
    // libqsgepaper consumes RGB at all.
    int gy0 = y0 + ramp_h + 50;
    int gh = H - gy0 - 80;
    if (gh < 1) gh = 1;
    for (int y = 0; y < gh; y++) {
        for (int x = 0; x < W; x++) {
            int r = x * 255 / (W - 1);
            int g = y * 255 / (gh - 1 > 0 ? gh - 1 : 1);
            int b = 255 - ((x + y) * 255 / (W + gh - 2));
            put_rgb(x, gy0 + y, clamp8(r), clamp8(g), clamp8(b));
        }
    }

    // Thin black frame + separators, so a mono result remains interpretable.
    for (int t = 0; t < 4; t++) {
        fill_rect_rgb(0, t, W, 1, 0, 0, 0);
        fill_rect_rgb(0, H - 1 - t, W, 1, 0, 0, 0);
        fill_rect_rgb(t, 0, 1, H, 0, 0, 0);
        fill_rect_rgb(W - 1 - t, 0, 1, H, 0, 0, 0);
    }
    fill_rect_rgb(0, bar_h, W, 4, 0, 0, 0);
    fill_rect_rgb(0, y0 + ramp_h, W, 4, 0, 0, 0);
}

static int open_input(const char *needle) {
    char path[64], name[128], lower[128];
    for (int i = 0; i < 8; i++) {
        snprintf(path, sizeof path, "/sys/class/input/event%d/device/name", i);
        FILE *f = fopen(path, "r"); if (!f) continue;
        if (!fgets(name, sizeof name, f)) { fclose(f); continue; }
        fclose(f); memset(lower, 0, sizeof lower);
        for (size_t j = 0; j < sizeof lower - 1 && name[j]; j++)
            lower[j] = (name[j] >= 'A' && name[j] <= 'Z') ? name[j] + 32 : name[j];
        if (!strstr(lower, needle)) continue;
        snprintf(path, sizeof path, "/dev/input/event%d", i);
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd >= 0) { int one = 1; ioctl(fd, EVIOCGRAB, &one); fprintf(stderr, "color_probe: %s -> %s\n", needle, path); }
        return fd;
    }
    return -1;
}

static void drain_inputs(int pwr_fd, int touch_fd) {
    struct input_event evs[64];
    if (pwr_fd >= 0) { ssize_t n; while ((n = read(pwr_fd, evs, sizeof evs)) > 0)
        for (int i = 0; i < (int)(n / sizeof(struct input_event)); i++)
            if (evs[i].type == EV_KEY && evs[i].code == KEY_POWER && evs[i].value == 1) g_quit = 1; }
    static int slot_active[MAX_SLOTS] = {0}; static int cur_slot = 0;
    if (touch_fd >= 0) { ssize_t n; while ((n = read(touch_fd, evs, sizeof evs)) > 0)
        for (int i = 0; i < (int)(n / sizeof(struct input_event)); i++) {
            if (evs[i].type == EV_ABS && evs[i].code == ABS_MT_SLOT) cur_slot = evs[i].value < 0 ? 0 : (evs[i].value >= MAX_SLOTS ? MAX_SLOTS-1 : evs[i].value);
            else if (evs[i].type == EV_ABS && evs[i].code == ABS_MT_TRACKING_ID) {
                slot_active[cur_slot] = evs[i].value != -1;
                int fingers = 0; for (int s = 0; s < MAX_SLOTS; s++) fingers += slot_active[s];
                if (fingers >= 5) g_quit = 1;
            }
        }}
}

int main(int argc, char **argv) {
    signal(SIGTERM, on_term); signal(SIGINT, on_term);
    int mode = argc > 1 ? atoi(argv[1]) : 4;
    int full = argc > 2 ? atoi(argv[2]) : 1;
    int content = argc > 3 ? atoi(argv[3]) : 1;

    if (quill_init() != 0) { fprintf(stderr, "color_probe: quill_init failed\n"); return 1; }
    W = quill_width(); H = quill_height(); STRIDE = quill_stride();
    BPP = STRIDE / (W > 0 ? W : 1); FB = quill_buffer();
    fprintf(stderr, "color_probe: %dx%d stride %d bpp %d fmt %d mode=%d full=%d content=%s\n",
            W, H, STRIDE, BPP, quill_format(), mode, full, content ? "Color" : "Mono");
    if (!FB || W <= 0 || H <= 0) return 1;

    draw_pattern();
    unsigned long token = quill_swap_ex(0, 0, W, H, mode, full, content);
    fprintf(stderr, "color_probe: swap token=%lu; exit with power or five-finger tap\n", token);

    int pwr_fd = open_input("powerkey"), touch_fd = open_input("touch");
    struct pollfd pfds[2] = {{.fd = pwr_fd, .events = POLLIN}, {.fd = touch_fd, .events = POLLIN}};
    while (!g_quit) {
        poll(pfds, 2, 100);
        drain_inputs(pwr_fd, touch_fd);
        quill_process_events();
    }
    fprintf(stderr, "color_probe: bye\n");
    return 0;
}
