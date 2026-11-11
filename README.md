<p align="center">
  <img src="docs/icon.png" width="96" alt="PS Stream icon">
</p>

<h1 align="center">PS Stream</h1>

<p align="center">
  A PS5 Remote Play client for Windows that lets you turn audio and video on and off separately.<br>
  Play on your TV and hear the game on your PC, or stream it to Discord.
</p>

<p align="center">
  <a href="../../releases/latest"><img alt="Latest release" src="https://img.shields.io/github/v/release/Warhead500metalmonster/ps-stream?style=flat-square&label=release&color=0070d1"></a>
  <a href="../../releases"><img alt="Downloads" src="https://img.shields.io/github/downloads/Warhead500metalmonster/ps-stream/total?style=flat-square&color=0070d1"></a>
  <a href="../../actions/workflows/release.yml"><img alt="Build" src="https://img.shields.io/github/actions/workflow/status/Warhead500metalmonster/ps-stream/release.yml?branch=main&style=flat-square&label=build"></a>
  <img alt="Windows 10 and 11" src="https://img.shields.io/badge/Windows-10%20%7C%2011-555?style=flat-square">
  <a href="LICENSE"><img alt="License: AGPL-3.0" src="https://img.shields.io/badge/license-AGPL--3.0-555?style=flat-square"></a>
</p>

<p align="center">
  <a href="../../releases/latest"><b>⬇ Download for Windows</b></a>
  &nbsp;·&nbsp; <a href="#getting-started">Getting started</a>
  &nbsp;·&nbsp; <a href="#features">Features</a>
  &nbsp;·&nbsp; <a href="#legal">Legal</a>
</p>

![PS Stream with audio on](docs/play.png)

## Why

If you play on a TV and use Discord on your PC, the usual way to get game audio on the PC is to run a full Remote Play client. That wastes CPU and Wi-Fi on a video you never watch. PS Stream can skip the video.

## Features

- **Audio-only mode.** Video isn't decoded, and the PS5 is asked for the smallest stream.
- **Video on demand.** Turn video on when you want to stream to Discord, and pick the "PS Stream - Video" window there. Press `F11` in that window, or maximize it, for full screen. Turning video on or off reconnects, which takes about 8 seconds because the PS5 needs that long to let go of the old connection. The app opens the way you left it, so starting with video doesn't cost that wait.
- **Simple by default.** Two big buttons, an **Audio only** choice, and five quality presets from 360p at 0.5 Mbps to 1080p 60 FPS at 50 Mbps. To set resolution, frame rate, bitrate and codec yourself, use **Custom** or **Advanced settings**.
- **Built for weak Wi-Fi.** Made for people who can't use a cable or 5 GHz. Pauses Windows' background Wi-Fi scans while connected and keeps your PC awake. The audio buffer grows to the gaps your Wi-Fi actually has, up to 0.3 s with video and 0.5 s in audio only, and shrinks again when the Wi-Fi calms down. After a stall, it catches up without a second silence. On lossy Wi-Fi it keeps resending what got lost, asks the PS5 again for a fresh picture when one is lost on the way, and reconnects by itself if the stream goes silent for 15 seconds. It tells you when the router changes its Wi-Fi channel.
- **Audio controls.** Choose the output device and maximum delay, and change the volume right on the Play page.
- **Live connection numbers.** Press **Details** to see connection time, ping, audio dropouts, video frame rate and lost frames.
- **Connects by itself.** Checks for the PS5 about once a second and connects as soon as it answers. If the PS5 is in rest mode, the app sends it a wake-up signal. You can turn that off with **Wake PS5** in Settings. It also finds the console again if the router gives it a new address.
- **Runs in the background.** Minimizes to the tray, can start with Windows, remembers where its windows were, and opening it a second time shows the running copy.
- **Global hotkeys.** `Ctrl+Alt+F9` for audio, `Ctrl+Alt+F10` for video and `Ctrl+Alt+F8` to mute. You can change them.
- **Play with any gamepad on your PC.** Turn on **Gamepad on PC** in Settings to play with an Xbox, DirectInput or other pad plugged into your PC. Pads SDL doesn't recognize use a common layout, so some buttons may differ. Back + Start is the PS button, and Back + Y is the touchpad.
- **Instant mute.** Mute with the speaker icon or the hotkey. The connection stays open, so sound comes right back.
- **Connection warnings.** The app tells you when Wi-Fi can't keep up and offers to lower the video quality. In the tray, it notifies you when the connection drops or returns.
- **Turkish and English interface.**

