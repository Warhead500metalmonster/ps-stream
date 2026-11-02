// Custom-painted interface, included from main.c. Immediate mode: ui_frame() draws the current
// page and handles input in one pass, and runs again after every mouse or status change.

#include <objidl.h>
#include <gdiplus.h>
#include <dwmapi.h>
#include <math.h>
#include <shellapi.h>

#include "i18n.c"

#define UI_W 920
#define WEAK_WINDOW_MS 30000
#define WEAK_LOST_FRAMES 30
#define WEAK_DROPOUTS 3
#define WEAK_REPEAT_MS 300000
// A test copy must never find, or be stopped by, the copy someone is using
#ifdef PSSTREAM_TEST
#define WINDOW_CLASS L"PSStreamTestWindow"
#define INSTANCE_MUTEX L"PSStreamTestInstance"
#else
#define WINDOW_CLASS L"PSStreamWindow"
#define INSTANCE_MUTEX L"PSStreamSingleInstance"
#endif
#define UI_H 662

#define COLOR(a, r, g, b) ((ARGB)(((a) << 24) | ((r) << 16) | ((g) << 8) | (b)))
#define WHITE(a) COLOR(a, 255, 255, 255)
#define C_TEXT WHITE(255)
#define C_MUTED WHITE(165)
#define C_ACCENT COLOR(255, 10, 112, 230)
#define C_GREEN COLOR(255, 74, 222, 128)
#define C_AMBER COLOR(255, 251, 191, 36)
#define C_DARK COLOR(255, 24, 26, 31)
#define C_FIELD COLOR(255, 43, 45, 51)

#define ICON_AUDIO L""
#define ICON_VIDEO L""
#define ICON_GEAR L""
#define ICON_BULB L""
#define ICON_CHECK L"\uE73E"
#define ICON_SYNC L"\uE895"
#define ICON_INFO L"\uE946"
#define ICON_VOLUME L"\uE995"
#define ICON_MUTED L"\uE74F"
#define ICON_PLAY L""

enum { UI_NONE, UI_NAV0, UI_NAV1, UI_NAV2, UI_AUDIO, UI_VIDEO, UI_MUTE, UI_LOWER, UI_HK_MUTE, UI_DETAILS, UI_ADVANCED, UI_SIMPLE, UI_APPLY, UI_FIND, UI_LOGIN, UI_ACCOUNT_CLEAR, UI_PAIR, UI_REPAIR,
	UI_DEVICE, UI_HK_AUDIO, UI_HK_VIDEO, UI_HW, UI_RECONNECT, UI_CONTROLLER, UI_WIFI, UI_AUTOSTART, UI_AUTOWAKE, UI_WINSTART, UI_TRAY, UI_UPDATES, UI_QUICK_VOLUME, UI_WAKE, UI_UPDATE_NOW, UI_OPEN_LOGS, UI_OPEN_REPO, UI_BITRATE, UI_AO_BITRATE, UI_VOLUME, UI_BUFFER, UI_LOSS,
	UI_PRESET0 = 46, UI_RES0 = 52, UI_FPS0 = 60, UI_CODEC0 = 65, UI_AORES0 = 70, UI_LANG0 = 80, UI_MAX = 96 };

static struct {
	GpGraphics *gx;
	GpFont *small, *body, *semi, *title, *big, *icon, *icon_big;
	GpStringFormat *fmt;
	HDC back_dc, bg_dc;
	HBITMAP back_bmp;
	HWND edits[3];
	float anim_values[UI_MAX * 2], anim_step;
	bool anim_set[UI_MAX * 2], animating;
	double anim_last;
	int timer_ticks;
	bool details, advanced, was_live, weak;
	int weak_lost, weak_drops;
	DWORD weak_checked_at, weak_shown_at;
	HICON logo;
	UINT taskbar_created;
	bool in_tray;
	HBRUSH field_brush;

	int page;
	Settings draft;
	float mx, my;
	bool press, release;
	int active, released_id, capturing;
	bool again, open_device_menu, in_frame, logging_in, repairing;
	const wchar_t *tip;
	RectF rects[UI_MAX];
	bool rect_set[UI_MAX];
	int hot_id;
	char **devices;
	int device_count;
} ui;

static float F(float v) { return v * g.dpi / 96.0f; }

// ---------- drawing ----------

static GpPath *path_round(float x, float y, float w, float h, float r)
{
	GpPath *p;
	GdipCreatePath(FillModeAlternate, &p);
	float d = r * 2;
	GdipAddPathArc(p, x, y, d, d, 180, 90);
	GdipAddPathArc(p, x + w - d, y, d, d, 270, 90);
	GdipAddPathArc(p, x + w - d, y + h - d, d, d, 0, 90);
	GdipAddPathArc(p, x, y + h - d, d, d, 90, 90);
	GdipClosePathFigure(p);
	return p;
}

static void fill_round(float x, float y, float w, float h, float r, ARGB color)
{
	x = F(x); y = F(y); w = F(w); h = F(h); r = F(r);
	GpPath *p = path_round(x, y, w, h, r);
	GpSolidFill *b;
	GdipCreateSolidFill(color, &b);
	GdipFillPath(ui.gx, (GpBrush *)b, p);
	GdipDeleteBrush((GpBrush *)b);
	GdipDeletePath(p);
}

static void stroke_round(float x, float y, float w, float h, float r, float width, ARGB color)
{
	x = F(x); y = F(y); w = F(w); h = F(h); r = F(r);
	GpPath *p = path_round(x, y, w, h, r);
	GpPen *pen;
	GdipCreatePen1(color, F(width), UnitPixel, &pen);
	GdipDrawPath(ui.gx, pen, p);
	GdipDeletePen(pen);
	GdipDeletePath(p);
}

static void text(const wchar_t *s, GpFont *font, float x, float y, float w, float h, ARGB color, int halign, int valign)
{
	s = tr(s);
	RectF r = { F(x), F(y), F(w), F(h) };
	GpSolidFill *b;
	GdipCreateSolidFill(color, &b);
	GdipSetStringFormatAlign(ui.fmt, (StringAlignment)halign);
	GdipSetStringFormatLineAlign(ui.fmt, (StringAlignment)valign);
	GdipDrawString(ui.gx, s, -1, font, &r, ui.fmt, (GpBrush *)b);
	GdipDeleteBrush((GpBrush *)b);
}

static void dot(float x, float y, float d, ARGB color)
{
	fill_round(x, y, d, d, d / 2, color);
}

// A soft wave rising from the bottom of the given area, in device pixels.
static void wave(GpGraphics *gx, float x, float y, float w, float h, float start, float c1, float c2, float end, ARGB color)
{
	GpPath *p;
	GdipCreatePath(FillModeAlternate, &p);
	GdipAddPathBezier(p, x, y + h * start, x + w * 0.33f, y + h * c1, x + w * 0.66f, y + h * c2, x + w, y + h * end);
	GdipAddPathLine(p, x + w, y + h * end, x + w, y + h);
	GdipAddPathLine(p, x + w, y + h, x, y + h);
	GdipClosePathFigure(p);
	GpSolidFill *b;
	GdipCreateSolidFill(color, &b);
	GdipFillPath(gx, (GpBrush *)b, p);
	GdipDeleteBrush((GpBrush *)b);
	GdipDeletePath(p);
}

static void glow(GpGraphics *gx, float cx, float cy, float radius, ARGB color)
{
	GpPath *p;
	GdipCreatePath(FillModeAlternate, &p);
	GdipAddPathEllipse(p, cx - radius, cy - radius, radius * 2, radius * 2);
	GpPathGradient *b;
	GdipCreatePathGradientFromPath(p, &b);
	GdipSetPathGradientCenterColor(b, color);
	ARGB edge = color & 0x00ffffff;
	INT count = 1;
	GdipSetPathGradientSurroundColorsWithCount(b, &edge, &count);
	GdipFillPath(gx, (GpBrush *)b, p);
	GdipDeleteBrush((GpBrush *)b);
	GdipDeletePath(p);
}

static void background_paint(GpGraphics *gx)
{
	float w = F(UI_W), h = F(UI_H);
	GpPointF a = { 0, 0 }, b = { 0, h };
	GpLineGradient *grad;
	GdipCreateLineBrush(&a, &b, COLOR(255, 30, 32, 39), COLOR(255, 16, 17, 22), WrapModeTile, &grad);
	GdipFillRectangle(gx, (GpBrush *)grad, 0, 0, w, h);
	GdipDeleteBrush((GpBrush *)grad);
	glow(gx, w * 0.05f, h * 0.02f, w * 0.48f, COLOR(46, 240, 150, 50));
	glow(gx, w * 0.98f, h * 0.10f, w * 0.52f, COLOR(58, 40, 100, 240));
	wave(gx, 0, 0, w, h, 0.62f, 0.86f, 0.30f, 0.58f, WHITE(6));
	wave(gx, 0, 0, w, h, 0.84f, 0.62f, 0.98f, 0.70f, WHITE(8));
}

// ---------- widgets ----------

typedef struct { bool hot, click, held; } Hit;

// Eases a value toward its target; frames keep coming until it gets there. A widget's second
// value (its on/off state, beside its hover) lives at id + UI_MAX.
static float anim(int slot, float target)
{
	float *v = &ui.anim_values[slot];
	if(!ui.anim_set[slot])
	{
		ui.anim_set[slot] = true;
		*v = target;
		return target;
	}
	*v += (target - *v) * ui.anim_step;
	if(fabsf(target - *v) < 0.004f)
		*v = target;
	else
		ui.animating = true;
	return *v;
}

static ARGB mix(ARGB a, ARGB b, float t)
{
	ARGB out = 0;
	for(int shift = 0; shift < 32; shift += 8)
	{
		float ca = (float)((a >> shift) & 255), cb = (float)((b >> shift) & 255);
		out |= (ARGB)(ca + (cb - ca) * t + 0.5f) << shift;
	}
	return out;
}

static bool mouse_in(float x, float y, float w, float h)
{
	return ui.mx >= F(x) && ui.mx < F(x + w) && ui.my >= F(y) && ui.my < F(y + h);
}

static Hit hit(int id, float x, float y, float w, float h)
{
	RectF r = { x, y, w, h };
	ui.rects[id] = r;
	ui.rect_set[id] = true;
	Hit out;
	out.hot = mouse_in(x, y, w, h);
	if(ui.press && out.hot)
	{
		ui.active = id;
		ui.press = false;
	}
	out.held = ui.active == id;
	out.click = ui.release && ui.released_id == id && out.hot;
	return out;
}

static bool pill_button(int id, const wchar_t *label, float x, float y, float w, float h, bool primary, bool enabled)
{
	Hit b = hit(id, x, y, w, h);
	ARGB bg, fg;
	if(!enabled)
	{
		bg = WHITE(30);
		fg = WHITE(110);
	}
	else if(primary)
	{
		bg = mix(WHITE(255), WHITE(200), anim(id, b.held ? 1.0f : b.hot ? 0.5f : 0));
		fg = C_DARK;
	}
	else
	{
		bg = mix(WHITE(32), WHITE(72), anim(id, b.held ? 1.0f : b.hot ? 0.55f : 0));
		fg = C_TEXT;
	}
	fill_round(x, y, w, h, h / 2, bg);
	text(label, ui.semi, x, y, w, h, fg, 1, 1);
	return enabled && b.click;
}

static bool chip(int id, const wchar_t *label, float x, float y, float w, float h, bool selected)
{
	Hit b = hit(id, x, y, w, h);
	float on = anim(id + UI_MAX, selected ? 1.0f : 0);
	fill_round(x, y, w, h, 10, mix(mix(WHITE(24), WHITE(48), anim(id, b.hot ? 1.0f : 0)), C_ACCENT, on));
	text(label, ui.semi, x, y, w, h, mix(WHITE(215), C_TEXT, on), 1, 1);
	return b.click;
}

static bool switch_toggle(int id, float x, float y, bool on)
{
	Hit b = hit(id, x, y, 46, 26);
	float t = anim(id + UI_MAX, on ? 1.0f : 0);
	fill_round(x, y, 46, 26, 13, mix(mix(WHITE(45), WHITE(70), anim(id, b.hot ? 1.0f : 0)), C_ACCENT, t));
	dot(x + 3 + 20 * t, y + 3, 20, WHITE(255));
	return b.click;
}

