#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h> 
#include <ArduinoJson.h>
#include <vector>
#include <algorithm> 
#include <memory> 
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h> 
#include <Adafruit_GFX.h>
#include <time.h>     
#include <sys/time.h> 
#include <freertos/task.h> 
#include "zlib_turbo.h" 
#include "animations.h"
#include "secrets.h"
#include "chao_assets.h"
#include <Preferences.h>
#include <esp_task_wdt.h>

// --- 1. CONFIGURATION  ------------
uint16_t animationBuffer[4096];
// --- CACHE TIMERS (Defined in seconds) ---
const long TRANSIT_CACHE_TTL = 60;   // 1 minute
const long DRIVE_CACHE_TTL = 300;     // 5 minutes

const long TWILIGHT_OFFSET_SECONDS = 2100; // 35 minutes past sunset

// --- SYSTEM CONFIG ---
const char* MUNI_OPERATOR_ID = "SF"; 
const char* NTP_SERVER = "pool.ntp.org";
const char* TIMEZONE_INFO = "PST8PDT,M3.2.0,M11.1.0"; // Back to UTC
const size_t MAX_DECOMPRESSED_SIZE = 10240; 
const uint32_t API_TASK_STACK_SIZE = 32768; 

// --- 1. BUTTON PINS ---
const int BTN_MUNI_N_PIN = 42; const int BTN_MUNI_S_PIN = 41; 
const int BTN_BART_N_PIN = 40; const int BTN_BART_S_PIN = 39; 
const int BTN_DRIVE_A_PIN =  45; const int BTN_DRIVE_B_PIN = 47; 

// --- DISPLAY HARDWARE DEFINITIONS ---
#define MOCK_DISPLAY false // Set to false to use the real display
const int PANEL_RES_X = 64;  //
const int PANEL_RES_Y = 64; 

// Pinout
HUB75_I2S_CFG::i2s_pins _pins = {
    .r1 = 1,
    .g1 = 2,
    .b1 = 4,
    .r2 = 5,
    .g2 = 6,
    .b2 = 7,
    
    .a = 8,
    .b = 15,
    .c = 16,
    .d = 17,
    .e = 18,

    .lat = 19,
    .oe = 20,
    .clk = 21
};

// S3-compatible config
HUB75_I2S_CFG mxconfig(
  PANEL_RES_X,   // Panel width
  PANEL_RES_Y,   // Panel height
  1,             // Number of panels chained
  _pins          // Pin config
);

MatrixPanel_I2S_DMA *dma_display = nullptr;
// --- End of Display Config ---


// --- DATA STRUCTURES & COUNTERS ---
struct TrainPrediction { 
    int minutes; 
    String destination; 
    String lineInitial; 
    String colorHex; 
    String bartColorName; 
};
struct DriveDestination { String name; String originLat; String originLon; String destLat; String destLon; String travelMode; String colorHex; };

// --- ActiveMode set to MUNI_N default ---
enum ActiveMode { MUNI_N, MUNI_S, BART_N, BART_S, DRIVE_A, DRIVE_B };
ActiveMode currentMode = MUNI_N;

// --- Idle Screen State ---
enum IdleMode { IDLE_CLOCK, IDLE_WEATHER };
IdleMode currentIdleMode = IDLE_CLOCK; // Default to clock

// --- Sunrise/Sunset & Moon Data ---
time_t todaySunrise = 0;
time_t todaySunset = 0;
float currentMoonPhase = 0.0; // 0=New, 0.25=First, 0.5=Full, 0.75=Last

String currentMoonPhaseName = "";

// --- Weather Forecast Cache ---
struct WeatherForecast {
    time_t timestamp;
    int temp;
    String main;        // e.g., "Clouds", "Rain"
    String description; // e.g., "few clouds", "light rain"
    String timeString;
};
std::vector<WeatherForecast> weatherForecasts;


unsigned long lastWeatherFetchTime = 0;
const long WEATHER_CACHE_TTL = 300; // 5 minutes in seconds

// --- Travel Mode State ---
enum DriveMode { MODE_DRIVING, MODE_TRANSIT, MODE_BICYCLING, MODE_WALKING };
DriveMode currentDriveMode = MODE_DRIVING;
const String TRAVEL_MODE_API_NAMES[] = {"driving", "transit", "bicycling", "walking"};
const String TRAVEL_MODE_DISPLAY_NAMES[] = {"DRIVE", "METRO", "BIKE", "WALK"};
const String TRAVEL_MODE_TITLE_NAMES[] = {"Drive", "Metro", "Bike", "Walk"};
const int NUM_DRIVE_MODES = 4;
// --- End of Travel Variables ---

int muniNorthIndex = 0; int muniSouthIndex = 0;
int bartNorthIndex = 0; int bartSouthIndex = 0;
int driveAIndex = 0; int driveBIndex = 0;


// --- TAMAGOTCHI CONFIGURATION ---
bool isTamagotchiActive = false;
unsigned long lastPetUpdate = 0;
unsigned long lastPetDecay = 0;
const int PET_TICK_RATE = 200;    
const int PET_DECAY_RATE = 5000;  


// MENU CONSTANTS
const int ICON_FEED = 0;
const int ICON_LIGHT = 1;
const int ICON_PLAY = 2;
const int ICON_MEDICINE = 3;
const int ICON_BATHROOM = 4;
const int ICON_METER = 5;
const int ICON_DISCIPLINE = 6;
const int ICON_ATTENTION = 7; 

int selectedIcon = 0; 
bool isAttentionNeeded = false; 

bool screenDirty = true;

// --- EGG SELECTION DATA ---
// Top-Left (x1, y1) and Bottom-Right (x2, y2) for the 8 eggs
// Order: Default, Blue, Green, Yellow, Pink, Red, Silver, Shadow
const uint8_t EGG_COORDS[8][4] = {
    {3,  10, 16, 27}, // 0: Default
    {18, 10, 31, 27}, // 1: Dark Blue
    {33, 10, 46, 27}, // 2: Green
    {48, 10, 61, 27}, // 3: Yellow
    {3,  36, 16, 53}, // 4: Pink
    {18, 36, 31, 53}, // 5: Red
    {33, 36, 46, 53}, // 6: Silver
    {48, 36, 61, 53}  // 7: Shadow
};

// Game State config
enum GameState { 
    STATE_MAIN, 
    STATE_FEED_MENU, 
    STATE_STATS,
    STATE_PLAY_MENU,
    STATE_GAME_PLAY,
    STATE_CONNECT_3,
    STATE_FLAPPY_BIRD,
    STATE_MEDICINE_MENU,
    STATE_BATHROOM_MENU,
    STATE_SAVE_SELECT,
    STATE_EGG_SELECT,
    STATE_DEATH
};

int eggSelectionIndex = 0; // 0-7
GameState currentGameState = STATE_MAIN;
int subMenuSelection = 0;

// STATS & SLEEP TRACKING
int statsPage = 0; // 0=Hunger, 1=Happy, 2=Sleep
unsigned long lastSleepCheckTime = 0;

// --- TIMING CONSTANTS (IN SECONDS) ---
const long SECONDS_IN_12_HOURS = 43200; 
const long SECONDS_IN_YEAR = 86400;    // 1 Day = 1 Year
const long SECONDS_IN_DAY = 86400;     // Redudant but too lazy to switch now


// --- EGG PRICES ---
const int EGG_PRICES[8] = {0, 10, 25, 50, 75, 100, 250, 500};

struct PetStats {
    // --- CORE STATS ---
    int hunger;       
    int happiness;    
    int energy;       
    int discipline;   
    int sleepScore;   
    int colorID;

    // --- PHYSICAL STATS ---
    int weight;            
    time_t birthTime;      
    
    // --- ECONOMY ---
    int coins;             
    time_t lastCoinTime; 

    // --- VITALITY ---
    float vitality;        
    bool isDead;

    int alignment;   // 0 = Neutral, 1 = Angel, 2 = Devil
    bool hasEvolved;
    time_t evolutionStartTime;

    // --- TIMERS FOR ONE-TIME HITS ---
    bool hasTriggeredHungerWarn;    
    bool hasTriggeredHungerCrit;    
    bool hasTriggeredFunWarn;       
    bool hasTriggeredFunCrit;       
    bool hasTriggeredSick5Min;      
    bool hasTriggeredSick1Hr;       
    bool hasTriggeredNeglect1Hr;    

    // --- TIMERS FOR DAMAGE DURATION (Start of Event) ---
    time_t lastHungerTime;
    time_t zeroHungerTime;
    time_t zeroFunTime;
    time_t lastSicknessTime;
    time_t nextSickDamageTime;
    time_t lastShowerTime;
    time_t nextDirtyDamageTime;
    time_t lastMisbehaveTime;
    time_t nextDisciplineDamageTime;
    time_t lastNonWeightDamageTime; // Last time non-weight damage was taken (for 7-day streak)
    
    // --- TIMERS FOR IMMUNITY (End of Event) ---
    time_t lastCleanedTime;      // Set when you wash them
    time_t lastCuredTime;        // Set when you give medicine
    time_t lastDisciplinedTime;  // Set when you praise/scold
    
    // --- CHECKS ---
    time_t lastDailyCheckTime;      
    time_t lastEventCheckTime;

    // --- VISUALS ---
    int x; 
    int y; 
    int poseIndex;
    int frame;
    String status;  
    unsigned long actionStartTime;

    bool isSleeping;
    bool isSick;
    bool isDirty;
    bool isMisbehaving;
    bool isLightsOn;
    time_t lightsOffTime;        // Unix timestamp when the current darkness period started (0 = lights are on)
    long accumulatedDarkSeconds; // Total dark seconds accumulated across all light-off periods this sleep cycle
    time_t lightsOffDayTime;        // Unix timestamp when darkness started during the awake window (0 = lights on)
    long accumulatedDayDarkSeconds; // Total dark seconds accumulated during the awake window this day cycle
};

// INITIALIZATION
PetStats myPet = {
  100, 80, 80, 0, 6, 0,        // Core Stats
  10, 0,                       // Weight, Birth
  10, 0,                       // Coins, LastCoin
  100.0, false,                // Vitality, isDead
  0, false, 0,                  // Alignment, Evolved, EvoTime
  false, false, false, false, false, false, false, // 7 Trigger Bools
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, // 10 Loop Timers
  0, 0, 0,                     // 3 Immunity Timers
  0, 0,                        // Daily/Event Timers
  23, 23, 0, 0, "IDLE", 0,     // Position & Visuals
  false, false, false, false, true, 0, 0, 0, 0 // States + lightsOffTime + accumulatedDarkSeconds + lightsOffDayTime + accumulatedDayDarkSeconds
};

// --- Save File Globals ---
Preferences preferences;
String currentSaveSlot = ""; // "leah" or "juan"
int saveMenuSelection = 0;   // 0 = Leah, 1 = Juan
void applyDamage(float amount, bool isWeightDamage = false);

// --- MINI GAME VARIABLES ---
int gameCursorX = 0;
int gameDirection = 1; // 1 = Right, -1 = Left
int gameSpeed = 4;     // How many pixels to move per frame
unsigned long lastGameFrame = 0;
const int GAME_TICK_RATE = 50; // Fast updates for smooth animation

// --- CONNECT 3 VARIABLES ---
// 5 Columns x 5 Rows. 0: Empty, 1: Player, 2: CPU
int board[5][5] = {{0}};
int connect3CursorX = 2; // Middle column (0-4)
int playerTurn = 1;      // 1: Player, 2: CPU
bool gameActive = false;
unsigned long lastCpuMoveTime = 0;
const int CPU_MOVE_DELAY = 1000; // 1 second
int connect3LossStreak = 0; // Consecutive losses; +5 vitality at 3-in-a-row

// --- FLAPPY BIRD VARIABLES ---
struct FlappyPipe {
    int x;
    int gapY;    // Y center of the gap
    bool passed;
};
float flappyY = 32.0f;
float flappyVelY = 0.0f;
bool flappyActive = false;    // True while game is running (after first tap)
bool flappyDead = false;      // True after collision
int flappyScore = 0;
FlappyPipe flappyPipes[3];
int flappyActivePipes = 0;
int flappyPipeTimer = 0;      // Frames until next pipe spawns
const float FLAPPY_GRAVITY = 0.35f;
const float FLAPPY_FLAP_VEL = -3.2f;
const int FLAPPY_PIPE_W = 6;
const int FLAPPY_GAP_HALF = 11; // Half-gap: total gap = 22px
const int FLAPPY_CHAO_X = 12;  // Fixed horizontal position of the Chao
bool flappyGavePlayBonus = false;   // +5 happiness on first tap
bool flappyGave1PointBonus = false; // +20 happiness when score reaches 1
bool flappyGave10PointBonus = false;// +25 happiness when score reaches 10

// --- PLAY MENU VARIABLE ---
int playMenuSelection = 0; // 0: Catch, 1: Connect 3, 2: Flappy Bird

// --- BITMAP ASSETS ---
const uint8_t icons_8x8[8][8] = {
  {0x08, 0x3C, 0x7E, 0x7E, 0x7E, 0x7E, 0x7E, 0x3C}, // 0: FEED (Apple)
  {0x18, 0x3C, 0x7E, 0x7E, 0x7E, 0x3C, 0x18, 0x18}, // 1: LIGHT (Bulb)
  {0x2A, 0x3E, 0x1C, 0x1C, 0x1C, 0x1C, 0x3E, 0x7F}, // 2: PLAY (Rook)
  {0x3C, 0x18, 0x7E, 0x3C, 0x3C, 0x3C, 0x3C, 0x18}, // 3: MEDICINE (Syringe)
  {0x60, 0x40, 0x40, 0x40, 0xFF, 0xFF, 0xFF, 0x7E}, // 4: BATHROOM (Shower)
  {0x00, 0x7E, 0x00, 0x7E, 0x00, 0x7E, 0x00, 0x7E}, // 5: METER (List/Stats)
  {0x81, 0x42, 0x66, 0x00, 0x00, 0x00, 0x7E, 0x81}, // 6: DISCIPLINE (Face)
  {0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x00, 0x18}  // 7: ATTENTION (Exclamation)
};

// --- ASSETS ---
const uint8_t icon_skull[8] = {
  0x3C, // ..XXXX..
  0x7E, // .XXXXXX.
  0xDB, // XX.XX.XX (Eyes)
  0x7E, // .XXXXXX.
  0x3C, // ..XXXX..
  0x24, // ..X..X.. (Teeth)
  0x24, // ..X..X..
  0x00  // ........
};

// I don't think we use this anymore but oh well
const uint8_t icon_stink[8] = {
  0x00, 
  0x54, // .X.X.X..
  0x2A, // ..X.X.X.
  0x54, // .X.X.X..
  0x2A, // ..X.X.X.
  0x00, 
  0x00,
  0x00
};

const uint8_t heart_filled[8] = {0x00, 0x66, 0xFF, 0xFF, 0xFF, 0x7E, 0x3C, 0x18};
const uint8_t heart_empty[8]  = {0x00, 0x66, 0x99, 0x81, 0x81, 0x42, 0x24, 0x18};

// DRAW HELPER
void drawGenericBitmap(int x, int y, const uint8_t *bitmap, uint16_t color) {
    for (int row = 0; row < 8; row++) {
        for (int col = 0; col < 8; col++) {
            if (bitmap[row] & (1 << (7 - col))) {
                dma_display->drawPixel(x + col, y + row, color);
            }
        }
    }
}

// Simple 16x16 Pixel Art (Blob Pet)
// 1 = White outline, 2 = Color body, 0 = Transparent
const uint16_t pet_idle_1[256] = { /* Simplified for brevity, we will generate programmatically in render */ };



// *****************************************************************
// *** CACHE TIMERS (One per independent fetch) ***
unsigned long lastBartFetchTime = 0;
unsigned long lastMuniNorthFetchTime = 0;
unsigned long lastMuniSouthFetchTime = 0;
// ---  Cache timers are per-mode ---
unsigned long lastDriveAFetchTime[NUM_DRIVE_MODES] = {0, 0, 0, 0};
unsigned long lastDriveBFetchTime[NUM_DRIVE_MODES] = {0, 0, 0, 0};

// ---  Prediction vectors are per-mode ---
std::vector<TrainPrediction> muniNorthPredictions;
std::vector<TrainPrediction> muniSouthPredictions;
std::vector<TrainPrediction> bartNorthPredictions;
std::vector<TrainPrediction> bartSouthPredictions;
std::vector<TrainPrediction> driveAPredictions[NUM_DRIVE_MODES];
std::vector<TrainPrediction> driveBPredictions[NUM_DRIVE_MODES];
// *****************************************************************


// *** TASK CONTROL ***
bool isBooting = true;
TaskHandle_t apiTaskHandle = NULL;
portMUX_TYPE fetchFlagsMux = portMUX_INITIALIZER_UNLOCKED; // Mutex for flag safety
enum FetchFlag { FLAG_NONE = 0, FLAG_BART = 1, FLAG_MUNI_N = 2, FLAG_MUNI_S = 4, FLAG_DRIVE_A = 8, FLAG_DRIVE_B = 16 };
volatile uint8_t fetchFlags = FLAG_NONE; 
bool isMoonTime();

// Display logic flags
ActiveMode modeToDisplayAfterFetch = MUNI_N; 
volatile bool dataIsReadyToDisplay = false; 
volatile uint32_t activeFetchID = 0; 
volatile uint32_t dataFetchID = 0; 

// --- Animation State ---
volatile bool isFetchingMUNI = false; // Flag to control MUNI animation
int animationFrame = 0;
unsigned long lastFrameTime = 0;
const int ANIMATION_SPEED_MS = 300; // 300ms between frames

// --- BART Animation State ---
volatile bool isFetchingBART = false;
int bartAnimationFrame = 0;
unsigned long lastBARTFrameTime = 0;

// --- DRIVE Animation State ---
volatile bool isFetchingDRIVE = false;
int driveAnimationFrame = 0;
unsigned long lastDRIVEFrameTime = 0;

// --- METRO Animation State ---
volatile bool isFetchingMETRO = false;
int metroAnimationFrame = 0;
unsigned long lastMETROFrameTime = 0;

// --- BIKE Animation State ---
volatile bool isFetchingBIKE = false;
int bikeAnimationFrame = 0;
unsigned long lastBIKEFrameTime = 0;

// --- WALK Animation State ---
volatile bool isFetchingWALK = false;
int walkAnimationFrame = 0;
unsigned long lastWALKFrameTime = 0;

int appleTimeFrame = 0;
unsigned long lastAppleFrameTime = 0;

int clockAnimationFrame = 0;
unsigned long lastClockAnimFrameTime = 0;

// --- Brightness Control ---
const uint8_t BRIGHTNESS_LEVELS[] = {70, 45, 20}; // Bright, Medium, Dim
const char* BRIGHTNESS_NAMES[] = {"DIM 3", "DIM 2", "DIM 1"};
const int NUM_BRIGHTNESS_LEVELS = 3;
uint8_t currentBrightnessSetting = 0; // Start at 0 (Bright)

// --- Clock/Inactivity Timers ---
unsigned long lastActivityTime = 0;
const long INACTIVITY_TIMEOUT = 20000; // 20 seconds until clock appears
unsigned long lastClockRenderTime = 0;
bool isClockActive = false;
bool justExitedClock = false;
// *****************************************************************

// *************************************************************************
// *** DRIVE DESTINATION DATA ***
// *************************************************************************

const DriveDestination DRIVE_DATA_A[] = {
    {"JORDON", SF_HOME_LAT, SF_HOME_LON, JORDON_LAT, JORDON_LON, "driving", "#006324"}, 
    {"PARNELLA", SF_HOME_LAT, SF_HOME_LON, PARNELLA_LAT, PARNELLA_LON, "driving", "#7fa322"},
    {"MIKEBECCA", SF_HOME_LAT, SF_HOME_LON, MIKEBECCA_LAT, MIKEBECCA_LON, "driving", "#2600BD"},
    {"LEEVIKA", SF_HOME_LAT, SF_HOME_LON, LEEVIKA_LAT, LEEVIKA_LON, "driving", "#998FFF"},
    {"FLORA", SF_HOME_LAT, SF_HOME_LON, FLORA_LAT, FLORA_LON, "driving", "#8500D1"},
    {"JULIA", SF_HOME_LAT, SF_HOME_LON, JULIA_LAT, JULIA_LON, "driving", "#FF82D0"},
    {"MOM", SF_HOME_LAT, SF_HOME_LON, MOM_LAT, MOM_LON, "driving", "#FFE100"},
    {"DAD", SF_HOME_LAT, SF_HOME_LON, DAD_LAT, DAD_LON, "driving", "#00FF19"},
    {"EUNICE", SF_HOME_LAT, SF_HOME_LON, EUNICE_LAT, EUNICE_LON, "driving", "#FF9500"},
    {"MINA", SF_HOME_LAT, SF_HOME_LON, MINA_LAT, MINA_LON, "driving", "#A61E00"},
    {"PANTS JOHN", SF_HOME_LAT, SF_HOME_LON, JOHN_LAT, JOHN_LON, "driving", "#ffd500"},
    {"FARHAN", SF_HOME_LAT, SF_HOME_LON, FARHAN_LAT, FARHAN_LON, "driving", "#FF0000"},
    {"BERKELEY", SF_HOME_LAT, SF_HOME_LON, BERKELEY_LAT, BERKELEY_LON, "driving", "#FFFFFF"},
};
const int DRIVE_DATA_A_COUNT = 13;

const DriveDestination DRIVE_DATA_B[] = {
    {"JORDON", JORDON_LAT, JORDON_LON, SF_HOME_LAT, SF_HOME_LON, "driving", "#006324"}, 
    {"PARNELLA", PARNELLA_LAT, PARNELLA_LON, SF_HOME_LAT, SF_HOME_LON, "driving", "#7fa322"},
    {"MIKEBECCA", MIKEBECCA_LAT, MIKEBECCA_LON, SF_HOME_LAT, SF_HOME_LON, "driving", "#2600BD"},
    {"LEEVIKA", LEEVIKA_LAT, LEEVIKA_LON, SF_HOME_LAT, SF_HOME_LON, "driving", "#998FFF"},
    {"FLORA", FLORA_LAT, FLORA_LON, SF_HOME_LAT, SF_HOME_LON, "driving", "#8500D1"},
    {"JULIA", JULIA_LAT, JULIA_LON, SF_HOME_LAT, SF_HOME_LON, "driving", "#FF82D0"},
    {"MOM", MOM_LAT, MOM_LON, SF_HOME_LAT, SF_HOME_LON, "driving", "#FFE100"},
    {"DAD", DAD_LAT, DAD_LON, SF_HOME_LAT, SF_HOME_LON, "driving", "#00FF19"},
    {"EUNICE", EUNICE_LAT, EUNICE_LON, SF_HOME_LAT, SF_HOME_LON, "driving", "#FF9500"},
    {"MINA", MINA_LAT, MINA_LON, SF_HOME_LAT, SF_HOME_LON, "driving", "#A61E00"},
    {"PANTS JOHN", JOHN_LAT, JOHN_LON, SF_HOME_LAT, SF_HOME_LON, "driving", "#ffd500"},
    {"FARHAN", FARHAN_LAT, FARHAN_LON, SF_HOME_LAT, SF_HOME_LON, "driving", "#FF0000"},
    {"BERKELEY", BERKELEY_LAT, BERKELEY_LON, SF_HOME_LAT, SF_HOME_LON, "driving", "#FFFFFF"},
};
const int DRIVE_DATA_B_COUNT = 13; 

// --- FUNCTION PROTOTYPES ---
void renderDisplay(String line1, String line2, String colorHex = "#FFFFFF", String timeColorHex = "");
void resetOtherCounters(ActiveMode activeMode);
void processMode(ActiveMode newMode, int& currentIndex, const std::vector<TrainPrediction>& predictions, const String& modeName);
void processDriveMode(ActiveMode newMode, int& currentIndex, const std::vector<TrainPrediction>& predictions); 
void fetchMuniPredictionsForStop(const char* stop_id, std::vector<TrainPrediction>& predictions);
void fetchBARTData();
void fetchMuniNorth();
void fetchMuniSouth();
void fetchDriveData(const DriveDestination* destinations, int destCount, std::vector<TrainPrediction>& predictions, unsigned long& lastFetchTime, bool isDriveTo);
void fetchDriveA();
void fetchDriveB();
void api_fetch_task(void *pvParameters); 
bool isTimeSynced(); 
void executeFetchAndDisplay(ActiveMode requestedMode); 
void displayClock(); 
void displayWeatherForecast();
void fetchWeather(); 
void drawWeatherIcon(String main, String description, int x, int y);
void drawIdleAnimation(struct tm* timeinfo);
void animateMuniLoading();
void animateBARTLoading();
void animateDRIVELoading();
void animateMETROLoading();
void animateBIKELoading();
void animateWALKLoading();
void displayAppleTime();
void fetchAstronomyData();

String formatForecastTime(time_t timestamp);


void initConnect3();
int dropPiece(int player, int col);
bool checkWin(int player, int lastCol, int lastRow);
void cpuMove();
void initFlappyBird();


// --- HELPER FUNCTIONS ---
void executeDriveModeCycle();
bool isFetching();


// *************************************************************************
// *** CRITICAL HELPER FUNCTION DEFINITIONS ***
// *************************************************************************

// --- Returns true if any fetch is active ---
bool isFetching() {
    return isFetchingMUNI || isFetchingBART || isFetchingDRIVE || isFetchingMETRO || isFetchingBIKE || isFetchingWALK;
}

// --- [ HELPER FUNCTION] ---
// This contains the logic for the drive mode cycle,
// so we can call it from multiple places.
void executeDriveModeCycle() {
    // 1. Cycle to the next mode
    currentDriveMode = (DriveMode)((currentDriveMode + 1) % NUM_DRIVE_MODES);
    
    // 2. Get the name for the display
    String modeName = TRAVEL_MODE_DISPLAY_NAMES[currentDriveMode];
    
    // 3. Show feedback to the user
    Serial.printf("Drive mode set to: %s\n", modeName.c_str());
    
    renderDisplay("MODE SET", modeName, "#FFFFFF");
    delay(1000); // Hold feedback on screen

    // 4. Go back to clock
    isClockActive = true; 
    justExitedClock = true;
    lastActivityTime = 0;
    
    // This tells displayClock() to do a full screen clear
    lastClockRenderTime = 0; 
}


bool isTimeSynced() {
    time_t now;
    time(&now);
    return now > 946684800; 
}

bool comparePredictions(const TrainPrediction& a, const TrainPrediction& b) {
    return a.minutes < b.minutes;
}

void sortPredictions(std::vector<TrainPrediction>& predictions) {
    std::sort(predictions.begin(), predictions.end(), comparePredictions);
}

// --- Simplified helper function ---
// The JSON gives us the hexcolor, so this function's
// only job is to set the 3-letter lineInitial.
void mapBartColorToDisplay(TrainPrediction& p) {
    // p.colorHex is already set from the JSON's "hexcolor" field.
    // We just set the lineInitial based on the "color" field (p.bartColorName).

    if (p.bartColorName.equalsIgnoreCase("YELLOW")) {
        p.lineInitial = "YEL"; 
    } else if (p.bartColorName.equalsIgnoreCase("RED")) {
        p.lineInitial = "RED";
    } else if (p.bartColorName.equalsIgnoreCase("GREEN")) {
        p.lineInitial = "GRN"; 
    } else if (p.bartColorName.equalsIgnoreCase("BLUE")) {
        p.lineInitial = "BLU"; 
    } else if (p.bartColorName.equalsIgnoreCase("ORANGE")) {
        p.lineInitial = "ORG"; 
    } else {
        // Default for any other lines (Beige, etc.)
        p.lineInitial = "TRN"; 
    }
}

void getMuniLineInfo(const String& routeTag, String& lineInitial, String& colorHex) {
    lineInitial = routeTag;
    colorHex = "#FFFFFF"; 

    if (routeTag == "J") colorHex = "#800080"; 
    else if (routeTag == "K") colorHex = "#FF00FF"; 
    else if (routeTag == "L") colorHex = "#00FF00"; 
    else if (routeTag == "M") colorHex = "#1E90FF"; 
    else if (routeTag == "N") colorHex = "#FFD700"; 
}


