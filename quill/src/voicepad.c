// voicepad: takeover app that writes speech transcripts received over TCP.
// Port 7777 speaks three dialects on the same socket:
//   - raw UTF-8 lines (the original phone PWA / parakeet bridge protocol)
//   - HTTP GET /             -> Android companion install/pair page
//   - HTTP GET /voicepad.apk -> native companion APK
//   - HTTP POST /say         -> body text is written onto the sheet
//   - HTTP POST /draw        -> remote phone ink
//   - HTTP POST /key         -> live Bluetooth/phone keyboard events
// A small QR code in the header encodes the install page URL (override:
// $VOICEPAD_URL), so the native phone companion is one scan away.
// Voice and keyboard events mutate one persisted Unicode document at a shared
// cursor. Text, static paper, and tool-tagged manuscript ink are separate
// compositing layers, so text edits do not destroy pen annotations.
// Exit: power button, 5-finger tap, or SIGTERM.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <stdint.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <poll.h>

extern int quill_init(void);
extern int quill_width(void);
extern int quill_height(void);
extern int quill_stride(void);
extern int quill_format(void);
extern unsigned char *quill_buffer(void);
extern unsigned long quill_swap(int x, int y, int w, int h, int mode, int full);
extern unsigned long quill_swap_mono_fast(int x, int y, int w, int h);
extern unsigned long quill_swap_mono_quality(int x, int y, int w, int h);
extern void quill_process_events(void);

#define EV_SYN 0
#define EV_KEY 1
#define EV_ABS 3
#define SYN_REPORT 0
#define ABS_X 0
#define ABS_Y 1
#define ABS_PRESSURE 24
#define ABS_MT_SLOT 47
#define ABS_MT_TRACKING_ID 57
#define BTN_TOOL_RUBBER 321
#define BTN_TOUCH 330
#define KEY_POWER 116
#define EVIOCGRAB 0x40044590
#define MAX_SLOTS 16
#define DIGI_MAX_X 11180
#define DIGI_MAX_Y 15340
#define PORT 7777
#define MAX_LINE 96
#define MAX_CONNS 8

struct input_event { struct timeval time; uint16_t type; uint16_t code; int32_t value; };
static volatile sig_atomic_t g_quit = 0;
static void on_term(int sig) { (void)sig; g_quit = 1; }
static int W, H, STRIDE, BPP;
static unsigned char *FB;
// Three compositing layers keep editable text separate from manuscript ink.
// BASE is the static header/paper, TEXT is regenerated from the document, and
// INK stores the latest pen operation per pixel (0 transparent, 1 ink, 2 mask).
static uint8_t *BASE_LAYER, *TEXT_LAYER, *PREV_TEXT_LAYER, *INK_LAYER;

static const unsigned char FONT5X7[95][5] = {
    {0x00,0x00,0x00,0x00,0x00},{0x00,0x00,0x5F,0x00,0x00},{0x00,0x07,0x00,0x07,0x00},
    {0x14,0x7F,0x14,0x7F,0x14},{0x24,0x2A,0x7F,0x2A,0x12},{0x23,0x13,0x08,0x64,0x62},
    {0x36,0x49,0x55,0x22,0x50},{0x00,0x05,0x03,0x00,0x00},{0x00,0x1C,0x22,0x41,0x00},
    {0x00,0x41,0x22,0x1C,0x00},{0x14,0x08,0x3E,0x08,0x14},{0x08,0x08,0x3E,0x08,0x08},
    {0x00,0x50,0x30,0x00,0x00},{0x08,0x08,0x08,0x08,0x08},{0x00,0x60,0x60,0x00,0x00},
    {0x20,0x10,0x08,0x04,0x02},{0x3E,0x51,0x49,0x45,0x3E},{0x00,0x42,0x7F,0x40,0x00},
    {0x42,0x61,0x51,0x49,0x46},{0x21,0x41,0x45,0x4B,0x31},{0x18,0x14,0x12,0x7F,0x10},
    {0x27,0x45,0x45,0x45,0x39},{0x3C,0x4A,0x49,0x49,0x30},{0x01,0x71,0x09,0x05,0x03},
    {0x36,0x49,0x49,0x49,0x36},{0x06,0x49,0x49,0x29,0x1E},{0x00,0x36,0x36,0x00,0x00},
    {0x00,0x56,0x36,0x00,0x00},{0x08,0x14,0x22,0x41,0x00},{0x14,0x14,0x14,0x14,0x14},
    {0x00,0x41,0x22,0x14,0x08},{0x02,0x01,0x51,0x09,0x06},{0x32,0x49,0x79,0x41,0x3E},
    {0x7E,0x11,0x11,0x11,0x7E},{0x7F,0x49,0x49,0x49,0x36},{0x3E,0x41,0x41,0x41,0x22},
    {0x7F,0x41,0x41,0x22,0x1C},{0x7F,0x49,0x49,0x49,0x41},{0x7F,0x09,0x09,0x09,0x01},
    {0x3E,0x41,0x49,0x49,0x7A},{0x7F,0x08,0x08,0x08,0x7F},{0x00,0x41,0x7F,0x41,0x00},
    {0x20,0x40,0x41,0x3F,0x01},{0x7F,0x08,0x14,0x22,0x41},{0x7F,0x40,0x40,0x40,0x40},
    {0x7F,0x02,0x0C,0x02,0x7F},{0x7F,0x04,0x08,0x10,0x7F},{0x3E,0x41,0x41,0x41,0x3E},
    {0x7F,0x09,0x09,0x09,0x06},{0x3E,0x41,0x51,0x21,0x5E},{0x7F,0x09,0x19,0x29,0x46},
    {0x46,0x49,0x49,0x49,0x31},{0x01,0x01,0x7F,0x01,0x01},{0x3F,0x40,0x40,0x40,0x3F},
    {0x1F,0x20,0x40,0x20,0x1F},{0x3F,0x40,0x38,0x40,0x3F},{0x63,0x14,0x08,0x14,0x63},
    {0x07,0x08,0x70,0x08,0x07},{0x61,0x51,0x49,0x45,0x43},{0x00,0x7F,0x41,0x41,0x00},
    {0x02,0x04,0x08,0x10,0x20},{0x00,0x41,0x41,0x7F,0x00},{0x04,0x02,0x01,0x02,0x04},
    {0x40,0x40,0x40,0x40,0x40},{0x00,0x01,0x02,0x04,0x00},{0x20,0x54,0x54,0x54,0x78},
    {0x7F,0x48,0x44,0x44,0x38},{0x38,0x44,0x44,0x44,0x20},{0x38,0x44,0x44,0x48,0x7F},
    {0x38,0x54,0x54,0x54,0x18},{0x08,0x7E,0x09,0x01,0x02},{0x0C,0x52,0x52,0x52,0x3E},
    {0x7F,0x08,0x04,0x04,0x78},{0x00,0x44,0x7D,0x40,0x00},{0x20,0x40,0x44,0x3D,0x00},
    {0x7F,0x10,0x28,0x44,0x00},{0x00,0x41,0x7F,0x40,0x00},{0x7C,0x04,0x18,0x04,0x78},
    {0x7C,0x08,0x04,0x04,0x78},{0x38,0x44,0x44,0x44,0x38},{0x7C,0x14,0x14,0x14,0x08},
    {0x08,0x14,0x14,0x18,0x7C},{0x7C,0x08,0x04,0x04,0x08},{0x48,0x54,0x54,0x54,0x20},
    {0x04,0x3F,0x44,0x40,0x20},{0x3C,0x40,0x40,0x20,0x7C},{0x1C,0x20,0x40,0x20,0x1C},
    {0x3C,0x40,0x30,0x40,0x3C},{0x44,0x28,0x10,0x28,0x44},{0x0C,0x50,0x50,0x50,0x3C},
    {0x44,0x64,0x54,0x4C,0x44},{0x00,0x08,0x36,0x41,0x00},{0x00,0x00,0x7F,0x00,0x00},
    {0x00,0x41,0x36,0x08,0x00},{0x08,0x08,0x2A,0x1C,0x08},
};