static bool slider(int id, float x, float y, float w, int *value, int min, int max)
{
	Hit b = hit(id, x - 8, y, w + 16, 28);
	int old = *value;
	if(b.held)
	{
		float t = (ui.mx - F(x)) / F(w);
		t = t < 0 ? 0 : t > 1 ? 1 : t;
		*value = min + (int)(t * (max - min) + 0.5f);
	}
	float t = (float)(*value - min) / (max - min);
	fill_round(x, y + 12, w, 4, 2, WHITE(45));
	fill_round(x, y + 12, w * t, 4, 2, C_ACCENT);
	float grow = anim(id, b.held || b.hot ? 1.0f : 0) * 2;
	dot(x + w * t - 8 - grow / 2, y + 6 - grow / 2, 16 + grow, mix(WHITE(230), WHITE(255), grow / 2));
	return *value != old;
}

// Large soft shapes are the slowest thing to paint and hardly ever change, so each is kept as a
// finished picture. Only something of the same size and kind can reuse one.
#define LAYER_COUNT 16
#define LAYER_MARGIN 14
typedef struct { GpBitmap *image; float x, y, w, h; int kind, left, top, width, height; } Layer;
static Layer layers[LAYER_COUNT];
static int layer_next;
static Layer *layer_current;
static GpGraphics *layer_saved_gx;

static bool layer_begin(float x, float y, float w, float h, int kind)
{
	for(int i = 0; i < LAYER_COUNT; i++)
	{
		Layer *l = &layers[i];
		if(l->image && l->x == x && l->y == y && l->w == w && l->h == h && l->kind == kind)
		{
			layer_current = l;
			return false;
		}
	}
	Layer *l = &layers[layer_next];
	layer_next = (layer_next + 1) % LAYER_COUNT;
	if(l->image)
		GdipDisposeImage((GpImage *)l->image);
	l->x = x;
	l->y = y;
	l->w = w;
	l->h = h;
	l->kind = kind;
	l->left = (int)F(x - LAYER_MARGIN);
	l->top = (int)F(y - LAYER_MARGIN);
	l->width = (int)F(w + LAYER_MARGIN * 2) + 2;
	l->height = (int)F(h + LAYER_MARGIN * 2) + 2;
	GdipCreateBitmapFromScan0(l->width, l->height, 0, PixelFormat32bppPARGB, NULL, &l->image);
	layer_saved_gx = ui.gx;
	GdipGetImageGraphicsContext((GpImage *)l->image, &ui.gx);
	GdipSetSmoothingMode(ui.gx, SmoothingModeAntiAlias);
	GdipTranslateWorldTransform(ui.gx, (float)-l->left, (float)-l->top, MatrixOrderPrepend);
	layer_current = l;
	return true;
}

static void layer_end(void)
{
	GdipDeleteGraphics(ui.gx);
	ui.gx = layer_saved_gx;
}

static void layer_draw(void)
{
	Layer *l = layer_current;
	GdipSetInterpolationMode(ui.gx, InterpolationModeNearestNeighbor);
	GdipSetPixelOffsetMode(ui.gx, PixelOffsetModeHalf);
	GdipDrawImageRectI(ui.gx, (GpImage *)l->image, l->left, l->top, l->width, l->height);
	GdipSetPixelOffsetMode(ui.gx, PixelOffsetModeDefault);
}

static void shadow(float x, float y, float w, float h, float r)
{
	for(int i = 7; i >= 1; i -= 2)
		fill_round(x - i, y - i + 5, w + i * 2, h + i * 2, r + i, COLOR(13, 0, 0, 0));
}

static void panel(float x, float y, float w, float h)
{
	if(layer_begin(x, y, w, h, 0))
	{
		shadow(x, y, w, h, 22);
		fill_round(x, y, w, h, 22, WHITE(13));
		stroke_round(x, y, w, h, 22, 1, WHITE(20));
		layer_end();
	}
	layer_draw();
}

static void heading(const wchar_t *s, float x, float y)
{
	fill_round(x, y + 6, 3, 11, 1.5f, C_ACCENT);
	text(s, ui.semi, x + 10, y, 300, 22, C_MUTED, 0, 1);
}

static void row_label(const wchar_t *s, const wchar_t *tip, float x, float y, float w)
{
	text(s, ui.body, x, y, 130, 30, C_TEXT, 0, 1);
	if(mouse_in(x, y, w, 34))
		ui.tip = tip;
}

// ---------- behaviour ----------

static void settings_commit(const Settings *s)
{
	bool device_changed = strcmp(s->device, g.settings.device) != 0;
	EnterCriticalSection(&g.lock);
	g.settings = *s;
	LeaveCriticalSection(&g.lock);
	ui.draft = *s;
	if(device_changed)
		g.audio_reopen = 1;
#ifndef PSSTREAM_TEST
	win_start_set(s->win_start != 0);
#else
	if(!g.test_hidden || GetEnvironmentVariableW(L"PSSTREAM_TEST_WINSTART", NULL, 0))
		win_start_set(s->win_start != 0);
#endif
	config_save_settings(s);
	SetEvent(g.wake);
}

static UINT hotkey_mods(int hotkey)
{
	int f = HIBYTE(hotkey);
	return MOD_NOREPEAT | (f & HOTKEYF_CONTROL ? MOD_CONTROL : 0) | (f & HOTKEYF_ALT ? MOD_ALT : 0) | (f & HOTKEYF_SHIFT ? MOD_SHIFT : 0);
}

static bool hotkeys_register(const Settings *s)
{
	UnregisterHotKey(g.hwnd, HOTKEY_AUDIO);
	UnregisterHotKey(g.hwnd, HOTKEY_VIDEO);
	UnregisterHotKey(g.hwnd, HOTKEY_MUTE);
	bool ok = true;
	if(LOBYTE(s->hotkey_audio))
		ok = RegisterHotKey(g.hwnd, HOTKEY_AUDIO, hotkey_mods(s->hotkey_audio), LOBYTE(s->hotkey_audio)) && ok;
	if(LOBYTE(s->hotkey_video))
		ok = RegisterHotKey(g.hwnd, HOTKEY_VIDEO, hotkey_mods(s->hotkey_video), LOBYTE(s->hotkey_video)) && ok;
	if(LOBYTE(s->hotkey_mute))
		ok = RegisterHotKey(g.hwnd, HOTKEY_MUTE, hotkey_mods(s->hotkey_mute), LOBYTE(s->hotkey_mute)) && ok;
	return ok;
}

static void hotkey_name(int hotkey, wchar_t *out, size_t n)
{
	int f = HIBYTE(hotkey), vk = LOBYTE(hotkey);
	if(!vk)
	{
		swprintf(out, n, L"Yok");
		return;
	}
	wchar_t key[32];
	LONG scan = (LONG)MapVirtualKeyW(vk, MAPVK_VK_TO_VSC) << 16;
	if(vk >= VK_PRIOR && vk <= VK_DELETE)
		scan |= 1 << 24; // Home, arrows and friends are extended keys
	if(!GetKeyNameTextW(scan, key, 32))
		swprintf(key, 32, L"%d", vk);
	swprintf(out, n, L"%ls%ls%ls%ls", f & HOTKEYF_CONTROL ? L"Ctrl+" : L"", f & HOTKEYF_ALT ? L"Alt+" : L"", f & HOTKEYF_SHIFT ? L"Shift+" : L"", key);
}

static void show_page(int page)
{
	if(page == PAGE_SETTINGS && ui.page != PAGE_SETTINGS)
		ui.draft = settings_get();
	ui.page = page;
	ui.capturing = 0;
	ui.again = true;
	for(int i = 0; i < 3; i++)
		ShowWindow(ui.edits[i], page == PAGE_PAIR ? SW_SHOW : SW_HIDE);
	SetFocus(g.hwnd);
}

static void volume_set(int volume)
{
	EnterCriticalSection(&g.lock);
	g.settings.volume = volume;
	LeaveCriticalSection(&g.lock);
	ui.draft.volume = volume;
	config_set_int("volume", volume);
}

static void toggle(int which)
{
	if(!g.paired)
	{
		show_page(PAGE_PAIR);
		set_status(L"Önce PS5'i eşleştir");
		return;
	}
	char host[256];
	get_text(ID_HOST, host, sizeof(host));
	host_set(host);
	if(!host[0])
	{
		show_page(PAGE_PAIR);
		set_status(L"PS5 IP adresini gir veya Otomatik bul'a bas");
		return;
	}
	if(which == UI_AUDIO)
		g.want_audio = !g.want_audio;
	else
		g.want_video = !g.want_video;
	SetEvent(g.wake);
}

static void mute_toggle(void)
{
	g.muted = !g.muted;
	set_status(g.muted ? L"Ses kısıldı. Bağlantı açık, açınca hemen geri gelir" : L"Ses geri açıldı");
}

// The next step down for someone whose Wi-Fi can't keep up. Returns -1 at the bottom.
static int preset_lower(const Settings *s)
{
	int best = -1;
	for(int i = 0; i < PRESET_COUNT; i++)
		if(presets[i].kbps < s->kbps)
			best = i;
	return best;
}

static void edit_cues(void)
{
	SendMessageW(ui.edits[0], EM_SETCUEBANNER, TRUE, (LPARAM)tr(L"Örnek: 192.168.1.20"));
	SendMessageW(ui.edits[1], EM_SETCUEBANNER, TRUE, (LPARAM)tr(L"Giriş yapınca kendiliğinden gelir"));
	SendMessageW(ui.edits[2], EM_SETCUEBANNER, TRUE, (LPARAM)tr(L"8 haneli kod"));
}

static void data_folder_open(void)
{
	ShellExecuteW(NULL, L"open", g.data_dir, NULL, NULL, SW_SHOWNORMAL);
}

static int preset_match(const Settings *s)
{
	for(int i = 0; i < PRESET_COUNT; i++)
		if(presets[i].res == s->res && presets[i].fps == s->fps && presets[i].kbps == s->kbps && presets[i].h265 == s->h265)
			return i;
	return PRESET_COUNT; // "custom"
}

static void bitrate_text(wchar_t *out, size_t n, int kbps)
{
	swprintf(out, n, tr(L"%d,%d Mbps"), kbps / 1000, kbps % 1000 / 100);
}

// ---------- pages ----------

static void card(int id, float x, float y, float w, float h, ARGB c1, ARGB c2, const wchar_t *icon,
		const wchar_t *title, const wchar_t *subtitle, bool on)
{
	float dx = F(x), dy = F(y), dw = F(w), dh = F(h);
	float lit = anim(id + UI_MAX, on ? 1.0f : 0);
	// A settled card is one finished picture. Only the fade in between paints the dimming live.
	if(layer_begin(x, y, w, h, id * 2 + (lit == 0)))
	{
		shadow(x, y, w, h, 26);
		GpPath *p = path_round(dx, dy, dw, dh, F(26));
		GpPointF a = { dx, dy }, b = { dx + dw, dy + dh };
		GpLineGradient *grad;
		GdipCreateLineBrush(&a, &b, c1, c2, WrapModeTile, &grad);
		GdipFillPath(ui.gx, (GpBrush *)grad, p);
		GdipDeleteBrush((GpBrush *)grad);

		GdipSetClipPath(ui.gx, p, CombineModeReplace);
		wave(ui.gx, dx, dy, dw, dh, 0.38f, 0.12f, 0.70f, 0.30f, WHITE(18));
		wave(ui.gx, dx, dy, dw, dh, 0.60f, 0.84f, 0.32f, 0.52f, WHITE(20));
		if(lit == 0)
		{
			GpSolidFill *dim;
			GdipCreateSolidFill(COLOR(135, 16, 17, 22), &dim);
			GdipFillPath(ui.gx, (GpBrush *)dim, p);
			GdipDeleteBrush((GpBrush *)dim);
		}
		GdipResetClip(ui.gx);
		GdipDeletePath(p);
		layer_end();
	}
	layer_draw();
	if(lit > 0 && lit < 1)
		fill_round(x, y, w, h, 26, COLOR((int)(135 * (1 - lit)), 16, 17, 22));
	stroke_round(x + 1, y + 1, w - 2, h - 2, 25, 1 + lit, mix(WHITE(26), C_GREEN, lit));

	dot(x + 26, y + 22, 56, on ? WHITE(38) : WHITE(18));
	text(icon, ui.icon_big, x + 26, y + 22, 56, 56, C_TEXT, 1, 1);
	text(title, ui.big, x + 96, y + 28, w - 120, 42, C_TEXT, 0, 1);
	text(subtitle, ui.body, x + 26, y + 92, w - 52, 48, on ? WHITE(235) : WHITE(195), 0, 0);

	bool connecting = on && !g.connected;
	bool blink = connecting && (GetTickCount() / 500) % 2;
	float py = y + h - 76;
	dot(x + 28, py + 20, 12, !on ? WHITE(110) : connecting ? (blink ? COLOR(110, 251, 191, 36) : C_AMBER) : C_GREEN);
	text(!on ? L"Kapalı" : connecting ? L"Bağlanıyor" : L"Açık", ui.title, x + 48, py, w - 200, 52, !on ? WHITE(190) : connecting ? C_AMBER : C_GREEN, 0, 1);
	if(pill_button(id, on ? L"Kapat" : L"Aç", x + w - 146, py, 124, 52, true, true))
		toggle(id);
}

