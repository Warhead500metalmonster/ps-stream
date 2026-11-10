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
#include <chiaki/controller.h>
#include <chiaki/discovery.h>
#include <chiaki/ffmpegdecoder.h>
#include <chiaki/log.h>
#include <chiaki/opusdecoder.h>
#include <chiaki/regist.h>
#include <chiaki/session.h>

#include <SDL.h>
#include <SDL_syswm.h>
#include <curl/curl.h>
#include <json-c/json.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <d3d11_1.h>
#include <dxgi.h>

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
#define APP_VERSION "0.1.39"
#define UPDATE_RECHECK_MS (6 * 60 * 60 * 1000)
#define UPDATE_TRIES 6
#define UPDATE_RETRY_MS 20000
// New versions install themselves, but never in the middle of a game: only this soon after the
// app opens, or once audio and video are both off. A version that failed this often waits for Update.
#define UPDATE_AUTO_GRACE_MS 120000
#define UPDATE_AUTO_MAX_FAILS 3
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
// A resting PS5 answers the search at once, so without this the checks ran about 5 times a second,
// each with a broadcast. Waking takes 10 s or more anyway.
#define PROBE_STANDBY_DELAY_MS 1000
// Asking every address on the network for the PS5 makes Windows send an ARP broadcast for each.
// All 254 at once was a burst of hundreds of broadcasts, slow on Wi-Fi and seen as an attack by some
// routers, which then cut the PC off. So the search asks a few at a time, starts only after the
// usual ways failed for a while, and rests after asking them all.
#define SWEEP_AFTER_PROBES 20
#define SWEEP_REST_MS 600000
#define SWEEP_SLOW_PER_ROUND 4
#define SWEEP_FAST_PER_ROUND 8
#define SWEEP_GAP_MS 10
#define PROBE_TIMEOUT_MS 800
#define PROBE_LONG_TIMEOUT_MS 3000
#define PROBES_PER_LONG_WAIT 5
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
// The PS5 sends sound in bursts for the first seconds of a stream: across real sessions, half of the
// first dropouts came in the first 3 s. Until then the buffer is as large as the buffer setting, up to
// 150 ms, with twice the setting before a backlog is thrown away.
#define AUDIO_WARMUP_MS 8000
#define AUDIO_WARMUP_TARGET_MS 150
// Afterwards the extra backlog is played away by skipping a 10 ms piece of near silence now and then,
// so the sound doesn't stay further behind the picture than it needs to
#define AUDIO_DRAIN_SLACK_MS 20
#define AUDIO_DRAIN_GAP_MS 100
#define AUDIO_DRAIN_LOUD_GAP_MS 1000
#define AUDIO_QUIET_PEAK 1000
#define RECONNECT_MS 1500
// The PS5 sends sound a hundred times a second, silence included. Nothing at all for this long means
// it dropped the stream without saying so, and only a new connection brings it back.
#define STALL_MS 15000
#define AUDIO_REOPEN_MS 2000
#define IN_USE_QUIET_MS 15000
// After "Server shutting down" the PS5's Remote Play server stays up a few more seconds. Trying
// then got "in use" or hung for up to 51 s; when the PS5 stayed on it took 12 s or more anyway.
#define RETRY_AFTER_SHUTDOWN_MS 6000

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
	HOTKEY_AUDIO = 1, HOTKEY_VIDEO, HOTKEY_MUTE,
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
	int ao_kbps; // audio only always asks for 360p, the least the PS5 accepts
	int volume, buffer_ms, loss_pct, reconnect, no_controller, wifi_opt, autostart_audio, win_start, tray, auto_wake, gamepad;
	int hotkey_audio, hotkey_video, hotkey_mute; // HKM_GETHOTKEY format
	char device[256]; // empty is the system default
} Settings;

typedef struct {
	int res, fps, kbps, h265;
} Preset;

static const Preset presets[] = {
	{ 1, 30, 500, 0 },
	{ 1, 30, 2000, 0 },
	{ 3, 30, 6000, 0 },
	{ 4, 60, 20000, 0 },
	{ 4, 60, 50000, 1 },
};
#define PRESET_COUNT ((int)(sizeof(presets) / sizeof(presets[0])))
#define PRESET_DEFAULT 2

static struct {
	HWND hwnd;
	UINT dpi;
	wchar_t config_path[MAX_PATH], data_dir[MAX_PATH];
	FILE *log_file;
	long long log_bytes;
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
	volatile LONG wifi_channel; // 0 when unknown or not on Wi-Fi
	volatile LONG wifi_signal; // percent; WIFI_UNKNOWN when not measured, WIFI_NONE with no Wi-Fi connected
	wchar_t wifi_5ghz[33]; // a saved 5 GHz network in reach while this PC sits on 2.4 GHz, guarded by lock
	volatile LONG wifi_moved_at; // GetTickCount when the router moved its Wi-Fi to another channel mid-session, 0 for never
	volatile LONG ended, connected, fatal, audio_reopen;
	ChiakiQuitReason quit_reason;

	ChiakiSession session;
	ChiakiOpusDecoder opus;
	SDL_AudioDeviceID audio_dev;
	uint32_t audio_channels, audio_rate;
	bool audio_waiting;
	int audio_target_ms;
	DWORD audio_last_dropout, connected_at;
	volatile DWORD last_rx; // when the PS5 last sent sound or a picture
	DWORD audio_rx_at; // when the last sound arrived, 0 at the start of a session
	DWORD audio_first_at, audio_drained_at; // first sound of this session, last skipped piece
	volatile LONG audio_drained; // 10 ms pieces skipped to catch up since the last connection report
	volatile LONG pad_presses; // gamepad presses sent since the last connection report
	volatile LONG audio_gap_max; // longest wait between two sounds since the last connection report
	volatile LONG audio_dropouts, video_fps10, video_lost_total, ps5_standby;
	char update_tag[32];
	volatile LONG network_changed, regist_rejected, muted, session_live, rumble_new, rumble_left, rumble_right;
	CRITICAL_SECTION pad_lock, sdl_lock;
	wchar_t pad_name[64];
	int regist_tries;
	volatile LONG update_ready, update_busy, update_failed, update_restart;
	volatile LONG update_percent; // download progress for the update screen, 100 while unpacking
	DWORD started_at;
	char update_url[512], update_digest[72];
	bool relaunched;
	HANDLE single_instance;
	const wchar_t *update_note;
	DWORD update_note_at;
	HANDLE wlan;
	int16_t *volume_buf;
	int audio_fade_pos;
	DWORD audio_opened_at;
	char host_id[16];
	size_t volume_buf_size;

	ChiakiFfmpegDecoder decoder;
	HANDLE video_thread, video_ready;
	volatile LONG video_stop, frame_pending;
	volatile LONG frames_fed; // complete frames handed to the decoder
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
	bool same = wcscmp(g.status, text) == 0;
	if(!same)
		wcsncpy(g.status, text, 255);
	LeaveCriticalSection(&g.lock);
	if(!same)
		PostMessageW(g.hwnd, WM_APP_STATUS, 0, 0);
}

// A healthy connection repeats these every second, which fills the log until the moment that matters
// no longer fits, and the session ID doesn't belong in a file people send around.
static bool log_is_noise(const char *msg)
{
	static const char *const noise[] = { "CTRL RECEIVED", "CTRL SEND", "offset 0  1", "Ctrl received Heartbeat", "Senkusha sending Ping", "Senkusha received Pong", "Ctrl received valid Session Id" };
	for(size_t i = 0; i < sizeof(noise) / sizeof(noise[0]); i++)
		if(strstr(msg, noise[i]))
			return true;
	// The rows of a hex dump: an offset, then bytes
	return msg[0] == ' ' && strspn(msg, " 0123456789abcdef") >= 8;
}

// Sessions are added to one file, so restarting or updating doesn't wipe the one worth reading.
// Past the size limit the file moves to log.onceki.txt and a fresh one starts.
// Re-entrant, so a crash logged while the same thread was writing still gets written
static CRITICAL_SECTION log_lock;

// Must be called with log_lock held, or before other threads log
static void log_open(bool rotate)
{
	static bool lock_ready;
	if(!lock_ready)
		InitializeCriticalSection(&log_lock);
	lock_ready = true;
	wchar_t path[MAX_PATH], old[MAX_PATH];
	swprintf(path, MAX_PATH, L"%ls\\log.txt", g.data_dir);
	swprintf(old, MAX_PATH, L"%ls\\log.onceki.txt", g.data_dir);
	if(g.log_file)
		fclose(g.log_file);
	if(rotate)
		MoveFileExW(path, old, MOVEFILE_REPLACE_EXISTING);
	g.log_file = _wfopen(path, L"a");
	g.log_bytes = 0;
	if(g.log_file && _fseeki64(g.log_file, 0, SEEK_END) == 0)
		g.log_bytes = _ftelli64(g.log_file);
	if(!rotate && g.log_bytes > LOG_MAX_BYTES)
		log_open(true);
}

static void log_close(void)
{
	EnterCriticalSection(&log_lock);
	if(g.log_file)
		fclose(g.log_file);
	g.log_file = NULL;
	LeaveCriticalSection(&log_lock);
}

static void log_cb(ChiakiLogLevel level, const char *msg, void *user)
{
	// The library only reports that pairing failed. An answer from the PS5 means the code was
	// refused, and anything else means a packet got lost and trying again can work.
	if(strstr(msg, "Regist received HTTP code"))
		g.regist_rejected = 1;
	(void)level;
	(void)user;
	if(log_is_noise(msg))
		return;
	SYSTEMTIME t;
	GetLocalTime(&t);
	EnterCriticalSection(&log_lock);
	if(g.log_file && g.log_bytes > LOG_MAX_BYTES)
		log_open(true);
	if(g.log_file)
	{
		// Lines only carry the time, so each day gets a date line
		static WORD last_day;
		int n = 0;
		if(t.wDay != last_day)
			n += fprintf(g.log_file, "---- %04d-%02d-%02d ----\n", t.wYear, t.wMonth, t.wDay);
		last_day = t.wDay;
		n += fprintf(g.log_file, "%02d:%02d:%02d %s\n", t.wHour, t.wMinute, t.wSecond, msg);
		g.log_bytes += n > 0 ? n : 0;
		fflush(g.log_file);
	}
	LeaveCriticalSection(&log_lock);
}

