/*
 * Claude Traffic Light - floating status indicator for Windows
 * ------------------------------------------------------------
 * Shows a small traffic light on the edge of the screen when you switch
 * away from Claude, and whenever Claude changes state.
 *
 *   RED    -> Claude is waiting for your confirmation
 *   YELLOW -> Claude is working
 *   GREEN  -> done, ready for a new task
 *
 * Plain Win32: no runtime, no dependencies, ~40 KB, 0% CPU when idle.
 *
 * MIT licensed. See LICENSE.
 */

#define UNICODE
#define _UNICODE
#define WINVER 0x0601
#define _WIN32_WINNT 0x0601

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shellapi.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Constantes                                                          */
/* ------------------------------------------------------------------ */

#define WND_CLASS      L"SemaforoClaudeWnd"
#define APP_MUTEX      L"Local\\SemaforoClaudeSingleton"
#define WM_TRAY        (WM_APP + 1)
#define WM_SETSTATE    (WM_APP + 2)
#define TIMER_ANIM     1
#define TIMER_RAISE    2
#define TIMER_WATCH    3

#define ST_WAITING 0   /* rojo    */
#define ST_RUNNING 1   /* amarillo*/
#define ST_DONE    2   /* verde   */

#define PH_HIDDEN  0
#define PH_IN      1
#define PH_HOLD    2
#define PH_OUT     3

#define ID_SHOW      1001
#define ID_AUTOSTART 1002
#define ID_EXIT      1003
#define ID_TEST_R    1010
#define ID_TEST_Y    1011
#define ID_TEST_G    1012

/* lienzo base (se escala por DPI y por el ajuste del ini) */
#define BASE_W 84
#define BASE_H 292

/* ------------------------------------------------------------------ */
/* Estado global                                                       */
/* ------------------------------------------------------------------ */

static HINSTANCE g_inst;
static HWND      g_hwnd;
static HDC       g_memdc;
static HBITMAP   g_bmp[3];
static void     *g_bits[3];
static HBITMAP   g_oldbmp;
static HICON     g_trayicon;

static int  g_w, g_h;                 /* tamano final en pixeles       */
static int  g_state       = ST_DONE;
static int  g_phase       = PH_HIDDEN;
static int  g_alpha       = 0;        /* 0..255                        */
static DWORD g_holdStart  = 0;
static BOOL g_claudeFocus = FALSE;
static BOOL g_autostart   = FALSE;
static DWORD g_watchUntil = 0;   /* 0 = sin vigilancia */

/* configuracion */
static int   g_port          = 8787;
static int   g_holdMs        = 4000;
static int   g_fadeInMs      = 200;
static int   g_fadeOutMs     = 450;
static int   g_scalePct      = 75;
static int   g_verticalPct   = 68;   /* 0 = arriba, 50 = centro, 100 = abajo */
static int   g_margin        = 26;
static int   g_showWhenFocus = 0;
static int   g_showOnBlur    = 1;
static int   g_maxAlpha      = 240;
static WCHAR g_position[32]  = L"right";
static WCHAR g_match[128]    = L"claude";
static WCHAR g_iniPath[MAX_PATH];

/* ------------------------------------------------------------------ */
/* Utilidades                                                          */
/* ------------------------------------------------------------------ */

static double clampd(double v, double a, double b)
{
    return v < a ? a : (v > b ? b : v);
}

static void exeDir(WCHAR *out, size_t n)
{
    GetModuleFileNameW(NULL, out, (DWORD)n);
    WCHAR *p = wcsrchr(out, L'\\');
    if (p) *(p + 1) = 0;
}

static void lowerW(WCHAR *s)
{
    for (; *s; ++s)
        if (*s >= L'A' && *s <= L'Z') *s = (WCHAR)(*s + 32);
}

/* ------------------------------------------------------------------ */
/* Configuracion (semaforo.ini junto al .exe)                          */
/* ------------------------------------------------------------------ */

