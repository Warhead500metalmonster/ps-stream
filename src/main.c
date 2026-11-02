// PS Stream: PS5 Remote Play client with independently switchable audio and video.
// Linked against chiaki-lib, so the combined work is AGPL-3.0 (see chiaki-ng's COPYING).

#define SDL_MAIN_HANDLED
#ifndef UNICODE
#define UNICODE
#endif
#define COBJMACROS
#define _UNICODE
#undef WINVER
#undef _WIN32_WINNT
#define WINVER 0x0A00
#define _WIN32_WINNT 0x0A00

#include <chiaki/base64.h>
#include <chiaki/common.h>
#include <chiaki/discovery.h>
#include <chiaki/ffmpegdecoder.h>
#include <chiaki/log.h>
#include <chiaki/opusdecoder.h>
#include <chiaki/regist.h>
#include <chiaki/session.h>

#include <SDL.h>
#include <curl/curl.h>
#include <json-c/json.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>

#include <windows.h>
#include <bcrypt.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <iphlpapi.h>
#include <wlanapi.h>
#define Off UiaOff // uiautomationcore.h clashes with an enum from the Wi-Fi headers
#include <uiautomation.h>
#undef Off

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Below this rate we are outside what chiaki-ng offers, so a console that refuses to start
// gets a second try with it.
#define APP_VERSION "0.1.8"
#define UPDATE_DOWNLOAD_PREFIX "https://github.com/Warhead500metalmonster/ps-stream/releases/download/"
#define RELEASES_URL L"https://github.com/Warhead500metalmonster/ps-stream/releases/latest"
#define REPO_URL L"https://github.com/Warhead500metalmonster/ps-stream"
#define UPDATE_API_URL "https://api.github.com/repos/Warhead500metalmonster/ps-stream/releases/latest"
#define LOG_MAX_BYTES (5 * 1024 * 1024)
#define RETRY_MAX_MS 15000
#define RETRY_FAST_MS 2000
#define RETRY_FAST_WINDOW_MS 60000
#define PROBE_DELAY_MS 200
#define PROBE_SLOW_DELAY_MS 5000
#define PROBE_SLOW_AFTER_MS 300000
#define PROBES_PER_SWEEP 15
#define PROBES_PER_SESSION_TRY 20
#define WAKE_REPEAT_MS 30000
#define WAKE_MAX_TRIES 3
#define REGIST_TRIES 6
#define REGIST_RETRY_MS 1000
#define TIMER_REGIST_RETRY 3
#define STATS_LOG_EVERY 15
#define SAFE_KBPS 2000
#define RES_MIN 1
#define RETRY_MS 5000
// Audio waits for this much backlog before playing; each dropout raises it, quiet minutes lower it
#define AUDIO_TARGET_MIN_MS 30
#define AUDIO_TARGET_MAX_MS 100
#define AUDIO_TARGET_STEP_MS 10
#define AUDIO_TARGET_DECAY_MS 60000
#define RECONNECT_MS 3000

#define PSN_CLIENT_ID "ba495a24-818c-472b-b12d-ff231c1b5745"
#define PSN_CLIENT_ID_W L"ba495a24-818c-472b-b12d-ff231c1b5745"
#define PSN_CLIENT_SECRET "mvaiZkRsAsI1IBkY"
#define PSN_TOKEN_URL "https://auth.api.sonyentertainmentnetwork.com/2.0/oauth/token"
#define PSN_REDIRECT_URL "https://remoteplay.dl.playstation.net/remoteplay/redirect"
#define PSN_LOGIN_URL L"https://auth.api.sonyentertainmentnetwork.com/2.0/oauth/authorize?service_entity=urn:service-entity:psn&response_type=code&client_id=" \
	PSN_CLIENT_ID_W L"&redirect_uri=https://remoteplay.dl.playstation.net/remoteplay/redirect&scope=psn:clientapp%20referenceDataService:countryConfig.read%20pushNotification:webSocket.desktop.connect%20sessionManager:remotePlaySession.system.update&request_locale=en_US&ui=pr&service_logo=ps&layout_type=popup&smcid=remoteplay&prompt=always&PlatformPrivacyWs1=minimal&"
#define PSN_SCOPE "psn:clientapp%20referenceDataService:countryConfig.read%20pushNotification:webSocket.desktop.connect%20sessionManager:remotePlaySession.system.update"
#define LOGIN_TIMEOUT_MS (10 * 60 * 1000)

enum {
	ID_HOST = 100, ID_ACCOUNT, ID_PIN,
	HOTKEY_AUDIO = 1, HOTKEY_VIDEO,
	WM_APP_STATUS = WM_APP + 1, WM_APP_VIDEO_CLOSED, WM_APP_PAIRED, WM_APP_ALL_OFF, WM_APP_TEST_SHOT, WM_APP_LOGIN, WM_APP_TRAY, WM_APP_HOST_CHANGED
};

enum { PAGE_PLAY, PAGE_SETTINGS, PAGE_PAIR };

typedef struct {
	const wchar_t *name;
	unsigned int width, height;
} Resolution;

static const Resolution resolutions[] = {
	{ L"144p", 256, 144 }, // kept so saved indexes stay valid; the PS5 refuses it ("AvCap failed to initialize")
	{ L"360p", 640, 360 },
	{ L"540p", 960, 540 },
	{ L"720p", 1280, 720 },
	{ L"1080p", 1920, 1080 },
};
#define RES_COUNT ((int)(sizeof(resolutions) / sizeof(resolutions[0])))

typedef struct {
	int res, fps, kbps, h265, hw;
	int ao_res, ao_kbps;
	int volume, buffer_ms, loss_pct, reconnect, no_controller, wifi_opt, autostart_audio, win_start, tray, update_check, auto_wake;
	int hotkey_audio, hotkey_video; // HKM_GETHOTKEY format
	char device[256]; // empty is the system default
} Settings;

typedef struct {
	const wchar_t *name;
	int res, fps, kbps, h265;
} Preset;

static const Preset presets[] = {
	{ L"En düşük - 360p 30 FPS, 0,5 Mbps", 1, 30, 500, 0 },
	{ L"Düşük - 360p 30 FPS, 2 Mbps", 1, 30, 2000, 0 },
	{ L"Dengeli - 720p 30 FPS, 6 Mbps", 3, 30, 6000, 0 },
	{ L"Yüksek - 1080p 60 FPS, 20 Mbps", 4, 60, 20000, 0 },
	{ L"En yüksek - 1080p 60 FPS, H.265, 50 Mbps", 4, 60, 50000, 1 },
};
#define PRESET_COUNT ((int)(sizeof(presets) / sizeof(presets[0])))
#define PRESET_DEFAULT 2

static struct {
	HWND hwnd;
	UINT dpi;
	wchar_t config_path[MAX_PATH], data_dir[MAX_PATH];
	FILE *log_file;
	ChiakiLog log;
	bool test_hidden;
	const char *test_video_shot;

	bool pairing;
	char host[256];
	char regist_key[CHIAKI_SESSION_AUTH_SIZE];
	uint8_t morning[0x10];
	bool paired;

	ChiakiRegist regist;
	ChiakiRegisteredHost registered;
	bool regist_ok;

	CRITICAL_SECTION lock; // guards settings and status
	Settings settings;
	wchar_t status[256];

	HANDLE wake;
	volatile LONG want_audio, want_video, quit;
	volatile LONG ended, connected, fatal, audio_reopen;
	ChiakiQuitReason quit_reason;

	ChiakiSession session;
	ChiakiOpusDecoder opus;
	SDL_AudioDeviceID audio_dev;
	uint32_t audio_channels, audio_rate;
	bool audio_waiting;
	int audio_target_ms;
	DWORD audio_last_dropout, connected_at;
	volatile LONG audio_dropouts, video_fps10, video_lost_total, ps5_standby;
	char update_tag[32];
	volatile LONG network_changed, regist_rejected;
	int regist_tries;
	volatile LONG update_ready, update_busy, update_failed, update_restart;
	char update_url[512], update_digest[72];
	bool relaunched;
	HANDLE single_instance;
	const wchar_t *update_note;
	DWORD update_note_at;
	HANDLE wlan;
	int16_t *volume_buf;
	int audio_fade_pos;
	size_t volume_buf_size;

	ChiakiFfmpegDecoder decoder;
	HANDLE video_thread, video_ready;
	volatile LONG video_stop, frame_pending;
} g;

static int S(int v) { return MulDiv(v, g.dpi, 96); }
static const wchar_t *tr(const wchar_t *s);

static Settings settings_get(void)
{
	EnterCriticalSection(&g.lock);
	Settings s = g.settings;
	LeaveCriticalSection(&g.lock);
	return s;
}

static void set_status(const wchar_t *text)
{
	EnterCriticalSection(&g.lock);
	wcsncpy(g.status, text, 255);
	LeaveCriticalSection(&g.lock);
	PostMessageW(g.hwnd, WM_APP_STATUS, 0, 0);
}

static void log_cb(ChiakiLogLevel level, const char *msg, void *user)
{
	// The library only reports that pairing failed. An answer from the PS5 means the code was
	// refused, and anything else means a packet got lost and trying again can work.
	if(strstr(msg, "Regist received HTTP code"))
		g.regist_rejected = 1;
	(void)level;
	(void)user;
	static long written;
	if(!g.log_file || written > LOG_MAX_BYTES)
		return;
	SYSTEMTIME t;
	GetLocalTime(&t);
	int n = fprintf(g.log_file, "%02d:%02d:%02d %s\n", t.wHour, t.wMinute, t.wSecond, msg);
	if(n > 0)
		written += n;
	fflush(g.log_file);
}

// ---------- audio ----------

static void audio_open(void)
{
	if(g.audio_dev)
		SDL_CloseAudioDevice(g.audio_dev);
	Settings s = settings_get();
	SDL_AudioSpec want = { 0 };
	want.freq = g.audio_rate;
	want.format = AUDIO_S16SYS;
	want.channels = g.audio_channels;
	want.samples = 512;
	g.audio_dev = SDL_OpenAudioDevice(s.device[0] ? s.device : NULL, 0, &want, NULL, 0);
	if(!g.audio_dev && s.device[0])
		g.audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, NULL, 0);
	if(!g.audio_dev)
	{
		CHIAKI_LOGE(&g.log, "Audio device failed: %s", SDL_GetError());
		return;
	}
	CHIAKI_LOGI(&g.log, "Audio device open: %u Hz, %u channels", g.audio_rate, g.audio_channels);
	g.audio_waiting = true;
	if(!g.audio_target_ms)
		g.audio_target_ms = AUDIO_TARGET_MIN_MS;
	g.audio_last_dropout = GetTickCount();
}

static void audio_settings_cb(uint32_t channels, uint32_t rate, void *user)
{
	(void)user;
	if(g.audio_dev && g.audio_channels == channels && g.audio_rate == rate)
		return;
	g.audio_channels = channels;
	g.audio_rate = rate;
	audio_open();
}