static void put_px(int x, int y, unsigned char v) {
    if (x < 0 || y < 0 || x >= W || y >= H) return;
    unsigned char *p = FB + (size_t)y * STRIDE + (size_t)x * BPP;
    memset(p, v, BPP); if (BPP == 4) p[3] = 0xFF;
}
static void fill_rect(int x0, int y0, int w, int h, unsigned char v) {
    for (int y = y0; y < y0 + h && y < H; y++) for (int x = x0; x < x0 + w && x < W; x++) put_px(x, y, v);
}
static void draw_text(int x, int y, const char *s, int scale) {
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c < 32 || c > 126) c = '?';
        const unsigned char *g = FONT5X7[c - 32];
        for (int col = 0; col < 5; col++) for (int row = 0; row < 7; row++)
            if (g[col] & (1 << row)) fill_rect(x + col * scale, y + row * scale, scale, scale, 0x00);
        x += 6 * scale;
    }
}
static void capture_base_layer(void) {
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        unsigned char *p = FB + (size_t)y * STRIDE + (size_t)x * BPP;
        BASE_LAYER[(size_t)y * W + x] = p[0] < 128;
    }
}
static void text_layer_char(int x, int y, uint32_t codepoint, int scale) {
    unsigned char c = (codepoint >= 32 && codepoint <= 126) ? (unsigned char)codepoint : '?';
    const unsigned char *g = FONT5X7[c - 32];
    for (int col = 0; col < 5; col++) for (int row = 0; row < 7; row++) if (g[col] & (1 << row))
        for (int yy = 0; yy < scale; yy++) for (int xx = 0; xx < scale; xx++) {
            int px = x + col * scale + xx, py = y + row * scale + yy;
            if (px >= 0 && py >= 0 && px < W && py < H) TEXT_LAYER[(size_t)py * W + px] = 1;
        }
}
static void compose_rect(int x0, int y0, int w, int h) {
    if (x0 < 0) { w += x0; x0 = 0; }
    if (y0 < 0) { h += y0; y0 = 0; }
    if (x0 + w > W) w = W - x0;
    if (y0 + h > H) h = H - y0;
    if (w <= 0 || h <= 0) return;
    for (int y = y0; y < y0 + h; y++) for (int x = x0; x < x0 + w; x++) {
        size_t i = (size_t)y * W + x;
        unsigned char v = INK_LAYER[i] == 1 ? 0x00 :
                          (INK_LAYER[i] == 2 ? 0xFF :
                          ((BASE_LAYER[i] || TEXT_LAYER[i]) ? 0x00 : 0xFF));
        put_px(x, y, v);
    }
}

// ---------------------------------------------------------------- QR encoder
// Byte mode, ECC level L, versions 1-4 (all single RS block), fixed mask 0.
#define QR_MAX_VER 4
#define QR_MAX_SIZE (17 + 4 * QR_MAX_VER)
static uint8_t qr_grid[QR_MAX_SIZE * QR_MAX_SIZE];
static uint8_t qr_func[QR_MAX_SIZE * QR_MAX_SIZE];
static int qr_size;

static void qr_set(int x, int y, int dark, int func) {
    qr_grid[y * qr_size + x] = (uint8_t)dark;
    if (func) qr_func[y * qr_size + x] = 1;
}
static uint8_t gf_mul(uint8_t a, uint8_t b) {
    uint8_t r = 0;
    while (b) { if (b & 1) r ^= a; a = (uint8_t)((a << 1) ^ ((a & 0x80) ? 0x1D : 0)); b >>= 1; }
    return r;
}
static void rs_remainder(const uint8_t *data, int dlen, int ecclen, uint8_t *out) {
    uint8_t gen[32]; // generator poly coefficients, highest degree first, leading 1 implicit
    if (dlen < 0 || ecclen <= 0 || ecclen > (int)sizeof gen) return;
    memset(gen, 0, sizeof gen); gen[ecclen - 1] = 1;
    uint8_t root = 1;
    for (int i = 0; i < ecclen; i++) {
        for (int j = 0; j < ecclen; j++)
            gen[j] = (uint8_t)(gf_mul(gen[j], root) ^ (j + 1 < ecclen ? gen[j + 1] : 0));
        root = gf_mul(root, 2);
    }
    memset(out, 0, ecclen);
    for (int i = 0; i < dlen; i++) {
        uint8_t factor = data[i] ^ out[0];
        memmove(out, out + 1, ecclen - 1); out[ecclen - 1] = 0;
        for (int j = 0; j < ecclen; j++) out[j] ^= gf_mul(gen[j], factor);
    }
}
static void qr_finder(int cx, int cy) {
    for (int dy = -4; dy <= 4; dy++) for (int dx = -4; dx <= 4; dx++) {
        int x = cx + dx, y = cy + dy;
        if (x < 0 || y < 0 || x >= qr_size || y >= qr_size) continue;
        int d = abs(dx) > abs(dy) ? abs(dx) : abs(dy);
        qr_set(x, y, d != 2 && d != 4, 1);
    }
}
// Returns qr_size (modules per side) or 0 if text too long.
static int qr_encode(const char *text) {
    static const int datacw[5] = {0, 19, 34, 55, 80};
    static const int ecccw[5]  = {0, 7, 10, 15, 20};
    int len = (int)strlen(text), ver;
    for (ver = 1; ver <= QR_MAX_VER; ver++) if (len + 2 <= datacw[ver]) break;
    if (ver > QR_MAX_VER) return 0;
    int ndata = datacw[ver], necc = ecccw[ver];
    qr_size = 17 + 4 * ver;
    memset(qr_grid, 0, sizeof qr_grid); memset(qr_func, 0, sizeof qr_func);

    // bitstream: mode 0100, 8-bit count, data, terminator, pads
    uint8_t cw[100 + 20]; memset(cw, 0, sizeof cw);
    int bit = 0;
    #define PUTBITS(v, n) for (int b_ = (n) - 1; b_ >= 0; b_--, bit++) if (((v) >> b_) & 1) cw[bit >> 3] |= 0x80 >> (bit & 7)
    PUTBITS(4, 4); PUTBITS(len, 8);
    for (int i = 0; i < len; i++) { PUTBITS((unsigned char)text[i], 8); }
    int room = ndata * 8 - bit; PUTBITS(0, room < 4 ? room : 4);
    if (bit & 7) bit = (bit + 7) & ~7;
    for (int pad = 0xEC; bit < ndata * 8; pad ^= 0xEC ^ 0x11) { PUTBITS(pad, 8); }
    #undef PUTBITS
    rs_remainder(cw, ndata, necc, cw + ndata);

    // function patterns
    qr_finder(3, 3); qr_finder(qr_size - 4, 3); qr_finder(3, qr_size - 4);
    for (int i = 0; i < qr_size; i++) {
        if (!qr_func[6 * qr_size + i]) qr_set(i, 6, i % 2 == 0, 1);
        if (!qr_func[i * qr_size + 6]) qr_set(6, i, i % 2 == 0, 1);
    }
    if (ver >= 2) {
        int c = qr_size - 7; // single alignment pattern at (c, c) for v2-4
        for (int dy = -2; dy <= 2; dy++) for (int dx = -2; dx <= 2; dx++) {
            int d = abs(dx) > abs(dy) ? abs(dx) : abs(dy);
            qr_set(c + dx, c + dy, d != 1, 1);
        }
    }
    // reserve format areas + dark module
    for (int i = 0; i <= 8; i++) {
        if (i != 6) { qr_func[i * qr_size + 8] = 1; qr_func[8 * qr_size + i] = 1; }
        if (i < 8) { qr_func[8 * qr_size + qr_size - 1 - i] = 1; qr_func[(qr_size - 1 - i) * qr_size + 8] = 1; }
    }
    qr_set(8, qr_size - 8, 1, 1);

    // zigzag data placement
    int i = 0, total = (ndata + necc) * 8;
    for (int right = qr_size - 1; right >= 1; right -= 2) {
        if (right == 6) right = 5;
        for (int vert = 0; vert < qr_size; vert++) {
            for (int j = 0; j < 2; j++) {
                int x = right - j;
                int upward = ((right + 1) & 2) == 0;
                int y = upward ? qr_size - 1 - vert : vert;
                if (!qr_func[y * qr_size + x] && i < total) {
                    qr_grid[y * qr_size + x] = (cw[i >> 3] >> (7 - (i & 7))) & 1;
                    i++;
                }
            }
        }
    }
    // mask 0: invert non-function modules where (x + y) even
    for (int y = 0; y < qr_size; y++) for (int x = 0; x < qr_size; x++)
        if (!qr_func[y * qr_size + x] && ((x + y) & 1) == 0) qr_grid[y * qr_size + x] ^= 1;

    // format bits: ECC L (01) + mask 0, BCH(15,5), masked with 0x5412
    int fdata = (1 << 3) | 0, rem = fdata;
    for (int k = 0; k < 10; k++) rem = (rem << 1) ^ ((rem >> 9) * 0x537);
    int fbits = ((fdata << 10) | rem) ^ 0x5412;
    #define FBIT(k) (((fbits) >> (k)) & 1)
    for (int k = 0; k <= 5; k++) qr_set(8, k, FBIT(k), 1);
    qr_set(8, 7, FBIT(6), 1); qr_set(8, 8, FBIT(7), 1); qr_set(7, 8, FBIT(8), 1);
    for (int k = 9; k < 15; k++) qr_set(14 - k, 8, FBIT(k), 1);
    for (int k = 0; k < 8; k++) qr_set(qr_size - 1 - k, 8, FBIT(k), 1);
    for (int k = 8; k < 15; k++) qr_set(8, qr_size - 15 + k, FBIT(k), 1);
    #undef FBIT
    return qr_size;
}
static void qr_draw(int x0, int y0, int module_px) {
    int quiet = 4 * module_px;
    fill_rect(x0 - quiet, y0 - quiet, qr_size * module_px + 2 * quiet, qr_size * module_px + 2 * quiet, 0xFF);
    for (int y = 0; y < qr_size; y++) for (int x = 0; x < qr_size; x++)
        if (qr_grid[y * qr_size + x]) fill_rect(x0 + x * module_px, y0 + y * module_px, module_px, module_px, 0x00);
}