// ---------- audio ----------

static void audio_open(void)
{
	if(g.audio_dev)
		SDL_CloseAudioDevice(g.audio_dev);
	g.audio_opened_at = GetTickCount();
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
	// This runs on the thread that receives everything from the PS5. A game downloading or unpacking
	// on the PC kept it waiting for the CPU, and packets then came out in bursts with gaps between.
	static __thread bool boosted;
	if(!boosted)
	{
		boosted = true;
		SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
	}
	DWORD arrived = GetTickCount();
	if(!g.audio_rx_at)
		g.audio_first_at = arrived;
	if(g.audio_rx_at && (LONG)(arrived - g.audio_rx_at) > g.audio_gap_max)
		g.audio_gap_max = (LONG)(arrived - g.audio_rx_at);
	g.audio_rx_at = arrived;
	g.last_rx = arrived;
	// A headset that was unplugged leaves the device stopped for good, and with no device at all
	// this runs a hundred times a second, so only try every so often
	bool device_gone = !g.audio_dev || SDL_GetAudioDeviceStatus(g.audio_dev) == SDL_AUDIO_STOPPED;
	if(g.audio_rate && (InterlockedExchange(&g.audio_reopen, 0) || (device_gone && GetTickCount() - g.audio_opened_at >= AUDIO_REOPEN_MS)))
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
	// Muting keeps the stream and the buffer running, so sound comes back instantly
	int volume = g.muted ? 0 : s.volume;
	if(volume != 100)
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
			int v = buf[i] * volume / 100;
			g.volume_buf[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
		}
		buf = g.volume_buf;
	}
	Uint32 bytes_per_ms = g.audio_rate * g.audio_channels * sizeof(int16_t) / 1000;
	Uint32 queued = SDL_GetQueuedAudioSize(g.audio_dev);
	DWORD now = GetTickCount();
	// Two thirds leaves room for a burst before the backlog check below throws everything away.
	// Real sessions on weak Wi-Fi kept dropping out at the old half.
	int target_max = s.buffer_ms * 2 / 3 < AUDIO_TARGET_MAX_MS ? s.buffer_ms * 2 / 3 : AUDIO_TARGET_MAX_MS;
	bool warming = now - g.audio_first_at < AUDIO_WARMUP_MS;
	int target = warming ? (s.buffer_ms < AUDIO_WARMUP_TARGET_MS ? s.buffer_ms : AUDIO_WARMUP_TARGET_MS)
		: g.audio_target_ms > target_max ? target_max : g.audio_target_ms;
	if(queued > (Uint32)(warming ? 2 * s.buffer_ms : s.buffer_ms) * bytes_per_ms)
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
	else if(!warming && !g.audio_waiting && queued > (Uint32)(target + AUDIO_DRAIN_SLACK_MS) * bytes_per_ms
		&& now - g.audio_drained_at >= AUDIO_DRAIN_GAP_MS)
	{
		int peak = 0;
		for(size_t i = 0; i < count && peak < AUDIO_QUIET_PEAK; i++)
			peak = buf[i] > peak ? buf[i] : -buf[i] > peak ? -buf[i] : peak;
		// A piece with sound in it goes only now and then, and the next one fades in so it doesn't click
		if(peak < AUDIO_QUIET_PEAK || now - g.audio_drained_at >= AUDIO_DRAIN_LOUD_GAP_MS)
		{
			if(peak >= AUDIO_QUIET_PEAK)
				g.audio_fade_pos = 0;
			g.audio_drained_at = now;
			InterlockedIncrement(&g.audio_drained);
			return;
		}
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
			for(uint32_t c = 0; c < g.audio_channels; c++)
				buf[frame * g.audio_channels + c] = (int16_t)(buf[frame * g.audio_channels + c] * g.audio_fade_pos / fade_frames);
	}
	SDL_QueueAudio(g.audio_dev, buf, (Uint32)(count * sizeof(int16_t)));
	if(g.audio_waiting && SDL_GetQueuedAudioSize(g.audio_dev) >= (Uint32)target * bytes_per_ms)
	{
		SDL_PauseAudioDevice(g.audio_dev, 0);
		g.audio_waiting = false;
	}
}

// ---------- video ----------

// A decoder that takes frames but gives no picture back. In a real session the picture froze for 17
// and 25 s while every frame arrived whole and the sound played on: nothing was missing, so nothing
// asked the PS5 for a fresh picture, and an IDR frame was what ended both freezes.
#define DECODER_STALL_FRAMES 10
#define DECODER_STALL_MS 700
#define DECODER_STALL_RETRY_MS 1500

static bool video_sample_cb(uint8_t *buf, size_t buf_size, int32_t frames_lost, bool frame_recovered, void *user)
{
	InterlockedIncrement(&g.frames_fed);
	return chiaki_ffmpeg_decoder_video_sample_cb(buf, buf_size, frames_lost, frame_recovered, user);
}

static void frame_available_cb(ChiakiFfmpegDecoder *decoder, void *user)
{
	(void)decoder;
	(void)user;
	g.last_rx = GetTickCount();
	if(InterlockedExchange(&g.frame_pending, 1))
		return;
	SDL_Event e = { 0 };
	e.type = SDL_USEREVENT;
	SDL_PushEvent(&e);
}

static int config_int(const char *key, int fallback, int min, int max);
static void config_set_int(const char *key, int value);

// Hardware-decoded frames stay on the graphics card: its video processor converts and scales them
// straight into the window. Copying each frame down to memory and back up cost about 4.5 ms at 1080p.
#define GPU_VIEWS 32

typedef struct {
	AVD3D11VADeviceContext *hw; // the decoder's device, with the lock that guards its context
	ID3D11VideoDevice *vdev;
	ID3D11VideoContext *vctx;
	ID3D11VideoContext1 *vctx1; // newer drivers only follow the colour space given through this one
	IDXGISwapChain *swap;
	ID3D11VideoProcessorEnumerator *vpe;
	ID3D11VideoProcessor *vp;
	ID3D11VideoProcessorOutputView *out;
	ID3D11VideoProcessorInputView *in[GPU_VIEWS];
	ID3D11Texture2D *in_tex[GPU_VIEWS];
	UINT in_slice[GPU_VIEWS];
	int in_count, src_w, src_h, out_w, out_h;
	HWND hwnd;
} GpuView;

static void gpu_views_clear(GpuView *v)
{
	for(int i = 0; i < v->in_count; i++)
		ID3D11VideoProcessorInputView_Release(v->in[i]);
	v->in_count = 0;
}

static void gpu_close(GpuView *v)
{
	gpu_views_clear(v);
	if(v->out)
		ID3D11VideoProcessorOutputView_Release(v->out);
	if(v->vp)
		ID3D11VideoProcessor_Release(v->vp);
	if(v->vpe)
		ID3D11VideoProcessorEnumerator_Release(v->vpe);
	if(v->swap)
		IDXGISwapChain_Release(v->swap);
	if(v->vctx1)
		ID3D11VideoContext1_Release(v->vctx1);
	if(v->vctx)
		ID3D11VideoContext_Release(v->vctx);
	if(v->vdev)
		ID3D11VideoDevice_Release(v->vdev);
	memset(v, 0, sizeof(*v));
}

static bool gpu_open(GpuView *v, SDL_Window *window, AVBufferRef *device)
{
	memset(v, 0, sizeof(*v));
	SDL_SysWMinfo info;
	SDL_VERSION(&info.version);
	if(!device || !SDL_GetWindowWMInfo(window, &info))
		return false;
	v->hwnd = info.info.win.window;
	v->hw = ((AVHWDeviceContext *)device->data)->hwctx;
	IDXGIDevice *dxgi = NULL;
	IDXGIAdapter *adapter = NULL;
	IDXGIFactory *factory = NULL;
	bool ok = SUCCEEDED(ID3D11Device_QueryInterface(v->hw->device, &IID_ID3D11VideoDevice, (void **)&v->vdev))
		&& SUCCEEDED(ID3D11DeviceContext_QueryInterface(v->hw->device_context, &IID_ID3D11VideoContext, (void **)&v->vctx))
		&& SUCCEEDED(ID3D11Device_QueryInterface(v->hw->device, &IID_IDXGIDevice, (void **)&dxgi))
		&& SUCCEEDED(IDXGIDevice_GetAdapter(dxgi, &adapter))
		&& SUCCEEDED(IDXGIAdapter_GetParent(adapter, &IID_IDXGIFactory, (void **)&factory));
	if(ok && FAILED(ID3D11VideoContext_QueryInterface(v->vctx, &IID_ID3D11VideoContext1, (void **)&v->vctx1)))
		v->vctx1 = NULL;
	if(ok)
	{
		// The blit model, like SDL's own drawing, so Discord can capture the window the same way
		DXGI_SWAP_CHAIN_DESC desc = { 0 };
		desc.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
		desc.SampleDesc.Count = 1;
		desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		desc.BufferCount = 1;
		desc.OutputWindow = v->hwnd;
		desc.Windowed = TRUE;
		desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
		v->hw->lock(v->hw->lock_ctx);
		ok = SUCCEEDED(IDXGIFactory_CreateSwapChain(factory, (IUnknown *)v->hw->device, &desc, &v->swap));
		v->hw->unlock(v->hw->lock_ctx);
		if(ok)
			IDXGIFactory_MakeWindowAssociation(factory, v->hwnd, DXGI_MWA_NO_ALT_ENTER); // F11 stays SDL's
	}
	if(factory)
		IDXGIFactory_Release(factory);
	if(adapter)
		IDXGIAdapter_Release(adapter);
	if(dxgi)
		IDXGIDevice_Release(dxgi);
	if(!ok)
		gpu_close(v);
	return ok;
}