int timeUntilMinutes(const String& isoTime) {
    if (isoTime.isEmpty()) return 99;

    struct tm tm;
    time_t now_t;
    time(&now_t); // Get current local epoch time

    // 1. Parse the UTC timestamp string from the API
    if (sscanf(isoTime.c_str(), "%d-%d-%dT%d:%d:%d", 
                &tm.tm_year, &tm.tm_mon, &tm.tm_mday, 
                &tm.tm_hour, &tm.tm_min, &tm.tm_sec) != 6) {
        return 99; 
    }

    tm.tm_year -= 1900; 
    tm.tm_mon -= 1;     
    tm.tm_isdst = 0;   // Tell the struct it's not in daylight saving
    
    // 2. Temporarily set timezone to UTC
    setenv("TZ", "UTC-0:00", 1);
    tzset();

    // 3. Call mktime(). It will treat the 'tm' struct as UTC
    //    and return the correct UTC epoch time.
    time_t target_t = mktime(&tm);

    // 4. Restore original timezone (using your global constant)
    setenv("TZ", TIMEZONE_INFO, 1);
    tzset();
    
    // 5. Calculate the difference.
    double diff_seconds = difftime(target_t, now_t);
    
    return (int)std::max(0.0, diff_seconds / 60.0);
}


// --- HTTP UTILITY FUNCTION ---

String makeHttpRequest(const String& url) {
    bool useSecure = url.startsWith("https://");
    
    std::unique_ptr<WiFiClientSecure> secureClient;
    std::unique_ptr<WiFiClient> plainClient;
    
    if (useSecure) {
        secureClient = std::make_unique<WiFiClientSecure>();
        secureClient->setInsecure();
    } else {
        plainClient = std::make_unique<WiFiClient>();
    }
    
    HTTPClient http;
    WiFiClient* clientToUse = useSecure ? (WiFiClient*)secureClient.get() : plainClient.get();

    if (!http.begin(*clientToUse, url)) {
        Serial.println("[HTTP] ERROR: HTTP/HTTPS begin failed.");
        return "";
    }
    
    http.setTimeout(15000); 
    
    int httpCode = http.GET();
    String payload = "";

    if (httpCode == HTTP_CODE_OK) {
        payload = http.getString();
    } else {
        Serial.printf("[HTTP] GET failed (%d): %s\n", httpCode, http.errorToString(httpCode).c_str());
    }
    http.end();
    return payload;
}


// *************************************************************************
// *** FORECAST TIME HELPER (2-digit) ***
// *************************************************************************
String formatForecastTime(time_t timestamp) {
    // Convert Unix time_t to a local tm struct
    struct tm * timeinfo;
    timeinfo = localtime(&timestamp);
    
    // Format the time as 2-digit 24-hour time (e.g., "09", "18")
    char timeBuffer[4];
    strftime(timeBuffer, sizeof(timeBuffer), "%H", timeinfo);
    
    return String(timeBuffer);
}

// *************************************************************************
// *** API FETCH FUNCTIONS ***
// *************************************************************************
// --- Switched to snprintf for URL building ---
void fetchMuniPredictionsForStop(const char* stop_id, std::vector<TrainPrediction>& predictions) {
    predictions.clear();
    Serial.printf("\n--- [MUNI] Fetching data for stop %s ---\n", stop_id);

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;

    char muniUrl[256];
    snprintf(muniUrl, sizeof(muniUrl),
             "https://api.511.org/transit/StopMonitoring?api_key=%s&agency=%s&stopcode=%s&MaximumStopVisits=10&format=json",
             SF511_API_KEY, MUNI_OPERATOR_ID, stop_id);

    const char * headerKeys[] = {"Content-Encoding"};
    http.collectHeaders(headerKeys, 1);

    if (!http.begin(client, muniUrl)) {
        Serial.println("[MUNI] HTTPS begin failed.");
        return;
    }

    http.setTimeout(10000);
    http.addHeader("Accept-Encoding", "gzip");

    int httpCode = http.GET();
    if (httpCode != HTTP_CODE_OK) {
        Serial.printf("[MUNI] GET failed (%d): %s\n", httpCode, http.errorToString(httpCode).c_str());
        http.end();
        return;
    }

    // 1. Allocate a SINGLE raw buffer on the heap (No String objects!)
    std::unique_ptr<char[]> jsonBuffer(new char[MAX_DECOMPRESSED_SIZE + 1]);
    if (!jsonBuffer) {
        Serial.println("[MUNI] Memory allocation failed.");
        http.end();
        return;
    }

    // 2. Read Data (Handle GZIP or Plain Text into the SAME buffer)
    int dataLength = 0;
    String encoding = http.header("Content-Encoding");

    if (encoding == "gzip") {
        int compressedSize = http.getSize();
        if (compressedSize > 0) {
            // Read compressed data into a temp buffer
            std::unique_ptr<uint8_t[]> gzipBuffer(new uint8_t[compressedSize + 1]);
            if (gzipBuffer) {
                int bytesRead = http.getStream().readBytes((char*)gzipBuffer.get(), compressedSize);
                // Decompress directly into our main jsonBuffer
                int result = zt_gunzip(gzipBuffer.get(), bytesRead, (uint8_t*)jsonBuffer.get());
                if (result == ZT_SUCCESS) {
                    // Manual safety null-terminate, just in case
                    jsonBuffer.get()[MAX_DECOMPRESSED_SIZE] = '\0'; 
                    dataLength = strlen(jsonBuffer.get()); 
                } else {
                    Serial.println("[MUNI] Decompression failed.");
                    http.end(); return;
                }
            }
        }
    } else {
        // Plain Text: Read directly from stream into buffer. No .getString()!
        WiFiClient* stream = http.getStreamPtr();
        // Read up to buffer size, leave room for null terminator
        dataLength = stream->readBytes(jsonBuffer.get(), MAX_DECOMPRESSED_SIZE);
        jsonBuffer.get()[dataLength] = '\0'; // Null terminate manually
    }
    http.end();

    if (dataLength == 0) {
        Serial.println("[MUNI] Empty payload.");
        return;
    }

    // 3. Robust "Pointer Math" XML Cleanup (Zero memory copying)
    char* jsonStart = strstr(jsonBuffer.get(), "{\"ServiceDelivery\"");
    if (jsonStart == nullptr) {
        // Fallback: If strict match fails, just find the first curly brace
        jsonStart = strchr(jsonBuffer.get(), '{');
    }
    
    // If we found a start, verify we have an end
    if (jsonStart != nullptr) {
        char* jsonEnd = strrchr(jsonStart, '}');
        if (jsonEnd != nullptr) {
            *(jsonEnd + 1) = '\0'; // Chop off the XML garbage by moving the null terminator
        } else {
            jsonStart = jsonBuffer.get(); // Fallback to raw buffer
        }
    } else {
         jsonStart = jsonBuffer.get(); // Fallback to raw buffer
    }

    // 4. Parse JSON (Using the raw pointer, not a String)
    // We use Filter to save even more memory since we only need specific fields
    DynamicJsonDocument* doc = new DynamicJsonDocument(MAX_DECOMPRESSED_SIZE);
    
    // We parse 'jsonStart' which points to the clean part of our buffer
    DeserializationError error = deserializeJson(*doc, jsonStart);

    if (error) {
        Serial.printf("[MUNI] JSON failed: %s\n", error.c_str());
        delete doc; 
        return;
    }

    // 5. Safe Traversal
    JsonVariant serviceDelivery = (*doc)["ServiceDelivery"];
    if (serviceDelivery.isNull()) { delete doc; return; }

    JsonVariant monitoringDelivery = serviceDelivery["StopMonitoringDelivery"];
    if (monitoringDelivery.isNull()) { delete doc; return; }

    JsonVariant deliveryVariant; // Use Variant to handle both Object and Array
    
    // Handle the "Sometimes it's an Array, sometimes an Object" weirdness
    if (monitoringDelivery.is<JsonArray>()) {
        JsonArray arr = monitoringDelivery.as<JsonArray>();
        if (arr.size() > 0) deliveryVariant = arr[0];
    } else {
        deliveryVariant = monitoringDelivery;
    }

    if (deliveryVariant.isNull()) { delete doc; return; }

    // Check for the "No Service" condition (Empty Array)
    JsonArray departures = deliveryVariant["MonitoredStopVisit"].as<JsonArray>();
    if (departures.isNull() || departures.size() == 0) {
        Serial.println("[MUNI] No active trains (Empty List).");
        // This is NOT an error, it just means no trains. Return safely.
        delete doc; 
        return;
    }

    // 6. Extraction Loop
    for (JsonObject departure : departures) {
         // Add safety checks for every hop
         if (!departure.containsKey("MonitoredVehicleJourney")) continue;
         JsonObject journey = departure["MonitoredVehicleJourney"];
         
         if (!journey.containsKey("MonitoredCall")) continue;
         JsonObject call = journey["MonitoredCall"];

         String routeID = journey["LineRef"].as<String>();
         String destination = journey["DestinationName"].as<String>();
         String expectedTimeStr = call["ExpectedArrivalTime"].as<String>();
         
         if (routeID.isEmpty() || expectedTimeStr.isEmpty()) continue;
         
         int minutes = timeUntilMinutes(expectedTimeStr);
         
         // Destination Overrides
         if (strcmp(stop_id, "15726") == 0) {
             destination = "Embarcad";
         } else if (strcmp(stop_id, "16998") == 0) {
             if (routeID == "K") destination = "Balboa P";
             else if (routeID == "L") destination = "SF Zoo";
             else if (routeID == "M") destination = "San Jose";
         }

         if (minutes >= 0) {
             String lineInitial, colorHex;
             getMuniLineInfo(routeID, lineInitial, colorHex);
             TrainPrediction p = {minutes, destination, lineInitial, colorHex, ""}; 
             predictions.push_back(p);
         }
    }
    
    delete doc;
}

// --- JSON-parsing version ---
void fetchBARTData() {
    Serial.println("\n--- [BART] Fetching ALL data for 16TH (JSON) ---");
    bartNorthPredictions.clear();
    bartSouthPredictions.clear();
    bool success = false;

    // --- Build the JSON API URL ---
    // We use snprintf for memory safety, and add "&json=y"
    char bartUrl[200];
    snprintf(bartUrl, sizeof(bartUrl), 
             "https://api.bart.gov/api/etd.aspx?cmd=etd&orig=%s&key=%s&json=y",
             BART_ORIGIN, BART_API_KEY);

    String payload = makeHttpRequest(bartUrl);

    if (payload.length() < 100) {
        Serial.println("[BART] ERROR: No or short response received.");
        return;
    }

    // --- Deserialize the JSON payload ---
    DynamicJsonDocument doc(MAX_DECOMPRESSED_SIZE); 
    DeserializationError error = deserializeJson(doc, payload);

    if (error) {
        Serial.printf("[BART] JSON parsing failed: %s\n", error.c_str());
        return;
    }

    // --- Traverse the JSON structure ---
    // root -> station (array) -> etd (array) -> estimate (array)
    JsonArray stationArray = doc["root"]["station"];
    if (stationArray.isNull()) {
        Serial.println("[BART] ERROR: JSON structure invalid, 'station' array not found.");
        return;
    }

    // Loop through stations
    for (JsonObject station : stationArray) {
        JsonArray etdArray = station["etd"];
        if (etdArray.isNull()) continue; // Go to next station if no 'etd'

        // Loop through all destinations for this station
        for (JsonObject etd : etdArray) {
            
            // 1. Get the original destination name from the API
            String destination = etd["destination"].as<String>();
            if (destination.isEmpty()) continue;

            // --- Apply your destination override logic ---
            if (destination.startsWith("Pittsburg")) {
                destination = "Pittsbur";
            } else if (destination.startsWith("Antioch")) {
                destination = "Antioch";
            } else if (destination.startsWith("Dublin")) {
                destination = "Dublin";
            } else if (destination.startsWith("Berryessa")) {
                destination = "Berryess";
            } else if (destination.startsWith("Richmond")) {
                destination = "Richmond";
            } else if (destination.startsWith("SF Airport")) {
                destination = "SF Arprt";
            } else if (destination.startsWith("SFO")) {
                destination = "SF Arprt";
            } else if (destination.startsWith("Daly City")) {
                destination = "Daly Cty";
            } else if (destination.startsWith("Millbrae")) {
                destination = "Millbrae";
            }
            // --- End of override logic ---

            // Loop through the estimates (trains) for this destination
            JsonArray estimateArray = etd["estimate"];
            if (estimateArray.isNull()) continue;

            for (JsonObject estimate : estimateArray) {
                String minutesStr = estimate["minutes"].as<String>();
                String direction = estimate["direction"].as<String>();
                String rawColor = estimate["color"].as<String>();
                String hexColor = estimate["hexcolor"].as<String>();

                if (minutesStr.isEmpty() || direction.isEmpty()) {
                    continue; // Skip invalid estimate
                }

                success = true; // We found at least one valid train
                int minutes = (minutesStr == "Leaving") ? 0 : minutesStr.toInt();

                // 2. Create the prediction object
                TrainPrediction p;
                p.minutes = minutes;
                p.destination = destination;    // Already abbreviated (e.g. "Daly Cty")

                // --- FIX FOR "WHITE" (Unscheduled/Ghost) TRAINS ---
                if (rawColor == "WHITE") {
                    
                    // RED LINE (Richmond <-> Millbrae)
                    if (destination == "Richmond" || destination == "Millbrae") {
                        rawColor = "RED";
                        hexColor = "#ff0000";
                    }
                    
                    // YELLOW LINE (Antioch <-> SFO)
                    else if (destination == "Antioch" || destination == "SF Arprt") {
                        rawColor = "YELLOW";
                        hexColor = "#ffff33";
                    }
                    
                    // BLUE LINE (Dublin <-> Daly City)
                    // We default Daly City to Blue because it runs later/more often
                    else if (destination == "Dublin" || destination == "Daly Cty") {
                        rawColor = "BLUE";
                        hexColor = "#0099cc";
                    }
                    
                    // GREEN LINE (Berryessa)
                    else if (destination == "Berryess") {
                        rawColor = "GREEN";
                        hexColor = "#339933";
                    }
                    
                    // NO 'ELSE' BLOCK:
                    // If the train is going somewhere weird (e.g. "24th St"), 
                    // it stays WHITE ("TRN"), which correctly alerts you that it's unusual.
                }
                // ---------------------------------------------

                p.bartColorName = rawColor; 
                p.colorHex = hexColor;

                // 3. Call helper to set the 3-letter initial
                mapBartColorToDisplay(p); 

                // 4. Add to the correct vector
                if (direction == "North") {
                    bartNorthPredictions.push_back(p);
                } else if (direction == "South") {
                    bartSouthPredictions.push_back(p);
                }
            }
        }
    }

    // --- Sort and update cache timer ---
    sortPredictions(bartNorthPredictions);
    sortPredictions(bartSouthPredictions); 

    Serial.printf("[BART] Live Data Refreshed: Northbound: %d, Southbound: %d.\n", 
                    bartNorthPredictions.size(), bartSouthPredictions.size());
    
    if (success) {
        lastBartFetchTime = millis() / 1000;
    }
}

void fetchMuniNorth() {
    Serial.println("\n--- [MUNI] Fetching Northbound data ---");
    fetchMuniPredictionsForStop(MUNI_STOP_ID_N, muniNorthPredictions);
    sortPredictions(muniNorthPredictions);
    lastMuniNorthFetchTime = millis() / 1000;
    Serial.printf("[MUNI North] Data Refreshed: %d predictions.\n", muniNorthPredictions.size());
}

void fetchMuniSouth() {
    Serial.println("\n--- [MUNI] Fetching Southbound data ---");
    fetchMuniPredictionsForStop(MUNI_STOP_ID_S, muniSouthPredictions);
    sortPredictions(muniSouthPredictions);
    lastMuniSouthFetchTime = millis() / 1000;
    Serial.printf("[MUNI South] Data Refreshed: %d predictions.\n", muniSouthPredictions.size());
}

// --- With snprintf and String::reserve() ---
void fetchDriveData(const DriveDestination* destinations, int destCount, 
                    std::vector<TrainPrediction>& predictions, unsigned long& lastFetchTime, bool isDriveTo) {
    
    Serial.println("[GMAPS] Fetching BATCHED Drive/Transit data...");
    predictions.clear();
    bool success = false;
    
    if (destCount == 0) return; // Nothing to fetch

    String apiModeString = TRAVEL_MODE_API_NAMES[currentDriveMode];
    Serial.printf("[GMAPS] Using travel mode: %s\n", apiModeString.c_str());

    // --- (Part 1 - URL String Building) ---
    String originsStr;
    String destinationsStr;
    originsStr.reserve(350);
    destinationsStr.reserve(350);
    
    if (isDriveTo) {
        originsStr = destinations[0].originLat + "," + destinations[0].originLon;
        for (int i = 0; i < destCount; i++) {
            destinationsStr += destinations[i].destLat + "," + destinations[i].destLon;
            if (i < destCount - 1) destinationsStr += "|";
        }
        Serial.println("[GMAPS] Mode: One-to-Many (Drive A)");
    } else {
        destinationsStr = destinations[0].destLat + "," + destinations[0].destLon;
        for (int i = 0; i < destCount; i++) {
            originsStr += destinations[i].originLat + "," + destinations[i].originLon;
            if (i < destCount - 1) originsStr += "|";
        }
        Serial.println("[GMAPS] Mode: Many-to-One (Drive B)");
    }

    char gmapsUrl[600];
    snprintf(gmapsUrl, sizeof(gmapsUrl),
             "https://maps.googleapis.com/maps/api/distancematrix/json?units=imperial&origins=%s&destinations=%s&mode=%s&departure_time=now&key=%s",
             originsStr.c_str(),
             destinationsStr.c_str(),
             apiModeString.c_str(),
             GMAPS_API_KEY);
    
    // --- (Part 2 - HTTP Request & JSON Parsing) ---
    String payload = makeHttpRequest(gmapsUrl);

    if (payload.length() > 0) {
        DynamicJsonDocument doc(MAX_DECOMPRESSED_SIZE);
        if (deserializeJson(doc, payload)) {
             Serial.println("[GMAPS] JSON parse failed.");
             return;
        }

        if (doc["status"].as<String>() == "OK") {
            
            if (isDriveTo) {
                // One Origin, Many Destinations
                JsonArray elements = doc["rows"][0]["elements"].as<JsonArray>();
                int elementIndex = 0;
                for (JsonObject element : elements) {
                    if (element["status"].as<String>() == "OK") {
                        long duration_normal = element["duration"]["value"].as<long>();
                        long duration_traffic = element["duration_in_traffic"].isNull() ? duration_normal : element["duration_in_traffic"]["value"].as<long>();
                        
                        String trafficColorHex = ""; 
                        if (currentDriveMode == MODE_DRIVING && duration_normal > 0) {
                            double ratio = (double)duration_traffic / (double)duration_normal;
                            if (ratio > 1.6) trafficColorHex = "#FF0000";      // Red
                            else if (ratio > 1.2) trafficColorHex = "#FFFF00"; // Yellow
                            else trafficColorHex = "#00FF00";                 // Green
                        }
                        
                        const DriveDestination& dest = destinations[elementIndex]; 
                        String displayInitial = TRAVEL_MODE_DISPLAY_NAMES[currentDriveMode];
                        predictions.push_back({(int)(duration_traffic / 60), dest.name, displayInitial, dest.colorHex, trafficColorHex});
                        success = true;
                    }
                    elementIndex++;
                }
            } else {
                // Many Origins, One Destination (DRIVE_B)
                JsonArray rows = doc["rows"].as<JsonArray>();
                int rowIndex = 0;
                for (JsonObject row : rows) {
                    JsonObject element = row["elements"][0]; 
                    if (element["status"].as<String>() == "OK") {
                        long duration_normal = element["duration"]["value"].as<long>();
                       long duration_traffic = element["duration_in_traffic"].isNull() ? duration_normal : element["duration_in_traffic"]["value"].as<long>();
                        
                        // Get the destination info first so we can check the name
                        const DriveDestination& dest = destinations[rowIndex];
                        
                        // --- [JORDON'S MOTORCYCLE RULE (AGGRESSIVE)] ---
                        long duration_to_use = duration_traffic; // Default to car time
                        
                        // Check if mode is DRIVING and the destination name is "JORDON"
                         if (currentDriveMode == MODE_DRIVING && dest.name == "JORDON") {
                            
                            // 1. Calculate the pure traffic delay (Total - Normal)
                            long traffic_delay = duration_traffic - duration_normal;
                            
                            // Safety: ensure delay isn't negative (glitch protection)
                            if (traffic_delay < 0) traffic_delay = 0;

                            // 2. Logic: Speed on base drive + Filter through delay
                            //    (double) cast ensures the 1.3 division happens correctly before turning back to long
                            long base_moto_time = (long)((double)duration_normal / 1.2);
                            long filtered_delay = traffic_delay / 2.5;
                            
                            duration_to_use = base_moto_time + filtered_delay;
                            
                            Serial.printf("[GMAPS] Moto Rule: Car %ldm -> Moto %ldm\n", (duration_traffic/60), (duration_to_use/60));
                        }
                        // --- [END OF LOGIC] ---

                        String trafficColorHex = ""; // Default: empty
                       if (currentDriveMode == MODE_DRIVING && duration_normal > 0) {
                            // Use the *original* duration_traffic for the color ratio
                            double ratio = (double)duration_traffic / (double)duration_normal;
                             if (ratio > 1.6) trafficColorHex = "#FF0000";      // Red
                            else if (ratio > 1.2) trafficColorHex = "#FFFF00"; // Yellow
                             else trafficColorHex = "#00FF00";                 // Green
                        }

                        String displayInitial = TRAVEL_MODE_DISPLAY_NAMES[currentDriveMode];
                        // Use the (possibly modified) duration_to_use for the prediction
                       predictions.push_back({(int)(duration_to_use / 60), dest.name, displayInitial, dest.colorHex, trafficColorHex});
                           success = true;
                   }
                    rowIndex++;
                }
            }
        } else {
           String errorMsg = doc["error_message"].as<String>();
            if (errorMsg.length() > 0) Serial.println("[GMAPS] API Error: " + errorMsg);
        }
    } 

    if (success) {
        lastFetchTime = millis() / 1000;
    }
    Serial.printf("[GMAPS] Data Refreshed: %d predictions.\n", predictions.size());
}

// --- fetchDriveA calls the base function ---
void fetchDriveA() {
    // --- Pass the specific vector and timer for the current mode ---
    // Pass 'true' because this is "Drive To" (one origin, many destinations)
    fetchDriveData(DRIVE_DATA_A, DRIVE_DATA_A_COUNT, 
                   driveAPredictions[currentDriveMode], 
                   lastDriveAFetchTime[currentDriveMode], true);
}
// --- fetchDriveB calls the base function ---
void fetchDriveB() {
    // ---  Pass the specific vector and timer for the current mode ---
    // Pass 'false' because this is "Drive From" (many origins, one destination)
    fetchDriveData(DRIVE_DATA_B, DRIVE_DATA_B_COUNT, 
                   driveBPredictions[currentDriveMode], 
                   lastDriveBFetchTime[currentDriveMode], false);
}


void executeFetchAndDisplay(ActiveMode requestedMode) {
    if (requestedMode == MUNI_N) { 
        processMode(MUNI_N, muniNorthIndex, muniNorthPredictions, "MUNI North"); 
    } else if (requestedMode == MUNI_S) { 
        processMode(MUNI_S, muniSouthIndex, muniSouthPredictions, "MUNI South"); 
    } else if (requestedMode == BART_N) {
        processMode(BART_N, bartNorthIndex, bartNorthPredictions, "BART North"); 
    } else if (requestedMode == BART_S) { 
        processMode(BART_S, bartSouthIndex, bartSouthPredictions, "BART South"); 
    } else if (requestedMode == DRIVE_A) { 
        // --- Pass the predictions for the current drive mode ---
        processDriveMode(DRIVE_A, driveAIndex, driveAPredictions[currentDriveMode]); 
    } else if (requestedMode == DRIVE_B) { 
        // --- Pass the predictions for the current drive mode ---
        processDriveMode(DRIVE_B, driveBIndex, driveBPredictions[currentDriveMode]); 
    }
}


// *****************************************************************
// *** BACKGROUND API TASK: Runs only when signaled and checks flags ***
// *****************************************************************
// --- Replaced with Fetch ID and Mutex lock to fix race condition ---
void api_fetch_task(void *pvParameters) {
    Serial.println("[API Task] Ready and waiting for button signal.");
    uint8_t flagsToProcess = FLAG_NONE; // Local copy

    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        // --- [RACE CONDITION FIX] ---
        // 1. Snapshot the *current* active fetch ID. This is the tag for this task.
        uint32_t myFetchID = activeFetchID;
        
        // 2. Lock the mutex just long enough to read and clear the flags
        taskENTER_CRITICAL(&fetchFlagsMux);
        flagsToProcess = fetchFlags;    // Copy the flags
        fetchFlags = FLAG_NONE;       // Clear the global flags
        taskEXIT_CRITICAL(&fetchFlagsMux);  // Release the lock
        // --- [END OF FIX] ---

        if (flagsToProcess != FLAG_NONE && !isFetchingMUNI && !isFetchingBART && !isFetchingDRIVE) {
             renderDisplay("FETCHING", "DATA...", "#FFFFFF"); 
        }

        // --- Execute Fetches based on the LOCAL flags ---
        if (flagsToProcess & FLAG_BART) {
            fetchBARTData();
        }
        if (flagsToProcess & FLAG_MUNI_N) {
            fetchMuniNorth();
        }
        if (flagsToProcess & FLAG_MUNI_S) {
            fetchMuniSouth();
        }
        if (flagsToProcess & FLAG_DRIVE_A) {
            fetchDriveA();
        }
        if (flagsToProcess & FLAG_DRIVE_B) {
            fetchDriveB();
        }
        
        // --- [RACE CONDITION FIX (part 2)] ---
        // 3. Tag the data with the ID from when this task started.
        dataFetchID = myFetchID;
        dataIsReadyToDisplay = true; 
    }
}
// *****************************************************************