// -------------------------------------------------------------- sheet & text
// The canvas is persistent: text is inked once at the cursor and never
// repainted, so the pen eraser removes it for good, like any other stroke.
#define TEXT_SCALE 5
#define CHAR_STEP (6 * TEXT_SCALE)
#define LINE_STEP 76
#define TEXT_LEFT 100
#define TEXT_RIGHT 100
#define DOC_MAX 32768
#define DOCUMENT_FILE "voicepad-document.txt"
#define CURSOR_FILE "voicepad-cursor.txt"

static uint32_t document[DOC_MAX];
static uint16_t layout_row[DOC_MAX + 1], layout_col[DOC_MAX + 1];
static int document_len, document_cursor, document_last_row;
static int text_cols, text_rows, text_top, view_top_row;
static int preferred_col = -1;
static int document_dirty;
static long long document_changed_ms;
static int sheet_top;
static int ink_segment_count;

struct recti { int x, y, w, h; };
static void ink_rebuild_visible(void);

static long long now_ms(void) {
    struct timeval tv; gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

// Interactive changes are accumulated as one exact focus-diff rectangle.
// After a short idle period that rectangle gets at most one *partial* Quill
// quality update. CompleteRefresh is intentionally forbidden here: on the
// Gallery panel that flag can promote even a tiny QRect to a whole-screen job.
static long long cleanup_last_change_ms;
static int cleanup_pending, cleanup_debt;
static struct recti cleanup_rect;
static void cleanup_mark_rect(int x, int y, int w, int h, int debt) {
    if (debt <= 0 || w <= 0 || h <= 0) return;
    if (x < 0) { w += x; x = 0; }
    if (y < sheet_top) { h -= sheet_top - y; y = sheet_top; }
    if (x + w > W) w = W - x;
    if (y + h > H) h = H - y;
    if (w <= 0 || h <= 0) return;
    if (!cleanup_pending) cleanup_rect = (struct recti){x, y, w, h};
    else {
        int x0 = x < cleanup_rect.x ? x : cleanup_rect.x;
        int y0 = y < cleanup_rect.y ? y : cleanup_rect.y;
        int x1 = x + w > cleanup_rect.x + cleanup_rect.w ? x + w : cleanup_rect.x + cleanup_rect.w;
        int y1 = y + h > cleanup_rect.y + cleanup_rect.h ? y + h : cleanup_rect.y + cleanup_rect.h;
        cleanup_rect = (struct recti){x0, y0, x1 - x0, y1 - y0};
    }
    if (debt > cleanup_debt) cleanup_debt = debt;
    cleanup_pending = 1; cleanup_last_change_ms = now_ms();
}
static int utf8_decode_one(const unsigned char *s, int len, uint32_t *out) {
    if (len <= 0) return 0;
    if (s[0] < 0x80) { *out = s[0]; return 1; }
    if ((s[0] & 0xE0) == 0xC0 && len >= 2 && (s[1] & 0xC0) == 0x80) {
        *out = ((uint32_t)(s[0] & 0x1F) << 6) | (s[1] & 0x3F); return 2;
    }
    if ((s[0] & 0xF0) == 0xE0 && len >= 3 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
        *out = ((uint32_t)(s[0] & 0x0F) << 12) | ((uint32_t)(s[1] & 0x3F) << 6) | (s[2] & 0x3F); return 3;
    }
    if ((s[0] & 0xF8) == 0xF0 && len >= 4 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80 && (s[3] & 0xC0) == 0x80) {
        *out = ((uint32_t)(s[0] & 7) << 18) | ((uint32_t)(s[1] & 0x3F) << 12) |
               ((uint32_t)(s[2] & 0x3F) << 6) | (s[3] & 0x3F); return 4;
    }
    *out = '?'; return 1;
}
static int utf8_encode_one(uint32_t cp, char *out) {
    if (cp <= 0x7F) { out[0] = (char)cp; return 1; }
    if (cp <= 0x7FF) { out[0] = (char)(0xC0 | (cp >> 6)); out[1] = (char)(0x80 | (cp & 63)); return 2; }
    if (cp <= 0xFFFF) {
        out[0] = (char)(0xE0 | (cp >> 12)); out[1] = (char)(0x80 | ((cp >> 6) & 63)); out[2] = (char)(0x80 | (cp & 63)); return 3;
    }
    if (cp > 0x10FFFF) cp = '?';
    out[0] = (char)(0xF0 | (cp >> 18)); out[1] = (char)(0x80 | ((cp >> 12) & 63));
    out[2] = (char)(0x80 | ((cp >> 6) & 63)); out[3] = (char)(0x80 | (cp & 63)); return 4;
}
static char *document_utf8(int *out_len) {
    char *out = malloc((size_t)document_len * 4 + 1); if (!out) return NULL;
    int n = 0;
    for (int i = 0; i < document_len; i++) n += utf8_encode_one(document[i], out + n);
    out[n] = 0; if (out_len) *out_len = n; return out;
}
static void layout_rebuild(void) {
    int row = 0, col = 0;
    for (int i = 0; i < document_len; i++) {
        layout_row[i] = (uint16_t)row; layout_col[i] = (uint16_t)col;
        if (document[i] == '\n') { row++; col = 0; }
        else if (++col >= text_cols) { row++; col = 0; }
    }
    layout_row[document_len] = (uint16_t)row; layout_col[document_len] = (uint16_t)col;
    document_last_row = row;
}
static int ensure_cursor_visible(void) {
    int old = view_top_row, row = layout_row[document_cursor];
    if (row < view_top_row) view_top_row = row;
    else if (row >= view_top_row + text_rows) view_top_row = row - text_rows + 1;
    if (view_top_row < 0) view_top_row = 0;
    return old != view_top_row;
}
static void render_text_layer(void) {
    size_t offset = (size_t)sheet_top * W, bytes = (size_t)(H - sheet_top) * W;
    memcpy(PREV_TEXT_LAYER + offset, TEXT_LAYER + offset, bytes);
    memset(TEXT_LAYER + offset, 0, bytes);
    for (int i = 0; i < document_len; i++) {
        if (document[i] == '\n') continue;
        int row = layout_row[i];
        if (row < view_top_row || row >= view_top_row + text_rows) continue;
        int x = TEXT_LEFT + layout_col[i] * CHAR_STEP;
        int y = text_top + (row - view_top_row) * LINE_STEP;
        text_layer_char(x, y, document[i], TEXT_SCALE);
    }
}
static struct recti cursor_rect(void) {
    int x = TEXT_LEFT + layout_col[document_cursor] * CHAR_STEP - 4;
    int y = text_top + (layout_row[document_cursor] - view_top_row) * LINE_STEP - 3;
    return (struct recti){x, y, 4, 7 * TEXT_SCALE + 6};
}
static int cursor_drawn, retired_cursor_pending;
static struct recti retired_cursor_rect;
static long long cursor_due_ms;
static void draw_cursor_pixels(void) {
    struct recti r = cursor_rect(); fill_rect(r.x, r.y, r.w, r.h, 0x00);
}
static void cursor_retire(void) {
    if (cursor_drawn) {
        struct recti r = cursor_rect();
        retired_cursor_rect = r; retired_cursor_pending = 1;
        cleanup_mark_rect(r.x - 3, r.y - 3, r.w + 6, r.h + 6, 16);
    }
    cursor_drawn = 0;
}
static void cursor_schedule(void) { cursor_due_ms = now_ms() + 70; }
static void cursor_show_if_due(void) {
    if (cursor_drawn || now_ms() < cursor_due_ms) return;
    struct recti r = cursor_rect();
    compose_rect(r.x - 2, r.y - 2, r.w + 4, r.h + 4);
    draw_cursor_pixels(); cursor_drawn = 1;
    quill_swap_mono_fast(r.x - 2, r.y - 2, r.w + 4, r.h + 4);
    cleanup_mark_rect(r.x - 3, r.y - 3, r.w + 6, r.h + 6, 1);
}
static void cleanup_run_if_idle(void) {
    if (!cleanup_pending || now_ms() - cleanup_last_change_ms < 400) return;
    struct recti r = cleanup_rect;
    cleanup_pending = cleanup_debt = 0;
    // Never let background cleanup become a page refresh. Large reflows are
    // left to the normal interactive path rather than blocking input.
    if (r.h > 192 || (long long)r.w * r.h > (long long)W * 192) return;
    compose_rect(r.x, r.y, r.w, r.h);
    if (cursor_drawn) draw_cursor_pixels();
    quill_swap_mono_quality(r.x, r.y, r.w, r.h); // mode 3, full=0 by contract
}
static void document_mark_dirty(void) {
    document_dirty = 1; document_changed_ms = now_ms();
}
static void document_refresh(int dirty_row, int old_view, int old_last_row, int mode, int cleanup_debt) {
    layout_rebuild();
    int view_changed = ensure_cursor_visible();
    render_text_layer();
    int full_repaint = view_changed || old_view != view_top_row;
    int y0, y1;
    if (full_repaint) {
        ink_rebuild_visible();
        y0 = sheet_top; y1 = H;
    } else {
        int first = dirty_row < view_top_row ? view_top_row : dirty_row;
        int last = old_last_row > document_last_row ? old_last_row : document_last_row;
        if ((int)layout_row[document_cursor] > last) last = layout_row[document_cursor];
        if (first >= view_top_row + text_rows) first = view_top_row;
        if (last >= view_top_row + text_rows) last = view_top_row + text_rows - 1;
        if (last < first) last = first;
        y0 = text_top + (first - view_top_row) * LINE_STEP - 4;
        y1 = text_top + (last - view_top_row) * LINE_STEP + 7 * TEXT_SCALE + 5;
        if (y0 < sheet_top) y0 = sheet_top;
        if (y1 > H) y1 = H;
    }
    int x0 = 0, x1 = W - 1;
    if (!full_repaint) {
        x0 = W; x1 = -1; int dy0 = H, dy1 = -1;
        for (int y = y0; y < y1; y++) for (int x = 0; x < W; x++) {
            size_t i = (size_t)y * W + x;
            if (PREV_TEXT_LAYER[i] == TEXT_LAYER[i]) continue;
            if (x < x0) x0 = x; if (x > x1) x1 = x;
            if (y < dy0) dy0 = y; if (y > dy1) dy1 = y;
        }
        if (retired_cursor_pending) {
            struct recti r = retired_cursor_rect;
            if (r.x < x0) x0 = r.x; if (r.x + r.w - 1 > x1) x1 = r.x + r.w - 1;
            if (r.y < dy0) dy0 = r.y; if (r.y + r.h - 1 > dy1) dy1 = r.y + r.h - 1;
        }
        if (x1 < x0 || dy1 < dy0) { retired_cursor_pending = 0; cursor_schedule(); return; }
        x0 -= 3; x1 += 3; y0 = dy0 - 3; y1 = dy1 + 4;
        if (x0 < 0) x0 = 0; if (x1 >= W) x1 = W - 1;
        if (y0 < sheet_top) y0 = sheet_top; if (y1 > H) y1 = H;
    }
    retired_cursor_pending = 0;
    compose_rect(x0, y0, x1 - x0 + 1, y1 - y0);
    int immediate_mode = full_repaint ? 3 : mode;
    if (immediate_mode == 0) quill_swap_mono_fast(x0, y0, x1 - x0 + 1, y1 - y0);
    else quill_swap_mono_quality(x0, y0, x1 - x0 + 1, y1 - y0);
    if (!full_repaint && immediate_mode == 0)
        cleanup_mark_rect(x0, y0, x1 - x0 + 1, y1 - y0, cleanup_debt);
    cursor_schedule();
}
static void document_insert(const uint32_t *chars, int count, int mode, int cleanup_debt) {
    if (count <= 0) return;
    if (count > DOC_MAX - document_len) count = DOC_MAX - document_len;
    if (count <= 0) return;
    int dirty_row = layout_row[document_cursor], old_view = view_top_row, old_last = document_last_row;
    cursor_retire();
    memmove(document + document_cursor + count, document + document_cursor,
            (size_t)(document_len - document_cursor) * sizeof document[0]);
    memcpy(document + document_cursor, chars, (size_t)count * sizeof chars[0]);
    document_cursor += count; document_len += count; preferred_col = -1;
    document_mark_dirty(); document_refresh(dirty_row, old_view, old_last, mode, cleanup_debt);
}
static void document_insert_utf8(const char *text, int trailing_space, int mode) {
    uint32_t chars[4096]; int count = 0, len = (int)strlen(text), off = 0;
    while (off < len && count < (int)(sizeof chars / sizeof chars[0]) - 1) {
        uint32_t cp; int used = utf8_decode_one((const unsigned char *)text + off, len - off, &cp);
        if (!used) break; off += used;
        if (cp == '\r') continue;
        chars[count++] = cp;
    }
    if (trailing_space && count > 0 && chars[count - 1] != ' ' && chars[count - 1] != '\n' && chars[count - 1] != '\t')
        chars[count++] = ' ';
    document_insert(chars, count, mode, mode == 0 ? 2 : 0);
}
static void document_backspace(void) {
    if (document_cursor <= 0) return;
    int at = document_cursor - 1, dirty_row = layout_row[at], old_view = view_top_row, old_last = document_last_row;
    cursor_retire();
    memmove(document + at, document + document_cursor, (size_t)(document_len - document_cursor) * sizeof document[0]);
    document_cursor--; document_len--; preferred_col = -1;
    document_mark_dirty(); document_refresh(dirty_row, old_view, old_last, 0, 16);
}
static void document_delete(void) {
    if (document_cursor >= document_len) return;
    int dirty_row = layout_row[document_cursor], old_view = view_top_row, old_last = document_last_row;
    cursor_retire();
    memmove(document + document_cursor, document + document_cursor + 1,
            (size_t)(document_len - document_cursor - 1) * sizeof document[0]);
    document_len--; preferred_col = -1;
    document_mark_dirty(); document_refresh(dirty_row, old_view, old_last, 0, 16);
}
static int index_at_row_col(int row, int col) {
    int best = -1, best_col = -1;
    for (int i = 0; i <= document_len; i++) if ((int)layout_row[i] == row) {
        int c = layout_col[i];
        if (c <= col && c >= best_col) { best = i; best_col = c; }
        else if (best < 0) best = i;
    }
    return best;
}
static void document_move_cursor(char direction) {
    struct recti old_rect = cursor_rect();
    int old_cursor = document_cursor, was_drawn = cursor_drawn;
    int row = layout_row[document_cursor], col = layout_col[document_cursor];
    if (direction == 'L' && document_cursor > 0) { document_cursor--; preferred_col = -1; }
    else if (direction == 'R' && document_cursor < document_len) { document_cursor++; preferred_col = -1; }
    else if (direction == 'U' || direction == 'D') {
        if (preferred_col < 0) preferred_col = col;
        int target = row + (direction == 'U' ? -1 : 1);
        int found = target >= 0 ? index_at_row_col(target, preferred_col) : -1;
        if (found >= 0) document_cursor = found;
    } else if (direction == 'H') {
        int found = index_at_row_col(row, 0); if (found >= 0) document_cursor = found; preferred_col = -1;
    } else if (direction == 'E') {
        int found = index_at_row_col(row, text_cols); if (found >= 0) document_cursor = found; preferred_col = -1;
    }
    if (document_cursor == old_cursor) return;
    if (was_drawn) cleanup_mark_rect(old_rect.x - 3, old_rect.y - 3, old_rect.w + 6, old_rect.h + 6, 16);
    cursor_drawn = 0;
    int changed_view = ensure_cursor_visible();
    if (changed_view) {
        render_text_layer(); ink_rebuild_visible(); compose_rect(0, sheet_top, W, H - sheet_top);
        quill_swap_mono_quality(0, sheet_top, W, H - sheet_top);
    } else if (was_drawn) {
        // Only the physically visible old caret needs erasing. While arrow
        // events are arriving faster than the debounce, intermediate logical
        // cursor positions never touch glass at all.
        compose_rect(old_rect.x - 2, old_rect.y - 2, old_rect.w + 4, old_rect.h + 4);
        quill_swap_mono_fast(old_rect.x - 2, old_rect.y - 2, old_rect.w + 4, old_rect.h + 4);
    }
    cursor_schedule(); document_mark_dirty();
}
static void document_save(void) {
    int len = 0; char *text = document_utf8(&len); if (!text) return;
    FILE *f = fopen(DOCUMENT_FILE ".tmp", "wb");
    if (f) { fwrite(text, 1, len, f); fclose(f); rename(DOCUMENT_FILE ".tmp", DOCUMENT_FILE); }
    free(text);
    f = fopen(CURSOR_FILE ".tmp", "w");
    if (f) { fprintf(f, "%d\n", document_cursor); fclose(f); rename(CURSOR_FILE ".tmp", CURSOR_FILE); }
    document_dirty = 0;
}
static void document_load(void) {
    FILE *f = fopen(DOCUMENT_FILE, "rb");
    if (f) {
        if (fseek(f, 0, SEEK_END) == 0) {
            long size = ftell(f); rewind(f);
            if (size > 0 && size <= DOC_MAX * 4L) {
                unsigned char *buf = malloc((size_t)size);
                if (buf) {
                    size_t got = fread(buf, 1, (size_t)size, f);
                    for (size_t i = 0; i < got && document_len < DOC_MAX;) {
                        uint32_t cp; int used = utf8_decode_one(buf + i, (int)(got - i), &cp);
                        if (!used) break; document[document_len++] = cp; i += (size_t)used;
                    }
                    free(buf);
                }
            }
        }
        fclose(f);
    }
    document_cursor = document_len;
    f = fopen(CURSOR_FILE, "r");
    if (f) { int c; if (fscanf(f, "%d", &c) == 1 && c >= 0 && c <= document_len) document_cursor = c; fclose(f); }
    layout_rebuild(); ensure_cursor_visible();
}
static void document_initial_render(void) {
    layout_rebuild(); ensure_cursor_visible(); render_text_layer(); ink_rebuild_visible();
    compose_rect(0, sheet_top, W, H - sheet_top); draw_cursor_pixels(); cursor_drawn = 1;
}
static void live_key_command(const char *text) {
    while (*text == ' ') text++;
    if (*text == 'C') { uint32_t cp = (uint32_t)strtoul(text + 1, NULL, 10); document_insert(&cp, 1, 0, 2); }
    else if (*text == 'N') { uint32_t cp = '\n'; document_insert(&cp, 1, 0, 2); }
    else if (*text == 'T') { uint32_t spaces[4] = {' ',' ',' ',' '}; document_insert(spaces, 4, 0, 2); }
    else if (*text == 'B') document_backspace();
    else if (*text == 'X') document_delete();
    else if (strchr("LRUDHE", *text)) document_move_cursor(*text);
}
static void add_wrapped(const char *text) { document_insert_utf8(text, 1, 3); }

// ------------------------------------------------------------------- network
static const char PAGE[] =
    "<!doctype html><meta charset=utf-8>"
    "<meta name=viewport content=\"width=device-width,initial-scale=1\">"
    "<title>Voicepad for Android</title>"
    "<style>body{font:17px system-ui,sans-serif;max-width:42em;margin:2em auto;padding:0 1em;color:#222}"
    "h1{text-align:center}.b,button{display:block;box-sizing:border-box;width:100%;margin:1em 0;padding:.9em;"
    "border:0;border-radius:14px;background:#202124;color:white;text-align:center;text-decoration:none;font-size:1.15em}"
    "button{background:#1769aa}li{margin:.65em 0}.note{padding:1em;background:#f1f3f4;border-radius:12px}code{word-break:break-all}</style>"
    "<h1>Voicepad for Android</h1>"
    "<p>The native companion has real microphone permission and talks directly to this tablet over Wi-Fi.</p>"
    "<a class=b href=/voicepad.apk download>1. Download the Android app</a>"
    "<button onclick=pair()>2. Pair and open the installed app</button>"
    "<ol><li>Open the downloaded APK. If Android asks, allow this browser to install unknown apps.</li>"
    "<li>Tap <b>Open</b>; the app finds this tablet automatically. If needed, return here and tap <b>Pair and open</b>.</li>"
    "<li>Grant microphone access and tap <b>Start microphone</b>.</li></ol>"
    "<p class=note>Keep both devices on the same Wi-Fi. Tablet address: <code id=a></code></p>"
    "<script>document.getElementById('a').textContent=location.host;"
    "function pair(){location.href='intent://pair?host='+encodeURIComponent(location.host)"
    "+'#Intent;scheme=voicepad;package=works.earendil.voicepad;end'}</script>";

struct conn { int fd; int used; int raw; char buf[8192]; };
static struct conn conns[MAX_CONNS];
static void remote_draw_command(const char *text);

static int listen_sock(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0); if (fd < 0) return -1;
    int one = 1; setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a; memset(&a, 0, sizeof a); a.sin_family = AF_INET; a.sin_port = htons(PORT); a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr *)&a, sizeof a) || listen(fd, MAX_CONNS)) { close(fd); return -1; }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    return fd;
}
static int discovery_sock(void) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0); if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof one);
    struct sockaddr_in a; memset(&a, 0, sizeof a); a.sin_family = AF_INET; a.sin_port = htons(PORT); a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr *)&a, sizeof a)) { close(fd); return -1; }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    return fd;
}
static void conn_close(struct conn *c) { close(c->fd); c->fd = -1; c->used = 0; c->raw = 0; }
static void send_all(int fd, const char *data, int len) {
    int off = 0, spins = 0;
    while (off < len && spins < 3000) {
        ssize_t n = write(fd, data + off, len - off);
        if (n > 0) off += (int)n;
        else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) { usleep(1000); spins++; }
        else break;
    }
}
static void http_respond(struct conn *c, const char *status, const char *type, const char *body, int len) {
    char hdr[256];
    int hl = snprintf(hdr, sizeof hdr,
        "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %d\r\nConnection: close\r\nCache-Control: no-store\r\n\r\n",
        status, type, len);
    send_all(c->fd, hdr, hl);
    if (len > 0) send_all(c->fd, body, len);
    conn_close(c);
}
static void http_respond_apk(struct conn *c) {
    int apk = open("voicepad.apk", O_RDONLY);
    struct stat st;
    if (apk < 0 || fstat(apk, &st) != 0) {
        if (apk >= 0) close(apk);
        const char *msg = "voicepad.apk is not installed in this bundle\n";
        http_respond(c, "404 Not Found", "text/plain; charset=utf-8", msg, (int)strlen(msg));
        return;
    }
    char hdr[320];
    int hl = snprintf(hdr, sizeof hdr,
        "HTTP/1.1 200 OK\r\nContent-Type: application/vnd.android.package-archive\r\n"
        "Content-Disposition: attachment; filename=voicepad.apk\r\nContent-Length: %lld\r\n"
        "Connection: close\r\nCache-Control: no-store\r\n\r\n", (long long)st.st_size);
    send_all(c->fd, hdr, hl);
    char chunk[16384]; ssize_t n;
    while ((n = read(apk, chunk, sizeof chunk)) > 0) send_all(c->fd, chunk, (int)n);
    close(apk);
    conn_close(c);
}
// Returns 1 if the connection produced transcript text to render.
static int conn_process(struct conn *c) {
    int drew = 0;
    if (!c->raw && c->used >= 4) {
        if (memcmp(c->buf, "GET ", 4) && memcmp(c->buf, "POST", 4)) c->raw = 1;
    }
    if (c->raw) {
        char *start = c->buf, *nl;
        while ((nl = strchr(start, '\n'))) {
            *nl = 0;
            if (*start) { add_wrapped(start); drew = 1; }
            start = nl + 1;
        }
        c->used = (int)strlen(start); memmove(c->buf, start, c->used + 1);
        return drew;
    }
    if (c->used < 4) return 0;
    char *hend = strstr(c->buf, "\r\n\r\n");
    if (!hend) { if (c->used >= (int)sizeof(c->buf) - 1) conn_close(c); return 0; }
    char method[8] = "", path[256] = "";
    sscanf(c->buf, "%7s %255s", method, path);
    if (!strcmp(method, "GET")) {
        if (!strcmp(path, "/") || !strncmp(path, "/?", 2))
            http_respond(c, "200 OK", "text/html; charset=utf-8", PAGE, (int)(sizeof PAGE - 1));
        else if (!strcmp(path, "/voicepad.apk"))
            http_respond_apk(c);
        else if (!strcmp(path, "/document")) {
            int len = 0; char *body = document_utf8(&len);
            if (body) { http_respond(c, "200 OK", "text/plain; charset=utf-8", body, len); free(body); }
            else { const char *no = "allocation failed\n"; http_respond(c, "500 Internal Server Error", "text/plain", no, (int)strlen(no)); }
        } else if (!strcmp(path, "/state")) {
            char state[256]; int len = snprintf(state, sizeof state,
                "{\"length\":%d,\"cursor\":%d,\"row\":%u,\"column\":%u,\"viewRow\":%d,\"inkSegments\":%d}\n",
                document_len, document_cursor, layout_row[document_cursor], layout_col[document_cursor], view_top_row, ink_segment_count);
            http_respond(c, "200 OK", "application/json", state, len);
        } else if (!strcmp(path, "/health")) {
            const char *ok = "voicepad 0.4.2 partial-only-cleanup\n";
            http_respond(c, "200 OK", "text/plain; charset=utf-8", ok, (int)strlen(ok));
        } else {
            const char *no = "not found\n";
            http_respond(c, "404 Not Found", "text/plain; charset=utf-8", no, (int)strlen(no));
        }
        return 0;
    }
    if (strcmp(method, "POST")) {
        const char *no = "method not allowed\n";
        http_respond(c, "405 Method Not Allowed", "text/plain; charset=utf-8", no, (int)strlen(no));
        return 0;
    }
    int clen = 0;
    for (char *h = c->buf; h < hend; h++)
        if (!strncasecmp(h, "content-length:", 15)) { clen = atoi(h + 15); break; }
    if (clen < 0 || clen >= (int)sizeof(c->buf) - 1) { conn_close(c); return 0; }
    char *body = hend + 4;
    int have = c->used - (int)(body - c->buf);
    if (have < clen) return 0;
    char text[4096]; int n = clen < (int)sizeof text - 1 ? clen : (int)sizeof text - 1;
    memcpy(text, body, n); text[n] = 0;
    if (!strcmp(path, "/draw")) {
        remote_draw_command(text);
    } else if (!strcmp(path, "/key")) {
        live_key_command(text);
    } else if (!strcmp(path, "/say") || !strcmp(path, "/")) {
        if (n > 0) { add_wrapped(text); drew = 1; }
    } else {
        const char *no = "not found\n";
        http_respond(c, "404 Not Found", "text/plain; charset=utf-8", no, (int)strlen(no));
        return drew;
    }
    const char *done = "HTTP/1.1 204 No Content\r\nConnection: close\r\n\r\n";
    send_all(c->fd, done, (int)strlen(done));
    conn_close(c);
    return drew;
}