## Getting started

You need Windows 10 or 11 (64-bit), a PS5 on the same network, and a web browser for signing in.

1. Download the latest zip from [Releases](../../releases/latest) and extract it.
2. Open `PS Stream.exe`. Keep the folder together, because the app needs the files in `dist`. Windows may warn you because the app isn't signed. Choose **More info**, then **Run anyway**.
   - A short tour shows where everything is. Press **Skip tour** if you don't need it, and replay it anytime with **Show tour** in Settings.
3. On the **Pairing** page:
   - The app looks for your PS5. If it can't find it, type the console's IP address.
   - Press **Sign in with PSN** and sign in on Sony's page in your browser. Your password goes only to Sony. While it waits, the app checks your browser's address bar and clipboard for Sony's redirect address, and links your account when it appears. If it doesn't link, copy the address from the address bar.
   - On the PS5, go to **Settings > System > Remote Play > Link Device** and type the 8-digit code into the app. Pairing starts by itself.
4. Press **Turn on** under **Audio** or **Video**.

You only pair once. After that, audio starts when you open the app, and you can turn video on whenever you need it. To choose yourself each time, turn off **Audio at startup** in Settings.

![Pairing page](docs/pair.png)

## Keeping your DualSense on the PS5

If you pair the app with the same PSN account you play on, the DualSense stops working on the PS5 while the app is connected.

To play with the DualSense on the PS5, pair the app with a **second PSN account** on the console. Your DualSense keeps working, and you don't need a controller on the PC.

The **DualSense on PS5** setting under **Advanced settings** stops the app from announcing a virtual controller, but this doesn't fix the problem on its own. It's on by default. Turn it off if connecting fails.

## Settings

![Settings page](docs/settings.png)

The PS5 address and account ID stay saved until you delete them. To use another account, press **Delete** on the Pairing page, sign in again and pair. Settings, pairing data and a log are stored in `%APPDATA%\PSStream`. The log (`log.txt`) is the first place to look if something goes wrong, and **Open logs** in Settings takes you there.

For the best results on Wi-Fi, connect the PS5 to your router with a cable, or use 5 GHz. If the picture breaks up, lower the quality to **Balanced** or **Low**.

## Updates

The app keeps itself up to date. At startup, it asks GitHub for the latest release number. That check sends no account or console data.

When there's a newer version, the app downloads it from GitHub, checks it against the checksum GitHub lists for that file, replaces its files and restarts. Your pairing and settings stay as they are. It never interrupts a game: it installs right after the app opens, or once audio and video are both off.

If the new version doesn't start, the old files go back and the old version keeps running. After three failed tries, the app stops installing that version by itself, and the **Update** button at the bottom lets you try again.

## Known limitations

- Windows only.
- Waking the PS5 needs the console's option to turn on over the network, and hasn't been tested on a real console yet.
- Local network only.
- Gamepad support is off by default and hasn't been tested on a real console yet. Turning it on announces a controller to the PS5, which can drop a DualSense connected to it. Gyro and the rest of the touchpad aren't supported.
- The PS5 sends some video even in audio-only mode, so the network use doesn't reach zero.
- 360p is the lowest resolution. A PS5 refused 144p when tested.
- Rates below 2 Mbps are outside what chiaki-ng offers. If the console refuses one, the app falls back to 2 Mbps.

## Windows Defender says it's a trojan

PS Stream isn't signed, because a code-signing certificate costs money every year. Microsoft Defender sometimes guesses that a new unsigned program is harmful and reports it as something like `Trojan:Win32/Bearfoos.A!ml`. The `!ml` at the end means the guess came from machine learning, not from finding known malware. Every release is built on GitHub from the code in this repository, which you can read, and the app checks every update against the checksum GitHub lists before installing it.

If Defender removes `PS Stream.exe`:

1. Open **Windows Security > Virus & threat protection > Protection history**.
2. Open the PS Stream entry, choose **Actions**, then **Restore** (or **Allow on device**).
3. If the app still doesn't start, download the latest zip from [Releases](../../releases/latest) and extract it over the old folder. Your pairing and settings stay.