static void stat_tile(float x, float y, float w, float h, const wchar_t *label, const wchar_t *value, ARGB color)
{
	fill_round(x, y, w, h, 14, WHITE(13));
	stroke_round(x, y, w, h, 14, 1, WHITE(16));
	text(label, ui.small, x + 14, y + 8, w - 20, 20, C_MUTED, 0, 1);
	text(value, ui.semi, x + 14, y + 28, w - 20, 24, color, 0, 1);
}

static void page_play(void)
{
	Settings s = settings_get();

	// Without the details row, the cards take its room
	float extra = anim(UI_NONE + UI_MAX, ui.details ? 0 : 70);
	float cw = (UI_W - 48 - 16) / 2.0f, cy = 78, ch = 330 + extra;
	card(UI_AUDIO, 24, cy, cw, ch, COLOR(255, 214, 134, 46), COLOR(255, 110, 58, 24), ICON_AUDIO,
			L"Ses", L"Oyunun sesini bilgisayardan duy.", g.want_audio != 0);

	// Volume right on the card, since it's the control people reach for most
	float vy = cy + ch - 124;
	Hit mute = hit(UI_MUTE, 40, vy - 2, 32, 32);
	dot(40, vy - 2, 32, COLOR((int)(60 * anim(UI_MUTE, mute.hot ? 1.0f : 0)), 255, 255, 255));
	text(g.muted ? ICON_MUTED : ICON_VOLUME, ui.icon, 42, vy, 28, 28, g.muted ? C_AMBER : WHITE(220), 1, 1);
	if(mute.click)
		mute_toggle();
	if(mouse_in(40, vy - 2, 32, 32))
		ui.tip = L"Sesi kısar, bağlantı açık kalır. Kısayol: Gelişmiş ayarlar";
	int volume = s.volume;
	if(slider(UI_QUICK_VOLUME, 80, vy, cw - 150, &volume, 0, 200))
		volume_set(volume);
	wchar_t pct[16];
	swprintf(pct, 16, tr(L"%%%d"), volume);
	text(g.muted ? L"Kısık" : pct, ui.semi, cw - 50, vy, 58, 28, g.muted ? C_AMBER : C_TEXT, 2, 1);
	if(mouse_in(80, vy, cw - 70, 28))
		ui.tip = L"Oyun sesinin seviyesi. %100 üstü sesi yükseltir.";

	float vx = 24 + cw + 16;
	card(UI_VIDEO, vx, cy, cw, ch, COLOR(255, 44, 104, 232), COLOR(255, 18, 34, 118), ICON_VIDEO,
			L"Görüntü", L"Oyunu bilgisayarda gör.\nDiscord'da \"PS Stream - Görüntü\" penceresini seç.", g.want_video != 0);
	if(mouse_in(vx, cy, cw, ch - 80))
		ui.tip = L"Görüntü penceresinde F11 tam ekran yapar.";

	static const wchar_t *const names[] = { L"En düşük", L"Düşük", L"Dengeli", L"Yüksek", L"En yüksek" };
	static const wchar_t *const subs[] = { L"360p, 0,5 Mbps", L"360p, 2 Mbps", L"720p, 6 Mbps", L"1080p 60, 20 Mbps", L"1080p 60, 50 Mbps" };
	int current = preset_match(&s);
	float gap = 10, w = (UI_W - 48 - gap * 5) / 6, y = 420 + extra, h = 74;
	for(int i = 0; i <= PRESET_COUNT; i++)
	{
		float x = 24 + i * (w + gap);
		bool selected = i == current;
		Hit b = hit(UI_PRESET0 + i, x, y, w, h);
		float on = anim(UI_PRESET0 + i + UI_MAX, selected ? 1.0f : 0);
		fill_round(x, y, w, h, 16, mix(mix(WHITE(18), WHITE(40), anim(UI_PRESET0 + i, b.hot ? 1.0f : 0)), C_ACCENT, on));
		if(on < 1)
			stroke_round(x, y, w, h, 16, 1, mix(WHITE(16), C_ACCENT, on));
		// Five rising bars show the level at a glance
		float bars_x = x + w / 2 - 19;
		for(int bar = 0; bar < 5 && i < PRESET_COUNT; bar++)
		{
			float bh = 3 + bar * 2.5f;
			fill_round(bars_x + bar * 8, y + 19 - bh, 5, bh, 1.5f, bar <= i ? (selected ? WHITE(255) : WHITE(190)) : WHITE(selected ? 70 : 34));
		}
		if(i == PRESET_COUNT)
			text(ICON_GEAR, ui.icon, x, y + 2, w, 20, selected ? C_TEXT : C_MUTED, 1, 1);
		text(i < PRESET_COUNT ? names[i] : L"Özel", ui.semi, x, y + 22, w, 24, C_TEXT, 1, 1);
		text(i < PRESET_COUNT ? subs[i] : L"Ayarlardan seç", ui.small, x, y + 46, w, 20, selected ? WHITE(225) : C_MUTED, 1, 1);
		if(mouse_in(x, y, w, h))
			ui.tip = L"Görüntü kalitesi. Sadece ses modunu etkilemez.";
		if(b.click)
		{
			if(i == PRESET_COUNT)
			{
				ui.advanced = true;
				show_page(PAGE_SETTINGS);
			}
			else
			{
				s.res = presets[i].res;
				s.fps = presets[i].fps;
				s.kbps = presets[i].kbps;
				s.h265 = presets[i].h265;
				settings_commit(&s);
			}
		}
	}
	if(extra > 1)
		return;

	// Live connection numbers, for anyone who wants to see what the Wi-Fi is doing
	bool live = (g.want_audio || g.want_video) && g.connected;
	wchar_t uptime[32] = L"-", ping[32] = L"-", audio[64] = L"-", video[64] = L"-";
	int ping_ms = -1;
	if(live)
	{
		DWORD seconds = (GetTickCount() - g.connected_at) / 1000;
		swprintf(uptime, 32, L"%02lu:%02lu:%02lu", seconds / 3600, seconds / 60 % 60, seconds % 60);
		// Measured once when connecting; 1 ms exactly is the library's "couldn't measure" value
		ping_ms = (int)(g.session.rtt_us / 1000);
		if(g.session.rtt_us != 1000)
			swprintf(ping, 32, L"%d ms", ping_ms);
		else
			ping_ms = -1;
		if(g.want_audio)
			swprintf(audio, 64, tr(L"%d ms tampon, %ld kesinti"), g.audio_target_ms, (long)g.audio_dropouts);
		if(g.want_video && g.video_fps10)
			swprintf(video, 64, tr(L"%ld FPS, %ld kayıp"), (long)((g.video_fps10 + 5) / 10), (long)g.video_lost_total);
	}
	float ty = 504, th = 60, tw[] = { 170, 130, 300 };
	float tx = 24;
	ARGB idle = WHITE(120);
	stat_tile(tx, ty, tw[0], th, L"BAĞLANTI SÜRESİ", uptime, live ? C_TEXT : idle);
	tx += tw[0] + 10;
	stat_tile(tx, ty, tw[1], th, L"PING", ping, ping_ms < 0 ? idle : ping_ms < 30 ? C_GREEN : ping_ms < 80 ? C_TEXT : C_AMBER);
	tx += tw[1] + 10;
	stat_tile(tx, ty, tw[2], th, L"SES", audio, live && g.want_audio ? (g.audio_dropouts ? C_AMBER : C_TEXT) : idle);
	tx += tw[2] + 10;
	stat_tile(tx, ty, UI_W - 24 - tx, th, L"GÖRÜNTÜ", video, live && g.want_video && g.video_fps10 ? (g.video_lost_total ? C_AMBER : C_TEXT) : idle);
}

static void chips_row(int id0, const wchar_t *const *labels, int count, int *value, float x, float y, float w)
{
	float gap = 6, cw = (w - gap * (count - 1)) / count;
	for(int i = 0; i < count; i++)
		if(chip(id0 + i, labels[i], x + i * (cw + gap), y, cw, 30, *value == i))
			*value = i;
}

static void switch_row(int id, const wchar_t *label, const wchar_t *tip, float x, float y, float w, int *value)
{
	text(label, ui.body, x, y, w - 50, 30, C_TEXT, 0, 1);
	if(mouse_in(x, y, w, 34))
		ui.tip = tip;
	if(switch_toggle(id, x + w - 46, y + 2, *value != 0))
		*value = !*value;
}

static bool settings_dirty(const Settings *a, const Settings *b)
{
	return a->res != b->res || a->fps != b->fps || a->kbps != b->kbps || a->h265 != b->h265 || a->hw != b->hw
		|| a->ao_res != b->ao_res || a->ao_kbps != b->ao_kbps || a->loss_pct != b->loss_pct
		|| a->no_controller != b->no_controller || a->wifi_opt != b->wifi_opt || a->autostart_audio != b->autostart_audio
		|| a->win_start != b->win_start || a->tray != b->tray || a->update_check != b->update_check || a->auto_wake != b->auto_wake
		|| a->reconnect != b->reconnect || a->hotkey_audio != b->hotkey_audio || a->hotkey_video != b->hotkey_video || a->hotkey_mute != b->hotkey_mute
		|| strcmp(a->device, b->device) != 0;
}

static void plain_switch(int id, const wchar_t *label, const wchar_t *hint, float x, float y, float w, int *value)
{
	text(label, ui.semi, x, y, w - 60, 26, C_TEXT, 0, 1);
	text(hint, ui.small, x, y + 26, w - 60, 22, C_MUTED, 0, 1);
	if(switch_toggle(id, x + w - 46, y + 10, *value != 0))
		*value = !*value;
}

static void device_picker(float x, float y, float w)
{
	wchar_t device_w[64];
	wcscpy(device_w, tr(L"Windows varsayılanı"));
	if(ui.draft.device[0])
		MultiByteToWideChar(CP_UTF8, 0, ui.draft.device, -1, device_w, 63);
	device_w[63] = 0;
	Hit dev = hit(UI_DEVICE, x, y, w, 40);
	fill_round(x, y, w, 40, 12, mix(WHITE(24), WHITE(48), anim(UI_DEVICE, dev.hot ? 1.0f : 0)));
	text(device_w, ui.body, x + 14, y, w - 28, 40, C_TEXT, 0, 1);
	if(dev.click)
		ui.open_device_menu = true;
}

static void page_settings_simple(void)
{
	Settings *d = &ui.draft;
	wchar_t t[64];
	float lx = 24, rx = 468, pw = 428, py = 78, ph = 506;
	panel(lx, py, pw, ph);
	panel(rx, py, pw, ph);

	float x = lx + 24, w = pw - 48, y = py + 20;
	heading(L"SES", x, y);
	y += 36;
	text(L"Ses nereden çıksın?", ui.semi, x, y, w, 26, C_TEXT, 0, 1);
	device_picker(x, y + 30, w);
	y += 92;
	text(L"Ses seviyesi", ui.semi, x, y, w, 26, C_TEXT, 0, 1);
	if(slider(UI_VOLUME, x + 8, y + 32, w - 80, &d->volume, 0, 200))
		volume_set(d->volume);
	swprintf(t, 64, tr(L"%%%d"), d->volume);
	text(t, ui.semi, x + w - 64, y + 32, 64, 28, C_TEXT, 2, 1);
	y += 86;
	plain_switch(UI_AUTOSTART, L"Açılışta ses", L"Uygulama açılınca ses başlasın", x, y, w, &d->autostart_audio);
	y += 66;
	plain_switch(UI_AUTOWAKE, L"PS5'i uyandır", L"Bağlanırken PS5 uykudaysa açılsın", x, y, w, &d->auto_wake);

	x = rx + 24;
	y = py + 20;
	heading(L"UYGULAMA", x, y);
	y += 40;
	plain_switch(UI_WINSTART, L"Windows ile başlat", L"Bilgisayar açılınca uygulama da açılsın", x, y, w, &d->win_start);
	y += 66;
	plain_switch(UI_TRAY, L"Tepsiye küçült", L"Küçültünce saatin yanında dursun", x, y, w, &d->tray);
	y += 66;
	plain_switch(UI_UPDATES, L"Güncelleme denetle", L"Yeni sürüm çıkınca haber versin", x, y, w, &d->update_check);

	y = py + ph - 124;
	if(pill_button(UI_ADVANCED, L"Gelişmiş ayarlar", x, y, w, 46, false, true))
	{
		ui.advanced = true;
		ui.again = true;
	}
	y += 58;
	swprintf(t, 64, L"PS Stream v%hs", APP_VERSION);
	text(t, ui.semi, x, y, 150, 34, C_MUTED, 0, 1);
	if(pill_button(UI_OPEN_REPO, L"GitHub", x + w - 114, y, 114, 34, false, true))
		ShellExecuteW(NULL, L"open", REPO_URL, NULL, NULL, SW_SHOWNORMAL);

	// Nothing here reconnects the stream, so there's no reason to ask for an Apply press
	Settings current = settings_get();
	if(settings_dirty(d, &current))
		settings_commit(d);
}

