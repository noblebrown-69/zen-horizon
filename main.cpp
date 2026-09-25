// Zen Horizon 1.3 — quiet fullscreen idle dashboard.
//
// Drawing is a software framebuffer with cached glyphs and dirty rectangles, so a
// 1080p frame does not repaint the whole screen and text is not re-rasterized
// every tick. Stocks, weather, zip lookup, and the optional LLM health check
// run on one background thread; a slow API cannot stall the frame loop.

#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <SDL2/SDL_ttf.h>
#include <curl/curl.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <mutex>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include "json.hpp"

namespace {

constexpr const char* kVersion = "1.3";
constexpr float kPi = 3.14159265358979323846f;
constexpr int kDesignW = 1920;
constexpr int kDesignH = 1080;
constexpr int kPetals = 36;

using json = nlohmann::json;
using clock_type = std::chrono::steady_clock;

struct Stock {
    std::string symbol;
    double price = 0;
    double change = 0;
    bool valid = false;
};

struct WeatherDay {
    std::string label;
    int maxF = 0;
    int minF = 0;
    int precip = 0;
    bool showMin = false;
};

enum class LlmState { Hidden, Pending, Online, Offline, Loading };

struct SourceStatus {
    bool configured = true;
    bool attempted = false;
    bool ok = false;
    bool ever = false;
    clock_type::time_point updated{};
};

struct World {
    std::vector<Stock> stocks;
    SourceStatus stocksStatus;
    std::vector<WeatherDay> weather;
    SourceStatus weatherStatus;
    bool showLlm = false;
    LlmState llm = LlmState::Hidden;
    std::string llmLabel;
    std::uint64_t gen = 0;
};

struct Config {
    std::vector<std::string> stocks{"SPY", "QQQ", "VOO"};
    std::string zip = "85001";
    double lat = 33.4484;
    double lon = -112.0740;
    bool hasLatLon = false;
    bool fullscreen = true;
    int fps = 20;
    int stockSec = 120;
    int weatherSec = 600;
    int quoteSec = 1800;
    int bgSec = 300;
    int llmSec = 30;
    std::string llmUrl = "http://127.0.0.1:8080/health";
    std::string timezone = "auto";
};

struct Requests {
    bool stocks = false;
    bool weather = false;
    bool coords = false;
    bool llm = false;
    bool any() const { return stocks || weather || coords || llm; }
};

struct Glyph {
    std::string key;
    SDL_Surface* surf = nullptr;
    int w = 0;
    int h = 0;

    void clear() {
        if (surf) SDL_FreeSurface(surf);
        surf = nullptr;
        key.clear();
        w = 0;
        h = 0;
    }

    void prepare(TTF_Font* font, SDL_Color color, const std::string& text, int wrap, bool italic) {
        if (!font || text.empty()) {
            clear();
            return;
        }
        std::string next = text;
        next.push_back('|');
        next += std::to_string(wrap);
        next.push_back('|');
        next += italic ? 'i' : 'n';
        next.push_back('|');
        next += std::to_string(color.r);
        next.push_back(',');
        next += std::to_string(color.g);
        next.push_back(',');
        next += std::to_string(color.b);
        next.push_back('|');
        next += std::to_string(TTF_FontHeight(font));
        if (next == key && surf) return;

        clear();
        TTF_SetFontStyle(font, italic ? TTF_STYLE_ITALIC : TTF_STYLE_NORMAL);
        SDL_Surface* rendered = wrap > 0
            ? TTF_RenderUTF8_Blended_Wrapped(font, text.c_str(), color, static_cast<Uint32>(wrap))
            : TTF_RenderUTF8_Blended(font, text.c_str(), color);
        TTF_SetFontStyle(font, TTF_STYLE_NORMAL);
        if (!rendered) return;
        surf = rendered;
        w = rendered->w;
        h = rendered->h;
        key = std::move(next);
    }
};

struct Particle {
    float x, y, vx, vy;
};

struct HttpResult {
    bool ok = false;
    long code = 0;
    std::string body;
};

volatile std::sig_atomic_t gRunning = 1;

std::mutex gMu;
std::condition_variable gCv;
bool gQuit = false;
Config gCfg;
json gConfigDoc = json::object();
bool gConfigParseOk = true;
World gWorld;
Requests gReq;
std::thread gWorker;

int gFps = 20;
int gStockSec = 120;
int gWeatherSec = 600;
int gQuoteSec = 1800;
int gBgSec = 300;
int gLlmSec = 30;
std::string gLlmUrl = "http://127.0.0.1:8080/health";
std::string gTimezone = "auto";

SDL_Window* gWindow = nullptr;
SDL_Surface* gScreen = nullptr;
SDL_Surface* gScene = nullptr;
SDL_Surface* gFrame = nullptr;
SDL_Renderer* gRenderer = nullptr;
SDL_Texture* gFrameTex = nullptr;
bool gPresentSurface = false;
bool gFullscreen = true;
int gViewW = 1280;
int gViewH = 720;

TTF_Font* gFontTitle = nullptr;
TTF_Font* gFontText = nullptr;
TTF_Font* gFontSerif = nullptr;
TTF_Font* gFontClock = nullptr;
TTF_Font* gFontMeta = nullptr;
int gTitlePx = 0;
int gTextPx = 0;
int gSerifPx = 0;
int gClockPx = 0;
int gMetaPx = 0;

std::vector<SDL_Surface*> gBgOrig;
std::vector<SDL_Surface*> gBgReady;
int gScaledW = 0;
int gScaledH = 0;
int gBgIndex = 0;
int gNextBg = 0;
bool gFading = false;
int gFadeStep = -1;
clock_type::time_point gFadeStart{};
clock_type::time_point gLastBg{};
clock_type::time_point gLastQuote{};
int gQuoteIndex = 0;

SDL_Surface* gPetal = nullptr;
std::vector<Particle> gPetals;
std::vector<SDL_Rect> gPrevOverlay;
bool gSceneDirty = true;

Glyph gTitleGlyph;
Glyph gClockGlyph;
Glyph gStatusGlyph;
Glyph gQuoteGlyph;
Glyph gStocksFooter;
Glyph gWeatherFooter;
Glyph gLlmGlyph;
std::vector<Glyph> gStockSym;
std::vector<Glyph> gStockPrice;
std::vector<Glyph> gWeatherLines;

World gSnap;
std::string gSnapZip;
std::vector<std::string> gSnapSymbols;
std::uint64_t gSeenGen = 0;
int gStockBucket = -2;
int gWeatherBucket = -2;

const std::vector<std::string> kQuotes = {
    "The ultimate aim of karate lies not in victory or defeat, but in the perfection of the character of its participants. - Gichin Funakoshi",
    "Be like water making its way through cracks. Do not be assertive, but adjust to the object, and you shall find a way around or through it. - Bruce Lee",
    "The plateau is the beginning of despair for many. But for those who persist, it becomes the foundation of mastery. - Bruce Lee",
    "Empty your mind, be formless, shapeless—like water. - Bruce Lee",
    "Do not pray for an easy life, pray for the strength to endure a difficult one. - Bruce Lee",
    "The journey of a thousand miles begins with a single step. - Lao Tzu",
    "Simplicity is the ultimate sophistication. - Leonardo da Vinci",
    "The only way to do great work is to love what you do. - Steve Jobs",
    "In the middle of difficulty lies opportunity. - Albert Einstein",
    "The best way to predict the future is to create it. - Peter Drucker"
};

const SDL_Color kWhite{255, 255, 255, 255};
const SDL_Color kDim{168, 168, 174, 255};
const SDL_Color kStale{186, 164, 132, 255};
const SDL_Color kUp{118, 176, 128, 255};
const SDL_Color kDown{196, 122, 114, 255};
const SDL_Color kOnline{126, 168, 132, 255};
const SDL_Color kOffline{168, 132, 120, 255};
const SDL_Color kLoading{176, 160, 120, 255};

void onSignal(int) { gRunning = 0; }

std::string trim(std::string s) {
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    const auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

std::string urlEncode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out.push_back(static_cast<char>(c));
        else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 15]);
        }
    }
    return out;
}

std::string shellQuote(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out.push_back(c);
    }
    out.push_back('\'');
    return out;
}

int clampInt(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

int jsonInt(const json& j, const char* key, int fallback, int lo, int hi) {
    if (!j.contains(key) || !j[key].is_number()) return fallback;
    const double v = j[key].get<double>();
    if (!std::isfinite(v)) return fallback;
    return clampInt(static_cast<int>(std::lround(v)), lo, hi);
}

std::string upperCopy(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

bool goodSymbol(const std::string& s) {
    if (s.empty() || s.size() > 12) return false;
    for (unsigned char c : s) {
        if (!(std::isalnum(c) || c == '.' || c == '-')) return false;
    }
    return true;
}

std::string keyFilePath() {
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME")) {
        if (*xdg) return std::string(xdg) + "/zen-horizon/finnhub.key";
    }
    if (const char* home = std::getenv("HOME")) {
        if (*home) return std::string(home) + "/.config/zen-horizon/finnhub.key";
    }
    return "";
}

std::string loadFinnhubKey() {
    if (const char* env = std::getenv("FINNHUB_API_KEY")) {
        std::string key = trim(env);
        if (!key.empty()) return key;
    }
    const std::string path = keyFilePath();
    if (path.empty()) return "";
    std::ifstream in(path);
    if (!in) return "";
    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        return line;
    }
    return "";
}

