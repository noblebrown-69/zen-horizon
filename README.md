# Zen Horizon

Portable C++ idle dashboard (SDL2). One binary. Lives on this Dropbox copy and on `~/zen-horizon` on the Z840.

**Version:** 1.21 (`6fc9b58`) — weather forecast day names use real dates.  
**Last source check:** 2026-09-10 on franklin-HP-Z840-Workstation. Home and Dropbox trees match (`main.cpp` / `Makefile` identical). `config.json` is machine-local (lat/lon/stocks/zip) and is dirty vs git on both copies.

This is **not** the Minamoto AI stack. Ops notes for that: `Dropbox/Shoin/Docs/Development/AI/Minamoto/`.

## What it does

- Fullscreen-ish 1920×1080 SDL window (F11 toggles fullscreen, ESC quits)
- Live stocks via Finnhub (`S` opens a zenity editor)
- Weather ticker (`W` opens zenity; uses lat/lon/zip in `config.json`)
- Rotating quotes (Bruce Lee plateau included)
- Falling sakura, cycling Japanese art backgrounds in `assets/`
- Clock, lower-right weather

## Build

```bash
sudo apt install zenity libsdl2-dev libsdl2-ttf-dev libsdl2-image-dev libcurl4-openssl-dev
cd ~/zen-horizon   # or this Dropbox folder
make
./zen-horizon
```

`Makefile`: g++ `-O2 -std=c++17`, links SDL2 + SDL2_ttf + SDL2_image + libcurl.

## Config (`config.json`)

```json
{
  "lat": 33.6315,
  "lon": -112.052,
  "stocks": ["CNXC", "ACN", "PLTR", "OKLO", "SPCX"],
  "zip": "85022"
}
```

Do not commit a Finnhub key into git. The current `main.cpp` still has a **hardcoded Finnhub API key** — that is a known bug / cleanup item.

## Known bugs / unfinished (2026-09-10)

The README’s “one-time setup” list below the install block is leftover wishlist, not shipped:

- Add system clock display
- Keyboard shortcuts for navigation
- Local-only mode without internet
- Configurable refresh intervals
- More particle effects

Observed issues from the source:

1. **Hardcoded Finnhub key** in `main.cpp` (`API_KEY`). Rotate it and load from env or `config.json`.
2. **No error UI** if Finnhub/weather HTTP fails beyond `valid=false` on stocks.
3. **Fixed 1920×1080** — not a real dashboard on other resolutions without letterboxing.
4. **Refresh intervals baked in** (stocks 120s, weather 10 min, quotes 30 min, bg 5 min).
5. **Single-threaded curl in the render loop** can hitch the 60 FPS path when APIs stall.
6. Home and Dropbox both have a dirty `config.json` (Phoenix 85022 / those five tickers). That is expected per machine; don’t “fix” it by committing unless you want that as the default.

## Two copies

| Path | Role |
|------|------|
| `/home/franklin/zen-horizon` | Working tree on the Z840 |
| `/media/franklin/Storage/Dropbox/Development/ZenHorizon` | Dropbox copy (this folder) |

Keep source in Dropbox. Rebuild the binary on each machine (`make clean && make`) — do not rely on copying `zen-horizon` ELF across distros.

## Git

```
6fc9b58 v1.21: fixed weather forecast day names with real date calculation
849d61e v1.2: native zenity dialogs for stocks & weather
e70675f v1.1 final - dashboard complete
```

Branch: `main` tracking `origin/main`.