static void loadConfig(void)
{
    WCHAR dir[MAX_PATH];
    exeDir(dir, MAX_PATH);

    /* nombre nuevo primero; si no esta, se acepta el viejo */
    _snwprintf(g_iniPath, MAX_PATH, L"%straffic-light.ini", dir);
    g_iniPath[MAX_PATH - 1] = 0;
    if (GetFileAttributesW(g_iniPath) == INVALID_FILE_ATTRIBUTES) {
        _snwprintf(g_iniPath, MAX_PATH, L"%ssemaforo.ini", dir);
        g_iniPath[MAX_PATH - 1] = 0;
        if (GetFileAttributesW(g_iniPath) == INVALID_FILE_ATTRIBUTES)
            return; /* sin ini: valores por defecto */
    }

    /* Se leen las claves en espanol y despues las mismas en ingles, en la
       seccion [semaforo] y en [traffic-light]. Gana la ultima que exista,
       asi los dos juegos de nombres funcionan y ninguno rompe al otro. */
    #define CFG_INT(var, es, en)                                              \
        var = GetPrivateProfileIntW(L"semaforo",      es, var, g_iniPath);    \
        var = GetPrivateProfileIntW(L"semaforo",      en, var, g_iniPath);    \
        var = GetPrivateProfileIntW(L"traffic-light", es, var, g_iniPath);    \
        var = GetPrivateProfileIntW(L"traffic-light", en, var, g_iniPath);

    CFG_INT(g_port,          L"puerto",           L"port")
    CFG_INT(g_holdMs,        L"duracion_ms",      L"duration_ms")
    CFG_INT(g_fadeInMs,      L"fade_in_ms",       L"fade_in_ms")
    CFG_INT(g_fadeOutMs,     L"fade_out_ms",      L"fade_out_ms")
    CFG_INT(g_scalePct,      L"tamano_pct",       L"size_pct")
    CFG_INT(g_verticalPct,   L"altura_pct",       L"vertical_pct")
    CFG_INT(g_margin,        L"margen",           L"margin")
    CFG_INT(g_showWhenFocus, L"mostrar_con_foco", L"show_when_focused")
    CFG_INT(g_showOnBlur,    L"mostrar_al_salir", L"show_on_blur")
    CFG_INT(g_maxAlpha,      L"opacidad",         L"opacity")
    #undef CFG_INT

    /* Para las cadenas se usa un centinela: si la clave no existe, el valor
       anterior se mantiene. Nunca se pasa el mismo buffer como origen y
       destino, que es comportamiento indefinido. */
    #define CFG_STR(buf, n, sec, key)                                          \
        {                                                                      \
            WCHAR tmp_[256];                                                   \
            GetPrivateProfileStringW(sec, key, L"\x01", tmp_, 256, g_iniPath); \
            if (tmp_[0] != 1) { lstrcpynW(buf, tmp_, n); }                     \
        }

    lstrcpynW(g_position, L"right",  32);
    lstrcpynW(g_match,    L"claude", 128);

    CFG_STR(g_position, 32,  L"semaforo",      L"posicion")
    CFG_STR(g_position, 32,  L"semaforo",      L"position")
    CFG_STR(g_position, 32,  L"traffic-light", L"posicion")
    CFG_STR(g_position, 32,  L"traffic-light", L"position")

    CFG_STR(g_match, 128, L"semaforo",      L"titulo")
    CFG_STR(g_match, 128, L"semaforo",      L"title_match")
    CFG_STR(g_match, 128, L"traffic-light", L"titulo")
    CFG_STR(g_match, 128, L"traffic-light", L"title_match")
    #undef CFG_STR

    lowerW(g_position);
    lowerW(g_match);

    if (g_verticalPct < 0)   g_verticalPct = 0;
    if (g_verticalPct > 100) g_verticalPct = 100;
    if (g_scalePct < 40)   g_scalePct = 40;
    if (g_scalePct > 400)  g_scalePct = 400;
    if (g_holdMs   < 300)  g_holdMs   = 300;
    if (g_holdMs   > 60000) g_holdMs  = 60000;
    if (g_maxAlpha < 30)   g_maxAlpha = 30;
    if (g_maxAlpha > 255)  g_maxAlpha = 255;
    if (g_fadeInMs  < 1)   g_fadeInMs  = 1;
    if (g_fadeOutMs < 1)   g_fadeOutMs = 1;
    if (g_port < 1 || g_port > 65535) g_port = 8787;
}

static BOOL isClaudeWindow(HWND h);   /* definida mas abajo */

/* ------------------------------------------------------------------ */
/* Dibujo (BGRA premultiplicado, top-down)                             */
/* ------------------------------------------------------------------ */

typedef struct { double r, g, b; } RGB3;

/* colores de las lentes encendidas */
static const RGB3 COL[3] = {
    { 236.0,  38.0,  36.0 },   /* rojo     */
    { 250.0, 202.0,  22.0 },   /* amarillo */
    {  74.0, 216.0,  46.0 }    /* verde    */
};

/* composicion "source-over" sobre buffer premultiplicado */
static void blendPx(unsigned char *p, double r, double g, double b, double a)
{
    if (a <= 0.0) return;
    if (a > 1.0) a = 1.0;
    double ia = 1.0 - a;
    p[0] = (unsigned char)clampd(b * a + p[0] * ia, 0, 255);
    p[1] = (unsigned char)clampd(g * a + p[1] * ia, 0, 255);
    p[2] = (unsigned char)clampd(r * a + p[2] * ia, 0, 255);
    p[3] = (unsigned char)clampd(255.0 * a + p[3] * ia, 0, 255);
}

/* suma aditiva (brillos y halos), tambien premultiplicada */
static void addPx(unsigned char *p, double r, double g, double b, double a)
{
    if (a <= 0.0) return;
    p[0] = (unsigned char)clampd(p[0] + b * a, 0, 255);
    p[1] = (unsigned char)clampd(p[1] + g * a, 0, 255);
    p[2] = (unsigned char)clampd(p[2] + r * a, 0, 255);
    p[3] = (unsigned char)clampd(p[3] + 255.0 * a, 0, 255);
}

static double covRoundRect(double x, double y, double cx, double cy,
                           double hw, double hh, double rad)
{
    double dx = fabs(x - cx) - (hw - rad); if (dx < 0) dx = 0;
    double dy = fabs(y - cy) - (hh - rad); if (dy < 0) dy = 0;
    double d = sqrt(dx * dx + dy * dy) - rad;
    return clampd(0.5 - d, 0.0, 1.0);
}

static double covCircle(double x, double y, double cx, double cy, double rad)
{
    double dx = x - cx, dy = y - cy;
    double d = sqrt(dx * dx + dy * dy) - rad;
    return clampd(0.5 - d, 0.0, 1.0);
}

/* ------------------------------------------------------------------
 * Semaforo "de calle": cuerpo metalico oscuro, tapa superior,
 * visera curva sobre cada lente, lentes de vidrio con brillo y poste.
 * Todas las medidas estan en unidades del lienzo base (BASE_W x BASE_H)
 * y se escalan solas.
 * ------------------------------------------------------------------ */
