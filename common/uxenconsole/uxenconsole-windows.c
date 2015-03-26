/*
 * Copyright 2014-2015, Bromium, Inc.
 * Author: Julian Pidancet <julian@pidancet.net>
 * SPDX-License-Identifier: ISC
 */

#include <windows.h>
#include <windowsx.h>
#define ERR_WINDOWS
#define ERR_AUTO_CONSOLE
#include <err.h>

#include "uxenconsolelib.h"

DECLARE_PROGNAME;

#define BUF_SZ 1024
struct console {
    uxenconsole_context_t ctx;
    HANDLE channel_event;
    HWND window;
    HINSTANCE instance;
    int show;
    HDC dc;
    HANDLE surface_handle;
    HBITMAP surface;
    int width;
    int height;
    int mouse_left;
    int mouse_captured;
    int last_mouse_x;
    int last_mouse_y;
    int kbd_state;
    int kbd_dead_key;
    int kbd_comp_key;
    int kbd_last_key;
    int kbd_unicode_key;
    unsigned char tx_buf[BUF_SZ];
    unsigned int tx_len;
    HCURSOR cursor;
    int requested_width;
    int requested_height;
    int stop;
};

enum {
    KBD_STATE_NORMAL = 0,
    KBD_STATE_DEADKEY_PRESSED,
    KBD_STATE_DEADKEY_RELEASED,
    KBD_STATE_COMPKEY_PRESSED,
    KBD_STATE_UNICODE,
};

static void
reset_mouse_tracking(HWND hwnd)
{
    TRACKMOUSEEVENT mousetrack;

    mousetrack.cbSize = sizeof (mousetrack);
    mousetrack.dwFlags = 0x2; /* TIME_LEAVE */
    mousetrack.hwndTrack = hwnd;
    mousetrack.dwHoverTime = 0;

    TrackMouseEvent(&mousetrack);
}

#ifndef WM_MOUSEHWHEEL
#define WM_MOUSEHWHEEL 0x020E
#endif