void chdirToExe() {
    char buf[4096];
    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return;
    buf[n] = '\0';
    std::string path(buf);
    const auto slash = path.find_last_of('/');
    if (slash == std::string::npos || slash == 0) return;
    if (::chdir(path.substr(0, slash).c_str()) != 0) {
        std::perror("chdir");
    }
}

int scaleX(int x) { return static_cast<int>(std::lround(static_cast<double>(x) * gViewW / kDesignW)); }
int scaleY(int y) { return static_cast<int>(std::lround(static_cast<double>(y) * gViewH / kDesignH)); }

float fontScale() {
    float s = std::min(gViewW / static_cast<float>(kDesignW), gViewH / static_cast<float>(kDesignH));
    if (s < 0.6f) s = 0.6f;
    if (s > 2.5f) s = 2.5f;
    return s;
}

int px(int base) { return std::max(11, static_cast<int>(std::lround(base * fontScale()))); }

SDL_Rect clipRect(SDL_Rect r, int w, int h) {
    if (r.x < 0) {
        r.w += r.x;
        r.x = 0;
    }
    if (r.y < 0) {
        r.h += r.y;
        r.y = 0;
    }
    if (r.x + r.w > w) r.w = w - r.x;
    if (r.y + r.h > h) r.h = h - r.y;
    if (r.w < 0) r.w = 0;
    if (r.h < 0) r.h = 0;
    return r;
}

SDL_Rect inflate(SDL_Rect r, int m) {
    r.x -= m;
    r.y -= m;
    r.w += m * 2;
    r.h += m * 2;
    return r;
}

void blitAt(SDL_Surface* src, SDL_Surface* dst, int x, int y) {
    if (!src || !dst) return;
    SDL_Rect d{x, y, src->w, src->h};
    SDL_BlitSurface(src, nullptr, dst, &d);
}

SDL_Surface* makeLikeScreen(int w, int h) {
    if (w <= 0 || h <= 0) return nullptr;
    if (gScreen && gScreen->format) {
        return SDL_CreateRGBSurfaceWithFormat(0, w, h, gScreen->format->BitsPerPixel, gScreen->format->format);
    }
    return SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
}

SDL_Surface* makeArgb(int w, int h) {
    if (w <= 0 || h <= 0) return nullptr;
    return SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
}

void fillBlend(SDL_Surface* dst, SDL_Rect r, Uint8 R, Uint8 G, Uint8 B, Uint8 A) {
    r = clipRect(r, dst ? dst->w : 0, dst ? dst->h : 0);
    if (!dst || r.w <= 0 || r.h <= 0) return;
    SDL_Surface* box = makeArgb(r.w, r.h);
    if (!box) return;
    SDL_FillRect(box, nullptr, SDL_MapRGBA(box->format, R, G, B, A));
    SDL_SetSurfaceBlendMode(box, SDL_BLENDMODE_BLEND);
    SDL_Rect d = r;
    SDL_BlitSurface(box, nullptr, dst, &d);
    SDL_FreeSurface(box);
}

void blitCover(SDL_Surface* src, SDL_Surface* dst) {
    if (!src || !dst || src->w <= 0 || src->h <= 0) return;
    const float scale = std::max(static_cast<float>(dst->w) / src->w, static_cast<float>(dst->h) / src->h);
    int sw = static_cast<int>(std::lround(dst->w / scale));
    int sh = static_cast<int>(std::lround(dst->h / scale));
    sw = clampInt(sw, 1, src->w);
    sh = clampInt(sh, 1, src->h);
    SDL_Rect sr{(src->w - sw) / 2, (src->h - sh) / 2, sw, sh};
    SDL_Rect dr{0, 0, dst->w, dst->h};
    SDL_BlitScaled(src, &sr, dst, &dr);
}

std::string formatQuote(const std::string& text) {
    const auto pos = text.rfind(" - ");
    if (pos == std::string::npos) return text;
    return "\"" + text.substr(0, pos) + "\"\n- " + text.substr(pos + 3);
}

std::string agoPhrase(clock_type::time_point t, bool stale) {
    auto sec = std::chrono::duration_cast<std::chrono::seconds>(clock_type::now() - t).count();
    if (sec < 0) sec = 0;
    std::string core;
    if (sec < 45) core = "updated just now";
    else if (sec < 90) core = "updated 1 min ago";
    else if (sec < 3600) core = "updated " + std::to_string(sec / 60) + " min ago";
    else if (sec < 7200) core = "updated 1 hour ago";
    else core = "updated " + std::to_string(sec / 3600) + " hours ago";
    if (stale) return "stale · " + core;
    return core;
}

int ageBucket(const SourceStatus& s) {
    if (!s.ever) return -1;
    const auto sec = std::chrono::duration_cast<std::chrono::seconds>(clock_type::now() - s.updated).count();
    if (sec < 45) return 0;
    return 1 + static_cast<int>(sec / 60);
}

std::string stocksFooterText(const SourceStatus& s) {
    if (!s.configured) return "stocks unavailable";
    if (!s.attempted) return "";
    if (!s.ever) return "stocks unavailable";
    return agoPhrase(s.updated, !s.ok);
}

std::string weatherFooterText(const SourceStatus& s) {
    if (!s.attempted) return "";
    if (!s.ever) return "weather unavailable";
    return agoPhrase(s.updated, !s.ok);
}

SDL_Color footerColor(const SourceStatus& s, bool missingConfig) {
    if (missingConfig || !s.ok || !s.ever) return kStale;
    return kDim;
}

int weekdayFromYmd(int y, int m, int d) {
    static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    if (m < 3) y -= 1;
    return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
}

bool parseYmd(const std::string& s, int& y, int& m, int& d) {
    if (s.size() < 10 || s[4] != '-' || s[7] != '-') return false;
    try {
        y = std::stoi(s.substr(0, 4));
        m = std::stoi(s.substr(5, 2));
        d = std::stoi(s.substr(8, 2));
    } catch (...) {
        return false;
    }
    return m >= 1 && m <= 12 && d >= 1 && d <= 31;
}

std::string niceModel(std::string p) {
    const auto slash = p.find_last_of("/\\");
    if (slash != std::string::npos) p = p.substr(slash + 1);
    if (p.size() > 5) {
        std::string ext = p.substr(p.size() - 5);
        for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (ext == ".gguf") p.resize(p.size() - 5);
    }
    const size_t cap = 36;
    if (p.size() <= cap) return p;
    std::string cut = p.substr(0, cap);
    while (!cut.empty() && static_cast<unsigned char>(cut.back()) >= 0x80) cut.pop_back();
    return cut + "…";
}

std::string modelFromJson(const json& j) {
    auto take = [](const json& v) -> std::string {
        if (!v.is_string()) return "";
        return niceModel(v.get<std::string>());
    };
    if (j.is_object()) {
        for (const char* key : {"model_path", "model", "model_alias"}) {
            if (!j.contains(key)) continue;
            std::string s = take(j[key]);
            if (!s.empty()) return s;
        }
    }
    if (j.is_array()) {
        for (const auto& slot : j) {
            if (!slot.is_object() || !slot.contains("model")) continue;
            std::string s = take(slot["model"]);
            if (!s.empty()) return s;
        }
    }
    return "";
}

std::string siblingEndpoint(std::string url, const char* leaf) {
    const auto q = url.find('?');
    if (q != std::string::npos) url.erase(q);
    while (!url.empty() && url.back() == '/') url.pop_back();
    const auto scheme = url.find("://");
    const auto path = url.find('/', scheme == std::string::npos ? 0 : scheme + 3);
    if (path == std::string::npos) return url + "/" + leaf;
    return url.substr(0, path) + "/" + leaf;
}

size_t writeCb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out = static_cast<std::string*>(userdata);
    const size_t n = size * nmemb;
    if (out->size() + n > 1024 * 1024) return 0;
    out->append(ptr, n);
    return n;
}

HttpResult httpGet(CURL* curl, const std::string& url, long timeoutSec) {
    HttpResult r;
    if (!curl || url.empty()) return r;
    curl_easy_reset(curl);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &r.body);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeoutSec);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, timeoutSec < 3 ? timeoutSec : 3L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 3L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "zen-horizon/1.3");
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
    const CURLcode rc = curl_easy_perform(curl);
    if (rc != CURLE_OK) return r;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &r.code);
    r.ok = r.code >= 200 && r.code < 300;
    return r;
}