static void renderLight(unsigned char *buf, int w, int h, int active)
{
    const double s  = (double)w / (double)BASE_W;   /* escala */
    const double cx = w / 2.0;

    /* geometria (en unidades base, multiplicadas por s) */
    const double bodyTop = 16.0 * s, bodyBot = 232.0 * s;
    const double bodyHW  = 31.0 * s;
    const double bodyCY  = (bodyTop + bodyBot) / 2.0;
    const double bodyHH  = (bodyBot - bodyTop) / 2.0;
    const double bodyRad = 7.0 * s;

    const double capTop = 5.0 * s, capBot = 19.0 * s;
    const double capHW  = 34.0 * s, capRad = 3.5 * s;

    const double lensR  = 21.5 * s;
    const double lensY[3] = { 62.0 * s, 124.0 * s, 186.0 * s };

    const double hoodIn  = 23.2 * s;     /* radio interno de la visera */
    const double hoodOut = 33.4 * s;     /* radio externo              */
    const double hoodDrop = 0.20;        /* cuanto baja por los lados  */

    const double poleHW  = 5.0 * s;
    const double poleTop = 230.0 * s, poleBot = (double)h;

    const double glowR = 46.0 * s;

    memset(buf, 0, (size_t)w * h * 4);

    for (int y = 0; y < h; ++y) {
        unsigned char *row = buf + (size_t)y * w * 4;
        const double py = y + 0.5;

        for (int x = 0; x < w; ++x) {
            unsigned char *p = row + x * 4;
            const double px_ = x + 0.5;
            const double dxc = px_ - cx;

            /* ---------------- halo de la luz encendida ---------------- */
            {
                double dy = py - lensY[active];
                double d  = sqrt(dxc * dxc + dy * dy);
                if (d < glowR) {
                    double t = 1.0 - d / glowR;
                    addPx(p, COL[active].r, COL[active].g, COL[active].b,
                          0.34 * t * t * t);
                }
            }

            /* ---------------- poste ---------------- */
            if (py > poleTop - 2.0 * s) {
                double c = covRoundRect(px_, py, cx, (poleTop + poleBot) / 2.0,
                                        poleHW, (poleBot - poleTop) / 2.0, 2.0 * s);
                if (c > 0.0) {
                    double u = dxc / poleHW;                    /* -1 .. 1 */
                    double v = 92.0 - 46.0 * fabs(u) - 26.0 * u; /* cilindro */
                    blendPx(p, v * 0.94, v, v * 0.95, c);
                }
            }

            /* ---------------- sombra general ---------------- */
            {
                double sh = covRoundRect(px_, py + 2.5 * s, cx, bodyCY,
                                         bodyHW + 3.0 * s, bodyHH + 3.0 * s,
                                         bodyRad + 3.0 * s);
                blendPx(p, 0, 0, 0, sh * 0.30);
            }

            /* ---------------- tapa superior ---------------- */
            {
                double c = covRoundRect(px_, py, cx, (capTop + capBot) / 2.0,
                                        capHW, (capBot - capTop) / 2.0, capRad);
                if (c > 0.0) {
                    double k = clampd((py - capTop) / (capBot - capTop), 0, 1);
                    double u = clampd(dxc / capHW, -1, 1);
                    double v = 82.0 - 40.0 * k - 20.0 * u;
                    blendPx(p, v * 0.90, v, v * 0.92, c);
                    /* filo oscuro */
                    double in = covRoundRect(px_, py, cx, (capTop + capBot) / 2.0,
                                             capHW - 1.3 * s,
                                             (capBot - capTop) / 2.0 - 1.3 * s, capRad);
                    blendPx(p, 18, 22, 19, clampd(c - in, 0, 1) * 0.75);
                }
            }

            /* ---------------- cuerpo ---------------- */
            {
                double c = covRoundRect(px_, py, cx, bodyCY, bodyHW, bodyHH, bodyRad);
                if (c > 0.0) {
                    double k = clampd((py - bodyTop) / (bodyBot - bodyTop), 0, 1);
                    double u = clampd(dxc / bodyHW, -1, 1);
                    /* gris verdoso, mas claro arriba y a la izquierda */
                    double v = 68.0 - 26.0 * k - 24.0 * u
                             + 26.0 * pow(clampd(-u, 0, 1), 4.0)   /* filo izq  */
                             - 10.0 * pow(clampd( u, 0, 1), 4.0);  /* filo der  */
                    blendPx(p, v * 0.90, v, v * 0.92, c);

                    double in = covRoundRect(px_, py, cx, bodyCY,
                                             bodyHW - 1.8 * s, bodyHH - 1.8 * s, bodyRad);
                    blendPx(p, 12, 15, 13, clampd(c - in, 0, 1) * 0.92);

                    /* tornillos en las esquinas */
                    for (int i = 0; i < 4; ++i) {
                        double sx = cx + ((i & 1) ? 24.0 : -24.0) * s;
                        double sy = (i < 2 ? bodyTop + 8.0 * s : bodyBot - 8.0 * s);
                        double sc = covCircle(px_, py, sx, sy, 1.9 * s);
                        if (sc > 0.0) {
                            blendPx(p, 30, 34, 31, sc * 0.85);
                            double hl = covCircle(px_, py, sx - 0.5 * s,
                                                  sy - 0.6 * s, 1.0 * s);
                            blendPx(p, 150, 156, 150, hl * 0.45);
                        }
                    }
                }
            }

            /* ---------------- viseras + lentes ---------------- */
            for (int i = 0; i < 3; ++i) {
                double dy = py - lensY[i];
                double d  = sqrt(dxc * dxc + dy * dy);

                /* --- visera: arco grueso con las puntas redondeadas ---
                   Se calcula la distancia al eje del arco; fuera del tramo
                   angular se usa la distancia a la punta, y eso da la
                   terminacion redonda en lugar del corte recto.        */
                if (d < hoodOut + 3.0 * s) {
                    const double Rc  = (hoodIn + hoodOut) / 2.0;   /* eje       */
                    const double th  = (hoodOut - hoodIn) / 2.0;   /* espesor/2 */
                    const double phiMax = 1.5707963 + hoodDrop;    /* medio arco*/

                    double phi = atan2(dxc, -dy);                  /* 0 arriba  */
                    double dist;
                    if (fabs(phi) <= phiMax) {
                        dist = fabs(d - Rc);
                    } else {
                        double sg = (phi > 0.0) ? 1.0 : -1.0;
                        double ex = cx + sg * Rc * sin(phiMax);
                        double ey = lensY[i] - Rc * cos(phiMax);
                        double ax = px_ - ex, ay = py - ey;
                        dist = sqrt(ax * ax + ay * ay);
                    }

                    /* contorno oscuro que envuelve toda la visera */
                    double ol = clampd(0.5 - (dist - (th + 0.9 * s)), 0, 1);
                    if (ol > 0.0) {
                        blendPx(p, 13, 16, 14, ol * 0.78);

                        double c = clampd(0.5 - (dist - th), 0, 1);
                        if (c > 0.0) {
                            double u  = clampd((d - hoodIn) / (hoodOut - hoodIn), 0, 1);
                            double nx = (d > 0.001) ? dxc / d : 0.0;
                            double ny = (d > 0.001) ? dy  / d : -1.0;
                            /* luz desde arriba a la izquierda */
                            double lit  = clampd(0.5 - 0.62 * nx - 0.60 * ny, 0, 1);
                            double prof = sin(clampd(u, 0, 1) * 3.14159265);
                            double v = 16.0 + 96.0 * (0.22 + 0.78 * prof)
                                              * (0.22 + 0.95 * lit);
                            /* las puntas se apagan un poco, como en la sombra */
                            double tip = clampd((fabs(phi) - phiMax) / 0.30, 0, 1);
                            v *= 1.0 - 0.38 * tip;
                            blendPx(p, v * 0.90, v, v * 0.92, c);
                            /* labio interno y filo externo oscuros */
                            blendPx(p, 8, 11, 9,  c * clampd(1.0 - u * 3.2, 0, 1) * 0.38);
                            blendPx(p, 10, 13, 11, c * clampd((u - 0.80) * 5.0, 0, 1) * 0.70);
                        }
                    }
                }

                /* --- asiento de la lente: solo una sombra suave, sin aro --- */
                {
                    double c = covCircle(px_, py, cx, lensY[i], lensR + 1.3 * s);
                    blendPx(p, 16, 19, 17, c * 0.85);
                }

                /* --- lente de vidrio --- */
                double c = covCircle(px_, py, cx, lensY[i], lensR);
                if (c <= 0.0) continue;

                double nx = dxc / lensR;
                double ny = dy  / lensR;
                double dd = nx * nx + ny * ny;
                double nz = sqrt(dd < 1.0 ? 1.0 - dd : 0.0);
                double r01 = sqrt(clampd(dd, 0, 1));   /* 0 centro .. 1 borde */

                /* luz difusa desde arriba a la izquierda */
                double diff = clampd(-0.36 * nx - 0.46 * ny + 0.81 * nz, 0, 1);
                /* el vidrio se apaga hacia el borde en vez de brillar */
                double edge = 1.0 - 0.42 * pow(r01, 3.0);
                /* sombra que le tira la visera sobre la parte de arriba */
                double cast = clampd(-ny - 0.30, 0, 1) * 0.26;

                if (i == active) {
                    double m = (0.66 + 0.48 * diff) * edge;
                    blendPx(p, COL[i].r * m, COL[i].g * m, COL[i].b * m, c);
                    blendPx(p, 0, 0, 0, c * cast);
                    /* reflejo especular chico y difuso */
                    /* nucleo encendido: da la sensacion de luz propia */
                    addPx(p, COL[i].r, COL[i].g, COL[i].b,
                          c * pow(1.0 - r01, 2.2) * 0.34);
                    /* reflejo especular chico y difuso */
                    addPx(p, 255, 255, 255, c * pow(diff, 11.0) * 0.38);
                } else {
                    /* apagada: vidrio oscuro, apenas se adivina el color */
                    double m = (0.085 + 0.095 * diff) * edge;
                    blendPx(p, COL[i].r * m + 11.0 * edge,
                               COL[i].g * m + 12.5 * edge,
                               COL[i].b * m + 11.5 * edge, c);
                    blendPx(p, 0, 0, 0, c * cast);
                    addPx(p, 205, 212, 208, c * pow(diff, 15.0) * 0.14);
                }
            }
        }
    }
}


