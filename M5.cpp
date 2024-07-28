#include "M5.h"
#include <XPT2046_Touchscreen.h>

//#define BLK_PWM_CHANNEL 7  // LEDC_CHANNEL_7
SPIClass mySpi(VSPI);
XPT2046_Touchscreen ts(XPT2046_CS, XPT2046_IRQ);
uint16_t touchScreenMinimumX = 200, touchScreenMaximumX = 3700, touchScreenMinimumY = 240,touchScreenMaximumY = 3800;

void M5Display::begin() {
    init();
    setRotation(1);
    fillScreen(0);

    // Core2 Init the back-light LED PWM
    //ledcSetup(BLK_PWM_CHANNEL, 44100, 8);
    //ledcAttachPin(TFT_BL, BLK_PWM_CHANNEL);
    //ledcWrite(BLK_PWM_CHANNEL, 80);
}

void M5Display::clear() {
    fillScreen(BLACK);
}

float AXP192::GetBatVoltage() {
    return 3.7;
}

void AXP192::SetSpkEnable(bool enable) {
}

void AXP192::SetLcdVoltage(uint16_t voltage) {
    //todo set backlight level
}

bool M5Touch::ispressed() {
    return ts.touched();
}

Event::operator uint16_t() {
    return state;
}

void M5Stack::begin() {
    Lcd.begin();
    Serial.begin(115200);

    // Start the SPI for the touch screen and init the TS library
    configureTouchScreen();
}

// Call this after startMozzi() which itself calls i2s_set_pin() that destroys this setup for pin 25.
void M5Stack::configureTouchScreen() {

    // Start the SPI for the touch screen and init the TS library
    mySpi.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI, XPT2046_CS);
    ts.begin(mySpi);
    ts.setRotation(1);
}

float M5Stack::getx(){
    return ts.getPoint().x;
}
void M5Stack::update() {
    // Get touch state
    static bool lastUpdateTouched = false;
    bool touched = ts.tirqTouched() && ts.touched();
    if (touched && !lastUpdateTouched) { // don't raise a new touch event until after last touch is released
        TS_Point p = ts.getPoint();
        // Some very basic auto calibration so it doesn't go out of range
        // if(p.x < touchScreenMinimumX) touchScreenMinimumX = p.x;
        // if(p.x > touchScreenMaximumX) touchScreenMaximumX = p.x;
        // if(p.y < touchScreenMinimumY) touchScreenMinimumY = p.y;
        // if(p.y > touchScreenMaximumY) touchScreenMaximumY = p.y;
        // Map this to the pixel position
        Buttons.event.state = E_TOUCH;
        // Note TFT_HEIGHT/WIDTH are swapped since rotation=1
        Buttons.event.from.x = map(p.x,touchScreenMinimumX,touchScreenMaximumX,0,TFT_HEIGHT-1); /* Touchscreen X calibration */
        Buttons.event.from.y = map(p.y,touchScreenMinimumY,touchScreenMaximumY,0,TFT_WIDTH-1); /* Touchscreen Y calibration */
        Serial.print("touch at x = ");
        Serial.print(Buttons.event.from.x);
        Serial.print(", y = ");
        Serial.print(Buttons.event.from.y);
        Serial.println();
        lastUpdateTouched = true;
    } else if (touched && lastUpdateTouched) {
        Buttons.event.state = 0;
        lastUpdateTouched = true;
    } else {
        Buttons.event.state = 0;
        lastUpdateTouched = false;
    }
}

M5Stack M5;