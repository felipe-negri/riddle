// color_blend_probe: software transparency / color mixing / stacking probe.
//
// There is no hardware alpha in quill/libqsgepaper: the framebuffer is RGB.
// This probe tests whether *software-composited* translucent colors survive the
// Paper Pro color waveform, and whether repeated partial color updates can be
// stacked predictably.
//
// Usage: color_blend_probe [mode] [patch_full] [delay_ms]
//   mode:       default 4
//   patch_full: default 0
//   delay_ms:   default 550
//
// Layout:
//   Top:    single-pass alpha blend over a gray grid: red/green/blue translucent panels overlap.
//   Middle: same area built by sequential partial updates, stacking one layer at a time.
//   Bottom: additive-looking swatches and repeated same-rect stacking.

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
struct C { unsigned char r, g, b; };

static volatile sig_atomic_t g_quit = 0;
static void on_term(int sig) { (void)sig; g_quit = 1; }
static int W, H, STRIDE, BPP;
static unsigned char *FB;

static long long now_ms(void) { struct timeval tv; gettimeofday(&tv, NULL); return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000; }
static void msleep(int ms) { usleep((useconds_t)ms * 1000); }
static unsigned char clamp8(int v) { return v < 0 ? 0 : (v > 255 ? 255 : (unsigned char)v); }

static void put_rgb(int x, int y, struct C c) {
    if (x < 0 || y < 0 || x >= W || y >= H) return;
    unsigned char *p = FB + (size_t)y * STRIDE + (size_t)x * BPP;
    if (BPP == 4) { p[0] = c.b; p[1] = c.g; p[2] = c.r; p[3] = 0xFF; }
    else if (BPP == 2) { uint16_t v = ((c.r >> 3) << 11) | ((c.g >> 2) << 5) | (c.b >> 3); p[0] = v & 255; p[1] = v >> 8; }
    else memset(p, (30 * c.r + 59 * c.g + 11 * c.b) / 100, BPP);
}
static struct C get_rgb(int x, int y) {
    if (x < 0 || y < 0 || x >= W || y >= H) return (struct C){255,255,255};
    unsigned char *p = FB + (size_t)y * STRIDE + (size_t)x * BPP;
    if (BPP == 4) return (struct C){p[2], p[1], p[0]};
    if (BPP == 2) {
        uint16_t v = (uint16_t)p[0] | ((uint16_t)p[1] << 8);
        return (struct C){(unsigned char)((((v >> 11) & 31) * 255 + 15) / 31), (unsigned char)((((v >> 5) & 63) * 255 + 31) / 63), (unsigned char)(((v & 31) * 255 + 15) / 31)};
    }
    return (struct C){p[0], p[0], p[0]};
}
static struct C blend(struct C dst, struct C src, int alpha) {
    int inv = 255 - alpha;
    return (struct C){
        clamp8((src.r * alpha + dst.r * inv + 127) / 255),
        clamp8((src.g * alpha + dst.g * inv + 127) / 255),
        clamp8((src.b * alpha + dst.b * inv + 127) / 255),
    };
}
static void fill_rect(int x0, int y0, int w, int h, struct C c) {
    for (int y = y0; y < y0 + h && y < H; y++) for (int x = x0; x < x0 + w && x < W; x++) put_rgb(x, y, c);
}
static void alpha_rect(int x0, int y0, int w, int h, struct C c, int alpha) {
    for (int y = y0; y < y0 + h && y < H; y++) for (int x = x0; x < x0 + w && x < W; x++) put_rgb(x, y, blend(get_rgb(x, y), c, alpha));
}
static void frame_rect(int x, int y, int w, int h, int t, struct C c) {
    fill_rect(x, y, w, t, c); fill_rect(x, y + h - t, w, t, c); fill_rect(x, y, t, h, c); fill_rect(x + w - t, y, t, h, c);
}
static void draw_grid_rect(int x0, int y0, int w, int h) {
    for (int y = y0; y < y0 + h && y < H; y++) for (int x = x0; x < x0 + w && x < W; x++) {
        int v = 238;
        if (((x - x0) % 120) < 3 || ((y - y0) % 120) < 3) v = 185;
        if (((x - x0) % 30) == 0 || ((y - y0) % 30) == 0) v -= 18;
        put_rgb(x, y, (struct C){clamp8(v), clamp8(v), clamp8(v)});
    }
}
static void swap_rect(const char *tag, int x, int y, int w, int h, int mode, int full) {
    long long t0 = now_ms();
    unsigned long tok = quill_swap_ex(x, y, w, h, mode, full, 1);
    fprintf(stderr, "color_blend_probe: %-14s rect=%d,%d %dx%d mode=%d full=%d token=%lu call_ms=%lld\n", tag, x,y,w,h,mode,full,tok,now_ms()-t0);
    quill_process_events();
}