// Fits the picture into the window without stretching it, with black bars where it doesn't fill
static RECT gpu_fit(int src_w, int src_h, int out_w, int out_h)
{
	RECT r = { 0, 0, out_w, out_h };
	if((long long)out_w * src_h > (long long)out_h * src_w)
	{
		int w = (int)((long long)out_h * src_w / src_h);
		r.left = (out_w - w) / 2;
		r.right = r.left + w;
	}
	else
	{
		int h = (int)((long long)out_w * src_h / src_w);
		r.top = (out_h - h) / 2;
		r.bottom = r.top + h;
	}
	return r;
}

// Must be called with the device lock held
static bool gpu_prepare(GpuView *v, const AVFrame *f, int out_w, int out_h)
{
	if(f->width != v->src_w || f->height != v->src_h || out_w != v->out_w || out_h != v->out_h || !v->vp)
	{
		if(v->vp)
			ID3D11VideoProcessor_Release(v->vp);
		if(v->vpe)
			ID3D11VideoProcessorEnumerator_Release(v->vpe);
		v->vp = NULL;
		v->vpe = NULL;
		gpu_views_clear(v); // views belong to the enumerator they were made with
		if(v->out)
			ID3D11VideoProcessorOutputView_Release(v->out);
		v->out = NULL;
		D3D11_VIDEO_PROCESSOR_CONTENT_DESC desc = { 0 };
		desc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
		desc.InputWidth = f->width;
		desc.InputHeight = f->height;
		desc.OutputWidth = out_w;
		desc.OutputHeight = out_h;
		desc.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
		if(FAILED(ID3D11VideoDevice_CreateVideoProcessorEnumerator(v->vdev, &desc, &v->vpe))
			|| FAILED(ID3D11VideoDevice_CreateVideoProcessor(v->vdev, v->vpe, 0, &v->vp)))
			return false;
		// Driver touch-ups like noise reduction cost time and aren't wanted on game footage
		ID3D11VideoContext_VideoProcessorSetStreamAutoProcessingMode(v->vctx, v->vp, 0, FALSE);
		ID3D11VideoContext_VideoProcessorSetStreamFrameFormat(v->vctx, v->vp, 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
		D3D11_VIDEO_COLOR black = { .RGBA = { 0, 0, 0, 1 } };
		ID3D11VideoContext_VideoProcessorSetOutputBackgroundColor(v->vctx, v->vp, FALSE, &black);
		D3D11_VIDEO_PROCESSOR_COLOR_SPACE rgb = { 0 }; // full range RGB out
		ID3D11VideoContext_VideoProcessorSetOutputColorSpace(v->vctx, v->vp, &rgb);
		if(v->vctx1)
			ID3D11VideoContext1_VideoProcessorSetOutputColorSpace1(v->vctx1, v->vp, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
		// The decoder's textures are taller than the picture: rows are padded to a multiple of 16, or 128
		// for H.265, so 1080p H.265 comes in a 1152-row texture. Without a source rectangle the padding
		// was scaled in too and showed as a black band along the bottom.
		RECT src = { 0, 0, f->width, f->height };
		ID3D11VideoContext_VideoProcessorSetStreamSourceRect(v->vctx, v->vp, 0, TRUE, &src);
		RECT dest = gpu_fit(f->width, f->height, out_w, out_h);
		ID3D11VideoContext_VideoProcessorSetStreamDestRect(v->vctx, v->vp, 0, TRUE, &dest);
		v->src_w = f->width;
		v->src_h = f->height;
	}
	if(!v->out)
	{
		if(FAILED(IDXGISwapChain_ResizeBuffers(v->swap, 0, out_w, out_h, DXGI_FORMAT_UNKNOWN, 0)))
			return false;
		ID3D11Texture2D *back = NULL;
		if(FAILED(IDXGISwapChain_GetBuffer(v->swap, 0, &IID_ID3D11Texture2D, (void **)&back)))
			return false;
		D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC desc = { .ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D };
		HRESULT hr = ID3D11VideoDevice_CreateVideoProcessorOutputView(v->vdev, (ID3D11Resource *)back, v->vpe, &desc, &v->out);
		ID3D11Texture2D_Release(back);
		if(FAILED(hr))
			return false;
		v->out_w = out_w;
		v->out_h = out_h;
	}
	// An untagged stream gets BT.601, SDL's default, so the picture looks the same on both paths
	bool bt709 = f->colorspace == AVCOL_SPC_BT709;
	D3D11_VIDEO_PROCESSOR_COLOR_SPACE yuv = { 0 };
	yuv.YCbCr_Matrix = bt709;
	yuv.Nominal_Range = f->color_range == AVCOL_RANGE_JPEG ? D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255 : D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
	ID3D11VideoContext_VideoProcessorSetStreamColorSpace(v->vctx, v->vp, 0, &yuv);
	if(v->vctx1)
	{
		bool full = f->color_range == AVCOL_RANGE_JPEG;
		DXGI_COLOR_SPACE_TYPE cs = bt709 ? (full ? DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P709 : DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709)
			: (full ? DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P601 : DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P601);
		ID3D11VideoContext1_VideoProcessorSetStreamColorSpace1(v->vctx1, v->vp, 0, cs);
	}
	return true;
}

// The decoder reuses a handful of texture slices, so their views are made once and kept
static ID3D11VideoProcessorInputView *gpu_input(GpuView *v, ID3D11Texture2D *tex, UINT slice)
{
	for(int i = 0; i < v->in_count; i++)
		if(v->in_tex[i] == tex && v->in_slice[i] == slice)
			return v->in[i];
	if(v->in_count == GPU_VIEWS)
		gpu_views_clear(v);
	D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC desc = { .ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D };
	desc.Texture2D.ArraySlice = slice;
	ID3D11VideoProcessorInputView *view = NULL;
	if(FAILED(ID3D11VideoDevice_CreateVideoProcessorInputView(v->vdev, (ID3D11Resource *)tex, v->vpe, &desc, &view)))
		return NULL;
	v->in[v->in_count] = view;
	v->in_tex[v->in_count] = tex;
	v->in_slice[v->in_count] = slice;
	v->in_count++;
	return view;
}

static bool gpu_draw(GpuView *v, const AVFrame *f)
{
#ifdef PSSTREAM_TEST
	if(GetEnvironmentVariableW(L"PSSTREAM_TEST_GPU_FAIL", NULL, 0))
		return false;
#endif
	RECT client;
	GetClientRect(v->hwnd, &client);
	if(client.right <= 0 || client.bottom <= 0)
		return true; // minimized, nothing to draw into
	v->hw->lock(v->hw->lock_ctx);
	bool ok = gpu_prepare(v, f, client.right, client.bottom);
	ID3D11VideoProcessorInputView *in = ok ? gpu_input(v, (ID3D11Texture2D *)f->data[0], (UINT)(intptr_t)f->data[1]) : NULL;
	if(in)
	{
		D3D11_VIDEO_PROCESSOR_STREAM stream = { .Enable = TRUE, .pInputSurface = in };
		ok = SUCCEEDED(ID3D11VideoContext_VideoProcessorBlt(v->vctx, v->vp, v->out, 0, 1, &stream))
			&& SUCCEEDED(IDXGISwapChain_Present(v->swap, 0, 0));
	}
	v->hw->unlock(v->hw->lock_ctx);
	return ok && in;
}

#ifdef PSSTREAM_TEST
static void gpu_shot(GpuView *v, const char *path)
{
	ID3D11Texture2D *back = NULL, *copy = NULL;
	if(FAILED(IDXGISwapChain_GetBuffer(v->swap, 0, &IID_ID3D11Texture2D, (void **)&back)))
		return;
	D3D11_TEXTURE2D_DESC desc;
	ID3D11Texture2D_GetDesc(back, &desc);
	desc.Usage = D3D11_USAGE_STAGING;
	desc.BindFlags = 0;
	desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	desc.MiscFlags = 0;
	D3D11_MAPPED_SUBRESOURCE map;
	v->hw->lock(v->hw->lock_ctx);
	if(SUCCEEDED(ID3D11Device_CreateTexture2D(v->hw->device, &desc, NULL, &copy)))
	{
		ID3D11DeviceContext_CopyResource(v->hw->device_context, (ID3D11Resource *)copy, (ID3D11Resource *)back);
		if(SUCCEEDED(ID3D11DeviceContext_Map(v->hw->device_context, (ID3D11Resource *)copy, 0, D3D11_MAP_READ, 0, &map)))
		{
			SDL_Surface *s = SDL_CreateRGBSurfaceWithFormatFrom(map.pData, desc.Width, desc.Height, 32, map.RowPitch, SDL_PIXELFORMAT_ARGB8888);
			SDL_SaveBMP(s, path);
			SDL_FreeSurface(s);
			ID3D11DeviceContext_Unmap(v->hw->device_context, (ID3D11Resource *)copy, 0);
		}
		ID3D11Texture2D_Release(copy);
	}
	v->hw->unlock(v->hw->lock_ctx);
	ID3D11Texture2D_Release(back);
}
#endif

// While the video window is dragged bigger or smaller, its inside keeps the picture's shape, so the
// picture fills it with no bars at the sides or top and bottom. Maximizing or snapping can still give
// it another shape; F11 fills the screen.
static LRESULT CALLBACK video_sizing_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref)
{
	(void)id;
	const SIZE *pic = (const SIZE *)ref;
	if(msg == WM_SIZING && pic->cx > 0 && pic->cy > 0)
	{
		RECT *r = (RECT *)lp, win, client;
		GetWindowRect(hwnd, &win);
		GetClientRect(hwnd, &client);
		int frame_w = (win.right - win.left) - client.right, frame_h = (win.bottom - win.top) - client.bottom;
		if(wp == WMSZ_TOP || wp == WMSZ_BOTTOM)
			r->right = r->left + MulDiv(r->bottom - r->top - frame_h, pic->cx, pic->cy) + frame_w;
		else
		{
			int h = MulDiv(r->right - r->left - frame_w, pic->cy, pic->cx) + frame_h;
			if(wp == WMSZ_TOPLEFT || wp == WMSZ_TOPRIGHT)
				r->top = r->bottom - h;
			else
				r->bottom = r->top + h;
		}
	}
	return DefSubclassProc(hwnd, msg, wp, lp);
}

// Called when the stream's size is known or changes: brings a plain window to the picture's shape
static void video_shape_set(SDL_Window *window, SIZE *shape, int w, int h)
{
	shape->cx = w;
	shape->cy = h;
	if(w <= 0 || h <= 0 || (SDL_GetWindowFlags(window) & (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_MAXIMIZED | SDL_WINDOW_MINIMIZED)))
		return;
	int win_w, win_h;
	SDL_GetWindowSize(window, &win_w, &win_h);
	int want = (int)((long long)win_w * h / w);
	if(abs(want - win_h) > 1)
		SDL_SetWindowSize(window, win_w, want);
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
	EnterCriticalSection(&g.sdl_lock);
	bool video_ok = SDL_InitSubSystem(SDL_INIT_VIDEO) == 0;
	LeaveCriticalSection(&g.sdl_lock);
	if(!video_ok)
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
	// The PS5 sends 16:9, so the window opens at that shape and the picture fills it
	int start_w = config_int("video_w", S(960), 320, 16384);
	SDL_Window *window = SDL_CreateWindow(title, win_x, win_y, start_w, start_w * 9 / 16,
			SDL_WINDOW_RESIZABLE | (g.test_hidden ? SDL_WINDOW_HIDDEN : 0));
	SIZE pic_shape = { 16, 9 }; // the stream's own size once a frame says so
	HWND video_hwnd = NULL;
	if(window)
	{
		SDL_SysWMinfo info;
		SDL_VERSION(&info.version);
		if(SDL_GetWindowWMInfo(window, &info) && SetWindowSubclass(info.info.win.window, video_sizing_proc, 1, (DWORD_PTR)&pic_shape))
			video_hwnd = info.info.win.window;
	}
	GpuView gpu;
	bool on_gpu = window && gpu_open(&gpu, window, g.decoder.hw_device_ctx);
	SDL_Renderer *renderer = window && !on_gpu ? SDL_CreateRenderer(window, -1, 0) : NULL;
	AVFrame *last = NULL; // kept for redrawing the window between frames
	if(!renderer && !on_gpu)
	{
		CHIAKI_LOGE(&g.log, "SDL window failed: %s", SDL_GetError());
		set_status(L"Görüntü penceresi açılamadı, ses çalışmaya devam eder");
	}
	SetEvent(g.video_ready);

	SDL_Texture *texture = NULL;
	int tex_w = 0, tex_h = 0, tex_format = AV_PIX_FMT_NONE;
	AVFrame *sw_frame = av_frame_alloc();
	bool dirty = true;
	double stat_pull = 0, stat_copy = 0, stat_upload = 0, stat_present = 0, stat_start = now_ms();
	int stat_frames = 0, stat_lost = 0, stat_windows = 0;
	double stat_loss = 0; // highest packet loss chiaki measured in this window, which it reports to the PS5
	bool fullscreen = false;
	LONG pictures = 0, stall_fed = g.frames_fed, stall_pictures = 0, stall_asks = 0;
	double stall_since = now_ms(), stall_asked_at = 0;
	while((renderer || on_gpu) && !g.video_stop)
	{
		// Frames going in with no picture coming out: ask the PS5 for a fresh picture to start from,
		// and again every so often until one shows
		LONG fed = g.frames_fed;
		if(pictures != stall_pictures || !g.session_live || !g.connected)
		{
			if(stall_asks && pictures != stall_pictures)
				CHIAKI_LOGI(&g.log, "Video picture back after %.0f ms", now_ms() - stall_since);
			stall_pictures = pictures;
			stall_fed = fed;
			stall_since = now_ms();
			stall_asks = 0;
		}
		else if(fed - stall_fed >= DECODER_STALL_FRAMES && now_ms() - stall_since >= DECODER_STALL_MS
			&& (!stall_asks || now_ms() - stall_asked_at >= DECODER_STALL_RETRY_MS))
		{
			if(!stall_asks)
				CHIAKI_LOGW(&g.log, "Video decoder took %ld frames without giving a picture, asking the PS5 for a fresh one", (long)(fed - stall_fed));
			chiaki_session_request_idr(&g.session);
			stall_asks++;
			stall_asked_at = now_ms();
		}

		SDL_Event e;
		if(SDL_WaitEventTimeout(&e, 100))
		{
			do
			{
				if(e.type == SDL_QUIT)
					PostMessageW(g.hwnd, WM_APP_VIDEO_CLOSED, 0, 0);
				else if(e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_MAXIMIZED && !fullscreen)
				{
					// A maximized window is never 16:9 with the taskbar and title bar taking room, so the
					// picture had bars at the sides. Maximize goes full screen instead; F11 or Esc comes back.
					SDL_RestoreWindow(window);
					fullscreen = true;
					SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN_DESKTOP);
					CHIAKI_LOGI(&g.log, "Video window maximized, going full screen");
					dirty = true;
				}
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
			if(f)
				pictures++;
			stat_pull += now_ms() - t0;
			stat_lost += lost;
			if(f && on_gpu && f->format != AV_PIX_FMT_D3D11)
			{
				// Not a frame the GPU path can take, so SDL draws from here on
				CHIAKI_LOGW(&g.log, "Video frame not on the GPU, drawing it with SDL instead");
				av_frame_free(&last);
				gpu_close(&gpu);
				on_gpu = false;
				renderer = SDL_CreateRenderer(window, -1, 0);
				if(!renderer)
					av_frame_free(&f);
			}
			if(f && on_gpu)
			{
				if(f->width != tex_w || f->height != tex_h)
				{
					tex_w = f->width;
					tex_h = f->height;
					video_shape_set(window, &pic_shape, tex_w, tex_h);
					CHIAKI_LOGI(&g.log, "Video: %dx%d, hardware decoding, drawn on the GPU", tex_w, tex_h);
				}
				av_frame_free(&last);
				last = f;
				stat_frames++;
				dirty = true;
			}
			else if(f)
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
						video_shape_set(window, &pic_shape, tex_w, tex_h);
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
		if(dirty && on_gpu)
		{
			double t0 = now_ms();
			if(last && !gpu_draw(&gpu, last))
			{
				// Some drivers can't run the video processor; a black window would be worse than the copy
				CHIAKI_LOGW(&g.log, "Drawing video on the GPU failed, drawing it with SDL instead");
				av_frame_free(&last);
				gpu_close(&gpu);
				on_gpu = false;
				renderer = SDL_CreateRenderer(window, -1, 0);
				if(!renderer)
					CHIAKI_LOGE(&g.log, "SDL window failed: %s", SDL_GetError());
			}
#ifdef PSSTREAM_TEST
			if(g.test_video_shot && last)
			{
				gpu_shot(&gpu, g.test_video_shot);
				g.test_video_shot = NULL;
			}
#endif
			stat_present += now_ms() - t0;
			dirty = false;
		}
		else if(dirty && renderer)
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
		double loss = g.session.stream_connection.congestion_control.packet_loss;
		if(loss > stat_loss)
			stat_loss = loss;
		if(now_ms() - stat_start >= 2000 && stat_frames)
		{
			double fps = stat_frames * 1000.0 / (now_ms() - stat_start);
			g.video_fps10 = (LONG)(fps * 10);
			InterlockedExchangeAdd(&g.video_lost_total, stat_lost);
			if(stat_lost || ++stat_windows >= STATS_LOG_EVERY)
			{
				stat_windows = 0;
				CHIAKI_LOGI(&g.log, "Video stats: %.1f fps, lost %d, packet loss peak %.0f%%, %.1f Mbps, per frame ms: pull %.1f, copy %.1f, upload %.1f, present %.1f",
						fps, stat_lost, stat_loss * 100, g.session.stream_connection.measured_bitrate,
						stat_pull / stat_frames, stat_copy / stat_frames, stat_upload / stat_frames, stat_present / stat_frames);
			}
			stat_pull = stat_copy = stat_upload = stat_present = 0;
			stat_frames = stat_lost = 0;
			stat_loss = 0;
			stat_start = now_ms();
		}
	}
	av_frame_free(&sw_frame);
	av_frame_free(&last);
	if(on_gpu)
		gpu_close(&gpu);
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
		if(video_hwnd)
			RemoveWindowSubclass(video_hwnd, video_sizing_proc, 1);
		SDL_DestroyWindow(window);
	}
	EnterCriticalSection(&g.sdl_lock);
	SDL_QuitSubSystem(SDL_INIT_VIDEO);
	LeaveCriticalSection(&g.sdl_lock);
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

// ---------- gamepad ----------

#define PAD_MAX 8
#define PAD_POLL_MS 4
#define PAD_IDLE_POLL_MS 100
#define PAD_CHORD_DELAY_MS 120
#define PAD_RUMBLE_MS 1000
#define PAD_MAX_NAME 64

// Pads without a known mapping, mostly older DirectInput ones, usually follow this common layout.
static void pad_add_generic_mapping(int index)
{
	char guid[33], mapping[512];
	SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(index), guid, sizeof(guid));
	const char *name = SDL_JoystickNameForIndex(index);
	snprintf(mapping, sizeof(mapping), "%s,%s,a:b0,b:b1,x:b2,y:b3,leftshoulder:b4,rightshoulder:b5,back:b6,start:b7,leftstick:b8,rightstick:b9,"
			"dpup:h0.1,dpright:h0.2,dpdown:h0.4,dpleft:h0.8,leftx:a0,lefty:a1,rightx:a2,righty:a3,", guid, name ? name : "Gamepad");
	SDL_GameControllerAddMapping(mapping);
	CHIAKI_LOGI(&g.log, "Gamepad with no known mapping, using the common layout");
}

static void pad_set_name(const char *name)
{
	// Converted whole and cut afterwards, since a name too long for the buffer fails to convert,
	// and an empty name would read as no pad at all
	wchar_t wide[512];
	if(!MultiByteToWideChar(CP_UTF8, 0, name, -1, wide, 512))
		wcscpy(wide, name[0] ? L"Gamepad" : L"");
	wide[PAD_MAX_NAME - 1] = 0;
	EnterCriticalSection(&g.lock);
	bool changed = wcscmp(g.pad_name, wide) != 0;
	wcscpy(g.pad_name, wide);
	LeaveCriticalSection(&g.lock);
	if(!changed)
		return;
	CHIAKI_LOGI(&g.log, "Gamepad: %s", name[0] ? name : "none");
	if(wide[0])
	{
		wchar_t text[128];
		swprintf(text, 128, tr(L"Gamepad bağlandı: %ls"), wide);
		set_status(text);
	}
	else
		PostMessageW(g.hwnd, WM_APP_STATUS, 0, 0);
}

static void pad_map_buttons(SDL_GameController *pad, uint32_t *buttons)
{
	static const struct { SDL_GameControllerButton from; uint32_t to; } map[] = {
		// By position on the pad, so Xbox A is the PlayStation cross
		{ SDL_CONTROLLER_BUTTON_A, CHIAKI_CONTROLLER_BUTTON_CROSS },
		{ SDL_CONTROLLER_BUTTON_B, CHIAKI_CONTROLLER_BUTTON_MOON },
		{ SDL_CONTROLLER_BUTTON_X, CHIAKI_CONTROLLER_BUTTON_BOX },
		{ SDL_CONTROLLER_BUTTON_Y, CHIAKI_CONTROLLER_BUTTON_PYRAMID },
		{ SDL_CONTROLLER_BUTTON_DPAD_LEFT, CHIAKI_CONTROLLER_BUTTON_DPAD_LEFT },
		{ SDL_CONTROLLER_BUTTON_DPAD_RIGHT, CHIAKI_CONTROLLER_BUTTON_DPAD_RIGHT },
		{ SDL_CONTROLLER_BUTTON_DPAD_UP, CHIAKI_CONTROLLER_BUTTON_DPAD_UP },
		{ SDL_CONTROLLER_BUTTON_DPAD_DOWN, CHIAKI_CONTROLLER_BUTTON_DPAD_DOWN },
		{ SDL_CONTROLLER_BUTTON_LEFTSHOULDER, CHIAKI_CONTROLLER_BUTTON_L1 },
		{ SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, CHIAKI_CONTROLLER_BUTTON_R1 },
		{ SDL_CONTROLLER_BUTTON_LEFTSTICK, CHIAKI_CONTROLLER_BUTTON_L3 },
		{ SDL_CONTROLLER_BUTTON_RIGHTSTICK, CHIAKI_CONTROLLER_BUTTON_R3 },
		{ SDL_CONTROLLER_BUTTON_START, CHIAKI_CONTROLLER_BUTTON_OPTIONS },
		{ SDL_CONTROLLER_BUTTON_BACK, CHIAKI_CONTROLLER_BUTTON_SHARE },
		{ SDL_CONTROLLER_BUTTON_GUIDE, CHIAKI_CONTROLLER_BUTTON_PS },
		{ SDL_CONTROLLER_BUTTON_TOUCHPAD, CHIAKI_CONTROLLER_BUTTON_TOUCHPAD },
		{ SDL_CONTROLLER_BUTTON_MISC1, CHIAKI_CONTROLLER_BUTTON_SHARE },
	};
	for(size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++)
		if(SDL_GameControllerGetButton(pad, map[i].from))
			*buttons |= map[i].to;
}

static int16_t pad_stronger(int16_t a, int b)
{
	if(b > 32767)
		b = 32767;
	if(b < -32767)
		b = -32767;
	return abs(b) > abs(a) ? (int16_t)b : a;
}

// Xbox pads hide their guide button from programs, and most have no touchpad, so button pairs stand in:
// Back + Start is the PS button, and Back + Y is the touchpad. The pair's single buttons wait a moment,
// so pressing a pair doesn't also press Share or Options.
static void pad_apply_chords(uint32_t *buttons)
{
	static DWORD held_since;
	static bool held;
	const uint32_t back = CHIAKI_CONTROLLER_BUTTON_SHARE, start = CHIAKI_CONTROLLER_BUTTON_OPTIONS, y = CHIAKI_CONTROLLER_BUTTON_PYRAMID;
	if((*buttons & back) && (*buttons & start))
	{
		*buttons = (*buttons & ~(back | start)) | CHIAKI_CONTROLLER_BUTTON_PS;
		held = false;
		return;
	}
	if((*buttons & back) && (*buttons & y))
	{
		*buttons = (*buttons & ~(back | y)) | CHIAKI_CONTROLLER_BUTTON_TOUCHPAD;
		held = false;
		return;
	}
	if(!(*buttons & (back | start)))
	{
		held = false;
		return;
	}
	if(!held)
	{
		held = true;
		held_since = GetTickCount();
	}
	if(GetTickCount() - held_since < PAD_CHORD_DELAY_MS)
		*buttons &= ~(back | start);
}

#ifdef PSSTREAM_TEST
// Plays a fixed set of inputs on a virtual pad, so all of this runs without one plugged in.
static void pad_test_script(void)
{
	static SDL_Joystick *virtual_pad;
	static DWORD started;
	static int step;
	char kind[16];
	if(!GetEnvironmentVariableA("PSSTREAM_TEST_PAD", kind, sizeof(kind)) || step > 6)
		return;
	bool generic = strcmp(kind, "generic") == 0;
	if(!virtual_pad && !started)
	{
		int index = SDL_JoystickAttachVirtual(generic ? SDL_JOYSTICK_TYPE_UNKNOWN : SDL_JOYSTICK_TYPE_GAMECONTROLLER, 6, 15, 1);
		CHIAKI_LOGI(&g.log, "[pad test] virtual pad index %d: %s", index, index < 0 ? SDL_GetError() : "ok");
		started = GetTickCount();
		return;
	}
	if(!virtual_pad)
	{
		if(GetTickCount() - started < 800)
			return;
		for(int i = 0; i < SDL_NumJoysticks() && !virtual_pad; i++)
			if(SDL_JoystickIsVirtual(i))
				virtual_pad = SDL_JoystickFromInstanceID(SDL_JoystickGetDeviceInstanceID(i));
		started = GetTickCount();
		return;
	}
	if(GetTickCount() - started < 500u * (step + 1) || !g.connected)
		return;
	CHIAKI_LOGI(&g.log, "[pad test] step %d", step);
	// Buttons in a virtual pad follow SDL's order: 0 A, 4 back, 6 start, 3 Y; axes 0 left x, 4 left trigger
	bool common = generic;
	int a = 0, back = common ? 6 : 4, start = common ? 7 : 6;
	if(step == 0)
		SDL_JoystickSetVirtualButton(virtual_pad, a, 1);
	if(step == 1)
	{
		SDL_JoystickSetVirtualButton(virtual_pad, a, 0);
		SDL_JoystickSetVirtualAxis(virtual_pad, 0, 20000);
		if(!common)
			SDL_JoystickSetVirtualAxis(virtual_pad, 4, 32767);
	}
	if(step == 2)
	{
		SDL_JoystickSetVirtualAxis(virtual_pad, 0, 0);
		if(!common)
			SDL_JoystickSetVirtualAxis(virtual_pad, 4, -32768);
		SDL_JoystickSetVirtualButton(virtual_pad, back, 1);
		SDL_JoystickSetVirtualButton(virtual_pad, start, 1);
	}
	if(step == 3)
	{
		SDL_JoystickSetVirtualButton(virtual_pad, back, 0);
		SDL_JoystickSetVirtualButton(virtual_pad, start, 0);
	}
	if(step == 4)
		SDL_JoystickSetVirtualButton(virtual_pad, start, 1);
	if(step == 5)
		SDL_JoystickSetVirtualButton(virtual_pad, start, 0);
	step++;
}
#endif

static DWORD WINAPI gamepad_main(void *arg)
{
	(void)arg;
	SDL_GameController *pads[PAD_MAX] = { 0 };
	bool sdl_on = false;
	int devices_logged = -1;
	SDL_JoystickID open_failed = -1; // tried once, so the log isn't flooded every poll
	while(!g.quit)
	{
		if(!settings_get().gamepad)
		{
			if(sdl_on)
			{
				for(int i = 0; i < PAD_MAX; i++)
					if(pads[i])
						SDL_GameControllerClose(pads[i]);
				memset(pads, 0, sizeof(pads));
				EnterCriticalSection(&g.sdl_lock);
				SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER);
				LeaveCriticalSection(&g.sdl_lock);
				sdl_on = false;
				devices_logged = -1;
				open_failed = -1;
				pad_set_name("");
			}
			Sleep(250);
			continue;
		}
		if(!sdl_on)
		{
			// Without this, the pad stops reporting whenever another window has focus, like the game on the TV
			SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
			// XInput, DirectInput and HIDAPI cover every pad. This one adds about 150 MB and nothing else
			SDL_SetHint(SDL_HINT_JOYSTICK_WGI, "0");
			// Without this, SDL only looks for pads at the moment Windows reports a new device. An Xbox
			// pad gets its XInput slot a moment later, so one plugged in or turned on while the app ran
			// was never found. SDL's own thread checks the slots every second.
			SDL_SetHint(SDL_HINT_JOYSTICK_THREAD, "1");
			EnterCriticalSection(&g.sdl_lock);
			bool ok = SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) == 0;
			LeaveCriticalSection(&g.sdl_lock);
			if(!ok)
			{
				CHIAKI_LOGE(&g.log, "SDL gamepad failed: %s", SDL_GetError());
				Sleep(2000);
				continue;
			}
			// State is read directly, so nothing would ever empty the event queue
			SDL_JoystickEventState(SDL_IGNORE);
			SDL_GameControllerEventState(SDL_IGNORE);
			sdl_on = true;
		}
		// SDL's device windows made on this thread only hear about new pads through its messages
		MSG msg;
		while(PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
			DispatchMessageW(&msg);
		SDL_GameControllerUpdate();
#ifdef PSSTREAM_TEST
		pad_test_script();
#endif
		if(SDL_NumJoysticks() != devices_logged)
		{
			devices_logged = SDL_NumJoysticks();
			CHIAKI_LOGI(&g.log, "Gamepad devices found: %d", devices_logged);
		}

		for(int i = 0; i < PAD_MAX; i++)
			if(pads[i] && !SDL_GameControllerGetAttached(pads[i]))
			{
				SDL_GameControllerClose(pads[i]);
				pads[i] = NULL;
			}
		for(int index = 0; index < SDL_NumJoysticks(); index++)
		{
			SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(index);
			bool known = false;
			for(int i = 0; i < PAD_MAX; i++)
				known = known || (pads[i] && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pads[i])) == id);
			if(known || id == open_failed)
				continue;
			if(!SDL_IsGameController(index))
				pad_add_generic_mapping(index);
			for(int i = 0; i < PAD_MAX; i++)
				if(!pads[i])
				{
					pads[i] = SDL_GameControllerOpen(index);
					if(!pads[i])
					{
						open_failed = id;
						CHIAKI_LOGW(&g.log, "Gamepad %s couldn't be opened: %s", SDL_JoystickNameForIndex(index), SDL_GetError());
					}
					break;
				}
		}

		// Everything plugged in acts as one controller. A pad can show up twice through remapping
		// tools like DS4Windows, and this way it doesn't matter which copy answers.
		ChiakiControllerState state;
		chiaki_controller_state_set_idle(&state);
		const char *name = "";
		for(int i = 0; i < PAD_MAX; i++)
		{
			SDL_GameController *pad = pads[i];
			if(!pad)
				continue;
			if(!name[0])
				name = SDL_GameControllerName(pad) && SDL_GameControllerName(pad)[0] ? SDL_GameControllerName(pad) : "Gamepad";
			pad_map_buttons(pad, &state.buttons);
			int l2 = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT) >> 7;
			int r2 = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) >> 7;
			state.l2_state = l2 > state.l2_state ? (uint8_t)l2 : state.l2_state;
			state.r2_state = r2 > state.r2_state ? (uint8_t)r2 : state.r2_state;
			state.left_x = pad_stronger(state.left_x, SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTX));
			state.left_y = pad_stronger(state.left_y, SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTY));
			state.right_x = pad_stronger(state.right_x, SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_RIGHTX));
			state.right_y = pad_stronger(state.right_y, SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_RIGHTY));
		}
		pad_apply_chords(&state.buttons);
		pad_set_name(name);

		if(InterlockedExchange(&g.rumble_new, 0))
			for(int i = 0; i < PAD_MAX; i++)
				if(pads[i])
					SDL_GameControllerRumble(pads[i], (Uint16)(g.rumble_left * 257), (Uint16)(g.rumble_right * 257), PAD_RUMBLE_MS);

		// The session can end at any moment, and the library rejects a repeat of the same state itself
		bool live = false, sender = false;
		EnterCriticalSection(&g.pad_lock);
		if(g.session_live && g.connected)
		{
			chiaki_session_set_controller_state(&g.session, &state);
			live = true;
			sender = g.session.stream_connection.feedback_sender_active;
		}
		LeaveCriticalSection(&g.pad_lock);
		// Says once a session where a press went, so a pad that "does nothing" can be told apart from
		// one the PS5 never heard about
		static bool told;
		static uint32_t last_buttons;
		bool pressed = state.buttons & ~last_buttons;
		last_buttons = state.buttons;
		if(!live)
			told = false;
		else if(pressed && name[0])
		{
			InterlockedIncrement(&g.pad_presses);
			if(!told)
			{
				told = true;
				if(sender)
					CHIAKI_LOGI(&g.log, "Gamepad press sent to the PS5 (buttons %#x)", (unsigned)state.buttons);
				else
					CHIAKI_LOGW(&g.log, "Gamepad press not sent: this connection has no controller on the PS5");
			}
		}
		SDL_Delay(live ? PAD_POLL_MS : PAD_IDLE_POLL_MS);
	}
	for(int i = 0; i < PAD_MAX; i++)
		if(pads[i])
			SDL_GameControllerClose(pads[i]);
	return 0;
}