static void audio_frame_cb(int16_t *buf, size_t samples_count, void *user)
{
	(void)user;
	// A headset that was unplugged leaves the device stopped for good
	if((InterlockedExchange(&g.audio_reopen, 0) || (g.audio_dev && SDL_GetAudioDeviceStatus(g.audio_dev) == SDL_AUDIO_STOPPED)) && g.audio_rate)
		audio_open();
	if(!g.audio_dev)
		return;
	if(!g.want_audio)
	{
		// Muted, so going quiet here isn't a dropout, and turning audio off is immediate
		if(!g.audio_waiting)
		{
			SDL_PauseAudioDevice(g.audio_dev, 1);
			SDL_ClearQueuedAudio(g.audio_dev);
			g.audio_waiting = true;
		}
		return;
	}
	Settings s = settings_get();
	size_t count = samples_count * g.audio_channels;
	if(s.volume != 100)
	{
		if(count > g.volume_buf_size)
		{
			int16_t *grown = realloc(g.volume_buf, count * sizeof(int16_t));
			if(!grown)
				return;
			g.volume_buf = grown;
			g.volume_buf_size = count;
		}
		for(size_t i = 0; i < count; i++)
		{
			int v = buf[i] * s.volume / 100;
			g.volume_buf[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
		}
		buf = g.volume_buf;
	}
	Uint32 bytes_per_ms = g.audio_rate * g.audio_channels * sizeof(int16_t) / 1000;
	Uint32 queued = SDL_GetQueuedAudioSize(g.audio_dev);
	DWORD now = GetTickCount();
	int target_max = s.buffer_ms / 2 < AUDIO_TARGET_MAX_MS ? s.buffer_ms / 2 : AUDIO_TARGET_MAX_MS;
	if(queued > (Uint32)s.buffer_ms * bytes_per_ms)
	{
		// Wi-Fi bursts pile up here; dropping the backlog keeps sound in sync with the TV
		SDL_ClearQueuedAudio(g.audio_dev);
		SDL_PauseAudioDevice(g.audio_dev, 1);
		g.audio_waiting = true;
	}
	else if(!g.audio_waiting && queued == 0)
	{
		// Ran dry: refill before resuming, so jitter makes one short gap instead of constant crackle
		SDL_PauseAudioDevice(g.audio_dev, 1);
		g.audio_waiting = true;
		g.audio_last_dropout = now;
		InterlockedIncrement(&g.audio_dropouts);
		if(g.audio_target_ms < target_max)
		{
			g.audio_target_ms += AUDIO_TARGET_STEP_MS;
			CHIAKI_LOGI(&g.log, "Audio dropout, buffer now %d ms", g.audio_target_ms);
		}
	}
	else if(now - g.audio_last_dropout > AUDIO_TARGET_DECAY_MS && g.audio_target_ms > AUDIO_TARGET_MIN_MS)
	{
		g.audio_target_ms -= AUDIO_TARGET_STEP_MS;
		g.audio_last_dropout = now;
	}
	// Starting from silence on a loud sample makes a click, so ramp up over 10 ms
	int fade_frames = g.audio_rate / 100;
	if(SDL_GetQueuedAudioSize(g.audio_dev) == 0)
		g.audio_fade_pos = 0;
	if(g.audio_fade_pos < fade_frames)
	{
		if(buf != g.volume_buf)
		{
			if(count > g.volume_buf_size)
			{
				int16_t *grown = realloc(g.volume_buf, count * sizeof(int16_t));
				if(!grown)
					return;
				g.volume_buf = grown;
				g.volume_buf_size = count;
			}
			memcpy(g.volume_buf, buf, count * sizeof(int16_t));
			buf = g.volume_buf;
		}
		for(size_t frame = 0; frame < samples_count && g.audio_fade_pos < fade_frames; frame++, g.audio_fade_pos++)
			for(int c = 0; c < g.audio_channels; c++)
				buf[frame * g.audio_channels + c] = (int16_t)(buf[frame * g.audio_channels + c] * g.audio_fade_pos / fade_frames);
	}
	SDL_QueueAudio(g.audio_dev, buf, (Uint32)(count * sizeof(int16_t)));
	if(g.audio_waiting && SDL_GetQueuedAudioSize(g.audio_dev) >= (Uint32)(g.audio_target_ms < target_max ? g.audio_target_ms : target_max) * bytes_per_ms)
	{
		SDL_PauseAudioDevice(g.audio_dev, 0);
		g.audio_waiting = false;
	}
}

// ---------- video ----------

static void frame_available_cb(ChiakiFfmpegDecoder *decoder, void *user)
{
	(void)decoder;
	(void)user;
	if(InterlockedExchange(&g.frame_pending, 1))
		return;
	SDL_Event e = { 0 };
	e.type = SDL_USEREVENT;
	SDL_PushEvent(&e);
}

static int config_int(const char *key, int fallback, int min, int max);
static void config_set_int(const char *key, int value);

static double now_ms(void)
{
	static LARGE_INTEGER freq;
	if(!freq.QuadPart)
		QueryPerformanceFrequency(&freq);
	LARGE_INTEGER t;
	QueryPerformanceCounter(&t);
	return t.QuadPart * 1000.0 / freq.QuadPart;
}

static DWORD WINAPI video_thread_main(void *arg)
{
	(void)arg;
	if(SDL_InitSubSystem(SDL_INIT_VIDEO) != 0)
	{
		CHIAKI_LOGE(&g.log, "SDL video failed: %s", SDL_GetError());
		SetEvent(g.video_ready);
		return 1;
	}
	SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
	char title[128];
	WideCharToMultiByte(CP_UTF8, 0, tr(L"PS Stream - Görüntü"), -1, title, sizeof(title), NULL, NULL);
	// Back where it was last time, so a Discord layout doesn't have to be redone
	int win_x = config_int("video_x", 99999, -32768, 32767), win_y = config_int("video_y", 99999, -32768, 32767);
	POINT win_corner = { win_x + 40, win_y + 40 };
	if(win_x == 99999 || win_y == 99999 || !MonitorFromPoint(win_corner, MONITOR_DEFAULTTONULL))
		win_x = win_y = SDL_WINDOWPOS_CENTERED;
	SDL_Window *window = SDL_CreateWindow(title, win_x, win_y, config_int("video_w", S(960), 320, 16384), config_int("video_h", S(540), 180, 16384),
			SDL_WINDOW_RESIZABLE | (g.test_hidden ? SDL_WINDOW_HIDDEN : 0));
	SDL_Renderer *renderer = window ? SDL_CreateRenderer(window, -1, 0) : NULL;
	if(!renderer)
		CHIAKI_LOGE(&g.log, "SDL window failed: %s", SDL_GetError());
	SetEvent(g.video_ready);

	SDL_Texture *texture = NULL;
	int tex_w = 0, tex_h = 0, tex_format = AV_PIX_FMT_NONE;
	AVFrame *sw_frame = av_frame_alloc();
	bool dirty = true;
	double stat_pull = 0, stat_copy = 0, stat_upload = 0, stat_present = 0, stat_start = now_ms();
	int stat_frames = 0, stat_lost = 0, stat_windows = 0;
	bool fullscreen = false;
	while(renderer && !g.video_stop)
	{
		SDL_Event e;
		if(SDL_WaitEventTimeout(&e, 100))
		{
			do
			{
				if(e.type == SDL_QUIT)
					PostMessageW(g.hwnd, WM_APP_VIDEO_CLOSED, 0, 0);
				else if(e.type == SDL_WINDOWEVENT || e.type == SDL_RENDER_TARGETS_RESET)
					dirty = true;
				else if(e.type == SDL_RENDER_DEVICE_RESET)
				{
					// A GPU reset or switch loses the texture; the next frame builds a new one
					if(texture)
						SDL_DestroyTexture(texture);
					texture = NULL;
					dirty = true;
				}
				else if(e.type == SDL_KEYDOWN && !e.key.repeat && (e.key.keysym.sym == SDLK_F11 || (e.key.keysym.sym == SDLK_ESCAPE && fullscreen)))
				{
					fullscreen = !fullscreen;
					SDL_SetWindowFullscreen(window, fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
					dirty = true;
				}
			} while(SDL_PollEvent(&e));
		}
		if(InterlockedExchange(&g.frame_pending, 0))
		{
			int32_t lost;
			double t0 = now_ms();
			ChiakiFfmpegFrame frame = chiaki_ffmpeg_decoder_pull_frame(&g.decoder, &lost);
			AVFrame *f = frame.frame;
			stat_pull += now_ms() - t0;
			stat_lost += lost;
			if(f)
			{
				AVFrame *show = f;
				if(f->hw_frames_ctx)
				{
					av_frame_unref(sw_frame);
					t0 = now_ms();
					show = av_hwframe_transfer_data(sw_frame, f, 0) == 0 ? sw_frame : NULL;
					stat_copy += now_ms() - t0;
				}
				if(show && (show->format == AV_PIX_FMT_NV12 || show->format == AV_PIX_FMT_YUV420P || show->format == AV_PIX_FMT_YUVJ420P))
				{
					if(!texture || show->width != tex_w || show->height != tex_h || show->format != tex_format)
					{
						if(texture)
							SDL_DestroyTexture(texture);
						tex_w = show->width;
						tex_h = show->height;
						tex_format = show->format;
						texture = SDL_CreateTexture(renderer, show->format == AV_PIX_FMT_NV12 ? SDL_PIXELFORMAT_NV12 : SDL_PIXELFORMAT_IYUV,
								SDL_TEXTUREACCESS_STREAMING, tex_w, tex_h);
						SDL_RenderSetLogicalSize(renderer, tex_w, tex_h);
						CHIAKI_LOGI(&g.log, "Video: %dx%d, %s", tex_w, tex_h, f->hw_frames_ctx ? "hardware decoding" : "software decoding");
					}
					t0 = now_ms();
					if(texture && show->format == AV_PIX_FMT_NV12)
						SDL_UpdateNVTexture(texture, NULL, show->data[0], show->linesize[0], show->data[1], show->linesize[1]);
					else if(texture)
						SDL_UpdateYUVTexture(texture, NULL, show->data[0], show->linesize[0], show->data[1], show->linesize[1], show->data[2], show->linesize[2]);
					stat_upload += now_ms() - t0;
					stat_frames++;
					dirty = true;
				}
				av_frame_free(&f);
			}
		}
		if(dirty)
		{
			double t0 = now_ms();
			SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
			SDL_RenderClear(renderer);
			if(texture)
				SDL_RenderCopy(renderer, texture, NULL, NULL);
#ifdef PSSTREAM_TEST
			if(g.test_video_shot && texture)
			{
				SDL_Surface *shot = SDL_CreateRGBSurfaceWithFormat(0, S(960), S(540), 32, SDL_PIXELFORMAT_ARGB8888);
				SDL_RenderSetLogicalSize(renderer, 0, 0);
				SDL_RenderClear(renderer);
				SDL_RenderCopy(renderer, texture, NULL, NULL);
				SDL_RenderReadPixels(renderer, NULL, SDL_PIXELFORMAT_ARGB8888, shot->pixels, shot->pitch);
				SDL_SaveBMP(shot, g.test_video_shot);
				SDL_FreeSurface(shot);
				g.test_video_shot = NULL;
			}
#endif
			SDL_RenderPresent(renderer);
			stat_present += now_ms() - t0;
			dirty = false;
		}
		if(now_ms() - stat_start >= 2000 && stat_frames)
		{
			double fps = stat_frames * 1000.0 / (now_ms() - stat_start);
			g.video_fps10 = (LONG)(fps * 10);
			InterlockedExchangeAdd(&g.video_lost_total, stat_lost);
			if(stat_lost || ++stat_windows >= STATS_LOG_EVERY)
			{
				stat_windows = 0;
				CHIAKI_LOGI(&g.log, "Video stats: %.1f fps, lost %d, per frame ms: pull %.1f, copy %.1f, upload %.1f, present %.1f",
						fps, stat_lost, stat_pull / stat_frames, stat_copy / stat_frames, stat_upload / stat_frames, stat_present / stat_frames);
			}
			stat_pull = stat_copy = stat_upload = stat_present = 0;
			stat_frames = stat_lost = 0;
			stat_start = now_ms();
		}
	}
	av_frame_free(&sw_frame);
	if(texture)
		SDL_DestroyTexture(texture);
	if(renderer)
		SDL_DestroyRenderer(renderer);
	if(window)
	{
		if(!fullscreen && !(SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED))
		{
			SDL_GetWindowPosition(window, &win_x, &win_y);
			int win_w, win_h;
			SDL_GetWindowSize(window, &win_w, &win_h);
			config_set_int("video_x", win_x);
			config_set_int("video_y", win_y);
			config_set_int("video_w", win_w);
			config_set_int("video_h", win_h);
		}
		SDL_DestroyWindow(window);
	}
	SDL_QuitSubSystem(SDL_INIT_VIDEO);
	return 0;
}

static bool video_start(ChiakiCodec codec, unsigned int fps, bool hw)
{
	ChiakiErrorCode err = CHIAKI_ERR_UNKNOWN;
	if(hw)
		err = chiaki_ffmpeg_decoder_init(&g.decoder, &g.log, codec, fps, "d3d11va", NULL, frame_available_cb, NULL);
	if(err != CHIAKI_ERR_SUCCESS)
		err = chiaki_ffmpeg_decoder_init(&g.decoder, &g.log, codec, fps, NULL, NULL, frame_available_cb, NULL);
	if(err != CHIAKI_ERR_SUCCESS)
		return false;
	g.video_stop = 0;
	g.frame_pending = 0;
	ResetEvent(g.video_ready);
	g.video_thread = CreateThread(NULL, 0, video_thread_main, NULL, 0, NULL);
	// SDL's event queue must exist before the decoder pushes to it
	WaitForSingleObject(g.video_ready, INFINITE);
	return true;
}

static void video_stop(void)
{
	g.video_stop = 1;
	WaitForSingleObject(g.video_thread, INFINITE);
	CloseHandle(g.video_thread);
	g.video_thread = NULL;
	chiaki_ffmpeg_decoder_fini(&g.decoder);
}

// ---------- session ----------

static void session_event_cb(ChiakiEvent *event, void *user)
{
	(void)user;
	switch(event->type)
	{
		case CHIAKI_EVENT_CONNECTED:
			g.connected_at = GetTickCount();
			g.connected = 1;
			SetEvent(g.wake);
			set_status(g.want_video ? L"Bağlandı. Görüntü açık" : L"Bağlandı. Ses açık");
			break;
		case CHIAKI_EVENT_LOGIN_PIN_REQUEST:
			g.fatal = 1;
			set_status(L"PS5'te Remote Play giriş şifresi açık, kapatıp tekrar dene");
			g.ended = 1;
			SetEvent(g.wake);
			break;
		case CHIAKI_EVENT_QUIT:
			g.quit_reason = event->quit.reason;
			CHIAKI_LOGI(&g.log, "Session quit: %s (%s)", chiaki_quit_reason_string(event->quit.reason),
					event->quit.reason_str ? event->quit.reason_str : "");
			g.ended = 1;
			SetEvent(g.wake);
			break;
		default:
			break;
	}
}

static void profile_set(ChiakiConnectVideoProfile *p, int res, int fps, int kbps, ChiakiCodec codec, bool safe)
{
	if(safe && kbps < SAFE_KBPS)
		kbps = SAFE_KBPS;
	p->width = resolutions[res].width;
	p->height = resolutions[res].height;
	p->max_fps = fps;
	p->bitrate = kbps;
	p->codec = codec;
}

static void host_get(char *out, size_t size)
{
	EnterCriticalSection(&g.lock);
	snprintf(out, size, "%s", g.host);
	LeaveCriticalSection(&g.lock);
}

static void host_set(const char *host)
{
	EnterCriticalSection(&g.lock);
	snprintf(g.host, sizeof(g.host), "%s", host);
	LeaveCriticalSection(&g.lock);
}

static bool session_start(bool video, const Settings *s, bool safe)
{
	ChiakiCodec codec = s->h265 ? CHIAKI_CODEC_H265 : CHIAKI_CODEC_H264;
	ChiakiConnectInfo info;
	memset(&info, 0, sizeof(info));
	info.ps5 = true;
	char host[256];
	host_get(host, sizeof(host));
	info.host = host;
	memcpy(info.regist_key, g.regist_key, sizeof(info.regist_key));
	memcpy(info.morning, g.morning, sizeof(info.morning));
	profile_set(&info.video_profile, s->res, s->fps, s->kbps, codec, safe);
	info.video_profile_auto_downgrade = true;
	info.packet_loss_max = s->loss_pct / 100.0;
	info.enable_idr_on_fec_failure = true;
	info.disable_controller = s->no_controller != 0;
	info.audio_video_disabled = video ? 0 : CHIAKI_VIDEO_DISABLED;

	chiaki_opus_decoder_init(&g.opus, &g.log);
	chiaki_opus_decoder_set_cb(&g.opus, audio_settings_cb, audio_frame_cb, NULL);
	if(video && !video_start(codec, s->fps, s->hw != 0))
	{
		chiaki_opus_decoder_fini(&g.opus);
		return false;
	}

	g.connected = 0;
	g.ended = 0;
	g.audio_dropouts = 0;
	g.video_fps10 = 0;
	g.video_lost_total = 0;
	ChiakiErrorCode err = chiaki_session_init(&g.session, &info, &g.log);
	if(err != CHIAKI_ERR_SUCCESS)
	{
		CHIAKI_LOGE(&g.log, "Session init failed: %s", chiaki_error_string(err));
		goto error;
	}

	// The console keeps sending video even when the client ignores it, and chiaki_session_init
	// forces 2 Mbps when video is disabled, so the smallest stream has to be set afterwards.
	if(!video)
		profile_set(&g.session.connect_info.video_profile, s->ao_res, 30, s->ao_kbps, CHIAKI_CODEC_H264, safe);

	chiaki_session_set_event_cb(&g.session, session_event_cb, NULL);
	ChiakiAudioSink sink;
	chiaki_opus_decoder_get_sink(&g.opus, &sink);
	chiaki_session_set_audio_sink(&g.session, &sink);
	if(video)
		chiaki_session_set_video_sample_cb(&g.session, chiaki_ffmpeg_decoder_video_sample_cb, &g.decoder);

	err = chiaki_session_start(&g.session);
	if(err != CHIAKI_ERR_SUCCESS)
	{
		CHIAKI_LOGE(&g.log, "Session start failed: %s", chiaki_error_string(err));
		chiaki_session_fini(&g.session);
		goto error;
	}
	CHIAKI_LOGI(&g.log, "Session started: %ux%u@%u %u kbps, video %s, controller %s",
			g.session.connect_info.video_profile.width, g.session.connect_info.video_profile.height,
			g.session.connect_info.video_profile.max_fps, g.session.connect_info.video_profile.bitrate, video ? "on" : "off", s->no_controller ? "hidden" : "announced");
	return true;

error:
	if(video)
		video_stop();
	chiaki_opus_decoder_fini(&g.opus);
	return false;
}

static void session_stop(bool video)
{
	chiaki_session_stop(&g.session);
	chiaki_session_join(&g.session);
	chiaki_session_fini(&g.session);
	g.connected = 0;
	if(video)
		video_stop();
	chiaki_opus_decoder_fini(&g.opus);
	if(g.audio_dev)
	{
		SDL_PauseAudioDevice(g.audio_dev, 1);
		SDL_ClearQueuedAudio(g.audio_dev);
		g.audio_waiting = true;
	}
}

static const wchar_t *quit_reason_text(ChiakiQuitReason reason)
{
	switch(reason)
	{
		case CHIAKI_QUIT_REASON_SESSION_REQUEST_RP_IN_USE:
			return L"PS5 başka bir Remote Play bağlantısında, tekrar deneniyor...";
		case CHIAKI_QUIT_REASON_SESSION_REQUEST_RP_CRASH:
			return L"PS5'te Remote Play hata verdi, tekrar deneniyor...";
		case CHIAKI_QUIT_REASON_SESSION_REQUEST_RP_VERSION_MISMATCH:
			return L"PS5 yazılımı bu sürümle uyumsuz";
		case CHIAKI_QUIT_REASON_STREAM_CONNECTION_REMOTE_SHUTDOWN:
			return L"PS5 kapandı, açılması bekleniyor...";
		case CHIAKI_QUIT_REASON_SESSION_REQUEST_CONNECTION_REFUSED:
		case CHIAKI_QUIT_REASON_CTRL_CONNECTION_REFUSED:
			return L"PS5 bağlantıyı reddetti, tekrar deneniyor...";
		default:
			return L"Bağlantı yok, tekrar deneniyor...";
	}
}

static bool quit_reason_is_final(ChiakiQuitReason reason)
{
	return reason == CHIAKI_QUIT_REASON_SESSION_REQUEST_RP_VERSION_MISMATCH;
}

// Only a console that accepted the session and then ended the stream may be refusing the
// bitrate. A console that's off or still busy says nothing about it.
static bool quit_reason_refused_stream(ChiakiQuitReason reason)
{
	return reason == CHIAKI_QUIT_REASON_STREAM_CONNECTION_UNKNOWN
		|| reason == CHIAKI_QUIT_REASON_STREAM_CONNECTION_REMOTE_DISCONNECTED;
}

// Stops Windows' periodic Wi-Fi scans, which stall traffic for moments, and asks the driver for
// streaming mode. No admin rights needed; Windows undoes both when the handle closes.
static void wifi_tune(bool on)
{
	if(!on)
	{
		if(g.wlan)
		{
			WlanCloseHandle(g.wlan, NULL);
			g.wlan = NULL;
			CHIAKI_LOGI(&g.log, "Wi-Fi low latency off");
		}
		return;
	}
	if(g.wlan)
		return;
	DWORD version;
	if(WlanOpenHandle(2, NULL, &version, &g.wlan) != ERROR_SUCCESS)
	{
		g.wlan = NULL;
		return;
	}
	WLAN_INTERFACE_INFO_LIST *list = NULL;
	if(WlanEnumInterfaces(g.wlan, NULL, &list) != ERROR_SUCCESS)
		return;
	for(DWORD i = 0; i < list->dwNumberOfItems; i++)
	{
		if(list->InterfaceInfo[i].isState != wlan_interface_state_connected)
			continue;
		const GUID *id = &list->InterfaceInfo[i].InterfaceGuid;
		BOOL off = FALSE, enable = TRUE;
		DWORD scan = WlanSetInterface(g.wlan, id, wlan_intf_opcode_background_scan_enabled, sizeof(off), &off, NULL);
		DWORD stream = WlanSetInterface(g.wlan, id, wlan_intf_opcode_media_streaming_mode, sizeof(enable), &enable, NULL);
		DWORD size = 0;
		BOOL *active = NULL;
		bool confirmed = WlanQueryInterface(g.wlan, id, wlan_intf_opcode_media_streaming_mode, NULL, &size, (void **)&active, NULL) == ERROR_SUCCESS && active && *active;
		if(active)
			WlanFreeMemory(active);
		CHIAKI_LOGI(&g.log, "Wi-Fi low latency on: scan result %lu, streaming result %lu, streaming active %d", scan, stream, confirmed);
	}
	WlanFreeMemory(list);
}

// Asks for the console on the known address first, then on the whole network, so a PS5 that got a
// new address from the router is found again. Returns false if no PS5 answered.
static bool ps5_search(const char *prefer, int rounds, bool sweep, char *ip, size_t ip_size, bool *standby)
{
	SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if(s == INVALID_SOCKET)
		return false;
	BOOL on = TRUE;
	DWORD timeout = 400;
	setsockopt(s, SOL_SOCKET, SO_BROADCAST, (char *)&on, sizeof(on));
	setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char *)&timeout, sizeof(timeout));

	uint32_t targets[17];
	int target_count = 0;
	uint32_t prefer_addr = prefer && prefer[0] ? inet_addr(prefer) : INADDR_NONE;
	if(prefer_addr != INADDR_NONE)
		targets[target_count++] = prefer_addr;
	targets[target_count++] = INADDR_BROADCAST;
	static char table_buf[4096];
	MIB_IPADDRTABLE *table = (MIB_IPADDRTABLE *)table_buf;
	ULONG table_size = sizeof(table_buf);
	if(GetIpAddrTable(table, &table_size, FALSE) == NO_ERROR)
		for(DWORD i = 0; i < table->dwNumEntries && target_count < 17; i++)
			if(table->table[i].dwAddr && table->table[i].dwAddr != htonl(INADDR_LOOPBACK))
				targets[target_count++] = table->table[i].dwAddr | ~table->table[i].dwMask;

	static const char msg[] = "SRCH * HTTP/1.1\ndevice-discovery-protocol-version:00030010\n";
	bool found = false;
	for(int round = 0; round < rounds && !g.quit; round++)
	{
		for(int i = 0; i < target_count; i++)
		{
			struct sockaddr_in to = { 0 };
			to.sin_family = AF_INET;
			to.sin_port = htons(9302);
			to.sin_addr.s_addr = targets[i];
			sendto(s, msg, sizeof(msg) - 1, 0, (struct sockaddr *)&to, sizeof(to));
		}
		// Many routers drop broadcasts between Wi-Fi devices, but pass a message sent to one address
		if(sweep && round % 5 == 0)
		{
			for(DWORD i = 0; i < table->dwNumEntries; i++)
			{
				uint32_t addr = ntohl(table->table[i].dwAddr);
				if(!addr || (addr >> 24) == 127 || (addr >> 16) == 0xA9FE)
					continue;
				for(uint32_t host_part = 1; host_part < 255; host_part++)
				{
					struct sockaddr_in to = { 0 };
					to.sin_family = AF_INET;
					to.sin_port = htons(9302);
					to.sin_addr.s_addr = htonl((addr & 0xFFFFFF00) | host_part);
					sendto(s, msg, sizeof(msg) - 1, 0, (struct sockaddr *)&to, sizeof(to));
				}
			}
		}
		// Read every answer of this round, so a second PS5 at home can't win over the known one
		while(true)
		{
			char buf[1024];
			struct sockaddr_in from;
			int from_len = sizeof(from);
			int n = recvfrom(s, buf, sizeof(buf) - 1, 0, (struct sockaddr *)&from, &from_len);
			if(n <= 0)
				break;
			buf[n] = 0;
			if(!strstr(buf, "host-type:PS5"))
				continue;
			bool preferred = from.sin_addr.s_addr == prefer_addr;
			if(!found || preferred)
			{
				snprintf(ip, ip_size, "%s", inet_ntoa(from.sin_addr));
				*standby = strstr(buf, "HTTP/1.1 620") != NULL;
			}
			found = true;
			if(preferred)
				break;
		}
		if(found)
			break;
	}
	closesocket(s);
	return found;
}