static void buildBitmaps(void)
{
    HDC screen = GetDC(NULL);
    g_memdc = CreateCompatibleDC(screen);

    BITMAPINFO bi;
    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = g_w;
    bi.bmiHeader.biHeight      = -g_h;          /* top-down */
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    for (int i = 0; i < 3; ++i) {
        g_bmp[i] = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &g_bits[i], NULL, 0);
        if (g_bits[i])
            renderLight((unsigned char *)g_bits[i], g_w, g_h, i);
    }
    g_oldbmp = (HBITMAP)SelectObject(g_memdc, g_bmp[g_state]);
    ReleaseDC(NULL, screen);
}

/* ------------------------------------------------------------------ */
/* Icono de bandeja                                                    */
/* ------------------------------------------------------------------ */

static HICON makeTrayIcon(int state)
{
    const int N = 16;
    BITMAPINFO bi;
    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = N;
    bi.bmiHeader.biHeight      = -N;
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void *bits = NULL;
    HDC screen = GetDC(NULL);
    HBITMAP color = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    ReleaseDC(NULL, screen);
    if (!color) return NULL;

    unsigned char *b = (unsigned char *)bits;
    memset(b, 0, (size_t)N * N * 4);
    for (int y = 0; y < N; ++y) {
        for (int x = 0; x < N; ++x) {
            unsigned char *p = b + ((size_t)y * N + x) * 4;
            double c = covCircle(x + 0.5, y + 0.5, N / 2.0, N / 2.0, 6.6);
            if (c <= 0) continue;
            blendPx(p, COL[state].r, COL[state].g, COL[state].b, c);
            double rim = c - covCircle(x + 0.5, y + 0.5, N / 2.0, N / 2.0, 5.2);
            blendPx(p, 0, 0, 0, clampd(rim, 0, 1) * 0.35);
        }
    }

    unsigned char maskbits[N * N / 8];
    memset(maskbits, 0, sizeof(maskbits));
    HBITMAP mask = CreateBitmap(N, N, 1, 1, maskbits);
    ICONINFO ii;
    ZeroMemory(&ii, sizeof(ii));
    ii.fIcon    = TRUE;
    ii.hbmColor = color;
    ii.hbmMask  = mask;
    HICON ic = CreateIconIndirect(&ii);
    DeleteObject(color);
    DeleteObject(mask);
    return ic;
}