// --- [64x64] 2. UPDATED DISPLAY & MODE LOGIC ---
void renderDisplay(String line1, String line2, String colorHex, String timeColorHex) {
    if (MOCK_DISPLAY) {
        Serial.printf("MOCK DISPLAY: %s | %s \n", line1.c_str(), line2.c_str());
        return;
    }
    if (dma_display == nullptr) return;

    // 1. Extract R, G, B values for the Line/Status Color
    int r = strtol(colorHex.substring(1, 3).c_str(), NULL, 16);
    int g = strtol(colorHex.substring(3, 5).c_str(), NULL, 16);
    int b = strtol(colorHex.substring(5, 7).c_str(), NULL, 16);
    uint16_t color = dma_display->color565(r, g, b);

    // 2. Handle Night Mode Dimming (Feature #3)
    bool isNight = isMoonTime();
    // If night, use Dark Grey (110,110,110), else White
    uint16_t mainTextColor = isNight ? dma_display->color565(150, 150, 150) : dma_display->color565(255, 255, 255);
    
    // Keep "Time Color" override logic, but default to mainTextColor if empty
    uint16_t time_color_16bit;
    if (timeColorHex.isEmpty()) {
        time_color_16bit = mainTextColor; 
    } else {
        int r_t = strtol(timeColorHex.substring(1, 3).c_str(), NULL, 16);
        int g_t = strtol(timeColorHex.substring(3, 5).c_str(), NULL, 16);
        int b_t = strtol(timeColorHex.substring(5, 7).c_str(), NULL, 16);
        time_color_16bit = dma_display->color565(r_t, g_t, b_t);
    }

    dma_display->fillScreen(dma_display->color565(0, 0, 0)); // Clear screen

    // --- CHECK SYSTEM MESSAGES ---
    bool isSystemMessage = (line1 == "BOOTING" || line1 == "READY" || line1 == "FETCHING" || \
                            line1 == "WAIT" || line1 == "ERROR" || line1 == "NO SERVICE" || line1 == "DRIVE TIME" || \
                            line1 == "TIME" || line1 == "BRIGHT" || line1 == "MEDIUM" || line1 == "DIM" || \
                            line1 == "DRIVE" || line1 == "METRO" || line1 == "BIKE" || line1 == "WALK" || \
                            line1 == "MODE SET" || line1 == "LEVEL" || line1 == "IDLE MODE");
    
    bool isClockDisplay = false;
    bool isDriveModeDisplay = (currentMode == DRIVE_A || currentMode == DRIVE_B) && !isSystemMessage && !isClockDisplay;
    
    // --- ARRIVAL TIME CALCULATION ---
    int minutesToAdd = -1; // -1 indicates no prediction data found
    if (!isSystemMessage && !isClockDisplay) {
        // Parse "MIN" from line1 (works for "N5MIN" or "15MIN")
        int minIndex = line1.indexOf("MIN");
        if (minIndex > 0) {
            // Find where the number starts (walk backwards from 'M')
            int startParam = minIndex - 1;
            while (startParam >= 0 && isDigit(line1.charAt(startParam))) {
                startParam--;
            }
            // Extract the number substring
            String minVal = line1.substring(startParam + 1, minIndex);
            minutesToAdd = minVal.toInt();
        }
    }

    if (isSystemMessage) {
        dma_display->setTextSize(1);
        int16_t x1, y1; uint16_t w, h;
        dma_display->getTextBounds(line1, 0, 0, &x1, &y1, &w, &h);
        dma_display->setCursor(std::max(static_cast<int16_t>(2), (int16_t)((PANEL_RES_X - w) / 2)), 5);
        dma_display->setTextColor(color);
        dma_display->print(line1);

        dma_display->setTextColor(mainTextColor); // Use Dimmed/White
        String line2_trun = line2;
        int yPos2 = (line1 == "MODE SET" || line1 == "LEVEL") ? 20 : 25;
        if (line1 == "MODE SET" || line1 == "LEVEL") dma_display->setTextSize(2);
        else {
            dma_display->setTextSize(1);
            if (line2_trun.length() > 10) line2_trun = line2_trun.substring(0, 10);
        }
        
        dma_display->getTextBounds(line2_trun, 0, 0, &x1, &y1, &w, &h);
        dma_display->setCursor(std::max(static_cast<int16_t>(2), (int16_t)((PANEL_RES_X - w) / 2)), yPos2);
        dma_display->print(line2_trun); 
    } 
    
    else if (isDriveModeDisplay) {
        // --- 4-Line Drive Layout ---
        
        // Line 1: Destination (was line2)
        dma_display->setTextSize(1);
        dma_display->setTextColor(color);
        dma_display->setTextColor(color); 

        String destStr = line2;
        if (destStr.length() > 10) destStr = destStr.substring(0, 9) + "."; 
        
        int16_t x1, y1; uint16_t w, h;
        dma_display->getTextBounds(destStr, 0, 0, &x1, &y1, &w, &h);
        dma_display->setCursor(std::max(static_cast<int16_t>(2), (int16_t)((PANEL_RES_X - w) / 2)), 5);
        dma_display->print(destStr);

        // Line 2: Duration (was line1)
        dma_display->setTextColor(time_color_16bit); // Traffic color
        String timeStr = line1;
        int yPos_time = 20;
        if (minutesToAdd > 99) {
            dma_display->setTextSize(1); yPos_time = 24; timeStr.replace("MIN", " MIN");
        } else {
            dma_display->setTextSize(2);
            if (timeStr.length() == 4 && isDigit(timeStr.charAt(0)) && timeStr.endsWith("MIN")) timeStr.replace("MIN", " MIN");
        }
        dma_display->getTextBounds(timeStr, 0, 0, &x1, &y1, &w, &h);
        dma_display->setCursor(std::max(static_cast<int16_t>(2), (int16_t)((PANEL_RES_X - w) / 2)), yPos_time);
        dma_display->print(timeStr);

        // Line 3: "Drive To" Text
        dma_display->setTextSize(1);
        dma_display->setTextColor(mainTextColor); // Dim this
        String modeStr = TRAVEL_MODE_TITLE_NAMES[currentDriveMode];
        String driveTypeStr = (currentMode == DRIVE_A) ? modeStr + " To" : modeStr + " From";
        
        dma_display->getTextBounds(driveTypeStr, 0, 0, &x1, &y1, &w, &h);
        dma_display->setCursor(std::max(static_cast<int16_t>(2), (int16_t)((PANEL_RES_X - w) / 2)), 40);
        dma_display->print(driveTypeStr);
        
    } else {
        // --- Transit Layout ---
        
        // Split "N5MIN"
        String data1 = line1;
        String data2 = "";
        int splitPos = -1;
        for (int i = 0; i < line1.length(); ++i) {
            if (isDigit(line1.charAt(i))) { splitPos = i; break; }
        }
        if (splitPos != -1) {
            data1 = line1.substring(0, splitPos);
            data2 = line1.substring(splitPos);
        }
        
        // Line 1: Route (e.g., "N", "RED") - Keep this bright/colored
        dma_display->setTextSize(2);
        dma_display->setTextColor(color); 
        int16_t x1, y1; uint16_t w, h;
        dma_display->getTextBounds(data1, 0, 0, &x1, &y1, &w, &h);
        dma_display->setCursor(std::max(static_cast<int16_t>(2), (int16_t)((PANEL_RES_X - w) / 2)), 2);
        dma_display->print(data1);

        // Line 2: Time Remaining ("5MIN") - Dim this at night
        dma_display->setTextColor(mainTextColor); 
        int yPos_trn = 20;
        if (minutesToAdd > 99) {
            dma_display->setTextSize(1); yPos_trn = 24; data2.replace("MIN", " MIN");
        } else {
            dma_display->setTextSize(2);
            if (data2.length() == 4 && isDigit(data2.charAt(0)) && data2.endsWith("MIN")) data2.replace("MIN", " MIN");
        }
        dma_display->getTextBounds(data2, 0, 0, &x1, &y1, &w, &h);
        dma_display->setCursor(std::max(static_cast<int16_t>(2), (int16_t)((PANEL_RES_X - w) / 2)), yPos_trn);
        dma_display->print(data2);

        // Line 3: Destination - Dim this at night
        dma_display->setTextSize(1);
        dma_display->setTextColor(mainTextColor);
        String destStr = "->" + line2;
        if (destStr.length() > 10) destStr = destStr.substring(0, 8) + "..";
        dma_display->getTextBounds(destStr, 0, 0, &x1, &y1, &w, &h);
        dma_display->setCursor(std::max(static_cast<int16_t>(2), (int16_t)((PANEL_RES_X - w) / 2)), 40);
        dma_display->print(destStr);
    }

    // --- BOTTOM CLOCK ---
    if (isTimeSynced() && !isClockDisplay && !isClockActive && !isBooting) {
        char timeBuffer[10];
        time_t now;
        time(&now);
        
        // Calculate Arrival Time if we have prediction data
        if (minutesToAdd >= 0) {
            time_t arrivalTime = now + (minutesToAdd * 60);
            struct tm * arrivalInfo = localtime(&arrivalTime);
            strftime(timeBuffer, sizeof(timeBuffer), "%H:%M", arrivalInfo);
            
            // Feature 2: Set color to match the line color
            dma_display->setTextColor(color); 
        } 
        else {
            // Fallback to current time if no prediction data (e.g., system message)
            struct tm * timeinfo = localtime(&now);
            strftime(timeBuffer, sizeof(timeBuffer), "%H:%M", timeinfo);
            dma_display->setTextColor(mainTextColor);
        }
        
        dma_display->setTextSize(1);
        int16_t x1, y1; uint16_t w, h;
        dma_display->getTextBounds(timeBuffer, 0, 0, &x1, &y1, &w, &h);
        dma_display->setCursor(std::max(static_cast<int16_t>(2), (int16_t)((PANEL_RES_X - w) / 2)), PANEL_RES_Y - 10);
        dma_display->print(timeBuffer);
    }
    
    dma_display->flipDMABuffer(); 
}

void resetOtherCounters(ActiveMode activeMode) {
    if (activeMode != MUNI_N) muniNorthIndex = 0;
    if (activeMode != MUNI_S) muniSouthIndex = 0;
    if (activeMode != BART_N) bartNorthIndex = 0;
    if (activeMode != BART_S) bartSouthIndex = 0;
    if (activeMode != DRIVE_A) driveAIndex = 0;
    if (activeMode != DRIVE_B) driveBIndex = 0;
}

// ---  processMode  ---
void processMode(ActiveMode newMode, int& currentIndex, const std::vector<TrainPrediction>& predictions, const String& modeName) {
    resetOtherCounters(newMode);
    currentMode = newMode;

    if (predictions.empty()) {
        renderDisplay("NO SERVICE", modeName + " OFFLINE", "#FF0000"); // Red for error
        lastActivityTime = millis() - (INACTIVITY_TIMEOUT - 5000);
        return;
    }

    if (currentIndex >= predictions.size()) { currentIndex = 0; }

    const TrainPrediction& p = predictions[currentIndex];
    // Time format remains compressed here (e.g., "N5MIN") for easy parsing in renderDisplay
    String line1 = p.lineInitial + String(p.minutes) + "MIN";
    
    // Let renderDisplay handle truncation
    String line2 = p.destination;
    
    renderDisplay(line1, line2, p.colorHex, p.bartColorName);
    currentIndex++;
}

// --- processDriveMode ---
void processDriveMode(ActiveMode newMode, int& currentIndex, const std::vector<TrainPrediction>& predictions) {
    resetOtherCounters(newMode);
    currentMode = newMode;
    
    if (predictions.empty()) { 
        // --- Show the current mode in the error message
        String modeName = TRAVEL_MODE_DISPLAY_NAMES[currentDriveMode];
        renderDisplay(modeName, "Error calculating...", "#FF0000"); // Red for error
        lastActivityTime = millis() - (INACTIVITY_TIMEOUT - 5000);
        return;
    }
    
    if (currentIndex >= predictions.size()) { currentIndex = 0; }

    const TrainPrediction& p = predictions[currentIndex];
    
    // line1 is the time: e.g., "15MIN"
    // No spaces are added here, renderDisplay handles the single-digit space if required.
    String line1 = String(p.minutes) + "MIN";
    // line2 is the destination, which renderDisplay will promote to line 1
    String line2 = p.destination;

    renderDisplay(line1, line2, p.colorHex, p.bartColorName);
    currentIndex++;
}


// Okay now some Chao stuff
// Helper to draw a 1-color bitmap
void drawIconBitmap(int x, int y, int iconIndex, uint16_t color) {
    for (int row = 0; row < 8; row++) {
        for (int col = 0; col < 8; col++) {
            if (icons_8x8[iconIndex][row] & (1 << (7 - col))) {
                dma_display->drawPixel(x + col, y + row, color);
            }
        }
    }
}

// Calculates total seconds lights were off within the sleep window ending at wakeTime.
// Sleep window = 10:30 PM (wakeTime - 30600s) to wakeTime (7:00 AM).
// Sums all accumulated dark periods plus any ongoing darkness period.
long calculateDarkSleepSeconds(time_t wakeTime) {
    long total = myPet.accumulatedDarkSeconds;
    // Add the current ongoing darkness period if lights are still off
    if (myPet.lightsOffTime != 0) {
        time_t sleepWindowStart = wakeTime - (8 * 3600 + 30 * 60); // 10:30 PM
        time_t effectiveStart = (myPet.lightsOffTime > sleepWindowStart) ? myPet.lightsOffTime : sleepWindowStart;
        if (wakeTime > effectiveStart) {
            total += (long)(wakeTime - effectiveStart);
        }
    }
    return total;
}

// Scores the night's sleep and applies rewards/penalties.
// Call this exactly once per wake-up event, passing the 7 AM boundary time.
void evaluateSleepQuality(time_t wakeTime) {
    long darkSeconds = calculateDarkSleepSeconds(wakeTime);
    // Good sleep threshold: 5 hours of lights-off within the sleep window
    if (darkSeconds >= 18000) {
        myPet.sleepScore = min(6, myPet.sleepScore + 2);
        Serial.printf("[SLEEP] Good sleep: %ld s dark. +2 sleepScore.\n", darkSeconds);
    } else {
        myPet.sleepScore = max(0, myPet.sleepScore - 2);
        applyDamage(4.0);
        Serial.printf("[SLEEP] Poor sleep: %ld s dark. -2 sleepScore, +4 damage.\n", darkSeconds);
    }
    myPet.lightsOffTime = 0;        // Reset for the new day
    myPet.accumulatedDarkSeconds = 0; // Reset accumulator for the new day
}

// Calculates total seconds lights were off within the awake window ending at bedtime.
// Awake window = 7:00 AM (bedtime - 56400s) to 10:30 PM (bedtime).
long calculateDayDarkSeconds(time_t bedtime) {
    long total = myPet.accumulatedDayDarkSeconds;
    // Add the current ongoing darkness period if lights are still off
    if (myPet.lightsOffDayTime != 0) {
        time_t dayWindowStart = bedtime - (15 * 3600 + 30 * 60); // 7:00 AM
        time_t effectiveStart = (myPet.lightsOffDayTime > dayWindowStart) ? myPet.lightsOffDayTime : dayWindowStart;
        if (bedtime > effectiveStart) {
            total += (long)(bedtime - effectiveStart);
        }
    }
    return total;
}

// Scores the day's light exposure and applies damage if lights were off too long.
// Call this exactly once per sleep event, passing the 10:30 PM boundary time.
void evaluateDayLightQuality(time_t bedtime) {
    long darkSeconds = calculateDayDarkSeconds(bedtime);
    // Bad day threshold: 5 hours of lights-off within the awake window
    if (darkSeconds >= 18000) {
        applyDamage(3.0);
        Serial.printf("[DAY] Lights off too long: %ld s dark. +3 damage.\n", darkSeconds);
    } else {
        Serial.printf("[DAY] Lights on enough: %ld s dark. No penalty.\n", darkSeconds);
    }
    myPet.lightsOffDayTime = 0;         // Reset for the new day
    myPet.accumulatedDayDarkSeconds = 0; // Reset accumulator for the new day
}

void checkAutoSleep() {
    // Run this check once per second to save math cycles
    if (millis() - lastSleepCheckTime < 1000) return;
    lastSleepCheckTime = millis();

    if (!isTimeSynced()) return;

    time_t now; time(&now);
    struct tm * t = localtime(&now);
    int currentMins = (t->tm_hour * 60) + t->tm_min;

    // --- SLEEP WINDOW: 10:30 PM (1350) to 7:00 AM (420) ---
    bool shouldBeAsleep = (currentMins >= 1350 || currentMins < 420);

    // --- STATE TRANSITIONS ---

    // A. WAKE UP EVENT (7:00 AM)
    // Runs if the device is actually on and awake at 7:00 AM
    if (myPet.isSleeping && !shouldBeAsleep) {
        myPet.isSleeping = false;
        myPet.status = "IDLE";
        myPet.energy = 100;

        // Build the exact 7 AM timestamp for this morning
        struct tm tmWake = *localtime(&now);
        tmWake.tm_hour = 7; tmWake.tm_min = 0; tmWake.tm_sec = 0;
        time_t today7AM = mktime(&tmWake);

        evaluateSleepQuality(today7AM);
        saveGame();
    }

    // B. GO TO SLEEP EVENT (10:30 PM)
    else if (!myPet.isSleeping && shouldBeAsleep) {
        myPet.isSleeping = true;
        myPet.status = "SLEEPING";

        // Build the exact 10:30 PM timestamp for this evening
        struct tm tmBed = *localtime(&now);
        tmBed.tm_hour = 22; tmBed.tm_min = 30; tmBed.tm_sec = 0;
        time_t today1030PM = mktime(&tmBed);

        evaluateDayLightQuality(today1030PM);
        // Save so lightsOffTime is persisted before the device might be closed
        saveGame();
    }

    // --- [FAILSAFE] FORCE ENERGY REFILL IF JUST WOKE UP LIVE ---
    if (currentMins >= 420 && currentMins < 425 && myPet.energy < 100) {
        myPet.energy = 100;
    }
}

void checkDailyCoins() {
    if (!isTimeSynced()) return;
    time_t now; 
    time(&now);
    
    bool earnedCoins = false; // Track if we actually paid out

    // Loop to catch up on missed days
    while (now - myPet.lastCoinTime >= SECONDS_IN_DAY) {
        myPet.lastCoinTime += SECONDS_IN_DAY;

        int daysAlive = (myPet.lastCoinTime - myPet.birthTime) / SECONDS_IN_DAY;
        if (daysAlive < 0) daysAlive = 0;

        int payout = (daysAlive / 5) + 1;
        if (payout > 5) payout = 5;

        myPet.coins += payout;
        earnedCoins = true; // Mark that we changed data
        
        Serial.printf("[COINS] Catch-up Payout! Earned: %d\n", payout);
        screenDirty = true; 
    }
    
    // Only save if we actually changed something
    if (earnedCoins) {
         saveGame();
    }
}


void drawGardenBackground() {
    // Check Alignment (0=Neutral, 1=Angel, 2=Devil)
    if (myPet.alignment == 1) {
        // ANGEL
        dma_display->drawRGBBitmap(0, 12, garden_angel_background, 64, 40);
    } 
    else if (myPet.alignment == 2) {
        // DEVIL
        dma_display->drawRGBBitmap(0, 12, garden_devil_background, 64, 40);
    } 
    else {
        // NEUTRAL (Default)
        dma_display->drawRGBBitmap(0, 12, garden_background, 64, 40);
    }
}

void drawTransparentBitmap(int x, int y, const uint16_t *bitmap, int w, int h) {
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
            // Read color from program memory
            uint16_t color = pgm_read_word(&bitmap[j * w + i]);
            // Only draw if NOT transparent (Black)
            if (color != 0x0000) {
                dma_display->drawPixel(x + i, y + j, color);
            }
        }
    }
}

// 1. Age Multiplier (Fragility)
float getAgeMultiplier() {
    time_t now; time(&now);
    float days = (float)(now - myPet.birthTime) / SECONDS_IN_DAY;

    if (days < 5) return 1.0;
    if (days < 10) return 1.5;
    if (days < 20) return 2.0;
    return 3.0; // Senior Mode
}

// 2. Target Weight Calculation
int getTargetWeight() {
    time_t now; time(&now);
    int days = (now - myPet.birthTime) / SECONDS_IN_DAY;
    
    // Cap calculation at 18 days
    if (days > 18) days = 18;
    
    // Formula: 5 + (Age * 2) -> Max 41 lbs
    return 5 + (days * 2);
}

// 3. Apply Damage Helper
void applyDamage(float amount, bool isWeightDamage) {
    if (myPet.isDead) return;

    // Track when non-weight damage last occurred (used for 7-day streak bonus)
    if (!isWeightDamage) {
        time_t now; time(&now);
        myPet.lastNonWeightDamageTime = now;
    }

    float mult = getAgeMultiplier();
    float finalDmg = amount * mult;
    
    myPet.vitality -= finalDmg;
    if (myPet.vitality < 0) myPet.vitality = 0;
    
    // Immediate Death Save
    if (myPet.vitality == 0 && !myPet.isDead) {
        myPet.isDead = true;
        saveGame();
        Serial.println("!!! PET DIED !!!");
    }
    
    Serial.printf("[VITALITY] Hit: %.2f (Base: %.1f x %.1f). Remaining: %.2f\n", 
                  finalDmg, amount, mult, myPet.vitality);
}

void checkVitality() {
    if (myPet.isDead || !isTimeSynced()) return;

    time_t now; time(&now);

    // --- A. SICKNESS ---
    if (myPet.isSick) {
        long sickDuration = now - myPet.lastSicknessTime;
        
        // 1. One-Time Triggers (The "Catch Up" Logic)
        if (sickDuration > 300 && !myPet.hasTriggeredSick5Min) {
            applyDamage(1.0);
            myPet.hasTriggeredSick5Min = true;
        }
        if (sickDuration > 3600) {
            if (!myPet.hasTriggeredSick1Hr) {
                applyDamage(5.0);
                myPet.hasTriggeredSick1Hr = true;
                // Initialize the recurring timer aligned to when the critical hit theoretically happened
                myPet.nextSickDamageTime = myPet.lastSicknessTime + 3600 + (8 * 3600);
            }
            
            // 2. Recurring Loop (Handles multiple missed cycles)
            // If we are past the next damage time, keep hitting until we catch up
            while (now >= myPet.nextSickDamageTime) {
                applyDamage(3.0); // Live damage value
                myPet.nextSickDamageTime += (8 * 3600); // Advance by 8 hours
            }
        }
    } else {
        myPet.hasTriggeredSick5Min = false;
        myPet.hasTriggeredSick1Hr = false;
    }

    // --- B. HUNGER ---
    // Warning (<= 25%)
    if (myPet.hunger <= 25 && !myPet.hasTriggeredHungerWarn) {
        applyDamage(1.0);
        myPet.weight = max(1, myPet.weight - 2);
        myPet.hasTriggeredHungerWarn = true;
    } else if (myPet.hunger > 25) {
        myPet.hasTriggeredHungerWarn = false;
    }

    // Critical (0%)
    if (myPet.hunger <= 0) {
        if (!myPet.hasTriggeredHungerCrit) {
            applyDamage(5.0);
            myPet.weight = max(1, myPet.weight - 4);
            myPet.hasTriggeredHungerCrit = true;
            myPet.zeroHungerTime = now; // Start the clock
        }
        
        // Recurring Starvation Loop (Catch up logic)
        // If 12 hours have passed since zeroHungerTime...
        while (now - myPet.zeroHungerTime >= (12 * 3600)) {
            applyDamage(5.0);
            myPet.weight = max(1, myPet.weight - 3);
            // Advance the timer by 12 hours to keep the grid aligned
            myPet.zeroHungerTime += (12 * 3600); 
        }
    } else {
        myPet.hasTriggeredHungerCrit = false;
    }

    // --- C. HAPPINESS (FUN) ---
    // Warning (<= 25%)
    if (myPet.happiness <= 25 && !myPet.hasTriggeredFunWarn) {
        applyDamage(3.0);
        myPet.hasTriggeredFunWarn = true;
    } else if (myPet.happiness > 25) {
        myPet.hasTriggeredFunWarn = false;
    }
    
    // Critical (0%)
    if (myPet.happiness <= 0) {
        if (!myPet.hasTriggeredFunCrit) {
            applyDamage(5.0);
            myPet.hasTriggeredFunCrit = true;
            myPet.zeroFunTime = now;
        }
        // Depression Loop (Catch up logic)
        while (now - myPet.zeroFunTime >= (12 * 3600)) {
            applyDamage(5.0);
            myPet.zeroFunTime += (12 * 3600);
        }
    } else {
        myPet.hasTriggeredFunCrit = false;
    }

    // --- D. HYGIENE (DIRT) ---
    if (myPet.isDirty) {
        // Init timer if invalid
        if (myPet.nextDirtyDamageTime == 0) myPet.nextDirtyDamageTime = now + (8 * 3600);
        
        // Recurring Dirt Loop
        while (now >= myPet.nextDirtyDamageTime) {
            applyDamage(3.0);
            myPet.nextDirtyDamageTime += (8 * 3600);
        }
    } else {
        myPet.nextDirtyDamageTime = 0;
    }

    // --- E. DISCIPLINE (BEHAVIOR) ---
    if (myPet.isMisbehaving) {
        long misbehaveDuration = now - myPet.lastMisbehaveTime;
        
        // 1 Hour Neglect Trigger
        if (misbehaveDuration > 3600) {
             if (!myPet.hasTriggeredNeglect1Hr) {
                applyDamage(3.0);
                myPet.hasTriggeredNeglect1Hr = true;
                myPet.nextDisciplineDamageTime = myPet.lastMisbehaveTime + 3600 + (8 * 3600);
             }

             // Recurring Neglect Loop
             // Guard against 0 (unpersisted or old save) to prevent catch-up loop from epoch
             if (myPet.nextDisciplineDamageTime == 0) myPet.nextDisciplineDamageTime = myPet.lastMisbehaveTime + 3600 + (8 * 3600);
             while (now >= myPet.nextDisciplineDamageTime) {
                applyDamage(2.0);
                myPet.nextDisciplineDamageTime += (8 * 3600);
             }
        }
    } else {
        myPet.hasTriggeredNeglect1Hr = false;
    }
}

void checkDailyEvents() {
    time_t now; time(&now);
    
    // Initialize if 0 (first run)
    if (myPet.lastDailyCheckTime == 0) {
        myPet.lastDailyCheckTime = now;
        return;
    }

    // Run once per 24-hour period, catching up on all missed days
    bool ranCheck = false;
    while (now - myPet.lastDailyCheckTime >= SECONDS_IN_DAY) {
        ranCheck = true;
        Serial.println("[DAILY] Running Daily Health Check...");
 
        // 1. WEIGHT CHECK
        int target = getTargetWeight();
        int diff = abs(myPet.weight - target);
 
        if (diff > 5) {
            float excess = (float)(diff - 5);
            float penalty = floor(excess / 2.5);
            if (penalty < 1.0) penalty = 1.0;

            Serial.printf("[WEIGHT] Target: %d, Actual: %d. Penalty: %.1f\n", target, myPet.weight, penalty);
            applyDamage(penalty, true); // Weight damage does NOT reset the no-damage streak
        }

        // Advance by exactly one day so we catch up on all missed days
        myPet.lastDailyCheckTime += SECONDS_IN_DAY;
    }

    // 7-day no-non-weight-damage streak bonus
    if (myPet.lastNonWeightDamageTime != 0) {
        time_t streakDays = (now - myPet.lastNonWeightDamageTime) / SECONDS_IN_DAY;
        if (streakDays >= 7) {
            myPet.vitality = min(100.0f, myPet.vitality + 10.0f);
            // Advance the streak clock by 7 days so it re-triggers after another 7 clean days
            myPet.lastNonWeightDamageTime += (7 * SECONDS_IN_DAY);
            ranCheck = true; // Ensure saveGame() runs
            Serial.println("[VITALITY] +10 bonus: 7 days without non-weight damage!");
        }
    }

    if (ranCheck) {
        saveGame();
    }
}

