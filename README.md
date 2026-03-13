# Zen Horizon Dashboard

A portable, old-school C++ application that mirrors the HTML version of the Zen Horizon Dashboard. Features a zen aesthetic with Japanese art backgrounds, falling sakura particles, live stock data, and rotating inspirational quotes.

## Features

- Fullscreen dashboard with Japanese art backgrounds that cycle every 5 minutes
- Live stock prices for OKLO, PLTR, CNXC, ACN from Finnhub API
- Rotating inspirational quotes in Japanese font
- 65 falling sakura particles for ambiance
- Neon glow effect on the title "禅の地平線"
- Toggle fullscreen with F11, quit with Escape
- Portable single binary that runs on any Linux machine

## Dependencies

All dependencies are free and installable via apt on Linux Mint XFCE:

- SDL2 (graphics and windowing)
- SDL2_ttf (text rendering)
- SDL2_image (image loading)
- libcurl (HTTP requests)
- nlohmann/json (header-only JSON parser)

## Build Instructions

### 1. Install Dependencies

Run these commands in your terminal:

```bash
sudo apt update
sudo apt install libsdl2-dev libsdl2-ttf-dev libsdl2-image-dev libcurl4-openssl-dev
```

### 2. Download nlohmann/json Header

```bash
wget https://github.com/nlohmann/json/releases/download/v3.11.2/json.hpp -O json.hpp
```

### 3. Download Fonts

Download the following fonts and place them in the `assets/` folder:

- [Noto Serif JP Regular](https://fonts.google.com/specimen/Noto+Serif+JP) - save as `NotoSerifJP-Regular.ttf`
- [Noto Sans JP Regular](https://fonts.google.com/specimen/Noto+Sans+JP) - save as `NotoSansJP-Regular.ttf`

### 4. Download Background Images

Download 8 Japanese art images from Unsplash and name them `bg1.jpg` to `bg8.jpg` in the `assets/` folder. Use these search terms for appropriate zen-themed images:

- Japanese landscape
- Zen garden
- Sakura trees
- Mountain temples
- Traditional Japanese art

### 5. Build the Application

```bash
make
```

This will produce the `zen-horizon` binary.

## Run Instructions

```bash
./zen-horizon
```

The application starts in fullscreen mode. Press F11 to toggle windowed mode, Escape to quit.

## Dropbox Sync

This project is designed to live in `/home/franklin/Dropbox/Development/ZenHorizon` for seamless sync across machines. The single binary can be dropped into Dropbox and run on any compatible Linux system without installation.

## Architecture

- Pure C++17 with no external frameworks
- SDL2 for cross-platform graphics and input
- libcurl for API requests
- nlohmann/json for JSON parsing
- 60 FPS game loop with proper timing
- Old-school procedural programming style

## API Key

The application uses a Finnhub API key for stock data. The key is hardcoded in `main.cpp` for simplicity. In a production environment, consider using environment variables or config files.

## Troubleshooting

- If fonts don't load, ensure they're in the `assets/` folder with correct names
- If images don't load, check file formats (JPG/PNG supported)
- If stock data doesn't update, check internet connection and API key validity
- For compilation errors, ensure all dependencies are installed correctly

## Future Enhancements

- Add system clock display
- Keyboard shortcuts for navigation
- Local-only mode without internet dependency
- Configurable refresh intervals
- More particle effects