static int open_input(const char *needle) {
    char path[64], name[128], lower[128];
    for (int i = 0; i < 8; i++) {
        snprintf(path, sizeof path, "/sys/class/input/event%d/device/name", i);
        FILE *f = fopen(path, "r"); if (!f) continue;
        if (!fgets(name, sizeof name, f)) { fclose(f); continue; }
        fclose(f); memset(lower, 0, sizeof lower);
        for (size_t j = 0; j < sizeof lower - 1 && name[j]; j++) lower[j] = (name[j] >= 'A' && name[j] <= 'Z') ? name[j] + 32 : name[j];
        if (!strstr(lower, needle)) continue;
        snprintf(path, sizeof path, "/dev/input/event%d", i);
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd >= 0) { int one = 1; ioctl(fd, EVIOCGRAB, &one); fprintf(stderr, "color_blend_probe: %s -> %s\n", needle, path); }
        return fd;
    }
    return -1;
}
static void drain_inputs(int pwr_fd, int touch_fd) {
    struct input_event evs[64];
    if (pwr_fd >= 0) { ssize_t n; while ((n = read(pwr_fd, evs, sizeof evs)) > 0) for (int i = 0; i < (int)(n / sizeof(struct input_event)); i++) if (evs[i].type == EV_KEY && evs[i].code == KEY_POWER && evs[i].value == 1) g_quit = 1; }
    static int slot_active[MAX_SLOTS] = {0}; static int cur_slot = 0;
    if (touch_fd >= 0) { ssize_t n; while ((n = read(touch_fd, evs, sizeof evs)) > 0) for (int i = 0; i < (int)(n / sizeof(struct input_event)); i++) {
        if (evs[i].type == EV_ABS && evs[i].code == ABS_MT_SLOT) cur_slot = evs[i].value < 0 ? 0 : (evs[i].value >= MAX_SLOTS ? MAX_SLOTS-1 : evs[i].value);
        else if (evs[i].type == EV_ABS && evs[i].code == ABS_MT_TRACKING_ID) { slot_active[cur_slot] = evs[i].value != -1; int fingers = 0; for (int s = 0; s < MAX_SLOTS; s++) fingers += slot_active[s]; if (fingers >= 5) g_quit = 1; }
    }}
}