void calculateOfflineProgress() {
    time_t now; time(&now);
    if (now < 100000) return; // Sanity check

    // --- 1. ENERGY RECOVERY & OFFLINE SLEEP EVALUATION ---
    struct tm tmNow = *localtime(&now);
    tmNow.tm_hour = 7; tmNow.tm_min = 0; tmNow.tm_sec = 0;
    time_t today7AM = mktime(&tmNow);

    // Sleep window: 10:30 PM the previous night to 7:00 AM
    time_t sleepWindowStart = today7AM - (8 * 3600 + 30 * 60);

    // Build the 10:30 PM timestamp for this evening
    struct tm tmBed = *localtime(&now);
    tmBed.tm_hour = 22; tmBed.tm_min = 30; tmBed.tm_sec = 0;
    time_t today1030PM = mktime(&tmBed);
    time_t dayWindowStart = today1030PM - (15 * 3600 + 30 * 60); // 7:00 AM

    if (now >= today7AM && myPet.isSleeping) {
        // Device was closed during the night and is now being opened after 7 AM.
        // lightsOffTime already holds the real Unix timestamp from when lights went off.
        Serial.println("[LOAD] Crossed 7AM boundary. Restoring Energy.");
        myPet.energy = 100;
        myPet.isSleeping = false;

        evaluateSleepQuality(today7AM);
        Serial.println("[LOAD] Offline sleep evaluated from lightsOffTime.");

    } else if (now < today7AM && now >= sleepWindowStart) {
        // Device opened mid-sleep-window — pet should still be asleep.
        // No accumulator needed; lightsOffTime already captures when lights went off.
        myPet.isSleeping = true;
        myPet.status = "SLEEPING";
        Serial.println("[LOAD] Booted during sleep window. Pet is sleeping.");
    }

    // --- 1B. DAYTIME LIGHT EVALUATION (offline crossing of 10:30 PM) ---
    // If device was closed during the day and reopened after 10:30 PM (and Chao was awake),
    // evaluate the daytime light quality against the 10:30 PM boundary.
    if (now >= today1030PM && !myPet.isSleeping && (myPet.lightsOffDayTime != 0 || myPet.accumulatedDayDarkSeconds > 0)) {
        evaluateDayLightQuality(today1030PM);
        Serial.println("[LOAD] Offline daytime light evaluated from lightsOffDayTime.");
    }

    // --- 2. RANDOM EVENT SPAWNER (RNG) ---
    // This gives the pet a chance to become sick/dirty while you were away.
    // NOTE: This does NOT apply damage. It just sets the flag. 
    // The damage will start ticking in checkVitality() immediately after this.

    // A. DIRTY LOGIC (33% Chance, 24h Immunity)
    if (!myPet.isDirty && (now - myPet.lastCleanedTime > SECONDS_IN_DAY)) {
        if (random(0, 100) < 33) {
            myPet.isDirty = true;
            myPet.lastShowerTime = now; 
            // We set the next damage time to 8 hours from NOW.
            // This gives the player a chance to clean it before damage starts.
            myPet.nextDirtyDamageTime = now + (8 * 3600);
            Serial.println("[LOAD] RNG: Pet spawned Dirty.");
        }
    }

    // B. SICK LOGIC (5% Chance, 48h Immunity)
    if (!myPet.isSick && (now - myPet.lastCuredTime > 172800)) {
        if (random(0, 100) < 5) {
            myPet.isSick = true;
            myPet.lastSicknessTime = now;
            // Set first damage warning triggers to FALSE so they can fire in checkVitality
            myPet.hasTriggeredSick5Min = false;
            myPet.hasTriggeredSick1Hr = false;
            myPet.happiness = max(0, myPet.happiness - 30);
            Serial.println("[LOAD] RNG: Pet spawned Sick.");
        }
    }

    // C. DISCIPLINE LOGIC (20% Chance, 24h Immunity)
    if (!myPet.isMisbehaving && !myPet.isSleeping && (now - myPet.lastDisciplinedTime > SECONDS_IN_DAY)) {
        if (random(0, 100) < 20) {
            myPet.isMisbehaving = true;
            myPet.lastMisbehaveTime = now;
            myPet.hasTriggeredNeglect1Hr = false;
            isAttentionNeeded = true;
            Serial.println("[LOAD] RNG: Pet spawned Misbehaving.");
        }
    }

    // Calculate any coins earned while the device was off or in clock mode
    checkDailyCoins();
}


void updateTamagotchi() {

    // --- CHECK VITALITY ---
    if (currentGameState != STATE_DEATH) {
        checkVitality();
    }

    // --- STATE: DEATH (R.I.P.) ---
    if (currentGameState == STATE_DEATH) {
        dma_display->fillScreen(0x0000);
        
        int yOffset = (millis() / 500) % 2; 
        dma_display->drawCircle(32, 20 + yOffset, 6, 0xFFE0); 
        dma_display->drawCircle(32, 20 + yOffset, 7, 0xFFE0);
        drawCenteredText("R.I.P.", 35, 0xF800); 
        
        time_t now; time(&now);
        int days = (now - myPet.birthTime) / SECONDS_IN_DAY;
        dma_display->setCursor(15, 50);
        dma_display->setTextColor(0xFFFF);
        dma_display->setTextSize(1);
        dma_display->print("AGE: "); dma_display->print(days);
        
        dma_display->flipDMABuffer();
        return; 
    }

    // --- LOGIC CALLS ---
    checkVitality();     // Real-time damage
    checkDailyEvents();  // Weight/Env damage

    // --- 1. HARDWARE DIMMING ---
    uint8_t targetBrightness;
    if (!myPet.isLightsOn && currentGameState == STATE_MAIN) {
        targetBrightness = 5; 
    } else {
        targetBrightness = BRIGHTNESS_LEVELS[currentBrightnessSetting]; 
    }

    static uint8_t lastAppliedBrightness = 255;
    if (lastAppliedBrightness != targetBrightness) {
        dma_display->setBrightness8(targetBrightness);
        lastAppliedBrightness = targetBrightness;
    }

    // --- 2. GAME LOGIC ---
    if (isTimeSynced()) {
        time_t now; time(&now);
        
        // Standard Hunger Logic
        while (now - myPet.lastHungerTime >= SECONDS_IN_12_HOURS) {
            myPet.lastHungerTime += SECONDS_IN_12_HOURS; 
            
            if (myPet.hunger > 0) {
                myPet.hunger -= 25;
                
                if (myPet.hunger <= 0) {
                    myPet.hunger = 0;
                    // Start the starvation clock for checkVitality to use
                    if (myPet.zeroHungerTime == 0) {
                        myPet.zeroHungerTime = myPet.lastHungerTime; 
                    }
                }
            }
        }
    }

    // --- 3. ANIMATION TIMER ---
    static unsigned long lastAnimTick = 0;
    if (millis() - lastAnimTick > 2000) {
        myPet.frame++;
        lastAnimTick = millis();
        screenDirty = true; 
    }

    // --- 4. UI CHANGE DETECTION ---
    static int last_selectedIcon = -1;
    static int last_subMenuSelection = -1;
    static GameState last_gameState = STATE_MAIN;
    static int last_statsPage = -1;
    static int last_playMenuSelection = -1;
    static bool last_isLightsOn = false;

    if (selectedIcon != last_selectedIcon || 
        subMenuSelection != last_subMenuSelection ||
        currentGameState != last_gameState ||
        statsPage != last_statsPage ||
        playMenuSelection != last_playMenuSelection ||
        myPet.isLightsOn != last_isLightsOn) {
            
        screenDirty = true;
        last_selectedIcon = selectedIcon;
        last_subMenuSelection = subMenuSelection;
        last_gameState = currentGameState;
        last_statsPage = statsPage;
        last_playMenuSelection = playMenuSelection;
        last_isLightsOn = myPet.isLightsOn;
    }

    if (currentGameState == STATE_GAME_PLAY || currentGameState == STATE_CONNECT_3 || currentGameState == STATE_FLAPPY_BIRD) {
        screenDirty = true;
    }

    if (!screenDirty) return;

    // ============================================================
    // DRAWING LOGIC
    // ============================================================

    // --- STATE: MAIN SCREEN ---
    if (currentGameState == STATE_MAIN) {
        
        // 1. Draw Background
        drawGardenBackground(); 

        // 2. Draw Top Bar
        dma_display->fillRect(0, 0, 64, 12, 0x0000); 
        uint16_t colorWhite = dma_display->color565(255, 255, 255);
        uint16_t colorGray = dma_display->color565(50, 50, 50);
        uint16_t colorSelect = dma_display->color565(255, 255, 0); 
        uint16_t colorAttn = dma_display->color565(255, 0, 0);       

        for (int i = 0; i < 4; i++) {
            int x = 4 + (i * 16); int y = 2;
            if (selectedIcon == i) dma_display->drawRect(x-2, y-2, 12, 12, colorSelect);
            drawIconBitmap(x, y, i, (selectedIcon == i) ? colorWhite : colorGray);
        }

        // 3. Draw Bottom Bar
        dma_display->fillRect(0, 52, 64, 12, 0x0000); 
        for (int i = 4; i < 8; i++) {
            int x = 4 + ((i - 4) * 16); int y = 54; 
            if (i == ICON_ATTENTION) {
                 bool alert = myPet.isMisbehaving || myPet.isSick || myPet.isDirty || (myPet.hunger < 20);
                 drawIconBitmap(x, y, i, alert ? colorAttn : colorGray);
            } else {
                 if (selectedIcon == i) dma_display->drawRect(x-2, y-2, 12, 12, colorSelect);
                 drawIconBitmap(x, y, i, (selectedIcon == i) ? colorWhite : colorGray);
            }
        }

        // 4. Draw Pet
        // --- COCOON CHECK ---
        if (myPet.evolutionStartTime > 0 && !myPet.hasEvolved) {
            // Center Ground
            int cx = 23; int cy = 30;
            drawChao(cx, cy, chao_cocoon); 
        } 
        // --- NORMAL CHAO ---
        else {
            int px = myPet.x;
            int py = myPet.y;

            if (myPet.status == "SLEEPING") {
                 int animFrame = myPet.frame % 2; 
                 const uint8_t* sleepSprite = chao_sleep[animFrame];
                 drawChao(px, py + 2, sleepSprite);
                 dma_display->setCursor(px + 18, py); 
                 dma_display->setTextColor(dma_display->color565(200, 200, 255)); 
                 dma_display->print("Z");
            }
            else {
                const uint8_t** currentActionArray = chao_poses[myPet.poseIndex];
                int animFrame = myPet.frame % 2;
                const uint8_t* currentSprite = currentActionArray[animFrame];
                drawChao(px, py, currentSprite);

                if (myPet.isSick) {
                    drawGenericBitmap(px + 12, py - 6, icon_skull, 0xFFFF);
                }
            }
        }
    }

    // --- STATE: SAVE SELECTION ---
    else if (currentGameState == STATE_SAVE_SELECT) {
        dma_display->fillScreen(0x0000); 
        
        dma_display->setTextSize(1); 
        dma_display->setTextColor(0xFFFF);
        dma_display->setCursor(10, 10); 
        dma_display->print("SAVE FILE");
        
        dma_display->setCursor(15, 30); 
        if(saveMenuSelection == 0) {
             dma_display->setTextColor(0x07E0); 
             dma_display->print("> LEAH");
        } else {
             dma_display->setTextColor(0x7BEF); 
             dma_display->print("  LEAH");
        }

        dma_display->setCursor(15, 45); 
        if(saveMenuSelection == 1) {
             dma_display->setTextColor(0x07E0); 
             dma_display->print("> JUAN");
        } else {
             dma_display->setTextColor(0x7BEF); 
             dma_display->print("  JUAN");
        }
    }

    // --- STATE: EGG SELECTION ---
    if (currentGameState == STATE_EGG_SELECT) {
        dma_display->drawRGBBitmap(0, 0, egg_select, 64, 64);

        uint8_t x1 = EGG_COORDS[eggSelectionIndex][0];
        uint8_t y1 = EGG_COORDS[eggSelectionIndex][1];
        uint8_t x2 = EGG_COORDS[eggSelectionIndex][2];
        uint8_t y2 = EGG_COORDS[eggSelectionIndex][3];
        int w = x2 - x1 + 1;
        int h = y2 - y1 + 1;
        
        dma_display->drawRect(x1 - 1, y1, w + 2, h, 0xFFFF); 

        dma_display->setTextSize(1);
        dma_display->setTextColor(0xFFE0); 
        
        String coinStr = String(myPet.coins);
        int16_t xd, yd; uint16_t wd, hd;
        dma_display->getTextBounds(coinStr, 0, 0, &xd, &yd, &wd, &hd);
        int centerX = std::max(0, (64 - (int)wd) / 2);
        dma_display->setCursor(centerX, 2);
        dma_display->print(coinStr);
    }

    // --- STATE: SUB MENUS ---
    else if (currentGameState == STATE_FEED_MENU) {
        dma_display->fillScreen(0x0000); 
        dma_display->setTextSize(1); dma_display->setTextColor(0xFFFF);
        dma_display->setCursor(20, 5); dma_display->print("FEED?");
        dma_display->setCursor(15, 25); dma_display->print(subMenuSelection == 0 ? "> SNACK" : "  SNACK");
        dma_display->setCursor(15, 40); dma_display->print(subMenuSelection == 1 ? "> MEAL"  : "  MEAL");
    }
    else if (currentGameState == STATE_MEDICINE_MENU) {
        dma_display->fillScreen(0x0000); 
        drawGenericBitmap(28, 5, icon_skull, 0xF800); 
        dma_display->setTextSize(1); dma_display->setTextColor(0xFFFF);
        dma_display->setCursor(16, 20); dma_display->print("SICK :(");
        dma_display->setCursor(5, 35); dma_display->print("GIVE MEDS");
        dma_display->setTextColor(0x07E0); dma_display->setCursor(10, 50); dma_display->print("> CONFIRM");
    }
    else if (currentGameState == STATE_BATHROOM_MENU) {
        dma_display->fillScreen(0x0000); 
        drawGenericBitmap(28, 5, icon_stink, dma_display->color565(100, 100, 50)); 
        dma_display->setTextSize(1); dma_display->setTextColor(0xFFFF);
        dma_display->setCursor(12, 20); dma_display->print("DIRTY :(");
        dma_display->setCursor(10, 35); dma_display->print("SHOWER?");
        dma_display->setTextColor(0x07E0); dma_display->setCursor(10, 50); dma_display->print("> CONFIRM");
    }
    
    // --- STATE: STATS ---
    else if (currentGameState == STATE_STATS) {
        dma_display->fillScreen(0x0000); 
        dma_display->setTextSize(1); dma_display->setTextColor(0xFFFF);
        int score = 0; int maxHearts = 4; uint16_t heartColor = 0xFFFF;

        if (statsPage == 0) { dma_display->setCursor(16, 5); dma_display->print("HUNGER"); score = myPet.hunger; heartColor = 0x07E0; } 
        else if (statsPage == 1) { dma_display->setCursor(22, 5); dma_display->print("FUN"); score = (myPet.happiness + myPet.hunger)/2; heartColor = 0xF81F; } 
        else if (statsPage == 2) { 
            dma_display->setCursor(10, 5); 
            dma_display->setTextColor(0xF800); 
            dma_display->print("VITALITY"); 
            
            const uint16_t* heartBmp = heart_vit_20; 
            if (myPet.vitality > 80)      heartBmp = heart_vit_100;
            else if (myPet.vitality > 60) heartBmp = heart_vit_80;
            else if (myPet.vitality > 40) heartBmp = heart_vit_60;
            else if (myPet.vitality > 20) heartBmp = heart_vit_40;
            
            dma_display->drawRGBBitmap(24, 25, heartBmp, 16, 16);
            maxHearts = 0; 
        }
        else if (statsPage == 3) {
             time_t now; time(&now); int age = (now - myPet.birthTime) / SECONDS_IN_YEAR;
             dma_display->setCursor(2, 5); dma_display->setTextColor(0x07E0); dma_display->print("STATS");
             dma_display->setTextColor(0xFFFF); 
             dma_display->setCursor(2, 20); dma_display->print("AGE: "); dma_display->print(age); dma_display->print(" YR");
             dma_display->setCursor(2, 35); dma_display->print("WGT: "); dma_display->print(myPet.weight); dma_display->print(" LB");
        }

        if (statsPage < 3) {
            // 1. Determine spacing
            int spacing = (maxHearts == 6) ? 9 : 14; 
            
            // 2. Calculate EXACT width of the heart row
            // Width = (Count * 8px Heart) + ((Count - 1) * Gap)
            // Note: Gap is (Spacing - 8)
            int totalRowWidth = (maxHearts * 8) + ((maxHearts - 1) * (spacing - 8));
            
            // 3. Calculate Start X to center it perfectly
            int startX = (64 - totalRowWidth) / 2;

            for (int i = 0; i < maxHearts; i++) {
                int x = startX + (i * spacing);
                
                bool filled = (maxHearts == 6) ? (i < score) : ((score > i * 25));
                if (filled) drawGenericBitmap(x, 25, heart_filled, heartColor);
                else drawGenericBitmap(x, 25, heart_empty, dma_display->color565(80, 80, 80));
            }
        }
        for(int i=0; i<4; i++) { dma_display->drawPixel(20 + (i * 8), 55, (statsPage == i) ? 0xFFFF : 0x3333); }
    }

    // --- STATE: PLAY MENU ---
    else if (currentGameState == STATE_PLAY_MENU) {
        dma_display->fillScreen(0x0000); 
        dma_display->setTextColor(0xFFFF);
        dma_display->setCursor(10, 10); dma_display->setTextColor(playMenuSelection == 0 ? 0x07E0 : 0xFFFF); dma_display->print("CATCH");
        dma_display->setCursor(10, 26); dma_display->setTextColor(playMenuSelection == 1 ? 0x07E0 : 0xFFFF); dma_display->print("CONNECT 3");
        if (myPet.alignment == 1) {
            dma_display->setCursor(10, 42); dma_display->setTextColor(playMenuSelection == 2 ? 0x07E0 : 0xFFFF); dma_display->print("FLAPPY");
        }
    }

    // --- STATE: CATCH GAME ---
    else if (currentGameState == STATE_GAME_PLAY) {
    dma_display->fillScreen(0x0000);
    
    if (millis() - lastGameFrame > GAME_TICK_RATE) {
        lastGameFrame = millis();
        gameCursorX += (gameSpeed * gameDirection);

        // --- CLAMP POSITION ---
        // 1. Hit Right Wall (60 is 64 width - 4 cursor width)
        if (gameCursorX >= 60) {
            gameCursorX = 60;    // Force it back to the edge
            gameDirection = -1;  // Bounce Left
        }
        // 2. Hit Left Wall
        else if (gameCursorX <= 0) {
            gameCursorX = 0;     // Force it back to the edge
            gameDirection = 1;   // Bounce Right
        }
        // ----------------------------------
    }
        dma_display->setTextColor(0xFFFF); dma_display->setCursor(20, 5); dma_display->print("CATCH!");
        dma_display->drawRect(24, 40, 16, 8, 0x07E0);
        dma_display->drawLine(0, 48, 63, 48, 0x528A);
        dma_display->fillRect(gameCursorX, 42, 4, 4, 0x07FF);
    }

    // --- STATE: CONNECT 3 ---
    else if (currentGameState == STATE_CONNECT_3) {
        
        // 1. CPU LOGIC (Runs at full speed so the AI doesn't lag)
        if (gameActive && playerTurn == 2 && millis() - lastCpuMoveTime > CPU_MOVE_DELAY) { 
             cpuMove(); 
        }

        // 2. DRAWING (Capped at ~30 FPS to kill the rolling black bars)
        static unsigned long lastDrawTime = 0;
        if (millis() - lastDrawTime > 33) { // 33ms = ~30 Frames Per Second
            lastDrawTime = millis();

            dma_display->fillScreen(0x0000); 

            const int CELL_SIZE = 10; const int BX = 7; const int BY = 12; 
            
            // Draw Cursor
            if (gameActive && playerTurn == 1) {
                int cx = BX + (connect3CursorX * CELL_SIZE);
                dma_display->fillTriangle(cx + 5, BY - 2, cx + 2, BY - 6, cx + 8, BY - 6, 0xF800);
            }
            
            // Draw Board
            for (int c = 0; c < 5; c++) {
                for (int r = 0; r < 5; r++) {
                    int x = c * CELL_SIZE + BX; int y = r * CELL_SIZE + BY;
                    dma_display->drawRect(x, y, CELL_SIZE, CELL_SIZE, 0xFFFF); 
                    if (board[c][r] == 1) dma_display->fillCircle(x + 5, y + 5, 3, 0xF800); 
                    else if (board[c][r] == 2) dma_display->fillCircle(x + 5, y + 5, 3, 0x001F); 
                }
            }
            
            // Draw Win/Lose Border
            if (!gameActive) {
                dma_display->drawRect(0, 0, 64, 64, (playerTurn == 1) ? 0x07E0 : 0xF800);
                dma_display->drawRect(1, 1, 62, 62, (playerTurn == 1) ? 0x07E0 : 0xF800);
            }
            
            dma_display->flipDMABuffer();
        }
    }

    // --- STATE: FLAPPY BIRD ---
    else if (currentGameState == STATE_FLAPPY_BIRD) {
        static unsigned long lastFlappyDraw = 0;
        if (millis() - lastFlappyDraw > 33) { // ~30 FPS
            lastFlappyDraw = millis();

            // --- PHYSICS & GAME UPDATE (only while actively flying) ---
            if (flappyActive && !flappyDead) {
                flappyVelY += FLAPPY_GRAVITY;
                flappyY += flappyVelY;

                // Move pipes left
                for (int i = 0; i < flappyActivePipes; i++) {
                    flappyPipes[i].x -= 1;
                }

                // Remove off-screen pipes
                for (int i = 0; i < flappyActivePipes; ) {
                    if (flappyPipes[i].x + FLAPPY_PIPE_W < 0) {
                        for (int j = i; j < flappyActivePipes - 1; j++) flappyPipes[j] = flappyPipes[j + 1];
                        flappyActivePipes--;
                    } else { i++; }
                }

                // Spawn new pipes
                flappyPipeTimer--;
                if (flappyPipeTimer <= 0 && flappyActivePipes < 3) {
                    int gapOptions[5] = {14, 22, 30, 38, 46};
                    flappyPipes[flappyActivePipes].x = 64;
                    flappyPipes[flappyActivePipes].gapY = gapOptions[random(0, 5)];
                    flappyPipes[flappyActivePipes].passed = false;
                    flappyActivePipes++;
                    flappyPipeTimer = 55;
                }

                // Score: passed a pipe
                for (int i = 0; i < flappyActivePipes; i++) {
                    if (!flappyPipes[i].passed && flappyPipes[i].x + FLAPPY_PIPE_W < FLAPPY_CHAO_X - 4) {
                        flappyPipes[i].passed = true;
                        flappyScore++;
                        // +20 happiness for reaching 1 point
                        if (flappyScore >= 1 && !flappyGave1PointBonus) {
                            myPet.happiness = min(100, myPet.happiness + 20);
                            flappyGave1PointBonus = true;
                        }
                        // +25 happiness for reaching 10 points
                        if (flappyScore >= 10 && !flappyGave10PointBonus) {
                            myPet.happiness = min(100, myPet.happiness + 25);
                            flappyGave10PointBonus = true;
                        }
                    }
                }

                // Collision: walls
                bool dead = (flappyY - 3 < 0 || flappyY + 3 > 61);

                // Collision: pipes
                for (int i = 0; i < flappyActivePipes && !dead; i++) {
                    int px = flappyPipes[i].x;
                    int pgapY = flappyPipes[i].gapY;
                    if (FLAPPY_CHAO_X + 3 > px && FLAPPY_CHAO_X - 3 < px + FLAPPY_PIPE_W) {
                        if ((int)flappyY - 3 < pgapY - FLAPPY_GAP_HALF || (int)flappyY + 3 > pgapY + FLAPPY_GAP_HALF) {
                            dead = true;
                        }
                    }
                }

                if (dead) {
                    flappyDead = true;
                    myPet.energy = max(0, myPet.energy - 15);
                }
            }

            // --- DRAW ---
            dma_display->fillScreen(0x0000);

            // Ground line
            dma_display->drawLine(0, 61, 63, 61, 0xA540);

            // Draw pipes
            for (int i = 0; i < flappyActivePipes; i++) {
                int px = flappyPipes[i].x;
                int pgapY = flappyPipes[i].gapY;
                int topH = pgapY - FLAPPY_GAP_HALF;
                int botY = pgapY + FLAPPY_GAP_HALF;
                if (topH > 0)  dma_display->fillRect(px, 0, FLAPPY_PIPE_W, topH, 0x07E0);
                if (botY < 62) dma_display->fillRect(px, botY, FLAPPY_PIPE_W, 62 - botY, 0x07E0);
                // Pipe caps (wider, slightly darker green)
                int capX = max(0, px - 1);
                int capW = (px - 1 < 0) ? FLAPPY_PIPE_W + px : FLAPPY_PIPE_W + 2;
                if (topH > 3)  dma_display->fillRect(capX, max(0, topH - 3), capW, 3, 0x0340);
                if (botY < 59) dma_display->fillRect(capX, botY, capW, 3, 0x0340);
            }

            // Draw Chao (Sonic Adventure Battle 2 style)
            int cx = FLAPPY_CHAO_X;
            int cy = (int)flappyY;
            dma_display->fillCircle(cx, cy, 3, currentChao.C_BL);           // body
            dma_display->fillCircle(cx, cy - 2, 2, currentChao.C_LB);       // head highlight
            dma_display->fillRect(cx - 1, cy - 9, 2, 2, currentChao.C_LB);  // floating orb (2x2 square)            
            dma_display->fillCircle(cx - 2, cy - 1, 1, currentChao.C_WH);  // left eye
            dma_display->fillCircle(cx + 1, cy - 1, 1, currentChao.C_WH);  // right eye
            dma_display->drawPixel(cx - 2, cy - 1, currentChao.C_DK);       // left pupil
            dma_display->drawPixel(cx + 1, cy - 1, currentChao.C_DK);       // right pupil
            dma_display->fillCircle(cx + 3, cy + 1, 1, currentChao.C_PK);  // right wing
            dma_display->fillCircle(cx - 3, cy + 1, 1, currentChao.C_PK);  // left wing

            // Score (top right)
            dma_display->setTextColor(0xFFFF);
            dma_display->setCursor(48, 2);
            dma_display->print(flappyScore);

            // Ready screen
            if (!flappyActive && !flappyDead) {
                dma_display->setCursor(16, 28);
                dma_display->setTextColor(0xFFE0);
                dma_display->print("TAP!");
            }

            // Game over overlay
            if (flappyDead) {
                dma_display->drawRect(0, 0, 64, 64, 0xF800);
                dma_display->drawRect(1, 1, 62, 62, 0xF800);
                dma_display->setCursor(10, 22);
                dma_display->setTextColor(0xFFFF);
                dma_display->print("SCORE");
                dma_display->setCursor(26, 33);
                dma_display->print(flappyScore);
            }

            dma_display->flipDMABuffer();
        }
    }

    dma_display->flipDMABuffer();
    screenDirty = false;
}

// *****************************************************************
// *** SAVE SYSTEM & TIME TRAVEL LOGIC ***
// *****************************************************************

void saveGame() {
    if (currentSaveSlot == "") {
        Serial.println("[SAVE ERROR] No save slot selected!");
        return; 
    }

    Serial.println("Saving game for: " + currentSaveSlot);
    preferences.begin(currentSaveSlot.c_str(), false); // RW Mode

    // 1. BASIC STATS
    preferences.putInt("hunger", myPet.hunger);
    preferences.putInt("happy", myPet.happiness);
    preferences.putInt("energy", myPet.energy);
    preferences.putInt("disc", myPet.discipline);
    preferences.putInt("sleep", myPet.sleepScore);
    preferences.putInt("weight", myPet.weight);
    preferences.putInt("color", myPet.colorID);

    // 2. TIMESTAMPS
    preferences.putUInt("birth", (uint32_t)myPet.birthTime);

    preferences.remove("l_hung"); 
    preferences.putUInt("l_hung", (uint32_t)myPet.lastHungerTime);
    
    preferences.remove("l_sick");
    preferences.putUInt("l_sick", (uint32_t)myPet.lastSicknessTime);
    
    preferences.remove("l_dirty");
    preferences.putUInt("l_dirty", (uint32_t)myPet.lastShowerTime);
    
    preferences.remove("l_misb");
    preferences.putUInt("l_misb", (uint32_t)myPet.lastMisbehaveTime);

    preferences.remove("n_disc_dmg");
    preferences.putUInt("n_disc_dmg", (uint32_t)myPet.nextDisciplineDamageTime);

    preferences.remove("l_clean");
    preferences.putUInt("l_clean", (uint32_t)myPet.lastCleanedTime);
    
    preferences.remove("l_cure");
    preferences.putUInt("l_cure",  (uint32_t)myPet.lastCuredTime);
    
    preferences.remove("l_disc");
    preferences.putUInt("l_disc",  (uint32_t)myPet.lastDisciplinedTime);

    preferences.remove("l_nwdmg");
    preferences.putUInt("l_nwdmg", (uint32_t)myPet.lastNonWeightDamageTime);
    
    // 3. STATES
    preferences.putBool("sick", myPet.isSick);
    preferences.putBool("dirty", myPet.isDirty);
    preferences.putBool("misb", myPet.isMisbehaving);
    preferences.putBool("lights", myPet.isLightsOn);

    // 4. VITALITY
    preferences.putFloat("vitality", myPet.vitality);
    preferences.putBool("isDead", myPet.isDead);

    // 5. EVOLUTION
    preferences.putInt("align", myPet.alignment);
    preferences.putBool("evolved", myPet.hasEvolved);
    preferences.putUInt("evoTime", (uint32_t)myPet.evolutionStartTime);

    // 6. ECONOMY
    preferences.putInt("coins", myPet.coins);
    preferences.putUInt("l_coin", (uint32_t)myPet.lastCoinTime);

    // --- SAVE CURRENT TIME ---
    // This lets us know exactly when you last played
    preferences.putUInt("lastSave", (uint32_t)time(NULL)); 

    // --- SAVE TRIGGER FLAGS (Prevents Boot Loop) ---
    preferences.putBool("t_hungW", myPet.hasTriggeredHungerWarn);
    preferences.putBool("t_hungC", myPet.hasTriggeredHungerCrit);
    preferences.putBool("t_funW",  myPet.hasTriggeredFunWarn);
    preferences.putBool("t_funC",  myPet.hasTriggeredFunCrit);
    preferences.putBool("t_sick5", myPet.hasTriggeredSick5Min);
    preferences.putBool("t_sick1", myPet.hasTriggeredSick1Hr);
    preferences.putBool("t_neg1",  myPet.hasTriggeredNeglect1Hr);

    // 7. SLEEP TIMESTAMP & ACCUMULATOR
    preferences.putUInt("lightsOff", (uint32_t)myPet.lightsOffTime);
    preferences.putLong("darkAccum", myPet.accumulatedDarkSeconds);
    // 8. DAYTIME LIGHT TIMESTAMP & ACCUMULATOR
    preferences.putUInt("lightsOffDay", (uint32_t)myPet.lightsOffDayTime);
    preferences.putLong("dayDarkAccum", myPet.accumulatedDayDarkSeconds);
    
    preferences.end();
    Serial.println("[SAVE] Game Saved Successfully.");
}