// Discovery answers get lost while Wi-Fi or Windows' firewall are still settling after startup,
// but the console's Remote Play port answers or it doesn't.
static bool ps5_port_open(const char *host, long timeout_ms)
{
	struct sockaddr_in to = { 0 };
	to.sin_family = AF_INET;
	to.sin_port = htons(9295);
	to.sin_addr.s_addr = inet_addr(host);
	if(to.sin_addr.s_addr == INADDR_NONE)
		return false;
	SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if(s == INVALID_SOCKET)
		return false;
	u_long nonblocking = 1;
	ioctlsocket(s, FIONBIO, &nonblocking);
	connect(s, (struct sockaddr *)&to, sizeof(to));
	fd_set writable, failed;
	FD_ZERO(&writable);
	FD_ZERO(&failed);
	FD_SET(s, &writable);
	FD_SET(s, &failed);
	struct timeval timeout = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
	bool open = select(0, NULL, &writable, &failed, &timeout) > 0 && FD_ISSET(s, &writable);
	closesocket(s);
	return open;
}

// A network coming up is the best moment to try again, so don't wait for the next retry.
static DWORD WINAPI network_watch_main(void *arg)
{
	(void)arg;
	while(!g.quit && NotifyAddrChange(NULL, NULL) == NO_ERROR)
	{
		g.network_changed = 1;
		SetEvent(g.wake);
	}
	return 0;
}