// ---------- session ----------

static void session_event_cb(ChiakiEvent *event, void *user)
{
	(void)user;
	switch(event->type)
	{
		case CHIAKI_EVENT_CONNECTED:
			g.connected_at = GetTickCount();
			g.last_rx = g.connected_at;
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
		case CHIAKI_EVENT_RUMBLE:
			g.rumble_left = event->rumble.left;
			g.rumble_right = event->rumble.right;
			g.rumble_new = 1;
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
	// A gamepad on the PC needs the virtual controller the DualSense setting hides
	info.disable_controller = s->no_controller && !s->gamepad;
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
	g.audio_rx_at = 0;
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
		profile_set(&g.session.connect_info.video_profile, RES_MIN, 30, s->ao_kbps, CHIAKI_CODEC_H264, safe);

	chiaki_session_set_event_cb(&g.session, session_event_cb, NULL);
	ChiakiAudioSink sink;
	chiaki_opus_decoder_get_sink(&g.opus, &sink);
	chiaki_session_set_audio_sink(&g.session, &sink);
	if(video)
		chiaki_session_set_video_sample_cb(&g.session, video_sample_cb, &g.decoder);

	err = chiaki_session_start(&g.session);
	if(err != CHIAKI_ERR_SUCCESS)
	{
		CHIAKI_LOGE(&g.log, "Session start failed: %s", chiaki_error_string(err));
		chiaki_session_fini(&g.session);
		goto error;
	}
	InterlockedExchange(&g.session_live, 1);
	CHIAKI_LOGI(&g.log, "Session started: %ux%u@%u %u kbps, video %s, controller %s",
			g.session.connect_info.video_profile.width, g.session.connect_info.video_profile.height,
			g.session.connect_info.video_profile.max_fps, g.session.connect_info.video_profile.bitrate, video ? "on" : "off", info.disable_controller ? "hidden" : "announced");
	return true;

error:
	if(video)
		video_stop();
	chiaki_opus_decoder_fini(&g.opus);
	return false;
}

static void session_stop(bool video)
{
	// The next session, even after a restart, starts from the buffer this one settled on
	static int saved_target;
	if(g.audio_target_ms && g.audio_target_ms != saved_target)
	{
		config_set_int("audio_target", g.audio_target_ms);
		saved_target = g.audio_target_ms;
	}
	EnterCriticalSection(&g.pad_lock);
	g.session_live = 0;
	LeaveCriticalSection(&g.pad_lock);
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
	// Only a console that hangs up on purpose. An unknown stream error is usually the handshake
	// lost on weak Wi-Fi, and raising the bitrate for that only made the next try heavier.
	return reason == CHIAKI_QUIT_REASON_STREAM_CONNECTION_REMOTE_DISCONNECTED;
}

// Stops Windows' periodic Wi-Fi scans, which stall traffic for moments, and asks the driver for
// streaming mode. No admin rights needed; Windows undoes both when the handle closes.
#define WIFI_UNKNOWN -1
#define WIFI_NONE -2
#define WIFI_REFRESH_MS 10000

static void config_read(const wchar_t *section, const char *key, char *buf, DWORD size);
static void config_write(const wchar_t *section, const char *key, const char *value);

// Routers left on "auto channel" move their Wi-Fi to another channel now and then, and every device
// on it loses the connection for a few seconds up to a minute. That looks exactly like an app choking
// the internet, so the log says when it happens. The last channel is kept, so a move while the app
// was closed shows too. live: seen during a session, so it explains a dropout happening now.
static void wifi_note_channel(const DOT11_MAC_ADDRESS bssid, ULONG channel, bool live)
{
	if(!channel)
		return;
	char ap[16], seen[32], before[32] = "";
	snprintf(ap, sizeof(ap), "%02x%02x%02x%02x%02x%02x", bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5]);
	snprintf(seen, sizeof(seen), "%s/%lu", ap, channel);
	config_read(L"wifi", "router", before, sizeof(before));
	if(strcmp(before, seen) == 0)
		return;
	config_write(L"wifi", "router", seen);
	// Another router or access point is a different network, not a move
	size_t ap_len = strlen(ap);
	if(strncmp(before, ap, ap_len) != 0 || before[ap_len] != '/')
		return;
	if(live)
	{
		InterlockedExchange(&g.wifi_moved_at, (LONG)(GetTickCount() | 1));
		CHIAKI_LOGW(&g.log, "The router moved its Wi-Fi from channel %s to %lu just now. Every device on it loses the connection for a moment when that happens",
				before + ap_len + 1, channel);
	}
	else
		CHIAKI_LOGW(&g.log, "The router moved its Wi-Fi from channel %s to %lu since the last connection. Every device on it loses the connection for a moment when that happens",
				before + ap_len + 1, channel);
}