static void page_settings(void)
{
	if(!ui.advanced)
	{
		page_settings_simple();
		return;
	}
	Settings *d = &ui.draft;
	wchar_t t[64];
	float lx = 24, rx = 468, pw = 428, py = 78, ph = 506;
	panel(lx, py, pw, ph);
	panel(rx, py, pw, ph);

	static const wchar_t *const res_names[] = { L"360p", L"540p", L"720p", L"1080p" };
	static const wchar_t *const fps_names[] = { L"30 FPS", L"60 FPS" };
	static const wchar_t *const codec_names[] = { L"H.264", L"H.265" };

	float x = lx + 20, cx = lx + 150, cw = pw - 170, y = py + 14;
	heading(L"GÖRÜNTÜ MODU", x, y);
	y += 30;
	row_label(L"Çözünürlük", L"Yüksek çözünürlük daha fazla veri ister. PS5 360p'nin altını kabul etmez.", x, y, pw - 40);
	int res_i = d->res - RES_MIN;
	chips_row(UI_RES0, res_names, RES_COUNT - RES_MIN, &res_i, cx, y, cw);
	d->res = res_i + RES_MIN;
	y += 42;
	row_label(L"Kare hızı", L"60 FPS daha akıcıdır ama daha fazla veri ister.", x, y, pw - 40);
	int fps_i = d->fps == 60;
	chips_row(UI_FPS0, fps_names, 2, &fps_i, cx, y, cw);
	d->fps = fps_i ? 60 : 30;
	y += 42;
	row_label(L"Bitrate", L"Yüksek bitrate daha temiz görüntü verir, Wi-Fi zayıfsa takılma yapar.", x, y, pw - 40);
	int steps = d->kbps / 500;
	slider(UI_BITRATE, cx + 8, y, cw - 94, &steps, 1, 200);
	d->kbps = steps * 500;
	bitrate_text(t, 64, d->kbps);
	text(t, ui.body, cx + cw - 80, y, 80, 30, C_TEXT, 2, 1);
	y += 42;
	row_label(L"Codec", L"H.265 aynı bitrate'te daha temizdir. H.264 daha uyumludur.", x, y, pw - 40);
	chips_row(UI_CODEC0, codec_names, 2, &d->h265, cx, y, cw);
	y += 42;
	row_label(L"Donanımla çöz", L"Görüntüyü ekran kartı çözer, işlemci rahatlar. Açık kalması önerilir.", x, y, pw - 40);
	if(switch_toggle(UI_HW, cx, y + 2, d->hw != 0))
		d->hw = !d->hw;
	y += 50;
	heading(L"SADECE SES MODU", x, y);
	y += 30;
	row_label(L"İstenen veri", L"PS5 görüntüyü yine 360p gönderir ama çözülmez. Wi-Fi'ın kötüyse en düşükte bırak. PS5 kabul etmezse 2 Mbps'ye döner.", x, y, pw - 40);
	steps = d->ao_kbps / 100;
	slider(UI_AO_BITRATE, cx + 8, y, cw - 94, &steps, 1, 20);
	d->ao_kbps = steps * 100;
	bitrate_text(t, 64, d->ao_kbps);
	text(t, ui.body, cx + cw - 80, y, 80, 30, C_TEXT, 2, 1);
	y += 46;
	heading(L"GENEL", x, y);
	y += 28;
	float col = (pw - 40 - 20) / 2, x2 = x + col + 20;
	switch_row(UI_WIFI, L"Düşük gecikme", L"Bağlıyken Windows'un Wi-Fi taramalarını durdurur, bilgisayarı uyanık tutar. Bağlantı kapanınca eski hâline döner.", x, y, col, &d->wifi_opt);
	switch_row(UI_AUTOSTART, L"Açılışta ses", L"Uygulama açılınca ses kendiliğinden başlar. PS5 kapalıysa açılana kadar dener.", x2, y, col, &d->autostart_audio);
	y += 40;
	switch_row(UI_WINSTART, L"Windows ile başlat", L"Bilgisayar açılınca uygulama saatin yanında küçük olarak başlar.", x, y, col, &d->win_start);
	switch_row(UI_TRAY, L"Tepsiye küçült", L"Küçültünce görev çubuğunda yer kaplamaz, saatin yanındaki simgede durur.", x2, y, col, &d->tray);
	y += 40;
	switch_row(UI_UPDATES, L"Güncelleme denetle", L"Açılışta GitHub'dan son sürüm numarasını sorar. Hakkında bilgi göndermez, kendisi indirmez.", x, y, col, &d->update_check);

	x = rx + 20;
	cx = rx + 150;
	y = py + 14;
	heading(L"SES", x, y);
	y += 30;
	row_label(L"Çıkış aygıtı", L"Oyun sesi bu aygıttan çıkar. Discord'dan ayrı kulaklığa verebilirsin.", x, y, pw - 40);
	wchar_t device_w[64];
	wcscpy(device_w, tr(L"Windows varsayılanı"));
	if(d->device[0])
		MultiByteToWideChar(CP_UTF8, 0, d->device, -1, device_w, 63);
	device_w[63] = 0;
	Hit dev = hit(UI_DEVICE, cx, y, cw, 30);
	fill_round(cx, y, cw, 30, 10, dev.hot ? WHITE(48) : WHITE(24));
	text(device_w, ui.body, cx + 12, y, cw - 24, 30, C_TEXT, 0, 1);
	if(dev.click)
		ui.open_device_menu = true;
	y += 42;
	row_label(L"Ses seviyesi", L"Hemen uygulanır. %100 üstü sesi yükseltir.", x, y, pw - 40);
	if(slider(UI_VOLUME, cx + 8, y, cw - 94, &d->volume, 0, 200))
		volume_set(d->volume);
	swprintf(t, 64, tr(L"%%%d"), d->volume);
	text(t, ui.body, cx + cw - 80, y, 80, 30, C_TEXT, 2, 1);
	y += 42;
	row_label(L"En fazla gecikme", L"Ses bu kadar geride kalırsa birikmiş kısım atılır. Düşük değer daha senkron, yüksek değer daha az kesinti.", x, y, pw - 40);
	if(slider(UI_BUFFER, cx + 8, y, cw - 94, &d->buffer_ms, 40, 500))
	{
		EnterCriticalSection(&g.lock);
		g.settings.buffer_ms = d->buffer_ms;
		LeaveCriticalSection(&g.lock);
		config_set_int("buffer_ms", d->buffer_ms);
	}
	swprintf(t, 64, L"%d ms", d->buffer_ms);
	text(t, ui.body, cx + cw - 80, y, 80, 30, C_TEXT, 2, 1);
	y += 50;
	heading(L"BAĞLANTI", x, y);
	y += 30;
	row_label(L"Kayıp bildirimi", L"PS5'e bildirilen en yüksek paket kaybı. Düşük değer PS5'in kaliteyi düşürmesini azaltır.", x, y, pw - 40);
	slider(UI_LOSS, cx + 8, y, cw - 94, &d->loss_pct, 1, 20);
	swprintf(t, 64, tr(L"%%%d"), d->loss_pct);
	text(t, ui.body, cx + cw - 80, y, 80, 30, C_TEXT, 2, 1);
	y += 40;
	switch_row(UI_RECONNECT, L"Yeniden bağlan", L"Bağlantı koparsa veya PS5 kapanırsa, PS5 dönene kadar tekrar dener.", x, y, col, &d->reconnect);
	switch_row(UI_CONTROLLER, L"DualSense PS5'te", L"Açıkken PS5'e sanal kumanda bildirilmez, DualSense'in PS5'e bağlı kalması için. Bağlantı sorun çıkarırsa kapat.", x + col + 20, y, col, &d->no_controller);
	y += 46;
	heading(L"KISAYOLLAR", x, y);
	y += 28;
	for(int i = 0; i < 3; i++)
	{
		static const int ids[] = { UI_HK_AUDIO, UI_HK_VIDEO, UI_HK_MUTE };
		static const wchar_t *const labels[] = { L"Ses", L"Görüntü", L"Sesi kıs" };
		int id = ids[i];
		row_label(labels[i], L"Tıkla, sonra tuş kombinasyonuna bas. Esc iptal eder, Delete kaldırır.", x, y, pw - 40);
		hotkey_name(i == 0 ? d->hotkey_audio : i == 1 ? d->hotkey_video : d->hotkey_mute, t, 64);
		if(chip(id, ui.capturing == id ? L"Tuşa bas..." : t, cx, y, cw, 30, ui.capturing == id))
		{
			ui.capturing = id;
			SetFocus(g.hwnd);
		}
		y += 36;
	}
	y += 8;
	swprintf(t, 64, L"PS Stream v%hs", APP_VERSION);
	text(t, ui.semi, x, y, 150, 34, C_MUTED, 0, 1);
	if(pill_button(UI_OPEN_LOGS, L"Kayıtları aç", cx + 12, y, 114, 34, false, true))
		data_folder_open();
	if(pill_button(UI_SIMPLE, L"Basit görünüm", cx + 134, y, 114, 34, false, true))
	{
		ui.advanced = false;
		ui.again = true;
	}
}