static void my_url(char *out, int outsz) {
    const char *env = getenv("VOICEPAD_URL");
    if (env && *env) { snprintf(out, outsz, "%s", env); return; }
    char best[64] = "10.11.99.1"; int best_rank = -1;
    struct ifaddrs *ifs = NULL;
    if (getifaddrs(&ifs) == 0) {
        for (struct ifaddrs *a = ifs; a; a = a->ifa_next) {
            if (!a->ifa_addr || a->ifa_addr->sa_family != AF_INET) continue;
            if (!strcmp(a->ifa_name, "lo")) continue;
            int rank = strncmp(a->ifa_name, "wlan", 4) == 0 ? 2 : (strncmp(a->ifa_name, "usb", 3) == 0 ? 1 : 0);
            if (rank > best_rank) {
                best_rank = rank;
                inet_ntop(AF_INET, &((struct sockaddr_in *)a->ifa_addr)->sin_addr, best, sizeof best);
            }
        }
        freeifaddrs(ifs);
    }
    snprintf(out, outsz, "http://%s:%d/", best, PORT);
}

// --------------------------------------------------------------------- input
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
        if (fd >= 0) { int one = 1; ioctl(fd, EVIOCGRAB, &one); }
        return fd;
    }
    return -1;
}
static void drain_nonpen(int pwr_fd, int touch_fd) {
    struct input_event evs[64];
    if (pwr_fd >= 0) { ssize_t n; while ((n = read(pwr_fd, evs, sizeof evs)) > 0) for (int i = 0; i < (int)(n / sizeof evs[0]); i++) if (evs[i].type == EV_KEY && evs[i].code == KEY_POWER && evs[i].value == 1) g_quit = 1; }
    static int slot_active[MAX_SLOTS] = {0}; static int cur_slot = 0;
    if (touch_fd >= 0) { ssize_t n; while ((n = read(touch_fd, evs, sizeof evs)) > 0) for (int i = 0; i < (int)(n / sizeof evs[0]); i++) {
        if (evs[i].type == EV_ABS && evs[i].code == ABS_MT_SLOT) cur_slot = evs[i].value < 0 ? 0 : (evs[i].value >= MAX_SLOTS ? MAX_SLOTS-1 : evs[i].value);
        else if (evs[i].type == EV_ABS && evs[i].code == ABS_MT_TRACKING_ID) { slot_active[cur_slot] = evs[i].value != -1; int fingers = 0; for (int s = 0; s < MAX_SLOTS; s++) fingers += slot_active[s]; if (fingers >= 5) g_quit = 1; }
    }}
}