void saveConfigLocked(bool userInitiated = false) {
    if (!gConfigParseOk && !userInitiated) return;
    if (userInitiated) gConfigParseOk = true;
    gConfigDoc["lat"] = gCfg.lat;
    gConfigDoc["lon"] = gCfg.lon;
    gConfigDoc["zip"] = gCfg.zip;
    gConfigDoc["stocks"] = gCfg.stocks;
    gConfigDoc["fullscreen"] = gCfg.fullscreen;
    gConfigDoc["fps"] = gCfg.fps;
    gConfigDoc["stock_refresh_sec"] = gCfg.stockSec;
    gConfigDoc["weather_refresh_sec"] = gCfg.weatherSec;
    gConfigDoc["quote_rotate_sec"] = gCfg.quoteSec;
    gConfigDoc["bg_cycle_sec"] = gCfg.bgSec;
    gConfigDoc["llm_health_url"] = gCfg.llmUrl;
    gConfigDoc["llm_refresh_sec"] = gCfg.llmSec;
    gConfigDoc["timezone"] = gCfg.timezone;
    std::ofstream out("config.json");
    if (!out) return;
    out << gConfigDoc.dump(4) << '\n';
}

void loadConfig(bool& fullscreen) {
    std::ifstream in("config.json");
    if (!in) {
        gCfg = Config{};
        fullscreen = gCfg.fullscreen;
        saveConfigLocked();
        return;
    }
    try {
        in >> gConfigDoc;
    } catch (const std::exception& e) {
        std::cerr << "config.json: " << e.what() << " (using defaults, not rewriting config.json)\n";
        gConfigDoc = json::object();
        gCfg = Config{};
        gConfigParseOk = false;
        fullscreen = gCfg.fullscreen;
        return;
    }
    if (!gConfigDoc.is_object()) gConfigDoc = json::object();

    Config c;
    if (gConfigDoc.contains("stocks") && gConfigDoc["stocks"].is_array()) {
        c.stocks.clear();
        for (const auto& item : gConfigDoc["stocks"]) {
            if (!item.is_string()) continue;
            std::string sym = upperCopy(trim(item.get<std::string>()));
            if (goodSymbol(sym)) c.stocks.push_back(sym);
        }
    }
    if (c.stocks.empty()) c.stocks = {"SPY", "QQQ", "VOO"};
    if (c.stocks.size() > 16) c.stocks.resize(16);

    if (gConfigDoc.contains("zip")) {
        if (gConfigDoc["zip"].is_string()) c.zip = trim(gConfigDoc["zip"].get<std::string>());
        else if (gConfigDoc["zip"].is_number_integer()) c.zip = std::to_string(gConfigDoc["zip"].get<long long>());
    }
    if (c.zip.size() > 5) c.zip = c.zip.substr(0, 5);

    if (gConfigDoc.contains("lat") && gConfigDoc.contains("lon") &&
        gConfigDoc["lat"].is_number() && gConfigDoc["lon"].is_number()) {
        c.lat = gConfigDoc["lat"].get<double>();
        c.lon = gConfigDoc["lon"].get<double>();
        if (std::isfinite(c.lat) && std::isfinite(c.lon)) c.hasLatLon = true;
    }
    if (gConfigDoc.contains("fullscreen") && gConfigDoc["fullscreen"].is_boolean()) {
        c.fullscreen = gConfigDoc["fullscreen"].get<bool>();
    }
    c.fps = jsonInt(gConfigDoc, "fps", c.fps, 1, 60);
    c.stockSec = jsonInt(gConfigDoc, "stock_refresh_sec", c.stockSec, 15, 86400);
    c.weatherSec = jsonInt(gConfigDoc, "weather_refresh_sec", c.weatherSec, 60, 86400);
    c.quoteSec = jsonInt(gConfigDoc, "quote_rotate_sec", c.quoteSec, 10, 86400);
    c.bgSec = jsonInt(gConfigDoc, "bg_cycle_sec", c.bgSec, 15, 86400);
    c.llmSec = jsonInt(gConfigDoc, "llm_refresh_sec", c.llmSec, 5, 86400);
    if (gConfigDoc.contains("llm_health_url") && gConfigDoc["llm_health_url"].is_string()) {
        c.llmUrl = trim(gConfigDoc["llm_health_url"].get<std::string>());
    }
    if (gConfigDoc.contains("timezone") && gConfigDoc["timezone"].is_string()) {
        std::string tz = trim(gConfigDoc["timezone"].get<std::string>());
        if (!tz.empty() && tz.size() < 64) c.timezone = tz;
    }
    gCfg = std::move(c);
    fullscreen = gCfg.fullscreen;
}

void publishGenLocked() { gWorld.gen++; }

void seedDisplayedStocksLocked(bool withKey) {
    gWorld.stocks.clear();
    gWorld.stocks.reserve(gCfg.stocks.size());
    for (const auto& sym : gCfg.stocks) gWorld.stocks.push_back(Stock{sym, 0, 0, false});
    gWorld.stocksStatus.configured = withKey;
    gWorld.stocksStatus.attempted = !withKey;
    gWorld.stocksStatus.ok = false;
    gWorld.stocksStatus.ever = false;
    publishGenLocked();
}

bool fetchCoords(CURL* curl, const std::string& zip, double& lat, double& lon) {
    if (zip.size() != 5) return false;
    for (unsigned char c : zip) {
        if (!std::isdigit(c)) return false;
    }
    const std::string url = "https://api.zippopotam.us/us/" + urlEncode(zip);
    const HttpResult res = httpGet(curl, url, 8);
    if (!res.ok) return false;
    try {
        const json j = json::parse(res.body);
        if (!j.contains("places") || !j["places"].is_array() || j["places"].empty()) return false;
        const auto& place = j["places"][0];
        if (!place.contains("latitude") || !place.contains("longitude")) return false;
        const double newLat = std::stod(place["latitude"].get<std::string>());
        const double newLon = std::stod(place["longitude"].get<std::string>());
        if (!std::isfinite(newLat) || !std::isfinite(newLon)) return false;
        lat = newLat;
        lon = newLon;
        return true;
    } catch (...) {
        return false;
    }
}

void fetchStocks(CURL* curl) {
    std::vector<std::string> symbols;
    {
        std::lock_guard<std::mutex> lk(gMu);
        symbols = gCfg.stocks;
    }
    const std::string key = loadFinnhubKey();
    if (key.empty()) {
        std::lock_guard<std::mutex> lk(gMu);
        seedDisplayedStocksLocked(false);
        return;
    }

    std::vector<Stock> rows;
    rows.reserve(symbols.size());
    bool anyHttp = false;
    bool anyFail = false;
    for (const auto& sym : symbols) {
        Stock row;
        row.symbol = sym;
        const std::string url = "https://finnhub.io/api/v1/quote?symbol=" + urlEncode(sym) + "&token=" + urlEncode(key);
        const HttpResult res = httpGet(curl, url, 6);
        if (!res.ok) {
            anyFail = true;
            rows.push_back(row);
            continue;
        }
        anyHttp = true;
        try {
            const json j = json::parse(res.body);
            if (j.contains("c") && j["c"].is_number()) {
                row.price = j["c"].get<double>();
                row.valid = true;
            }
            if (j.contains("dp") && j["dp"].is_number()) {
                row.change = j["dp"].get<double>();
                row.valid = true;
            }
        } catch (...) {
            row.valid = false;
        }
        rows.push_back(row);
    }

    std::lock_guard<std::mutex> lk(gMu);
    gWorld.stocksStatus.configured = true;
    gWorld.stocksStatus.attempted = true;
    if (!anyHttp) {
        gWorld.stocksStatus.ok = false;
        if (!gWorld.stocksStatus.ever) {
            gWorld.stocks.clear();
            for (const auto& sym : symbols) gWorld.stocks.push_back(Stock{sym, 0, 0, false});
        }
        publishGenLocked();
        return;
    }
    gWorld.stocks = std::move(rows);
    gWorld.stocksStatus.ok = !anyFail;
    gWorld.stocksStatus.ever = true;
    gWorld.stocksStatus.updated = clock_type::now();
    publishGenLocked();
}

int roundNumber(const json& v, int fallback) {
    if (!v.is_number()) return fallback;
    return static_cast<int>(std::lround(v.get<double>()));
}