// Signal and channel of the Wi-Fi this PC is on. Measured once when connecting, it missed the signal
// sinking mid-game, which on weak Wi-Fi is the first thing worth knowing about a dropout.
static void wifi_read_link(void)
{
	WLAN_INTERFACE_INFO_LIST *list = NULL;
	if(!g.wlan || WlanEnumInterfaces(g.wlan, NULL, &list) != ERROR_SUCCESS)
		return;
	LONG signal = WIFI_NONE, channel_number = 0;
	for(DWORD i = 0; i < list->dwNumberOfItems; i++)
	{
		if(list->InterfaceInfo[i].isState != wlan_interface_state_connected)
			continue;
		const GUID *id = &list->InterfaceInfo[i].InterfaceGuid;
		DWORD size = 0;
		WLAN_CONNECTION_ATTRIBUTES *link = NULL;
		ULONG *channel = NULL;
		if(WlanQueryInterface(g.wlan, id, wlan_intf_opcode_current_connection, NULL, &size, (void **)&link, NULL) == ERROR_SUCCESS && link)
			signal = (LONG)link->wlanAssociationAttributes.wlanSignalQuality;
		if(WlanQueryInterface(g.wlan, id, wlan_intf_opcode_channel_number, NULL, &size, (void **)&channel, NULL) == ERROR_SUCCESS && channel)
			channel_number = (LONG)*channel;
		if(link && channel)
			wifi_note_channel(link->wlanAssociationAttributes.dot11Bssid, *channel, true);
		if(link)
			WlanFreeMemory(link);
		if(channel)
			WlanFreeMemory(channel);
		break;
	}
	WlanFreeMemory(list);
	if(signal == WIFI_NONE && g.wifi_signal >= 0)
		CHIAKI_LOGW(&g.log, "Wi-Fi connection lost");
	else if(signal >= 0 && g.wifi_signal == WIFI_NONE)
		CHIAKI_LOGI(&g.log, "Wi-Fi connection back, signal %ld%% on channel %ld", (long)signal, (long)channel_number);
	g.wifi_signal = signal;
	g.wifi_channel = channel_number;
}