static void trayUpdate(BOOL add)
{
    NOTIFYICONDATAW nid;
    ZeroMemory(&nid, sizeof(nid));
    nid.cbSize = sizeof(nid);
    nid.hWnd   = g_hwnd;
    nid.uID    = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAY;

    HICON old = g_trayicon;
    g_trayicon = makeTrayIcon(g_state);
    nid.hIcon = g_trayicon;

    const WCHAR *txt = g_state == ST_WAITING ? L"Rojo - esperando confirmacion"
                     : g_state == ST_RUNNING ? L"Amarillo - trabajando"
                                             : L"Verde - listo";
    _snwprintf(nid.szTip, 127, L"Claude Traffic Light v1.2.0\n%s", txt);
    nid.szTip[127] = 0;

    Shell_NotifyIconW(add ? NIM_ADD : NIM_MODIFY, &nid);
    if (old) DestroyIcon(old);
}

static void trayRemove(void)
{
    NOTIFYICONDATAW nid;
    ZeroMemory(&nid, sizeof(nid));
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hwnd;
    nid.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    if (g_trayicon) { DestroyIcon(g_trayicon); g_trayicon = NULL; }
}

/* ------------------------------------------------------------------ */
/* Inicio automatico con Windows                                       */
/* ------------------------------------------------------------------ */

#define RUN_KEY L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define RUN_VAL     L"ClaudeTrafficLight"
#define RUN_VAL_OLD L"SemaforoClaude"   /* version anterior, se limpia sola */

static BOOL autostartGet(void)
{
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_READ, &k) != ERROR_SUCCESS)
        return FALSE;
    LONG r  = RegQueryValueExW(k, RUN_VAL,     NULL, NULL, NULL, NULL);
    LONG r2 = RegQueryValueExW(k, RUN_VAL_OLD, NULL, NULL, NULL, NULL);
    RegCloseKey(k);
    return r == ERROR_SUCCESS || r2 == ERROR_SUCCESS;
}

static void autostartSet(BOOL on)
{
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS)
        return;
    RegDeleteValueW(k, RUN_VAL_OLD);        /* saca la entrada vieja */
    if (on) {
        WCHAR path[MAX_PATH + 4];
        path[0] = L'"';
        GetModuleFileNameW(NULL, path + 1, MAX_PATH);
        wcscat(path, L"\"");
        RegSetValueExW(k, RUN_VAL, 0, REG_SZ, (const BYTE *)path,
                       (DWORD)((wcslen(path) + 1) * sizeof(WCHAR)));
    } else {
        RegDeleteValueW(k, RUN_VAL);
    }
    RegCloseKey(k);
    g_autostart = on;
}

/* ------------------------------------------------------------------ */
/* Posicion y presentacion                                             */
/* ------------------------------------------------------------------ */

static void computePos(int *ox, int *oy)
{
    /* usamos el monitor donde esta la ventana activa */
    HWND fg = GetForegroundWindow();
    HMONITOR mon = fg ? MonitorFromWindow(fg, MONITOR_DEFAULTTOPRIMARY)
                      : MonitorFromPoint((POINT){0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi;
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, &mi)) {
        mi.rcWork.left = 0; mi.rcWork.top = 0;
        mi.rcWork.right  = GetSystemMetrics(SM_CXSCREEN);
        mi.rcWork.bottom = GetSystemMetrics(SM_CYSCREEN);
    }

    int L = mi.rcWork.left, T = mi.rcWork.top;
    int R = mi.rcWork.right, B = mi.rcWork.bottom;
    int m = g_margin;

    BOOL left = (wcsstr(g_position, L"left") != NULL);
    BOOL top  = (wcsstr(g_position, L"top") != NULL);
    BOOL bot  = (wcsstr(g_position, L"bottom") != NULL);

    *ox = left ? L + m : R - g_w - m;
    if (top)      *oy = T + m;
    else if (bot) *oy = B - g_h - m;
    else {
        /* vertical_pct recorre el alto util: 0 arriba, 50 centro, 100 abajo.
           El centro exacto queda alto y molesta; por defecto va mas abajo. */
        int libre = (B - T) - g_h - 2 * m;
        if (libre < 0) libre = 0;
        *oy = T + m + (int)((double)libre * g_verticalPct / 100.0 + 0.5);
    }
}