void loadGame(String slotName) {
    currentSaveSlot = slotName;
    Serial.println("Loading game for: " + currentSaveSlot);
    
    // OPEN IN READ-ONLY MODE
    preferences.begin(currentSaveSlot.c_str(), true); 
    
    uint32_t savedBirth = preferences.getUInt("birth", 0);
    
    if (savedBirth != 0) {
        // --- LOAD EXISTING FILE ---
        myPet.hunger = preferences.getInt("hunger", 100);
        myPet.happiness = preferences.getInt("happy", 80);
        myPet.energy = preferences.getInt("energy", 80);
        myPet.discipline = preferences.getInt("disc", 0);
        myPet.sleepScore = preferences.getInt("sleep", 6);
        myPet.weight = preferences.getInt("weight", 5);
        myPet.coins = preferences.getInt("coins", 10);
        myPet.lastCoinTime = (time_t)preferences.getUInt("l_coin", 0);
        
        // --- RESTORE SAVED COLOR ---
        myPet.colorID = preferences.getInt("color", 0);
        switch(myPet.colorID) {
            case 0: currentChao = CHAO_DEFAULT; break;
            case 1: currentChao = CHAO_DARK_BLUE; break;
            case 2: currentChao = CHAO_FOREST; break;
            case 3: currentChao = CHAO_YELLOW; break;
            case 4: currentChao = CHAO_PINK; break;
            case 5: currentChao = CHAO_RED; break;
            case 6: currentChao = CHAO_SILVER; break;
            case 7: currentChao = CHAO_SHADOW; break;
        }

        myPet.birthTime           = (time_t)savedBirth;
        myPet.lastHungerTime      = (time_t)preferences.getUInt("l_hung", 0);
        myPet.lastSicknessTime    = (time_t)preferences.getUInt("l_sick", 0);
        // Note: Saved key is "l_dirty", variable is lastShowerTime
        myPet.lastShowerTime      = (time_t)preferences.getUInt("l_dirty", 0); 
        myPet.lastMisbehaveTime        = (time_t)preferences.getUInt("l_misb", 0);
        myPet.nextDisciplineDamageTime = (time_t)preferences.getUInt("n_disc_dmg", 0);
        myPet.lastCleanedTime     = (time_t)preferences.getUInt("l_clean", 0);
        myPet.lastCuredTime       = (time_t)preferences.getUInt("l_cure", 0);
        myPet.lastDisciplinedTime = (time_t)preferences.getUInt("l_disc", 0);
        // If this is an existing save without this field, default to now so no spurious reward
        { time_t n; time(&n); myPet.lastNonWeightDamageTime = (time_t)preferences.getUInt("l_nwdmg", (uint32_t)n); }
        
        myPet.isSick = preferences.getBool("sick", false);
        myPet.isDirty = preferences.getBool("dirty", false);
        myPet.isMisbehaving = preferences.getBool("misb", false);
        myPet.isLightsOn = preferences.getBool("lights", true);
        
        // --- LOAD VITALITY & DEATH ---
        myPet.vitality = preferences.getFloat("vitality", 100.0);
        myPet.isDead = preferences.getBool("isDead", false);

        // --- LOAD EVOLUTION ---
        myPet.alignment = preferences.getInt("align", 0);
        myPet.hasEvolved = preferences.getBool("evolved", false);
        myPet.evolutionStartTime = (time_t)preferences.getUInt("evoTime", 0);
       
       // --- LOAD TRIGGER FLAGS ---
        // If these load as TRUE, the game won't damage you again instantly
        myPet.hasTriggeredHungerWarn = preferences.getBool("t_hungW", false);
        myPet.hasTriggeredHungerCrit = preferences.getBool("t_hungC", false);
        myPet.hasTriggeredFunWarn    = preferences.getBool("t_funW", false);
        myPet.hasTriggeredFunCrit    = preferences.getBool("t_funC", false);
        myPet.hasTriggeredSick5Min   = preferences.getBool("t_sick5", false);
        myPet.hasTriggeredSick1Hr    = preferences.getBool("t_sick1", false);
        myPet.hasTriggeredNeglect1Hr = preferences.getBool("t_neg1", false);

        // --- LOAD SLEEP TIMESTAMP & ACCUMULATOR ---
        myPet.lightsOffTime = (time_t)preferences.getUInt("lightsOff", 0);
        myPet.accumulatedDarkSeconds = preferences.getLong("darkAccum", 0);
        // --- LOAD DAYTIME LIGHT TIMESTAMP & ACCUMULATOR ---
        myPet.lightsOffDayTime = (time_t)preferences.getUInt("lightsOffDay", 0);
        myPet.accumulatedDayDarkSeconds = preferences.getLong("dayDarkAccum", 0);

        // This releases the lock so saveGame() can write later
        preferences.end(); 
        // ****************************************

        Serial.println("File Loaded. Calculating offline progress...");
        calculateOfflineProgress();

        // --- LOGIC: Check for death on load ---
        if (myPet.isDead) {
           // If he died in his sleep (or previous session), show the RIP screen now
           currentGameState = STATE_DEATH;
        } else {
           // He is alive, go to main garden
           currentGameState = STATE_MAIN;
           spawnPetRandomly(); 
        }
        
    } else {
        // --- NO SAVE FOUND -> GO TO EGG SELECT ---
        
        // 1. Try to load "coins" (Inheritance)
        myPet.coins = preferences.getInt("coins", 10);
        
        // *** CRITICAL FIX: CLOSE THE FILE HERE TOO ***
        preferences.end();
        // *********************************************
        
        Serial.println("No save found. Going to Egg Selection.");
        time_t now; time(&now);
        myPet.birthTime = now;
        myPet.lastHungerTime = now;
        myPet.lastCoinTime = now;
        myPet.lastNonWeightDamageTime = now;
        myPet.isLightsOn = true;
        myPet.alignment = 0;
        myPet.hasEvolved = false;
        myPet.evolutionStartTime = 0;

        // Reset cursor
        eggSelectionIndex = 0;
        // Switch to Egg Select Screen
        currentGameState = STATE_EGG_SELECT;
        screenDirty = true;
    }

    // ========================================================
    // --- TRANSFORMATION LOGIC (ON RELOAD ONLY) ---
    // ========================================================
    time_t now;
    time(&now);
    int ageYears = (now - myPet.birthTime) / SECONDS_IN_YEAR;

    // CHECK 1: ENTER COCOON
    // If 18+, Not Evolved, and NOT already in Cocoon -> Start Cocoon
    if (ageYears >= 18 && !myPet.hasEvolved && myPet.evolutionStartTime == 0) {
        Serial.println("[EVO] Chao is 18. Entering Cocoon State.");
        myPet.evolutionStartTime = now;
        
        saveGame(); // This will now work because preferences.end() was called above
    }

    // CHECK 2: HATCH FROM COCOON
    // If in Cocoon AND 24 hours have passed
    else if (myPet.evolutionStartTime > 0 && (now - myPet.evolutionStartTime >= SECONDS_IN_DAY)) {
        
        Serial.println("[EVO] Cocoon timer complete. Transforming!");
        // 1. Show Splash Screen
        dma_display->fillScreen(0x0000);
        drawCenteredText("TRANSFORM!", 28, 0xFFFF);
        dma_display->flipDMABuffer();
        delay(3000);
        // Suspense...

        // 2. Calculate Alignment
        // ANGEL (80-100)
        if (myPet.vitality >= 80) {
             myPet.alignment = 1;
             dma_display->fillScreen(0xFFFF); // White Flash
        } 
        // DEVIL (60-79)
        else if (myPet.vitality >= 60) {
             myPet.alignment = 2;
             dma_display->fillScreen(0x0000); // Black Flash
        } 
        // NEUTRAL (59-)
        else {
             myPet.alignment = 0;
             dma_display->fillScreen(0x001F); // Blue Flash
        }
        
        dma_display->flipDMABuffer();
        delay(500);

        // 3. Finalize
        myPet.hasEvolved = true;
        myPet.evolutionStartTime = 0; // Clear cocoon timer

        saveGame(); // Saves the new alignment and evolved state safely.
        
        Serial.printf("[EVO] Transformation Complete. Alignment: %d\n", myPet.alignment);
    }
}

// Helper to generate a Rainbow Color (RGB565)
uint16_t getRainbowColor() {
    // Adjust the "40" to change speed (Higher = Slower, Lower = Faster)
    byte pos = (millis() / 40) & 0xFF; 
    
    if (pos < 85) {
        return dma_display->color565(pos * 3, 255 - pos * 3, 0);
    } else if (pos < 170) {
        pos -= 85;
        return dma_display->color565(255 - pos * 3, 0, pos * 3);
    } else {
        pos -= 170;
        return dma_display->color565(0, pos * 3, 255 - pos * 3);
    }
}

// Helper function to draw a Chao using the Active Palette
void drawChao(int x, int y, const uint8_t* spriteFrame) {
  for (int i = 0; i < 18 * 18; i++) {
    uint8_t colorIndex = pgm_read_byte(&spriteFrame[i]); 
    uint16_t color = currentChao.C_TR; // Default

    switch (colorIndex) {
      case 0: color = currentChao.C_BK; break;
      case 1: color = currentChao.C_TR; break;

      // --- WINGS (Angel/Devil Logic) ---
      case 2: 
          if (myPet.alignment == 1) {
              color = 0xFFFF; // ANGEL: Pure White Wings
          } else if (myPet.alignment == 2) {
              color = 0x18e3; // DEVIL: Pure Black Wings
          } else {
              color = currentChao.C_PK; // NEUTRAL: Normal Palette
          }
          break;
      // --------------------------------

      case 3: color = currentChao.C_WH; break;
      case 4: color = currentChao.C_DK; break;
      
      // --- BODY (Dirty Logic) ---
      case 5: 
          if (myPet.isDirty && (i % 7 == 0)) {
              color = 0x8200; // Brown spots
          } else {
              color = currentChao.C_BL; 
          }
          break;
      // --------------------------

      case 6: color = currentChao.C_LB; break;
      
      // --- EMOTE BALL (Misbehaving/Shadow Logic) ---
      case 7: 
          if (myPet.isMisbehaving) {
              color = 0xF800; // RED
          } 
          else if (myPet.colorID == 7) { 
              color = getRainbowColor(); // Shadow Chao Rainbow
          } else {
              color = currentChao.C_EM; 
          }
          break;
      // -------------------------------------------

      // --- FEET/ACCENTS (Angel/Devil Logic) ---
      case 8: 
          if (myPet.alignment == 1) {
              color = 0xFFFF; // ANGEL: Pure White Feet
          } else if (myPet.alignment == 2) {
              color = 0x18e3; // DEVIL: Pure Black Feet
          } else {
              color = currentChao.C_YE; // NEUTRAL: Normal Palette
          }
          break;
      // ----------------------------------------
    }

    // Draw the pixel (Index 1 is transparency)
    if (colorIndex != 1) { 
      int px = x + (i % 18);
      int py = y + (i / 18);
      dma_display->drawPixel(px, py, color); 
    }
  }
}

// --- HELPER: CHECK IF POSITION IS VALID ---
bool isValidSpawn(int x, int relFeetY) {
    // x = relative to left of screen (0-63)
    // relFeetY = pixel count from TOP of background (starts at 0, ends at 40)
    
    // 1. TREE LOGIC (Block the specific tree pixels)
    // Pixels 51x20 to 52x23. We add a small buffer for the sprite width.
    // If the sprite's left edge is near the tree, or right edge overlaps.
    if (x >= 40 && x <= 55 && relFeetY >= 20 && relFeetY <= 25) {
        return false; 
    }

    // 2. POOL LOGIC (The diagonal line)
    // We determine the "Minimum Y" (highest point on screen) the feet can be.
    // If the random Y is higher (smaller number) than this, it's in the water.
    int minSafeY = 20; // Default sky limit (pixel 20)

    if (x < 3)       minSafeY = 32;
    else if (x < 6)  minSafeY = 33;
    else if (x < 10) minSafeY = 34;
    else if (x < 12) minSafeY = 35;
    else if (x < 17) minSafeY = 36;
    else if (x < 19) minSafeY = 37;
    else if (x < 22) minSafeY = 38;
    else             minSafeY = 39; // x >= 22 (Pool ends, but keep curve logic)
    
    // If we are to the right of the pool (x > 30), we can technically go back up to 20
    // But let's smooth it out. If x > 25, let it go back to sky limit (20)
    if (x > 25) minSafeY = 20;

    // If the feet are "higher" (smaller Y value) than the safe limit, invalid.
    if (relFeetY < minSafeY) return false;

    return true;
}

// --- HELPER: SPAWN PET ---
void spawnPetRandomly() {
    bool validSpotFound = false;
    int attempts = 0;
    
    // Dimensions
    int topNavHeight = 12;
    int spriteW = 18; 
    int spriteH = 18; 
    
    while (!validSpotFound && attempts < 100) {
        int randX = random(0, 64 - spriteW);
        int randFeetY = random(22, 40); 
        
        if (isValidSpawn(randX, randFeetY)) {
            myPet.x = randX;
            myPet.y = (topNavHeight + randFeetY) - spriteH; 
            validSpotFound = true;
        }
        attempts++;
    }

    if (!validSpotFound) {
        myPet.x = 23; myPet.y = 23; 
    }

    // --- ANIMATION SELECTION LOGIC ---
    // 0: Stand, 1: Walk, 2: Lay, 3: Crawl, 4: Chill
    
    time_t now = time(NULL);
    // Uses your defined year length (whether 1 day or 2 days)
    int ageYears = (now - myPet.birthTime) / SECONDS_IN_YEAR; 

    if (ageYears < 2) {
        // BABY (0-1): Lay (2) or Crawl (3)
        myPet.poseIndex = random(2, 4); 
    } 
    else if (ageYears > 5) {
        // MATURE (6+): No Crawling (3) allowed.
        // We want to pick randomly between: 0 (Stand), 1 (Walk), 2 (Lay), 4 (Chill)
        
        // 1. Define the valid options for an old pet
        int validPoses[] = {0, 1, 2, 4};
        
        // 2. Pick a random index (0, 1, 2, or 3)
        // random(0, 4) returns 0, 1, 2, or 3
        int randIndex = random(0, 4); 
        
        // 3. Assign the actual pose number from the list
        myPet.poseIndex = validPoses[randIndex];
    }
    else {
        // YOUNG ADULT (2-5): Anything goes (0-4)
        myPet.poseIndex = random(0, 5); 
    }
    // ---------------------------------
    
    // Reset animation frame so it starts clean
    myPet.frame = 0; 
}

// Post Chao Code setup now
// --- ARDUINO SETUP FUNCTION ---

void setup() {
    Serial.begin(115200);

    // --- TEMPORARY WIPE COMMAND ---
    // Uncomment these 3 lines, upload the code, let it run once.
    // Then comment them out and upload again.
    // preferences.begin("leah", false); preferences.clear(); preferences.end();
    // preferences.begin("juan", false); preferences.clear(); preferences.end();
    // Serial.println("!!! SAVES WIPED !!!");
    // ------------------------------

    // 1. Set up button pins FIRST
    pinMode(BTN_MUNI_N_PIN, INPUT_PULLUP); pinMode(BTN_MUNI_S_PIN, INPUT_PULLUP);
    pinMode(BTN_BART_N_PIN, INPUT_PULLUP); pinMode(BTN_BART_S_PIN, INPUT_PULLUP);
    pinMode(BTN_DRIVE_A_PIN, INPUT_PULLUP); pinMode(BTN_DRIVE_B_PIN, INPUT_PULLUP);

    // 2. Initialize the display hardware
    Serial.println("[Display] Initializing Matrix Panel...");
    
    // Set S3-specific config
    mxconfig.clkphase = false;
    mxconfig.i2sspeed = HUB75_I2S_CFG::HZ_20M; 
    
    dma_display = new MatrixPanel_I2S_DMA(mxconfig);
    
    if (!dma_display->begin()) {
        Serial.println("!!! FAILED to allocate memory for display !!!");
        while(true) delay(1000);
    }

    // Set brightness
    dma_display->setBrightness8(BRIGHTNESS_LEVELS[currentBrightnessSetting]); 
    Serial.println("[Display] Init OK.");

    // 3. Draw Boot Screen
    renderDisplay("BOOTING", "Connecting", "#FFFFFF");

    // --- WATCHDOG CONFIGURATION ---
    
    // 1. Turn OFF the default 5-second watchdog
    esp_task_wdt_deinit(); 

    // 2. Define 30-second rule
    esp_task_wdt_config_t twdt_config = {
        .timeout_ms = 30000, // 30 Seconds (plenty of time for slow WiFi)
        .idle_core_mask = (1 << 0),
        .trigger_panic = true
    };
    
    // 3. Start the watchdog
    esp_task_wdt_init(&twdt_config);
    
    // 4. Add the current task (loop) to the watchdog
    esp_task_wdt_add(NULL); 

    // 5. Connect to WiFi
    Serial.print("\nConnecting to WiFi...");
    WiFi.begin(WIFI_SSID, WIFI_PASS); 
    int attempts = 0;
    
    // --- FEED DOG IN LOOP ---
    while (WiFi.status() != WL_CONNECTED && attempts < 20) { 
        delay(500); 
        Serial.print("."); 
        attempts++; 
        esp_task_wdt_reset(); 
    }
    
    if (WiFi.status() == WL_CONNECTED) { 
        Serial.println("\nWiFi CONNECTED"); 
    } else { 
        Serial.println("\nWiFi FAILED. Check credentials."); 
        renderDisplay("ERROR", "WIFIFAILED", "#FF0000");
        // Don't infinite loop here, or watchdog will kill it.
    }

    // 6. Start the background API fetching task
    xTaskCreatePinnedToCore(
        api_fetch_task,       
        "API_Fetch_Task",     
        API_TASK_STACK_SIZE, 
        NULL,                 
        1,                    
        &apiTaskHandle,       
        0                     
    );
    
    // 7. Sync Time
    renderDisplay("BOOTING", "Syncing", "#FFFFFF");
    configTime(0, 0, NTP_SERVER); 
    setenv("TZ", TIMEZONE_INFO, 1);
    tzset();

    Serial.println("[TIME CHECK] Waiting for stable time sync...");
    
    // --- FEED DOG IN LOOP ---
    int time_attempts = 0;
    while (!isTimeSynced()) {
        delay(1000); 
        Serial.print(".");
        esp_task_wdt_reset(); 
        
        time_attempts++;
        if(time_attempts > 60) {
             Serial.println("Time Sync Timeout. Rebooting...");
             ESP.restart(); 
        }
    }
    Serial.println("\n[TIME] Sync complete.");

    // 8. Reset Cache Timers
    lastBartFetchTime = 0;
    lastMuniNorthFetchTime = 0;
    lastMuniSouthFetchTime = 0;

    // 9. Final state
    Serial.println("\n[TIME] Sync complete. Device is READY. Loading clock...");
    isBooting = false;
    isClockActive = true;       
    lastActivityTime = 0; 

    // 10. Initialize Pet Timers if this is a fresh boot
    time_t now; 
    time(&now);
    if (myPet.birthTime == 0) {
        myPet.birthTime = now;
        myPet.lastHungerTime = now;
        myPet.zeroHungerTime = 0; // Not starving yet
    }
    // --- SPAWN PET ---
    spawnPetRandomly();
}

// Helper to center text on the 64x64 screen
void drawCenteredText(String text, int y, uint16_t color) {
    int16_t x1, y1; uint16_t w, h;
    dma_display->getTextBounds(text, 0, 0, &x1, &y1, &w, &h);
    int x = (64 - w) / 2;
    // Ensure we don't go off-screen
    if (x < 0) x = 0; 
    
    dma_display->setCursor(x, y);
    dma_display->setTextColor(color);
    dma_display->print(text);
}