static bool ps5_wake_send(void);

static bool stream_changed(const Settings *a, const Settings *b, bool video)
{
	if(a->loss_pct != b->loss_pct || a->no_controller != b->no_controller)
		return true;
	if(!video)
		return a->ao_res != b->ao_res || a->ao_kbps != b->ao_kbps;
	return a->res != b->res || a->fps != b->fps || a->kbps != b->kbps || a->h265 != b->h265 || a->hw != b->hw;
}

// All session start/stop happens here because stopping can block for seconds.
static DWORD WINAPI worker_main(void *arg)
{
	(void)arg;
	bool running = false, run_video = false, safe = false, waiting = false, hold_status = false;
	int fails = 0;
	Settings run = { 0 };
	DWORD retry_at = 0, trying_since = 0, wake_at = 0;
	int probes = 0, wake_tries = 0;
	CloseHandle(CreateThread(NULL, 0, network_watch_main, NULL, 0, NULL));
	while(true)
	{
		DWORD timeout = INFINITE;
		if(waiting)
		{
			LONG left = (LONG)(retry_at - GetTickCount());
			timeout = left > 0 ? (DWORD)left : 0;
		}
		WaitForSingleObject(g.wake, timeout);
		if(InterlockedExchange(&g.network_changed, 0) && waiting && !running)
		{
			CHIAKI_LOGI(&g.log, "Network changed, trying again soon");
			trying_since = GetTickCount();
			retry_at = trying_since + 1000;
		}

		bool want_video = g.want_video != 0;
		bool want_any = !g.quit && (g.want_audio || want_video);
		Settings s = settings_get();
		// Only while connected, so a PS5 that's off doesn't keep the PC awake or its Wi-Fi tuned
		bool live = want_any && g.connected;
		wifi_tune(live && s.wifi_opt);
		// The PC often sits untouched while the game plays on the TV
		SetThreadExecutionState(ES_CONTINUOUS | (live ? ES_SYSTEM_REQUIRED : 0) | (live && want_video ? ES_DISPLAY_REQUIRED : 0));
		if(live)
		{
			fails = 0;
			trying_since = 0;
			probes = 0;
			wake_tries = 0;
			g.ps5_standby = 0;
		}

		if(running)
		{
			bool ended = InterlockedExchange(&g.ended, 0) != 0;
			bool changed = stream_changed(&s, &run, run_video);
			if(ended || !want_any || want_video != run_video || changed)
			{
				bool was_connected = g.connected != 0;
				ChiakiQuitReason reason = g.quit_reason;
				session_stop(run_video);
				running = false;
				waiting = true;
				if(changed)
					safe = false;
				if(ended && want_any)
				{
					if(!was_connected && quit_reason_refused_stream(reason))
						safe = true;
					if(g.fatal || quit_reason_is_final(reason) || !s.reconnect)
					{
						if(!g.fatal)
							set_status(quit_reason_is_final(reason) ? quit_reason_text(reason) : L"Bağlantı koptu");
						g.fatal = 0;
						// Keep the reason on screen instead of replacing it with "off"
						hold_status = true;
						g.want_audio = 0;
						g.want_video = 0;
						PostMessageW(g.hwnd, WM_APP_ALL_OFF, 0, 0);
						waiting = false;
						continue;
					}
					if(!was_connected)
						fails++;
					if(!trying_since)
						trying_since = GetTickCount();
					// Right after startup the network needs a moment, so keep trying quickly and quietly
					bool early = GetTickCount() - trying_since < RETRY_FAST_WINDOW_MS;
					int delay = early ? RETRY_FAST_MS : RETRY_MAX_MS;
					retry_at = GetTickCount() + delay;
					const wchar_t *text = quit_reason_text(reason);
					if(wcscmp(text, L"Bağlantı yok, tekrar deneniyor...") == 0)
						text = early ? L"PS5'e bağlanılıyor..." : L"PS5'e ulaşılamıyor. Açık mı? Denemeye devam ediyorum";
					set_status(text);
					CHIAKI_LOGI(&g.log, "Connection attempt failed (%s), %d in a row, next in %d ms", chiaki_quit_reason_string(reason), fails, delay);
				}
				else
				{
					retry_at = GetTickCount() + RECONNECT_MS;
					if(want_any)
						set_status(L"Yeni ayarlarla yeniden bağlanıyor...");
				}
			}
		}
		if(g.quit)
			break;
		if(!want_any)
		{
			if(hold_status)
				hold_status = false;
			else if(waiting || !running)
				set_status(g.paired ? L"Kapalı. Başlamak için Ses veya Görüntü'ye bas" : L"Önce Eşleştirme sayfasından PS5'i bağla");
			waiting = false;
			safe = false;
			fails = 0;
			trying_since = 0;
			probes = 0;
			wake_tries = 0;
			g.ps5_standby = 0;
			continue;
		}
		hold_status = false;
		if(!running && (!waiting || (LONG)(retry_at - GetTickCount()) <= 0))
		{
			waiting = false;
			if(!trying_since)
				trying_since = GetTickCount();
			{
				char host[256], ip[64] = "";
				bool standby = false;
				host_get(host, sizeof(host));
				// A full attempt takes 5 s to give up on a PS5 that's away. This takes a moment, so
				// connecting can begin the moment the console is back.
				probes++;
				bool open = ps5_port_open(host, 800);
				if(!open)
				{
					bool answered = ps5_search(host, 1, probes % PROBES_PER_SWEEP == 1, ip, sizeof(ip), &standby);
					g.ps5_standby = answered && standby;
					if(answered && strcmp(ip, host) != 0)
					{
						CHIAKI_LOGI(&g.log, "PS5 moved to a new address");
						host_set(ip);
						PostMessageW(g.hwnd, WM_APP_HOST_CHANGED, 0, 0);
						open = !standby;
					}
					// A resting PS5 may not answer the search at all, so try waking it either way
					if(s.auto_wake && g.paired && wake_tries < WAKE_MAX_TRIES && (LONG)(GetTickCount() - wake_at) >= 0)
					{
						wake_tries++;
						wake_at = GetTickCount() + WAKE_REPEAT_MS;
						ps5_wake_send();
					}
					if(probes % 30 == 1)
						CHIAKI_LOGI(&g.log, "PS5 check %d: port closed, search %s", probes, !answered ? "no answer" : standby ? "rest mode" : "on");
				}
				// In case the check itself is what's being blocked, a real attempt now and then
				if(!open && probes % PROBES_PER_SESSION_TRY != 0)
				{
					DWORD waited = GetTickCount() - trying_since;
					if(g.ps5_standby)
						set_status(s.auto_wake ? L"PS5 uyanıyor..." : L"PS5 uyku modunda. Uyandırabilir veya PS5'i açabilirsin");
					else
						set_status(waited < RETRY_FAST_WINDOW_MS ? L"PS5'e bağlanılıyor..." : L"PS5'e ulaşılamıyor. Açık mı? Denemeye devam ediyorum");
					waiting = true;
					retry_at = GetTickCount() + (waited < PROBE_SLOW_AFTER_MS ? PROBE_DELAY_MS : PROBE_SLOW_DELAY_MS);
					continue;
				}
				if(open)
					CHIAKI_LOGI(&g.log, "PS5 port open after %d checks", probes);
				g.ps5_standby = 0;
			}
			set_status(L"Bağlanıyor...");
			if(session_start(want_video, &s, safe))
			{
				running = true;
				run_video = want_video;
				run = s;
			}
			else
			{
				set_status(L"Bağlanılamadı, PS5 IP adresini kontrol et. Tekrar deneniyor...");
				fails++;
				waiting = true;
				retry_at = GetTickCount() + RETRY_MS;
			}
		}
	}
	return 0;
}