// Manuscript ink is recorded in document-space segments rather than baked
// into text pixels. `tool` deliberately distinguishes pen from eraser so a
// later recognizer can reinterpret eraser gestures as deletion annotations.
#define INK_SEGMENT_MAX 100000
struct ink_segment {
    int32_t x0, y0, x1, y1;
    uint16_t radius;
    uint8_t tool, reserved;
    uint32_t stroke;
};
static struct ink_segment ink_segments[INK_SEGMENT_MAX];
static uint32_t next_stroke_id = 1;

static void ink_pixel(int x, int y, int black) {
    if (x < 0 || y < sheet_top || x >= W || y >= H) return;
    INK_LAYER[(size_t)y * W + x] = black ? 1 : 2;
    put_px(x, y, black ? 0x00 : 0xFF);
}
static void stamp(int cx, int cy, int r, int black) {
    for (int dy = -r; dy <= r; dy++) for (int dx = -r; dx <= r; dx++)
        if (dx * dx + dy * dy <= r * r) ink_pixel(cx + dx, cy + dy, black);
}
static void ink_line(int x0, int y0, int x1, int y1, int r, int black) {
    int dx = abs(x1 - x0), dy = abs(y1 - y0);
    int steps = dx > dy ? dx : dy; if (steps < 1) steps = 1;
    for (int i = 0; i <= steps; i++) stamp(x0 + (x1 - x0) * i / steps, y0 + (y1 - y0) * i / steps, r, black);
}
static void ink_record_segment(int x0, int y0, int x1, int y1, int r, int black, uint32_t stroke) {
    if (ink_segment_count < INK_SEGMENT_MAX) {
        int offset = view_top_row * LINE_STEP;
        ink_segments[ink_segment_count++] = (struct ink_segment){
            x0, y0 + offset, x1, y1 + offset, (uint16_t)r, (uint8_t)(black ? 0 : 1), 0, stroke
        };
    }
    ink_line(x0, y0, x1, y1, r, black);
}
static void ink_rebuild_visible(void) {
    if (!INK_LAYER) return;
    memset(INK_LAYER + (size_t)sheet_top * W, 0, (size_t)(H - sheet_top) * W);
    int offset = view_top_row * LINE_STEP;
    for (int i = 0; i < ink_segment_count; i++) {
        struct ink_segment *s = &ink_segments[i];
        int y0 = s->y0 - offset, y1 = s->y1 - offset, r = s->radius;
        int lo = y0 < y1 ? y0 : y1, hi = y0 > y1 ? y0 : y1;
        if (hi + r < sheet_top || lo - r >= H) continue;
        ink_line(s->x0, y0, s->x1, y1, r, s->tool == 0);
    }
}
static void dirty_add(int *x0, int *y0, int *x1, int *y1, int x, int y, int m) {
    if (x - m < *x0) *x0 = x - m;
    if (y - m < *y0) *y0 = y - m;
    if (x + m > *x1) *x1 = x + m;
    if (y + m > *y1) *y1 = y + m;
}