void handleButtonPress() {
    static unsigned long lastPressTime = 0;
    static unsigned long comboStartTime = 0; 

    // --- 1. READ BUTTONS ---
    bool muniNPressed = (digitalRead(BTN_MUNI_N_PIN) == LOW); 
    bool muniSPressed = (digitalRead(BTN_MUNI_S_PIN) == LOW); 
    bool bartNPressed = (digitalRead(BTN_BART_N_PIN) == LOW); 
    bool bartSPressed = (digitalRead(BTN_BART_S_PIN) == LOW); 
    bool driveAPressed = (digitalRead(BTN_DRIVE_A_PIN) == LOW); 
    bool driveBPressed = (digitalRead(BTN_DRIVE_B_PIN) == LOW); 

    // --- 2. MASTER COMBO (Toggle Game) ---
    if (driveBPressed && muniSPressed) { 
        if (comboStartTime == 0) comboStartTime = millis();
        if (millis() - comboStartTime > 1000) {
            isTamagotchiActive = !isTamagotchiActive;
            comboStartTime = 0;          
            lastPressTime = millis(); 
            
            if (isTamagotchiActive) {
                // ===============================================
                // 1. POCKET CHAO INTRO SEQUENCE
                // ===============================================
                unsigned long introStartTime = millis();
                bool waitingForInput = true;
                bool abortStartup = false; // [NEW] Flag to track if we are cancelling
                
                // Wait for the combo buttons to be released 
                while(digitalRead(BTN_DRIVE_B_PIN) == LOW || digitalRead(BTN_MUNI_S_PIN) == LOW) {
                    delay(10);
                }

                // Blocking Loop for Intro Animation
                while (waitingForInput) {
                    unsigned long currentMillis = millis();
                    unsigned long elapsed = currentMillis - introStartTime;

                    // 1. Calculate Frame (Flip every 2000ms)
                    int pFrame = (elapsed / 2000) % 2;

                    // 2. Draw the Splash Screen
                    dma_display->drawRGBBitmap(0, 0, pocketchao_frames[pFrame], 64, 64);
                    dma_display->flipDMABuffer();

                    // 3. Input Check (Only allowed AFTER 0.5 seconds)
                    if (elapsed > 500) {
                         
                         // [A] BACK BUTTON -> CANCEL STARTUP
                         if (digitalRead(BTN_DRIVE_A_PIN) == LOW) {
                             waitingForInput = false;
                             abortStartup = true;
                         }
                         
                         // [B] ANY OTHER BUTTON -> START GAME
                         else if (digitalRead(BTN_MUNI_N_PIN) == LOW || digitalRead(BTN_MUNI_S_PIN) == LOW ||
                             digitalRead(BTN_BART_N_PIN) == LOW || digitalRead(BTN_BART_S_PIN) == LOW ||
                             digitalRead(BTN_DRIVE_B_PIN) == LOW) {
                                 waitingForInput = false;
                                 // abortStartup stays false
                         }
                    }
                    
                    esp_task_wdt_reset();
                    delay(10);
                }
                
                // --- LOGIC SPLIT: EXIT OR START ---

                // CASE 1: USER PRESSED BACK (EXIT)
                if (abortStartup) {
                    dma_display->setBrightness8(BRIGHTNESS_LEVELS[currentBrightnessSetting]);
                    isTamagotchiActive = false;
                    isClockActive = true; 
                    justExitedClock = true; 
                    lastActivityTime = 0; 
                    lastClockRenderTime = 0;
                    
                    // Wait for release
                    while(digitalRead(BTN_DRIVE_A_PIN) == LOW) { delay(10); }
                    lastPressTime = millis();
                    return; // EXIT FUNCTION IMMEDIATELY
                }

                // CASE 2: USER PRESSED START (CONTINUE)
                // ===============================================
                // 2. TRANSITION TO SAVE SELECT
                // ===============================================
                
                // Wait for the button that started the game to be released
                while(digitalRead(BTN_MUNI_N_PIN) == LOW || digitalRead(BTN_MUNI_S_PIN) == LOW ||
                      digitalRead(BTN_BART_N_PIN) == LOW || digitalRead(BTN_BART_S_PIN) == LOW ||
                      digitalRead(BTN_DRIVE_B_PIN) == LOW) {
                    delay(10);
                }
                
                currentGameState = STATE_SAVE_SELECT; 
                saveMenuSelection = 0; 
                lastPressTime = millis(); 
                
                dma_display->setBrightness8(myPet.isLightsOn ? 60 : 5);

            } else {
                // === EXITING GAME ===
                dma_display->setBrightness8(BRIGHTNESS_LEVELS[currentBrightnessSetting]);
                isClockActive = true; justExitedClock = true; lastActivityTime = 0; lastClockRenderTime = 0; 
            }
        }
        return; 
    } else {
        comboStartTime = 0; 
    }

    // --- 3. IF GAME IS ACTIVE ---
    if (isTamagotchiActive) {
        if (millis() - lastPressTime < 250) return; 

        // --------------------------------------
        // STATE: SAVE SELECTION
        // --------------------------------------
        if (currentGameState == STATE_SAVE_SELECT) {
            
            // TOGGLE SELECTION (BART N)
            if (bartNPressed) {
                saveMenuSelection = !saveMenuSelection; 
                screenDirty = true;
                lastPressTime = millis();
                while(digitalRead(BTN_BART_N_PIN) == LOW) { delay(10); }
            }
            
            // SELECT FILE (BART S)
            else if (bartSPressed) {
                // --- LOAD LOGIC ---
                dma_display->fillScreen(0x0000);
                drawCenteredText("LOADING", 28, 0xFFFF); 
                dma_display->flipDMABuffer();
                
                if (saveMenuSelection == 0) loadGame("leah");
                else loadGame("juan");

                while(digitalRead(BTN_BART_S_PIN) == LOW) { delay(10); }
                lastPressTime = millis();
                return; 
            }

            // [NEW] BACK BUTTON (Return to Title Screen)
            else if (driveAPressed) {
                // 1. Wait for Back Button Release
                while(digitalRead(BTN_DRIVE_A_PIN) == LOW) { delay(10); }

                // 2. RUN INTRO SEQUENCE AGAIN
                unsigned long introStartTime = millis();
                bool waitingForInput = true;
                bool exitToClock = false; 

                while (waitingForInput) {
                    unsigned long currentMillis = millis();
                    unsigned long elapsed = currentMillis - introStartTime;

                    // Draw Intro Frame
                    int pFrame = (elapsed / 2000) % 2;
                    dma_display->drawRGBBitmap(0, 0, pocketchao_frames[pFrame], 64, 64);
                    dma_display->flipDMABuffer();

                    // Input Check (Debounce 250ms)
                    if (elapsed > 250) {
                         // BACK (Drive A) -> Exit to Clock
                         if (digitalRead(BTN_DRIVE_A_PIN) == LOW) {
                             waitingForInput = false;
                             exitToClock = true;
                         }
                         // START (Any other) -> Return to Save Select
                         else if (digitalRead(BTN_MUNI_N_PIN) == LOW || digitalRead(BTN_MUNI_S_PIN) == LOW ||
                             digitalRead(BTN_BART_N_PIN) == LOW || digitalRead(BTN_BART_S_PIN) == LOW ||
                             digitalRead(BTN_DRIVE_B_PIN) == LOW) {
                                 waitingForInput = false;
                                 exitToClock = false;
                         }
                    }
                    esp_task_wdt_reset();
                    delay(10);
                }

                // 3. HANDLE RESULT
                if (exitToClock) {
                    // EXIT THE GAME COMPLETELY
                    dma_display->setBrightness8(BRIGHTNESS_LEVELS[currentBrightnessSetting]);
                    isTamagotchiActive = false;
                    isClockActive = true; 
                    justExitedClock = true; 
                    lastActivityTime = 0; 
                    lastClockRenderTime = 0;
                    
                    while(digitalRead(BTN_DRIVE_A_PIN) == LOW) { delay(10); }
                    lastPressTime = millis();
                    return;
                } else {
                    // RETURN TO SAVE SELECT
                    // We just stay in this state, but we wait for the start button to release
                    while(digitalRead(BTN_MUNI_N_PIN) == LOW || digitalRead(BTN_MUNI_S_PIN) == LOW ||
                          digitalRead(BTN_BART_N_PIN) == LOW || digitalRead(BTN_BART_S_PIN) == LOW ||
                          digitalRead(BTN_DRIVE_B_PIN) == LOW) {
                        delay(10);
                    }
                    screenDirty = true; // Redraw the save menu
                    lastPressTime = millis();
                    return;
                }
            }
        }

        // --------------------------------------
        // STATE: EGG SELECTION
        // --------------------------------------
        else if (currentGameState == STATE_EGG_SELECT) {
            
            // SCROLL (BART_N) -> Find next AFFORDABLE egg
            if (bartNPressed) {
                int attempts = 0;
                // Loop until we find an affordable egg
                do {
                    eggSelectionIndex++;
                    if (eggSelectionIndex > 7) eggSelectionIndex = 0;
                    attempts++;
                } while (myPet.coins < EGG_PRICES[eggSelectionIndex] && attempts < 9);
                
                screenDirty = true;
                lastPressTime = millis();
                while(digitalRead(BTN_BART_N_PIN) == LOW) { delay(10); }
            }
            
            // CONFIRM (BART_S) -> Buy & Hatch
            else if (bartSPressed) {
                
                // 1. DEDUCT COST
                int cost = EGG_PRICES[eggSelectionIndex];
                if (myPet.coins >= cost) {
                    myPet.coins -= cost;
                    
                    // 2. Set Color
                    myPet.colorID = eggSelectionIndex;
                    switch(eggSelectionIndex) {
                        case 0: currentChao = CHAO_DEFAULT; break;
                        case 1: currentChao = CHAO_DARK_BLUE; break;
                        case 2: currentChao = CHAO_FOREST; break;
                        case 3: currentChao = CHAO_YELLOW; break;
                        case 4: currentChao = CHAO_PINK; break;
                        case 5: currentChao = CHAO_RED; break;
                        case 6: currentChao = CHAO_SILVER; break;
                        case 7: currentChao = CHAO_SHADOW; break;
                    }

                    // 3. Reset Stats
                    myPet.hunger = 100;
                    myPet.happiness = 80;
                    myPet.energy = 80;
                    myPet.weight = 5; 
                    myPet.discipline = 0;
                    myPet.sleepScore = 6;
                    myPet.isSick = false;
                    myPet.isDirty = false;
                    myPet.isMisbehaving = false;

                    myPet.vitality = 100.0; 
                    myPet.isDead = false;

                    myPet.alignment = 0;
                    myPet.hasEvolved = false;
                    myPet.evolutionStartTime = 0;
                    
                    time_t now = time(NULL); // Get current time once
                    myPet.birthTime = now; 
                    myPet.lastCoinTime = now; 

                    // --- GRANT INITIAL IMMUNITY ---
                    myPet.lastCleanedTime = now;
                    myPet.lastCuredTime = now;
                    myPet.lastDisciplinedTime = now;

                    
                    // 4. Save & Spawn
                    saveGame(); 
                    spawnPetRandomly();
                    currentGameState = STATE_MAIN; // Change State
                    
                    // 5. Visual Feedback (CENTERED)
                    dma_display->fillScreen(0x0000);
                    drawCenteredText("HATCHED!", 28, 0x07E0); // Green, Centered
                    dma_display->flipDMABuffer();
                    delay(1500);

                    // Wait for release & Stop
                    while(digitalRead(BTN_BART_S_PIN) == LOW) { delay(10); }
                    lastPressTime = millis();
                    return; 
                }
            }
            // BACK BUTTON (Return to Save Select)
            else if (driveAPressed) {
                currentGameState = STATE_SAVE_SELECT;
                screenDirty = true;
                while(digitalRead(BTN_DRIVE_A_PIN) == LOW) { delay(10); }
                lastPressTime = millis();
                return;
            }
        }


        // --------------------------------------
        // STATE: MAIN SCREEN
        // --------------------------------------
        else if (currentGameState == STATE_MAIN) {
            
            // A BUTTON (Scroll)
            if (bartNPressed) {
                selectedIcon++;
                if (selectedIcon > 7) selectedIcon = 0; 
                lastPressTime = millis();
            }
            
            // B BUTTON (Select)
            else if (bartSPressed) {
                if (selectedIcon == ICON_FEED) {
                    if (!myPet.isSleeping) { 
                        currentGameState = STATE_FEED_MENU;
                        subMenuSelection = 0; 
                    }
                }
                else if (selectedIcon == ICON_LIGHT) {
                    myPet.isLightsOn = !myPet.isLightsOn;
                    if (!myPet.isLightsOn) {
                        // Record the exact moment this darkness period began
                        time_t now; time(&now);
                        myPet.lightsOffTime = now;
                        // Also track daytime darkness if Chao is awake
                        if (!myPet.isSleeping) {
                            myPet.lightsOffDayTime = now;
                        }
                    } else {
                        // Lights back on — accumulate the darkness period that just ended
                        time_t now; time(&now);
                        if (myPet.lightsOffTime != 0) {
                            struct tm tmWake = *localtime(&now);
                            tmWake.tm_hour = 7; tmWake.tm_min = 0; tmWake.tm_sec = 0;
                            time_t today7AM = mktime(&tmWake);
                            time_t sleepWindowStart = today7AM - (8 * 3600 + 30 * 60); // 10:30 PM
                            // Cap the end of the dark period at now or 7AM, whichever is earlier
                            time_t darkEnd = (now < today7AM) ? now : today7AM;
                            time_t effectiveStart = (myPet.lightsOffTime > sleepWindowStart) ? myPet.lightsOffTime : sleepWindowStart;
                            if (darkEnd > effectiveStart) {
                                myPet.accumulatedDarkSeconds += (long)(darkEnd - effectiveStart);
                            }
                            myPet.lightsOffTime = 0;
                        }
                        // Accumulate daytime darkness if Chao is awake
                        if (!myPet.isSleeping && myPet.lightsOffDayTime != 0) {
                            struct tm tmBed = *localtime(&now);
                            tmBed.tm_hour = 22; tmBed.tm_min = 30; tmBed.tm_sec = 0;
                            time_t today1030PM = mktime(&tmBed);
                            time_t dayWindowStart = today1030PM - (15 * 3600 + 30 * 60); // 7:00 AM
                            // Cap the end of the dark period at now or 10:30 PM, whichever is earlier
                            time_t darkEnd = (now < today1030PM) ? now : today1030PM;
                            time_t effectiveStart = (myPet.lightsOffDayTime > dayWindowStart) ? myPet.lightsOffDayTime : dayWindowStart;
                            if (darkEnd > effectiveStart) {
                                myPet.accumulatedDayDarkSeconds += (long)(darkEnd - effectiveStart);
                            }
                            myPet.lightsOffDayTime = 0;
                        }
                    }
                    saveGame();
                    screenDirty = true;
                }
                else if (selectedIcon == ICON_PLAY) {
                    if (!myPet.isSleeping) {
                        if (myPet.energy >= 0) {
                            currentGameState = STATE_PLAY_MENU;
                            playMenuSelection = 0; 
                        } else {
                            // Too tired feedback
                            dma_display->fillScreen(0x0000);
                            dma_display->setCursor(16, 28);
                            dma_display->setTextColor(0xF800); 
                            dma_display->print("TIRED!");
                            dma_display->flipDMABuffer();
                            delay(1000);
                        }
                    } else { 
                        // Sleeping feedback
                        dma_display->fillScreen(0x0000);
                        dma_display->setCursor(16, 28);
                        dma_display->setTextColor(0x001F); 
                        dma_display->print("Zzz...");
                        dma_display->flipDMABuffer();
                        delay(1000);
                    }
                }
                else if (selectedIcon == ICON_MEDICINE) {
                    if (myPet.isSick) {
                        currentGameState = STATE_MEDICINE_MENU;
                    } else {
                        // Healthy feedback
                        dma_display->fillScreen(0x0000);
                        dma_display->setCursor(12, 28);
                        dma_display->setTextColor(0x07E0); 
                        dma_display->print("HEALTHY!");
                        dma_display->flipDMABuffer();
                        delay(1000);
                    }
                }
                else if (selectedIcon == ICON_BATHROOM) {
                    if (myPet.isDirty) {
                        currentGameState = STATE_BATHROOM_MENU;
                    } else {
                        // Clean feedback
                        dma_display->fillScreen(0x0000);
                        dma_display->setCursor(20, 28);
                        dma_display->setTextColor(0x07E0); 
                        dma_display->print("CLEAN!");
                        dma_display->flipDMABuffer();
                        delay(1000);
                    }
                }
                else if (selectedIcon == ICON_DISCIPLINE) {
                    if (myPet.isMisbehaving) {
                        myPet.isMisbehaving = false;
                        isAttentionNeeded = false; 
                        myPet.discipline = min(100, myPet.discipline + 10);
                        // Set Immunity Timer
                        time_t now; time(&now);
                        myPet.lastDisciplinedTime = now;
                        
                        // Praise feedback
                        dma_display->fillScreen(0x0000);
                        dma_display->setCursor(16, 28);
                        dma_display->setTextColor(0x07E0); 
                        dma_display->print("PRAISED"); 
                        dma_display->flipDMABuffer();
                        delay(1000);
                    } else {
                        myPet.happiness = max(0, myPet.happiness - 25);
                        applyDamage(3.0);
                        // Mistake feedback
                        dma_display->fillScreen(0x0000);
                        dma_display->setCursor(16, 28);
                        dma_display->setTextColor(0xF800); 
                        dma_display->print("WHY? :(");
                        dma_display->flipDMABuffer();
                        delay(1000);
                    }
                }
                else if (selectedIcon == ICON_METER) {
                      currentGameState = STATE_STATS;
                      statsPage = 0;
                }
                
                // Wait for release & Return
                while(digitalRead(BTN_BART_S_PIN) == LOW) { delay(10); } 
                lastPressTime = millis();
                return; 
            }
            
            // C BUTTON (Exit Game)
            else if (driveAPressed) {
                // Save & Exit Logic
                dma_display->fillScreen(0x0000);
                dma_display->setCursor(20, 28);
                dma_display->setTextColor(0xFFFF);
                dma_display->print("SAVING"); 
                dma_display->flipDMABuffer();
                
                saveGame();
                delay(1000); 

                dma_display->fillScreen(0x0000);
                dma_display->setCursor(20, 28);
                dma_display->print("BYE!");
                dma_display->flipDMABuffer();
                delay(500);
                
                dma_display->setBrightness8(BRIGHTNESS_LEVELS[currentBrightnessSetting]);

                isTamagotchiActive = false;
                isClockActive = true; justExitedClock = true; 
                lastActivityTime = 0; lastClockRenderTime = 0;
                
                while(digitalRead(BTN_DRIVE_A_PIN) == LOW) { delay(10); }
                lastPressTime = millis(); 
                return;
            }
        }

        // --------------------------------------
        // STATE: FEED MENU
        // --------------------------------------
        else if (currentGameState == STATE_FEED_MENU) {
            if (bartNPressed) { 
                subMenuSelection = !subMenuSelection; 
                lastPressTime = millis(); 
            } 
            else if (bartSPressed) { 
                time_t now; time(&now);
                int ageYears = (now - myPet.birthTime) / SECONDS_IN_YEAR;
                
                // 0 = Snack (25 Hunger), 1 = Meal (50 Hunger)
                int foodValue = (subMenuSelection == 0) ? 25 : 50;
                int potentialWeightGain = (subMenuSelection == 0) ? 1 : 2;
                
                // --- RESTORED LOGIC ---
                if (subMenuSelection == 0) {
                    // SNACK: High Happiness (+5)
                    myPet.happiness = min(100, myPet.happiness + 5); 
                } else {
                    // MEAL: Low Happiness (+15)
                    myPet.happiness = min(100, myPet.happiness + 15);
                }
                // ----------------------

                // Weight Gain Logic
                bool gainsWeight = false;
                if (ageYears < 18) gainsWeight = true; // Always gain weight if young
                else if (myPet.hunger + foodValue > 100) gainsWeight = true; // Only if overeating if adult

                if (gainsWeight) myPet.weight += potentialWeightGain;

                // Apply Hunger Fill
                myPet.hunger = min(100, myPet.hunger + foodValue);
                myPet.lastHungerTime = now; 
                myPet.zeroHungerTime = 0;    

                // Feedback
                dma_display->fillScreen(0x0000); 
                dma_display->setCursor(20, 20); 
                dma_display->setTextColor(0xFFFF);
                dma_display->print("YUM!");
                dma_display->flipDMABuffer(); 
                delay(1000);

                spawnPetRandomly();
                currentGameState = STATE_MAIN; 
                
                while(digitalRead(BTN_BART_S_PIN) == LOW) { delay(10); }
                lastPressTime = millis();
                return; // STOP
            }
            // BACK BUTTON (Cancel Feeding)
            else if (driveAPressed) {
                currentGameState = STATE_MAIN;
                screenDirty = true;
                while(digitalRead(BTN_DRIVE_A_PIN) == LOW) { delay(10); }
                lastPressTime = millis();
                return;
            }
        }

        // --------------------------------------
        // STATE: MEDICINE MENU
        // --------------------------------------
        else if (currentGameState == STATE_MEDICINE_MENU) {
            // CONFIRM (GIVE MEDS) - BART_S
            if (bartSPressed) {
                time_t now; time(&now);
                long sickDuration = now - myPet.lastSicknessTime;

                myPet.isSick = false;
                myPet.happiness = min(100, myPet.happiness + 10);
                // Set Immunity Timer
                myPet.lastCuredTime = now;

                // Bonus: +5 vitality for curing within 3 seconds of getting sick
                if (myPet.lastSicknessTime != 0 && sickDuration <= 3) {
                    myPet.vitality = min(100.0f, myPet.vitality + 5.0f);
                    Serial.println("[VITALITY] +5 bonus: Cured within 3 seconds of sickness!");
                }

                dma_display->fillScreen(0x0000);
                dma_display->setCursor(16, 28);
                dma_display->setTextColor(0x07E0); // Green
                dma_display->print("CURED!");
                dma_display->flipDMABuffer();
                delay(1000);
                
                currentGameState = STATE_MAIN;
                while(digitalRead(BTN_BART_S_PIN) == LOW) { delay(10); }
                lastPressTime = millis();
                return; // STOP
            }
            // CANCEL (EXIT) - DRIVE_A
            else if (driveAPressed) {
                currentGameState = STATE_MAIN;
                while(digitalRead(BTN_DRIVE_A_PIN) == LOW) { delay(10); }
                lastPressTime = millis();
                return; // STOP
            }
        }

        // --------------------------------------
        // STATE: BATHROOM MENU
        // --------------------------------------
        else if (currentGameState == STATE_BATHROOM_MENU) {
            // CONFIRM (CLEAN) - BART_S
            if (bartSPressed) {
                myPet.isDirty = false;
                myPet.happiness = min(100, myPet.happiness + 10); 
                // Set Immunity Timer
                time_t now; time(&now);
                myPet.lastCleanedTime = now;
                
                // Visual Feedback
                dma_display->fillScreen(0x0000);
                dma_display->setCursor(16, 28);
                dma_display->setTextColor(0x07E0); // Green
                dma_display->print("SPARKLE!"); 
                
                // Draw some water drops
                for(int i=0; i<20; i++) {
                    dma_display->drawPixel(random(10,54), random(10,50), 0x001F); // Blue
                }
                
                dma_display->flipDMABuffer();
                delay(1000);
                
                currentGameState = STATE_MAIN;
                while(digitalRead(BTN_BART_S_PIN) == LOW) { delay(10); }
                lastPressTime = millis();
                return; // STOP
            }
            // CANCEL (EXIT) - DRIVE_A
            else if (driveAPressed) {
                currentGameState = STATE_MAIN;
                while(digitalRead(BTN_DRIVE_A_PIN) == LOW) { delay(10); }
                lastPressTime = millis();
                return; // STOP
            }
        }

        // --------------------------------------
        // STATE: STATS METER
        // --------------------------------------
        else if (currentGameState == STATE_STATS) {
            
            // SCROLL PAGES (BART_N)
            if (bartNPressed) {
                statsPage++;
                if (statsPage > 3) statsPage = 0; 
                
                lastPressTime = millis();
                while(digitalRead(BTN_BART_N_PIN) == LOW) { delay(10); }
            }

            // BACK / EXIT (DRIVE_A)
            else if (driveAPressed) {
                currentGameState = STATE_MAIN;
                while(digitalRead(BTN_DRIVE_A_PIN) == LOW) { delay(10); }
                lastPressTime = millis();
                return; // STOP
            }
        }

        // --------------------------------------
        // STATE: DEATH RESTART
        // --------------------------------------
        else if (currentGameState == STATE_DEATH) {
            // Any button press resets the save
            if (bartSPressed || driveAPressed || bartNPressed) {
                
                // 1. Visual Feedback
                dma_display->fillScreen(0x0000);
                drawCenteredText("TRY AGAIN?", 28, 0xFFFF);
                dma_display->flipDMABuffer();
                
                // 2. OPEN SAVE & BACKUP COINS
                String slot = (saveMenuSelection == 0) ? "leah" : "juan";
                Serial.printf("[DEATH] Wiping save slot: %s\n", slot.c_str());
                
                preferences.begin(slot.c_str(), false); // RW Mode
                
                int inheritance = preferences.getInt("coins", 0); // Backup coins
                Serial.printf("[DEATH] Coins saved: %d\n", inheritance);

                // 3. NUCLEAR OPTION: Explicitly remove keys
                // preferences.clear(); // This seems unreliable on some cores
                
                preferences.remove("birth");   // FORCE DELETE birth time
                preferences.putUInt("birth", 0); // DOUBLE TAP: Write 0 explicitly
                
                preferences.remove("isDead");  // FORCE DELETE dead status
                preferences.putBool("isDead", false); // DOUBLE TAP: Write false

                // 4. RESTORE COINS
                preferences.putInt("coins", inheritance);
                
                preferences.end(); // Commit
                Serial.println("[DEATH] Wipe Complete. Keys removed.");
                
                delay(1000); 
                
                // 5. Reset RAM status (Crucial!)
                myPet.isDead = false;
                myPet.birthTime = 0; // Reset this so it doesn't accidentally save old data later
                myPet.coins = inheritance; 
                
                // 6. Go back to menu
                currentGameState = STATE_SAVE_SELECT;
                
                // Reset timers
                lastPressTime = millis();
                while(digitalRead(BTN_BART_S_PIN) == LOW || digitalRead(BTN_DRIVE_A_PIN) == LOW) { delay(10); }
            }
        }

        // --------------------------------------
        // STATE: PLAY MENU
        // --------------------------------------
        else if (currentGameState == STATE_PLAY_MENU) {
            
            // SCROLL OPTIONS (BART_N)
            if (bartNPressed) {
                int menuCount = (myPet.alignment == 1) ? 3 : 2;
                playMenuSelection = (playMenuSelection + 1) % menuCount;
                lastPressTime = millis();
                while(digitalRead(BTN_BART_N_PIN) == LOW) { delay(10); }
            }

            // SELECT GAME (BART_S)
            else if (bartSPressed) {
                if (playMenuSelection == 0) {
                    currentGameState = STATE_GAME_PLAY;
                    gameCursorX = 0;
                } else if (playMenuSelection == 1) {
                    initConnect3();
                    currentGameState = STATE_CONNECT_3;
                } else if (playMenuSelection == 2 && myPet.alignment == 1) {
                    initFlappyBird();
                    currentGameState = STATE_FLAPPY_BIRD;
                }
                
                while(digitalRead(BTN_BART_S_PIN) == LOW) { delay(10); }
                lastPressTime = millis();
                return; // STOP
            }
            
            // BACK (DRIVE_A)
            else if (driveAPressed) {
                currentGameState = STATE_MAIN;
                while(digitalRead(BTN_DRIVE_A_PIN) == LOW) { delay(10); }
                lastPressTime = millis();
                return; // STOP
            }
        }

        // --------------------------------------
        // STATE: CATCH GAME
        // --------------------------------------
        else if (currentGameState == STATE_GAME_PLAY) {
            // BACK BUTTON (Exit without playing)
            if (driveAPressed) {
                currentGameState = STATE_PLAY_MENU; 
                while(digitalRead(BTN_DRIVE_A_PIN) == LOW) { delay(10); }
                lastPressTime = millis();
                return; // STOP
            }
            // SELECT BUTTON (Attempt to Catch)
            else if (bartSPressed) {
                bool isHit = (gameCursorX >= 22 && gameCursorX <= 38);

                dma_display->fillScreen(0x0000);
                dma_display->setCursor(20, 28);
                
                if (isHit) {
                    dma_display->setTextColor(0x07E0); // Green
                    dma_display->print("NICE!");
                    myPet.happiness = min(100, myPet.happiness + 15);
                    myPet.energy = max(0, myPet.energy - 10);
                } else {
                    dma_display->setTextColor(0xF800); // Red
                    dma_display->print("MISS");
                    myPet.happiness = min(100, myPet.happiness + 5); 
                    myPet.energy = max(0, myPet.energy - 25);
                }
                
                dma_display->flipDMABuffer();
                delay(1000);
                currentGameState = STATE_MAIN; // Return to main screen
                
                while(digitalRead(BTN_BART_S_PIN) == LOW) { delay(10); }
                lastPressTime = millis();
                return; // STOP
            }
        }

        // --------------------------------------
        // STATE: CONNECT 3
        // --------------------------------------
        else if (currentGameState == STATE_CONNECT_3) {
            
            // 1. MOVE CURSOR (Left/Right - BART N)
            // Only allow movement if the game is actually running
            if (gameActive) {
                if (bartNPressed) {
                    connect3CursorX++;
                    if (connect3CursorX > 4) connect3CursorX = 0;
                    screenDirty = true;
                    while(digitalRead(BTN_BART_N_PIN) == LOW) { delay(10); }
                    lastPressTime = millis();
                }
            }

            // 2. EXECUTE / EXIT BUTTON (BART S)
            if (bartSPressed) {
                
                // [SCENARIO A] GAME IS OVER (Win or Lose)
                // If we are here, the game ended in a PREVIOUS button press or CPU move.
                // This press confirms we saw the result and want to go home.
                if (!gameActive) {
                    currentGameState = STATE_MAIN;
                    spawnPetRandomly(); 
                    screenDirty = true;
                    
                    // Buffer: Wait for release
                    while(digitalRead(BTN_BART_S_PIN) == LOW) { delay(10); }
                    lastPressTime = millis();
                    return; 
                }
                
                // [SCENARIO B] GAME IS RUNNING
                // We are dropping a piece.
                int r = dropPiece(1, connect3CursorX); 
                if (r != -1) { 
                    if (checkWin(1, connect3CursorX, r)) {
                        // PLAYER WINS
                        gameActive = false;
                        connect3LossStreak = 0; // Reset loss streak

                        // Reward
                        myPet.happiness = min(100, myPet.happiness + 25);
                        myPet.energy = max(0, myPet.energy - 10);
                        
                        // IMPORTANT: We do NOT exit here.
                        // We just flagged the game as "Over".
                        // The user sees the win on screen.
                        // They must let go and press AGAIN to trigger [Scenario A].
                        
                    } else {
                        // Game continues -> CPU Turn
                        playerTurn = 2; 
                        lastCpuMoveTime = millis(); 
                    }
                    screenDirty = true; 
                }
                
                // Buffer: Wait for release so we don't double-trigger
                while(digitalRead(BTN_BART_S_PIN) == LOW) { delay(10); }
                lastPressTime = millis();
            }

            // 3. BACK BUTTON (Drive A) -> Returns to Game Menu
            // (Only if you want to quit mid-game or go back to menu instead of garden)
            else if (driveAPressed) {
                currentGameState = STATE_PLAY_MENU;
                screenDirty = true;
                while(digitalRead(BTN_DRIVE_A_PIN) == LOW) { delay(10); }
                lastPressTime = millis();
                return;
            }
        }

        // --------------------------------------
        // STATE: FLAPPY BIRD
        // --------------------------------------
        else if (currentGameState == STATE_FLAPPY_BIRD) {

            // FLAP (BART_S)
            if (bartSPressed) {
                if (flappyDead) {
                    // Game over: return to main
                    currentGameState = STATE_MAIN;
                    spawnPetRandomly();
                    screenDirty = true;
                    while(digitalRead(BTN_BART_S_PIN) == LOW) { delay(10); }
                    lastPressTime = millis();
                    return;
                } else if (!flappyActive) {
                    // First tap: start the game
                    flappyActive = true;
                    flappyVelY = FLAPPY_FLAP_VEL;
                    // +5 happiness just for playing
                    if (!flappyGavePlayBonus) {
                        myPet.happiness = min(100, myPet.happiness + 5);
                        flappyGavePlayBonus = true;
                    }
                } else {
                    // Flap!
                    flappyVelY = FLAPPY_FLAP_VEL;
                }
                while(digitalRead(BTN_BART_S_PIN) == LOW) { delay(10); }
                lastPressTime = millis();
            }

            // BACK (DRIVE_A)
            else if (driveAPressed) {
                currentGameState = STATE_PLAY_MENU;
                screenDirty = true;
                while(digitalRead(BTN_DRIVE_A_PIN) == LOW) { delay(10); }
                lastPressTime = millis();
                return;
            }
        }

        lastActivityTime = millis();
        return;
    }

    // --- 4. STANDARD SYSTEM LOGIC (TRANSIT) ---
    if (millis() - lastPressTime < 500) return; 

    bool anyButtonPressed = muniNPressed || muniSPressed || bartNPressed || bartSPressed || driveAPressed || driveBPressed;

    if (anyButtonPressed) {
        lastPressTime = millis(); 
        lastActivityTime = millis();
        isClockActive = false;
    } else {
        return; 
    }
    
    // [CANCEL LOGIC 1: Brightness]
    if (muniNPressed && muniSPressed) {
        if (isFetchingMUNI || isFetchingBART) { isFetchingMUNI = false; isFetchingBART = false; activeFetchID++; }
        currentBrightnessSetting = (currentBrightnessSetting + 1) % NUM_BRIGHTNESS_LEVELS;
        dma_display->setBrightness8(BRIGHTNESS_LEVELS[currentBrightnessSetting]);
        renderDisplay("LEVEL", BRIGHTNESS_NAMES[currentBrightnessSetting], "#FFFFFF");
        delay(1000); isClockActive = true; justExitedClock = true; lastActivityTime = 0; lastClockRenderTime = 0; 
        return;
    }

    // [CANCEL LOGIC 2: Idle Mode]
    if (bartNPressed && bartSPressed) {
        if (isFetchingMUNI || isFetchingBART) { isFetchingMUNI = false; isFetchingBART = false; activeFetchID++; }
        if (currentIdleMode == IDLE_CLOCK) { currentIdleMode = IDLE_WEATHER; renderDisplay("IDLE MODE", "WEATHER", "#00FFFF"); } 
        else { currentIdleMode = IDLE_CLOCK; renderDisplay("IDLE MODE", "CLOCK", "#00FFFF"); }
        delay(1000); isClockActive = true; justExitedClock = true; lastActivityTime = 0; lastClockRenderTime = 0; 
        return;
    }

    // [CANCEL LOGIC 3: Drive Cycle]
    if (driveAPressed && driveBPressed) {
        if (isFetchingDRIVE || isFetchingMETRO || isFetchingBIKE || isFetchingWALK) {
             isFetchingDRIVE = false; isFetchingMETRO = false; isFetchingBIKE = false; isFetchingWALK = false; activeFetchID++;
        }
        executeDriveModeCycle();
        delay(1000); isClockActive = true; justExitedClock = true; lastActivityTime = 0; lastClockRenderTime = 0; 
        return;
    }

    if (isFetching()) return; 
    if (!isTimeSynced() || WiFi.status() != WL_CONNECTED) return;

    ActiveMode mode = currentMode;
    bool needsFetch = false;
    unsigned long now = millis() / 1000; 
    #define IS_CACHE_EXPIRED(last_time, ttl) (now - last_time >= ttl || last_time == 0)

    if (bartNPressed || bartSPressed) {
        unsigned long comboWaitStartTime = millis();
        bool isCombo = false;
        while (millis() - comboWaitStartTime < 200) {
            bool otherPressed = bartNPressed ? (digitalRead(BTN_BART_S_PIN) == LOW) : (digitalRead(BTN_BART_N_PIN) == LOW);
            if (otherPressed) { isCombo = true; break; } 
            delay(10);
        }
        if (isCombo) return;

        mode = bartNPressed ? BART_N : BART_S;
        if (IS_CACHE_EXPIRED(lastBartFetchTime, TRANSIT_CACHE_TTL)) {
            taskENTER_CRITICAL(&fetchFlagsMux); fetchFlags |= FLAG_BART; taskEXIT_CRITICAL(&fetchFlagsMux);  
            needsFetch = true; isFetchingBART = true; bartNorthIndex = 0; bartSouthIndex = 0;
        } else {
            if (justExitedClock) { 
                if (bartNPressed && bartNorthIndex > 0) bartNorthIndex--;
                if (bartSPressed && bartSouthIndex > 0) bartSouthIndex--;
                justExitedClock = false;
            }
            executeFetchAndDisplay(mode);
        }
    }
    else if (muniNPressed) {
        mode = MUNI_N;
        if (IS_CACHE_EXPIRED(lastMuniNorthFetchTime, TRANSIT_CACHE_TTL)) {
            taskENTER_CRITICAL(&fetchFlagsMux); fetchFlags |= FLAG_MUNI_N; taskEXIT_CRITICAL(&fetchFlagsMux);  
            needsFetch = true; isFetchingMUNI = true; muniNorthIndex = 0;
        } else {
            if (justExitedClock) { if (muniNorthIndex > 0) muniNorthIndex--; justExitedClock = false; }
            executeFetchAndDisplay(mode);
        }
    }
    else if (muniSPressed) {
        mode = MUNI_S;
        if (IS_CACHE_EXPIRED(lastMuniSouthFetchTime, TRANSIT_CACHE_TTL)) {
            taskENTER_CRITICAL(&fetchFlagsMux); fetchFlags |= FLAG_MUNI_S; taskEXIT_CRITICAL(&fetchFlagsMux);  
            needsFetch = true; isFetchingMUNI = true; muniSouthIndex = 0;
        } else {
            if (justExitedClock) { if (muniSouthIndex > 0) muniSouthIndex--; justExitedClock = false; }
            executeFetchAndDisplay(mode);
        }
    }
    else if (driveAPressed) {
        mode = DRIVE_A;
        if (IS_CACHE_EXPIRED(lastDriveAFetchTime[currentDriveMode], DRIVE_CACHE_TTL)) {
            taskENTER_CRITICAL(&fetchFlagsMux); fetchFlags |= FLAG_DRIVE_A; taskEXIT_CRITICAL(&fetchFlagsMux);  
            needsFetch = true; driveAIndex = 0;
             if (currentDriveMode == MODE_TRANSIT) isFetchingMETRO = true;
             else if (currentDriveMode == MODE_BICYCLING) isFetchingBIKE = true;
             else if (currentDriveMode == MODE_WALKING) isFetchingWALK = true;
             else isFetchingDRIVE = true;
        } else {
            if (justExitedClock) { if (driveAIndex > 0) driveAIndex--; justExitedClock = false; }
            executeFetchAndDisplay(mode);
        }
    }
    else if (driveBPressed) {
        mode = DRIVE_B;
        if (IS_CACHE_EXPIRED(lastDriveBFetchTime[currentDriveMode], DRIVE_CACHE_TTL)) {
            taskENTER_CRITICAL(&fetchFlagsMux); fetchFlags |= FLAG_DRIVE_B; taskEXIT_CRITICAL(&fetchFlagsMux);  
            needsFetch = true; driveBIndex = 0;
             if (currentDriveMode == MODE_TRANSIT) isFetchingMETRO = true;
             else if (currentDriveMode == MODE_BICYCLING) isFetchingBIKE = true;
             else if (currentDriveMode == MODE_WALKING) isFetchingWALK = true;
             else isFetchingDRIVE = true;
        } else {
            if (justExitedClock) { if (driveBIndex > 0) driveBIndex--; justExitedClock = false; }
            executeFetchAndDisplay(mode);
        }
    }

    if (needsFetch) {
        activeFetchID++; 
        modeToDisplayAfterFetch = mode;
        animationFrame = 0; bartAnimationFrame = 0; driveAnimationFrame = 0;
        xTaskNotifyGive(apiTaskHandle);
    }
}


