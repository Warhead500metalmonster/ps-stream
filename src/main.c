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
	WM_APP_STATUS = WM_APP + 1, WM_APP_VIDEO_CLOSED, WM_APP_PAIRED, WM_APP_ALL_OFF, WM_APP_TEST_SHOT, WM_APP_LOGIN
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
	int volume, buffer_ms, loss_pct, reconnect, no_controller, wifi_opt, autostart_audio;
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
	char config_path[MAX_PATH];
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
	DWORD audio_last_dropout;
	HANDLE wlan;
	int16_t *volume_buf;
	size_t volume_buf_size;

	ChiakiFfmpegDecoder decoder;
	HANDLE video_thread, video_ready;
	volatile LONG video_stop, frame_pending;
} g;

static int S(int v) { return MulDiv(v, g.dpi, 96); }

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
	(void)level;
	(void)user;
	if(!g.log_file)
		return;
	SYSTEMTIME t;
	GetLocalTime(&t);
	fprintf(g.log_file, "%02d:%02d:%02d %s\n", t.wHour, t.wMinute, t.wSecond, msg);
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
	if(InterlockedExchange(&g.audio_reopen, 0) && g.audio_rate)
		audio_open();
	if(!g.audio_dev || !g.want_audio)
		return;
	Settings s = settings_get();
	size_t count = samples_count * g.audio_channels;
	if(s.volume != 100)
	{
		if(count > g.volume_buf_size)
		{
			g.volume_buf = realloc(g.volume_buf, count * sizeof(int16_t));
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
	SDL_Window *window = SDL_CreateWindow("PS Stream - Goruntu", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
			S(960), S(540), SDL_WINDOW_RESIZABLE | (g.test_hidden ? SDL_WINDOW_HIDDEN : 0));
	SDL_Renderer *renderer = window ? SDL_CreateRenderer(window, -1, 0) : NULL;
	if(!renderer)
		CHIAKI_LOGE(&g.log, "SDL window failed: %s", SDL_GetError());
	SetEvent(g.video_ready);

	SDL_Texture *texture = NULL;
	int tex_w = 0, tex_h = 0, tex_format = AV_PIX_FMT_NONE;
	AVFrame *sw_frame = av_frame_alloc();
	bool dirty = true;
	double stat_pull = 0, stat_copy = 0, stat_upload = 0, stat_present = 0, stat_start = now_ms();
	int stat_frames = 0, stat_lost = 0;
	while(renderer && !g.video_stop)
	{
		SDL_Event e;
		if(SDL_WaitEventTimeout(&e, 100))
		{
			do
			{
				if(e.type == SDL_QUIT)
					PostMessageW(g.hwnd, WM_APP_VIDEO_CLOSED, 0, 0);
				else if(e.type == SDL_WINDOWEVENT)
					dirty = true;
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
		if(now_ms() - stat_start >= 5000 && stat_frames)
		{
			CHIAKI_LOGI(&g.log, "Video stats: %.1f fps, lost %d, per frame ms: pull %.1f, copy %.1f, upload %.1f, present %.1f",
					stat_frames * 1000.0 / (now_ms() - stat_start), stat_lost, stat_pull / stat_frames, stat_copy / stat_frames, stat_upload / stat_frames, stat_present / stat_frames);
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
		SDL_DestroyWindow(window);
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
			g.connected = 1;
			SetEvent(g.wake);
			set_status(g.want_video ? L"Bağlı - görüntü açık" : L"Bağlı - sadece ses, görüntü çözülmüyor");
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

static bool session_start(bool video, const Settings *s, bool safe)
{
	ChiakiCodec codec = s->h265 ? CHIAKI_CODEC_H265 : CHIAKI_CODEC_H264;
	ChiakiConnectInfo info;
	memset(&info, 0, sizeof(info));
	info.ps5 = true;
	info.host = g.host;
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
		SDL_ClearQueuedAudio(g.audio_dev);
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
			return L"PS5 kapandı";
		case CHIAKI_QUIT_REASON_SESSION_REQUEST_CONNECTION_REFUSED:
		case CHIAKI_QUIT_REASON_CTRL_CONNECTION_REFUSED:
			return L"PS5 bağlantıyı reddetti, tekrar deneniyor...";
		default:
			return L"Bağlantı yok, tekrar deneniyor...";
	}
}

static bool quit_reason_is_final(ChiakiQuitReason reason)
{
	return reason == CHIAKI_QUIT_REASON_SESSION_REQUEST_RP_VERSION_MISMATCH
		|| reason == CHIAKI_QUIT_REASON_STREAM_CONNECTION_REMOTE_SHUTDOWN;
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
	bool running = false, run_video = false, safe = false, waiting = false;
	Settings run = { 0 };
	DWORD retry_at = 0;
	while(true)
	{
		DWORD timeout = INFINITE;
		if(waiting)
		{
			LONG left = (LONG)(retry_at - GetTickCount());
			timeout = left > 0 ? (DWORD)left : 0;
		}
		WaitForSingleObject(g.wake, timeout);

		bool want_video = g.want_video != 0;
		bool want_any = !g.quit && (g.want_audio || want_video);
		Settings s = settings_get();
		// Only while connected, so a PS5 that's off doesn't keep the PC awake or its Wi-Fi tuned
		bool live = want_any && g.connected;
		wifi_tune(live && s.wifi_opt);
		// The PC often sits untouched while the game plays on the TV
		SetThreadExecutionState(ES_CONTINUOUS | (live ? ES_SYSTEM_REQUIRED : 0) | (live && want_video ? ES_DISPLAY_REQUIRED : 0));

		if(running)
		{
			bool ended = InterlockedExchange(&g.ended, 0) != 0;
			if(ended || !want_any || want_video != run_video || stream_changed(&s, &run, run_video))
			{
				bool was_connected = g.connected != 0;
				ChiakiQuitReason reason = g.quit_reason;
				session_stop(run_video);
				running = false;
				waiting = true;
				retry_at = GetTickCount() + (ended ? RETRY_MS : RECONNECT_MS);
				if(ended && want_any)
				{
					// "In use" only means the console has not released the last session yet
					if(!was_connected && reason != CHIAKI_QUIT_REASON_SESSION_REQUEST_RP_IN_USE)
						safe = true;
					if(g.fatal || quit_reason_is_final(reason) || !s.reconnect)
					{
						if(!g.fatal)
							set_status(quit_reason_is_final(reason) ? quit_reason_text(reason) : L"Bağlantı koptu");
						g.fatal = 0;
						PostMessageW(g.hwnd, WM_APP_ALL_OFF, 0, 0);
						waiting = false;
						continue;
					}
					set_status(quit_reason_text(reason));
				}
				else if(want_any)
					set_status(L"Yeni ayarlarla yeniden bağlanıyor...");
			}
		}
		if(g.quit)
			break;
		if(!want_any)
		{
			if(waiting || !running)
				set_status(g.paired ? L"Kapalı. Başlamak için Ses veya Görüntü'ye bas" : L"Önce Eşleştirme sayfasından PS5'i bağla");
			waiting = false;
			safe = false;
			continue;
		}
		if(!running && (!waiting || (LONG)(retry_at - GetTickCount()) <= 0))
		{
			waiting = false;
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

static void config_get(const char *key, char *buf, DWORD size)
{
	GetPrivateProfileStringA("ps5", key, "", buf, size, g.config_path);
}

static void config_set(const char *key, const char *value)
{
	WritePrivateProfileStringA("ps5", key, value, g.config_path);
}

static int config_int(const char *key, int fallback, int min, int max)
{
	int v = GetPrivateProfileIntA("settings", key, fallback, g.config_path);
	return v < min || v > max ? fallback : v;
}

static void config_set_int(const char *key, int value)
{
	char v[16];
	snprintf(v, sizeof(v), "%d", value);
	WritePrivateProfileStringA("settings", key, v, g.config_path);
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
	s->hotkey_audio = config_int("hotkey_audio", MAKEWORD(VK_F9, HOTKEYF_CONTROL | HOTKEYF_ALT), 0, 0xffff);
	s->hotkey_video = config_int("hotkey_video", MAKEWORD(VK_F10, HOTKEYF_CONTROL | HOTKEYF_ALT), 0, 0xffff);
	GetPrivateProfileStringA("settings", "device", "", s->device, sizeof(s->device), g.config_path);
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
	config_set_int("hotkey_audio", s->hotkey_audio);
	config_set_int("hotkey_video", s->hotkey_video);
	WritePrivateProfileStringA("settings", "device", s->device, g.config_path);
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
	SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if(s == INVALID_SOCKET)
	{
		finding = 0;
		return 1;
	}
	BOOL on = TRUE;
	DWORD timeout = 700;
	setsockopt(s, SOL_SOCKET, SO_BROADCAST, (char *)&on, sizeof(on));
	setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char *)&timeout, sizeof(timeout));

	uint32_t targets[17];
	int target_count = 0;
	targets[target_count++] = INADDR_BROADCAST;
	static char table_buf[4096];
	MIB_IPADDRTABLE *table = (MIB_IPADDRTABLE *)table_buf;
	ULONG table_size = sizeof(table_buf);
	if(GetIpAddrTable(table, &table_size, FALSE) == NO_ERROR)
		for(DWORD i = 0; i < table->dwNumEntries && target_count < 16; i++)
			if(table->table[i].dwAddr && table->table[i].dwAddr != htonl(INADDR_LOOPBACK))
				targets[target_count++] = table->table[i].dwAddr | ~table->table[i].dwMask;
	char typed[64];
	get_text(ID_HOST, typed, sizeof(typed));
	uint32_t typed_addr = inet_addr(typed);
	if(typed[0] && typed_addr != INADDR_NONE)
		targets[target_count++] = typed_addr;

	static const char msg[] = "SRCH * HTTP/1.1\ndevice-discovery-protocol-version:00030010\n";
	set_status(L"PS5 aranıyor...");
	bool found = false;
	for(int round = 0; round < 12 && !found && !g.quit; round++)
	{
		for(int i = 0; i < target_count; i++)
		{
			struct sockaddr_in to = { 0 };
			to.sin_family = AF_INET;
			to.sin_port = htons(9302);
			to.sin_addr.s_addr = targets[i];
			sendto(s, msg, sizeof(msg) - 1, 0, (struct sockaddr *)&to, sizeof(to));
		}
		char buf[1024];
		struct sockaddr_in from;
		int from_len = sizeof(from);
		int n = recvfrom(s, buf, sizeof(buf) - 1, 0, (struct sockaddr *)&from, &from_len);
		if(n <= 0)
			continue;
		buf[n] = 0;
		if(!strstr(buf, "host-type:PS5"))
			continue;
		found = true;
		SetDlgItemTextA(g.hwnd, ID_HOST, inet_ntoa(from.sin_addr));
		set_status(strstr(buf, "HTTP/1.1 620") ? L"PS5 bulundu ama uyku modunda, PS5'i aç" : L"PS5 bulundu");
	}
	closesocket(s);
	if(!found)
		set_status(L"PS5 bulunamadı. PS5 açık mı, aynı ağda mı?");
	CHIAKI_LOGI(&g.log, "PS5 search: %s, tried %d addresses", found ? "found" : "not found", target_count);
	finding = 0;
	PostMessageW(g.hwnd, WM_APP_STATUS, 0, 0);
	return 0;
}

static void find_ps5(void)
{
	if(InterlockedExchange(&finding, 1))
		return;
	CloseHandle(CreateThread(NULL, 0, find_ps5_thread, NULL, 0, NULL));
}

typedef struct {
	char data[16384];
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
	return chiaki_base64_encode((const uint8_t *)&id, sizeof(id), out, out_size) == CHIAKI_ERR_SUCCESS;
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
	char account[1024], pin[32];
	get_text(ID_HOST, g.host, sizeof(g.host));
	get_text(ID_ACCOUNT, account, sizeof(account));
	get_text(ID_PIN, pin, sizeof(pin));
	if(!g.host[0])
	{
		set_status(L"PS5 IP adresini gir veya Bul'a bas");
		return;
	}
	if(strstr(account, "code="))
	{
		SetCursor(LoadCursor(NULL, IDC_WAIT));
		char id[64];
		if(!account_id_from_url(account, id, sizeof(id)))
		{
			set_status(L"Sony'den hesap ID alınamadı, girişi tekrar dene");
			return;
		}
		SetDlgItemTextA(g.hwnd, ID_ACCOUNT, id);
		snprintf(account, sizeof(account), "%s", id);
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
		set_status(L"Eşleştirme olmadı. Kodu ve IP'yi kontrol et");
		return;
	}
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
}

#include "ui.c"