void fetchWeather(CURL* curl) {
    double lat = 0;
    double lon = 0;
    bool has = false;
    {
        std::lock_guard<std::mutex> lk(gMu);
        lat = gCfg.lat;
        lon = gCfg.lon;
        has = gCfg.hasLatLon;
    }
    if (!has) {
        std::lock_guard<std::mutex> lk(gMu);
        gWorld.weatherStatus.attempted = true;
        gWorld.weatherStatus.ok = false;
        publishGenLocked();
        return;
    }

    std::ostringstream url;
    url.setf(std::ios::fixed);
    url.precision(4);
    url << "https://api.open-meteo.com/v1/forecast?latitude=" << lat
        << "&longitude=" << lon
        << "&daily=temperature_2m_max,temperature_2m_min,precipitation_probability_max"
        << "&temperature_unit=fahrenheit&timezone=" << urlEncode(gTimezone)
        << "&forecast_days=6";

    const HttpResult res = httpGet(curl, url.str(), 8);
    if (!res.ok) {
        std::lock_guard<std::mutex> lk(gMu);
        gWorld.weatherStatus.attempted = true;
        gWorld.weatherStatus.ok = false;
        publishGenLocked();
        return;
    }

    std::vector<WeatherDay> days;
    try {
        const json j = json::parse(res.body);
        if (!j.contains("daily") || !j["daily"].is_object()) throw std::runtime_error("no daily");
        const json& daily = j["daily"];
        const json& times = daily.at("time");
        const json& tmax = daily.at("temperature_2m_max");
        const json& tmin = daily.at("temperature_2m_min");
        const json& prec = daily.at("precipitation_probability_max");
        static const char* names[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
        const size_t n = std::min<size_t>(6, times.size());
        for (size_t i = 0; i < n; ++i) {
            if (!times[i].is_string()) continue;
            WeatherDay day;
            const std::string ymd = times[i].get<std::string>();
            int y = 0, m = 0, d = 0;
            if (i == 0) day.label = "Today";
            else if (i == 1) day.label = "Tomorrow";
            else if (parseYmd(ymd, y, m, d)) day.label = names[weekdayFromYmd(y, m, d)];
            else day.label = "Day";
            day.maxF = roundNumber(tmax.at(i), 0);
            day.minF = roundNumber(tmin.at(i), 0);
            day.precip = i < prec.size() ? roundNumber(prec.at(i), 0) : 0;
            day.showMin = (i == 0);
            days.push_back(day);
        }
    } catch (...) {
        std::lock_guard<std::mutex> lk(gMu);
        gWorld.weatherStatus.attempted = true;
        gWorld.weatherStatus.ok = false;
        publishGenLocked();
        return;
    }

    std::lock_guard<std::mutex> lk(gMu);
    if (days.empty()) {
        gWorld.weatherStatus.attempted = true;
        gWorld.weatherStatus.ok = false;
        publishGenLocked();
        return;
    }
    gWorld.weather = std::move(days);
    gWorld.weatherStatus.attempted = true;
    gWorld.weatherStatus.ok = true;
    gWorld.weatherStatus.ever = true;
    gWorld.weatherStatus.updated = clock_type::now();
    publishGenLocked();
}

void fetchLlm(CURL* curl) {
    if (gLlmUrl.empty()) {
        std::lock_guard<std::mutex> lk(gMu);
        gWorld.showLlm = false;
        gWorld.llm = LlmState::Hidden;
        publishGenLocked();
        return;
    }
    const HttpResult health = httpGet(curl, gLlmUrl, 2);
    LlmState state = LlmState::Offline;
    std::string label = "offline";
    if (health.code == 503) {
        state = LlmState::Loading;
        label = "loading";
    } else if (health.ok) {
        state = LlmState::Online;
        label = "llm";
        bool explicitOk = true;
        try {
            const json j = json::parse(health.body);
            if (j.contains("status") && j["status"].is_string()) {
                std::string status = j["status"].get<std::string>();
                for (char& c : status) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                explicitOk = (status == "ok" || status == "healthy" || status == "ready");
                if (status.find("load") != std::string::npos) {
                    state = LlmState::Loading;
                    label = "loading";
                    explicitOk = false;
                }
            }
        } catch (...) {
            std::string body = trim(health.body);
            for (char& c : body) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (!(body.empty() || body == "ok" || body.find("ok") != std::string::npos)) explicitOk = false;
        }
        if (state == LlmState::Online && explicitOk) {
            std::string model;
            const HttpResult props = httpGet(curl, siblingEndpoint(gLlmUrl, "props"), 2);
            if (props.ok) {
                try { model = modelFromJson(json::parse(props.body)); } catch (...) {}
            }
            if (model.empty()) {
                const HttpResult slots = httpGet(curl, siblingEndpoint(gLlmUrl, "slots"), 2);
                if (slots.ok) {
                    try { model = modelFromJson(json::parse(slots.body)); } catch (...) {}
                }
            }
            if (!model.empty()) label = model;
        } else if (state == LlmState::Online && !explicitOk) {
            state = LlmState::Offline;
            label = "offline";
        }
    }

    std::lock_guard<std::mutex> lk(gMu);
    gWorld.showLlm = true;
    gWorld.llm = state;
    gWorld.llmLabel = label;
    publishGenLocked();
}

void workerMain() {
    CURL* curl = curl_easy_init();
    auto nextStock = clock_type::now();
    auto nextWeather = clock_type::now();
    auto nextLlm = clock_type::now();
    auto nextCoords = clock_type::now();

    while (true) {
        bool doStock = false, doWeather = false, doLlm = false, doCoords = false;
        bool haveZip = false;
        {
            std::unique_lock<std::mutex> lk(gMu);
            if (gQuit) break;
            const auto now = clock_type::now();
            haveZip = !gCfg.zip.empty();
            doStock = gReq.stocks || now >= nextStock;
            doWeather = gReq.weather || now >= nextWeather;
            doLlm = !gLlmUrl.empty() && (gReq.llm || now >= nextLlm);
            doCoords = haveZip && (gReq.coords || now >= nextCoords);
            if (!doStock && !doWeather && !doLlm && !doCoords) {
                auto wake = nextStock;
                if (nextWeather < wake) wake = nextWeather;
                if (!gLlmUrl.empty() && nextLlm < wake) wake = nextLlm;
                if (haveZip && nextCoords < wake) wake = nextCoords;
                gCv.wait_until(lk, wake, [] { return gQuit || gReq.any(); });
                continue;
            }
            if (doStock) gReq.stocks = false;
            if (doWeather) gReq.weather = false;
            if (doLlm) gReq.llm = false;
            if (doCoords) gReq.coords = false;
        }

        {
            std::lock_guard<std::mutex> lk(gMu);
            if (gQuit) break;
        }

        if (doLlm) {
            fetchLlm(curl);
            nextLlm = clock_type::now() + std::chrono::seconds(gLlmSec);
        }
        if (doWeather) {
            bool has = false;
            {
                std::lock_guard<std::mutex> lk(gMu);
                has = gCfg.hasLatLon;
            }
            if (has) fetchWeather(curl);
            else if (!doCoords) {
                std::lock_guard<std::mutex> lk(gMu);
                gWorld.weatherStatus.attempted = true;
                gWorld.weatherStatus.ok = false;
                publishGenLocked();
            }
            nextWeather = clock_type::now() + std::chrono::seconds(gWeatherSec);
        }
        if (doCoords) {
            std::string zip;
            double oldLat = 0, oldLon = 0;
            bool had = false;
            {
                std::lock_guard<std::mutex> lk(gMu);
                zip = gCfg.zip;
                oldLat = gCfg.lat;
                oldLon = gCfg.lon;
                had = gCfg.hasLatLon;
            }
            double lat = 0, lon = 0;
            if (fetchCoords(curl, zip, lat, lon)) {
                const bool changed = !had || std::fabs(lat - oldLat) > 1e-4 || std::fabs(lon - oldLon) > 1e-4;
                {
                    std::lock_guard<std::mutex> lk(gMu);
                    gCfg.lat = lat;
                    gCfg.lon = lon;
                    gCfg.hasLatLon = true;
                    if (changed) saveConfigLocked();
                    if (changed) gReq.weather = true;
                }
                if (changed) {
                    gCv.notify_one();
                    nextWeather = clock_type::now();
                }
                nextCoords = clock_type::now() + std::chrono::hours(12);
            } else {
                if (!had) {
                    std::lock_guard<std::mutex> lk(gMu);
                    gWorld.weatherStatus.attempted = true;
                    gWorld.weatherStatus.ok = false;
                    publishGenLocked();
                }
                nextCoords = clock_type::now() + std::chrono::seconds(90);
            }
        }
        if (doStock) {
            fetchStocks(curl);
            nextStock = clock_type::now() + std::chrono::seconds(gStockSec);
        }
    }
    if (curl) curl_easy_cleanup(curl);
}

bool zenityEntry(const std::string& title, const std::string& text, const std::string& prefill, std::string& result) {
    const std::string cmd = "zenity --entry --title=" + shellQuote(title) +
                            " --text=" + shellQuote(text) +
                            " --entry-text=" + shellQuote(prefill);
    FILE* pipe = ::popen(cmd.c_str(), "r");
    if (!pipe) return false;
    std::string output;
    char buf[256];
    while (std::fgets(buf, sizeof(buf), pipe)) output += buf;
    const int rc = ::pclose(pipe);
    if (rc != 0) return false;
    result = trim(output);
    return !result.empty();
}

void editStocks() {
    std::string prefill;
    {
        std::lock_guard<std::mutex> lk(gMu);
        for (size_t i = 0; i < gCfg.stocks.size(); ++i) {
            if (i) prefill += ',';
            prefill += gCfg.stocks[i];
        }
    }
    std::string input;
    if (!zenityEntry("Stocks", "Enter comma-separated stock symbols:", prefill, input)) return;

    std::vector<std::string> symbols;
    std::stringstream ss(input);
    std::string token;
    while (std::getline(ss, token, ',')) {
        token = upperCopy(trim(token));
        if (goodSymbol(token)) symbols.push_back(token);
    }
    if (symbols.empty()) symbols = {"SPY", "QQQ", "VOO"};
    if (symbols.size() > 16) symbols.resize(16);

    const bool haveKey = !loadFinnhubKey().empty();
    {
        std::lock_guard<std::mutex> lk(gMu);
        gCfg.stocks = symbols;
        saveConfigLocked(true);
        seedDisplayedStocksLocked(haveKey);
        gReq.stocks = true;
    }
    gCv.notify_one();
}

void editWeather() {
    std::string prefill;
    {
        std::lock_guard<std::mutex> lk(gMu);
        prefill = gCfg.zip;
    }
    std::string input;
    if (!zenityEntry("Weather", "Enter US zip code:", prefill, input)) return;
    std::string zip;
    for (unsigned char c : input) {
        if (std::isdigit(c)) zip.push_back(static_cast<char>(c));
    }
    if (zip.size() < 5) return;
    zip = zip.substr(0, 5);

    {
        std::lock_guard<std::mutex> lk(gMu);
        gCfg.zip = zip;
        gWorld.weatherStatus.ok = false;
        publishGenLocked();
        saveConfigLocked(true);
        gReq.coords = true;
    }
    gCv.notify_one();
}

void clearGlyphs() {
    gTitleGlyph.clear();
    gClockGlyph.clear();
    gStatusGlyph.clear();
    gQuoteGlyph.clear();
    gStocksFooter.clear();
    gWeatherFooter.clear();
    gLlmGlyph.clear();
    for (auto& g : gStockSym) g.clear();
    for (auto& g : gStockPrice) g.clear();
    for (auto& g : gWeatherLines) g.clear();
    gStockSym.clear();
    gStockPrice.clear();
    gWeatherLines.clear();
}

void closeFonts() {
    if (gFontTitle) TTF_CloseFont(gFontTitle);
    if (gFontText) TTF_CloseFont(gFontText);
    if (gFontSerif) TTF_CloseFont(gFontSerif);
    if (gFontClock) TTF_CloseFont(gFontClock);
    if (gFontMeta) TTF_CloseFont(gFontMeta);
    gFontTitle = gFontText = gFontSerif = gFontClock = gFontMeta = nullptr;
    gTitlePx = gTextPx = gSerifPx = gClockPx = gMetaPx = 0;
}

bool ensureFonts() {
    const int title = px(72);
    const int text = px(19);
    const int serif = px(22);
    const int clock = px(24);
    const int meta = px(15);
    if (gFontTitle && title == gTitlePx && text == gTextPx && serif == gSerifPx && clock == gClockPx && meta == gMetaPx) {
        return true;
    }
    closeFonts();
    clearGlyphs();
    gFontTitle = TTF_OpenFont("assets/NotoSerifJP-Regular.ttf", title);
    gFontSerif = TTF_OpenFont("assets/NotoSerifJP-Regular.ttf", serif);
    gFontClock = TTF_OpenFont("assets/NotoSerifJP-Regular.ttf", clock);
    gFontText = TTF_OpenFont("assets/NotoSansJP-Regular.ttf", text);
    gFontMeta = TTF_OpenFont("assets/NotoSansJP-Regular.ttf", meta);
    if (!gFontTitle || !gFontText || !gFontSerif || !gFontClock || !gFontMeta) {
        std::cerr << "Font loading failed: " << TTF_GetError() << "\n";
        return false;
    }
    for (TTF_Font* font : {gFontTitle, gFontText, gFontSerif, gFontClock, gFontMeta}) {
        TTF_SetFontHinting(font, TTF_HINTING_LIGHT);
    }
    gTitlePx = title;
    gTextPx = text;
    gSerifPx = serif;
    gClockPx = clock;
    gMetaPx = meta;
    gSceneDirty = true;
    return true;
}

void freeScaledBackgrounds() {
    for (SDL_Surface* s : gBgReady) SDL_FreeSurface(s);
    gBgReady.clear();
    gScaledW = 0;
    gScaledH = 0;
}

void loadBackgrounds() {
    for (SDL_Surface* s : gBgOrig) SDL_FreeSurface(s);
    gBgOrig.clear();
    for (int i = 1; i <= 8; ++i) {
        const std::string path = "assets/bg" + std::to_string(i) + ".jpg";
        SDL_Surface* surf = IMG_Load(path.c_str());
        if (!surf) {
            std::cerr << "Image load failed: " << path << " - " << IMG_GetError() << "\n";
            continue;
        }
        gBgOrig.push_back(surf);
    }
}

void ensureScaledBackgrounds() {
    if (gViewW <= 0 || gViewH <= 0) return;
    if (gScaledW == gViewW && gScaledH == gViewH && gBgReady.size() == gBgOrig.size()) return;
    freeScaledBackgrounds();
    for (SDL_Surface* src : gBgOrig) {
        SDL_Surface* dst = makeArgb(gViewW, gViewH);
        if (!dst) continue;
        SDL_FillRect(dst, nullptr, SDL_MapRGBA(dst->format, 18, 20, 22, 255));
        blitCover(src, dst);
        SDL_SetSurfaceBlendMode(dst, SDL_BLENDMODE_NONE);
        gBgReady.push_back(dst);
    }
    gScaledW = gViewW;
    gScaledH = gViewH;
    if (gBgIndex >= static_cast<int>(gBgReady.size())) gBgIndex = 0;
    gSceneDirty = true;
}

void rebuildPetal() {
    const int d = std::max(3, px(4));
    if (gPetal && gPetal->w == d && gPetal->h == d) return;
    if (gPetal) SDL_FreeSurface(gPetal);
    gPetal = makeArgb(d, d);
    if (!gPetal) return;
    SDL_FillRect(gPetal, nullptr, SDL_MapRGBA(gPetal->format, 0, 0, 0, 0));
    if (SDL_LockSurface(gPetal) != 0) {
        SDL_FreeSurface(gPetal);
        gPetal = nullptr;
        return;
    }
    auto* pixels = static_cast<Uint32*>(gPetal->pixels);
    const int pitch = gPetal->pitch / 4;
    const int r = d / 2;
    const Uint32 color = SDL_MapRGBA(gPetal->format, 232, 186, 196, 150);
    for (int y = 0; y < d; ++y) {
        for (int x = 0; x < d; ++x) {
            const int dx = x - r;
            const int dy = y - r;
            if (dx * dx + dy * dy <= r * r) pixels[y * pitch + x] = color;
        }
    }
    SDL_UnlockSurface(gPetal);
    SDL_SetSurfaceBlendMode(gPetal, SDL_BLENDMODE_BLEND);
}

void initPetals() {
    gPetals.clear();
    std::mt19937 rng(static_cast<unsigned>(std::chrono::high_resolution_clock::now().time_since_epoch().count()));
    std::uniform_real_distribution<float> ux(0.f, static_cast<float>(std::max(gViewW, 1)));
    std::uniform_real_distribution<float> uy(0.f, static_cast<float>(std::max(gViewH, 1)));
    std::uniform_real_distribution<float> uvx(-18.f, 18.f);
    std::uniform_real_distribution<float> uvy(34.f, 72.f);
    gPetals.reserve(kPetals);
    for (int i = 0; i < kPetals; ++i) gPetals.push_back(Particle{ux(rng), uy(rng), uvx(rng), uvy(rng)});
}

void stepPetals(float dt) {
    if (gViewW <= 0 || gViewH <= 0) return;
    for (auto& p : gPetals) {
        p.x += p.vx * dt;
        p.y += p.vy * dt;
        if (p.y > gViewH) {
            p.y = -8.f;
            p.x = std::fmod(p.x + gViewW * 0.37f, static_cast<float>(gViewW));
            if (p.x < 0) p.x += gViewW;
        }
        if (p.x < -12.f) p.x = static_cast<float>(gViewW);
        if (p.x > gViewW + 12.f) p.x = -4.f;
    }
}

std::string clockText() {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_r(&now, &local);
    char buf[96];
    std::strftime(buf, sizeof(buf), "%Y年%m月%d日  %H:%M:%S", &local);
    return buf;
}

std::string dotLabel() {
    switch (gSnap.llm) {
        case LlmState::Online: return gSnap.llmLabel.empty() ? "llm" : gSnap.llmLabel;
        case LlmState::Loading: return "loading";
        case LlmState::Offline: return "offline";
        case LlmState::Pending: return "llm";
        case LlmState::Hidden: return "";
    }
    return "";
}

SDL_Color dotColor() {
    switch (gSnap.llm) {
        case LlmState::Online: return kOnline;
        case LlmState::Loading: return kLoading;
        case LlmState::Pending: return kDim;
        case LlmState::Offline: return kOffline;
        case LlmState::Hidden: return kDim;
    }
    return kDim;
}

void drawDot(SDL_Surface* dst, int cx, int cy, SDL_Color color) {
    const int d = std::max(6, px(8));
    SDL_Surface* dot = makeArgb(d, d);
    if (!dot) return;
    SDL_FillRect(dot, nullptr, SDL_MapRGBA(dot->format, 0, 0, 0, 0));
    if (SDL_LockSurface(dot) != 0) {
        SDL_FreeSurface(dot);
        return;
    }
    auto* pixels = static_cast<Uint32*>(dot->pixels);
    const int pitch = dot->pitch / 4;
    const int r = d / 2;
    const Uint32 col = SDL_MapRGBA(dot->format, color.r, color.g, color.b, 230);
    for (int y = 0; y < d; ++y) {
        for (int x = 0; x < d; ++x) {
            const int dx = x - r;
            const int dy = y - r;
            if (dx * dx + dy * dy <= r * r) pixels[y * pitch + x] = col;
        }
    }
    SDL_UnlockSurface(dot);
    SDL_SetSurfaceBlendMode(dot, SDL_BLENDMODE_BLEND);
    blitAt(dot, dst, cx - r, cy - r);
    SDL_FreeSurface(dot);
}

void syncSnapshot() {
    std::lock_guard<std::mutex> lk(gMu);
    if (gWorld.gen == gSeenGen) return;
    gSnap = gWorld;
    gSnapZip = gCfg.zip;
    gSnapSymbols = gCfg.stocks;
    gSeenGen = gWorld.gen;
    gSceneDirty = true;
}

void tickQuoteAndBackground() {
    const auto now = clock_type::now();
    if (gQuoteSec > 0 && now - gLastQuote >= std::chrono::seconds(gQuoteSec)) {
        if (!kQuotes.empty()) gQuoteIndex = (gQuoteIndex + 1) % static_cast<int>(kQuotes.size());
        gLastQuote = now;
        gSceneDirty = true;
    }
    const int n = static_cast<int>(gBgOrig.size());
    if (n <= 1 || gBgSec <= 0) return;
    if (!gFading) {
        if (now - gLastBg >= std::chrono::seconds(gBgSec)) {
            gFading = true;
            gFadeStart = now;
            gFadeStep = -1;
            gNextBg = (gBgIndex + 1) % n;
            gLastBg = now;
        }
        return;
    }
    const float t = std::chrono::duration<float>(now - gFadeStart).count() / 1.6f;
    const int step = t >= 1.f ? 8 : static_cast<int>(t * 8.f);
    if (step != gFadeStep) {
        gFadeStep = step;
        gSceneDirty = true;
    }
    if (t >= 1.f) {
        gFading = false;
        gBgIndex = gNextBg;
        gFadeStep = -1;
        gSceneDirty = true;
    }
}

void tickAgeLabels() {
    const int sb = ageBucket(gSnap.stocksStatus);
    const int wb = ageBucket(gSnap.weatherStatus);
    if (sb != gStockBucket || wb != gWeatherBucket) {
        gStockBucket = sb;
        gWeatherBucket = wb;
        gSceneDirty = true;
    }
}

void prepareLines() {
    gStockSym.resize(gSnap.stocks.size());
    gStockPrice.resize(gSnap.stocks.size());
    for (size_t i = 0; i < gSnap.stocks.size(); ++i) {
        const Stock& s = gSnap.stocks[i];
        gStockSym[i].prepare(gFontText, kWhite, s.symbol + "   ", 0, false);
        if (s.valid) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%.2f  (%.1f%%)", s.price, s.change);
            const SDL_Color col = s.change >= 0 ? kUp : kDown;
            gStockPrice[i].prepare(gFontText, col, buf, 0, false);
        } else {
            gStockPrice[i].prepare(gFontText, kDim, "—", 0, false);
        }
    }

    const std::string stockFoot = stocksFooterText(gSnap.stocksStatus);
    gStocksFooter.prepare(gFontMeta, footerColor(gSnap.stocksStatus, !gSnap.stocksStatus.configured), stockFoot, 0, false);

    gWeatherLines.resize(gSnap.weather.size());
    for (size_t i = 0; i < gSnap.weather.size(); ++i) {
        const WeatherDay& day = gSnap.weather[i];
        std::ostringstream line;
        line << day.label << "   " << day.maxF << "°";
        if (day.showMin) line << " / " << day.minF << "°";
        line << "   ·   " << day.precip << "%";
        gWeatherLines[i].prepare(gFontText, kWhite, line.str(), 0, false);
    }
    const std::string weatherFoot = weatherFooterText(gSnap.weatherStatus);
    gWeatherFooter.prepare(gFontMeta, footerColor(gSnap.weatherStatus, false), weatherFoot, 0, false);

    const int quoteBoxW = std::max(scaleX(360), std::min(scaleX(520), gViewW * 32 / 100));
    const int wrap = std::max(80, quoteBoxW - scaleX(36));
    if (!kQuotes.empty()) {
        gQuoteGlyph.prepare(gFontSerif, kWhite, formatQuote(kQuotes[static_cast<size_t>(gQuoteIndex)]), wrap, true);
    }

    std::ostringstream status;
    status << "Current stocks: ";
    for (size_t i = 0; i < gSnapSymbols.size(); ++i) {
        if (i) status << ' ';
        status << gSnapSymbols[i];
    }
    status << "   |   Zip: " << gSnapZip << "   |   Press S for stocks, W for weather";
    gStatusGlyph.prepare(gFontText, kWhite, status.str(), 0, false);

    if (gSnap.showLlm) gLlmGlyph.prepare(gFontMeta, kDim, dotLabel(), 0, false);
    else gLlmGlyph.clear();
}