static void page_pair(void)
{
	float w = (UI_W - 48 - 32) / 3.0f, y = 78, h = 506;
	static const wchar_t *const titles[] = { L"PS5'i bul", L"Hesabını bağla", L"Kodu gir" };
	static const wchar_t *const bodies[] = {
		L"PS5'ini aç.\n\nOtomatik bul'a bas, uygulama PS5'i bulur.\n\nBulamazsa IP adresini kutuya yaz.\nPS5'te: Ayarlar > Sistem > Sistem Yazılımı > Konsol Bilgisi",
		L"PSN ile giriş yap'a bas.\n\nAçılan sayfada giriş yap. Gerisi kendiliğinden olur.\n\nOlmazsa adres çubuğundaki adresi kopyala.\nŞifren sadece Sony'ye gider.",
		L"PS5'te şuraya git:\nAyarlar > Sistem > Uzaktan Oynatma > Cihazı Bağla\n\nEkranda çıkan 8 haneli kodu kutuya yaz. Hepsi bu, bir kere yapman yeterli."
	};
	static const wchar_t *const nums[] = { L"1", L"2", L"3" };
	bool paired_view = g.paired && !ui.repairing;
	// A paired PS5 has done step two even if the account field was cleared afterwards
	bool steps_done[3] = { GetWindowTextLengthW(ui.edits[0]) > 0, GetWindowTextLengthW(ui.edits[1]) > 0 || paired_view, paired_view };
	float field_y = y + 372;
	int next_step = !steps_done[0] ? 0 : !steps_done[1] ? 1 : !steps_done[2] ? 2 : 3;
	for(int i = 0; i < 3; i++)
	{
		float x = 24 + i * (w + 16);
		panel(x, y, w, h);
		stroke_round(x, y, w, h, 22, 2, COLOR((int)(255 * anim(UI_MAX + UI_FIND + i, next_step == i ? 1.0f : 0)), 10, 112, 230));
		dot(x + 20, y + 20, 36, steps_done[i] ? COLOR(255, 34, 160, 90) : C_ACCENT);
		if(steps_done[i])
			text(ICON_CHECK, ui.icon, x + 20, y + 20, 36, 36, C_TEXT, 1, 1);
		else
			text(nums[i], ui.title, x + 20, y + 20, 36, 36, C_TEXT, 1, 1);
		text(titles[i], ui.title, x + 66, y + 20, w - 80, 36, C_TEXT, 0, 1);
		bool done = i == 2 && paired_view;
		text(done ? L"PS5 eşleşti. Bir daha kod girmen gerekmez.\n\nOynat sayfasından Ses veya Görüntü'yü aç, yeterli.\n\nKodu sadece başka bir hesap bağlamak istersen girersin." : bodies[i], ui.body, x + 20, y + 74, w - 40, 284, WHITE(205), 0, 0);
		if(!done)
		{
			fill_round(x + 20, field_y, w - 40, 44, 14, C_FIELD);
			if(GetFocus() == ui.edits[i])
				stroke_round(x + 20, field_y, w - 40, 44, 14, 2, C_ACCENT);
		}
	}
	ShowWindow(ui.edits[2], paired_view ? SW_HIDE : SW_SHOW);
	float x0 = 24, x1 = 24 + w + 16, x2 = 24 + 2 * (w + 16), by = y + 432, bw = w - 40;
	if(pill_button(UI_FIND, finding ? L"Aranıyor..." : L"Otomatik bul", x0 + 20, by, bw, 46, next_step == 0, !finding))
		find_ps5();
	if(pill_button(UI_LOGIN, ui.logging_in ? L"Giriş bekleniyor (iptal)" : L"PSN ile giriş yap", x1 + 20, by, bw - 58, 46, next_step == 1, true))
	{
		if(ui.logging_in)
			login_cancel = 1;
		else
		{
			ui.logging_in = true;
			CloseHandle(CreateThread(NULL, 0, login_main, NULL, 0, NULL));
			set_status(L"Tarayıcında açılan Sony sayfasından giriş yap");
		}
	}
	if(pill_button(UI_ACCOUNT_CLEAR, L"Sil", x1 + 20 + bw - 50, by, 50, 46, false, true))
	{
		SetDlgItemTextW(g.hwnd, ID_ACCOUNT, L"");
		ui.repairing = true;
		ui.again = true;
		set_status(L"Hesap kimliği silindi. Yeni hesapla giriş yapıp eşleştirebilirsin");
	}
	if(mouse_in(x1 + 20 + bw - 50, by, 50, 46))
		ui.tip = L"Kayıtlı hesap kimliğini siler. Eski eşleştirme, yenisini yapana kadar çalışır.";
	if(paired_view)
	{
		if(pill_button(UI_REPAIR, L"Başka hesapla eşleştir", x2 + 20, by - 56, bw, 46, false, true))
		{
			ui.repairing = true;
			ui.again = true;
		}
		if(pill_button(UI_PAIR, L"Oynat'a dön", x2 + 20, by, bw, 46, true, true))
			show_page(PAGE_PLAY);
	}
	else
	{
		// The old pairing keeps working until a new one replaces it, so this can always be undone
		if(g.paired && pill_button(UI_REPAIR, L"Vazgeç", x2 + 20, by - 56, bw, 46, false, true))
		{
			ui.repairing = false;
			ui.again = true;
			set_status(L"Eski eşleştirme kullanılmaya devam ediyor");
		}
		if(pill_button(UI_PAIR, g.pairing ? L"Eşleştiriliyor..." : L"Eşleştir", x2 + 20, by, bw, 46, next_step == 2, !g.pairing))
			pair_start();
	}
}

static void top_bar(void)
{
	GdipFlush(ui.gx, FlushIntentionSync);
	DrawIconEx(ui.back_dc, S(24), S(18), ui.logo, S(44), S(44), 0, NULL, DI_NORMAL);
	text(L"PS Stream", ui.title, 76, 18, 104, 44, C_TEXT, 0, 1);

	static const wchar_t *const names[] = { L"Oynat", L"Ayarlar", L"Eşleştirme" };
	float x = 182, y = 20, h = 40, w = 106;
	fill_round(x, y, w * 3 + 8, h, h / 2, WHITE(18));
	stroke_round(x, y, w * 3 + 8, h, h / 2, 1, WHITE(14));
	float slide = anim(UI_NAV0 + UI_MAX, (float)ui.page);
	for(int i = 0; i < 3; i++)
	{
		float bx = x + 4 + i * w;
		Hit b = hit(UI_NAV0 + i, bx, y + 4, w, h - 8);
		float hover = anim(UI_NAV0 + i, b.hot && ui.page != i ? 1.0f : 0);
		if(hover > 0)
			fill_round(bx, y + 4, w, h - 8, (h - 8) / 2, COLOR((int)(18 * hover), 255, 255, 255));
	}
	fill_round(x + 4 + slide * w, y + 4, w, h - 8, (h - 8) / 2, C_ACCENT);
	for(int i = 0; i < 3; i++)
	{
		float bx = x + 4 + i * w;
		Hit b = hit(UI_NAV0 + i, bx, y + 4, w, h - 8);
		text(names[i], ui.semi, bx, y + 4, w, h - 8, ui.page == i || b.hot ? C_TEXT : C_MUTED, 1, 1);
		if(b.click)
			show_page(i);
	}

	if(chip(UI_LANG0, L"TR", 590, y + 4, 42, h - 8, !lang_english) && lang_english)
	{
		lang_english = false;
		config_set("lang", "tr");
		edit_cues();
	}
	if(chip(UI_LANG0 + 1, L"EN", 636, y + 4, 42, h - 8, lang_english) && !lang_english)
	{
		lang_english = true;
		config_set("lang", "en");
		edit_cues();
	}

	wchar_t host[300];
	char host_text[256];
	host_get(host_text, sizeof(host_text));
	bool live = (g.want_audio || g.want_video) && g.connected;
	wcscpy(host, !g.paired ? L"PS5 eşleşmedi" : live ? L"PS5 bağlı" : L"PS5 bağlı değil");
	float pw = 210, px = UI_W - 24 - pw;
	fill_round(px, y, pw, h, h / 2, WHITE(18));
	stroke_round(px, y, pw, h, h / 2, 1, WHITE(14));
	dot(px + 16, y + 15, 10, live ? C_GREEN : g.paired ? WHITE(120) : C_AMBER);
	if(g.paired && mouse_in(px, y, pw, h))
	{
		static wchar_t address_tip[300];
		swprintf(address_tip, 300, tr(L"PS5 adresi: %hs"), host_text);
		ui.tip = address_tip;
	}
	text(host, ui.body, px + 34, y, pw - 44, h, C_TEXT, 0, 1);
}

static void status_bar(void)
{
	float x = 24, y = UI_H - 64, w = UI_W - 48, h = 48;
	fill_round(x, y, w, h, 16, COLOR(190, 36, 38, 46));
	stroke_round(x, y, w, h, 16, 1, WHITE(14));
	bool on = g.want_audio || g.want_video;
	bool live = on && g.connected;
	text(live ? ICON_CHECK : on ? ICON_SYNC : ICON_INFO, ui.icon, x + 8, y, 36, h, live ? C_GREEN : on ? C_AMBER : C_MUTED, 1, 1);

	wchar_t status[256];
	EnterCriticalSection(&g.lock);
	wcscpy(status, g.status);
	LeaveCriticalSection(&g.lock);

	Settings current = settings_get();
	float tw = w - 56;
	if(ui.page == PAGE_SETTINGS && settings_dirty(&ui.draft, &current))
	{
		tw -= 130;
		if(pill_button(UI_APPLY, L"Uygula", x + w - 124, y + 7, 114, 34, true, true))
		{
			settings_commit(&ui.draft);
			set_status(hotkeys_register(&ui.draft) ? L"Ayarlar uygulandı" : L"Ayarlar uygulandı ama bir kısayol başka programda kullanılıyor");
		}
	}
	else if(g.ps5_standby)
	{
		tw -= 170;
		if(pill_button(UI_WAKE, L"PS5'i uyandır", x + w - 164, y + 7, 154, 34, true, true))
			CloseHandle(CreateThread(NULL, 0, ps5_wake_thread, NULL, 0, NULL));
	}
	else if(ui.weak && g.want_video && preset_lower(&current) >= 0)
	{
		tw -= 170;
		if(pill_button(UI_LOWER, L"Kaliteyi düşür", x + w - 164, y + 7, 154, 34, true, true))
		{
			const Preset *p = &presets[preset_lower(&current)];
			current.res = p->res;
			current.fps = p->fps;
			current.kbps = p->kbps;
			current.h265 = p->h265;
			ui.weak = false;
			settings_commit(&current);
		}
	}
	else if(g.update_ready)
	{
		bool one_click = update_available_here();
		wchar_t label[64];
		swprintf(label, 64, tr(one_click ? L"Güncelle: %hs" : L"Yeni sürüm: %hs"), g.update_tag);
		tw -= 190;
		if(pill_button(UI_UPDATE_NOW, g.update_busy ? L"Güncelleniyor..." : label, x + w - 184, y + 7, 174, 34, true, !g.update_busy))
		{
			if(one_click)
				update_begin();
			else
				ShellExecuteW(NULL, L"open", RELEASES_URL, NULL, NULL, SW_SHOWNORMAL);
		}
	}
	else if(ui.page == PAGE_PLAY)
	{
		tw -= 120;
		if(pill_button(UI_DETAILS, ui.details ? L"Detayları gizle" : L"Detaylar", x + w - 134, y + 7, 124, 34, false, true))
		{
			ui.details = !ui.details;
			config_set_int("details", ui.details);
		}
	}
	const wchar_t *shown = status;
	if(g.update_note && GetTickCount() - g.update_note_at < 8000 && !g.update_busy)
		shown = g.update_note;
	text(ui.tip ? ui.tip : shown, ui.body, x + 46, y, tw, h, C_TEXT, 0, 1);
}

static void devices_load(void)
{
	ui.device_count = SDL_GetNumAudioDevices(0);
	if(ui.device_count <= 0)
	{
		ui.device_count = 0;
		return;
	}
	ui.devices = calloc(ui.device_count, sizeof(char *));
	for(int i = 0; i < ui.device_count; i++)
	{
		const char *name = SDL_GetAudioDeviceName(i, 0);
		ui.devices[i] = strdup(name ? name : "");
	}
}

static void ui_frame(void)
{
	if(ui.in_frame)
		return;
	ui.in_frame = true;
	double frame_at = now_ms();
	double frame_ms = frame_at - ui.anim_last;
	ui.anim_last = frame_at;
	ui.anim_step = 1 - (float)exp(-(frame_ms > 33 ? 33 : frame_ms) / 45.0);
	ui.animating = false;
	BitBlt(ui.back_dc, 0, 0, S(UI_W), S(UI_H), ui.bg_dc, 0, 0, SRCCOPY);
	int passes = 0;
	do
	{
		if(passes)
			BitBlt(ui.back_dc, 0, 0, S(UI_W), S(UI_H), ui.bg_dc, 0, 0, SRCCOPY);
		ui.again = false;
		ui.tip = NULL;
		memset(ui.rect_set, 0, sizeof(ui.rect_set));
		top_bar();
		if(ui.page == PAGE_PLAY)
			page_play();
		else if(ui.page == PAGE_SETTINGS)
			page_settings();
		else
			page_pair();
		status_bar();
		ui.release = false;
		ui.press = false;
	} while(ui.again && ++passes < 3);

	ui.hot_id = UI_NONE;
	for(int i = 1; i < UI_MAX; i++)
		if(ui.rect_set[i] && mouse_in(ui.rects[i].X, ui.rects[i].Y, ui.rects[i].Width, ui.rects[i].Height))
			ui.hot_id = i;
#ifdef PSSTREAM_TEST
	{
		static int frames;
		static double spent, window_start;
		frames++;
		spent += now_ms() - frame_at;
		if(frame_at - window_start > 5000)
		{
			CHIAKI_LOGI(&g.log, "UI frames: %d in 5 s, %.1f ms each, animating=%d", frames, spent / frames, ui.animating);
			frames = 0;
			spent = 0;
			window_start = frame_at;
		}
	}
#endif
	// Fast frames only while something moves, so the audio-only mode stays cheap
	if(ui.animating)
		SetTimer(g.hwnd, 2, 15, NULL);
	else
		KillTimer(g.hwnd, 2);
	ui.in_frame = false;
	InvalidateRect(g.hwnd, NULL, FALSE);

	if(ui.open_device_menu)
	{
		ui.open_device_menu = false;
		for(int i = 0; i < ui.device_count; i++)
			free(ui.devices[i]);
		free(ui.devices);
		ui.devices = NULL;
		devices_load();
		HMENU menu = CreatePopupMenu();
		AppendMenuW(menu, ui.draft.device[0] ? MF_STRING : MF_STRING | MF_CHECKED, 1, tr(L"Windows varsayılanı"));
		for(int i = 0; i < ui.device_count; i++)
		{
			wchar_t name[256];
			MultiByteToWideChar(CP_UTF8, 0, ui.devices[i], -1, name, 256);
			AppendMenuW(menu, strcmp(ui.devices[i], ui.draft.device) == 0 ? MF_STRING | MF_CHECKED : MF_STRING, i + 2, name);
		}
		POINT pt;
		GetCursorPos(&pt);
		int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, g.hwnd, NULL);
		DestroyMenu(menu);
		if(cmd == 1)
			ui.draft.device[0] = 0;
		else if(cmd >= 2)
			snprintf(ui.draft.device, sizeof(ui.draft.device), "%s", ui.devices[cmd - 2]);
		ui_frame();
	}
}