To help others, you can report the file as safe at [Microsoft's submission page](https://www.microsoft.com/en-us/wdsi/filesubmission): choose **Home customer**, then **Incorrectly detected as malware/malicious**.

## Is it safe for my account?

This is an unofficial client, and Sony doesn't support it. It connects the same way as other open-source Remote Play clients such as chiaki-ng, and it doesn't modify your console or games. I don't know of any account bans caused by this, but I can't promise there is no risk, and using unofficial clients may conflict with PlayStation Network's terms. Use it at your own risk.

## Building from source

Install [MSYS2](https://www.msys2.org/) to `C:\msys64`, then install the packages from a MINGW64 shell:

```bash
pacman -S --needed make mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake mingw-w64-x86_64-ninja mingw-w64-x86_64-pkgconf mingw-w64-x86_64-curl mingw-w64-x86_64-json-c mingw-w64-x86_64-libevent mingw-w64-x86_64-miniupnpc mingw-w64-x86_64-openssl mingw-w64-x86_64-opus mingw-w64-x86_64-protobuf mingw-w64-x86_64-python mingw-w64-x86_64-python-protobuf mingw-w64-x86_64-SDL2 mingw-w64-x86_64-ffmpeg mingw-w64-x86_64-speexdsp
```

With [Git](https://git-scm.com/) on your `PATH`, run `build.cmd`. It downloads chiaki-ng v1.10.0, applies `patches/chiaki-ps-stream.patch` and builds the app.

Releases are built by GitHub Actions from the code in this repository ([`.github/workflows/release.yml`](.github/workflows/release.yml)). Every push is built to check that it compiles, and pushing a tag such as `v0.1.40` publishes that version. The tag has to match `APP_VERSION` in `src/main.c`, and `release-notes/v0.1.40.md` has to exist.

## Credits

PS Stream is built on [chiaki-ng](https://github.com/chiaki-ng/chiaki-ng)'s Remote Play library. It also uses SDL2, FFmpeg, Opus, OpenSSL, curl and json-c.

## Legal

### License

PS Stream is free software under the [GNU Affero General Public License v3 with the OpenSSL exception](LICENSE), the same license as chiaki-ng. You may use, study, share and change it. If you share a changed version, or run one for others over a network, you have to offer its source code under the same license.

### No warranty

PS Stream is provided "as is", without any warranty, to the extent the law allows. The authors aren't liable for any damage or loss from using it. Sections 15 and 16 of the [license](LICENSE) have the full terms.

### Trademarks

PS Stream is an independent, unofficial project. It isn't affiliated with, endorsed, sponsored or approved by Sony Interactive Entertainment. PlayStation, PS5, PlayStation Network, Remote Play and DualSense are trademarks or registered trademarks of Sony Interactive Entertainment Inc. Xbox is a trademark of Microsoft Corporation. Other names belong to their owners and are used only to describe compatibility.

### PlayStation Network terms

Using an unofficial client may go against the PlayStation Network Terms of Service. You're responsible for how you use your account. See [Is it safe for my account?](#is-it-safe-for-my-account)

### Privacy

PS Stream has no analytics, ads or tracking, and doesn't collect any data about you.

- **Your PS5:** the app talks to your console over your local network only.
- **Sony:** when you pair, you sign in on Sony's own page in your browser, and your password goes only to Sony. To catch Sony's redirect address, the app looks at your browser's address bar and the clipboard until pairing finishes, and at nothing else. Sony returns your account ID, which the app keeps to connect.
- **GitHub:** the app asks GitHub for the latest version number and downloads updates from it. GitHub sees your IP address, like any website. No account or console data is sent.
- **On your PC:** the PS5 address, your account ID, the pairing keys, your settings and a log are stored in `%APPDATA%\PSStream`. They never leave your PC. Delete that folder to remove them.

### Third-party software

The download includes libraries under their own licenses, among them FFmpeg (GPL), SDL2 (zlib), OpenSSL (Apache 2.0), Opus (BSD), curl (curl license) and json-c (MIT). `THIRD-PARTY-NOTICES.txt` in the download lists each one with its version, license and full license text, and says where to get the source code.