// ---------- config ----------

static void hex_encode(const uint8_t *in, size_t n, char *out)
{
	for(size_t i = 0; i < n; i++)
		sprintf(out + i * 2, "%02x", in[i]);
}

static bool hex_decode(const char *in, uint8_t *out, size_t n)
{
	if(strlen(in) != n * 2)
		return false;
	for(size_t i = 0; i < n; i++)
	{
		unsigned int v;
		if(sscanf(in + i * 2, "%2x", &v) != 1)
			return false;
		out[i] = (uint8_t)v;
	}
	return true;
}

static void config_read(const wchar_t *section, const char *key, char *buf, DWORD size)
{
	wchar_t k[64], v[1024];
	MultiByteToWideChar(CP_UTF8, 0, key, -1, k, 64);
	GetPrivateProfileStringW(section, k, L"", v, 1024, g.config_path);
	if(!WideCharToMultiByte(CP_UTF8, 0, v, -1, buf, size, NULL, NULL))
		buf[0] = 0;
}

static void config_write(const wchar_t *section, const char *key, const char *value)
{
	wchar_t k[64], v[1024];
	MultiByteToWideChar(CP_UTF8, 0, key, -1, k, 64);
	MultiByteToWideChar(CP_UTF8, 0, value, -1, v, 1024);
	WritePrivateProfileStringW(section, k, v, g.config_path);
}

static void config_get(const char *key, char *buf, DWORD size)
{
	config_read(L"ps5", key, buf, size);
}

static void config_set(const char *key, const char *value)
{
	config_write(L"ps5", key, value);
}

static int config_int(const char *key, int fallback, int min, int max)
{
	wchar_t k[64];
	MultiByteToWideChar(CP_UTF8, 0, key, -1, k, 64);
	int v = GetPrivateProfileIntW(L"settings", k, fallback, g.config_path);
	return v < min || v > max ? fallback : v;
}

static void config_set_int(const char *key, int value)
{
	char v[16];
	snprintf(v, sizeof(v), "%d", value);
	config_write(L"settings", key, v);
}

#define RUN_KEY L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define RUN_VALUE L"PS Stream"

static bool win_start_get(void)
{
	return RegGetValueW(HKEY_CURRENT_USER, RUN_KEY, RUN_VALUE, RRF_RT_REG_SZ, NULL, NULL, NULL) == ERROR_SUCCESS;
}

static void win_start_set(bool on)
{
	if(!on)
	{
		RegDeleteKeyValueW(HKEY_CURRENT_USER, RUN_KEY, RUN_VALUE);
		return;
	}
	wchar_t exe[MAX_PATH], line[MAX_PATH + 16];
	GetModuleFileNameW(NULL, exe, MAX_PATH);
	swprintf(line, MAX_PATH + 16, L"\"%ls\" --tray", exe);
	RegSetKeyValueW(HKEY_CURRENT_USER, RUN_KEY, RUN_VALUE, REG_SZ, line, (DWORD)((wcslen(line) + 1) * sizeof(wchar_t)));
}

static int hotkey_load(const char *key, int fallback)
{
	int hotkey = config_int(key, fallback, 0, 0xffff);
	int vk = LOBYTE(hotkey), f = HIBYTE(hotkey);
	// An older version allowed a bare ordinary key, which takes it away from every other program
	if(vk && !(f & (HOTKEYF_CONTROL | HOTKEYF_ALT)) && !(vk >= VK_F1 && vk <= VK_F24))
		return fallback;
	return hotkey;
}

static void config_load(void)
{
	char regist[64], morning[64];
	config_get("host", g.host, sizeof(g.host));
	config_get("regist_key", regist, sizeof(regist));
	config_get("morning", morning, sizeof(morning));
	g.paired = hex_decode(regist, (uint8_t *)g.regist_key, sizeof(g.regist_key))
		&& hex_decode(morning, g.morning, sizeof(g.morning));

	Settings *s = &g.settings;
	const Preset *p = &presets[PRESET_DEFAULT];
	s->res = config_int("res", p->res, RES_MIN, RES_COUNT - 1);
	s->fps = config_int("fps", p->fps, 30, 60) == 60 ? 60 : 30;
	s->kbps = config_int("kbps", p->kbps, 500, 100000);
	s->h265 = config_int("h265", p->h265, 0, 1);
	s->hw = config_int("hw", 1, 0, 1);
	s->ao_res = config_int("ao_res", RES_MIN, RES_MIN, RES_MIN);
	s->ao_kbps = config_int("ao_kbps", 500, 100, 2000);
	s->volume = config_int("volume", 100, 0, 200);
	s->buffer_ms = config_int("buffer_ms", 150, 40, 500);
	s->loss_pct = config_int("loss_pct", 5, 1, 20);
	s->reconnect = config_int("reconnect", 1, 0, 1);
	s->no_controller = config_int("no_controller", 1, 0, 1);
	s->wifi_opt = config_int("wifi_opt", 1, 0, 1);
	s->autostart_audio = config_int("autostart_audio", 1, 0, 1);
	s->win_start = win_start_get();
	s->tray = config_int("tray", 1, 0, 1);
	s->update_check = config_int("update_check", 1, 0, 1);
	s->auto_wake = config_int("auto_wake", 1, 0, 1);
	s->hotkey_audio = hotkey_load("hotkey_audio", MAKEWORD(VK_F9, HOTKEYF_CONTROL | HOTKEYF_ALT));
	s->hotkey_video = hotkey_load("hotkey_video", MAKEWORD(VK_F10, HOTKEYF_CONTROL | HOTKEYF_ALT));
	char device_hex[sizeof(s->device) * 2 + 1];
	config_read(L"settings", "device_hex", device_hex, sizeof(device_hex));
	size_t device_len = strlen(device_hex) / 2;
	memset(s->device, 0, sizeof(s->device));
	if(!device_len || device_len >= sizeof(s->device) || !hex_decode(device_hex, (uint8_t *)s->device, device_len))
		config_read(L"settings", "device", s->device, sizeof(s->device));
}

static void config_save_settings(const Settings *s)
{
	config_set_int("res", s->res);
	config_set_int("fps", s->fps);
	config_set_int("kbps", s->kbps);
	config_set_int("h265", s->h265);
	config_set_int("hw", s->hw);
	config_set_int("ao_res", s->ao_res);
	config_set_int("ao_kbps", s->ao_kbps);
	config_set_int("volume", s->volume);
	config_set_int("buffer_ms", s->buffer_ms);
	config_set_int("loss_pct", s->loss_pct);
	config_set_int("reconnect", s->reconnect);
	config_set_int("no_controller", s->no_controller);
	config_set_int("wifi_opt", s->wifi_opt);
	config_set_int("autostart_audio", s->autostart_audio);
	config_set_int("tray", s->tray);
	config_set_int("update_check", s->update_check);
	config_set_int("auto_wake", s->auto_wake);
	config_set_int("hotkey_audio", s->hotkey_audio);
	config_set_int("hotkey_video", s->hotkey_video);
	// Device names can hold any language, and an old INI file only stores its own code page
	char device_hex[sizeof(s->device) * 2 + 1];
	hex_encode((const uint8_t *)s->device, strlen(s->device), device_hex);
	device_hex[strlen(s->device) * 2] = 0;
	config_write(L"settings", "device_hex", device_hex);
}

// ---------- pairing ----------

static void get_text(int id, char *buf, int size)
{
	wchar_t w[1024];
	GetDlgItemTextW(g.hwnd, id, w, 1024);
	if(!WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, size, NULL, NULL))
		buf[0] = 0;
}

static volatile LONG finding;

// Wi-Fi drops single broadcasts, and the console can take seconds to answer, so keep asking for a while.
static DWORD WINAPI find_ps5_thread(void *arg)
{
	(void)arg;
	char typed[64], ip[64] = "";
	bool standby = false;
	get_text(ID_HOST, typed, sizeof(typed));
	set_status(L"PS5 aranıyor...");
	bool found = false;
	if(typed[0] && ps5_port_open(typed, 1500))
	{
		found = true;
		snprintf(ip, sizeof(ip), "%s", typed);
	}
	else
		found = ps5_search(typed, 50, true, ip, sizeof(ip), &standby);
	if(found)
	{
		SetDlgItemTextA(g.hwnd, ID_HOST, ip);
		set_status(standby ? L"PS5 bulundu ama uyku modunda, PS5'i aç" : L"PS5 bulundu");
	}
	else
		set_status(L"PS5 bulunamadı. PS5 açık mı, aynı ağda mı?");
	CHIAKI_LOGI(&g.log, "PS5 search: %s", found ? "found" : "not found");
	finding = 0;
	PostMessageW(g.hwnd, WM_APP_STATUS, 0, 0);
	return 0;
}