void rebuildScene() {
    if (!gScene) return;
    ensureScaledBackgrounds();
    if (!gBgReady.empty()) {
        const int idx = clampInt(gBgIndex, 0, static_cast<int>(gBgReady.size()) - 1);
        SDL_Rect full{0, 0, gViewW, gViewH};
        SDL_BlitSurface(gBgReady[static_cast<size_t>(idx)], nullptr, gScene, &full);
        if (gFading && gNextBg >= 0 && gNextBg < static_cast<int>(gBgReady.size()) && gFadeStep >= 0 && gFadeStep < 8) {
            SDL_Surface* next = gBgReady[static_cast<size_t>(gNextBg)];
            const Uint8 alpha = static_cast<Uint8>(std::lround((gFadeStep + 1) * 255.0 / 8.0));
            SDL_SetSurfaceBlendMode(next, SDL_BLENDMODE_BLEND);
            SDL_SetSurfaceAlphaMod(next, alpha);
            SDL_BlitSurface(next, nullptr, gScene, &full);
            SDL_SetSurfaceAlphaMod(next, 255);
            SDL_SetSurfaceBlendMode(next, SDL_BLENDMODE_NONE);
        }
    } else {
        SDL_FillRect(gScene, nullptr, SDL_MapRGB(gScene->format, 18, 20, 22));
    }

    prepareLines();
    const int padX = std::max(12, scaleX(18));
    const int padY = std::max(10, scaleY(16));
    const int lineStep = TTF_FontHeight(gFontText) + std::max(2, px(5));
    const int left = scaleX(50);
    const int top = scaleY(168);

    int stockInner = 0;
    for (size_t i = 0; i < gSnap.stocks.size(); ++i) {
        stockInner = std::max(stockInner, gStockSym[i].w + gStockPrice[i].w);
    }
    stockInner = std::max(stockInner, gStocksFooter.w);
    const int stockMax = std::max(scaleX(280), gViewW * 46 / 100);
    const int stockW = std::min(stockMax, std::max(scaleX(240), stockInner + padX * 2));
    const int stockRows = std::max(1, static_cast<int>(gSnap.stocks.size()));
    const int footH = std::max(gStocksFooter.h, TTF_FontHeight(gFontMeta));
    const int stockH = padY + stockRows * lineStep + footH + padY;
    SDL_Rect stockBox{left, top, stockW, stockH};
    fillBlend(gScene, stockBox, 220, 220, 225, 32);
    int y = stockBox.y + padY;
    for (size_t i = 0; i < gSnap.stocks.size(); ++i) {
        blitAt(gStockSym[i].surf, gScene, stockBox.x + padX, y);
        blitAt(gStockPrice[i].surf, gScene, stockBox.x + padX + gStockSym[i].w, y);
        y += lineStep;
    }
    if (gStocksFooter.surf) {
        blitAt(gStocksFooter.surf, gScene, stockBox.x + padX, stockBox.y + stockBox.h - padY - gStocksFooter.h);
    }

    const int quoteW = std::max(scaleX(360), std::min(scaleX(520), gViewW * 32 / 100));
    const int quoteH = std::max(scaleY(200), gQuoteGlyph.h + padY * 2);
    SDL_Rect quoteBox{gViewW - quoteW - scaleX(50), top, quoteW, quoteH};
    fillBlend(gScene, quoteBox, 220, 220, 225, 32);
    blitAt(gQuoteGlyph.surf, gScene, quoteBox.x + padX, quoteBox.y + padY);

    int weatherInner = gWeatherFooter.w;
    for (const auto& line : gWeatherLines) weatherInner = std::max(weatherInner, line.w);
    const int weatherMax = std::max(scaleX(260), gViewW * 34 / 100);
    const int weatherW = std::min(weatherMax, std::max(scaleX(280), weatherInner + padX * 2));
    const int weatherRows = std::max(1, static_cast<int>(gSnap.weather.size()));
    const int weatherH = padY + weatherRows * lineStep + footH + padY;
    const int margin = scaleX(28);
    SDL_Rect weatherBox{gViewW - weatherW - margin, gViewH - weatherH - scaleY(48), weatherW, weatherH};
    if (gSnap.weatherStatus.attempted || gSnap.weatherStatus.ever) {
        fillBlend(gScene, weatherBox, 220, 220, 225, 32);
        int wy = weatherBox.y + padY;
        for (const auto& line : gWeatherLines) {
            blitAt(line.surf, gScene, weatherBox.x + padX, wy);
            wy += lineStep;
        }
        if (gWeatherFooter.surf) {
            blitAt(gWeatherFooter.surf, gScene, weatherBox.x + padX, weatherBox.y + weatherBox.h - padY - gWeatherFooter.h);
        }
    }

    if (gStatusGlyph.surf) {
        const int sx = std::max(scaleX(20), (gViewW - gStatusGlyph.w) / 2);
        const int sy = gViewH - gStatusGlyph.h - scaleY(18);
        blitAt(gStatusGlyph.surf, gScene, sx, sy);
    }

    if (gSnap.showLlm && gLlmGlyph.surf) {
        const int dot = std::max(6, px(8));
        const int lx = left;
        const int ly = static_cast<int>(std::lround(gViewH * 0.90)) - gLlmGlyph.h - scaleY(10);
        drawDot(gScene, lx + dot / 2, ly + gLlmGlyph.h / 2, dotColor());
        blitAt(gLlmGlyph.surf, gScene, lx + dot + scaleX(10), ly);
    }
}