// ---------- window ----------

static void capture_hotkey(int vk)
{
	if(vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU || vk == VK_LWIN || vk == VK_RWIN)
		return;
	int *target = ui.capturing == UI_HK_AUDIO ? &ui.draft.hotkey_audio : ui.capturing == UI_HK_VIDEO ? &ui.draft.hotkey_video : &ui.draft.hotkey_mute;
	if(vk == VK_DELETE)
		*target = 0;
	else if(vk != VK_ESCAPE)
	{
		int f = (GetKeyState(VK_CONTROL) < 0 ? HOTKEYF_CONTROL : 0) | (GetKeyState(VK_MENU) < 0 ? HOTKEYF_ALT : 0) | (GetKeyState(VK_SHIFT) < 0 ? HOTKEYF_SHIFT : 0);
		// A bare ordinary key would be taken away from every other program
		if(!(f & (HOTKEYF_CONTROL | HOTKEYF_ALT)) && !(vk >= VK_F1 && vk <= VK_F24))
		{
			set_status(L"Ctrl veya Alt ile birlikte bas, ya da bir F tuşu seç");
			return;
		}
		*target = MAKEWORD(vk, f);
	}
	ui.capturing = 0;
	ui_frame();
}

static bool tray_icon(DWORD action)
{
	NOTIFYICONDATAW data = { 0 };
	data.cbSize = sizeof(data);
	data.hWnd = g.hwnd;
	data.uID = 1;
	data.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
	data.uCallbackMessage = WM_APP_TRAY;
	data.hIcon = LoadIconW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(1));
	bool on = g.want_audio || g.want_video;
	swprintf(data.szTip, 128, L"PS Stream: %ls", tr(!on ? L"Kapalı" : g.connected ? L"PS5 bağlı" : L"Bağlanıyor"));
	return Shell_NotifyIconW(action, &data) != 0;
}

// Someone with the window tucked away still needs to know the sound is gone, and why.
static void tray_note(const wchar_t *text)
{
	NOTIFYICONDATAW data = { 0 };
	data.cbSize = sizeof(data);
	data.hWnd = g.hwnd;
	data.uID = 1;
	data.uFlags = NIF_INFO;
	data.dwInfoFlags = NIIF_NONE | NIIF_NOSOUND;
	wcsncpy(data.szInfo, tr(text), 255);
	wcscpy(data.szInfoTitle, L"PS Stream");
	bool shown = Shell_NotifyIconW(NIM_MODIFY, &data) != 0;
	CHIAKI_LOGI(&g.log, "Tray note: %s", shown ? "shown" : "failed");
}

// Runs from the half-second timer, whether the window is showing or not.
static void connection_watch(void)
{
	bool live = (g.want_audio || g.want_video) && g.connected;
	if(live != ui.was_live)
	{
		ui.was_live = live;
		if(ui.in_tray)
		{
			tray_icon(NIM_MODIFY);
			if(live)
				tray_note(L"PS5'e bağlandı");
			else if(g.want_audio || g.want_video)
				tray_note(L"PS5 bağlantısı koptu, yeniden deneniyor");
		}
		ui.weak = false;
		ui.weak_lost = g.video_lost_total;
		ui.weak_drops = g.audio_dropouts;
		ui.weak_checked_at = GetTickCount();
	}
	DWORD now = GetTickCount();
	if(!live || now - ui.weak_checked_at < WEAK_WINDOW_MS)
		return;
	int lost = g.video_lost_total - ui.weak_lost, drops = g.audio_dropouts - ui.weak_drops;
	ui.weak_lost = g.video_lost_total;
	ui.weak_drops = g.audio_dropouts;
	ui.weak_checked_at = now;
	bool was_weak = ui.weak;
	ui.weak = false;
	if((lost >= WEAK_LOST_FRAMES || drops >= WEAK_DROPOUTS) && (!ui.weak_shown_at || now - ui.weak_shown_at > WEAK_REPEAT_MS))
	{
		CHIAKI_LOGI(&g.log, "Weak connection: %d lost frames, %d audio dropouts in the last %d s", lost, drops, WEAK_WINDOW_MS / 1000);
		ui.weak = true;
		ui.weak_shown_at = now;
		set_status(g.want_video ? L"Bağlantı zayıf, görüntü kare kaybediyor" : L"Wi-Fi zayıf, ses kesiliyor. Modeme yaklaşmak veya 5 GHz'e geçmek iyi gelir");
	}
	if(was_weak != ui.weak)
		ui_frame();
}

static void tray_enter(void)
{
	ui.in_tray = tray_icon(NIM_ADD);
	if(ui.in_tray)
		ShowWindow(g.hwnd, SW_HIDE);
	CHIAKI_LOGI(&g.log, "Tray icon: %s", ui.in_tray ? "on" : "failed");
}

static void tray_leave(void)
{
	if(ui.in_tray)
		tray_icon(NIM_DELETE);
	ui.in_tray = false;
	ShowWindow(g.hwnd, SW_RESTORE);
	SetForegroundWindow(g.hwnd);
	ui_frame();
}

static void tray_menu(void)
{
	HMENU menu = CreatePopupMenu();
	AppendMenuW(menu, MF_STRING, 1, tr(L"Pencereyi göster"));
	AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
	AppendMenuW(menu, MF_STRING | (g.want_audio ? MF_CHECKED : 0), 2, tr(L"Ses"));
	AppendMenuW(menu, MF_STRING | (g.want_video ? MF_CHECKED : 0), 3, tr(L"Görüntü"));
	AppendMenuW(menu, MF_STRING | (g.muted ? MF_CHECKED : 0), 5, tr(L"Sesi kıs"));
	AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
	AppendMenuW(menu, MF_STRING, 4, tr(L"Çıkış"));
	POINT pt;
	GetCursorPos(&pt);
	SetForegroundWindow(g.hwnd);
	int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, g.hwnd, NULL);
	DestroyMenu(menu);
	if(cmd == 1)
		tray_leave();
	else if(cmd == 2 || cmd == 3)
		toggle(cmd == 2 ? UI_AUDIO : UI_VIDEO);
	else if(cmd == 5)
		mute_toggle();
	else if(cmd == 4)
		DestroyWindow(g.hwnd);
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	if(ui.taskbar_created && msg == ui.taskbar_created && ui.in_tray)
		tray_icon(NIM_ADD); // Explorer restarted and forgot the icon
	switch(msg)
	{
		case WM_POWERBROADCAST:
			// Back from sleep: whatever was being waited for may be there now
			if(wp == PBT_APMRESUMEAUTOMATIC)
			{
				g.network_changed = 1;
				SetEvent(g.wake);
			}
			return TRUE;
		case WM_SIZE:
			if(wp == SIZE_MINIMIZED && settings_get().tray && !ui.in_tray)
				tray_enter();
			break;
		case WM_APP_TRAY:
			if(lp == WM_LBUTTONUP)
				tray_leave();
			else if(lp == WM_RBUTTONUP)
				tray_menu();
			return 0;
		case WM_TIMER:
			if(wp == 1)
				connection_watch();
			if(wp == 2)
			{
				ui_frame();
				return 0;
			}
			if(wp == TIMER_REGIST_RETRY)
			{
				KillTimer(hwnd, TIMER_REGIST_RETRY);
				g.pairing = false;
				pair_start();
				ui_frame();
				return 0;
			}
			// The blinking dot and the live numbers. Nothing else changes by itself, so an open
			// window costs no CPU while connected with the details hidden.
			if(IsWindowVisible(hwnd) && !IsIconic(hwnd) && ui.page == PAGE_PLAY && (g.want_audio || g.want_video) && (!g.connected || (ui.details && ++ui.timer_ticks % 2 == 0)))
				ui_frame();
			return 0;
		case WM_SETCURSOR:
			if((HWND)wp == hwnd && LOWORD(lp) == HTCLIENT && ui.hot_id != UI_NONE)
			{
				SetCursor(LoadCursorW(NULL, IDC_HAND));
				return TRUE;
			}
			break;
		case WM_APP_HOST_CHANGED:
		{
			char host[256];
			host_get(host, sizeof(host));
			SetDlgItemTextA(hwnd, ID_HOST, host);
			config_set("host", host);
			ui_frame();
			return 0;
		}
		case WM_COMMAND:
			if(HIWORD(wp) == EN_SETFOCUS || HIWORD(wp) == EN_KILLFOCUS)
				ui_frame();
			if(LOWORD(wp) == ID_PIN && HIWORD(wp) == EN_CHANGE && GetWindowTextLengthW(ui.edits[2]) == 8 && !g.pairing)
			{
				pair_start();
				ui_frame();
			}
			if(LOWORD(wp) == ID_HOST && HIWORD(wp) == EN_CHANGE)
			{
				char host[256];
				get_text(ID_HOST, host, sizeof(host));
				config_set("host", host);
			}
			if(LOWORD(wp) == ID_ACCOUNT && HIWORD(wp) == EN_CHANGE)
			{
				get_text(ID_ACCOUNT, pasted_redirect, sizeof(pasted_redirect));
				if(!strstr(pasted_redirect, "code="))
					config_set("account", pasted_redirect);
				else if(!ui.logging_in)
				{
					ui.logging_in = true;
					set_status(L"Hesap bilgisi alınıyor...");
					CloseHandle(CreateThread(NULL, 0, redirect_exchange_main, NULL, 0, NULL));
				}
			}
			return 0;
		case WM_PAINT:
		{
			PAINTSTRUCT ps;
			HDC dc = BeginPaint(hwnd, &ps);
			BitBlt(dc, 0, 0, S(UI_W), S(UI_H), ui.back_dc, 0, 0, SRCCOPY);
			EndPaint(hwnd, &ps);
			return 0;
		}
		case WM_ERASEBKGND:
			return 1;
		case WM_MOUSEMOVE:
		{
			ui.mx = (float)(short)LOWORD(lp);
			ui.my = (float)(short)HIWORD(lp);
			TRACKMOUSEEVENT track = { sizeof(track), TME_LEAVE, hwnd, 0 };
			TrackMouseEvent(&track);
			int hot = UI_NONE;
			for(int i = 1; i < UI_MAX; i++)
				if(ui.rect_set[i] && mouse_in(ui.rects[i].X, ui.rects[i].Y, ui.rects[i].Width, ui.rects[i].Height))
					hot = i;
			// Settings tips follow the mouse between rows, so that page always redraws
			if(hot != ui.hot_id || ui.active || ui.page == PAGE_SETTINGS)
				ui_frame();
			return 0;
		}
		case WM_MOUSELEAVE:
			if(ui.active)
				return 0;
			ui.mx = ui.my = -1;
			ui_frame();
			return 0;
		case WM_LBUTTONDOWN:
			ui.mx = (float)(short)LOWORD(lp);
			ui.my = (float)(short)HIWORD(lp);
			ui.press = true;
			ui.capturing = 0;
			SetFocus(hwnd);
			SetCapture(hwnd);
			ui_frame();
			return 0;
		case WM_LBUTTONUP:
			ui.mx = (float)(short)LOWORD(lp);
			ui.my = (float)(short)HIWORD(lp);
			ReleaseCapture();
			ui.released_id = ui.active;
			ui.active = 0;
			ui.release = true;
			ui_frame();
			return 0;
		case WM_KEYDOWN:
		case WM_SYSKEYDOWN:
			if(ui.capturing)
			{
				capture_hotkey((int)wp);
				return 0;
			}
			break;
		case WM_CTLCOLOREDIT:
			SetTextColor((HDC)wp, RGB(255, 255, 255));
			SetBkColor((HDC)wp, RGB(43, 45, 51));
			return (LRESULT)ui.field_brush;
		case WM_HOTKEY:
			if(wp == HOTKEY_MUTE)
				mute_toggle();
			else
				toggle(wp == HOTKEY_AUDIO ? UI_AUDIO : UI_VIDEO);
			ui_frame();
			return 0;
		case WM_APP_STATUS:
			ui_frame();
			return 0;
		case WM_APP_VIDEO_CLOSED:
			if(g.want_video)
				toggle(UI_VIDEO);
			ui_frame();
			return 0;
		case WM_APP_ALL_OFF:
			g.want_audio = 0;
			g.want_video = 0;
			SetEvent(g.wake);
			ui_frame();
			return 0;
		case WM_APP_PAIRED:
			pair_finished();
			ui.repairing = !g.regist_ok && ui.repairing;
			ui_frame();
			return 0;
		case WM_APP_LOGIN:
			ui.logging_in = false;
			if(wp == 1)
			{
				SetDlgItemTextA(hwnd, ID_ACCOUNT, login_account_id);
				// The browser has focus after signing in, and the code goes in here next
				if(ui.in_tray)
					tray_leave();
				SetForegroundWindow(hwnd);
				ui.repairing = true;
				ui.again = true;
				ShowWindow(ui.edits[2], SW_SHOW);
				SetFocus(ui.edits[2]);
				set_status(L"Hesap bağlandı. Şimdi PS5'teki 8 haneli kodu yaz");
				if(GetWindowTextLengthW(ui.edits[2]) == 8 && !g.pairing)
					pair_start();
			}
			else if(wp == 3)
				set_status(L"Giriş iptal edildi veya süresi doldu");
			else
				set_status(L"Sony'den hesap bilgisi alınamadı, tekrar dene");
			ui_frame();
			return 0;
#ifdef PSSTREAM_TEST
		case WM_APP_TEST_SHOT:
		{
			ui_frame();
			// GDI+ cannot read a bitmap that is selected into a DC
			HDC copy_dc = CreateCompatibleDC(ui.back_dc);
			HBITMAP copy = CreateCompatibleBitmap(ui.back_dc, S(UI_W), S(UI_H));
			HGDIOBJ old = SelectObject(copy_dc, copy);
			BitBlt(copy_dc, 0, 0, S(UI_W), S(UI_H), ui.back_dc, 0, 0, SRCCOPY);
			SelectObject(copy_dc, old);
			DeleteDC(copy_dc);
			GpBitmap *bmp;
			GdipCreateBitmapFromHBITMAP(copy, NULL, &bmp);
			static const CLSID png = { 0x557cf406, 0x1a04, 0x11d3, { 0x9a, 0x73, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e } };
			GdipSaveImageToFile(bmp, (const wchar_t *)lp, &png, NULL);
			GdipDisposeImage((GpImage *)bmp);
			DeleteObject(copy);
			SetEvent((HANDLE)wp);
			return 0;
		}
#endif
		case WM_DESTROY:
		{
			WINDOWPLACEMENT place = { sizeof(place) };
			if(GetWindowPlacement(hwnd, &place))
			{
				config_set_int("win_x", place.rcNormalPosition.left);
				config_set_int("win_y", place.rcNormalPosition.top);
			}
			if(ui.in_tray)
				tray_icon(NIM_DELETE);
			PostQuitMessage(0);
			return 0;
		}
	}
	return DefWindowProcW(hwnd, msg, wp, lp);
}