// Telling someone on 2.4 GHz to "use 5 GHz" only helps if they know which network that is. This
// names one Windows already has a password for and that's strong enough here to be worth it.
static void wifi_find_5ghz(const GUID *id, const DOT11_SSID *current)
{
	wchar_t best[33] = L"";
	ULONG best_quality = 0;
	WLAN_AVAILABLE_NETWORK_LIST *nets = NULL;
	if(WlanGetAvailableNetworkList(g.wlan, id, 0, NULL, &nets) == ERROR_SUCCESS && nets)
	{
		for(DWORD i = 0; i < nets->dwNumberOfItems; i++)
		{
			WLAN_AVAILABLE_NETWORK *net = &nets->Network[i];
			if(!(net->dwFlags & WLAN_AVAILABLE_NETWORK_HAS_PROFILE) || !net->dot11Ssid.uSSIDLength
				|| net->dot11Ssid.uSSIDLength > DOT11_SSID_MAX_LENGTH)
				continue;
			// The same name on 5 GHz is Windows' own choice to make
			if(net->dot11Ssid.uSSIDLength == current->uSSIDLength
				&& memcmp(net->dot11Ssid.ucSSID, current->ucSSID, current->uSSIDLength) == 0)
				continue;
			WLAN_BSS_LIST *bss = NULL;
			if(WlanGetNetworkBssList(g.wlan, id, &net->dot11Ssid, net->dot11BssType, net->bSecurityEnabled, NULL, &bss) != ERROR_SUCCESS || !bss)
				continue;
			for(DWORD b = 0; b < bss->dwNumberOfItems; b++)
			{
				WLAN_BSS_ENTRY *entry = &bss->wlanBssEntries[b];
				// 5 and 6 GHz; through a wall or two they fade fast, so a weak one isn't worth moving to
				if(entry->ulChCenterFrequency >= 4900000 && entry->uLinkQuality >= 45 && entry->uLinkQuality > best_quality)
				{
					best_quality = entry->uLinkQuality;
					char ssid[DOT11_SSID_MAX_LENGTH + 1];
					memcpy(ssid, net->dot11Ssid.ucSSID, net->dot11Ssid.uSSIDLength);
					ssid[net->dot11Ssid.uSSIDLength] = 0;
					if(!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, ssid, -1, best, 33))
						MultiByteToWideChar(CP_ACP, 0, ssid, -1, best, 33);
				}
			}
			WlanFreeMemory(bss);
		}
		WlanFreeMemory(nets);
	}
	EnterCriticalSection(&g.lock);
	wcscpy(g.wifi_5ghz, best);
	LeaveCriticalSection(&g.lock);
	if(best[0])
		CHIAKI_LOGI(&g.log, "A saved 5 GHz network is in reach, signal %lu%%", best_quality);
}