void drawOverlays(SDL_Surface* target, std::vector<SDL_Rect>& rects) {
    if (!target) return;
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(clock_type::now().time_since_epoch()).count();
    const float phase = static_cast<float>(ms % 4000) / 4000.f;
    const float s = 0.5f + 0.5f * std::sin(phase * 2.f * kPi);
    const Uint8 rg = static_cast<Uint8>(std::lround(170 + 85 * s));

    gTitleGlyph.prepare(gFontTitle, kWhite, "禅の地平線", 0, false);
    if (gTitleGlyph.surf) {
        SDL_SetSurfaceColorMod(gTitleGlyph.surf, rg, rg, 255);
        const int x = (gViewW - gTitleGlyph.w) / 2;
        const int y = scaleY(42);
        SDL_Rect r = inflate(SDL_Rect{x, y, gTitleGlyph.w, gTitleGlyph.h}, 2);
        r = clipRect(r, gViewW, gViewH);
        blitAt(gTitleGlyph.surf, target, x, y);
        SDL_SetSurfaceColorMod(gTitleGlyph.surf, 255, 255, 255);
        if (r.w > 0 && r.h > 0) rects.push_back(r);
    }

    gClockGlyph.prepare(gFontClock, kWhite, clockText(), 0, false);
    if (gClockGlyph.surf) {
        const int x = scaleX(50);
        const int y = static_cast<int>(std::lround(gViewH * 0.90));
        SDL_Rect r = inflate(SDL_Rect{x, y, gClockGlyph.w, gClockGlyph.h}, 2);
        r = clipRect(r, gViewW, gViewH);
        blitAt(gClockGlyph.surf, target, x, y);
        if (r.w > 0 && r.h > 0) rects.push_back(r);
    }

    if (gPetal) {
        for (const auto& p : gPetals) {
            const int x = static_cast<int>(p.x);
            const int y = static_cast<int>(p.y);
            SDL_Rect r = clipRect(SDL_Rect{x, y, gPetal->w, gPetal->h}, gViewW, gViewH);
            blitAt(gPetal, target, x, y);
            if (r.w > 0 && r.h > 0) rects.push_back(r);
        }
    }
}

