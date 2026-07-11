// quill_c: C ABI over the vendor e-ink engine (libqsgepaper's EPFramebuffer,
// accessed via asivery's epfb-re shim). Runs with xochitl STOPPED — this
// process becomes the display driver.
//
// The engine wants a Qt context; we create a QCoreApplication but drive our
// own loop — swapBuffers() is synchronous enough for ink.

#include "quill.h"
#include "epframebuffer.h"
#include <QCoreApplication>
#include <QImage>
#include <cstring>
#include <cstdio>

static QCoreApplication *g_app = nullptr;
static EPFramebuffer *g_fb = nullptr;
static QImage *g_aux = nullptr;

extern "C" {

// Returns 0 on success. After this, quill_buffer()/quill_swap() are usable.
int quill_init() {
    if (g_fb) return 0;
    static int argc = 1;
    static char arg0[] = "quill";
    static char *argv[] = {arg0, nullptr};
    g_app = new QCoreApplication(argc, argv);
    g_fb = EPFramebuffer::createControlledInstance();
    if (!g_fb) return 1;
    g_aux = g_fb->getAuxFramebuffer();
    if (!g_aux) return 2;
    fprintf(stderr, "quill: aux framebuffer %dx%d format=%d bpl=%lld\n",
            g_aux->width(), g_aux->height(), (int)g_aux->format(),
            (long long)g_aux->bytesPerLine());
    return 0;
}

// Geometry of the drawing buffer.
int quill_width()  { return g_aux ? g_aux->width() : 0; }
int quill_height() { return g_aux ? g_aux->height() : 0; }
int quill_stride() { return g_aux ? (int)g_aux->bytesPerLine() : 0; }
int quill_format() { return g_aux ? (int)g_aux->format() : -1; }

// Direct pointer into the aux framebuffer pixels.
unsigned char *quill_buffer() {
    return g_aux ? g_aux->bits() : nullptr;
}

// Push a region to glass. mode: 0=fastest(DU-ish) 1=fast 3/4/5=color-capable.
// full_refresh != 0 asks libqsgepaper for CompleteRefresh on the region.
unsigned long quill_swap_ex(int x, int y, int w, int h, int mode, int full_refresh, int content_type) {
    if (!g_fb || !g_aux || w <= 0 || h <= 0) return 0;
    // Clip before crossing the vendor ABI. Besides protecting QRect arithmetic,
    // this guarantees that partial-cleanup callers cannot accidentally expand
    // an edge-straddling dirty rectangle beyond the panel.
    long long right = (long long)x + w, bottom = (long long)y + h;
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = right > g_aux->width() ? g_aux->width() : (int)right;
    int y1 = bottom > g_aux->height() ? g_aux->height() : (int)bottom;
    if (x1 <= x0 || y1 <= y0) return 0;
    // CompleteRefresh can be promoted to a panel-wide operation by the vendor
    // backend even when QRect is small. Semantic partial wrappers always pass 0.
    QFlags<EPFramebuffer::UpdateFlag> flags = full_refresh
        ? EPFramebuffer::UpdateFlag::CompleteRefresh
        : EPFramebuffer::UpdateFlag::NoRefresh;
    EPContentType ct = content_type ? EPContentType::Color : EPContentType::Mono;
    return g_fb->swapBuffers(QRect(x0, y0, x1 - x0, y1 - y0), ct, (EPScreenMode)mode, flags);
}

// Compatibility wrapper: the original quill API was mono-only.
unsigned long quill_swap(int x, int y, int w, int h, int mode, int full_refresh) {
    return quill_swap_ex(x, y, w, h, mode, full_refresh, QUILL_CONTENT_MONO);
}

// Semantic wrappers for the paths verified on-device.
unsigned long quill_swap_mono_fast(int x, int y, int w, int h) {
    return quill_swap_ex(x, y, w, h, QUILL_MODE_FASTEST, 0, QUILL_CONTENT_MONO);
}

unsigned long quill_swap_mono_quality(int x, int y, int w, int h) {
    return quill_swap_ex(x, y, w, h, QUILL_MODE_COLOR3, 0, QUILL_CONTENT_MONO);
}

unsigned long quill_swap_color(int x, int y, int w, int h) {
    return quill_swap_ex(x, y, w, h, QUILL_MODE_COLOR4, 0, QUILL_CONTENT_COLOR);
}

unsigned long quill_swap_color_full(int x, int y, int w, int h) {
    return quill_swap_ex(x, y, w, h, QUILL_MODE_COLOR4, 1, QUILL_CONTENT_COLOR);
}

void quill_process_events() {
    if (g_app) QCoreApplication::processEvents();
}

} // extern "C"
