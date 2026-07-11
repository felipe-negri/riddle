// color_partial_probe: answer whether small Paper Pro color regions can be
// updated in-place without repainting the whole screen.
//
// Usage: color_partial_probe [mode] [patch_full] [delay_ms]
//   mode:       color update mode; default 4 (3/4/5 are the useful color modes)
//   patch_full: 0 default; set 1 to force CompleteRefresh for each small patch
//   delay_ms:   delay between patches so the sequence is visible; default 450
//
// What to watch:
//   1. A pastel/grid background is painted full-screen once.
//   2. Small color patches appear one by one using ONLY their dirty rectangle.
//   3. Some patches are erased back to the background using ONLY dirty rects.
// If only the rectangles visibly update and erased cells don't leave awful
// stains, small partial color UI is viable.

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

static long long now_ms(void) {
    struct timeval tv; gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}
static unsigned char clamp8(int v) { return v < 0 ? 0 : (v > 255 ? 255 : (unsigned char)v); }
static void msleep(int ms) { usleep((useconds_t)ms * 1000); }

static void put_rgb(int x, int y, unsigned char r, unsigned char g, unsigned char b) {
    if (x < 0 || y < 0 || x >= W || y >= H) return;
    unsigned char *p = FB + (size_t)y * STRIDE + (size_t)x * BPP;
    if (BPP == 4) { p[0] = b; p[1] = g; p[2] = r; p[3] = 0xFF; }
    else if (BPP == 2) { uint16_t v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3); p[0] = v & 255; p[1] = v >> 8; }
    else memset(p, (30 * r + 59 * g + 11 * b) / 100, BPP);
}

static struct C bg_at(int x, int y) {
    // Low-saturation paper-like gradient plus grid. This makes erase-back
    // ghosts easy to see without using brutal full-screen color bars.
    int r = 238 + x * 12 / (W > 1 ? W - 1 : 1) - y * 5 / (H > 1 ? H - 1 : 1);
    int g = 240 + y * 10 / (H > 1 ? H - 1 : 1);
    int b = 232 + (x + y) * 8 / (W + H > 2 ? W + H - 2 : 1);
    if ((x % 180) < 3 || (y % 180) < 3) r = g = b = 205;
    if ((x % 45) == 0 || (y % 45) == 0) { r -= 14; g -= 14; b -= 14; }
    return (struct C){clamp8(r), clamp8(g), clamp8(b)};
}

static void fill_bg_rect(int x0, int y0, int w, int h) {
    for (int y = y0; y < y0 + h && y < H; y++)
        for (int x = x0; x < x0 + w && x < W; x++) {
            struct C c = bg_at(x, y);
            put_rgb(x, y, c.r, c.g, c.b);
        }
}
static void fill_rect(int x0, int y0, int w, int h, struct C c) {
    for (int y = y0; y < y0 + h && y < H; y++)
        for (int x = x0; x < x0 + w && x < W; x++) put_rgb(x, y, c.r, c.g, c.b);
}
static void frame_rect(int x, int y, int w, int h, int t, struct C c) {
    fill_rect(x, y, w, t, c); fill_rect(x, y + h - t, w, t, c);
    fill_rect(x, y, t, h, c); fill_rect(x + w - t, y, t, h, c);
}