// Needs the console's rest mode to allow turning on over the network.
static bool ps5_wake_send(void)
{
	char key[CHIAKI_SESSION_AUTH_SIZE + 1], host[256];
	memcpy(key, g.regist_key, CHIAKI_SESSION_AUTH_SIZE);
	key[CHIAKI_SESSION_AUTH_SIZE] = 0;
	host_get(host, sizeof(host));
	ChiakiErrorCode err = chiaki_discovery_wakeup(&g.log, NULL, host, strtoull(key, NULL, 16), true);
	CHIAKI_LOGI(&g.log, "PS5 wake-up sent: %s", chiaki_error_string(err));
	return err == CHIAKI_ERR_SUCCESS;
}

static DWORD WINAPI ps5_wake_thread(void *arg)
{
	(void)arg;
	set_status(ps5_wake_send() ? L"Uyandırma sinyali gönderildi, PS5 açılıyor olabilir" : L"Uyandırma sinyali gönderilemedi");
	return 0;
}

static void find_ps5(void)
{
	if(InterlockedExchange(&finding, 1))
		return;
	CloseHandle(CreateThread(NULL, 0, find_ps5_thread, NULL, 0, NULL));
}

typedef struct {
	char data[65536];
	size_t size;
} HttpBody;

static size_t http_write(char *ptr, size_t size, size_t nmemb, void *user)
{
	HttpBody *body = user;
	size_t n = size * nmemb;
	if(body->size + n >= sizeof(body->data))
		return 0;
	memcpy(body->data + body->size, ptr, n);
	body->size += n;
	body->data[body->size] = 0;
	return n;
}

static bool psn_request(const char *url, const char *post, HttpBody *body)
{
	CURL *curl = curl_easy_init();
	if(!curl)
		return false;
	body->size = 0;
	body->data[0] = 0;
	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_USERPWD, PSN_CLIENT_ID ":" PSN_CLIENT_SECRET);
	curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS, (long)CURLSSLOPT_NATIVE_CA);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, http_write);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, body);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);
	if(post)
		curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post);
	CURLcode res = curl_easy_perform(curl);
	long status = 0;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
	curl_easy_cleanup(curl);
	// Failure bodies carry only Sony's error text; success bodies hold tokens and are never logged
	if(res != CURLE_OK)
		CHIAKI_LOGE(&g.log, "PSN %s failed: %s", post ? "token request" : "account request", curl_easy_strerror(res));
	else if(status != 200)
		CHIAKI_LOGE(&g.log, "PSN %s failed: HTTP %ld: %.300s", post ? "token request" : "account request", status, body->data);
	return res == CURLE_OK && status == 200;
}

static bool json_get(const char *json, const char *key, char *out, size_t size)
{
	struct json_object *root = json_tokener_parse(json);
	struct json_object *value;
	bool ok = root && json_object_object_get_ex(root, key, &value);
	if(ok)
		snprintf(out, size, "%s", json_object_get_string(value));
	json_object_put(root);
	return ok;
}

// Turns the address Sony redirects to after login into the account ID. The token is used once and never stored.
static bool account_id_from_url(const char *url, char *out, size_t out_size)
{
	const char *code = strstr(url, "code=");
	if(!code)
		return false;
	code += 5;
	size_t code_len = strcspn(code, "&#\"\\ ");
	static HttpBody body;
	char request[2048], token[1024], user_id[64];
	snprintf(request, sizeof(request), "grant_type=authorization_code&code=%.*s&scope=%s&redirect_uri=" PSN_REDIRECT_URL "&", (int)code_len, code, PSN_SCOPE);
	if(!psn_request(PSN_TOKEN_URL, request, &body) || !json_get(body.data, "access_token", token, sizeof(token)))
		return false;
	snprintf(request, sizeof(request), PSN_TOKEN_URL "/%s", token);
	if(!psn_request(request, NULL, &body) || !json_get(body.data, "user_id", user_id, sizeof(user_id)))
		return false;
	uint64_t id = strtoull(user_id, NULL, 10);
	if(!id)
		return false;
	return chiaki_base64_encode((const uint8_t *)&id, sizeof(id), out, out_size) == CHIAKI_ERR_SUCCESS;
}

static int version_number(const char *v)
{
	int a = 0, b = 0, c = 0;
	if(*v == 'v')
		v++;
	sscanf(v, "%d.%d.%d", &a, &b, &c);
	return a * 1000000 + b * 1000 + c;
}

// ---------- update ----------

// The file list of a release, and which of its files the update takes.
static void update_asset_find(const char *json, const char *tag)
{
	const char *prefix = UPDATE_DOWNLOAD_PREFIX;
#ifdef PSSTREAM_TEST
	static char test_prefix[256];
	if(GetEnvironmentVariableA("PSSTREAM_TEST_UPDATE_PREFIX", test_prefix, sizeof(test_prefix)))
		prefix = test_prefix;
#endif
	char wanted[96];
	snprintf(wanted, sizeof(wanted), "PS-Stream-%s-windows-x64.zip", tag);
	struct json_object *root = json_tokener_parse(json), *assets;
	if(root && json_object_object_get_ex(root, "assets", &assets) && json_object_is_type(assets, json_type_array))
	{
		for(size_t i = 0; i < json_object_array_length(assets); i++)
		{
			struct json_object *asset = json_object_array_get_idx(assets, i), *name, *url, *digest;
			if(!json_object_object_get_ex(asset, "name", &name) || !json_object_object_get_ex(asset, "browser_download_url", &url)
				|| !json_object_object_get_ex(asset, "digest", &digest) || strcmp(json_object_get_string(name), wanted) != 0)
				continue;
			const char *url_text = json_object_get_string(url), *digest_text = json_object_get_string(digest);
			if(digest_text && strncmp(url_text, prefix, strlen(prefix)) == 0 && strncmp(digest_text, "sha256:", 7) == 0 && strlen(digest_text) == 71)
			{
				snprintf(g.update_url, sizeof(g.update_url), "%s", url_text);
				snprintf(g.update_digest, sizeof(g.update_digest), "%s", digest_text + 7);
			}
		}
	}
	json_object_put(root);
}

// The folder holding the launcher and dist, or false when the app wasn't installed from the zip.
static bool update_install_root(wchar_t *root)
{
	GetModuleFileNameW(NULL, root, MAX_PATH);
	wchar_t *slash = wcsrchr(root, L'\\');
	if(!slash)
		return false;
	*slash = 0;
	slash = wcsrchr(root, L'\\');
	if(!slash || _wcsicmp(slash + 1, L"dist") != 0)
		return false;
	*slash = 0;
	wchar_t launcher[MAX_PATH + 32];
	swprintf(launcher, MAX_PATH + 32, L"%ls\\PS Stream.exe", root);
	return GetFileAttributesW(launcher) != INVALID_FILE_ATTRIBUTES;
}

static bool update_available_here(void)
{
	wchar_t root[MAX_PATH];
	return g.update_url[0] && !g.update_failed && update_install_root(root);
}

static void update_temp_dir(wchar_t *dir)
{
	wchar_t temp[MAX_PATH];
	GetTempPathW(MAX_PATH, temp);
	swprintf(dir, MAX_PATH, L"%lsPSStreamUpdate", temp);
}

static void dir_delete(const wchar_t *dir)
{
	wchar_t pattern[MAX_PATH + 8], path[MAX_PATH + 300];
	swprintf(pattern, MAX_PATH + 8, L"%ls\\*", dir);
	WIN32_FIND_DATAW found;
	HANDLE find = FindFirstFileW(pattern, &found);
	if(find == INVALID_HANDLE_VALUE)
		return;
	do
	{
		if(wcscmp(found.cFileName, L".") == 0 || wcscmp(found.cFileName, L"..") == 0)
			continue;
		swprintf(path, MAX_PATH + 300, L"%ls\\%ls", dir, found.cFileName);
		if((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(found.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
			dir_delete(path);
		else
			DeleteFileW(path);
	} while(FindNextFileW(find, &found));
	FindClose(find);
	RemoveDirectoryW(dir);
}

static bool file_sha256(const wchar_t *path, char *hex)
{
	FILE *f = _wfopen(path, L"rb");
	if(!f)
		return false;
	BCRYPT_ALG_HANDLE alg = NULL;
	BCRYPT_HASH_HANDLE hash = NULL;
	uint8_t digest[32];
	bool ok = BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0))
		&& BCRYPT_SUCCESS(BCryptCreateHash(alg, &hash, NULL, 0, NULL, 0, 0));
	static uint8_t chunk[65536];
	size_t n;
	while(ok && (n = fread(chunk, 1, sizeof(chunk), f)) > 0)
		ok = BCRYPT_SUCCESS(BCryptHashData(hash, chunk, (ULONG)n, 0));
	ok = ok && BCRYPT_SUCCESS(BCryptFinishHash(hash, digest, sizeof(digest), 0));
	if(hash)
		BCryptDestroyHash(hash);
	if(alg)
		BCryptCloseAlgorithmProvider(alg, 0);
	fclose(f);
	if(ok)
	{
		hex_encode(digest, sizeof(digest), hex);
		hex[64] = 0;
	}
	return ok;
}

static int update_progress_cb(void *user, curl_off_t total, curl_off_t now, curl_off_t ul_total, curl_off_t ul_now)
{
	(void)ul_total;
	(void)ul_now;
	int *last = user;
	int percent = total > 0 ? (int)(now * 100 / total) : 0;
	if(percent != *last)
	{
		*last = percent;
		wchar_t text[128];
		swprintf(text, 128, tr(L"Güncelleme indiriliyor... %%%d"), percent);
		set_status(text);
	}
	return g.quit != 0;
}

static void update_fail(const wchar_t *text, const char *why)
{
	CHIAKI_LOGE(&g.log, "Update failed: %s", why);
	wchar_t dir[MAX_PATH];
	update_temp_dir(dir);
	dir_delete(dir);
	g.update_failed = 1;
	g.update_busy = 0;
	set_status(text);
}

static DWORD WINAPI update_download_main(void *arg)
{
	(void)arg;
	wchar_t dir[MAX_PATH], zip[MAX_PATH + 16], command[MAX_PATH * 3], tar[MAX_PATH], staged[MAX_PATH + 48];
	update_temp_dir(dir);
	dir_delete(dir);
	CreateDirectoryW(dir, NULL);
	swprintf(zip, MAX_PATH + 16, L"%ls\\update.zip", dir);
	FILE *f = _wfopen(zip, L"wb");
	CURL *curl = f ? curl_easy_init() : NULL;
	if(!curl)
	{
		if(f)
			fclose(f);
		update_fail(L"Güncelleme indirilemedi. Yeni sürümü sayfasından indirebilirsin", "couldn't create the temp file");
		return 0;
	}
	int last_percent = -1;
	curl_easy_setopt(curl, CURLOPT_URL, g.update_url);
	curl_easy_setopt(curl, CURLOPT_USERAGENT, "PS-Stream/" APP_VERSION);
	curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS, (long)CURLSSLOPT_NATIVE_CA);
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 20L);
	curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);
	curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 60L);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, f);
	curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
	curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, update_progress_cb);
	curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &last_percent);
#ifndef PSSTREAM_TEST
	curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
	curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https");