LRESULT CALLBACK
window_proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    struct console *cons = (void *)GetWindowLongPtr(hwnd, GWLP_USERDATA);

    switch (message) {
    case WM_PAINT:
        {
            HDC hdc;
            PAINTSTRUCT ps;
            int x, y, w, h;

            hdc = BeginPaint(hwnd, &ps);
            x = ps.rcPaint.left;
            y = ps.rcPaint.top;
            w = ps.rcPaint.right - x;
            h = ps.rcPaint.bottom - y;
            BitBlt(hdc, x, y, w, h, cons->dc, x, y, SRCCOPY);
            EndPaint(hwnd, &ps);
        }
        return 0;
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
    case WM_MBUTTONUP:
    case WM_MOUSEMOVE:
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP:
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL:
        {
            POINT cursor;
            int dv = 0;
            int dh = 0;

            if (cons->mouse_left) {
                reset_mouse_tracking(hwnd);
                cons->mouse_left = 0;
            }

            if (!cons->mouse_captured && (message == WM_LBUTTONDOWN ||
                                          message == WM_RBUTTONDOWN ||
                                          message == WM_MBUTTONDOWN)) {
                cons->mouse_captured = message;
                SetCapture(hwnd);
            } else if (message == (cons->mouse_captured + 1)) {
                ReleaseCapture();
                cons->mouse_captured = 0;
            }

            cursor.x = GET_X_LPARAM(lParam);
            cursor.y = GET_Y_LPARAM(lParam);

            if (message == WM_MOUSEWHEEL) {
                ScreenToClient(hwnd, &cursor);
                dv = GET_WHEEL_DELTA_WPARAM(wParam);
            } else if (message == WM_MOUSEHWHEEL) {
                ScreenToClient(hwnd, &cursor);
                dh = GET_WHEEL_DELTA_WPARAM(wParam);
            }

            /*
             * Since we use SetCapture, we need to make sure we're not trying to
             * transmit negative or coordinates larger than the desktop size.
             */
            if ((cursor.x < 0) || (cursor.x >= cons->width) ||
                (cursor.y < 0) || (cursor.y >= cons->height)) {
                cursor.x = cons->last_mouse_x;
                cursor.y = cons->last_mouse_y;
            } else {
                cons->last_mouse_x = cursor.x;
                cons->last_mouse_y = cursor.y;
            }

            /* wParam maps to the flags parameter  */
            uxenconsole_mouse_event(cons->ctx, cursor.x, cursor.y, dv, dh,
                                    GET_KEYSTATE_WPARAM(wParam));
        }
        return 0;
    case WM_MOUSELEAVE:
        {
            cons->mouse_left = 1;
        }
        return 0;
    case WM_KEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYDOWN:
    case WM_SYSKEYUP:
        {
            unsigned char state[256];
            wchar_t chars[4];
            int nchars;
            HKL layout;
            int up = (message == WM_KEYUP) || (message == WM_SYSKEYUP);
            unsigned int scancode = (lParam >> 16) & 0x7f;

            layout = GetKeyboardLayout(0);
            GetKeyboardState(state);

            if (!up)
                cons->kbd_last_key = wParam;

            nchars = ToUnicodeEx(wParam, scancode, state, chars,
                                 sizeof(chars) / sizeof (wchar_t),
                                 0, layout);
            if (nchars > 0) {
                nchars = ToUnicodeEx(wParam, scancode, state, chars,
                                     sizeof(chars) / sizeof (wchar_t),
                                     0, layout);
            }

            /*
             * I see dead keys...
             */
            switch (cons->kbd_state) {
            case KBD_STATE_UNICODE:
                if (up && (wParam == cons->kbd_unicode_key ||
                           wParam == VK_PROCESSKEY))
                    cons->kbd_state = KBD_STATE_NORMAL;
                break;
            case KBD_STATE_COMPKEY_PRESSED:
                if (up && (cons->kbd_comp_key == wParam))
                    cons->kbd_state = KBD_STATE_NORMAL;
                if (up && (cons->kbd_dead_key == wParam))
                    cons->kbd_dead_key = 0;
                break;
            case KBD_STATE_DEADKEY_RELEASED:
                if (!up) {
                    cons->kbd_comp_key = wParam;
                    cons->kbd_state = KBD_STATE_COMPKEY_PRESSED;
                } else
                    goto sendkey;
                break;
            case KBD_STATE_DEADKEY_PRESSED:
                if (up) {
                    if (cons->kbd_dead_key == wParam) {
                        cons->kbd_state = KBD_STATE_DEADKEY_RELEASED;
                        cons->kbd_dead_key = 0;
                    } else
                        goto sendkey;
                } else { /* down */
                    cons->kbd_comp_key = wParam;
                    cons->kbd_state = KBD_STATE_COMPKEY_PRESSED;
                }
                break;
            case KBD_STATE_NORMAL:
                if (!up) {
                    if (wParam == VK_PROCESSKEY) {
                        cons->kbd_state = KBD_STATE_UNICODE;
                        cons->kbd_unicode_key = MapVirtualKeyW(scancode,
                                                         MAPVK_VSC_TO_VK_EX);
                        break;
                    } else if (wParam == VK_PACKET) {
                        cons->kbd_state = KBD_STATE_UNICODE;
                        cons->kbd_unicode_key = wParam;
                        break;
                    } else if (nchars == -1) {
                        cons->kbd_state = KBD_STATE_DEADKEY_PRESSED;
                        cons->kbd_dead_key = wParam;
                        break;
                    }
                }
sendkey:
                if (wParam == cons->kbd_dead_key)
                    cons->kbd_dead_key = 0;
                else
                    uxenconsole_keyboard_event(
                            cons->ctx,
                            wParam,
                            lParam & 0xffff,
                            scancode | (up ? 0x80 : 0x0),
                            (lParam >> 24) | KEYBOARD_EVENT_FLAG_UCS2,
                            chars, nchars);
                break;
            default:
                /* assert */
                break;
            }
        }
        return 0;
    case WM_CHAR:
    case WM_SYSCHAR:
        if (cons->kbd_state == KBD_STATE_COMPKEY_PRESSED ||
            cons->kbd_state == KBD_STATE_UNICODE) {
            wchar_t ch = wParam;
            unsigned char scancode = (lParam >> 16) & 0x7f;

            uxenconsole_keyboard_event(
                    cons->ctx,
                    cons->kbd_last_key,
                    lParam & 0xffff,
                    scancode,
                    (lParam >> 24) | KEYBOARD_EVENT_FLAG_UCS2,
                    &ch, 1);
            uxenconsole_keyboard_event(
                    cons->ctx,
                    cons->kbd_last_key,
                    lParam & 0xffff,
                    scancode | 0x80,
                    (lParam >> 24) | KEYBOARD_EVENT_FLAG_UCS2,
                    &ch, 1);
            return 0;
        }
        break;
    case WM_SIZING:
        {
            RECT *dst = (void *)lParam;
            RECT inner, outer;
            int w, h;

            GetClientRect(cons->window, &inner);
            GetWindowRect(cons->window, &outer);

            w = (inner.right - inner.left) -
                (outer.right - outer.left) +
                (dst->right - dst->left);

            h = (inner.bottom - inner.top) -
                (outer.bottom - outer.top) +
                (dst->bottom - dst->top);

            cons->requested_width = w;
            cons->requested_height = h;
        }
        return TRUE;
    case WM_EXITSIZEMOVE:
        {
            if (cons->requested_width && cons->requested_height)
                uxenconsole_request_resize(cons->ctx,
                                           cons->requested_width,
                                           cons->requested_height);
            cons->requested_width = 0;
            cons->requested_height = 0;
        }
        return 0;
    case WM_MOVING:
        {
            RECT src;
            RECT *dst = (RECT *)lParam;

            GetWindowRect(hwnd, &src);
            dst->right = dst->left + (src.right - src.left);
            dst->bottom = dst->top + (src.bottom - src.top);
        }
        return TRUE;
    default:
        break;
    }

    return DefWindowProcW(hwnd, message, wParam, lParam);
}