static void paintNow(void)
{
    if (!g_bits[g_state]) return;

    SelectObject(g_memdc, g_bmp[g_state]);

    int ox, oy;
    computePos(&ox, &oy);

    POINT dst = { ox, oy };
    POINT src = { 0, 0 };
    SIZE  sz  = { g_w, g_h };

    BLENDFUNCTION bf;
    bf.BlendOp             = AC_SRC_OVER;
    bf.BlendFlags          = 0;
    bf.SourceConstantAlpha = (BYTE)g_alpha;
    bf.AlphaFormat         = AC_SRC_ALPHA;

    HDC screen = GetDC(NULL);
    UpdateLayeredWindow(g_hwnd, screen, &dst, &sz, g_memdc, &src, 0, &bf, ULW_ALPHA);
    ReleaseDC(NULL, screen);
}

/* Vuelve a reclamar el tope del z-order.
   Hace falta llamarlo seguido: la luz aparece justo cuando OTRA aplicacion se
   esta activando, y esa activacion puede dejarnos debajo. Reclamarlo una sola
   vez al mostrar pierde esa carrera. */
static void assertTopmost(void)
{
    SetWindowPos(g_hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
}

/* Version fuerte, para el momento en que otra aplicacion se esta activando.
   Sacar y volver a poner el flag topmost fuerza a Windows a reinsertar la
   ventana arriba de todo en lugar de dejarla donde estaba.

   NO se usa AttachThreadInput aca: engancharse a la cola de entrada de la
   otra aplicacion hace falta para robar el foco, no para el z-order, y como
   sincroniza teclado y mouse entre los dos procesos puede trabar clics en la
   aplicacion de al lado. */
static void forceTopmost(void)
{
    SetWindowPos(g_hwnd, HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    SetWindowPos(g_hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
}

static void showLight(void)
{
    if (g_phase == PH_HIDDEN) {
        g_alpha = 0;
        paintNow();
        ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
    }
    forceTopmost();
    /* La activacion de la otra ventana puede completarse despues de esto,
       asi que lo repetimos un rato mas tarde para ganarle a los rezagados. */
    SetTimer(g_hwnd, TIMER_RAISE, 250, NULL);
    if (g_phase == PH_HIDDEN || g_phase == PH_OUT)
        g_phase = PH_IN;                 /* arrancar (o revertir) el fundido */
    else if (g_phase != PH_IN)
        g_phase = PH_HOLD;               /* ya visible: reiniciar la espera  */

    g_holdStart = GetTickCount();
    SetTimer(g_hwnd, TIMER_ANIM, 16, NULL);
    paintNow();
}

static void hideNow(void)
{
    KillTimer(g_hwnd, TIMER_ANIM);
    g_phase = PH_HIDDEN;
    g_alpha = 0;
    ShowWindow(g_hwnd, SW_HIDE);
}

static void onTimer(void)
{
    DWORD now  = GetTickCount();
    DWORD span = now - g_holdStart;

    /* barato: solo corre mientras la luz esta en pantalla */
    if (g_phase != PH_HIDDEN) assertTopmost();

    switch (g_phase) {
    case PH_IN: {
        int a = (int)(g_maxAlpha * ((double)span / g_fadeInMs));
        if (a >= g_maxAlpha) {
            g_alpha = g_maxAlpha;
            g_phase = PH_HOLD;
            g_holdStart = now;
        } else {
            g_alpha = a < 0 ? 0 : a;
        }
        paintNow();
        break;
    }
    case PH_HOLD:
        if (g_alpha != g_maxAlpha) { g_alpha = g_maxAlpha; paintNow(); }
        if ((int)span >= g_holdMs) {
            g_phase = PH_OUT;
            g_holdStart = now;
        }
        break;
    case PH_OUT: {
        double t = (double)span / g_fadeOutMs;
        if (t >= 1.0) { hideNow(); return; }
        g_alpha = (int)(g_maxAlpha * (1.0 - t));
        paintNow();
        break;
    }
    default:
        hideNow();
        break;
    }
}

static void setState(int st, int watchMs)
{
    if (st < 0 || st > 2) return;

    if (watchMs > 0 && st == ST_RUNNING) {
        g_watchUntil = GetTickCount() + (DWORD)watchMs;
        SetTimer(g_hwnd, TIMER_WATCH, 400, NULL);
    } else {
        g_watchUntil = 0;
        KillTimer(g_hwnd, TIMER_WATCH);
    }

    BOOL changed = (st != g_state);
    g_state = st;
    trayUpdate(FALSE);
    /* Recalculamos el foco en vez de confiar en el valor guardado: si nos
       perdimos algun evento de cambio de ventana, el guardado queda viejo y
       la luz no se muestra cuando deberia. */
    g_claudeFocus = isClaudeWindow(GetForegroundWindow());
    if (changed && (!g_claudeFocus || g_showWhenFocus))
        showLight();
    else if (changed && g_phase != PH_HIDDEN)
        paintNow();
}

/* ------------------------------------------------------------------ */
/* Deteccion de foco: es la ventana de Claude?                         */
/* ------------------------------------------------------------------ */

static BOOL isClaudeWindow(HWND h)
{
    if (!h) return FALSE;
    WCHAR title[512];
    int n = GetWindowTextW(h, title, 512);
    if (n <= 0) return FALSE;
    lowerW(title);
    return wcsstr(title, g_match) != NULL;
}

static void CALLBACK winEventProc(HWINEVENTHOOK hook, DWORD event, HWND hwnd,
                                  LONG idObject, LONG idChild,
                                  DWORD thread, DWORD time)
{
    (void)hook; (void)idChild; (void)thread; (void)time;
    if (idObject != OBJID_WINDOW || hwnd == g_hwnd) return;

    if (event == EVENT_OBJECT_NAMECHANGE) {
        /* Cambiar de pestana dentro del navegador NO cambia la ventana activa,
           asi que EVENT_SYSTEM_FOREGROUND no se dispara. Lo unico que cambia es
           el titulo de la ventana. Por eso escuchamos tambien los cambios de
           titulo de la ventana que esta en primer plano. */
        if (hwnd != GetForegroundWindow()) return;
    } else if (event != EVENT_SYSTEM_FOREGROUND) {
        return;
    }

    BOOL now = isClaudeWindow(hwnd);
    if (g_claudeFocus && !now && g_showOnBlur)
        showLight();                 /* te fuiste de Claude -> avisar */
    else if (g_phase != PH_HIDDEN)
        forceTopmost();              /* cambio de ventana: seguimos arriba */
    g_claudeFocus = now;
}

/* ------------------------------------------------------------------ */
/* Servidor HTTP local (para el userscript de claude.ai)               */
/* ------------------------------------------------------------------ */

static int parseStateToken(const char *s)
{
    if (!s) return -1;
    if (!_strnicmp(s, "waiting", 7) || !_strnicmp(s, "rojo", 4) ||
        !_strnicmp(s, "red", 3)     || !_strnicmp(s, "confirm", 7)) return ST_WAITING;
    if (!_strnicmp(s, "running", 7) || !_strnicmp(s, "amarillo", 8) ||
        !_strnicmp(s, "yellow", 6)  || !_strnicmp(s, "busy", 4) ||
        !_strnicmp(s, "working", 7)) return ST_RUNNING;
    if (!_strnicmp(s, "done", 4)    || !_strnicmp(s, "verde", 5) ||
        !_strnicmp(s, "green", 5)   || !_strnicmp(s, "idle", 4) ||
        !_strnicmp(s, "ready", 5))  return ST_DONE;
    return -1;
}

static DWORD WINAPI httpThread(LPVOID arg)
{
    (void)arg;
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 0;

    SOCKET srv = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (srv == INVALID_SOCKET) return 0;

    BOOL yes = TRUE;
    setsockopt(srv, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&yes, sizeof(yes));

    struct sockaddr_in a;
    ZeroMemory(&a, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port   = htons((u_short)g_port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   /* solo 127.0.0.1 */

    if (bind(srv, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(srv, 8) != 0) {
        closesocket(srv);
        WSACleanup();
        return 0;
    }

    for (;;) {
        SOCKET c = accept(srv, NULL, NULL);
        if (c == INVALID_SOCKET) { Sleep(50); continue; }

        DWORD tmo = 1500;
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tmo, sizeof(tmo));

        char buf[2048];
        int n = recv(c, buf, sizeof(buf) - 1, 0);
        if (n > 0) {
            buf[n] = 0;
            int st = -1;
            char *p = strstr(buf, "s=");
            if (!p) p = strstr(buf, "estado=");
            if (p) {
                p = strchr(p, '=');
                if (p) st = parseStateToken(p + 1);
            }
            /* w=<ms> pide vigilancia: si no llega otro aviso en ese tiempo,
               el programa pasa a verde por su cuenta. El temporizador vive
               aca y no en el navegador, que estrangula los suyos cuando la
               pestana esta en segundo plano. */
            int watch = 0;
            char *wp = strstr(buf, "w=");
            if (wp) watch = atoi(wp + 2);
            if (watch < 0) watch = 0;
            if (watch > 60000) watch = 60000;

            if (st >= 0)
                PostMessage(g_hwnd, WM_SETSTATE, (WPARAM)st, (LPARAM)watch);

            static const char *resp =
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/plain\r\n"
                "Access-Control-Allow-Origin: *\r\n"
                "Access-Control-Allow-Headers: *\r\n"
                "Access-Control-Allow-Methods: GET,POST,OPTIONS\r\n"
                /* permite que una pagina https hable con 127.0.0.1 sin extension */
                "Access-Control-Allow-Private-Network: true\r\n"
                "Access-Control-Max-Age: 600\r\n"
                "Content-Length: 2\r\n"
                "Connection: close\r\n\r\nok";
            send(c, resp, (int)strlen(resp), 0);
        }
        shutdown(c, SD_BOTH);
        closesocket(c);
    }
}

/* ------------------------------------------------------------------ */
/* Menu de bandeja                                                     */
/* ------------------------------------------------------------------ */

static void showMenu(void)
{
    POINT pt;
    GetCursorPos(&pt);
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, ID_SHOW, L"Mostrar ahora");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, ID_TEST_R, L"Probar rojo");
    AppendMenuW(m, MF_STRING, ID_TEST_Y, L"Probar amarillo");
    AppendMenuW(m, MF_STRING, ID_TEST_G, L"Probar verde");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING | (g_autostart ? MF_CHECKED : 0),
                ID_AUTOSTART, L"Iniciar con Windows");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, ID_EXIT, L"Salir");

    SetForegroundWindow(g_hwnd);
    TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, g_hwnd, NULL);
    DestroyMenu(m);
}

