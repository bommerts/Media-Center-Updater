/* Themed Win32 controls: fonts, colours, light and dark mode, and the drawing of buttons, switches and cards. */
#ifndef MCU_UI_H
#define MCU_UI_H

#include "common.h"
#include <commctrl.h>

typedef enum { FONT_BODY, FONT_STRONG, FONT_CAPTION, FONT_CAPTION_STRONG, FONT_SUBTITLE, FONT_TITLE, FONT_HEADER, FONT_COUNT } FontId;
typedef enum { TEXT_PRIMARY, TEXT_SECONDARY } TextRole;
typedef enum { NOTICE_INFO, NOTICE_SUCCESS, NOTICE_WARNING, NOTICE_ERROR } Severity;
/* What a control sits on, which decides the colour behind it. The four notice surfaces follow Severity. */
typedef enum { SURFACE_PAGE, SURFACE_CARD, SURFACE_INFO, SURFACE_SUCCESS, SURFACE_WARNING, SURFACE_ERROR, SURFACE_COUNT } Surface;
typedef enum { BUTTON_STANDARD, BUTTON_ACCENT, BUTTON_SWITCH, BUTTON_CHECK, BUTTON_TAB } ButtonKind;

typedef struct Theme
{
    bool dark;
    COLORREF surface[SURFACE_COUNT];
    COLORREF severity[4];           /* icon colour for each Severity */
    COLORREF card_stroke, text, text_secondary, text_disabled;
    COLORREF accent, accent_hot, accent_text;
    COLORREF control, control_hot, control_pressed, control_stroke;
    COLORREF field, field_stroke;
} Theme;

void ui_init(void);
void ui_shutdown(void);
void ui_set_dpi(UINT dpi);
int ui_px(int value);                           /* a size in 96-dpi units, scaled for the current display */
void ui_set_theme(bool dark);
const Theme *ui_theme(void);
bool ui_system_uses_dark(void);
HFONT ui_font(FontId id);
int ui_line_height(FontId id);

HWND ui_label(HWND parent, const wchar_t *text, FontId font, TextRole role, Surface surface);
HWND ui_button(HWND parent, int id, const wchar_t *text, ButtonKind kind, Surface surface);
HWND ui_edit(HWND parent, int id, bool password, const wchar_t *placeholder);
HWND ui_combo(HWND parent, int id);
HWND ui_spinner(HWND parent, Surface surface);
void ui_spinner_set(HWND spinner, bool active);
void ui_set_surface(HWND control, Surface surface);
void ui_set_selected(HWND tab, bool selected);
void ui_set_text(HWND control, const wchar_t *text);   /* does nothing when the text is unchanged */
void ui_add_tooltip(HWND control, const wchar_t *text);
/* Re-applies fonts and theme to a window's controls after the display scale or colour mode changes. */
void ui_refresh_controls(HWND root);
void ui_theme_scrollbars(HWND window);

int ui_text_height(HWND control, int width);    /* height of the control's text wrapped to a width */
SIZE ui_text_size(HWND control);                /* size of the control's text on one line */

/* Handlers for a parent window's messages. */
LRESULT ui_on_ctlcolor(UINT message, HDC dc, HWND control);
bool ui_on_notify(const NMHDR *header, LRESULT *result);

void ui_round_rect(HDC dc, const RECT *rect, int radius, COLORREF fill, COLORREF stroke, bool has_stroke);
void ui_circle(HDC dc, int x, int y, int diameter, COLORREF color);
void ui_severity_icon(HDC dc, int x, int y, int size, Severity severity);
void ui_logo(HDC dc, int x, int y, int size);

#endif
