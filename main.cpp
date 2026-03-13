#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <SDL2/SDL_image.h>
#include <curl/curl.h>
#include <string>
#include <vector>
#include <chrono>
#include <thread>
#include <iostream>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <cmath>
#include "json.hpp"

// WHAT: Constants define the application's core settings like window size, timing, and API details.
// WHY: Centralized configuration makes the code easier to maintain and modify for different setups.
const int WINDOW_WIDTH = 1920;
const int WINDOW_HEIGHT = 1080;
const int FPS = 60;
const int FRAME_DELAY = 1000 / FPS;
const std::string API_KEY = "d6kj03pr01qg51f446g0d6kj03pr01qg51f446gg";
const std::string FINNHUB_BASE_URL = "https://finnhub.io/api/v1/quote?symbol=";
const int STOCK_REFRESH_INTERVAL = 120000; // 120 seconds
const int WEATHER_REFRESH_INTERVAL = 600000; // 10 minutes
const int QUOTE_ROTATE_INTERVAL = 1800000; // 30 minutes
const int BG_CYCLE_INTERVAL = 300000; // 5 minutes
const int NUM_SAKURA = 65;
const float SAKURA_SPEED = 1.0f;
const int TITLE_GLOW_PERIOD = 2000; // 2 seconds

// WHAT: Quotes array contains inspirational quotes that rotate in the top-right box.
// WHY: Mirrors the exact quotes from the HTML version, including the Bruce Lee plateau quote.
const std::vector<std::string> QUOTES = {
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

// WHAT: Stock struct holds data for each stock symbol.
// WHY: Simple data structure to store price and change percentage.
struct Stock {
    std::string symbol;
    double price;
    double change;
};

// WHAT: WeatherDay struct holds daily weather data.
// WHY: Stores parsed weather info for display in the ticker.
struct WeatherDay {
    std::string dayName;
    int maxTemp;
    int minTemp;
    double precip;
};

// WHAT: Particle struct represents a sakura particle with position and velocity.
// WHY: Allows for realistic falling motion of the 65 sakura particles.
struct Particle {
    float x, y;
    float vx, vy;
};

// WHAT: Global variables for SDL resources and application state.
// WHY: Old-school C++ style avoids heavy classes, keeps everything accessible.
SDL_Window* window = nullptr;
SDL_Renderer* renderer = nullptr;
TTF_Font* titleFont = nullptr;
TTF_Font* textFont = nullptr;
TTF_Font* serifFont = nullptr;
TTF_Font* clockFont = nullptr;
std::vector<SDL_Texture*> bgTextures;
std::vector<Stock> stocks;
std::vector<WeatherDay> weather;
std::vector<Particle> sakuraParticles;
int currentQuoteIndex = 0;
int currentBgIndex = 0;
Uint32 lastStockUpdate = 0;
Uint32 lastQuoteRotate = 0;
Uint32 lastBgCycle = 0;
Uint32 lastWeatherUpdate = 0;
bool isFullscreen = true;
float bgAlpha = 1.0f;
int bgFadeDirection = 0;

// WHAT: WriteCallback function handles data received from libcurl.
// WHY: libcurl requires a callback to write response data into a string.
size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* s) {
    size_t newLength = size * nmemb;
    s->append((char*)contents, newLength);
    return newLength;
}

// WHAT: fetchStocks retrieves live stock data from Finnhub API.
// WHY: Provides real-time financial data for the stocks box, refreshing every 120 seconds.
void fetchStocks() {
    stocks.clear();
    std::vector<std::string> symbols = {"OKLO", "PLTR", "CNXC", "ACN"};
    
    for (const auto& symbol : symbols) {
        CURL* curl = curl_easy_init();
        if (curl) {
            std::string url = FINNHUB_BASE_URL + symbol + "&token=" + API_KEY;
            std::string response;
            
            curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
            
            CURLcode res = curl_easy_perform(curl);
            if (res == CURLE_OK) {
                try {
                    auto json = nlohmann::json::parse(response);
                    Stock stock;
                    stock.symbol = symbol;
                    stock.price = json["c"];
                    stock.change = json["dp"];
                    stocks.push_back(stock);
                } catch (const std::exception& e) {
                    std::cerr << "JSON error for " << symbol << ": " << e.what() << std::endl;
                }
            } else {
                std::cerr << "CURL error for " << symbol << ": " << curl_easy_strerror(res) << std::endl;
            }
            curl_easy_cleanup(curl);
        }
    }
}