static int
create_window(struct console *cons, int width, int height)
{
    WNDCLASSEXW wndclass;

    wndclass.cbSize         = sizeof(wndclass);
    wndclass.style          = 0;
    wndclass.lpfnWndProc    = window_proc;
    wndclass.cbClsExtra     = 0;
    wndclass.cbWndExtra     = 0;
    wndclass.hInstance      = cons->instance;
    wndclass.hIcon          = LoadIcon(NULL, IDI_APPLICATION);
    wndclass.hIconSm        = LoadIcon(NULL, IDI_APPLICATION);
    wndclass.hCursor        = LoadCursor(NULL, IDC_ARROW);
    wndclass.hbrBackground  = (HBRUSH)GetStockObject(WHITE_BRUSH);
    wndclass.lpszClassName  = L"uXenConsole";
    wndclass.lpszMenuName   = NULL;
    if (!RegisterClassExW(&wndclass))
        Werr(1, "RegisterClassEx failed");

    cons->window = CreateWindowExW(WS_EX_CLIENTEDGE,
                                   L"uXenConsole",
                                   L"uXen console",
                                   (WS_OVERLAPPEDWINDOW & ~(WS_MAXIMIZEBOX)),
                                   CW_USEDEFAULT,
                                   CW_USEDEFAULT,
                                   width,
                                   height,
                                   NULL,
                                   NULL,
                                   NULL,
                                   NULL);

    if (cons->window == NULL)
        Werr(1, "CreateWindowEx failed");
    if (!IsWindowUnicode(cons->window))
        errx(1, "Window is not unicode");

    printf("created window %p\n", cons->window);

    SetWindowLongPtr(cons->window, GWLP_USERDATA, (LONG_PTR)cons);

    ShowWindow(cons->window, cons->show);
    UpdateWindow(cons->window);
    reset_mouse_tracking(cons->window);

    return 0;
}