#endif
	CURLcode res = curl_easy_perform(curl);
	curl_easy_cleanup(curl);
	bool written = fclose(f) == 0;
	if(res != CURLE_OK || !written)
	{
		CHIAKI_LOGE(&g.log, "Update download: %s", curl_easy_strerror(res));
		update_fail(L"Güncelleme indirilemedi. Yeni sürümü sayfasından indirebilirsin", "download");
		return 0;
	}

	set_status(L"Güncelleme hazırlanıyor...");
	char hex[65];
	if(!file_sha256(zip, hex) || _stricmp(hex, g.update_digest) != 0)
	{
		update_fail(L"Güncelleme dosyası doğrulanamadı. Yeni sürümü sayfasından indirebilirsin", "checksum mismatch");
		return 0;
	}

	GetSystemDirectoryW(tar, MAX_PATH);
	wcsncat(tar, L"\\tar.exe", MAX_PATH - wcslen(tar) - 1);
	swprintf(command, MAX_PATH * 3, L"\"%ls\" -xf \"%ls\" -C \"%ls\"", tar, zip, dir);
	STARTUPINFOW si = { 0 };
	si.cb = sizeof(si);
	PROCESS_INFORMATION pi;
	DWORD code = 1;
	if(CreateProcessW(tar, command, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi))
	{
		WaitForSingleObject(pi.hProcess, 120000);
		GetExitCodeProcess(pi.hProcess, &code);
		CloseHandle(pi.hProcess);
		CloseHandle(pi.hThread);
	}
	swprintf(staged, MAX_PATH + 48, L"%ls\\PS Stream\\dist\\PS Stream.exe", dir);
	if(code != 0 || GetFileAttributesW(staged) == INVALID_FILE_ATTRIBUTES)
	{
		update_fail(L"Güncelleme dosyası doğrulanamadı. Yeni sürümü sayfasından indirebilirsin", "unpacking");
		return 0;
	}
	CHIAKI_LOGI(&g.log, "Update %s ready, restarting", g.update_tag);
	// The rest happens after a normal exit, so the PS5 is told we left and settings are saved
	g.update_restart = 1;
	PostMessageW(g.hwnd, WM_CLOSE, 0, 0);
	return 0;
}

static void update_begin(void)
{
	if(InterlockedExchange(&g.update_busy, 1))
		return;
	set_status(L"Güncelleme indiriliyor...");
	CloseHandle(CreateThread(NULL, 0, update_download_main, NULL, 0, NULL));
}

#define UPDATE_MAX_FILES 512
#define UPDATE_OLD_SUFFIX L".ps-old"

typedef struct { wchar_t target[MAX_PATH]; bool had_old, placed; } UpdateMove;
static UpdateMove update_moves[UPDATE_MAX_FILES];
static int update_move_count;

// OneDrive and antivirus programs hold new files open for a moment
static bool move_retry(const wchar_t *from, const wchar_t *to, DWORD flags)
{
	for(int i = 0; i < 20; i++)
	{
		if(MoveFileExW(from, to, flags))
			return true;
		Sleep(250);
	}
	return false;
}

static bool update_swap_folder(const wchar_t *from, const wchar_t *to)
{
	wchar_t pattern[MAX_PATH + 8], src[MAX_PATH + 300], old[MAX_PATH + 16];
	swprintf(pattern, MAX_PATH + 8, L"%ls\\*", from);
	WIN32_FIND_DATAW found;
	HANDLE find = FindFirstFileW(pattern, &found);
	if(find == INVALID_HANDLE_VALUE)
		return false;
	bool ok = true;
	do
	{
		if(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
			continue;
		if(update_move_count >= UPDATE_MAX_FILES)
		{
			ok = false;
			break;
		}
		UpdateMove *m = &update_moves[update_move_count];
		swprintf(src, MAX_PATH + 300, L"%ls\\%ls", from, found.cFileName);
		if(swprintf(m->target, MAX_PATH, L"%ls\\%ls", to, found.cFileName) < 0)
		{
			ok = false;
			break;
		}
		swprintf(old, MAX_PATH + 16, L"%ls" UPDATE_OLD_SUFFIX, m->target);
		m->had_old = false;
		m->placed = false;
		update_move_count++;
		if(GetFileAttributesW(m->target) != INVALID_FILE_ATTRIBUTES)
		{
			// A running program's files can't be overwritten, but they can be renamed
			if(!move_retry(m->target, old, MOVEFILE_REPLACE_EXISTING))
			{
				ok = false;
				break;
			}
			m->had_old = true;
		}
		if(!move_retry(src, m->target, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED))
		{
			ok = false;
			break;
		}
		m->placed = true;
	} while(FindNextFileW(find, &found));
	FindClose(find);
	return ok;
}

static void update_roll_back(void)
{
	wchar_t old[MAX_PATH + 16];
	for(int i = update_move_count - 1; i >= 0; i--)
	{
		UpdateMove *m = &update_moves[i];
		swprintf(old, MAX_PATH + 16, L"%ls" UPDATE_OLD_SUFFIX, m->target);
		if(m->placed)
			DeleteFileW(m->target);
		if(m->had_old)
			move_retry(old, m->target, MOVEFILE_REPLACE_EXISTING);
	}
}

static bool update_launch(const wchar_t *exe, const wchar_t *arg, PROCESS_INFORMATION *pi)
{
	wchar_t command[MAX_PATH + 64];
	swprintf(command, MAX_PATH + 64, L"\"%ls\" %ls", exe, arg);
	STARTUPINFOW si = { 0 };
	si.cb = sizeof(si);
	return CreateProcessW(exe, command, NULL, NULL, FALSE, 0, NULL, NULL, &si, pi);
}

// Last thing the old version does. The new one has to reach its window, or the old files go back.
static void update_finish(void)
{
	wchar_t root[MAX_PATH], exe[MAX_PATH + 48], dir[MAX_PATH], staged[MAX_PATH + 32], staged_dist[MAX_PATH + 32], dist[MAX_PATH + 16];
	if(!update_install_root(root))
		return;
	swprintf(exe, MAX_PATH + 48, L"%ls\\dist\\PS Stream.exe", root);
	swprintf(dist, MAX_PATH + 16, L"%ls\\dist", root);
	update_temp_dir(dir);
	swprintf(staged, MAX_PATH + 32, L"%ls\\PS Stream", dir);
	swprintf(staged_dist, MAX_PATH + 32, L"%ls\\PS Stream\\dist", dir);

	if(g.single_instance)
		CloseHandle(g.single_instance);
	HANDLE ready = CreateEventW(NULL, TRUE, FALSE, L"PSStreamUpdated");
	// The new version takes over the log file
	if(g.log_file)
		fclose(g.log_file);
	g.log_file = NULL;

	PROCESS_INFORMATION pi;
	bool ok = update_swap_folder(staged, root) && update_swap_folder(staged_dist, dist) && update_launch(exe, L"--updated", &pi);
	if(ok)
	{
		HANDLE waits[2] = { ready, pi.hProcess };
		ok = WaitForMultipleObjects(2, waits, FALSE, 60000) == WAIT_OBJECT_0;
		if(!ok)
		{
			TerminateProcess(pi.hProcess, 1);
			WaitForSingleObject(pi.hProcess, 5000);
		}
		CloseHandle(pi.hProcess);
		CloseHandle(pi.hThread);
	}
	if(!ok)
	{
		update_roll_back();
		if(update_launch(exe, L"--update-failed", &pi))
		{
			CloseHandle(pi.hProcess);
			CloseHandle(pi.hThread);
		}
	}
}

// Clears what the previous version left behind. It may still be closing, so this tries for a while.
static DWORD WINAPI update_cleanup_main(void *arg)
{
	(void)arg;
	wchar_t root[MAX_PATH], dir[MAX_PATH];
	bool installed = update_install_root(root);
	for(int attempt = 0; attempt < 6 && !g.quit; attempt++)
	{
		if(attempt)
			Sleep(2000);
		bool left = false;
		for(int folder = 0; folder < 2 && installed; folder++)
		{
			wchar_t base[MAX_PATH + 16], pattern[MAX_PATH + 32], path[MAX_PATH + 300];
			swprintf(base, MAX_PATH + 16, folder ? L"%ls\\dist" : L"%ls", root);
			swprintf(pattern, MAX_PATH + 32, L"%ls\\*" UPDATE_OLD_SUFFIX, base);
			WIN32_FIND_DATAW found;
			HANDLE find = FindFirstFileW(pattern, &found);
			if(find == INVALID_HANDLE_VALUE)
				continue;
			do
			{
				swprintf(path, MAX_PATH + 300, L"%ls\\%ls", base, found.cFileName);
				if(!DeleteFileW(path))
					left = true;
			} while(FindNextFileW(find, &found));
			FindClose(find);
		}
		if(!left)
			break;
	}
	// An update in progress uses this folder, but one can't have started this early
	if(!g.update_busy)
	{
		update_temp_dir(dir);
		dir_delete(dir);
	}
	return 0;
}

// Only asks GitHub for the latest version number. Nothing about the user is sent, nothing is downloaded.
static DWORD WINAPI update_check_main(void *arg)
{
	(void)arg;
	const char *current = APP_VERSION;
#ifdef PSSTREAM_TEST
	static char test_version[16];
	if(GetEnvironmentVariableA("PSSTREAM_TEST_VERSION", test_version, sizeof(test_version)))
		current = test_version;
#endif
	static HttpBody body;
	body.size = 0;
	body.data[0] = 0;
	CURL *curl = curl_easy_init();
	if(!curl)
		return 0;
	const char *api_url = UPDATE_API_URL;
#ifdef PSSTREAM_TEST
	static char test_api[256];
	if(GetEnvironmentVariableA("PSSTREAM_TEST_UPDATE_API", test_api, sizeof(test_api)))
		api_url = test_api;
#endif
	curl_easy_setopt(curl, CURLOPT_URL, api_url);
	curl_easy_setopt(curl, CURLOPT_USERAGENT, "PS-Stream/" APP_VERSION);
	curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS, (long)CURLSSLOPT_NATIVE_CA);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, http_write);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
	CURLcode res = curl_easy_perform(curl);
	curl_easy_cleanup(curl);
	char tag[32] = "";
	if(res == CURLE_OK && json_get(body.data, "tag_name", tag, sizeof(tag)) && version_number(tag) > version_number(current))
	{
		update_asset_find(body.data, tag);
		snprintf(g.update_tag, sizeof(g.update_tag), "%s", tag);
		g.update_ready = 1;
#ifdef PSSTREAM_TEST
		if(!g.relaunched && !g.update_failed && GetEnvironmentVariableA("PSSTREAM_TEST_AUTOUPDATE", NULL, 0))
			update_begin();
#endif
		PostMessageW(g.hwnd, WM_APP_STATUS, 0, 0);
	}
	CHIAKI_LOGI(&g.log, "Update check: current %s, latest %s (%s)", current, tag[0] ? tag : "unknown", curl_easy_strerror(res));
	return 0;
}

static char login_account_id[64];

// A brand-new browser profile leaves Sony's page blank, so sign-in happens in the user's own browser.
// The address Sony redirects to is read from the browser's address bar, or from the clipboard if
// the user copies it. Nothing else read here is kept, and this only runs while waiting for sign-in.
#define REDIRECT_MARK L"remoteplay/redirect?code="