/* ------------------------------------------------------------------ */
/* Ventana                                                             */
/* ------------------------------------------------------------------ */

static LRESULT CALLBACK wndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_TIMER:
        if (wp == TIMER_ANIM) onTimer();
        else if (wp == TIMER_WATCH) {
            /* se acabo el tiempo sin noticias del navegador: damos por
               terminado y pasamos a verde */
            if (g_watchUntil && (LONG)(GetTickCount() - g_watchUntil) >= 0) {
                KillTimer(h, TIMER_WATCH);
                g_watchUntil = 0;
                setState(ST_DONE, 0);
            }
        }
        else if (wp == TIMER_RAISE) {
            KillTimer(h, TIMER_RAISE);
            if (g_phase != PH_HIDDEN) forceTopmost();
        }
        return 0;

    case WM_SETSTATE:
        setState((int)wp, (int)lp);
        return 0;

    case WM_COPYDATA: {
        COPYDATASTRUCT *cds = (COPYDATASTRUCT *)lp;
        if (cds && cds->lpData && cds->cbData > 0) {
            char tmp[64];
            size_t n = cds->cbData < sizeof(tmp) - 1 ? cds->cbData : sizeof(tmp) - 1;
            memcpy(tmp, cds->lpData, n);
            tmp[n] = 0;
            if (!_stricmp(tmp, "show")) showLight();
            else {
                int st = parseStateToken(tmp);
                if (st >= 0) setState(st, 0);
            }
        }
        return 1;
    }

    case WM_TRAY:
        if (lp == WM_RBUTTONUP || lp == WM_CONTEXTMENU) showMenu();
        else if (lp == WM_LBUTTONUP) showLight();
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_SHOW:      showLight(); break;
        case ID_TEST_R:    setState(ST_WAITING, 0); showLight(); break;
        case ID_TEST_Y:    setState(ST_RUNNING, 0); showLight(); break;
        case ID_TEST_G:    setState(ST_DONE, 0);    showLight(); break;
        case ID_AUTOSTART: autostartSet(!g_autostart); break;
        case ID_EXIT:      DestroyWindow(h); break;
        }
        return 0;

    case WM_DISPLAYCHANGE:
    case WM_SETTINGCHANGE:
        if (g_phase != PH_HIDDEN) paintNow();
        return 0;

    case WM_DESTROY:
        trayRemove();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