// WHAT: fetchWeather retrieves 6-day weather forecast from Open-Meteo API for Phoenix.
// WHY: Provides weather data for the weather ticker, refreshing every 600 seconds (10 minutes).
// API change: Swapped precipitation_sum (mm) for precipitation_probability_max (%) to show chance of rain instead of amount.
// This teaches JSON field swapping fundamentals: changing API parameters for better zen dashboard relevance.
// Why % better: Probability gives zen uncertainty insight (e.g., 15% chance) vs. exact mm which feels too precise for contemplative vibe.
void fetchWeather() {
    weather.clear();
    std::string url = "https://api.open-meteo.com/v1/forecast?latitude=33.4484&longitude=-112.0740&daily=temperature_2m_max,temperature_2m_min,precipitation_probability_max&timezone=America%2FPhoenix&forecast_days=6";
    
    CURL* curl = curl_easy_init();
    if (curl) {
        std::string response;
        
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        
        CURLcode res = curl_easy_perform(curl);
        if (res == CURLE_OK) {
            try {
                auto json = nlohmann::json::parse(response);
                auto& daily = json["daily"];
                
                // Get day names for the next 6 days
                std::vector<std::string> dayNames = {"Today", "Tomorrow", "Wed", "Thu", "Fri", "Sat"};
                time_t current_time = ::time(nullptr);
                tm* tm_now = localtime(&current_time);

                for (size_t i = 0; i < 6 && i < daily["time"].size(); ++i) {
                    WeatherDay day;
                    day.dayName = dayNames[i];
                    day.maxTemp = daily["temperature_2m_max"][i];
                    day.minTemp = daily["temperature_2m_min"][i];
                    day.precip = daily["precipitation_probability_max"][i];
                    weather.push_back(day);
                }
            } catch (const std::exception& e) {
                std::cerr << "Weather JSON error: " << e.what() << std::endl;
            }
        } else {
            std::cerr << "Weather CURL error: " << curl_easy_strerror(res) << std::endl;
        }
        curl_easy_cleanup(curl);
    }
}

// WHAT: renderText creates an SDL_Texture from text using TTF.
// WHY: Enables rendering of Japanese text with proper fonts.
SDL_Texture* renderText(const std::string& text, TTF_Font* font, SDL_Color color) {
    SDL_Surface* surface = TTF_RenderUTF8_Blended(font, text.c_str(), color);
    if (!surface) return nullptr;
    SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer, surface);
    SDL_FreeSurface(surface);
    return texture;
}