static int remote_lx = -1, remote_ly = -1;
static uint32_t remote_stroke;
static void remote_draw_command(const char *text) {
    char action = 0; int nx = 0, ny = 0, eraser = 0;
    if (sscanf(text, " %c %d %d %d", &action, &nx, &ny, &eraser) < 1) return;
    if (action == 'U') { remote_lx = remote_ly = -1; return; }
    if (action != 'D' && action != 'M') return;
    if (action == 'D' || remote_lx < 0) remote_stroke = next_stroke_id++;
    if (nx < 0) nx = 0; if (nx > 10000) nx = 10000;
    if (ny < 0) ny = 0; if (ny > 10000) ny = 10000;
    int x = nx * (W - 1) / 10000;
    int y = sheet_top + ny * (H - sheet_top - 1) / 10000;
    int r = eraser ? 24 : 4;
    int x0 = 1 << 30, y0 = 1 << 30, x1 = -1, y1 = -1;
    if (action == 'M' && remote_lx >= 0) {
        ink_record_segment(remote_lx, remote_ly, x, y, r, !eraser, remote_stroke);
        dirty_add(&x0, &y0, &x1, &y1, remote_lx, remote_ly, r + 2);
    } else {
        ink_record_segment(x, y, x, y, r, !eraser, remote_stroke);
    }
    dirty_add(&x0, &y0, &x1, &y1, x, y, r + 2);
    if (x0 < 0) x0 = 0; if (y0 < sheet_top) y0 = sheet_top;
    if (x1 >= W) x1 = W - 1; if (y1 >= H) y1 = H - 1;
    quill_swap_mono_fast(x0, y0, x1 - x0 + 1, y1 - y0 + 1);
    cleanup_mark_rect(x0, y0, x1 - x0 + 1, y1 - y0 + 1, eraser ? 16 : 1);
    remote_lx = x; remote_ly = y;
}