void loop() {
    // Chao gets "Good Sleep" credit even if you are looking at train times
    checkAutoSleep();

    // --- WIFI RECONNECTION CHECK ---
        static unsigned long lastWifiCheck = 0;
        if (millis() - lastWifiCheck > 30000) { // Check every 30 seconds
          lastWifiCheck = millis();
            if (WiFi.status() != WL_CONNECTED) {
                Serial.println("[WiFi] Connection lost... Reconnecting...");
                WiFi.disconnect();
                WiFi.reconnect();
            }
        }

    if (isTamagotchiActive) {
        updateTamagotchi();
        handleButtonPress(); // Allow inputs
        delay(10);
        esp_task_wdt_reset();
        return; // Skip the rest of the loop (Clock, Weather, API checks)
    }
    // --- Global Weather Fetch Check ---
    // Ensure sun/moon data even if we never look at the weather screen 
    #define IS_CACHE_EXPIRED(last_time, ttl) ((millis() / 1000) - last_time >= ttl || last_time == 0)
    if (IS_CACHE_EXPIRED(lastWeatherFetchTime, WEATHER_CACHE_TTL)) {
        fetchWeather(); 
        fetchAstronomyData();
        lastWeatherFetchTime = millis() / 1000;
    }
    
    // Check if the API task has new data
    if (dataIsReadyToDisplay) {
        dataIsReadyToDisplay = false; // Consume the flag immediately

        // --- Check if the data's tag matches the active tag ---
        if (dataFetchID != activeFetchID) {
            // Stale data. Ignore it.
            Serial.printf("[Loop] Ignoring stale data. (Data ID: %u, Active ID: %u)\n", dataFetchID, activeFetchID);
            // We do NOT stop the animation, as it's for the new fetch.
        } else {
            // This is valid data. Display it.
            Serial.printf("[Loop] Valid data received. (ID: %u)\n", dataFetchID);
            lastActivityTime = millis();  // Reset inactivity timer
            executeFetchAndDisplay(modeToDisplayAfterFetch);
            
            
            // Stop all fetch animations
            isFetchingMUNI = false; 
            isFetchingBART = false;
            isFetchingDRIVE = false;
            isFetchingMETRO = false;
            isFetchingBIKE = false;
            isFetchingWALK = false;
            
            justExitedClock = false;      
        }
    }

    // Check for button presses
    handleButtonPress(); 

    // --- Animation logic ---
    if (isFetchingMUNI) {
        animateMuniLoading();
    } 
    else if (isFetchingBART) {
        animateBARTLoading();
    }
    else if (isFetchingDRIVE) {
        animateDRIVELoading();
    }
    else if (isFetchingMETRO) {
     animateMETROLoading();
    }
    else if (isFetchingBIKE) { 
     animateBIKELoading();
    }
    else if (isFetchingWALK) {
     animateWALKLoading();
    }

    // Check for inactivity or if clock mode is already active
    else if (isClockActive || (millis() - lastActivityTime > INACTIVITY_TIMEOUT)) {
        
        justExitedClock = true; // Set this flag when entering idle mode

        // --- [APPLE TIME LOGIC] ---
        if (!isTimeSynced()) {
            displayClock(); // Failsafe if time is not synced
        } else {
            time_t now;
            time(&now);
            struct tm * timeinfo;
            timeinfo = localtime(&now);

            // timeinfo->tm_hour == 16 is 4PM, timeinfo->tm_min == 0 is :00
            if (timeinfo->tm_hour == 16 && timeinfo->tm_min == 0) {
                // It's 4:00 PM!
                displayAppleTime(); // This function will set isClockActive = true
            } else {
                // --- It's any other time ---
                
                // Reset apple time frame so it starts fresh next time
                lastAppleFrameTime = 0; 
                appleTimeFrame = 0;

                // Now, show the correct idle screen
                if (currentIdleMode == IDLE_WEATHER) {
                    displayWeatherForecast();
                } else {
                    displayClock(); 
                }
            }
        }
        
    } else {
        // In an active mode (showing transit data)
        isClockActive = false;
        // Reset clock render timer so it updates immediately when it next becomes active
        lastClockRenderTime = 0;
    }
    
    delay(10); // Reduced delay
    esp_task_wdt_reset();
}


void drawIdleAnimation(struct tm* timeinfo) {
    
    // --- 1. Get current time_t ---
    time_t now;
    time(&now);

    // --- 2. Get Day/Month info ---
    int dayOfWeek = timeinfo->tm_wday; // 0=Sun, 1=Mon, ... 4=Thu
    int dayOfMonth = timeinfo->tm_mday;
    int month = timeinfo->tm_mon; 

    // Default Defaults
    const uint16_t* const* currentAnimFrames = shrimpflamingo_frames; 
    int currentAnimFrameCount = 2;
    long currentAnimSpeed = 2000;
    
    // State Tracking
    static int lastAnimFrameCount = 0;  
    static String lastPhaseName = "";
    String currentPhaseName = "";

    // --- METROID SPECIAL LOGIC VARIABLES ---
    static unsigned long lastMetroidActionTime = 0; // Tracks the 60s interval
    static bool isMetroidActing = false;           // Are we in the fast mode?

    // --- 3. LOGIC START: CHECK DAYTIME VS. NIGHTTIME ---
    time_t endTwilight = todaySunset + TWILIGHT_OFFSET_SECONDS;

    // Day Start check (rounded to midnight)
    time_t now_day_start = now - (now % 86400); 

    if (todaySunrise > 0 && now > todaySunrise && now < endTwilight) { 
        // --- [IT IS DAYTIME] ---
        currentPhaseName = "DAYTIME_SUN";

        // --- SPECIFIC DATES (Priority 1) ---
        // Day of Week/Holiday logic sets currentAnimFrames, currentAnimFrameCount, etc.
        if (month == 1 && dayOfMonth == 14) { 
            // Valentine's Day
            currentAnimFrames = vday_frames; currentAnimFrameCount = 9; currentAnimSpeed = 1000;
        } 
        else if (month == 11 && dayOfMonth == 25) { 
            // Christmas Day
            currentAnimFrames = christmas_frames; currentAnimFrameCount = 5;
        } 
        else if (month == 3 && dayOfMonth == 1) { 
            // April Fools
            currentAnimFrames = april_frames; currentAnimFrameCount = 6;
        } 
        else if (dayOfMonth == 13) { 
            // Scooby Doo
            currentAnimFrames = scoobydoo_frames; currentAnimFrameCount = 9;
        } 
        else if (dayOfMonth == 29) { 
            // Jaws
            currentAnimFrames = jaws_frames; currentAnimFrameCount = 2;
        } 
        
        // --- DAYS OF THE WEEK (Priority 2) ---
        else if (dayOfWeek == 5) { 
            // Friday Baboon (Weeks 1 & 3)
            if ((dayOfMonth >= 1 && dayOfMonth <= 7) || (dayOfMonth >= 15 && dayOfMonth <= 21)) {
                currentAnimFrames = baboon_frames; currentAnimFrameCount = 3;
            }
        } 
        else if (dayOfWeek == 1) { 
            // Monday Goose (Weeks 2 & 4)
            if ((dayOfMonth >= 8 && dayOfMonth <= 14) || (dayOfMonth >= 22 && dayOfMonth <= 28)) { 
                currentAnimFrames = goose_frames; currentAnimFrameCount = 5;
            }
        } 
        else if (dayOfWeek == 0) { 
            // Sunday Prince (End of Month)
            if ((dayOfMonth >= 29 && dayOfMonth <= 31)) {
                currentAnimFrames = prince_frames; currentAnimFrameCount = 2;
            }
        } 
        else if (dayOfWeek == 3) { 
            // Wednesday Apples
            currentAnimFrames = apples_frames; currentAnimFrameCount = 5;
        }
        
        // --- METROID SPECIAL (4th Thursday) ---
        else if (dayOfWeek == 4) { 
            // Only runs if it is the 4th Thursday (Dates 22-28)
            if (dayOfMonth >= 22 && dayOfMonth <= 28) {
                currentAnimFrames = metroid_frames;
                
                // 1. Check if it's time to trigger the fast action (Once every 3 minutes)
                if (!isMetroidActing && (millis() - lastMetroidActionTime > 180000)) {
                    isMetroidActing = true;
                    clockAnimationFrame = 2; // Jump straight to action
                    lastClockAnimFrameTime = millis(); // Force update NOW
                }

                // 2. Configure Speed based on Action State
                if (isMetroidActing) {
                    // --- FAST MODE ---
                    currentAnimFrameCount = 11; // Play all 11 frames
                    currentAnimSpeed = 100;      // Very fast (100ms)
                    
                    // Check if the loop finished (it wrapped back to 0 or 1)
                    if (clockAnimationFrame < 2) {
                        isMetroidActing = false; // Stop acting
                        lastMetroidActionTime = millis(); // Reset the timer
                    }
                } else {
                    // --- IDLE MODE ---
                    currentAnimFrameCount = 2;  // Only cycle frames 0 and 1
                    currentAnimSpeed = 2000;     // Slow "breathing" speed
                }
            }
            // If date is not 22-28, it falls through to Default (Shrimp)
        }
      
    // --- ELSE: IT IS NIGHTTIME (or data is missing) ---
    } else {
        currentAnimSpeed = 2500;
        
        // --- PHASE 1: ASTRONOMICAL PHASE MATCH ---
        // Default to a safe intermediate phase if the name isn't found
        currentAnimFrames = shrimpflamingo_frames; 
        currentAnimFrameCount = 2;
        currentPhaseName = currentMoonPhaseName; 
        
        // Ensure we have a phase name to compare against
        if (currentMoonPhaseName.isEmpty() || currentMoonPhaseName == "UNKNOWN") {
            currentPhaseName = "Unknown/Error";
        }
        
        // Use case-insensitive comparisons
        else if (currentMoonPhaseName.equalsIgnoreCase("New Moon")) {
            currentAnimFrames = newmoon_frames; currentAnimFrameCount = NEWMOON_FRAMES;
        } 
        else if (currentMoonPhaseName.equalsIgnoreCase("Waxing Crescent")) {
            currentAnimFrames = waxcres_frames; currentAnimFrameCount = WAXCRES_FRAMES;
        }
        else if (currentMoonPhaseName.equalsIgnoreCase("First Quarter")) {
            currentAnimFrames = firstquart_frames; currentAnimFrameCount = FIRSTQUART_FRAMES;
        }
        else if (currentMoonPhaseName.equalsIgnoreCase("Waxing Gibbous")) {
            currentAnimFrames = waxgib_frames; currentAnimFrameCount = WAXGIB_FRAMES;
        }
        else if (currentMoonPhaseName.equalsIgnoreCase("Full Moon")) {
            currentAnimFrames = fullmoon_frames; currentAnimFrameCount = FULLMOON_FRAMES;
        }
        else if (currentMoonPhaseName.equalsIgnoreCase("Waning Gibbous")) {
            currentAnimFrames = wangib_frames; currentAnimFrameCount = WANGIB_FRAMES;
        }
        else if (currentMoonPhaseName.equalsIgnoreCase("Last Quarter")) {
            currentAnimFrames = lastquart_frames; currentAnimFrameCount = LASTQUART_FRAMES;
        }
        else if (currentMoonPhaseName.equalsIgnoreCase("Waning Crescent")) {
            currentAnimFrames = wancres_frames; currentAnimFrameCount = WANCRES_FRAMES;
        }
    }
    // --- Day/Night Animation Logic End ---
    
    // --- RESET LOGIC ---
    // Track pointer change to prevent crashes when switching animations
    static const uint16_t* const* lastAnimFramesPtr = nullptr;

    if (currentAnimFrames != lastAnimFramesPtr) {
        clockAnimationFrame = 0;
        lastClockAnimFrameTime = 0; 
    }
    lastAnimFramesPtr = currentAnimFrames;
    
    lastAnimFrameCount = currentAnimFrameCount; 

    // --- UPDATE ANIMATION FRAME ---
    if (millis() - lastClockAnimFrameTime >= currentAnimSpeed) {
        lastClockAnimFrameTime = millis();
        clockAnimationFrame++;
        
        // Wrap around logic
        if (clockAnimationFrame >= currentAnimFrameCount) {
            clockAnimationFrame = 0; 
        }
    }
    
    // Safety check to prevent crashing if logic gets out of sync
    if (clockAnimationFrame >= currentAnimFrameCount) clockAnimationFrame = 0;
    
    int16_t icon_x = (PANEL_RES_X - 64) / 2;
    int16_t icon_y = (PANEL_RES_Y - 32);
    
    // Get pointer to the correct frame
    const uint16_t* bitmap_ptr = currentAnimFrames[clockAnimationFrame];
    
    // Use the Night Dimming Helper
    drawDayNightBitmap(icon_x, icon_y, bitmap_ptr, 64, 32);
}


// Helper to check if we should show Night colors
bool isMoonTime() {
    time_t now;
    time(&now);
    
    // If we don't have weather data yet, assume Day to be safe
    if (todaySunrise == 0 || todaySunset == 0) return false;

    time_t endTwilight = todaySunset + TWILIGHT_OFFSET_SECONDS;
    
    // If it is currently strictly "Daytime", return false
    if (now > todaySunrise && now < endTwilight) {
        return false;
    }
    
    // Otherwise, it is Moon Time (mOoOoN tImE)
    return true;
}


// --- Clock Display Function ---
void displayClock() {
    // Check update rate
    if (millis() - lastClockRenderTime < 50) { 
        if (!isClockActive) isClockActive = true;
        return;
    }

    // Clear screen once if entering from another mode
    if (lastClockRenderTime == 0) {
        dma_display->fillScreen(0x0000); 
    }
    lastClockRenderTime = millis();
    
    if (!isTimeSynced()) {
        isClockActive = false; 
        renderDisplay("TIME", "NOT SYNCED", "#FF0000");
        return;
    }

    // --- COLOR LOGIC ---
    bool night = isMoonTime();
    
    // Day: Cyan (0, 255, 255) | Night: Darker Blue (0, 80, 180)
    uint16_t timeColor = night ? dma_display->color565(0, 80, 180) : dma_display->color565(0, 255, 255);
    
    // Day: White (255, 255, 255) | Night: Dark Grey (80, 80, 80)
    uint16_t dateColor = night ? dma_display->color565(150, 150, 150) : dma_display->color565(255, 255, 255);


    // 1. Get Time & Date
    char timeBuffer[10]; 
    char dateBuffer[20]; 
    time_t now;
    time(&now);
    struct tm * timeinfo;
    timeinfo = localtime(&now);
    strftime(timeBuffer, sizeof(timeBuffer), "%H:%M", timeinfo);
    strftime(dateBuffer, sizeof(dateBuffer), "%a %m/%d", timeinfo);
    
    String dateStr = String(dateBuffer);
    dateStr.toUpperCase(); 

    String hourStr = String(timeBuffer).substring(0, 2); 
    String colonStr = ":";
    String minStr = String(timeBuffer).substring(3, 5); 

    // 2. Draw Time
    dma_display->setTextSize(2); 
    int16_t x1, y1;
    uint16_t w_hr, h_hr, w_col, h_col, w_min, h_min;

    dma_display->getTextBounds(hourStr, 0, 0, &x1, &y1, &w_hr, &h_hr);
    dma_display->getTextBounds(colonStr, 0, 0, &x1, &y1, &w_col, &h_col);
    dma_display->getTextBounds(minStr, 0, 0, &x1, &y1, &w_min, &h_min);

    int16_t colonX = (PANEL_RES_X - w_col) / 2;
    int16_t hourX = colonX - w_hr + 2;  
    int16_t minX = colonX + w_col - 2;  

    // Use the dynamic timeColor
    dma_display->setTextColor(timeColor, 0x0000); 
    
    dma_display->setCursor(hourX, 5);
    dma_display->print(hourStr);
    
    dma_display->setCursor(colonX, 5);
    dma_display->print(colonStr);
    
    dma_display->setCursor(minX, 5);
    dma_display->print(minStr);

    // 3. Draw Date
    dma_display->setTextSize(1); 
    int16_t x1_dat, y1_dat;
    uint16_t w_dat, h_dat;
    dma_display->getTextBounds(dateStr, 0, 0, &x1_dat, &y1_dat, &w_dat, &h_dat);
    int16_t xPos_dat = (PANEL_RES_X - w_dat) / 2;

    dma_display->setCursor(std::max(static_cast<int16_t>(2), xPos_dat), 24); 
    
    // Use the dynamic dateColor
    dma_display->setTextColor(dateColor, 0x0000); 
    dma_display->print(dateStr); 

    // 4. Animation
    drawIdleAnimation(timeinfo);

    isClockActive = true; 
    dma_display->flipDMABuffer();
}

// *************************************************************************
// *** WEATHER FETCH FUNCTION ***
// *************************************************************************
void fetchWeather() {
    Serial.println("\n--- [WEATHER] Fetching OneCall (v3.0) data ---");

    // Clear the old forecast data
    weatherForecasts.clear();
    
    // Use the "onecall" endpoint. 
    char weatherUrl[256];
    snprintf(weatherUrl, sizeof(weatherUrl),
             "https://api.openweathermap.org/data/3.0/onecall?lat=%s&lon=%s&units=imperial&exclude=minutely,current,alerts&appid=%s",
             SF_HOME_LAT, SF_HOME_LON, WEATHER_API_KEY);
    
    String payload = makeHttpRequest(weatherUrl);
    
    if (payload.length() < 100) {
        Serial.println("[WEATHER] ERROR: No or short response.");
        return; // Vector will be empty
    }

    DynamicJsonDocument doc(8192); 
    DeserializationError error = deserializeJson(doc, payload);

    if (error) {
        Serial.printf("[WEATHER] JSON parsing failed: %s\n", error.c_str());
        return;
    }

    // 1. Parse Daily Data (Sun & Moon)
    JsonArray dailyList = doc["daily"].as<JsonArray>();
    if (dailyList.isNull() || dailyList.size() == 0) {
        Serial.println("[WEATHER] JSON Error: 'daily' array not found or empty.");
        return;
    }

    JsonObject todayData = dailyList[0];
    
    // Update global variables with timestamps
    todaySunrise = todayData["sunrise"].as<time_t>();
    todaySunset = todayData["sunset"].as<time_t>();

    Serial.printf("[WEATHER] Updated Sun Data -> Rise: %ld, Set: %ld\n", todaySunrise, todaySunset);

    // 2. Parse Hourly Forecast
    JsonArray hourlyList = doc["hourly"].as<JsonArray>();
    if (hourlyList.isNull()) {
        Serial.println("[WEATHER] JSON Error: 'hourly' array not found.");
        return;
    }

    // Cast hourlyList.size() to (int) to resolve conflicting types in min()
    int forecastsToGrab = min((int)hourlyList.size(), 24); 

    for (int i = 0; i < forecastsToGrab; i++) {
        JsonObject item = hourlyList[i];
        if (item.isNull()) continue;

        WeatherForecast forecast;
        
        forecast.timestamp = item["dt"].as<time_t>();
        forecast.temp = (int)round(item["temp"].as<double>());
        
        forecast.main = item["weather"][0]["main"].as<String>();
        forecast.description = item["weather"][0]["description"].as<String>();

        forecast.timeString = formatForecastTime(forecast.timestamp);
        weatherForecasts.push_back(forecast);
    }
    
    Serial.printf("[WEATHER] Success: Loaded %d hourly forecast blocks.\n", weatherForecasts.size());
}

// *************************************************************************
// *** WEATHER DISPLAY FUNCTION ***
// *************************************************************************
void displayWeatherForecast() {
    // 1. Frame Rate Check (Keep animation at ~20 FPS)
    if (millis() - lastClockRenderTime < 50) { 
        if (!isClockActive) isClockActive = true;
        return;
    }

    // 2. Double Buffer & Update Logic
    // Counter to ensure we draw the static elements on BOTH buffers 
    // (Front and Back) before stopping. 
    static int staticFramesToDraw = 0; 
    
    // Check if we just entered this mode (Screen Clear)
    if (lastClockRenderTime == 0) {
        dma_display->fillScreen(0x0000); 
        staticFramesToDraw = 2; // Draw for next 2 frames to fill both buffers
    }
    lastClockRenderTime = millis();

    // 3. Check for New Data
    // We check if the global fetch timer has changed since we last drew the data.
    static unsigned long lastDrawnDataTime = 0;

    if (lastWeatherFetchTime != lastDrawnDataTime) {
        // The main loop fetched new data! We need to update the display.
        staticFramesToDraw = 2; 
        lastDrawnDataTime = lastWeatherFetchTime; // Sync our tracker
    }

    // --- COLOR LOGIC ---
    bool night = isMoonTime();
    uint16_t mainTempColor = night ? dma_display->color565(160, 100, 0) : dma_display->color565(255, 165, 0);
    uint16_t listColor = night ? dma_display->color565(150, 150, 150) : dma_display->color565(255, 255, 255);

    // ============================================================
    // STATIC DRAWING BLOCK (Only runs when needed)
    // ============================================================
    if (staticFramesToDraw > 0) {
        
        if (weatherForecasts.empty()) {
            dma_display->setTextSize(1);
            int16_t x1, y1; uint16_t w, h;
            dma_display->getTextBounds("LOADING", 0, 0, &x1, &y1, &w, &h);
            dma_display->setCursor((PANEL_RES_X - w) / 2, 14); 
            dma_display->setTextColor(listColor, 0x0000);
            dma_display->print("LOADING");
        } 
        else {
            const WeatherForecast& f_now = weatherForecasts[0];
            
            // 1. Draw Temp (Big)
            dma_display->setTextSize(2);
            String tempStr = String(f_now.temp);
            
            int16_t x1, y1; uint16_t w_temp, h_temp;
            dma_display->getTextBounds(tempStr, 0, 0, &x1, &y1, &w_temp, &h_temp);
            int16_t xPos_temp = (42 - w_temp) / 2; 
            
            dma_display->setCursor(std::max(static_cast<int16_t>(0), xPos_temp), 5);
            dma_display->setTextColor(mainTempColor, 0x0000); 
            dma_display->print(tempStr);
            
            // Draw Symbol (Degree sign)
            int16_t degreeX = dma_display->getCursorX(); 
            dma_display->fillRect(degreeX + 1, 5, 3, 3, mainTempColor); 
            dma_display->drawPixel(degreeX + 2, 6, 0x0000);     
            
            // 2. Draw Icon
            int iconX = ((42 - 8) / 2) - 2; 
            int iconY = 22;

            // Clear the area behind the icon 
            dma_display->fillRect(iconX, iconY, 16, 10, 0x0000); 
            
            drawWeatherIcon(f_now.main, f_now.description, iconX, iconY); 

            // 3. Draw Future Forecasts
            dma_display->setTextSize(1);
            int startY = 2;
            int lineHeight = 10;
            int startX = 46; 
            int forecastIndexes[] = {3, 6, 12};
            int numForecastsToShow = 3;

            // Clear the side list area before drawing
            dma_display->fillRect(startX, 0, 18, 32, 0x0000);

            for (int i = 0; i < numForecastsToShow; i++) {
                int forecastIndex = forecastIndexes[i];
                if (forecastIndex < weatherForecasts.size()) {
                    const WeatherForecast& f_future = weatherForecasts[forecastIndex];
                    String fTempStr = String(f_future.temp);
                    dma_display->setCursor(startX, startY + (i * lineHeight));
                    dma_display->setTextColor(listColor, 0x0000);
                    dma_display->print(fTempStr);
                }
            }
        }
        
        // Decrement the counter. 
        // Once this hits 0, we stop redrawing the top half
        staticFramesToDraw--; 
    }

    // ============================================================
    // ANIMATION BLOCK (Runs every frame)
    // ============================================================
    time_t now_t; time(&now_t);
    drawIdleAnimation(localtime(&now_t));

    isClockActive = true; 
    dma_display->flipDMABuffer();
}


