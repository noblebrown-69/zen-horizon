# Zen Horizon

Quiet fullscreen idle dashboard for an always-on display. One C++17 binary, SDL2, no GPU required.

**Version:** 1.3

It is meant to sit on a 1920×1080 screen (an HP Z840 running Ubuntu 24.04 was the target) while a CPU-only llama.cpp server uses the machine. The frame loop is capped, text is cached, and steady-state drawing updates only the petals, the clock, and the title glow.

## What it shows

- Cycling Japanese art from `assets/`, scaled to the real window
- A short sakura drift
- Clock, rotating quotes
- Stocks (Finnhub) and a six-day forecast (Open-Meteo)
- Optional local LLM status: a small dot, online / offline / loading, and the model name when llama.cpp will give it up

ESC quits. F11 toggles fullscreen. S edits stocks and W edits the weather zip, both through zenity. The mouse cursor is hidden while fullscreen.

## Build

```bash
sudo apt install zenity libsdl2-dev libsdl2-ttf-dev libsdl2-image-dev libcurl4-openssl-dev
make
./zen-horizon
```

`make` uses `g++ -O2 -std=c++17 -Wall -Wextra` and links SDL2, SDL2_ttf, SDL2_image, and libcurl. The binary looks for `assets/` and `config.json` next to itself, so the working directory does not matter.

```bash
./zen-horizon --windowed    # config fullscreen, but this run is a window
./zen-horizon --fullscreen  # ignore a fullscreen:false config for this run
./zen-horizon --help
```

Startup prints the mode, for example `1920x1080 fullscreen @ 20 fps (dirty-rect)`. `dirty-rect` is the light path. If the log says `software-renderer`, the window surface was unavailable and every frame is uploaded whole; on a Wayland session, `SDL_VIDEODRIVER=x11` uses Xwayland and gets the light path back.

## Config

`config.json` beside the binary. Missing keys keep their defaults, so an older file that only has `lat`, `lon`, `stocks`, and `zip` still loads.

```json
{
    "lat": 33.6315,
    "lon": -112.052,
    "stocks": ["CNXC", "ACN", "PLTR", "OKLO"],
    "zip": "85022",
    "fullscreen": true,
    "fps": 20,
    "stock_refresh_sec": 120,
    "weather_refresh_sec": 600,
    "quote_rotate_sec": 1800,
    "bg_cycle_sec": 300,
    "llm_health_url": "http://127.0.0.1:8080/health",
    "llm_refresh_sec": 30,
    "timezone": "auto"
}
```

| Key | Default | Meaning |
| --- | --- | --- |
| `lat`, `lon` | Phoenix | Used immediately for weather. A zip lookup refreshes them in the background. |
| `stocks` | `SPY`, `QQQ`, `VOO` | Up to 16 symbols. Edited live with S. |
| `zip` | `85001` | US zip. Edited live with W. |
| `fullscreen` | `true` | Startup mode. F11 toggles the current session only. |
| `fps` | `20` | Frame cap, 1–60. 20 is enough for the drift and keeps the CPU down. |
| `stock_refresh_sec` | `120` | Finnhub poll. Floor is 15 seconds. |
| `weather_refresh_sec` | `600` | Open-Meteo poll. |
| `quote_rotate_sec` | `1800` | Quote rotation. |
| `bg_cycle_sec` | `300` | Background crossfade. |
| `llm_health_url` | `http://127.0.0.1:8080/health` | llama.cpp health URL. `""` hides the status line. |
| `llm_refresh_sec` | `30` | Health poll. |
| `timezone` | `auto` | Passed to Open-Meteo (`America/Phoenix` works too). |

S and W rewrite `config.json` and keep any unknown keys.

## Finnhub key

Nothing in this tree is a secret. The key is read at each poll from:

1. `FINNHUB_API_KEY`
2. `$XDG_CONFIG_HOME/zen-horizon/finnhub.key`, or `~/.config/zen-horizon/finnhub.key`

Blank lines and `#` comments in the file are ignored. If neither source has a key, the stocks panel stays up and the dim line says `stocks unavailable`. The dashboard does not exit.

```bash
mkdir -p ~/.config/zen-horizon
umask 077
printf '%s\n' 'YOUR_KEY' > ~/.config/zen-horizon/finnhub.key
```

A key used to be hardcoded. It is gone from the tree. It is still in git history, so rotate that token; this repo does not rewrite history.

## LLM line

When `llm_health_url` is non-empty, a background poll hits that URL about every 30 seconds. HTTP 200 with an ok status is online (muted green dot). HTTP 503 or a loading status is `loading`. Anything else is `offline`. While online, the same poll tries llama.cpp `/props` and, if that has no model, `/slots`, and shows a short model name when one is there.

An empty `llm_health_url` removes the line entirely. The default points at a local llama.cpp server, which matches the machine this display shares.

## Login autostart

`zen-horizon.desktop` is a template. Point `Exec` and `Path` at the install, then:

```bash
mkdir -p ~/.config/autostart
cp zen-horizon.desktop ~/.config/autostart/zen-horizon.desktop
# edit Exec and Path so they are absolute
```

GNOME reads `~/.config/autostart` at login. `X-GNOME-Autostart-Delay=4` gives the session a moment to come up. Put the Finnhub key in the file above rather than in `Exec`, so the desktop entry stays free of secrets.

On the Wayland session, if fullscreen lands on the wrong backend:

```ini
Exec=env SDL_VIDEODRIVER=x11 /home/USER/zen-horizon/zen-horizon
```

## How it stays light

Network I/O never runs on the frame thread. libcurl verifies TLS peers (`CURLOPT_SSL_VERIFYPEER` / `CURLOPT_SSL_VERIFYHOST`). Glyphs are SDL surfaces kept until the string, color, or size changes. Most frames only restore the previous petal, title, and clock rectangles and draw the new ones. A background change crossfades in a handful of full-frame steps, then goes quiet again. The screensaver is inhibited while the process runs.

On the build machine, a 1920×1080 Xvfb run in the dirty-rect path sat at about 1% of one core at the default 20 fps, with the background thread idle between polls. That is not a measurement from the Z840.

## Known issues

- Zenity is modal. The picture pauses while the S or W dialog is open.
- The crossfade is a few full-frame blits. Between those, only small rectangles change.
- Quotes are compiled in.
- Weather needs Open-Meteo and the zip lookup needs Zippopotam. A failure leaves the last good numbers up, with a dim `stale · updated …` line. If nothing has ever loaded, the line says unavailable.
- Finnhub's free tier is happier at the default two-minute poll than at the 15-second floor.
- Git history from before 1.3 still contains an old Finnhub token. Rotate it.
- If the startup line says `software-renderer` instead of `dirty-rect`, the process is redrawing the whole frame. Prefer the X11/Xwayland window surface on that machine.