static void swap_rect(const char *tag, int x, int y, int w, int h, int mode, int full, int content) {
    long long t0 = now_ms();
    unsigned long tok = quill_swap_ex(x, y, w, h, mode, full, content);
    long long dt = now_ms() - t0;
    fprintf(stderr, "color_partial_probe: %-12s rect=%d,%d %dx%d mode=%d full=%d content=%s token=%lu call_ms=%lld\n",
            tag, x, y, w, h, mode, full, content ? "Color" : "Mono", tok, dt);
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
        if (fd >= 0) { int one = 1; ioctl(fd, EVIOCGRAB, &one); fprintf(stderr, "color_partial_probe: %s -> %s\n", needle, path); }
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
    int delay_ms = argc > 3 ? atoi(argv[3]) : 450;
    if (quill_init() != 0) return 1;
    W = quill_width(); H = quill_height(); STRIDE = quill_stride(); BPP = STRIDE / (W ? W : 1); FB = quill_buffer();
    fprintf(stderr, "color_partial_probe: %dx%d stride %d bpp %d fmt %d patch_mode=%d patch_full=%d delay=%dms\n", W,H,STRIDE,BPP,quill_format(),mode,patch_full,delay_ms);

    fill_bg_rect(0, 0, W, H);
    // Three horizontal experiment bands.
    struct C black = {0,0,0};
    frame_rect(40, 80, W - 80, H - 160, 5, black);
    for (int y = H/3; y < H; y += H/3) fill_rect(40, y, W - 80, 5, black);
    swap_rect("base-full", 0, 0, W, H, 4, 1, 1);
    msleep(delay_ms * 2);

    struct C cols[] = {{255,0,0},{0,180,0},{0,0,255},{255,220,0},{255,0,220},{0,210,230},{0,0,0},{255,255,255}};
    int n = (int)(sizeof cols / sizeof cols[0]);
    int margin = 90;
    int cell = (W - 2 * margin) / n;
    int sw = cell - 24;
    if (sw > 180) sw = 180;
    int sh = H / 8;

    // Band 1: clean small color additions.
    int y1 = 145;
    for (int i = 0; i < n; i++) {
        int x = margin + i * cell + 12;
        fill_rect(x, y1, sw, sh, cols[i]);
        frame_rect(x, y1, sw, sh, 3, black);
        swap_rect("add", x, y1, sw, sh, mode, patch_full, 1);
        msleep(delay_ms);
    }

    // Band 2: color additions on a non-white/grid background, then erase every
    // other patch back to exact background. Final ghosts are the important data.
    int y2 = H/3 + 120;
    for (int i = 0; i < n; i++) {
        int x = margin + i * cell + 12;
        fill_rect(x, y2, sw, sh, cols[(i + 2) % n]);
        frame_rect(x, y2, sw, sh, 3, black);
        swap_rect("overwrite", x, y2, sw, sh, mode, patch_full, 1);
        msleep(delay_ms / 2);
    }
    for (int i = 0; i < n; i += 2) {
        int x = margin + i * cell + 12;
        fill_bg_rect(x, y2, sw, sh);
        frame_rect(x, y2, sw, sh, 3, black);
        swap_rect("erase-bg", x, y2, sw, sh, mode, patch_full, 1);
        msleep(delay_ms);
    }

    // Band 3: repeated same-rectangle color churn, then erase to background.
    // If partial color accumulates ugly ghosts, this cell will reveal it.
    int x3 = W/2 - sw;
    int y3 = 2*H/3 + 130;
    int w3 = sw * 2;
    int h3 = sh + 70;
    for (int round = 0; round < 3; round++) {
        for (int i = 0; i < 6; i++) {
            fill_rect(x3, y3, w3, h3, cols[i]);
            frame_rect(x3, y3, w3, h3, 4, black);
            swap_rect("churn", x3, y3, w3, h3, mode, patch_full, 1);
            msleep(delay_ms);
        }
    }
    fill_bg_rect(x3, y3, w3, h3);
    frame_rect(x3, y3, w3, h3, 4, black);
    swap_rect("churn-erase", x3, y3, w3, h3, mode, patch_full, 1);

    int pwr_fd = open_input("powerkey"), touch_fd = open_input("touch");
    struct pollfd pfds[2] = {{.fd=pwr_fd,.events=POLLIN},{.fd=touch_fd,.events=POLLIN}};
    fprintf(stderr, "color_partial_probe: done; inspect patches/erased cells for local ghosting. Exit with power or five-finger tap.\n");
    while (!g_quit) { poll(pfds, 2, 100); drain_inputs(pwr_fd, touch_fd); quill_process_events(); }
    return 0;
}