static int
release_surface(struct console *cons)
{
    if (cons->surface) {
        DeleteObject(cons->surface);
        cons->surface = NULL;
    }
    if (cons->dc) {
        DeleteDC(cons->dc);
        cons->dc = NULL;
    }
    if (cons->surface_handle) {
        CloseHandle(cons->surface_handle);
        cons->surface_handle = NULL;
    }

    return 0;
}

static int
alloc_surface(struct console *cons,
              unsigned int width,
              unsigned int height,
              unsigned int linesize,
              unsigned int length,
              unsigned int bpp,
              unsigned int offset,
              HANDLE shm_handle)
{
    HDC hdc;
    BITMAPINFO bmi;
    void *p;

    if (linesize != (width * 4) || bpp != 32) {
        warnx("Invalid surface format");
        return -1;
    }

    cons->surface_handle = shm_handle;

    hdc = GetDC(cons->window);
    cons->dc = CreateCompatibleDC(hdc);
    if (!cons->dc) {
        Wwarn("CreateCompatibleDC");
        ReleaseDC(cons->window, hdc);
        CloseHandle(cons->surface_handle); cons->surface_handle = NULL;
        return -1;
    }
    ReleaseDC(cons->window, hdc);

    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    bmi.bmiHeader.biSizeImage = width * height * 4;

    cons->surface = CreateDIBSection(cons->dc, &bmi,
                                     DIB_RGB_COLORS,
                                     &p,
                                     cons->surface_handle, offset);
    if (!cons->surface) {
        Wwarn("CreateDIBSection");
        DeleteDC(cons->dc); cons->dc = NULL;
        CloseHandle(cons->surface_handle); cons->surface_handle = NULL;
        return -1;
    }
    SelectObject(cons->dc, cons->surface);

    return 0;
}

static void
console_resize_surface(void *priv,
                       unsigned int width,
                       unsigned int height,
                       unsigned int linesize,
                       unsigned int length,
                       unsigned int bpp,
                       unsigned int offset,
                       HANDLE shm_handle)
{
    struct console *cons = priv;
    int ret;

    printf("resize surface: "
           "width=%d height=%d linesize=%d length=%d bpp=%d offset=%d shm=%p\n",
           width, height, linesize, length, bpp, offset, shm_handle);

    release_surface(cons);

    if (!cons->window) {
        create_window(cons, width, height);
    } else {
        RECT inner, outer;
        int borderX, borderY;

        GetClientRect(cons->window, &inner);
        GetWindowRect(cons->window, &outer);

        borderX = (outer.right - outer.left) - (inner.right - inner.left);
        borderY = (outer.bottom - outer.top) - (inner.bottom - inner.top);

        SetWindowPos(cons->window, HWND_NOTOPMOST,
                     CW_USEDEFAULT, CW_USEDEFAULT,
                     width + borderX, height + borderY,
                     SWP_NOMOVE);
    }
    cons->width = width;
    cons->height = height;

    ret = alloc_surface(cons, width, height, linesize, length, bpp, offset, shm_handle);
    if (ret)
        errx(1, "alloc_surface failed");
}

static void
console_invalidate_rect(void *priv,
                        unsigned int x,
                        unsigned int y,
                        unsigned int w,
                        unsigned int h)
{
    struct console *cons = priv;
    RECT r = { x, y, x + w, y + h };

    InvalidateRect(cons->window, &r, FALSE);
}