static HWND add_edit(int id, const wchar_t *value, DWORD style, float x, float y, float w)
{
	HFONT font = CreateFontW(-S(15), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
	HWND e = CreateWindowExW(0, L"EDIT", value, WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL | style, S((int)x), S((int)y), S((int)w), S(24),
			g.hwnd, (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
	SendMessageW(e, WM_SETFONT, (WPARAM)font, TRUE);
	return e;
}

static GpFont *font_make(const wchar_t *family, float size)
{
	GpFontFamily *f;
	GpFont *font;
	if(GdipCreateFontFamilyFromName(family, NULL, &f) != Ok)
		GdipGetGenericFontFamilySansSerif(&f);
	GdipCreateFont(f, F(size), FontStyleRegular, UnitPixel, &font);
	return font;
}

static HBITMAP surface_create(HDC dc, GpGraphics **gx)
{
	BITMAPINFO info = { 0 };
	info.bmiHeader.biSize = sizeof(info.bmiHeader);
	info.bmiHeader.biWidth = S(UI_W);
	info.bmiHeader.biHeight = -S(UI_H);
	info.bmiHeader.biPlanes = 1;
	info.bmiHeader.biBitCount = 32;
	info.bmiHeader.biCompression = BI_RGB;
	void *bits;
	HBITMAP bmp = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, NULL, 0);
	SelectObject(dc, bmp);
	GpBitmap *image;
	GdipCreateBitmapFromScan0(S(UI_W), S(UI_H), S(UI_W) * 4, PixelFormat32bppPARGB, bits, &image);
	GdipGetImageGraphicsContext((GpImage *)image, gx);
	GdipSetSmoothingMode(*gx, SmoothingModeAntiAlias);
	return bmp;
}

static void ui_create(HINSTANCE inst)
{
	ULONG_PTR gdiplus_token;
	GdiplusStartupInput startup = { 1, NULL, FALSE, FALSE };
	GdiplusStartup(&gdiplus_token, &startup, NULL);

	ui.logo = LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, S(44), S(44), LR_DEFAULTCOLOR);
	ui.taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
	ui.small = font_make(L"Segoe UI", 12);
	ui.body = font_make(L"Segoe UI", 14);
	ui.semi = font_make(L"Segoe UI Semibold", 14);
	ui.title = font_make(L"Segoe UI Semibold", 18);
	ui.big = font_make(L"Segoe UI Semibold", 28);
	ui.icon = font_make(L"Segoe MDL2 Assets", 18);
	ui.icon_big = font_make(L"Segoe MDL2 Assets", 30);
	GdipCreateStringFormat(0, LANG_NEUTRAL, &ui.fmt);
	ui.field_brush = CreateSolidBrush(RGB(43, 45, 51));

	WNDCLASSW wc = { 0 };
	wc.lpfnWndProc = wnd_proc;
	wc.hInstance = inst;
	wc.hCursor = LoadCursor(NULL, IDC_ARROW);
	wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
	wc.lpszClassName = WINDOW_CLASS;
	RegisterClassW(&wc);

	DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
	RECT rect = { 0, 0, S(UI_W), S(UI_H) };
	AdjustWindowRect(&rect, style, FALSE);
	g.hwnd = CreateWindowW(wc.lpszClassName, L"PS Stream", style, CW_USEDEFAULT, CW_USEDEFAULT,
			rect.right - rect.left, rect.bottom - rect.top, NULL, NULL, inst, NULL);
	BOOL dark = TRUE;
	DwmSetWindowAttribute(g.hwnd, 20, &dark, sizeof(dark));
	POINT saved = { config_int("win_x", 99999, -32768, 32767), config_int("win_y", 99999, -32768, 32767) };
	POINT corner = { saved.x + 40, saved.y + 40 };
	if(saved.x != 99999 && saved.y != 99999 && MonitorFromPoint(corner, MONITOR_DEFAULTTONULL))
		SetWindowPos(g.hwnd, NULL, saved.x, saved.y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);

	// GDI+ blends in plain memory many times faster than on a bitmap owned by the display driver
	HDC screen = GetDC(g.hwnd);
	ui.back_dc = CreateCompatibleDC(screen);
	ui.bg_dc = CreateCompatibleDC(screen);
	ReleaseDC(g.hwnd, screen);
	GpGraphics *bg;
	ui.back_bmp = surface_create(ui.back_dc, &ui.gx);
	surface_create(ui.bg_dc, &bg);
	background_paint(bg);
	GdipDeleteGraphics(bg);

	GdipSetTextRenderingHint(ui.gx, TextRenderingHintAntiAliasGridFit);

	wchar_t host_w[256];
	MultiByteToWideChar(CP_UTF8, 0, g.host, -1, host_w, 256);
	float w = (UI_W - 48 - 32) / 3.0f, field_y = 78 + 382;
	ui.edits[0] = add_edit(ID_HOST, host_w, 0, 24 + 32, field_y, w - 64);
	wchar_t account_w[128] = L"";
	char account[128];
	config_get("account", account, sizeof(account));
	MultiByteToWideChar(CP_UTF8, 0, account, -1, account_w, 128);
	ui.edits[1] = add_edit(ID_ACCOUNT, account_w, 0, 24 + w + 16 + 32, field_y, w - 64);
	ui.edits[2] = add_edit(ID_PIN, L"", ES_NUMBER, 24 + 2 * (w + 16) + 32, field_y, w - 64);

	edit_cues();
	devices_load();
	ui.details = config_int("details", 0, 0, 1) != 0;
	ui.draft = g.settings;
	ui.mx = ui.my = -1;
	SetTimer(g.hwnd, 1, 500, NULL);
}

#ifdef PSSTREAM_TEST
static void test_shot(const wchar_t *dir, const wchar_t *name)
{
	wchar_t path[MAX_PATH];
	swprintf(path, MAX_PATH, L"%ls\\%ls.png", dir, name);
	HANDLE done = CreateEventW(NULL, FALSE, FALSE, NULL);
	PostMessageW(g.hwnd, WM_APP_TEST_SHOT, (WPARAM)done, (LPARAM)path);
	WaitForSingleObject(done, 5000);
	CloseHandle(done);
}

static void test_log_status(const char *step)
{
	wchar_t status[256];
	EnterCriticalSection(&g.lock);
	wcscpy(status, g.status);
	LeaveCriticalSection(&g.lock);
	char utf8[512];
	WideCharToMultiByte(CP_UTF8, 0, status, -1, utf8, sizeof(utf8), NULL, NULL);
	CHIAKI_LOGI(&g.log, "[%s] audio=%ld video=%ld connected=%ld video_thread=%d status=%s", step, (long)g.want_audio, (long)g.want_video, (long)g.connected, g.video_thread != NULL, utf8);
}

// Clicks by posting real mouse messages to the widget's last drawn position.
static void test_click(int id)
{
	RectF r = ui.rects[id];
	LPARAM pos = MAKELPARAM(S((int)(r.X + r.Width / 2)), S((int)(r.Y + r.Height / 2)));
	PostMessageW(g.hwnd, WM_MOUSEMOVE, 0, pos);
	PostMessageW(g.hwnd, WM_LBUTTONDOWN, MK_LBUTTON, pos);
	PostMessageW(g.hwnd, WM_LBUTTONUP, 0, pos);
	Sleep(150);
}

static DWORD WINAPI ui_test_main(void *arg)
{
	const wchar_t *dir = arg;
	Sleep(300);
	if(!g.paired)
	{
		test_shot(dir, L"pair");
		test_log_status("unpaired start");
		char saved[128];
		get_text(ID_ACCOUNT, saved, sizeof(saved));
		CHIAKI_LOGI(&g.log, "Account field at start: %s", saved);
		if(GetEnvironmentVariableW(L"PSSTREAM_TEST_CLEAR", NULL, 0))
		{
			test_click(UI_ACCOUNT_CLEAR);
			get_text(ID_ACCOUNT, saved, sizeof(saved));
			CHIAKI_LOGI(&g.log, "Account field after delete: '%s'", saved);
		}
		for(int i = 0; i < 150 && finding; i++)
			Sleep(200);
		test_log_status("search finished");
		wchar_t test_pin[16];
		if(GetEnvironmentVariableW(L"PSSTREAM_TEST_PIN", test_pin, 16))
		{
			SetDlgItemTextW(g.hwnd, ID_HOST, L"127.0.0.1");
			SetDlgItemTextW(g.hwnd, ID_ACCOUNT, L"AAAAAAAAAAA=");
			SetDlgItemTextW(g.hwnd, ID_PIN, test_pin);
			Sleep(300);
			for(int i = 0; i < 120 && g.pairing; i++)
				Sleep(100);
			test_log_status("pairing finished");
			get_text(ID_PIN, saved, sizeof(saved));
			CHIAKI_LOGI(&g.log, "PIN field after pairing: '%s', paired=%d", saved, g.paired);
		}
		test_shot(dir, L"pair_found");
		if(GetEnvironmentVariableW(L"PSSTREAM_TEST_LOGIN_URL", NULL, 0))
		{
			test_click(UI_LOGIN);
			for(int i = 0; i < 80 && (ui.logging_in || i < 2); i++)
				Sleep(200);
			test_log_status("login finished");
			char account[64];
			get_text(ID_ACCOUNT, account, sizeof(account));
			CHIAKI_LOGI(&g.log, "Account field: %s", account);
			test_shot(dir, L"pair_login");
		}
		PostMessageW(g.hwnd, WM_CLOSE, 0, 0);
		return 0;
	}
	test_shot(dir, L"play_off");
	test_click(UI_AUDIO);
	Sleep(800);
	test_log_status("audio on");
	test_shot(dir, L"play_audio");
	test_click(UI_MUTE);
	CHIAKI_LOGI(&g.log, "[mute] muted=%d", (int)g.muted);
	test_shot(dir, L"play_muted");
	test_click(UI_MUTE);
	CHIAKI_LOGI(&g.log, "[unmute] muted=%d", (int)g.muted);
	test_click(UI_VIDEO);
	Sleep(2600);
	test_log_status("video on");
	test_shot(dir, L"play_both");
	if(GetEnvironmentVariableW(L"PSSTREAM_TEST_WEAK", NULL, 0))
	{
		Sleep(800);
		ui.weak_checked_at -= WEAK_WINDOW_MS;
		InterlockedExchangeAdd(&g.video_lost_total, 40);
		Sleep(900);
		test_log_status("weak connection");
		test_shot(dir, L"play_weak");
		Settings before = settings_get();
		test_click(UI_LOWER);
		Settings after = settings_get();
		CHIAKI_LOGI(&g.log, "[lower quality] kbps %d to %d, weak=%d", before.kbps, after.kbps, ui.weak);
		Sleep(2600);
	}
	test_click(UI_PRESET0 + 3);
	Sleep(2600);
	test_log_status("preset high");
	test_click(UI_VIDEO);
	Sleep(2600);
	test_log_status("video off");
	test_click(UI_AUDIO);
	Sleep(800);
	test_log_status("audio off");

	test_click(UI_DETAILS);
	Sleep(500);
	test_shot(dir, L"play_details");
	test_click(UI_DETAILS);
	Sleep(500);
	test_click(UI_NAV1);
	test_shot(dir, L"settings");
	test_click(UI_TRAY);
	CHIAKI_LOGI(&g.log, "[simple settings] tray=%d applied without a press", settings_get().tray);
	test_click(UI_TRAY);
	test_click(UI_ADVANCED);
	test_shot(dir, L"settings_advanced");
	test_click(UI_RES0 + 3);
	test_click(UI_FPS0 + 1);
	RectF r = ui.rects[UI_BITRATE];
	PostMessageW(g.hwnd, WM_MOUSEMOVE, 0, MAKELPARAM(S((int)(r.X + 40)), S((int)(r.Y + 14))));
	PostMessageW(g.hwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(S((int)(r.X + 40)), S((int)(r.Y + 14))));
	PostMessageW(g.hwnd, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(S((int)(r.X + r.Width * 0.3f)), S((int)(r.Y + 14))));
	PostMessageW(g.hwnd, WM_LBUTTONUP, 0, MAKELPARAM(S((int)(r.X + r.Width * 0.3f)), S((int)(r.Y + 14))));
	Sleep(200);
	test_click(UI_HK_AUDIO);
	test_shot(dir, L"settings_dirty");
	PostMessageW(g.hwnd, WM_KEYDOWN, VK_ESCAPE, 0);
	Sleep(100);
	CHIAKI_LOGI(&g.log, "[hotkey esc] still capturing=%d", ui.capturing);
	if(GetEnvironmentVariableW(L"PSSTREAM_TEST_WINSTART", NULL, 0))
	{
		test_click(UI_WINSTART);
		test_click(UI_APPLY);
		CHIAKI_LOGI(&g.log, "[win start on] registry=%d", win_start_get());
		test_click(UI_WINSTART);
		test_click(UI_APPLY);
		CHIAKI_LOGI(&g.log, "[win start off] registry=%d", win_start_get());
	}
	test_click(UI_APPLY);
	Settings s = settings_get();
	CHIAKI_LOGI(&g.log, "[applied] res=%d fps=%d kbps=%d", s.res, s.fps, s.kbps);
	test_click(UI_NAV2);
	test_shot(dir, L"pair_paired");
	test_click(UI_ACCOUNT_CLEAR);
	Sleep(300);
	test_shot(dir, L"pair_repair");
	test_click(UI_REPAIR);
	Sleep(300);
	test_shot(dir, L"pair_cancelled");
	if(GetEnvironmentVariableW(L"PSSTREAM_TEST_TRAY", NULL, 0))
	{
		PostMessageW(g.hwnd, WM_SIZE, SIZE_MINIMIZED, 0);
		Sleep(300);
		CHIAKI_LOGI(&g.log, "[tray] in_tray=%d", ui.in_tray);
		ui.in_tray = ui.in_tray && tray_icon(NIM_DELETE) && false;
	}
	PostMessageW(g.hwnd, WM_CLOSE, 0, 0);
	return 0;
}

// Plays an Annex B H.264 file (one AUD per access unit) and silence through the real video and audio paths.
static DWORD WINAPI demo_main(void *arg)
{
	FILE *f = _wfopen((const wchar_t *)arg, L"rb");
	if(!f)
		return 1;
	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t *data = calloc(1, size + CHIAKI_VIDEO_BUFFER_PADDING_SIZE);
	fread(data, 1, size, f);
	fclose(f);

	g.want_audio = 1;
	audio_settings_cb(2, 48000, NULL);
	int16_t silence[480 * 2] = { 0 };
	char fps_text[8] = "30";
	GetEnvironmentVariableA("PSSTREAM_TEST_FPS", fps_text, sizeof(fps_text));
	int fps = atoi(fps_text);
	video_start(CHIAKI_CODEC_H264, fps, settings_get().hw != 0);
	long start = 0;
	int count = 0;
	char shot[MAX_PATH];
	for(long i = 4; i <= size - 5 && !g.quit; i++)
	{
		if(i != size - 5 && !(data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 0 && data[i + 3] == 1 && (data[i + 4] & 0x1f) == 9))
			continue;
		chiaki_ffmpeg_decoder_video_sample_cb(data + start, i - start, 0, false, &g.decoder);
		audio_frame_cb(silence, 480, NULL);
		start = i;
		if(++count == 45 && GetEnvironmentVariableA("PSSTREAM_TEST_SHOT", shot, sizeof(shot)))
			g.test_video_shot = shot;
		Sleep(1000 / fps);
	}
	CHIAKI_LOGI(&g.log, "Demo finished, frames fed: %d, queued audio bytes: %u", count, SDL_GetQueuedAudioSize(g.audio_dev));
	Sleep(300);
	video_stop();
	free(data);
	PostMessageW(g.hwnd, WM_CLOSE, 0, 0);
	return 0;
}
#endif

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmd, int show)
{
	(void)prev;
	(void)cmd;
	InitializeCriticalSection(&g.lock);
	SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);

	int argc = 0;
	wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
	bool start_in_tray = argc == 2 && wcscmp(argv[1], L"--tray") == 0;
	g.relaunched = argc == 2 && wcscmp(argv[1], L"--updated") == 0;
	if(argc == 2 && wcscmp(argv[1], L"--update-failed") == 0)
	{
		g.update_failed = 1;
		g.update_note = L"Güncelleme uygulanamadı, eski sürüm çalışıyor";
		g.update_note_at = GetTickCount();
	}
	if(g.relaunched)
	{
		g.update_note = L"Güncelleme tamamlandı";
		g.update_note_at = GetTickCount();
	}
	// A second copy would fight the first one for the PS5 and the hotkeys, so show the first instead
	if(argc < 3)
	{
		HANDLE single = CreateMutexW(NULL, FALSE, INSTANCE_MUTEX);
		g.single_instance = single;
		if(single && GetLastError() == ERROR_ALREADY_EXISTS)
		{
			HWND first = FindWindowW(WINDOW_CLASS, NULL);
			if(first)
			{
				PostMessageW(first, WM_APP_TRAY, 0, WM_LBUTTONUP);
				SetForegroundWindow(first);
			}
			return 0;
		}
	}

	wchar_t dir[MAX_PATH], log_path[MAX_PATH], old_log[MAX_PATH];
	SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, dir);
	wcsncat(dir, L"\\PSStream", MAX_PATH - wcslen(dir) - 1);
	CreateDirectoryW(dir, NULL);
