// Minimal M5Stack implementation to work with OscPocketM
// https://github.com/m5stack/M5Core2

#include <TFT_eSPI.h>

#define BLACK       TFT_BLACK
#define WHITE       TFT_WHITE
#define RED         TFT_RED
#define YELLOW      TFT_YELLOW
#define ORANGE      TFT_ORANGE
#define BLUE        TFT_BLUE
#define GREEN       TFT_GREEN

#define E_TOUCH        0x0001

// The CYD touch uses some non default SPI pins
// From https://github.com/witnessmenow/ESP32-Cheap-Yellow-Display/blob/main/Examples/Basics/2-TouchTest/2-TouchTest.ino
#define XPT2046_IRQ 36
#define XPT2046_MOSI 32
#define XPT2046_MISO 39
#define XPT2046_CLK 25
#define XPT2046_CS 33

class M5Display : public TFT_eSPI {
    public:
    void begin();
    void clear();
};

class AXP192 {
    public:
    float GetBatVoltage();
    void SetSpkEnable(bool);
    void SetLcdVoltage(uint16_t);
};

class M5Touch {
    public:
    bool ispressed();
};

class Point {
    public:
    int16_t x, y;
};

class Event {
   public:
    operator uint16_t();
    Point from;
    uint16_t state = 0;
};

class M5Buttons {
    public:
    Event event;
};

class M5Stack {
    public:
    void begin();
    void configureTouchScreen();
    void update();
    float getx();
    M5Display Lcd;
    AXP192 Axp;
    M5Buttons Buttons;
    M5Touch Touch;
    private:
};

extern M5Stack M5;