void presentSurface(bool full, const std::vector<SDL_Rect>& rects) {
    if (!gWindow) return;
    if (full || rects.empty()) {
        SDL_UpdateWindowSurface(gWindow);
        return;
    }
    if (SDL_UpdateWindowSurfaceRects(gWindow, rects.data(), static_cast<int>(rects.size())) != 0) {
        SDL_UpdateWindowSurface(gWindow);
    }
}

void presentRenderer(SDL_Surface* frame) {
    if (!gRenderer || !frame) return;
    if (!gFrameTex || gViewW != frame->w || gViewH != frame->h) {
        if (gFrameTex) SDL_DestroyTexture(gFrameTex);
        gFrameTex = SDL_CreateTexture(gRenderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, frame->w, frame->h);
    }
    if (!gFrameTex) return;
    SDL_Surface* conv = frame;
    SDL_Surface* owned = nullptr;
    if (frame->format->format != SDL_PIXELFORMAT_ARGB8888) {
        owned = SDL_ConvertSurfaceFormat(frame, SDL_PIXELFORMAT_ARGB8888, 0);
        conv = owned ? owned : frame;
    }
    SDL_UpdateTexture(gFrameTex, nullptr, conv->pixels, conv->pitch);
    if (owned) SDL_FreeSurface(owned);
    SDL_RenderClear(gRenderer);
    SDL_RenderCopy(gRenderer, gFrameTex, nullptr, nullptr);
    SDL_RenderPresent(gRenderer);
}

void drawFrame() {
    if (!ensureFonts()) return;
    if (!gScene || gScene->w != gViewW || gScene->h != gViewH) {
        if (gScene) SDL_FreeSurface(gScene);
        gScene = makeLikeScreen(gViewW, gViewH);
        gSceneDirty = true;
    }
    if (!gScene) return;
    rebuildPetal();

    std::vector<SDL_Rect> overlay;
    if (gPresentSurface) {
        bool full = false;
        std::vector<SDL_Rect> dirty;
        if (gSceneDirty) {
            rebuildScene();
            gSceneDirty = false;
            SDL_Rect all{0, 0, gViewW, gViewH};
            SDL_BlitSurface(gScene, nullptr, gScreen, &all);
            full = true;
            gPrevOverlay.clear();
        } else {
            for (const SDL_Rect& r : gPrevOverlay) {
                SDL_Rect src = r;
                SDL_Rect dst = r;
                SDL_BlitSurface(gScene, &src, gScreen, &dst);
                dirty.push_back(r);
            }
        }
        drawOverlays(gScreen, overlay);
        for (const SDL_Rect& r : overlay) dirty.push_back(r);
        gPrevOverlay = overlay;
        presentSurface(full, dirty);
    } else {
        if (!gFrame || gFrame->w != gViewW || gFrame->h != gViewH) {
            if (gFrame) SDL_FreeSurface(gFrame);
            gFrame = makeLikeScreen(gViewW, gViewH);
        }
        if (gSceneDirty) {
            rebuildScene();
            gSceneDirty = false;
        }
        if (gFrame) {
            SDL_Rect all{0, 0, gViewW, gViewH};
            SDL_BlitSurface(gScene, nullptr, gFrame, &all);
            drawOverlays(gFrame, overlay);
            presentRenderer(gFrame);
        }
    }
}

void applyCursor() { SDL_ShowCursor(gFullscreen ? SDL_DISABLE : SDL_ENABLE); }

// Some sessions accept SDL_WINDOW_FULLSCREEN_DESKTOP but leave the X window at
// its creation size, so a 1080p layout is drawn into a 720p window and the
// clock falls off the bottom. If that happens, pin a borderless window to the
// display instead.
void pinFullscreenToDisplay() {
    if (!gWindow || !gFullscreen) return;
    SDL_DisplayMode mode{};
    const int display = SDL_GetWindowDisplayIndex(gWindow);
    if (display < 0 || SDL_GetCurrentDisplayMode(display, &mode) != 0 || mode.w < 16 || mode.h < 16) return;
    // Size changes are ignored while a fullscreen flag is set, so drop it,
    // resize to the display, then enter fullscreen again.
    SDL_SetWindowFullscreen(gWindow, 0);
    SDL_SetWindowBordered(gWindow, SDL_FALSE);
    SDL_SetWindowPosition(gWindow, 0, 0);
    SDL_SetWindowSize(gWindow, mode.w, mode.h);
    SDL_SetWindowFullscreen(gWindow, SDL_WINDOW_FULLSCREEN_DESKTOP);
}