#ifdef PSSTREAM_TEST
	GetEnvironmentVariableW(L"PSSTREAM_TEST_DIR", dir, MAX_PATH);
#endif
	wcscpy(g.data_dir, dir);
	swprintf(g.config_path, MAX_PATH, L"%ls\\config.ini", dir);
	swprintf(log_path, MAX_PATH, L"%ls\\log.txt", dir);
	swprintf(old_log, MAX_PATH, L"%ls\\log.onceki.txt", dir);
	MoveFileExW(log_path, old_log, MOVEFILE_REPLACE_EXISTING);
	g.log_file = _wfopen(log_path, L"w");
	chiaki_log_init(&g.log, CHIAKI_LOG_ALL & ~CHIAKI_LOG_VERBOSE & ~CHIAKI_LOG_DEBUG, log_cb, NULL);

	chiaki_lib_init();
	curl_global_init(CURL_GLOBAL_DEFAULT);
	SDL_SetMainReady();
	SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
	SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON, "1");
	SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON_SMALL, "1");
	if(SDL_InitSubSystem(SDL_INIT_AUDIO) != 0)
		CHIAKI_LOGE(&g.log, "SDL audio failed: %s", SDL_GetError());
	config_load();
	char lang[8];
	config_get("lang", lang, sizeof(lang));
	lang_english = strcmp(lang, "en") == 0;

	g.dpi = GetDpiForSystem();
	g.wake = CreateEventW(NULL, FALSE, FALSE, NULL);
	g.video_ready = CreateEventW(NULL, TRUE, FALSE, NULL);
	wcscpy(g.status, g.paired ? L"Kapalı. Başlamak için Ses veya Görüntü'de Aç'a bas" : L"Başlamadan önce PS5'i bir kere eşleştirmen gerekiyor");

	ui_create(inst);
	show_page(g.paired ? PAGE_PLAY : PAGE_PAIR);
	if(!hotkeys_register(&g.settings))
		wcscpy(g.status, L"Bir kısayol başka programda kullanılıyor, Ayarlar'dan değiştir");
	ui_frame();
	if(!g.host[0])
		find_ps5();
	if(g.settings.update_check)
		CloseHandle(CreateThread(NULL, 0, update_check_main, NULL, 0, NULL));
	CloseHandle(CreateThread(NULL, 0, update_cleanup_main, NULL, 0, NULL));

	LPTHREAD_START_ROUTINE thread_main = worker_main;
	void *thread_arg = NULL;
	HANDLE test_thread = NULL;
#ifdef PSSTREAM_TEST
	if(argc == 3 && wcscmp(argv[1], L"--demo") == 0)
	{
		g.test_hidden = true;
		thread_main = demo_main;
		thread_arg = argv[2];
	}
	else if(argc == 3 && wcscmp(argv[1], L"--ui-test") == 0)
	{
		g.test_hidden = true;
		test_thread = CreateThread(NULL, 0, ui_test_main, argv[2], 0, NULL);
	}
#endif
	(void)test_thread;
	HANDLE worker = CreateThread(NULL, 0, thread_main, thread_arg, 0, NULL);
	bool autostart = !g.test_hidden;
#ifdef PSSTREAM_TEST
	autostart = autostart || GetEnvironmentVariableW(L"PSSTREAM_TEST_AUTOSTART", NULL, 0) != 0;
#endif
	if(autostart && g.paired && g.host[0] && g.settings.autostart_audio)
	{
		g.want_audio = 1;
		SetEvent(g.wake);
	}

	if(!g.test_hidden)
	{
		if(start_in_tray)
			tray_enter();
		else
			ShowWindow(g.hwnd, show);
	}
	// The window is up, so the version that started this one can go
	if(g.relaunched)
	{
		HANDLE updated = OpenEventW(EVENT_MODIFY_STATE, FALSE, L"PSStreamUpdated");
		if(updated)
		{
			SetEvent(updated);
			CloseHandle(updated);
		}
		SetForegroundWindow(g.hwnd);
	}
	MSG msg;
	while(GetMessageW(&msg, NULL, 0, 0) > 0)
	{
		// While assigning a hotkey, keys like Esc and Tab must reach the window instead of the dialog logic
		if(!ui.capturing && IsDialogMessageW(g.hwnd, &msg))
			continue;
		TranslateMessage(&msg);
		DispatchMessageW(&msg);
	}

	g.quit = 1;
	SetEvent(g.wake);
	// Stopping tells the PS5 we left. If that hangs, leave anyway instead of tearing SDL down under it
	if(WaitForSingleObject(worker, 10000) != WAIT_OBJECT_0)
	{
		if(g.update_restart)
			update_finish();
		ExitProcess(0);
	}
	SDL_Quit();
	curl_global_cleanup();
	if(g.update_restart)
		update_finish();
	if(g.log_file)
		fclose(g.log_file);
	return 0;
}