// WHAT: renderWrappedText creates an SDL_Texture from text with word wrapping, italic styling, and formatted layout.
// WHY: Long quotes like the Bruce Lee plateau one need wrapping to fit in the box. TTF_RenderUTF8_Blended_Wrapped handles line breaks at word boundaries within the pixel width. TTF_SetFontStyle applies italic for elegant zen slant. Formatting adds quotation marks and \n-separated citation for clean layout, teaching SDL text layout fundamentals.
SDL_Texture* renderWrappedText(const std::string& text, TTF_Font* font, SDL_Color color, int wrapWidth) {
    // Format the quote with quotation marks and citation on new line, e.g., "quote"\n- author.
    // This teaches string manipulation for text layout: splitting at " - " and adding \n for multi-line elegance.
    std::string formattedText = text;
    size_t pos = text.rfind(" - ");
    if (pos != std::string::npos) {
        std::string quote = text.substr(0, pos);
        std::string author = text.substr(pos + 3);
        formattedText = "\"" + quote + "\"\n- " + author;
    }
    
    // Temporarily set italic style for elegant quote appearance.
    // TTF_SetFontStyle modifies the font's rendering style globally until reset, enabling italic text without permanent changes.
    TTF_SetFontStyle(font, TTF_STYLE_ITALIC);
    
    // TTF_RenderUTF8_Blended_Wrapped creates a surface with text wrapped to fit within wrapWidth pixels.
    // It automatically inserts line breaks at word boundaries to prevent overflow, using the font's natural line spacing.
    SDL_Surface* surface = TTF_RenderUTF8_Blended_Wrapped(font, formattedText.c_str(), color, wrapWidth);
    
    // Immediately reset font style to normal to avoid affecting other text rendering.
    // This reset trick ensures style changes are temporary and don't leak to other render calls.
    TTF_SetFontStyle(font, TTF_STYLE_NORMAL);
    
    if (!surface) return nullptr;
    
    // Convert surface to texture for hardware-accelerated rendering.
    SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer, surface);
    SDL_FreeSurface(surface); // Free the CPU surface after uploading to GPU.
    return texture;
}

// WHAT: init initializes SDL subsystems and loads all assets.
// WHY: Sets up the graphics environment and prepares resources for the main loop.
bool init() {
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        std::cerr << "SDL init failed: " << SDL_GetError() << std::endl;
        return false;
    }
    
    if (TTF_Init() == -1) {
        std::cerr << "TTF init failed: " << TTF_GetError() << std::endl;
        return false;
    }
    
    if (!(IMG_Init(IMG_INIT_PNG | IMG_INIT_JPG) & (IMG_INIT_PNG | IMG_INIT_JPG))) {
        std::cerr << "IMG init failed: " << IMG_GetError() << std::endl;
        return false;
    }
    
    window = SDL_CreateWindow("Zen Horizon Dashboard", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 
                              WINDOW_WIDTH, WINDOW_HEIGHT, SDL_WINDOW_FULLSCREEN_DESKTOP);
    if (!window) {
        std::cerr << "Window creation failed: " << SDL_GetError() << std::endl;
        return false;
    }
    
    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    if (!renderer) {
        std::cerr << "Renderer creation failed: " << SDL_GetError() << std::endl;
        return false;
    }
    
    titleFont = TTF_OpenFont("assets/NotoSerifJP-Regular.ttf", 72);
    textFont = TTF_OpenFont("assets/NotoSansJP-Regular.ttf", 19);
    serifFont = TTF_OpenFont("assets/NotoSerifJP-Regular.ttf", 22);
    clockFont = TTF_OpenFont("assets/NotoSerifJP-Regular.ttf", 24);
    if (!titleFont || !textFont || !serifFont || !clockFont) {
        std::cerr << "Font loading failed: " << TTF_GetError() << std::endl;
        return false;
    }
    
    for (int i = 1; i <= 8; ++i) {
        std::string path = "assets/bg" + std::to_string(i) + ".jpg";
        SDL_Surface* surface = IMG_Load(path.c_str());
        if (surface) {
            SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer, surface);
            bgTextures.push_back(texture);
            SDL_FreeSurface(surface);
        } else {
            std::cerr << "Image load failed: " << path << " - " << IMG_GetError() << std::endl;
        }
    }
    
    for (int i = 0; i < NUM_SAKURA; ++i) {
        Particle p;
        p.x = rand() % WINDOW_WIDTH;
        p.y = rand() % WINDOW_HEIGHT;
        p.vx = (rand() % 100 - 50) / 50.0f * SAKURA_SPEED;
        p.vy = (rand() % 50 + 50) / 50.0f * SAKURA_SPEED;
        sakuraParticles.push_back(p);
    }
    
    return true;
}