int main(int argc, char **argv) {
    signal(SIGTERM, on_term); signal(SIGINT, on_term);
    int mode = argc > 1 ? atoi(argv[1]) : 4;
    int patch_full = argc > 2 ? atoi(argv[2]) : 0;
    int delay_ms = argc > 3 ? atoi(argv[3]) : 550;
    if (quill_init() != 0) return 1;
    W = quill_width(); H = quill_height(); STRIDE = quill_stride(); BPP = STRIDE / (W ? W : 1); FB = quill_buffer();
    fprintf(stderr, "color_blend_probe: %dx%d stride %d bpp %d fmt %d mode=%d patch_full=%d delay=%d\n", W,H,STRIDE,BPP,quill_format(),mode,patch_full,delay_ms);

    struct C white={255,255,255}, black={0,0,0}, red={255,0,0}, green={0,210,0}, blue={0,40,255}, yellow={255,230,0};
    fill_rect(0, 0, W, H, white);
    frame_rect(35, 60, W-70, H-120, 5, black);
    int band_h = (H - 180) / 3;
    int x0 = 100, ww = W - 200;
    for (int i = 0; i < 3; i++) { int y = 90 + i * band_h; draw_grid_rect(x0, y, ww, band_h - 35); frame_rect(x0, y, ww, band_h - 35, 3, black); }
    swap_rect("base", 0, 0, W, H, 4, 1);
    msleep(delay_ms * 2);

    // Top: precomposed in memory, then one dirty-rect update for the whole band.
    int yA = 90, hA = band_h - 35;
    alpha_rect(x0 + 80,  yA + 80, ww/2, hA/2, red,   112);
    alpha_rect(x0 + 230, yA + 150, ww/2, hA/2, green, 112);
    alpha_rect(x0 + 390, yA + 220, ww/2, hA/2, blue,  112);
    frame_rect(x0 + 80, yA + 80, ww/2, hA/2, 4, black);
    swap_rect("preblend-band", x0, yA, ww, hA, mode, patch_full);
    msleep(delay_ms * 2);

    // Middle: same alpha rectangles, but each layer is a separate partial update.
    int yB = 90 + band_h, hB = band_h - 35;
    alpha_rect(x0 + 80,  yB + 80, ww/2, hB/2, red,   112);
    frame_rect(x0 + 80, yB + 80, ww/2, hB/2, 4, black);
    swap_rect("stack-red", x0 + 80, yB + 80, ww/2, hB/2, mode, patch_full);
    msleep(delay_ms);
    alpha_rect(x0 + 230, yB + 150, ww/2, hB/2, green, 112);
    frame_rect(x0 + 230, yB + 150, ww/2, hB/2, 4, black);
    swap_rect("stack-green", x0 + 230, yB + 150, ww/2, hB/2, mode, patch_full);
    msleep(delay_ms);
    alpha_rect(x0 + 390, yB + 220, ww/2, hB/2, blue, 112);
    frame_rect(x0 + 390, yB + 220, ww/2, hB/2, 4, black);
    swap_rect("stack-blue", x0 + 390, yB + 220, ww/2, hB/2, mode, patch_full);
    msleep(delay_ms * 2);

    // Bottom: stacking in one fixed region. Left-to-right: 25/50/75% alpha;
    // then one box gets repeated red->blue->yellow alpha layers.
    int yC = 90 + 2 * band_h, hC = band_h - 35;
    int box = ww / 6;
    int alphas[] = {64, 128, 192};
    for (int i = 0; i < 3; i++) {
        int bx = x0 + 70 + i * (box + 45);
        alpha_rect(bx, yC + 95, box, hC - 180, red, alphas[i]);
        frame_rect(bx, yC + 95, box, hC - 180, 4, black);
        swap_rect("alpha-swatch", bx, yC + 95, box, hC - 180, mode, patch_full);
        msleep(delay_ms);
    }
    int sx = x0 + ww - box - 120, sy = yC + 95, sh = hC - 180;
    struct C seq[] = {red, blue, yellow, green, red, blue};
    for (int i = 0; i < 6; i++) {
        alpha_rect(sx, sy, box, sh, seq[i], 96);
        frame_rect(sx, sy, box, sh, 4, black);
        swap_rect("samebox-stack", sx, sy, box, sh, mode, patch_full);
        msleep(delay_ms);
    }

    int pwr_fd = open_input("powerkey"), touch_fd = open_input("touch");
    struct pollfd pfds[2] = {{.fd=pwr_fd,.events=POLLIN},{.fd=touch_fd,.events=POLLIN}};
    fprintf(stderr, "color_blend_probe: done. Top=preblend, middle=sequential stacking, bottom=alpha strengths/same-box stack.\n");
    while (!g_quit) { poll(pfds, 2, 100); drain_inputs(pwr_fd, touch_fd); quill_process_events(); }
    return 0;
}
