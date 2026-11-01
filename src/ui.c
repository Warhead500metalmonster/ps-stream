// Custom-painted interface, included from main.c. Immediate mode: ui_frame() draws the current
// page and handles input in one pass, and runs again after every mouse or status change.

#include <objidl.h>
#include <gdiplus.h>
#include <dwmapi.h>

#include "i18n.c"

#define UI_W 920
#define UI_H 590

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
#define ICON_PLAY L""

enum { UI_NONE, UI_NAV0, UI_NAV1, UI_NAV2, UI_AUDIO, UI_VIDEO, UI_GEAR_A, UI_GEAR_V, UI_APPLY, UI_FIND, UI_LOGIN, UI_ACCOUNT_CLEAR, UI_PAIR,
	UI_DEVICE, UI_HK_AUDIO, UI_HK_VIDEO, UI_HW, UI_RECONNECT, UI_CONTROLLER, UI_WIFI, UI_BITRATE, UI_AO_BITRATE, UI_VOLUME, UI_BUFFER, UI_LOSS,
	UI_PRESET0 = 40, UI_RES0 = 50, UI_FPS0 = 60, UI_CODEC0 = 65, UI_AORES0 = 70, UI_LANG0 = 80, UI_MAX = 96 };

static struct {
	GpGraphics *gx;
	GpFont *small, *body, *semi, *title, *big, *icon, *icon_big;
	GpStringFormat *fmt;
	HDC back_dc, bg_dc;
	HBITMAP back_bmp;
	HWND edits[3];
	HBRUSH field_brush;

	int page;
	Settings draft;
	float mx, my;
	bool press, release;
	int active, released_id, capturing;
	bool again, open_device_menu, in_frame, logging_in;
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

static void background_paint(GpGraphics *gx)
{
	float w = F(UI_W), h = F(UI_H);
	GpPointF a = { 0, 0 }, b = { 0, h };
	GpLineGradient *grad;
	GdipCreateLineBrush(&a, &b, COLOR(255, 46, 47, 52), COLOR(255, 22, 23, 27), WrapModeTile, &grad);
	GdipFillRectangle(gx, (GpBrush *)grad, 0, 0, w, h);
	GdipDeleteBrush((GpBrush *)grad);
	wave(gx, 0, 0, w, h, 0.42f, 0.18f, 0.72f, 0.38f, WHITE(9));
	wave(gx, 0, 0, w, h, 0.62f, 0.86f, 0.30f, 0.58f, WHITE(10));
	wave(gx, 0, 0, w, h, 0.84f, 0.62f, 0.98f, 0.70f, WHITE(12));
}

// ---------- widgets ----------

typedef struct { bool hot, click, held; } Hit;

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
		bg = b.hot ? WHITE(225) : WHITE(255);
		fg = C_DARK;
	}
	else
	{
		bg = b.hot ? WHITE(55) : WHITE(32);
		fg = C_TEXT;
	}
	fill_round(x, y, w, h, h / 2, bg);
	text(label, ui.semi, x, y, w, h, fg, 1, 1);
	return enabled && b.click;
}

static bool chip(int id, const wchar_t *label, float x, float y, float w, float h, bool selected)
{
	Hit b = hit(id, x, y, w, h);
	fill_round(x, y, w, h, 10, selected ? C_ACCENT : b.hot ? WHITE(48) : WHITE(24));
	text(label, ui.semi, x, y, w, h, selected ? C_TEXT : WHITE(215), 1, 1);
	return b.click;
}

static bool switch_toggle(int id, float x, float y, bool on)
{
	Hit b = hit(id, x, y, 46, 26);
	fill_round(x, y, 46, 26, 13, on ? C_ACCENT : b.hot ? WHITE(70) : WHITE(45));
	dot(on ? x + 23 : x + 3, y + 3, 20, WHITE(255));
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
	dot(x + w * t - 8, y + 6, 16, b.held || b.hot ? WHITE(255) : WHITE(230));
	return *value != old;
}

