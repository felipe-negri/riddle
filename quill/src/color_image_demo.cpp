// color_image_demo: render a PNG/JPEG/etc. through quill's verified color path.
// Usage: color_image_demo /path/to/image.png [fit|fill|stretch] [full]
// Exit: power button, 5-finger tap, or SIGTERM.

#include <QImage>
#include <QtGlobal>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <poll.h>

extern "C" {
#include "quill.h"
}

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

static void put_rgb(int x, int y, unsigned char r, unsigned char g, unsigned char b) {
    if (x < 0 || y < 0 || x >= W || y >= H) return;
    unsigned char *p = FB + (size_t)y * STRIDE + (size_t)x * BPP;
    if (BPP == 4) { p[0] = b; p[1] = g; p[2] = r; p[3] = 0xFF; }
    else if (BPP == 2) { uint16_t v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3); p[0] = v & 255; p[1] = v >> 8; }
    else memset(p, (30 * r + 59 * g + 11 * b) / 100, BPP);
}

static void clear_white(void) {
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) put_rgb(x, y, 255, 255, 255);
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
        if (fd >= 0) { int one = 1; ioctl(fd, EVIOCGRAB, &one); fprintf(stderr, "color_image_demo: %s -> %s\n", needle, path); }
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

static void blit_image(const QImage &src, const char *mode) {
    clear_white();
    Qt::AspectRatioMode aspect = Qt::KeepAspectRatio;
    if (strcmp(mode, "fill") == 0) aspect = Qt::KeepAspectRatioByExpanding;
    if (strcmp(mode, "stretch") == 0) aspect = Qt::IgnoreAspectRatio;

    QImage scaled = src.convertToFormat(QImage::Format_RGB32)
        .scaled(W, H, aspect, Qt::SmoothTransformation)
        .convertToFormat(QImage::Format_RGB32);

    int ox = (W - scaled.width()) / 2;
    int oy = (H - scaled.height()) / 2;
    int sx0 = ox < 0 ? -ox : 0, sy0 = oy < 0 ? -oy : 0;
    int dx0 = ox > 0 ? ox : 0, dy0 = oy > 0 ? oy : 0;
    int cw = qMin(W - dx0, scaled.width() - sx0), ch = qMin(H - dy0, scaled.height() - sy0);

    for (int y = 0; y < ch; y++) {
        const QRgb *row = (const QRgb *)scaled.constScanLine(sy0 + y);
        for (int x = 0; x < cw; x++) {
            QRgb px = row[sx0 + x];
            put_rgb(dx0 + x, dy0 + y, qRed(px), qGreen(px), qBlue(px));
        }
    }
}

int main(int argc, char **argv) {
    signal(SIGTERM, on_term); signal(SIGINT, on_term);
    if (argc < 2) { fprintf(stderr, "usage: color_image_demo /path/to/image.png [fit|fill|stretch] [full]\n"); return 2; }
    const char *mode = argc >= 3 ? argv[2] : "fit";
    int full = argc >= 4 ? atoi(argv[3]) : 0;

    if (quill_init() != 0) return 1;
    W = quill_width(); H = quill_height(); STRIDE = quill_stride(); BPP = STRIDE / (W ? W : 1); FB = quill_buffer();
    fprintf(stderr, "color_image_demo: %dx%d stride %d bpp %d fmt %d image=%s mode=%s full=%d\n", W, H, STRIDE, BPP, quill_format(), argv[1], mode, full);

    QImage img(argv[1]);
    if (img.isNull()) { fprintf(stderr, "color_image_demo: could not load %s\n", argv[1]); return 1; }
    blit_image(img, mode);
    quill_swap_ex(0, 0, W, H, QUILL_MODE_COLOR4, full, QUILL_CONTENT_COLOR);
    quill_process_events();

    int pwr_fd = open_input("powerkey"), touch_fd = open_input("touch");
    struct pollfd pfds[2] = {{.fd = pwr_fd, .events = POLLIN}, {.fd = touch_fd, .events = POLLIN}};
    while (!g_quit) { poll(pfds, 2, 100); drain_inputs(pwr_fd, touch_fd); quill_process_events(); }
    return 0;
}