void enterWindowed() {
    if (!gWindow) return;
    SDL_SetWindowFullscreen(gWindow, 0);
    SDL_SetWindowBordered(gWindow, SDL_TRUE);
    SDL_SetWindowSize(gWindow, 1280, 720);
    SDL_SetWindowPosition(gWindow, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
}

void noteViewSize() {
    if (gScreen) {
        gViewW = gScreen->w;
        gViewH = gScreen->h;
    } else if (gRenderer) {
        SDL_GetRendererOutputSize(gRenderer, &gViewW, &gViewH);
    }
}

void handleResize() {
    const int oldW = gViewW;
    const int oldH = gViewH;
    if (gPresentSurface) {
        gScreen = SDL_GetWindowSurface(gWindow);
        if (!gScreen) return;
    }
    noteViewSize();
    if (gViewW < 16 || gViewH < 16) return;
    if (gScene && gScene->w == gViewW && gScene->h == gViewH && oldW == gViewW && oldH == gViewH) return;
    if (gScene) SDL_FreeSurface(gScene);
    gScene = makeLikeScreen(gViewW, gViewH);
    if (gFrame) {
        SDL_FreeSurface(gFrame);
        gFrame = nullptr;
    }
    if (gFrameTex) {
        SDL_DestroyTexture(gFrameTex);
        gFrameTex = nullptr;
    }
    freeScaledBackgrounds();
    if (gPetal) {
        SDL_FreeSurface(gPetal);
        gPetal = nullptr;
    }
    const float sx = oldW > 0 ? static_cast<float>(gViewW) / oldW : 1.f;
    const float sy = oldH > 0 ? static_cast<float>(gViewH) / oldH : 1.f;
    if (gPetals.empty() || sx < 0.8f || sx > 1.25f || sy < 0.8f || sy > 1.25f) initPetals();
    else {
        for (auto& p : gPetals) {
            p.x *= sx;
            p.y *= sy;
        }
    }
    gPrevOverlay.clear();
    gSceneDirty = true;
}

bool initVideo(bool fullscreen) {
    SDL_SetHint(SDL_HINT_FRAMEBUFFER_ACCELERATION, "0");
    SDL_SetHint(SDL_HINT_RENDER_VSYNC, "0");
    SDL_SetHint(SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR, "1");
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        std::cerr << "SDL init failed: " << SDL_GetError() << "\n";
        return false;
    }
    SDL_DisableScreenSaver();
    Uint32 flags = SDL_WINDOW_RESIZABLE;
    int createW = 1280;
    int createH = 720;
    if (fullscreen) {
        flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
        SDL_DisplayMode mode{};
        if (SDL_GetCurrentDisplayMode(0, &mode) == 0 && mode.w >= 16 && mode.h >= 16) {
            createW = mode.w;
            createH = mode.h;
        }
    }
    gWindow = SDL_CreateWindow("Zen Horizon", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, createW, createH, flags);
    if (!gWindow && fullscreen) {
        std::cerr << "Fullscreen unavailable, opening a window instead\n";
        gFullscreen = false;
        gWindow = SDL_CreateWindow("Zen Horizon", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 720, SDL_WINDOW_RESIZABLE);
    }
    if (!gWindow) {
        std::cerr << "Window creation failed: " << SDL_GetError() << "\n";
        return false;
    }
    SDL_SetWindowMinimumSize(gWindow, 960, 540);
    pinFullscreenToDisplay();
    gScreen = SDL_GetWindowSurface(gWindow);
    if (gScreen && gScreen->w > 16 && gScreen->h > 16) {
        gPresentSurface = true;
        noteViewSize();
        return true;
    }
    gScreen = nullptr;
    gRenderer = SDL_CreateRenderer(gWindow, -1, SDL_RENDERER_SOFTWARE);
    if (!gRenderer) gRenderer = SDL_CreateRenderer(gWindow, -1, 0);
    if (!gRenderer) {
        std::cerr << "Renderer creation failed: " << SDL_GetError() << "\n";
        return false;
    }
    gPresentSurface = false;
    noteViewSize();
    std::cerr << "Window surface unavailable; using the software renderer\n";
    return gViewW > 16 && gViewH > 16;
}

void shutdownAll() {
    {
        std::lock_guard<std::mutex> lk(gMu);
        gQuit = true;
    }
    gCv.notify_all();
    if (gWorker.joinable()) gWorker.join();

    clearGlyphs();
    closeFonts();
    freeScaledBackgrounds();
    for (SDL_Surface* s : gBgOrig) SDL_FreeSurface(s);
    gBgOrig.clear();
    if (gPetal) SDL_FreeSurface(gPetal);
    if (gScene) SDL_FreeSurface(gScene);
    if (gFrame) SDL_FreeSurface(gFrame);
    if (gFrameTex) SDL_DestroyTexture(gFrameTex);
    if (gRenderer) SDL_DestroyRenderer(gRenderer);
    if (gWindow) SDL_DestroyWindow(gWindow);
    gPetal = nullptr;
    gScene = nullptr;
    gFrame = nullptr;
    gFrameTex = nullptr;
    gRenderer = nullptr;
    gWindow = nullptr;
    gScreen = nullptr;

    if (TTF_WasInit()) TTF_Quit();
    IMG_Quit();
    SDL_Quit();
    curl_global_cleanup();
}

enum class Parse { Ok, Help, Error };

Parse parseArgs(int argc, char** argv, int& fullscreenOverride) {
    fullscreenOverride = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            std::cout << "Zen Horizon " << kVersion << "\n\n"
                      << "Usage: zen-horizon [--fullscreen | --windowed]\n\n"
                      << "Reads config.json beside the executable.\n"
                      << "Finnhub key: $FINNHUB_API_KEY, or ~/.config/zen-horizon/finnhub.key\n"
                      << "Empty llm_health_url hides the local LLM status line.\n";
            return Parse::Help;
        }
        if (arg == "--windowed" || arg == "-w") fullscreenOverride = -1;
        else if (arg == "--fullscreen") fullscreenOverride = 1;
        else {
            std::cerr << "Unknown option: " << arg << "\n";
            return Parse::Error;
        }
    }
    return Parse::Ok;
}

int runLoop() {
    auto next = clock_type::now();
    auto prev = next;
    const auto frame = std::chrono::milliseconds(std::max(1, 1000 / gFps));
    while (gRunning) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) gRunning = 0;
            else if (event.type == SDL_WINDOWEVENT) {
                const Uint8 ev = event.window.event;
                if (ev == SDL_WINDOWEVENT_SIZE_CHANGED || ev == SDL_WINDOWEVENT_RESIZED) handleResize();
                else if (ev == SDL_WINDOWEVENT_EXPOSED) gSceneDirty = true;
            } else if (event.type == SDL_KEYDOWN && !event.key.repeat) {
                const SDL_Keycode key = event.key.keysym.sym;
                const SDL_Keymod mod = static_cast<SDL_Keymod>(event.key.keysym.mod);
                if (key == SDLK_ESCAPE) gRunning = 0;
                else if (key == SDLK_F11) {
                    gFullscreen = !gFullscreen;
                    if (gFullscreen) pinFullscreenToDisplay();
                    else enterWindowed();
                    applyCursor();
                    handleResize();
                } else if (!(mod & (KMOD_CTRL | KMOD_ALT | KMOD_GUI))) {
                    if (key == SDLK_s) editStocks();
                    else if (key == SDLK_w) editWeather();
                }
            }
        }
        if (!gRunning) break;

        const auto now = clock_type::now();
        float dt = std::chrono::duration<float>(now - prev).count();
        prev = now;
        if (dt < 0.f || dt > 0.25f) dt = 1.f / static_cast<float>(std::max(gFps, 1));
        stepPetals(dt);
        syncSnapshot();
        tickQuoteAndBackground();
        tickAgeLabels();
        drawFrame();

        next += frame;
        const auto after = clock_type::now();
        if (after > next) next = after;
        else std::this_thread::sleep_until(next);
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    int fullscreenOverride = 0;
    const Parse parsed = parseArgs(argc, argv, fullscreenOverride);
    if (parsed == Parse::Help) return 0;
    if (parsed == Parse::Error) return 2;

    chdirToExe();
    bool fullscreen = true;
    loadConfig(fullscreen);
    if (fullscreenOverride == 1) fullscreen = true;
    if (fullscreenOverride == -1) fullscreen = false;
    gFullscreen = fullscreen;
    gFps = gCfg.fps;
    gStockSec = gCfg.stockSec;
    gWeatherSec = gCfg.weatherSec;
    gQuoteSec = gCfg.quoteSec;
    gBgSec = gCfg.bgSec;
    gLlmSec = gCfg.llmSec;
    gLlmUrl = gCfg.llmUrl;
    gTimezone = gCfg.timezone;

    curl_global_init(CURL_GLOBAL_DEFAULT);
    if (TTF_Init() < 0) {
        std::cerr << "TTF init failed: " << TTF_GetError() << "\n";
        curl_global_cleanup();
        return 1;
    }
    if ((IMG_Init(IMG_INIT_JPG | IMG_INIT_PNG) & (IMG_INIT_JPG | IMG_INIT_PNG)) == 0) {
        std::cerr << "IMG init failed: " << IMG_GetError() << "\n";
        TTF_Quit();
        curl_global_cleanup();
        return 1;
    }
    if (!initVideo(fullscreen)) {
        shutdownAll();
        return 1;
    }
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    applyCursor();
    loadBackgrounds();
    handleResize();
    if (!ensureFonts()) {
        shutdownAll();
        return 1;
    }
    const bool haveKey = !loadFinnhubKey().empty();
    {
        std::lock_guard<std::mutex> lk(gMu);
        gWorld.showLlm = !gLlmUrl.empty();
        gWorld.llm = gLlmUrl.empty() ? LlmState::Hidden : LlmState::Pending;
        gWorld.llmLabel = "llm";
        seedDisplayedStocksLocked(haveKey);
    }
    gLastQuote = clock_type::now();
    gLastBg = gLastQuote;

    std::cerr << "Zen Horizon " << kVersion << " — " << gViewW << "x" << gViewH
              << (gFullscreen ? " fullscreen" : " windowed")
              << " @ " << gFps << " fps ("
              << (gPresentSurface ? "dirty-rect" : "software-renderer") << ")\n";
    if (!haveKey) std::cerr << "No Finnhub key; stocks will show as unavailable\n";

    gWorker = std::thread(workerMain);
    const int rc = runLoop();
    shutdownAll();
    return rc;
}
