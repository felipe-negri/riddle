// color_mode_compare: side-by-side Paper Pro color waveform comparison.
//
// Draws the same color test pattern in vertical panels, then refreshes each
// panel with a different EPScreenMode through EPContentType::Color. This makes
// mode differences visible on one page instead of relying on memory.
//
// Usage: color_mode_compare [full_refresh] [content_type]
//   full_refresh: 1 default
//   content_type: 1=Color default, 0=Mono control

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

static long long now_ms(void) {
    struct timeval tv; gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}
static unsigned char clamp8(int v) { return v < 0 ? 0 : (v > 255 ? 255 : (unsigned char)v); }
static void put_rgb(int x, int y, unsigned char r, unsigned char g, unsigned char b) {
    if (x < 0 || y < 0 || x >= W || y >= H) return;
    unsigned char *p = FB + (size_t)y * STRIDE + (size_t)x * BPP;
    if (BPP == 4) { p[0] = b; p[1] = g; p[2] = r; p[3] = 0xFF; }
    else if (BPP == 2) { uint16_t v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3); p[0] = v & 255; p[1] = v >> 8; }
    else memset(p, (30 * r + 59 * g + 11 * b) / 100, BPP);
}
static void fill_rect(int x0, int y0, int w, int h, unsigned char r, unsigned char g, unsigned char b) {
    for (int y = y0; y < y0 + h && y < H; y++) for (int x = x0; x < x0 + w && x < W; x++) put_rgb(x, y, r, g, b);
}

// Tiny 3x5 digit glyphs for labels: 0..9 plus M.
static const unsigned char D[10][5] = {
    {7,5,5,5,7},{2,6,2,2,7},{7,1,7,4,7},{7,1,7,1,7},{5,5,7,1,1},
    {7,4,7,1,7},{7,4,7,5,7},{7,1,1,1,1},{7,5,7,5,7},{7,5,7,1,7}
};
static void draw_digit(int x, int y, int d, int scale) {
    if (d < 0 || d > 9) return;
    for (int row = 0; row < 5; row++) for (int col = 0; col < 3; col++)
        if (D[d][row] & (1 << (2 - col))) fill_rect(x + col * scale, y + row * scale, scale, scale, 0, 0, 0);
}
static void draw_mode_label(int cx, int y, int mode) {
    int s = 12;
    // draw crude M: two vertical bars + peak
    fill_rect(cx - 54, y, s, 5*s, 0,0,0); fill_rect(cx - 18, y, s, 5*s, 0,0,0);
    fill_rect(cx - 42, y + s, s, s, 0,0,0); fill_rect(cx - 30, y + 2*s, s, s, 0,0,0);
    draw_digit(cx + 6, y, mode, s);
}

static void draw_panel(int x0, int pw, int mode) {
    fill_rect(x0, 0, pw, H, 255,255,255);
    // frame
    fill_rect(x0, 0, pw, 4, 0,0,0); fill_rect(x0, H-4, pw, 4, 0,0,0);
    fill_rect(x0, 0, 4, H, 0,0,0); fill_rect(x0+pw-4, 0, 4, H, 0,0,0);
    draw_mode_label(x0 + pw/2, 48, mode);

    struct C { unsigned char r,g,b; } bars[] = {{255,255,255},{0,0,0},{255,0,0},{0,255,0},{0,0,255},{255,255,0},{255,0,255},{0,255,255}};
    int nb = 8, y0 = 150, bh = H * 22 / 100, bw = pw / nb;
    for (int i = 0; i < nb; i++) fill_rect(x0 + i*bw, y0, (i == nb-1 ? pw - i*bw : bw), bh, bars[i].r, bars[i].g, bars[i].b);
    fill_rect(x0, y0 + bh, pw, 4, 0,0,0);

    int gy = y0 + bh + 55, gh = H - gy - 90;
    for (int y = 0; y < gh; y++) for (int x = 0; x < pw; x++) {
        int r = x * 255 / (pw - 1);
        int g = y * 255 / (gh - 1 > 0 ? gh - 1 : 1);
        int b = 255 - ((x + y) * 255 / (pw + gh - 2));
        put_rgb(x0 + x, gy + y, clamp8(r), clamp8(g), clamp8(b));
    }
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
        if (fd >= 0) { int one = 1; ioctl(fd, EVIOCGRAB, &one); fprintf(stderr, "color_mode_compare: %s -> %s\n", needle, path); }
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
    int full = argc > 1 ? atoi(argv[1]) : 1;
    int content = argc > 2 ? atoi(argv[2]) : 1;
    int modes[] = {1, 3, 4, 5};
    int nm = (int)(sizeof modes / sizeof modes[0]);
    if (quill_init() != 0) return 1;
    W = quill_width(); H = quill_height(); STRIDE = quill_stride(); BPP = STRIDE / (W ? W : 1); FB = quill_buffer();
    fprintf(stderr, "color_mode_compare: %dx%d stride %d bpp %d fmt %d full=%d content=%s\n", W,H,STRIDE,BPP,quill_format(),full,content?"Color":"Mono");
    int pw = W / nm;
    for (int i = 0; i < nm; i++) draw_panel(i * pw, (i == nm-1 ? W - i*pw : pw), modes[i]);
    for (int i = 0; i < nm; i++) {
        int x = i * pw, ww = (i == nm-1 ? W - i*pw : pw);
        long long t0 = now_ms();
        unsigned long token = quill_swap_ex(x, 0, ww, H, modes[i], full, content);
        long long dt = now_ms() - t0;
        fprintf(stderr, "color_mode_compare: mode=%d rect=%d,%d token=%lu call_ms=%lld\n", modes[i], x, ww, token, dt);
        quill_process_events();
        usleep(350000);
    }
    int pwr_fd = open_input("powerkey"), touch_fd = open_input("touch");
    struct pollfd pfds[2] = {{.fd=pwr_fd,.events=POLLIN},{.fd=touch_fd,.events=POLLIN}};
    while (!g_quit) { poll(pfds, 2, 100); drain_inputs(pwr_fd, touch_fd); quill_process_events(); }
    return 0;
}