int main(void) {
    signal(SIGTERM, on_term); signal(SIGINT, on_term); signal(SIGPIPE, SIG_IGN);
    if (quill_init() != 0) return 1;
    W = quill_width(); H = quill_height(); STRIDE = quill_stride(); BPP = STRIDE / (W ? W : 1); FB = quill_buffer();
    fprintf(stderr, "voicepad: %dx%d stride %d bpp %d fmt %d\n", W, H, STRIDE, BPP, quill_format());
    size_t pixels = (size_t)W * H;
    BASE_LAYER = calloc(pixels, 1); TEXT_LAYER = calloc(pixels, 1);
    PREV_TEXT_LAYER = calloc(pixels, 1); INK_LAYER = calloc(pixels, 1);
    if (!FB || !BASE_LAYER || !TEXT_LAYER || !PREV_TEXT_LAYER || !INK_LAYER) {
        fprintf(stderr, "voicepad: layer allocation failed\n"); return 1;
    }

    // header: title left, QR right, rule underneath — drawn once
    memset(FB, 0xFF, (size_t)STRIDE * H);
    char url[128]; my_url(url, sizeof url);
    int qpx = 8, qs = qr_encode(url);
    if (!qs) { snprintf(url, sizeof url, "http://10.11.99.1:%d/", PORT); qs = qr_encode(url); }
    int qw = qs * qpx;
    draw_text(70, 80, "VOICEPAD", 8);
    draw_text(70, 170, "scan to install and pair the Android app", 3);
    draw_text(70, 210, url, 3);
    qr_draw(W - 70 - qw, 55, qpx);
    sheet_top = 55 + qw + 40;
    if (sheet_top < 290) sheet_top = 290;
    fill_rect(60, sheet_top - 12, W - 120, 4, 0x00);
    text_cols = (W - TEXT_LEFT - TEXT_RIGHT) / CHAR_STEP;
    text_top = sheet_top + 24;
    text_rows = (H - 40 - 7 * TEXT_SCALE - text_top) / LINE_STEP + 1;
    if (text_cols < 1) text_cols = 1; if (text_rows < 1) text_rows = 1;
    capture_base_layer();
    document_load();
    document_initial_render();
    fprintf(stderr, "voicepad: document chars=%d cursor=%d grid=%dx%d view_row=%d\n",
            document_len, document_cursor, text_cols, text_rows, view_top_row);
    quill_swap(0, 0, W, H, 3, 1);

    int srv = listen_sock(); if (srv < 0) { perror("listen"); return 1; }
    int udp = discovery_sock();
    for (int i = 0; i < MAX_CONNS; i++) conns[i].fd = -1;
    int pen_fd = open_input("marker"), pwr_fd = open_input("powerkey"), touch_fd = open_input("touch");
    fprintf(stderr, "voicepad: url %s pen_fd %d\n", url, pen_fd);

    int rx = 0, ry = 0, pressure = 0, touching = 0, eraser = 0, have = 0, lx = -1, ly = -1;
    uint32_t pen_stroke = 0;
    int dx0 = 1 << 30, dy0 = 1 << 30, dx1 = -1, dy1 = -1, dirty_debt = 0;
    struct timeval last_flush = {0, 0};

    while (!g_quit) {
        struct pollfd pfds[5 + MAX_CONNS] = {
            {pen_fd, POLLIN, 0}, {pwr_fd, POLLIN, 0}, {touch_fd, POLLIN, 0},
            {srv, POLLIN, 0}, {udp, POLLIN, 0},
        };
        for (int i = 0; i < MAX_CONNS; i++) { pfds[5 + i].fd = conns[i].fd; pfds[5 + i].events = POLLIN; }
        poll(pfds, 5 + MAX_CONNS, 4);
        drain_nonpen(pwr_fd, touch_fd);

        // Pen pixels and tool-tagged segments are separate from editable text.
        // The eraser is currently a persistent white mask; its recorded tool
        // identity allows later reinterpretation as a deletion annotation.
        if (pen_fd >= 0) {
            struct input_event evs[96];
            ssize_t n = read(pen_fd, evs, sizeof evs);
            for (int i = 0; i < (int)((n > 0 ? n : 0) / sizeof evs[0]); i++) {
                struct input_event *e = &evs[i];
                if (e->type == EV_ABS && e->code == ABS_X) { rx = e->value; have = 1; }
                else if (e->type == EV_ABS && e->code == ABS_Y) { ry = e->value; have = 1; }
                else if (e->type == EV_ABS && e->code == ABS_PRESSURE) { pressure = e->value; have = 1; }
                else if (e->type == EV_KEY && e->code == BTN_TOOL_RUBBER) eraser = e->value;
                else if (e->type == EV_KEY && e->code == BTN_TOUCH) { touching = e->value; have = 1; }
                else if (e->type == EV_SYN && e->code == SYN_REPORT && have) {
                    have = 0;
                    int x = (int)((int64_t)rx * (W - 1) / DIGI_MAX_X);
                    int y = (int)((int64_t)ry * (H - 1) / DIGI_MAX_Y);
                    if (touching && pressure > 40) {
                        int r = eraser ? 22 : 2 + pressure * 3 / 4096;
                        if (lx < 0) pen_stroke = next_stroke_id++;
                        ink_record_segment(lx >= 0 ? lx : x, ly >= 0 ? ly : y, x, y, r, !eraser, pen_stroke);
                        if (eraser) dirty_debt = 16; else if (dirty_debt < 1) dirty_debt = 1;
                        dirty_add(&dx0, &dy0, &dx1, &dy1, x, y, r + 2);
                        if (lx >= 0) dirty_add(&dx0, &dy0, &dx1, &dy1, lx, ly, r + 2);
                        lx = x; ly = y;
                    } else lx = ly = -1;
                }
            }
        }
        if (dx1 >= 0) {
            struct timeval now; gettimeofday(&now, NULL);
            long ms = (now.tv_sec - last_flush.tv_sec) * 1000 + (now.tv_usec - last_flush.tv_usec) / 1000;
            if (ms >= 8) {
                if (dx0 < 0) dx0 = 0; if (dy0 < 0) dy0 = 0; if (dx1 >= W) dx1 = W - 1; if (dy1 >= H) dy1 = H - 1;
                quill_swap_mono_fast(dx0, dy0, dx1 - dx0 + 1, dy1 - dy0 + 1);
                cleanup_mark_rect(dx0, dy0, dx1 - dx0 + 1, dy1 - dy0 + 1, dirty_debt);
                dx0 = dy0 = 1 << 30; dx1 = dy1 = -1; dirty_debt = 0; last_flush = now;
            }
        }

        // network: discovery, accept, and service companion connections
        if (udp >= 0 && (pfds[4].revents & POLLIN)) {
            char request[128]; struct sockaddr_in peer; socklen_t peer_len = sizeof peer; ssize_t n;
            while ((n = recvfrom(udp, request, sizeof request - 1, 0, (struct sockaddr *)&peer, &peer_len)) > 0) {
                request[n] = 0;
                if (!strncmp(request, "VOICEPAD_DISCOVER_V1", 20)) {
                    char reply[256]; int rn = snprintf(reply, sizeof reply, "VOICEPAD_HERE_V1 %s", url);
                    sendto(udp, reply, rn, 0, (struct sockaddr *)&peer, peer_len);
                }
                peer_len = sizeof peer;
            }
        }
        if (pfds[3].revents & POLLIN) {
            int c = accept(srv, NULL, NULL);
            if (c >= 0) {
                fcntl(c, F_SETFL, fcntl(c, F_GETFL) | O_NONBLOCK);
                int slot = -1;
                for (int i = 0; i < MAX_CONNS; i++) if (conns[i].fd < 0) { slot = i; break; }
                if (slot < 0) { conn_close(&conns[0]); slot = 0; }
                conns[slot].fd = c; conns[slot].used = 0; conns[slot].raw = 0; conns[slot].buf[0] = 0;
            }
        }
        for (int i = 0; i < MAX_CONNS; i++) {
            struct conn *c = &conns[i];
            if (c->fd < 0 || !(pfds[5 + i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            ssize_t n = read(c->fd, c->buf + c->used, sizeof(c->buf) - c->used - 1);
            if (n <= 0) {
                if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;
                if (c->raw && c->used > 0) { add_wrapped(c->buf); }
                conn_close(c);
                continue;
            }
            c->used += (int)n; c->buf[c->used] = 0;
            conn_process(c);
        }
        cursor_show_if_due();
        cleanup_run_if_idle();
        if (document_dirty && now_ms() - document_changed_ms >= 1000) document_save();
        quill_process_events();
    }
    if (document_dirty) document_save();
    return 0;
}