// WHAT: cleanup releases all SDL resources and shuts down subsystems.
// WHY: Prevents memory leaks and ensures clean exit.
void cleanup() {
    for (auto texture : bgTextures) SDL_DestroyTexture(texture);
    if (titleFont) TTF_CloseFont(titleFont);
    if (textFont) TTF_CloseFont(textFont);
    if (serifFont) TTF_CloseFont(serifFont);
    if (clockFont) TTF_CloseFont(clockFont);
    if (renderer) SDL_DestroyRenderer(renderer);
    if (window) SDL_DestroyWindow(window);
    TTF_Quit();
    IMG_Quit();
    SDL_Quit();
    curl_global_cleanup();
}

// WHAT: update handles all game logic updates based on elapsed time.
// WHY: Separates logic from rendering for clean architecture.
void update(Uint32 deltaTime) {
    Uint32 currentTime = SDL_GetTicks();
    
    if (currentTime - lastStockUpdate > STOCK_REFRESH_INTERVAL) {
        fetchStocks();
        lastStockUpdate = currentTime;
    }
    
    if (currentTime - lastWeatherUpdate > WEATHER_REFRESH_INTERVAL) {
        fetchWeather();
        lastWeatherUpdate = currentTime;
    }
    
    if (currentTime - lastQuoteRotate > QUOTE_ROTATE_INTERVAL) {
        currentQuoteIndex = (currentQuoteIndex + 1) % QUOTES.size();
        lastQuoteRotate = currentTime;
    }
    
    if (currentTime - lastBgCycle > BG_CYCLE_INTERVAL) {
        bgFadeDirection = -1;
        lastBgCycle = currentTime;
    }
    
    bgAlpha += bgFadeDirection * 0.005f;
    if (bgAlpha <= 0.0f) {
        currentBgIndex = (currentBgIndex + 1) % bgTextures.size();
        bgFadeDirection = 1;
    } else if (bgAlpha >= 1.0f) {
        bgFadeDirection = 0;
    }
    
    for (auto& p : sakuraParticles) {
        p.x += p.vx;
        p.y += p.vy;
        if (p.y > WINDOW_HEIGHT) {
            p.y = 0;
            p.x = rand() % WINDOW_WIDTH;
        }
        if (p.x < 0 || p.x > WINDOW_WIDTH) p.vx = -p.vx;
    }
}