// --- Helper for Night Time Animation Dimming ---
void drawDayNightBitmap(int16_t x, int16_t y, const uint16_t *bitmap, int16_t w, int16_t h) {
    if (!isMoonTime()) {
        dma_display->drawRGBBitmap(x, y, bitmap, w, h);
    } else {
        // Check if the image fits in our static buffer
        if ((w * h) > 2048) {
            // Safety fallback: image too big for buffer, draw bright version
            dma_display->drawRGBBitmap(x, y, bitmap, w, h);
            return;
        }

        for (int i = 0; i < w * h; i++) {
            uint16_t color = bitmap[i];
            uint8_t r = (color >> 11) & 0x1F;
            uint8_t g = (color >> 5) & 0x3F;
            uint8_t b = color & 0x1F;
            
            // 25% darker
            r = (r * 3) >> 2;
            g = (g * 3) >> 2;
            b = (b * 3) >> 2;
            
            animationBuffer[i] = (r << 11) | (g << 5) | b;
        }
        // Draw from the static global buffer
        dma_display->drawRGBBitmap(x, y, animationBuffer, w, h);
    }
}


// --- MUNI Animation Function ---
void animateMuniLoading() {
    if (millis() - lastFrameTime < ANIMATION_SPEED_MS) return;
    lastFrameTime = millis();

    dma_display->fillScreen(dma_display->color565(0, 0, 0));

    int16_t x_pos = (PANEL_RES_X - MUNI_ANIM_WIDTH) / 2;
    int16_t y_pos = (PANEL_RES_Y - MUNI_ANIM_HEIGHT) / 2;
    const uint16_t* bitmap_ptr = muni_anim_frames[animationFrame];

    // Use the helper
    drawDayNightBitmap(x_pos, y_pos, bitmap_ptr, MUNI_ANIM_WIDTH, MUNI_ANIM_HEIGHT);

    dma_display->flipDMABuffer();

    animationFrame++;
    if (animationFrame >= 4) animationFrame = 0;
}

// --- BART Animation Function ---
void animateBARTLoading() {
    if (millis() - lastBARTFrameTime < ANIMATION_SPEED_MS) return;
    lastBARTFrameTime = millis();

    dma_display->fillScreen(dma_display->color565(0, 0, 0));

    int16_t x_pos = (PANEL_RES_X - BART_ANIM_WIDTH) / 2;
    int16_t y_pos = (PANEL_RES_Y - BART_ANIM_HEIGHT) / 2;
    const uint16_t* bitmap_ptr = bart_anim_frames[bartAnimationFrame];

    // Use the helper
    drawDayNightBitmap(x_pos, y_pos, bitmap_ptr, BART_ANIM_WIDTH, BART_ANIM_HEIGHT);

    dma_display->flipDMABuffer();

    bartAnimationFrame++;
    if (bartAnimationFrame >= 4) bartAnimationFrame = 0;
}

// --- DRIVE Animation Function ---
void animateDRIVELoading() {
    if (millis() - lastDRIVEFrameTime < ANIMATION_SPEED_MS) return;
    lastDRIVEFrameTime = millis();

    dma_display->fillScreen(dma_display->color565(0, 0, 0));

    int16_t x_pos = (PANEL_RES_X - DRIVE_ANIM_WIDTH) / 2;
    int16_t y_pos = (PANEL_RES_Y - DRIVE_ANIM_HEIGHT) / 2;
    const uint16_t* bitmap_ptr = drive_anim_frames[driveAnimationFrame];

    // Use the helper
    drawDayNightBitmap(x_pos, y_pos, bitmap_ptr, DRIVE_ANIM_WIDTH, DRIVE_ANIM_HEIGHT);

    dma_display->flipDMABuffer();

    driveAnimationFrame++;
    if (driveAnimationFrame >= 6) driveAnimationFrame = 0;
}

// --- METRO Animation Function ---
void animateMETROLoading() {
    if (millis() - lastMETROFrameTime < ANIMATION_SPEED_MS) return;
    lastMETROFrameTime = millis();

    dma_display->fillScreen(dma_display->color565(0, 0, 0));

    int16_t x_pos = (PANEL_RES_X - METRO_ANIM_WIDTH) / 2;
    int16_t y_pos = (PANEL_RES_Y - METRO_ANIM_HEIGHT) / 2;
    const uint16_t* bitmap_ptr = metro_anim_frames[metroAnimationFrame];

    // Use the helper
    drawDayNightBitmap(x_pos, y_pos, bitmap_ptr, METRO_ANIM_WIDTH, METRO_ANIM_HEIGHT);

    dma_display->flipDMABuffer();

    metroAnimationFrame++;
    if (metroAnimationFrame >= 6) metroAnimationFrame = 0;
}

// --- BIKE Animation Function ---
void animateBIKELoading() {
    if (millis() - lastBIKEFrameTime < ANIMATION_SPEED_MS) return;
    lastBIKEFrameTime = millis();

    dma_display->fillScreen(dma_display->color565(0, 0, 0));

    int16_t x_pos = (PANEL_RES_X - BIKE_ANIM_WIDTH) / 2;
    int16_t y_pos = (PANEL_RES_Y - BIKE_ANIM_HEIGHT) / 2;
    const uint16_t* bitmap_ptr = bike_anim_frames[bikeAnimationFrame];

    // Use the helper
    drawDayNightBitmap(x_pos, y_pos, bitmap_ptr, BIKE_ANIM_WIDTH, BIKE_ANIM_HEIGHT);

    dma_display->flipDMABuffer();

    bikeAnimationFrame++;
    if (bikeAnimationFrame >= 6) bikeAnimationFrame = 0;
}

// --- WALK Animation Function ---
void animateWALKLoading() {
    if (millis() - lastWALKFrameTime < ANIMATION_SPEED_MS) return;
    lastWALKFrameTime = millis();

    dma_display->fillScreen(dma_display->color565(0, 0, 0));

    int16_t x_pos = (PANEL_RES_X - WALK_ANIM_WIDTH) / 2;
    int16_t y_pos = (PANEL_RES_Y - WALK_ANIM_HEIGHT) / 2;
    const uint16_t* bitmap_ptr = walk_anim_frames[walkAnimationFrame];

    // Use the helper
    drawDayNightBitmap(x_pos, y_pos, bitmap_ptr, WALK_ANIM_WIDTH, WALK_ANIM_HEIGHT);

    dma_display->flipDMABuffer();

    walkAnimationFrame++;
    if (walkAnimationFrame >= 6) walkAnimationFrame = 0;
}


void displayAppleTime() {
    // Check if it's time to update the frame
    if (millis() - lastAppleFrameTime < ANIMATION_SPEED_MS) { 
        if (!isClockActive) isClockActive = true;
        return; // Not time yet
    }

    // --- [ARTIFACT] ---
    // If the last render time is 0, switched from
    // a data screen, so we must clear it once
    if (lastAppleFrameTime == 0) {
        dma_display->fillScreen(0x0000); 
    }
    lastAppleFrameTime = millis();
    
    // --- Center the animation ---
    int16_t x_pos = (PANEL_RES_X - 64) / 2;
    int16_t y_pos = (PANEL_RES_Y - 64) / 2;

    // Get the pointer to the current frame
    const uint16_t* bitmap_ptr = appletime_frames[appleTimeFrame];

    // Draw the current frame
    dma_display->drawRGBBitmap(
        x_pos, 
        y_pos, 
        bitmap_ptr, 
        64,     // Assuming 64px width
        64      // Assuming 64px height
    );

    // Send to display
    dma_display->flipDMABuffer();

    // Advance to the next frame
    appleTimeFrame++;
    if (appleTimeFrame >= 4) { // 4 is the number of frames
        appleTimeFrame = 0; // Loop back to the start
    }
    
    isClockActive = true; // We are in an "idle/clock" state

    // --- Reset the other clock timer ---
    // Main clock starts fresh when 4:01 PM hits.
    lastClockRenderTime = 0;
}


// *************************************************************************
// *** WEATHER ICON HELPER ***
// *************************************************S*********
// Draws a small 8x8 icon at the given x,y coordinate
void drawWeatherIcon(String main, String description, int x, int y) {
    uint16_t white = dma_display->color565(255, 255, 255);
    uint16_t yellow = dma_display->color565(255, 255, 0);
    uint16_t blue = dma_display->color565(0, 170, 255);
    uint16_t grey = dma_display->color565(180, 180, 180);
    uint16_t dark_grey = dma_display->color565(100, 100, 100);

    if (main == "Thunderstorm") {
        // --- Original rain cloud shape (dark) ---
        dma_display->fillCircle(x + 2, y + 2, 2, dark_grey); 
        dma_display->fillCircle(x + 5, y + 3, 3, dark_grey); 
        dma_display->fillCircle(x + 0, y + 3, 2, dark_grey); 
        // Bolt
        dma_display->drawLine(x + 4, y + 4, x + 2, y + 6, yellow);
        dma_display->drawLine(x + 2, y + 6, x + 5, y + 6, yellow);
        dma_display->drawLine(x + 5, y + 6, x + 3, y + 8, yellow);
    }
    else if (main == "Drizzle" || description == "light rain") {
        // --- Original rain cloud shape (light) ---
        dma_display->fillCircle(x + 2, y + 2, 2, grey);
        dma_display->fillCircle(x + 5, y + 3, 3, grey);
        dma_display->fillCircle(x + 0, y + 3, 2, grey);
        // Light drops
        dma_display->drawPixel(x + 2, y + 6, blue);
        dma_display->drawPixel(x + 5, y + 6, blue);
    }
    else if (main == "Rain") {
        // --- Original rain cloud shape (dark) ---
        dma_display->fillCircle(x + 2, y + 2, 2, dark_grey);
        dma_display->fillCircle(x + 5, y + 3, 3, dark_grey);
        dma_display->fillCircle(x + 0, y + 3, 2, dark_grey);
        // Heavy drops
        dma_display->drawLine(x + 1, y + 5, x, y + 7, blue);
        dma_display->drawLine(x + 4, y + 5, x + 3, y + 7, blue);
        dma_display->drawLine(x + 6, y + 5, x + 5, y + 7, blue);
    }
    else if (main == "Snow" || description == "freezing rain") {
        
        // Row 1: 3 empty, 6 white
        dma_display->fillRect(x+3, y, 6, 1, white);

        // Row 2: 1 empty, 9 white
        dma_display->fillRect(x+1, y+1, 9, 1, white);
        
        // Row 3: 11 white
        dma_display->fillRect(x, y+2, 11, 1, white);
        
        // Row 4, 11 white
        dma_display->fillRect(x, y+3, 11, 1, white);
        
        // Row 5, 1 dark grey, 3 white, 2 dark grey, 5 white, 1 dark grey
        dma_display->drawPixel(x, y+4, dark_grey);
        dma_display->fillRect(x+1, y+4, 3, 1, white);
        dma_display->fillRect(x+4, y+4, 2, 1, dark_grey);
        dma_display->fillRect(x+6, y+4, 5, 1, white);
        dma_display->drawPixel(x+11, y+4, dark_grey);
        
        // Row 6: 1 empty, 3 dark grey, 2 empty, 4 dark grey
        dma_display->fillRect(x+1, y+5, 3, 1, dark_grey);
        dma_display->fillRect(x+6, y+5, 4, 1, dark_grey);
        
        // Row 7: empty
        
        // Row 8: 1 white, 1 empty, 1 white, 2 empty, 1 white, 2 empty, 1 white
        dma_display->drawPixel(x, y+7, white);
        dma_display->drawPixel(x+2, y+7, white);
        dma_display->drawPixel(x+5, y+7, white);
        dma_display->drawPixel(x+8, y+7, white);
        
        // Row 9: 1 empty, 1 white, 2 empty, 1 white, 2 empty, 1 white
        dma_display->drawPixel(x+1, y+8, white);
        dma_display->drawPixel(x+4, y+8, white);
        dma_display->drawPixel(x+7, y+8, white);

    }
    else if (main == "Clouds") {
        
        if (description == "few clouds") {
            
            // Row 1: 2 empty, 3 yellow
            dma_display->fillRect(x+2, y, 3, 1, yellow);
            
            // Row 2: 1 empty, 5 yellow
            dma_display->fillRect(x+1, y+1, 5, 1, yellow);
            
            // Row 3: 7 yellow
            dma_display->fillRect(x, y+2, 7, 1, yellow);
            
            // Row 4: 7 yellow
            dma_display->fillRect(x, y+3, 7, 1, yellow);
            
            // Row 5: 5 yellow, 3 white
            dma_display->fillRect(x, y+4, 5, 1, yellow);
            dma_display->fillRect(x+5, y+4, 3, 1, white);
            
            // Row 6: (empty)

            // Row 7: 1 empty, 1 yellow, 1 white, 1 yellow, 5 white
            dma_display->drawPixel(x+1, y+5, yellow);
            dma_display->drawPixel(x+2, y+5, white);
            dma_display->drawPixel(x+3, y+5, yellow);
            dma_display->fillRect(x+4, y+5, 5, 1, white);
            
            // Row 8: 1 empty, 8 white
            dma_display->fillRect(x+1, y+6, 8, 1, white);
            
            // Row 9: 2 empty, 2 white, 1 empty, 3 white
            dma_display->fillRect(x+2, y+7, 2, 1, white);
            dma_display->fillRect(x+5, y+7, 3, 1, white);


        } else if (description == "scattered clouds") {
            
            // Row 1: 2 empty, 3 yellow
            dma_display->fillRect(x+2, y, 3, 1, yellow);

            // Row 2: 1 empty, 5 yellow
            dma_display->fillRect(x+1, y+1, 5, 1, yellow);
            
            // Row 3: 5 yellow, 3 white
            dma_display->fillRect(x, y+2, 5, 1, yellow);
            dma_display->fillRect(x+5, y+2, 3, 1, white);
            
            // Row 4: 1 yellow, 2 white, 1 yellow, 5 white
            dma_display->drawPixel(x, y+3, yellow);
            dma_display->fillRect(x+1, y+3, 2, 1, white);
            dma_display->drawPixel(x+3, y+3, yellow);
            dma_display->fillRect(x+4, y+3, 5, 1, white);
            
            // Row 5: 10 white
            dma_display->fillRect(x, y+4, 10, 1, white);
            
            // Row 6: 10 white
            dma_display->fillRect(x, y+5, 10, 1, white);
            
            // Row 7: 10 white
            dma_display->fillRect(x, y+6, 10, 1, white);
            
            // Row 8: 1 empty, 3 white, 1 empty, 4 white
            dma_display->fillRect(x+1, y+7, 3, 1, white);
            dma_display->fillRect(x+5, y+7, 4, 1, white);

        } else if (description == "broken clouds") {
            
            // Row 1: 3 empty, 3 yellow
            dma_display->fillRect(x+3, y, 3, 1, yellow);
            
            // Row 2: 1 empty, 1 white, 4 yellow, 3 white
            dma_display->drawPixel(x+1, y+1, white);
            dma_display->fillRect(x+2, y+1, 4, 1, yellow);
            dma_display->fillRect(x+6, y+1, 3, 1, white);
            
            // Row 3: 10 white
            dma_display->fillRect(x, y+2, 10, 1, white);
            
            // Row 4: 10 white, 1 dark grey
            dma_display->fillRect(x, y+3, 10, 1, white);
            dma_display->drawPixel(x+10, y+3, dark_grey);
            
            // Row 5: 10 white, 1 dark grey
            dma_display->fillRect(x, y+4, 10, 1, white);
            dma_display->drawPixel(x+10, y+4, dark_grey);
            
            // Row 6: 10 white, 1 dark grey
            dma_display->fillRect(x, y+5, 10, 1, white);
            dma_display->drawPixel(x+10, y+5, dark_grey);
            
            // Row 7: 3 dark grey, 2 white, 2 dark grey, 1 white, 3 dark grey
            dma_display->fillRect(x, y+6, 3, 1, dark_grey);
            dma_display->fillRect(x+3, y+6, 2, 1, white);
            dma_display->fillRect(x+5, y+6, 2, 1, dark_grey);
            dma_display->drawPixel(x+7, y+6, white);
            dma_display->fillRect(x+8, y+6, 3, 1, dark_grey);
            
            // Row 8: 2 empty, 3 dark grey, 1 empty, 4 dark grey
            dma_display->fillRect(x+2, y+7, 3, 1, dark_grey);
            dma_display->fillRect(x+6, y+7, 4, 1, dark_grey);
            
        } else { // "overcast clouds"
            // Default dark rain cloud shape
            dma_display->fillCircle(x + 2, y + 3, 3, dark_grey); 
            dma_display->fillCircle(x + 6, y + 4, 4, dark_grey);
            dma_display->fillCircle(x + 0, y + 4, 3, dark_grey);
        }
    }
    else if (main == "Clear") {
        dma_display->fillCircle(x + 4, y + 4, 4, yellow);
    }
    else { // Mist, Fog, Haze, etc.
        dma_display->drawLine(x, y + 2, x + 8, y + 2, grey);
        dma_display->drawLine(x, y + 4, x + 8, y + 4, grey);
        dma_display->drawLine(x, y + 6, x + 8, y + 6, grey);
    }
}

void showAllWeatherIcons() {
    dma_display->fillScreen(0x0000); // Clear screen
    dma_display->setTextSize(1);
    
    // Set positions
    int x_padding = 12; // 8px icon + 4px space
    int x = 4;

    // --- TOP ROW (5 Icons) ---
    int y_text_1 = 2;
    int y_icon_1 = 12;

    // 1. CLEAR
    dma_display->setCursor(x, y_text_1);
    dma_display->print("SUN");
    drawWeatherIcon("Clear", "clear sky", x, y_icon_1);
    
    // 2. FEW CLOUDS
    x += x_padding;
    dma_display->setCursor(x, y_text_1);
    dma_display->print("FEW");
    drawWeatherIcon("Clouds", "few clouds", x, y_icon_1);

    // 3. SCATTERED
    x += x_padding;
    dma_display->setCursor(x, y_text_1);
    dma_display->print("SCT");
    drawWeatherIcon("Clouds", "scattered clouds", x, y_icon_1);

    // 4. BROKEN
    x += x_padding;
    dma_display->setCursor(x, y_text_1);
    dma_display->print("BRK");
    drawWeatherIcon("Clouds", "broken clouds", x, y_icon_1);

    // 5. OVERCAST
    x += x_padding;
    dma_display->setCursor(x, y_text_1);
    dma_display->print("OVC");
    drawWeatherIcon("Clouds", "overcast clouds", x, y_icon_1);

    
    // --- BOTTOM ROW (5 Icons) ---
    int y_text_2 = 34;
    int y_icon_2 = 44;
    x = 4; // Reset x position

    // 6. FOG/MIST
    dma_display->setCursor(x, y_text_2);
    dma_display->print("FOG");
    drawWeatherIcon("Mist", "mist", x, y_icon_2);
    
    // 7. LIGHT RAIN
    x += x_padding;
    dma_display->setCursor(x, y_text_2);
    dma_display->print("L. RAIN");
    drawWeatherIcon("Drizzle", "light intensity drizzle", x, y_icon_2);

    // 8. HEAVY RAIN
    x += x_padding;
    dma_display->setCursor(x, y_text_2);
    dma_display->print("RAIN");
    drawWeatherIcon("Rain", "moderate rain", x, y_icon_2);

    // 9. THUNDERSTORM
    x += x_padding;
    dma_display->setCursor(x, y_text_2);
    dma_display->print("STORM");
    drawWeatherIcon("Thunderstorm", "thunderstorm", x, y_icon_2);

    // 10. SNOW
    x += x_padding;
    dma_display->setCursor(x, y_text_2);
    dma_display->print("SNOW");
    drawWeatherIcon("Snow", "snow", x, y_icon_2);

    dma_display->flipDMABuffer();
}

void fetchAstronomyData() {
    Serial.println("\n--- [ASTRO] Fetching moon phase name ---");

    // 1. Get the current time
    time_t now; 
    time(&now);
    struct tm * timeinfo = localtime(&now);

    // If it is early morning (before 6 AM), use YESTERDAY'S date.
    // This prevents the moon phase from flipping over to the next day 
    // while you are still "in the night" of the previous day.
    if (timeinfo->tm_hour < 6) {
        Serial.println("[ASTRO] Early morning detected. Fetching 'Yesterday's' moon phase.");
        time_t yesterday = now - 86400; // Subtract 24 hours (60*60*24)
        timeinfo = localtime(&yesterday);
    }

    char dateBuffer[12];
    strftime(dateBuffer, sizeof(dateBuffer), "%Y-%m-%d", timeinfo);

    Serial.printf("[ASTRO] Requesting data for date: %s\n", dateBuffer);

    char astroUrl[256];
    // 2. Use dateBuffer instead of "today" in the URL
    snprintf(astroUrl, sizeof(astroUrl),
             "https://api.weatherapi.com/v1/astronomy.json?key=%s&q=%s,%s&dt=%s",
             WEATHER_API_COM_KEY, SF_HOME_LAT, SF_HOME_LON, dateBuffer);
    
    String payload = makeHttpRequest(astroUrl);
    
    if (payload.length() < 10) {
        Serial.println("[ASTRO] ERROR: No response.");
        return;
    }

    DynamicJsonDocument doc(1024); 
    DeserializationError error = deserializeJson(doc, payload);

    if (error) {
        Serial.printf("[ASTRO] JSON parsing failed: %s\n", error.c_str());
        return;
    }

    if (doc.containsKey("error")) {
        Serial.printf("[ASTRO] API Error: %s\n", doc["error"]["message"].as<String>().c_str());
        return;
    }

    String phaseName = doc["astronomy"]["astro"]["moon_phase"].as<String>();
    
    if (!phaseName.isEmpty()) {
        currentMoonPhaseName = phaseName;
        Serial.printf("[ASTRO] Moon Phase Name: %s\n", currentMoonPhaseName.c_str());
    } else {
        currentMoonPhaseName = "UNKNOWN";
    }
}

void initConnect3() {
    for (int c = 0; c < 5; c++) {
        for (int r = 0; r < 5; r++) {
            board[c][r] = 0;
        }
    }
    connect3CursorX = 2; // Center
    playerTurn = 1; 
    gameActive = true;
    lastCpuMoveTime = millis();
    screenDirty = true;
}

// Gravity scans from Bottom (4) to Top (0)
int dropPiece(int player, int col) {
    for (int r = 4; r >= 0; r--) {
        if (board[col][r] == 0) {
            board[col][r] = player;
            return r; 
        }
    }
    return -1; // Column full
}

// Updated for 5x5 board
bool checkWin(int player, int lastCol, int lastRow) {
    if (lastRow == -1) return false;

    int dx[] = {1, 0, 1, 1};
    int dy[] = {0, 1, 1, -1};

    for (int dir = 0; dir < 4; dir++) {
        int count = 1;
        // Positive direction
        for (int i = 1; i <= 2; i++) {
            int c = lastCol + dx[dir] * i;
            int r = lastRow + dy[dir] * i;
            if (c >= 0 && c < 5 && r >= 0 && r < 5 && board[c][r] == player) count++;
            else break;
        }
        // Negative direction
        for (int i = 1; i <= 2; i++) {
            int c = lastCol - dx[dir] * i;
            int r = lastRow - dy[dir] * i;
            if (c >= 0 && c < 5 && r >= 0 && r < 5 && board[c][r] == player) count++;
            else break;
        }
        if (count >= 3) return true;
    }
    return false;
}

void cpuMove() {
    int bestMove = -1;
    int r; 

    // Priority 1: Can CPU win immediately?
    for (int col = 0; col < 5; col++) {
        r = dropPiece(2, col); 
        if (r != -1) {
            if (checkWin(2, col, r)) {
                bestMove = col;
                gameActive = false; // CPU Wins

                // --- CHAO WINS (Happy!) ---
                myPet.happiness = min(100, myPet.happiness + 50);
                myPet.energy = max(0, myPet.energy - 25);

                // Loss streak bonus: +5 vitality after 3 consecutive losses
                connect3LossStreak++;
                if (connect3LossStreak >= 3) {
                    myPet.vitality = min(100.0f, myPet.vitality + 5.0f);
                    connect3LossStreak = 0;
                    Serial.println("[VITALITY] +5 bonus: Lost Connect 3 three times in a row!");
                }

                return; // Leave piece there and exit
            }
            board[col][r] = 0; // Undo
        }
    }

    // Priority 2: Must CPU block Player?
    if (bestMove == -1) {
        for (int col = 0; col < 5; col++) {
            r = dropPiece(1, col); // Simulate Player Move
            if (r != -1) {
                if (checkWin(1, col, r)) {
                    bestMove = col; // Block this
                    board[col][r] = 0; 
                    break; 
                }
                board[col][r] = 0; 
            }
        }
    }

    // Priority 3: Random
    if (bestMove == -1) {
        int attempts = 0;
        do {
            bestMove = random(0, 5);
            if (board[bestMove][0] != 0) bestMove = -1; // Col full?
            attempts++;
        } while (bestMove == -1 && attempts < 20);
        
        // Failsafe
        if (bestMove == -1) {
            for(int i=0; i<5; i++) {
                if(board[i][0] == 0) { bestMove = i; break; }
            }
        }
    }

    // Execute Move
    if (bestMove != -1) {
        r = dropPiece(2, bestMove);
        if (checkWin(2, bestMove, r)) {
            gameActive = false; // CPU Wins

            // --- CHAO WINS (Happy!) ---
            myPet.happiness = min(100, myPet.happiness + 50);
            myPet.energy = max(0, myPet.energy - 10);

            // Loss streak bonus: +5 vitality after 3 consecutive losses
            connect3LossStreak++;
            if (connect3LossStreak >= 3) {
                myPet.vitality = min(100.0f, myPet.vitality + 5.0f);
                connect3LossStreak = 0;
                Serial.println("[VITALITY] +5 bonus: Lost Connect 3 three times in a row!");
            }

        } else {
            playerTurn = 1; // Back to Player
        }
    }
}

// *****************************************************************
// *** FLAPPY BIRD GAME ***
// *****************************************************************

void initFlappyBird() {
    flappyY = 32.0f;
    flappyVelY = 0.0f;
    flappyActive = false;
    flappyDead = false;
    flappyScore = 0;
    flappyActivePipes = 0;
    flappyPipeTimer = 55; // First pipe spawns after ~55 frames
    flappyGavePlayBonus = false;
    flappyGave1PointBonus = false;
    flappyGave10PointBonus = false;
    screenDirty = true;
}