static void wifi_tune(bool on)
{
	if(!on)
	{
		if(g.wlan)
		{
			WlanCloseHandle(g.wlan, NULL);
			g.wlan = NULL;
			g.wifi_signal = WIFI_UNKNOWN;
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
		g.wifi_signal = WIFI_NONE;
		// Connection problems usually come down to these, so the log should say them
		WLAN_CONNECTION_ATTRIBUTES *link = NULL;
		ULONG *channel = NULL;
		bool have_link = WlanQueryInterface(g.wlan, id, wlan_intf_opcode_current_connection, NULL, &size, (void **)&link, NULL) == ERROR_SUCCESS && link;
		bool have_channel = WlanQueryInterface(g.wlan, id, wlan_intf_opcode_channel_number, NULL, &size, (void **)&channel, NULL) == ERROR_SUCCESS && channel;
		g.wifi_channel = have_channel ? (LONG)*channel : 0;
		if(have_link)
			g.wifi_signal = (LONG)link->wlanAssociationAttributes.wlanSignalQuality;
		if(have_link && have_channel)
			wifi_note_channel(link->wlanAssociationAttributes.dot11Bssid, *channel, false);
		if(have_link && have_channel && *channel >= 1 && *channel <= 14)
			wifi_find_5ghz(id, &link->wlanAssociationAttributes.dot11Ssid);
		if(have_link)
			CHIAKI_LOGI(&g.log, "Wi-Fi link: signal %lu%%, receive %lu Mbps, send %lu Mbps, channel %lu",
					link->wlanAssociationAttributes.wlanSignalQuality, link->wlanAssociationAttributes.ulRxRate / 1000,
					link->wlanAssociationAttributes.ulTxRate / 1000, have_channel ? *channel : 0);
		if(link)
			WlanFreeMemory(link);
		if(channel)
			WlanFreeMemory(channel);
	}
	WlanFreeMemory(list);
}

static uint32_t sweep_next = 1; // next host part the sweep asks
static DWORD sweep_done_at; // when the last full sweep ended, 0 before one has
static bool sweep_ever_done;

// Asks for the console on the known address first, then on the whole network, so a PS5 that got a
// new address from the router is found again. sweep_per_round addresses of each network are also
// asked one by one each round, 0 for none. Returns false if no PS5 answered.
static bool ps5_search(const char *prefer, int rounds, int sweep_per_round, char *ip, size_t ip_size, bool *standby, char *host_id, size_t host_id_size)
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
		for(int k = 0; k < sweep_per_round; k++)
		{
			for(DWORD i = 0; i < table->dwNumEntries; i++)
			{
				uint32_t addr = ntohl(table->table[i].dwAddr);
				if(!addr || (addr >> 24) == 127 || (addr >> 16) == 0xA9FE)
					continue;
				struct sockaddr_in to = { 0 };
				to.sin_family = AF_INET;
				to.sin_port = htons(9302);
				to.sin_addr.s_addr = htonl((addr & 0xFFFFFF00) | sweep_next);
				sendto(s, msg, sizeof(msg) - 1, 0, (struct sockaddr *)&to, sizeof(to));
			}
			if(++sweep_next == 255)
			{
				sweep_next = 1;
				sweep_done_at = GetTickCount();
				sweep_ever_done = true;
			}
			Sleep(SWEEP_GAP_MS);
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
				const char *id = strstr(buf, "host-id:");
				if(host_id && id)
				{
					id += 8;
					snprintf(host_id, host_id_size, "%.*s", (int)strcspn(id, "\r\n"), id);
				}
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
static void config_set(const char *key, const char *value);

static bool stream_changed(const Settings *a, const Settings *b, bool video)
{
	if(a->loss_pct != b->loss_pct || a->no_controller != b->no_controller || a->gamepad != b->gamepad)
		return true;
	if(!video)
		return a->ao_kbps != b->ao_kbps;
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
	bool may_wake = true, id_checked = false;
	CloseHandle(CreateThread(NULL, 0, network_watch_main, NULL, 0, NULL));
	while(true)
	{
		DWORD timeout = INFINITE;
		if(waiting)
		{
			LONG left = (LONG)(retry_at - GetTickCount());
			timeout = left > 0 ? (DWORD)left : 0;
		}
		else if(running && g.connected)
			timeout = 1000; // to notice a stream that went quiet
		WaitForSingleObject(g.wake, timeout);
		if(InterlockedExchange(&g.network_changed, 0))
		{
			// Reconnecting Wi-Fi forgets the low latency settings, so they're applied again below
			wifi_tune(false);
			if(waiting && !running)
			{
				CHIAKI_LOGI(&g.log, "Network changed, trying again soon");
				trying_since = GetTickCount();
				retry_at = trying_since + 1000;
			}
			else if(running)
				// Wi-Fi dropping and coming back mid-game shows up here, which explains a gap that follows
				CHIAKI_LOGI(&g.log, "Network changed during the session");
		}

		bool want_video = g.want_video != 0;
		bool want_any = !g.quit && (g.want_audio || want_video);
		Settings s = settings_get();
		// Only while connected, so a PS5 that's off doesn't keep the PC awake or its Wi-Fi tuned
		bool live = want_any && g.connected;
		// Connecting has to survive the same Wi-Fi stalls as playing
		wifi_tune((live || (running && want_any)) && s.wifi_opt);
		// The PC often sits untouched while the game plays on the TV
		SetThreadExecutionState(ES_CONTINUOUS | (live ? ES_SYSTEM_REQUIRED : 0) | (live && want_video ? ES_DISPLAY_REQUIRED : 0));
		if(live)
		{
			fails = 0;
			trying_since = 0;
			probes = 0;
			wake_tries = 0;
			g.ps5_standby = 0;
			// A connected PS5 was on purpose or was already on. If it goes away from here, someone
			// probably turned it off, and it must not come back by itself.
			may_wake = false;
			static DWORD wifi_read_at;
			if(g.wlan && GetTickCount() - wifi_read_at >= WIFI_REFRESH_MS)
			{
				wifi_read_at = GetTickCount();
				wifi_read_link();
			}
			if(!id_checked && !g.host_id[0])
			{
				// Remembers which console this is, so a second one at home can't take its place
				char host[256], ip[64] = "", id[16] = "";
				bool standby = false;
				host_get(host, sizeof(host));
				if(ps5_search(host, 2, 0, ip, sizeof(ip), &standby, id, sizeof(id)) && strcmp(ip, host) == 0 && id[0])
				{
					snprintf(g.host_id, sizeof(g.host_id), "%s", id);
					config_set("host_id", id);
				}
			}
			id_checked = true;
		}

		if(running)
		{
			if(g.connected && GetTickCount() - g.last_rx > STALL_MS && !g.ended)
			{
				CHIAKI_LOGI(&g.log, "Nothing from the PS5 for %d s, connecting again", STALL_MS / 1000);
				g.quit_reason = CHIAKI_QUIT_REASON_STREAM_CONNECTION_UNKNOWN;
				g.ended = 1;
			}
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
					if(!was_connected && quit_reason_refused_stream(reason) && !safe)
					{
						safe = true;
						CHIAKI_LOGI(&g.log, "The PS5 refused the stream, trying again with at least %d kbps", SAFE_KBPS);
					}
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
					if(reason == CHIAKI_QUIT_REASON_STREAM_CONNECTION_REMOTE_SHUTDOWN && delay < RETRY_AFTER_SHUTDOWN_MS)
						delay = RETRY_AFTER_SHUTDOWN_MS;
					retry_at = GetTickCount() + delay;
					const wchar_t *text = quit_reason_text(reason);
					if(wcscmp(text, L"Bağlantı yok, tekrar deneniyor...") == 0)
						text = early ? L"PS5'e bağlanılıyor..." : L"PS5'e ulaşılamıyor. Açık mı? Denemeye devam ediyorum";
					// The PS5 stays "in use" for seconds after our own previous connection
					if(reason == CHIAKI_QUIT_REASON_SESSION_REQUEST_RP_IN_USE && GetTickCount() - trying_since < IN_USE_QUIET_MS)
						text = L"PS5'e bağlanılıyor...";
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
			may_wake = true;
			id_checked = false;
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
				// A name instead of an address can't be checked this way, so it goes straight to connecting.
				// The longer wait now and then covers a link too slow to answer in the usual time.
				bool open = inet_addr(host) == INADDR_NONE
					|| ps5_port_open(host, probes % PROBES_PER_LONG_WAIT == 0 ? PROBE_LONG_TIMEOUT_MS : PROBE_TIMEOUT_MS);
				if(!open)
				{
					char id[16] = "";
					bool sweep = probes >= SWEEP_AFTER_PROBES && (!sweep_ever_done || GetTickCount() - sweep_done_at >= SWEEP_REST_MS);
					bool answered = ps5_search(host, 1, sweep ? SWEEP_SLOW_PER_ROUND : 0, ip, sizeof(ip), &standby, id, sizeof(id));
					if(answered && strcmp(ip, host) != 0)
					{
						if(g.host_id[0] && id[0] && _stricmp(g.host_id, id) != 0)
						{
							if(probes % 30 == 1)
								CHIAKI_LOGI(&g.log, "A different PS5 answered, ignoring it");
							answered = false;
							standby = false;
						}
						else
						{
							CHIAKI_LOGI(&g.log, "PS5 moved to a new address");
							host_set(ip);
							PostMessageW(g.hwnd, WM_APP_HOST_CHANGED, 0, 0);
							open = !standby;
						}
					}
					g.ps5_standby = answered && standby;
					// A resting PS5 may not answer the search at all, so try waking it either way
					if(s.auto_wake && may_wake && g.paired && wake_tries < WAKE_MAX_TRIES && (LONG)(GetTickCount() - wake_at) >= 0)
					{
						wake_tries++;
						wake_at = GetTickCount() + WAKE_REPEAT_MS;
						ps5_wake_send();
					}
					if(probes % 30 == 1)
						CHIAKI_LOGI(&g.log, "PS5 check %d: port closed, search %s", probes, !answered ? "no answer" : standby ? "rest mode" : "on");
				}
				// Starting a session connects to this same port, so with it closed there's no point
				if(!open)
				{
					DWORD waited = GetTickCount() - trying_since;
					if(g.ps5_standby)
						set_status(s.auto_wake && may_wake ? L"PS5 uyanıyor..." : L"PS5 uyku modunda. Uyandırabilir veya PS5'i açabilirsin");
					else
						set_status(waited < RETRY_FAST_WINDOW_MS ? L"PS5'e bağlanılıyor..." : L"PS5'e ulaşılamıyor. Açık mı? Denemeye devam ediyorum");
					waiting = true;
					retry_at = GetTickCount() + (waited >= PROBE_SLOW_AFTER_MS ? PROBE_SLOW_DELAY_MS : g.ps5_standby ? PROBE_STANDBY_DELAY_MS : PROBE_DELAY_MS);
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
				set_status(L"Bağlantı başlatılamadı, tekrar deneniyor...");
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
	config_get("host_id", g.host_id, sizeof(g.host_id));
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
	s->ao_kbps = config_int("ao_kbps", 500, 100, 2000);
	s->volume = config_int("volume", 100, 0, 200);
	s->buffer_ms = config_int("buffer_ms", 150, 40, 500);
	// Starting at the smallest buffer meant crackling for the first seconds of every session until it
	// grew back. One step under what the last session needed lets it shrink again if the link got better.
	int learned = config_int("audio_target", 0, AUDIO_TARGET_MIN_MS, AUDIO_TARGET_MAX_MS);
	g.audio_target_ms = learned - AUDIO_TARGET_STEP_MS > AUDIO_TARGET_MIN_MS ? learned - AUDIO_TARGET_STEP_MS : AUDIO_TARGET_MIN_MS;
	s->loss_pct = config_int("loss_pct", 5, 1, 20);
	s->reconnect = config_int("reconnect", 1, 0, 1);
	s->no_controller = config_int("no_controller", 1, 0, 1);
	s->gamepad = config_int("gamepad", 0, 0, 1);
	s->wifi_opt = config_int("wifi_opt", 1, 0, 1);
	s->autostart_audio = config_int("autostart_audio", 1, 0, 1);
	s->win_start = win_start_get();
	s->tray = config_int("tray", 1, 0, 1);
	s->auto_wake = config_int("auto_wake", 1, 0, 1);
	s->hotkey_audio = hotkey_load("hotkey_audio", MAKEWORD(VK_F9, HOTKEYF_CONTROL | HOTKEYF_ALT));
	s->hotkey_video = hotkey_load("hotkey_video", MAKEWORD(VK_F10, HOTKEYF_CONTROL | HOTKEYF_ALT));
	s->hotkey_mute = hotkey_load("hotkey_mute", MAKEWORD(VK_F8, HOTKEYF_CONTROL | HOTKEYF_ALT));
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
	config_set_int("ao_kbps", s->ao_kbps);
	config_set_int("volume", s->volume);
	config_set_int("buffer_ms", s->buffer_ms);
	config_set_int("loss_pct", s->loss_pct);
	config_set_int("reconnect", s->reconnect);
	config_set_int("no_controller", s->no_controller);
	config_set_int("gamepad", s->gamepad);
	config_set_int("wifi_opt", s->wifi_opt);
	config_set_int("autostart_audio", s->autostart_audio);
	config_set_int("tray", s->tray);
	config_set_int("auto_wake", s->auto_wake);
	config_set_int("hotkey_audio", s->hotkey_audio);
	config_set_int("hotkey_video", s->hotkey_video);
	config_set_int("hotkey_mute", s->hotkey_mute);
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
		found = ps5_search(typed, 50, SWEEP_FAST_PER_ROUND, ip, sizeof(ip), &standby, NULL, 0);
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
	g.update_percent = percent;
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
	config_set_int("update_fails", config_int("update_fails", 0, 0, 100) + 1);
	set_status(text);
}

static void update_begin(void);

static void update_auto(void)
{
	if(!g.update_ready || g.update_busy || !update_available_here() || config_int("update_fails", 0, 0, 100) >= UPDATE_AUTO_MAX_FAILS)
		return;
	// Just updated and still not the newest means the release itself is wrong. Installing it again
	// would loop, so this counts as a failed try instead.
	static bool counted;
	if(g.relaunched)
	{
		if(!counted)
			config_set_int("update_fails", config_int("update_fails", 0, 0, 100) + 1);
		counted = true;
		return;
	}
	if((g.want_audio || g.want_video) && GetTickCount() - g.started_at > UPDATE_AUTO_GRACE_MS)
		return;
	CHIAKI_LOGI(&g.log, "Installing %s by itself", g.update_tag);
	update_begin();
	set_status(L"Yeni sürüm yükleniyor. Uygulama birazdan kendiliğinden yeniden açılacak");
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
	g.update_percent = 0;
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
	log_close();

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
	// Right after Windows starts, the network often isn't ready yet, and the next check is hours away
	CURLcode res;
	for(int attempt = 1;; attempt++)
	{
		body.size = 0;
		body.data[0] = 0;
		res = curl_easy_perform(curl);
		if(res == CURLE_OK || attempt == UPDATE_TRIES || g.quit)
			break;
		CHIAKI_LOGI(&g.log, "Update check failed (%s), trying again", curl_easy_strerror(res));
		Sleep(UPDATE_RETRY_MS * attempt);
	}
	curl_easy_cleanup(curl);
	char tag[32] = "";
	if(res == CURLE_OK && json_get(body.data, "tag_name", tag, sizeof(tag)) && version_number(tag) > version_number(current))
	{
		update_asset_find(body.data, tag);
		snprintf(g.update_tag, sizeof(g.update_tag), "%s", tag);
		g.update_ready = 1;
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
	const uint8_t *mac = g.registered.server_mac;
	snprintf(g.host_id, sizeof(g.host_id), "%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
	config_set("host_id", g.host_id);
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