// WHAT: render draws all visual elements to the screen.
// WHY: Handles the complete scene composition including backgrounds, particles, and UI.
void render() {
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);
    
    if (!bgTextures.empty()) {
        SDL_SetTextureAlphaMod(bgTextures[currentBgIndex], (Uint8)(bgAlpha * 255));
        SDL_RenderCopy(renderer, bgTextures[currentBgIndex], NULL, NULL);
    }
    
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    for (const auto& p : sakuraParticles) {
        SDL_SetRenderDrawColor(renderer, 255, 192, 203, 128);
        SDL_Rect rect = {(int)p.x, (int)p.y, 4, 4};
        SDL_RenderFillRect(renderer, &rect);
    }
    
    Uint32 time = SDL_GetTicks();
    float glow = 0.5f + 0.5f * sin(2 * M_PI * time / TITLE_GLOW_PERIOD);
    SDL_Color titleColor = {(Uint8)(255 * glow), (Uint8)(255 * glow), 255, 255};
    SDL_Texture* titleTexture = renderText("禅の地平線", titleFont, titleColor);
    if (titleTexture) {
        int w, h;
        SDL_QueryTexture(titleTexture, NULL, NULL, &w, &h);
        SDL_Rect dst = {WINDOW_WIDTH / 2 - w / 2, 50, w, h};
        SDL_RenderCopy(renderer, titleTexture, NULL, &dst);
        SDL_DestroyTexture(titleTexture);
    }

    // Clock repositioning: Moved from centered below title to lower-left corner at x=50, y=90% of screen height.
    // This teaches SDL positioning fundamentals: x=50 provides left margin, y=WINDOW_HEIGHT * 0.9 places it near bottom.
    // White-only color (255,255,255) ensures steady, readable text without glow/pulse for clean zen aesthetic.
    // No background box needed — clean white text overlays the art seamlessly.
    time_t current_time = ::time(nullptr);
    tm* tm_now = localtime(&current_time);
    char timeStr[100];
    strftime(timeStr, sizeof(timeStr), "%Y年%m月%d日 %H:%M:%S", tm_now);
    SDL_Color clockColor = {255, 255, 255, 255}; // Steady pure white, no glow
    SDL_Texture* clockTexture = renderText(timeStr, clockFont, clockColor);
    if (clockTexture) {
        int w, h;
        SDL_QueryTexture(clockTexture, NULL, NULL, &w, &h);
        SDL_Rect dst = {50, (int)(WINDOW_HEIGHT * 0.9), w, h}; // Lower-left positioning
        SDL_RenderCopy(renderer, clockTexture, NULL, &dst);
        SDL_DestroyTexture(clockTexture);
    }

    // Set blend mode for semi-transparent rendering, teaching SDL fundamentals: BLENDMODE_BLEND enables alpha compositing.
    // RGBA color (220,220,225,28) creates light smoky grey with very low opacity for subtle frosted effect.
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 220, 220, 225, 28);
    SDL_Rect stocksRect = {50, 150, 245, 200}; // Shrunk width by ~18% for compact look, tightened padding.
    SDL_RenderFillRect(renderer, &stocksRect);

    int y = 160;
    for (const auto& stock : stocks) {
        // Split rendering for conditional colors: symbols in pure white, price/% in green/red.
        // This teaches SDL rendering: separate textures allow different colors per text segment.
        std::string symbolPart = stock.symbol + ": $";
        SDL_Texture* symbolTexture = renderText(symbolPart, textFont, {255, 255, 255, 255});
        if (symbolTexture) {
            int symbolW, symbolH;
            SDL_QueryTexture(symbolTexture, NULL, NULL, &symbolW, &symbolH);
            SDL_Rect symbolDst = {55, y, symbolW, symbolH}; // Tightened padding from 60 to 55.
            SDL_RenderCopy(renderer, symbolTexture, NULL, &symbolDst);
            SDL_DestroyTexture(symbolTexture);

            // Price and change in conditional color: green for positive, red for negative.
            // Use %.2f formatting for prices like real money (e.g., $59.59), teaching C++ stream precision control.
            std::stringstream priceSS;
            priceSS << std::fixed << std::setprecision(1) << stock.change;
            std::stringstream priceStream;
            priceStream << std::fixed << std::setprecision(2) << stock.price;
            std::string pricePart = priceStream.str() + " (" + priceSS.str() + "%)";
            SDL_Color priceColor = stock.change >= 0 ? SDL_Color{0, 255, 0, 255} : SDL_Color{255, 0, 0, 255};
            SDL_Texture* priceTexture = renderText(pricePart, textFont, priceColor);
            if (priceTexture) {
                int priceW, priceH;
                SDL_QueryTexture(priceTexture, NULL, NULL, &priceW, &priceH);
                SDL_Rect priceDst = {55 + symbolW, y, priceW, priceH};
                SDL_RenderCopy(renderer, priceTexture, NULL, &priceDst);
                SDL_DestroyTexture(priceTexture);
            }
        }
        y += 24;
    }
    
    // Same blend mode and RGBA color for consistent frosted boxes across UI.
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 220, 220, 225, 28);
    SDL_Rect quotesRect = {WINDOW_WIDTH - 570, 150, 520, 200};
    SDL_RenderFillRect(renderer, &quotesRect);

    SDL_Texture* quoteTexture = renderWrappedText(QUOTES[currentQuoteIndex], serifFont, {255, 255, 255, 255}, 460);
    // Robust null-check fallback: if wrapped texture fails (e.g., font issues), fall back to single-line render.
    // This teaches SDL_ttf safety fundamentals: always check for null textures and provide fallbacks to prevent black rectangles.
    if (!quoteTexture) {
        // Set italic for fallback, render single line, then reset to avoid style leakage.
        TTF_SetFontStyle(serifFont, TTF_STYLE_ITALIC);
        quoteTexture = renderText(QUOTES[currentQuoteIndex], serifFont, {0, 0, 0, 255});
        TTF_SetFontStyle(serifFont, TTF_STYLE_NORMAL);
    }
    if (quoteTexture) {
        int w, h;
        SDL_QueryTexture(quoteTexture, NULL, NULL, &w, &h);
        SDL_Rect dst = {WINDOW_WIDTH - 560, 160, w, h};
        SDL_RenderCopy(renderer, quoteTexture, NULL, &dst);
        SDL_DestroyTexture(quoteTexture);
    }

    // Render weather ticker in lower-right with smoky background
    if (!weather.empty()) {
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(renderer, 220, 220, 225, 28);
        SDL_Rect weatherRect = {WINDOW_WIDTH - 320, WINDOW_HEIGHT - 180, 300, 160};
        SDL_RenderFillRect(renderer, &weatherRect);

        int y = WINDOW_HEIGHT - 170;
        for (size_t i = 0; i < weather.size(); ++i) {
            const auto& day = weather[i];
            // C-to-F conversion: Fahrenheit = (Celsius * 9/5) + 32, rounded to whole number with %.0f.
            // This teaches simple math fundamentals: linear temperature conversion formula, floating-point arithmetic, rounding for display.
            // Example: 32°C = (32 * 9/5) + 32 = 57.6 + 32 = 89.6 → 90°F with round().
            int fMax = round((day.maxTemp * 9.0 / 5.0) + 32.0);
            int fMin = round((day.minTemp * 9.0 / 5.0) + 32.0);
            std::stringstream ss;
            ss << day.dayName << ": " << fMax << "°";
            if (i == 0) {
                ss << "/" << fMin << "°";
            }
            ss << " • " << std::fixed << std::setprecision(0) << day.precip << "%";
            SDL_Texture* weatherTexture = renderText(ss.str(), textFont, {255, 255, 255, 255});
            if (weatherTexture) {
                int w, h;
                SDL_QueryTexture(weatherTexture, NULL, NULL, &w, &h);
                SDL_Rect dst = {WINDOW_WIDTH - 310, y, w, h};
                SDL_RenderCopy(renderer, weatherTexture, NULL, &dst);
                SDL_DestroyTexture(weatherTexture);
            }
            y += 24;
        }
    }

    SDL_RenderPresent(renderer);
}

// WHAT: main is the entry point and contains the classic game loop.
// WHY: Orchestrates initialization, event handling, updating, and rendering at 60 FPS.
int main(int argc, char* argv[]) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    
    if (!init()) {
        cleanup();
        return 1;
    }
    
    fetchStocks();
    fetchWeather();

    Uint32 frameStart;
    int frameTime;
    bool running = true;
    SDL_Event event;
    
    while (running) {
        frameStart = SDL_GetTicks();
        
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) running = false;
            else if (event.type == SDL_KEYDOWN) {
                if (event.key.keysym.sym == SDLK_ESCAPE) running = false;
                else if (event.key.keysym.sym == SDLK_F11) {
                    isFullscreen = !isFullscreen;
                    SDL_SetWindowFullscreen(window, isFullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
                }
            }
        }
        
        update(FRAME_DELAY);
        render();
        
        frameTime = SDL_GetTicks() - frameStart;
        if (FRAME_DELAY > frameTime) SDL_Delay(FRAME_DELAY - frameTime);
    }
    
    cleanup();
    return 0;
}