/* ------------------------------------------------------------------ */
/* Linea de comandos: semaforo.exe --estado running                    */
/* ------------------------------------------------------------------ */

static BOOL sendToRunning(const WCHAR *word)
{
    HWND target = FindWindowW(WND_CLASS, NULL);
    if (!target) return FALSE;

    char ascii[64];
    int i = 0;
    for (; word[i] && i < 63; ++i) ascii[i] = (char)(word[i] & 0x7F);
    ascii[i] = 0;

    COPYDATASTRUCT cds;
    cds.dwData = 1;
    cds.cbData = (DWORD)(i + 1);
    cds.lpData = ascii;
    SendMessageTimeoutW(target, WM_COPYDATA, 0, (LPARAM)&cds,
                        SMTO_ABORTIFHUNG, 2000, NULL);
    return TRUE;
}

/* devuelve 1 si el proceso debe terminar aca */
static int handleCli(void)
{
    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return 0;

    int quit = 0;
    for (int i = 1; i < argc; ++i) {
        if ((!_wcsicmp(argv[i], L"--estado") || !_wcsicmp(argv[i], L"--state") ||
             !_wcsicmp(argv[i], L"-s")) && i + 1 < argc) {
            sendToRunning(argv[i + 1]);
            quit = 1;
            break;
        }
        if (!_wcsicmp(argv[i], L"--mostrar") || !_wcsicmp(argv[i], L"--show")) {
            sendToRunning(L"show");
            quit = 1;
            break;
        }
        if (!_wcsicmp(argv[i], L"--salir") || !_wcsicmp(argv[i], L"--quit")) {
            HWND t = FindWindowW(WND_CLASS, NULL);
            if (t) PostMessageW(t, WM_CLOSE, 0, 0);
            quit = 1;
            break;
        }
    }
    LocalFree(argv);
    return quit;
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmd, int show)
{
    (void)prev; (void)cmd; (void)show;
    g_inst = inst;

    if (handleCli()) return 0;

    HANDLE mtx = CreateMutexW(NULL, FALSE, APP_MUTEX);
    if (mtx && GetLastError() == ERROR_ALREADY_EXISTS) {
        sendToRunning(L"show");     /* ya hay una instancia: solo la mostramos */
        return 0;
    }

    /* DPI-aware sin depender de versiones nuevas de Windows */
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    if (u32) {
        typedef BOOL (WINAPI *SPDA)(void);
        SPDA f = (SPDA)(void *)GetProcAddress(u32, "SetProcessDPIAware");
        if (f) f();
    }

    loadConfig();
    g_autostart = autostartGet();

    HDC sdc = GetDC(NULL);
    int dpi = GetDeviceCaps(sdc, LOGPIXELSX);
    ReleaseDC(NULL, sdc);
    if (dpi <= 0) dpi = 96;

    double k = (dpi / 96.0) * (g_scalePct / 100.0);
    g_w = (int)(BASE_W * k + 0.5);
    g_h = (int)(BASE_H * k + 0.5);
    g_margin = (int)(g_margin * (dpi / 96.0) + 0.5);

    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = wndProc;
    wc.hInstance     = inst;
    wc.lpszClassName = WND_CLASS;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    RegisterClassExW(&wc);

    g_hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST |
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        WND_CLASS, L"Semaforo Claude", WS_POPUP,
        0, 0, g_w, g_h, NULL, NULL, inst, NULL);
    if (!g_hwnd) return 1;

    buildBitmaps();
    trayUpdate(TRUE);

    g_claudeFocus = isClaudeWindow(GetForegroundWindow());

    /* aviso de arranque para que sepas que quedo andando */
    showLight();

    HWINEVENTHOOK hook = SetWinEventHook(
        EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, NULL,
        winEventProc, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

    /* Segundo enganche: cambios de titulo, que es como se detecta el cambio
       de pestana dentro de un mismo navegador. */
    HWINEVENTHOOK hookName = SetWinEventHook(
        EVENT_OBJECT_NAMECHANGE, EVENT_OBJECT_NAMECHANGE, NULL,
        winEventProc, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

    CreateThread(NULL, 0, httpThread, NULL, 0, NULL);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (hook) UnhookWinEvent(hook);
    if (hookName) UnhookWinEvent(hookName);
    if (mtx)  CloseHandle(mtx);
    return 0;
}