static void
console_update_cursor(void *priv,
                      unsigned int width,
                      unsigned int height,
                      unsigned int hot_x,
                      unsigned int hot_y,
                      unsigned int mask_offset,
                      unsigned int flags,
                      HANDLE shm_handle)
{
    struct console *cons = priv;
    unsigned char hidden_cursor[8] = { 0xff, 0xff, 0x00, 0x00 };
    HCURSOR hcursor;
    ICONINFO icon;

    icon.fIcon = FALSE; /* This is a cursor */
    icon.xHotspot = hot_x;
    icon.yHotspot = hot_y;
    icon.hbmColor = NULL;

    if (flags & CURSOR_UPDATE_FLAG_HIDE) {
        icon.hbmMask = CreateBitmap(1, 1 * 2, 1, 1, hidden_cursor);
    } else {
        size_t mask_len = (width * height + 7) / 8;
        char *p;

        p = MapViewOfFile(shm_handle, FILE_MAP_ALL_ACCESS, 0, 0,
                          mask_offset + mask_len);
        if (!p) {
            Wwarn("MapViewOfFile");
            CloseHandle(shm_handle);
            return;
        }

        if (flags & CURSOR_UPDATE_FLAG_MONOCHROME) {
            icon.hbmMask = CreateBitmap(width, height * 2, 1, 1, p + mask_offset);
        } else {
            icon.hbmMask = CreateBitmap(width, height, 1, 1, p + mask_offset);
            icon.hbmColor = CreateBitmap(width, height, 1, 32, p);
        }

        UnmapViewOfFile(p);
        CloseHandle(shm_handle);
    }

    hcursor = CreateIconIndirect(&icon);
    if (hcursor) {
        SetClassLongPtr(cons->window, GCLP_HCURSOR, (LONG_PTR)hcursor);
        SetCursor(hcursor);
        if (cons->cursor)
            DestroyIcon(cons->cursor);
        cons->cursor = hcursor;
    }

    DeleteObject(icon.hbmMask);
    if (icon.hbmColor)
        DeleteObject(icon.hbmColor);
}

static void
console_disconnected(void *priv)
{
    struct console *cons = priv;

    printf("disconnected\n");
    cons->stop = 1;
}

static ConsoleOps console_ops = {
    .resize_surface = console_resize_surface,
    .invalidate_rect = console_invalidate_rect,
    .update_cursor = console_update_cursor,

    .disconnected = console_disconnected,
};

static int
main_loop(struct console *cons)
{
    HANDLE events[1];
    DWORD w;
    int ret = 0;

    events[0] = cons->channel_event;

    while (!cons->stop) {
        w = MsgWaitForMultipleObjects(1, events, FALSE, INFINITE,
                                      QS_ALLINPUT);
        switch (w) {
        case WAIT_OBJECT_0:
            {
                uxenconsole_channel_event(cons->ctx, cons->channel_event, 0);
            }
            break;
        case WAIT_OBJECT_0 + 1:
            if (cons->window) {
                MSG msg;

                while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                }
            }
            break;
        default:
            Wwarn("MsgWaitForMultipleObjects");
            ret = -1;
            goto out;
        }

    }
out:

    return ret;
}

int WINAPI
WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
        LPSTR lpCmdLine, int iCmdShow)
{
    struct console cons;
    BOOL rc;
    int ret;
    int wargc;
    wchar_t **wargv;
    char pipename[512];

    memset(&cons, 0, sizeof (cons));
    cons.instance = hInstance;
    cons.show = iCmdShow;

    wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    if (wargc != 2)
        return -1;
    rc = WideCharToMultiByte(CP_ACP, 0, wargv[1], -1,
                             pipename, sizeof (pipename),
                             NULL, NULL);
    if (!rc)
        err(1, "WideCharToMultiByte");

    cons.ctx = uxenconsole_init(&console_ops, &cons, pipename);
    if (!cons.ctx)
        err(1, "uxenconsole_init");

    printf("Connecting to %s\n", pipename);
    cons.channel_event = uxenconsole_connect(cons.ctx);
    if (!cons.channel_event)
        err(1, "uxenconsole_connect");
    printf("Connected\n");
    ret = main_loop(&cons);

    uxenconsole_cleanup(cons.ctx);

    return ret;
}