static const CLSID clsid_uia = { 0xff48dba4, 0x60ef, 0x4201, { 0xaa, 0x87, 0x54, 0x10, 0x3e, 0xef, 0x59, 0x4e } };
static const IID iid_uia = { 0x30cbe57d, 0xd9d0, 0x452a, { 0xab, 0x13, 0x7a, 0xc5, 0xac, 0x48, 0x25, 0xee } };
enum { UIA_EDIT_TYPE = 50004, UIA_CONTROL_TYPE_PROP = 30003, UIA_VALUE_PROP = 30045 };

typedef struct {
	IUIAutomation *uia;
	IUIAutomationCondition *edit;
	wchar_t found[1024];
} AddressScan;

static bool is_browser_window(HWND h)
{
	if(!IsWindowVisible(h))
		return false;
	wchar_t cls[64];
	GetClassNameW(h, cls, 64);
	if(wcscmp(cls, L"Chrome_WidgetWin_1") != 0 && wcscmp(cls, L"MozillaWindowClass") != 0)
		return false;
	// Discord and other Electron apps share Chrome's window class
	DWORD pid = 0;
	GetWindowThreadProcessId(h, &pid);
	HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
	if(!process)
		return false;
	wchar_t path[MAX_PATH];
	DWORD size = MAX_PATH;
	bool ok = QueryFullProcessImageNameW(process, 0, path, &size) != 0;
	CloseHandle(process);
	if(!ok)
		return false;
	const wchar_t *name = wcsrchr(path, L'\\');
	name = name ? name + 1 : path;
	static const wchar_t *const browsers[] = { L"chrome.exe", L"msedge.exe", L"firefox.exe", L"brave.exe", L"opera.exe", L"vivaldi.exe" };
	for(size_t i = 0; i < sizeof(browsers) / sizeof(browsers[0]); i++)
		if(_wcsicmp(name, browsers[i]) == 0)
			return true;
	return false;
}

static BOOL CALLBACK address_scan_window(HWND h, LPARAM param)
{
	AddressScan *scan = (AddressScan *)param;
	if(!is_browser_window(h))
		return TRUE;
	IUIAutomationElement *root = NULL, *edit = NULL;
	if(IUIAutomation_ElementFromHandle(scan->uia, h, &root) == S_OK && root)
	{
		if(IUIAutomationElement_FindFirst(root, TreeScope_Descendants, scan->edit, &edit) == S_OK && edit)
		{
			VARIANT value;
			VariantInit(&value);
			if(IUIAutomationElement_GetCurrentPropertyValue(edit, UIA_VALUE_PROP, &value) == S_OK && value.vt == VT_BSTR && value.bstrVal
				&& wcsstr(value.bstrVal, REDIRECT_MARK))
				wcsncpy(scan->found, wcsstr(value.bstrVal, REDIRECT_MARK), 1023);
			VariantClear(&value);
			IUIAutomationElement_Release(edit);
		}
		IUIAutomationElement_Release(root);
	}
	return scan->found[0] ? FALSE : TRUE;
}

static void clipboard_redirect(wchar_t *out, size_t size)
{
	out[0] = 0;
	if(!OpenClipboard(NULL))
		return;
	HANDLE data = GetClipboardData(CF_UNICODETEXT);
	const wchar_t *text = data ? GlobalLock(data) : NULL;
	if(text)
	{
		const wchar_t *mark = wcsstr(text, REDIRECT_MARK);
		if(mark)
		{
			wcsncpy(out, mark, size - 1);
			out[size - 1] = 0;
		}
		GlobalUnlock(data);
	}
	CloseClipboard();
}

static volatile LONG login_cancel;

static DWORD WINAPI login_main(void *arg)
{
	(void)arg;
	const wchar_t *login_url = PSN_LOGIN_URL;
	bool test_mode = false;
#ifdef PSSTREAM_TEST
	static wchar_t test_url[512];
	if(GetEnvironmentVariableW(L"PSSTREAM_TEST_LOGIN_URL", test_url, 512))
	{
		login_url = test_url;
		test_mode = true;
	}
#endif
	CoInitializeEx(NULL, COINIT_MULTITHREADED);
	AddressScan scan = { 0 };
	if(CoCreateInstance(&clsid_uia, NULL, CLSCTX_INPROC_SERVER, &iid_uia, (void **)&scan.uia) == S_OK)
	{
		VARIANT type;
		VariantInit(&type);
		type.vt = VT_I4;
		type.lVal = UIA_EDIT_TYPE;
		IUIAutomation_CreatePropertyCondition(scan.uia, UIA_CONTROL_TYPE_PROP, type, &scan.edit);
	}
	else
		CHIAKI_LOGW(&g.log, "Address bar reading unavailable, waiting for the clipboard");

	// Anything already showing a redirect is an old, used code
	wchar_t stale_address[1024] = L"", stale_clip[1024];
	if(scan.edit)
	{
		EnumWindows(address_scan_window, (LPARAM)&scan);
		wcscpy(stale_address, scan.found);
	}
	clipboard_redirect(stale_clip, 1024);

	ShellExecuteW(NULL, L"open", login_url, NULL, NULL, SW_SHOWNORMAL);

	wchar_t redirect[1024] = L"";
	const char *source = "";
	DWORD start = GetTickCount();
	login_cancel = 0;
	while(!g.quit && !login_cancel && !redirect[0] && GetTickCount() - start < LOGIN_TIMEOUT_MS)
	{
		Sleep(700);
		wchar_t clip[1024];
		clipboard_redirect(clip, 1024);
		if(clip[0] && wcscmp(clip, stale_clip) != 0)
		{
			wcscpy(redirect, clip);
			source = "clipboard";
			break;
		}
		if(scan.edit)
		{
			scan.found[0] = 0;
			EnumWindows(address_scan_window, (LPARAM)&scan);
			if(scan.found[0] && wcscmp(scan.found, stale_address) != 0)
			{
				wcscpy(redirect, scan.found);
				source = "address bar";
			}
		}
	}
	if(scan.edit)
		IUIAutomationCondition_Release(scan.edit);
	if(scan.uia)
		IUIAutomation_Release(scan.uia);
	CoUninitialize();

	if(!redirect[0])
	{
		PostMessageW(g.hwnd, WM_APP_LOGIN, 3, 0);
		return 0;
	}
	CHIAKI_LOGI(&g.log, "Sign-in detected from the %s", source);
	char redirect_utf8[2048];
	WideCharToMultiByte(CP_UTF8, 0, redirect, -1, redirect_utf8, sizeof(redirect_utf8), NULL, NULL);
	if(test_mode)
	{
		snprintf(login_account_id, sizeof(login_account_id), "TEST");
		PostMessageW(g.hwnd, WM_APP_LOGIN, 1, 0);
		return 0;
	}
	bool ok = account_id_from_url(redirect_utf8, login_account_id, sizeof(login_account_id));
	PostMessageW(g.hwnd, WM_APP_LOGIN, ok ? 1 : 0, 0);
	return 0;
}

static char pasted_redirect[1024];

// For a redirect address pasted from the user's own browser.
static DWORD WINAPI redirect_exchange_main(void *arg)
{
	(void)arg;
	bool ok = account_id_from_url(pasted_redirect, login_account_id, sizeof(login_account_id));
	PostMessageW(g.hwnd, WM_APP_LOGIN, ok ? 1 : 0, 0);
	return 0;
}

static void regist_cb(ChiakiRegistEvent *event, void *user)
{
	(void)user;
	g.regist_ok = event->type == CHIAKI_REGIST_EVENT_TYPE_FINISHED_SUCCESS;
	if(g.regist_ok)
		g.registered = *event->registered_host;
	PostMessageW(g.hwnd, WM_APP_PAIRED, 0, 0);
}

static void pair_start(void)
{
	char account[1024], pin[32], host[256];
	get_text(ID_HOST, host, sizeof(host));
	get_text(ID_ACCOUNT, account, sizeof(account));
	get_text(ID_PIN, pin, sizeof(pin));
	if(!host[0])
	{
		set_status(L"PS5 IP adresini gir veya Bul'a bas");
		return;
	}
	host_set(host);
	if(strstr(account, "code="))
	{
		set_status(L"Hesap bilgisi alınıyor...");
		return;
	}

	if(!account[0])
	{
		set_status(L"Önce PSN ile giriş yap");
		return;
	}

	ChiakiRegistInfo info;
	memset(&info, 0, sizeof(info));
	size_t id_size = sizeof(info.psn_account_id);
	if(chiaki_base64_decode(account, strlen(account), info.psn_account_id, &id_size) != CHIAKI_ERR_SUCCESS || id_size != sizeof(info.psn_account_id))
	{
		set_status(L"Hesap ID geçersiz");
		return;
	}
	if(strlen(pin) != 8)
	{
		set_status(L"PS5'teki 8 haneli kodu gir");
		return;
	}
	info.target = CHIAKI_TARGET_PS5_1;
	info.host = g.host;
	info.pin = (uint32_t)strtoul(pin, NULL, 10);
	g.regist_rejected = 0;
	if(chiaki_regist_start(&g.regist, &g.log, &info, regist_cb, NULL) != CHIAKI_ERR_SUCCESS)
	{
		set_status(L"Eşleştirme başlatılamadı");
		return;
	}
	g.pairing = true;
	set_status(L"Eşleştiriliyor...");
}

static void show_page(int page);

static void pair_finished(void)
{
	chiaki_regist_fini(&g.regist);
	g.pairing = false;
	if(!g.regist_ok)
	{
		if(g.regist_rejected)
		{
			g.regist_tries = 0;
			set_status(L"PS5 bu kodu kabul etmedi. PS5'teki Cihazı Bağla ekranından yeni kod alıp yaz");
			SetDlgItemTextW(g.hwnd, ID_PIN, L"");
			return;
		}
		// Pairing starts with one network message and no second try, so one lost packet ends it.
		// The code is still good, and typing it again wouldn't help.
		if(++g.regist_tries < REGIST_TRIES)
		{
			CHIAKI_LOGI(&g.log, "Pairing attempt %d got no answer, trying again", g.regist_tries);
			g.pairing = true;
			set_status(L"PS5'e ulaşılmaya çalışılıyor...");
			SetTimer(g.hwnd, TIMER_REGIST_RETRY, REGIST_RETRY_MS, NULL);
			return;
		}
		g.regist_tries = 0;
		set_status(L"PS5'e ulaşılamadı. PS5 açık mı, IP doğru mu? Kod duruyor, Eşleştir'e basabilirsin");
		return;
	}
	g.regist_tries = 0;
	memcpy(g.regist_key, g.registered.rp_regist_key, sizeof(g.regist_key));
	memcpy(g.morning, g.registered.rp_key, sizeof(g.morning));
	g.paired = true;
	char hex[64];
	config_set("host", g.host);
	hex_encode((const uint8_t *)g.regist_key, sizeof(g.regist_key), hex);
	config_set("regist_key", hex);
	hex_encode(g.morning, sizeof(g.morning), hex);
	config_set("morning", hex);
	SetDlgItemTextW(g.hwnd, ID_PIN, L"");
	show_page(PAGE_PLAY);
	set_status(L"Eşleştirildi. Ses veya Görüntü'ye basabilirsin");
	if(g.settings.autostart_audio)
	{
		g.want_audio = 1;
		SetEvent(g.wake);
	}
}

#include "ui.c"