static void panel(float x, float y, float w, float h)
{
	fill_round(x, y, w, h, 22, WHITE(16));
	stroke_round(x, y, w, h, 22, 1, WHITE(18));
}

static void heading(const wchar_t *s, float x, float y)
{
	text(s, ui.semi, x, y, 300, 22, C_MUTED, 0, 1);
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
	bool ok = true;
	if(LOBYTE(s->hotkey_audio))
		ok = RegisterHotKey(g.hwnd, HOTKEY_AUDIO, hotkey_mods(s->hotkey_audio), LOBYTE(s->hotkey_audio)) && ok;
	if(LOBYTE(s->hotkey_video))
		ok = RegisterHotKey(g.hwnd, HOTKEY_VIDEO, hotkey_mods(s->hotkey_video), LOBYTE(s->hotkey_video)) && ok;
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
	wchar_t key[16];
	if(vk >= VK_F1 && vk <= VK_F24)
		swprintf(key, 16, L"F%d", vk - VK_F1 + 1);
	else
		swprintf(key, 16, L"%lc", (wchar_t)vk);
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

static void toggle(int which)
{
	if(!g.paired)
	{
		show_page(PAGE_PAIR);
		set_status(L"Önce PS5'i eşleştir");
		return;
	}
	get_text(ID_HOST, g.host, sizeof(g.host));
	if(!g.host[0])
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

static void card(int id, int gear_id, float x, float y, float w, float h, ARGB c1, ARGB c2, const wchar_t *icon,
		const wchar_t *title, const wchar_t *subtitle, const wchar_t *mode, const wchar_t *detail, bool on)
{
	float dx = F(x), dy = F(y), dw = F(w), dh = F(h);
	GpPath *p = path_round(dx, dy, dw, dh, F(30));
	GpPointF a = { dx, dy }, b = { dx + dw, dy + dh };
	GpLineGradient *grad;
	GdipCreateLineBrush(&a, &b, c1, c2, WrapModeTile, &grad);
	GdipFillPath(ui.gx, (GpBrush *)grad, p);
	GdipDeleteBrush((GpBrush *)grad);

	GdipSetClipPath(ui.gx, p, CombineModeReplace);
	wave(ui.gx, dx, dy, dw, dh, 0.38f, 0.12f, 0.70f, 0.30f, WHITE(20));
	wave(ui.gx, dx, dy, dw, dh, 0.60f, 0.84f, 0.32f, 0.52f, WHITE(22));
	if(!on)
	{
		GpSolidFill *dim;
		GdipCreateSolidFill(COLOR(120, 18, 19, 23), &dim);
		GdipFillPath(ui.gx, (GpBrush *)dim, p);
		GdipDeleteBrush((GpBrush *)dim);
	}
	GdipResetClip(ui.gx);
	GdipDeletePath(p);
	if(on)
		stroke_round(x + 1, y + 1, w - 2, h - 2, 29, 2, C_GREEN);

	text(title, ui.big, x + 26, y + 20, w - 90, 40, C_TEXT, 0, 1);
	text(subtitle, ui.body, x + 26, y + 60, w - 90, 44, WHITE(215), 0, 0);

	Hit gear = hit(gear_id, x + w - 58, y + 18, 40, 40);
	dot(x + w - 58, y + 18, 40, gear.hot ? WHITE(70) : WHITE(38));
	text(ICON_GEAR, ui.icon, x + w - 58, y + 18, 40, 40, C_TEXT, 1, 1);
	if(gear.click)
		show_page(PAGE_SETTINGS);

	float py = y + h - 112, ph = 98;
	fill_round(x + 14, py, w - 28, ph, 22, COLOR(120, 30, 32, 38));
	stroke_round(x + 14, py, w - 28, ph, 22, 1, WHITE(30));
	text(icon, ui.icon_big, x + 26, py, 48, ph, C_TEXT, 1, 1);
	text(mode, ui.title, x + 82, py + 10, w - 220, 28, C_TEXT, 0, 1);

	bool connecting = on && !g.connected;
	dot(x + 83, py + 45, 9, !on ? WHITE(120) : connecting ? C_AMBER : C_GREEN);
	text(!on ? L"Kapalı" : connecting ? L"Bağlanıyor" : L"Açık", ui.body, x + 98, py + 38, 120, 22, !on ? C_MUTED : connecting ? C_AMBER : C_GREEN, 0, 1);
	text(detail, ui.small, x + 82, py + 62, w - 210, 22, WHITE(190), 0, 1);

	if(pill_button(id, on ? L"Kapat" : L"Aç", x + w - 122, py + 28, 92, 42, true, true))
		toggle(id);
}

static void page_play(void)
{
	Settings s = settings_get();
	wchar_t detail[128], rate[32];

	float cw = (UI_W - 48 - 16) / 2.0f, cy = 78, ch = 330;
	bitrate_text(rate, 32, s.ao_kbps);
	swprintf(detail, 128, tr(L"PS5'ten istenen: %dp, %ls"), resolutions[s.ao_res].height, rate);
	card(UI_AUDIO, UI_GEAR_A, 24, cy, cw, ch, COLOR(255, 196, 124, 44), COLOR(255, 92, 52, 28), ICON_AUDIO,
			L"Ses", L"Oyun sesini duy, görüntü için\nişlemci ve veri harcama.", L"Oyun sesi", detail, g.want_audio != 0);

	bitrate_text(rate, 32, s.kbps);
	swprintf(detail, 128, L"%dp, %d FPS, %ls, %ls", resolutions[s.res].height, s.fps, rate, s.h265 ? L"H.265" : L"H.264");
	card(UI_VIDEO, UI_GEAR_V, 24 + cw + 16, cy, cw, ch, COLOR(255, 40, 92, 214), COLOR(255, 18, 38, 120), ICON_VIDEO,
			L"Görüntü", L"Discord'da yayın açmak için.\n\"PS Stream - Goruntu\" penceresini seç.", L"Oyun görüntüsü", detail, g.want_video != 0);

	static const wchar_t *const names[] = { L"En düşük", L"Düşük", L"Dengeli", L"Yüksek", L"En yüksek" };
	static const wchar_t *const subs[] = { L"360p, 0,5 Mbps", L"360p, 2 Mbps", L"720p, 6 Mbps", L"1080p 60, 20 Mbps", L"1080p 60, 50 Mbps" };
	int current = preset_match(&s);
	float gap = 10, w = (UI_W - 48 - gap * 5) / 6, y = 424, h = 66;
	for(int i = 0; i <= PRESET_COUNT; i++)
	{
		float x = 24 + i * (w + gap);
		bool selected = i == current;
		Hit b = hit(UI_PRESET0 + i, x, y, w, h);
		fill_round(x, y, w, h, 16, selected ? C_ACCENT : b.hot ? WHITE(44) : WHITE(22));
		text(i < PRESET_COUNT ? names[i] : L"Özel", ui.semi, x, y + 10, w, 24, C_TEXT, 1, 1);
		text(i < PRESET_COUNT ? subs[i] : L"Ayarlardan seç", ui.small, x, y + 34, w, 22, selected ? WHITE(225) : C_MUTED, 1, 1);
		if(mouse_in(x, y, w, h))
			ui.tip = L"Görüntü kalitesi. Sadece ses modunu etkilemez.";
		if(b.click)
		{
			if(i == PRESET_COUNT)
				show_page(PAGE_SETTINGS);
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
}

static void chips_row(int id0, const wchar_t *const *labels, int count, int *value, float x, float y, float w)
{
	float gap = 6, cw = (w - gap * (count - 1)) / count;
	for(int i = 0; i < count; i++)
		if(chip(id0 + i, labels[i], x + i * (cw + gap), y, cw, 30, *value == i))
			*value = i;
}

static void page_settings(void)
{
	Settings *d = &ui.draft;
	wchar_t t[64];
	float lx = 24, rx = 468, pw = 428, py = 78, ph = 434;
	panel(lx, py, pw, ph);
	panel(rx, py, pw, ph);

	static const wchar_t *const res_names[] = { L"144p", L"360p", L"540p", L"720p", L"1080p" };
	static const wchar_t *const fps_names[] = { L"30 FPS", L"60 FPS" };
	static const wchar_t *const codec_names[] = { L"H.264", L"H.265" };

	float x = lx + 20, cx = lx + 150, cw = pw - 170, y = py + 14;
	heading(L"GÖRÜNTÜ MODU", x, y);
	y += 30;
	row_label(L"Çözünürlük", L"144p deneyseldir, PS5 kabul etmezse otomatik olarak 360p'ye döner.", x, y, pw - 40);
	chips_row(UI_RES0, res_names, RES_COUNT, &d->res, cx, y, cw);
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
	row_label(L"Çözünürlük", L"PS5 görüntüyü yine gönderir, bu yüzden en küçüğü istenir. Görüntü çözülmez.", x, y, pw - 40);
	chips_row(UI_AORES0, res_names, 2, &d->ao_res, cx, y, cw);
	y += 42;
	row_label(L"İstenen veri", L"Wi-Fi'ın kötüyse en düşükte bırak. PS5 kabul etmezse 2 Mbps'ye döner.", x, y, pw - 40);
	steps = d->ao_kbps / 100;
	slider(UI_AO_BITRATE, cx + 8, y, cw - 94, &steps, 1, 20);
	d->ao_kbps = steps * 100;
	bitrate_text(t, 64, d->ao_kbps);
	text(t, ui.body, cx + cw - 80, y, 80, 30, C_TEXT, 2, 1);
	y += 38;
	heading(L"WI-FI", x, y);
	y += 24;
	row_label(L"Düşük gecikme", L"Bağlıyken Windows'un Wi-Fi taramalarını durdurur, bilgisayarı uyanık tutar. Bağlantı kapanınca eski hâline döner.", x, y, pw - 40);
	if(switch_toggle(UI_WIFI, cx, y + 2, d->wifi_opt != 0))
		d->wifi_opt = !d->wifi_opt;

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
	Hit dev = hit(UI_DEVICE, cx, y, cw, 30);
	fill_round(cx, y, cw, 30, 10, dev.hot ? WHITE(48) : WHITE(24));
	text(device_w, ui.body, cx + 12, y, cw - 24, 30, C_TEXT, 0, 1);
	if(dev.click)
		ui.open_device_menu = true;
	y += 42;
	row_label(L"Ses seviyesi", L"Hemen uygulanır. %100 üstü sesi yükseltir.", x, y, pw - 40);
	if(slider(UI_VOLUME, cx + 8, y, cw - 94, &d->volume, 0, 200))
	{
		EnterCriticalSection(&g.lock);
		g.settings.volume = d->volume;
		LeaveCriticalSection(&g.lock);
		config_set_int("volume", d->volume);
	}
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
	row_label(L"Yeniden bağlan", L"Bağlantı koparsa 5 saniyede bir tekrar dener.", x, y, pw - 40);
	if(switch_toggle(UI_RECONNECT, cx, y + 2, d->reconnect != 0))
		d->reconnect = !d->reconnect;
	y += 36;
	row_label(L"DualSense PS5'te", L"Açıkken PS5'e sanal kumanda bildirilmez, DualSense'in PS5'e bağlı kalması için. Bağlantı sorun çıkarırsa kapat.", x, y, pw - 40);
	if(switch_toggle(UI_CONTROLLER, cx, y + 2, d->no_controller != 0))
		d->no_controller = !d->no_controller;
	y += 40;
	heading(L"KISAYOLLAR", x, y);
	y += 28;
	for(int i = 0; i < 2; i++)
	{
		int id = i ? UI_HK_VIDEO : UI_HK_AUDIO;
		row_label(i ? L"Görüntü" : L"Ses", L"Tıkla, sonra tuş kombinasyonuna bas. Esc iptal eder, Delete kaldırır.", x, y, pw - 40);
		hotkey_name(i ? d->hotkey_video : d->hotkey_audio, t, 64);
		if(chip(id, ui.capturing == id ? L"Tuşa bas..." : t, cx, y, cw, 30, ui.capturing == id))
		{
			ui.capturing = id;
			SetFocus(g.hwnd);
		}
		y += 36;
	}
}

static void page_pair(void)
{
	float w = (UI_W - 48 - 32) / 3.0f, y = 78, h = 434;
	static const wchar_t *const titles[] = { L"PS5'i bul", L"Hesabını bağla", L"Kodu gir" };
	static const wchar_t *const bodies[] = {
		L"PS5 açık ve bu bilgisayarla aynı ağda olmalı. Uygulama kendisi arar. Bulamazsa Otomatik bul'a bas veya PS5'teki IP adresini yaz.\n\nPS5'te: Ayarlar > Sistem > Sistem Yazılımı > Konsol Bilgisi",
		L"PSN ile giriş yap'a bas. Sony'nin sayfası tarayıcında açılır. Giriş yapınca hesabın kendiliğinden bağlanır, sonra sekmeyi kapatabilirsin.\n\nŞifren sadece Sony'ye gider. Olmazsa adres çubuğundaki adresi kopyala.\n\nBaşka hesap için Sil'e bas, yeniden giriş yap ve eşleştir.",
		L"PS5'te: Ayarlar > Sistem > Uzaktan Oynatma > Cihazı Bağla\n\nEkranda çıkan 8 haneli kodu yaz. Eşleştirme kendiliğinden başlar. Bunu bir kere yapman yeterli."
	};
	static const wchar_t *const nums[] = { L"1", L"2", L"3" };
	for(int i = 0; i < 3; i++)
	{
		float x = 24 + i * (w + 16);
		panel(x, y, w, h);
		dot(x + 20, y + 20, 36, C_ACCENT);
		text(nums[i], ui.title, x + 20, y + 20, 36, 36, C_TEXT, 1, 1);
		text(titles[i], ui.title, x + 66, y + 20, w - 80, 36, C_TEXT, 0, 1);
		text(bodies[i], ui.body, x + 20, y + 74, w - 40, 210, WHITE(205), 0, 0);
		fill_round(x + 20, y + 300, w - 40, 44, 14, C_FIELD);
	}
	float x0 = 24, x1 = 24 + w + 16, x2 = 24 + 2 * (w + 16), by = y + 360, bw = w - 40;
	if(pill_button(UI_FIND, finding ? L"Aranıyor..." : L"Otomatik bul", x0 + 20, by, bw, 46, false, !finding))
		find_ps5();
	if(pill_button(UI_LOGIN, ui.logging_in ? L"Giriş bekleniyor (iptal)" : L"PSN ile giriş yap", x1 + 20, by, bw - 58, 46, false, true))
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
		set_status(L"Hesap kimliği silindi. Yeni hesapla giriş yapıp eşleştirebilirsin");
	}
	if(mouse_in(x1 + 20 + bw - 50, by, 50, 46))
		ui.tip = L"Kayıtlı hesap kimliğini siler. Eski eşleştirme, yenisini yapana kadar çalışır.";
	if(pill_button(UI_PAIR, g.pairing ? L"Eşleştiriliyor..." : L"Eşleştir", x2 + 20, by, bw, 46, true, !g.pairing))
		pair_start();
}

static void top_bar(void)
{
	dot(24, 18, 44, COLOR(255, 240, 160, 40));
	text(ICON_PLAY, ui.icon, 24, 18, 44, 44, C_DARK, 1, 1);

	static const wchar_t *const names[] = { L"Oynat", L"Ayarlar", L"Eşleştirme" };
	float x = 82, y = 20, h = 40, w = 108;
	fill_round(x, y, w * 3 + 8, h, h / 2, WHITE(22));
	for(int i = 0; i < 3; i++)
	{
		float bx = x + 4 + i * w;
		Hit b = hit(UI_NAV0 + i, bx, y + 4, w, h - 8);
		if(ui.page == i)
			fill_round(bx, y + 4, w, h - 8, (h - 8) / 2, C_ACCENT);
		text(names[i], ui.semi, bx, y + 4, w, h - 8, ui.page == i || b.hot ? C_TEXT : C_MUTED, 1, 1);
		if(b.click)
			show_page(i);
	}

	if(chip(UI_LANG0, L"TR", 590, y + 4, 42, h - 8, !lang_english) && lang_english)
	{
		lang_english = false;
		config_set("lang", "tr");
	}
	if(chip(UI_LANG0 + 1, L"EN", 636, y + 4, 42, h - 8, lang_english) && !lang_english)
	{
		lang_english = true;
		config_set("lang", "en");
	}

	wchar_t host[300];
	if(g.paired)
		swprintf(host, 300, L"PS5  %hs", g.host);
	else
		swprintf(host, 300, L"PS5 eşleşmedi");
	bool live = (g.want_audio || g.want_video) && g.connected;
	float pw = 210, px = UI_W - 24 - pw;
	fill_round(px, y, pw, h, h / 2, WHITE(22));
	dot(px + 16, y + 15, 10, live ? C_GREEN : g.paired ? WHITE(120) : C_AMBER);
	text(host, ui.body, px + 34, y, pw - 44, h, C_TEXT, 0, 1);
}

static void status_bar(void)
{
	float x = 24, y = UI_H - 64, w = UI_W - 48, h = 48;
	fill_round(x, y, w, h, 16, COLOR(170, 44, 46, 52));
	text(ICON_BULB, ui.icon, x + 8, y, 36, h, C_AMBER, 1, 1);

	wchar_t status[256];
	EnterCriticalSection(&g.lock);
	wcscpy(status, g.status);
	LeaveCriticalSection(&g.lock);

	Settings live = settings_get();
	bool dirty = ui.page == PAGE_SETTINGS && (ui.draft.res != live.res || ui.draft.fps != live.fps || ui.draft.kbps != live.kbps
		|| ui.draft.h265 != live.h265 || ui.draft.hw != live.hw || ui.draft.ao_res != live.ao_res || ui.draft.ao_kbps != live.ao_kbps
		|| ui.draft.loss_pct != live.loss_pct || ui.draft.no_controller != live.no_controller || ui.draft.wifi_opt != live.wifi_opt || ui.draft.reconnect != live.reconnect || ui.draft.hotkey_audio != live.hotkey_audio
		|| ui.draft.hotkey_video != live.hotkey_video || strcmp(ui.draft.device, live.device) != 0);

	float tw = w - 56;
	if(dirty)
	{
		tw -= 130;
		if(pill_button(UI_APPLY, L"Uygula", x + w - 124, y + 7, 114, 34, true, true))
		{
			settings_commit(&ui.draft);
			set_status(hotkeys_register(&ui.draft) ? L"Ayarlar uygulandı" : L"Ayarlar uygulandı ama bir kısayol başka programda kullanılıyor");
		}
	}
	else if(ui.page == PAGE_PLAY)
	{
		wchar_t a[32], v[32], keys[80];
		hotkey_name(live.hotkey_audio, a, 32);
		hotkey_name(live.hotkey_video, v, 32);
		swprintf(keys, 80, tr(L"Ses %ls   Görüntü %ls"), a, v);
		tw -= 280;
		text(keys, ui.small, x + w - 290, y, 278, h, C_MUTED, 2, 1);
	}
	text(ui.tip ? ui.tip : status, ui.body, x + 46, y, tw, h, C_TEXT, 0, 1);
}

static void ui_frame(void)
{
	if(ui.in_frame)
		return;
	ui.in_frame = true;
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
	ui.in_frame = false;
	InvalidateRect(g.hwnd, NULL, FALSE);

	if(ui.open_device_menu)
	{
		ui.open_device_menu = false;
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
	int *target = ui.capturing == UI_HK_AUDIO ? &ui.draft.hotkey_audio : &ui.draft.hotkey_video;
	if(vk == VK_DELETE)
		*target = 0;
	else if(vk != VK_ESCAPE)
	{
		int f = (GetKeyState(VK_CONTROL) < 0 ? HOTKEYF_CONTROL : 0) | (GetKeyState(VK_MENU) < 0 ? HOTKEYF_ALT : 0) | (GetKeyState(VK_SHIFT) < 0 ? HOTKEYF_SHIFT : 0);
		*target = MAKEWORD(vk, f);
	}
	ui.capturing = 0;
	ui_frame();
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	switch(msg)
	{
		case WM_COMMAND:
			if(LOWORD(wp) == ID_PIN && HIWORD(wp) == EN_CHANGE && GetWindowTextLengthW(ui.edits[2]) == 8
				&& GetWindowTextLengthW(ui.edits[1]) > 0 && !g.pairing)
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
			if(hot != ui.hot_id || ui.active || ui.page != PAGE_PAIR)
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
			ui_frame();
			return 0;
		case WM_APP_LOGIN:
			ui.logging_in = false;
			if(wp == 1)
			{
				SetDlgItemTextA(hwnd, ID_ACCOUNT, login_account_id);
				SetFocus(ui.edits[2]);
				set_status(L"Hesap bağlandı. Şimdi PS5'teki 8 haneli kodu yaz");
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
			PostQuitMessage(0);
			return 0;
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

static void ui_create(HINSTANCE inst)
{
	ULONG_PTR gdiplus_token;
	GdiplusStartupInput startup = { 1, NULL, FALSE, FALSE };
	GdiplusStartup(&gdiplus_token, &startup, NULL);

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
	wc.lpszClassName = L"PSStreamWindow";
	RegisterClassW(&wc);

	DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
	RECT rect = { 0, 0, S(UI_W), S(UI_H) };
	AdjustWindowRect(&rect, style, FALSE);
	g.hwnd = CreateWindowW(wc.lpszClassName, L"PS Stream", style, CW_USEDEFAULT, CW_USEDEFAULT,
			rect.right - rect.left, rect.bottom - rect.top, NULL, NULL, inst, NULL);
	BOOL dark = TRUE;
	DwmSetWindowAttribute(g.hwnd, 20, &dark, sizeof(dark));

	HDC screen = GetDC(g.hwnd);
	ui.back_dc = CreateCompatibleDC(screen);
	ui.back_bmp = CreateCompatibleBitmap(screen, S(UI_W), S(UI_H));
	SelectObject(ui.back_dc, ui.back_bmp);
	ui.bg_dc = CreateCompatibleDC(screen);
	SelectObject(ui.bg_dc, CreateCompatibleBitmap(screen, S(UI_W), S(UI_H)));
	ReleaseDC(g.hwnd, screen);

	GpGraphics *bg;
	GdipCreateFromHDC(ui.bg_dc, &bg);
	GdipSetSmoothingMode(bg, SmoothingModeAntiAlias);
	background_paint(bg);
	GdipDeleteGraphics(bg);

	GdipCreateFromHDC(ui.back_dc, &ui.gx);
	GdipSetSmoothingMode(ui.gx, SmoothingModeAntiAlias);
	GdipSetTextRenderingHint(ui.gx, TextRenderingHintAntiAliasGridFit);

	wchar_t host_w[256];
	MultiByteToWideChar(CP_UTF8, 0, g.host, -1, host_w, 256);
	float w = (UI_W - 48 - 32) / 3.0f, field_y = 78 + 310;
	ui.edits[0] = add_edit(ID_HOST, host_w, 0, 24 + 32, field_y, w - 64);
	wchar_t account_w[128] = L"";
	char account[128];
	config_get("account", account, sizeof(account));
	MultiByteToWideChar(CP_UTF8, 0, account, -1, account_w, 128);
	ui.edits[1] = add_edit(ID_ACCOUNT, account_w, 0, 24 + w + 16 + 32, field_y, w - 64);
	ui.edits[2] = add_edit(ID_PIN, L"", ES_NUMBER, 24 + 2 * (w + 16) + 32, field_y, w - 64);

	ui.device_count = SDL_GetNumAudioDevices(0);
	if(ui.device_count > 0)
	{
		ui.devices = calloc(ui.device_count, sizeof(char *));
		for(int i = 0; i < ui.device_count; i++)
		{
			const char *name = SDL_GetAudioDeviceName(i, 0);
			ui.devices[i] = strdup(name ? name : "");
		}
	}
	ui.draft = g.settings;
	ui.mx = ui.my = -1;
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
		for(int i = 0; i < 60 && finding; i++)
			Sleep(200);
		test_log_status("search finished");
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
	test_click(UI_VIDEO);
	Sleep(2600);
	test_log_status("video on");
	test_shot(dir, L"play_both");
	test_click(UI_PRESET0 + 3);
	Sleep(2600);
	test_log_status("preset high");
	test_click(UI_VIDEO);
	Sleep(2600);
	test_log_status("video off");
	test_click(UI_AUDIO);
	Sleep(800);
	test_log_status("audio off");

	test_click(UI_NAV1);
	test_shot(dir, L"settings");
	test_click(UI_RES0 + 4);
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
	test_click(UI_APPLY);
	Settings s = settings_get();
	CHIAKI_LOGI(&g.log, "[applied] res=%d fps=%d kbps=%d", s.res, s.fps, s.kbps);
	test_click(UI_NAV2);
	test_shot(dir, L"pair_paired");
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

	char dir[MAX_PATH], log_path[MAX_PATH];
	SHGetFolderPathA(NULL, CSIDL_APPDATA, NULL, 0, dir);
	strncat(dir, "\\PSStream", sizeof(dir) - strlen(dir) - 1);
	CreateDirectoryA(dir, NULL);
#ifdef PSSTREAM_TEST
	GetEnvironmentVariableA("PSSTREAM_TEST_DIR", dir, sizeof(dir));
#endif
	snprintf(g.config_path, sizeof(g.config_path), "%.240s\\config.ini", dir);
	snprintf(log_path, sizeof(log_path), "%.240s\\log.txt", dir);
	g.log_file = fopen(log_path, "w");
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

	LPTHREAD_START_ROUTINE thread_main = worker_main;
	void *thread_arg = NULL;
	HANDLE test_thread = NULL;
#ifdef PSSTREAM_TEST
	int argc;
	wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
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

	if(!g.test_hidden)
		ShowWindow(g.hwnd, show);
	MSG msg;
	while(GetMessageW(&msg, NULL, 0, 0) > 0)
	{
		if(IsDialogMessageW(g.hwnd, &msg))
			continue;
		TranslateMessage(&msg);
		DispatchMessageW(&msg);
	}

	g.quit = 1;
	SetEvent(g.wake);
	WaitForSingleObject(worker, 10000);
	SDL_Quit();
	curl_global_cleanup();
	if(g.log_file)
		fclose(g.log_file);
	return 0;
}
