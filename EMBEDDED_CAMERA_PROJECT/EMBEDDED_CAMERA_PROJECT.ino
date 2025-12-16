/* =============================================================================
 * CONAN AI CAMERA
 * EDGE IMPULSE + ESP32-CAM + TFT DISPLAY
 * =============================================================================
 */

#include <Embedded_inferencing.h>
#include "edge-impulse-sdk/dsp/image/image.hpp"
#include <esp_camera.h>
#include <TFT_eSPI.h>

/* TFT display instance */
TFT_eSPI tft = TFT_eSPI();

/* =============================================================================
 * GPIO PIN DEFINITIONS
 * =============================================================================
 */
#define BUTTON_PIN 13          // User input button
#define FLASH_PIN  4           // Camera flash LED

/* =============================================================================
 * BUTTON AND MODE TIMING PARAMETERS (milliseconds)
 * =============================================================================
 */
#define LONG_PRESS_TIME 700    // Duration to trigger continuous mode
#define FREEZE_TIME     2000   // Duration to freeze inference result
#define LIVE_TIME       1000   // Duration of live preview between inferences

/* =============================================================================
 * MODE LABEL BLINK INTERVAL
 * =============================================================================
 */
#define MODE_BLINK_INTERVAL 1000

/* =============================================================================
 * CAMERA MODEL SELECTION
 * =============================================================================
 */
#define CAMERA_MODEL_AI_THINKER

#if defined(CAMERA_MODEL_AI_THINKER)
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27
#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22
#endif

/* =============================================================================
 * EDGE IMPULSE CAMERA CONFIGURATION
 * =============================================================================
 */
#define EI_CAMERA_RAW_FRAME_BUFFER_COLS 320
#define EI_CAMERA_RAW_FRAME_BUFFER_ROWS 240
#define EI_CAMERA_FRAME_BYTE_SIZE       3

/* =============================================================================
 * SYSTEM STATE VARIABLES
 * =============================================================================
 */
bool capture_mode = false;         // Single capture (freeze) mode
bool continuous_mode = false;      // Continuous inference mode
bool continuous_freeze = true;     // Freeze state during continuous mode

bool last_button_state = HIGH;
unsigned long button_press_time = 0;
bool long_press_handled = false;
unsigned long continuous_timer = 0;

/* Mode label blinking control */
bool mode_label_visible = true;
unsigned long mode_blink_timer = 0;

/* =============================================================================
 * IMAGE BUFFER
 * =============================================================================
 */
uint8_t *snapshot_buf = NULL;

/* =============================================================================
 * CAMERA HARDWARE CONFIGURATION STRUCTURE
 * =============================================================================
 */
camera_config_t camera_config = {
  .pin_pwdn = PWDN_GPIO_NUM,
  .pin_reset = RESET_GPIO_NUM,
  .pin_xclk = XCLK_GPIO_NUM,
  .pin_sscb_sda = SIOD_GPIO_NUM,
  .pin_sscb_scl = SIOC_GPIO_NUM,
  .pin_d7 = Y9_GPIO_NUM,
  .pin_d6 = Y8_GPIO_NUM,
  .pin_d5 = Y7_GPIO_NUM,
  .pin_d4 = Y6_GPIO_NUM,
  .pin_d3 = Y5_GPIO_NUM,
  .pin_d2 = Y4_GPIO_NUM,
  .pin_d1 = Y3_GPIO_NUM,
  .pin_d0 = Y2_GPIO_NUM,
  .pin_vsync = VSYNC_GPIO_NUM,
  .pin_href = HREF_GPIO_NUM,
  .pin_pclk = PCLK_GPIO_NUM,
  .xclk_freq_hz = 20000000,
  .ledc_timer   = LEDC_TIMER_0,
  .ledc_channel = LEDC_CHANNEL_0,
  .pixel_format = PIXFORMAT_RGB565,
  .frame_size   = FRAMESIZE_QVGA,
  .fb_count     = 2,
  .fb_location  = CAMERA_FB_IN_PSRAM,
  .grab_mode    = CAMERA_GRAB_WHEN_EMPTY
};

/* =============================================================================
 * SPLASH SCREEN DISPLAY
 * =============================================================================
 */
void showSplashScreen() {
  tft.fillScreen(TFT_BLACK);

  // Application title
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setTextSize(4);
  tft.setCursor(80, 70);
  tft.print("Conan");

  delay(1500);

  // Author credits
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(2);

  tft.setCursor(80, 100);
  tft.print("by Exconde");

  tft.setCursor(70, 130);
  tft.print("and Macalintal");

  delay(1500);

  tft.fillScreen(TFT_BLACK);
}

/* =============================================================================
 * MODE LABEL RENDERING (LIVE / CAPTURE / CONTINUOUS)
 * =============================================================================
 */
void drawMode(const char* txt, uint16_t color) {
  tft.setTextSize(2);

  uint16_t text_w = strlen(txt) * 6 * 2;
  uint16_t text_h = 8 * 2;

  // Clear previous label area
  tft.fillRect(0, 0, text_w + 12, text_h + 12, TFT_BLACK);

  if (!mode_label_visible) return;

  tft.setTextColor(color, TFT_BLACK);
  tft.setCursor(6, 6);
  tft.print(txt);
}

/* =============================================================================
 * EDGE IMPULSE DATA PROVIDER CALLBACK
 * =============================================================================
 */
static int ei_camera_get_data(size_t offset, size_t length, float *out_ptr) {
  size_t px = offset * 3;
  for (size_t i = 0; i < length; i++) {
    out_ptr[i] =
      (snapshot_buf[px + 2] << 16) |
      (snapshot_buf[px + 1] << 8)  |
       snapshot_buf[px];
    px += 3;
  }
  return 0;
}

/* =============================================================================
 * CAMERA INITIALIZATION
 * =============================================================================
 */
bool ei_camera_init(void) {
  if (esp_camera_init(&camera_config) != ESP_OK) return false;
  sensor_t *s = esp_camera_sensor_get();
  s->set_vflip(s, 1);
  return true;
}

/* =============================================================================
 * IMAGE CAPTURE AND PREPROCESSING FOR EDGE IMPULSE
 * =============================================================================
 */
bool ei_camera_capture(uint32_t w, uint32_t h) {

  if (!continuous_mode) {
    digitalWrite(FLASH_PIN, HIGH);
    delay(1000);
    digitalWrite(FLASH_PIN, LOW);
    delay(20);
  }

  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) return false;

  bool ok = fmt2rgb888(fb->buf, fb->len, PIXFORMAT_RGB565, snapshot_buf);
  esp_camera_fb_return(fb);
  if (!ok) return false;

  ei::image::processing::crop_and_interpolate_rgb888(
    snapshot_buf,
    EI_CAMERA_RAW_FRAME_BUFFER_COLS,
    EI_CAMERA_RAW_FRAME_BUFFER_ROWS,
    snapshot_buf,
    w, h
  );

  return true;
}

/* =============================================================================
 * EDGE IMPULSE INFERENCE EXECUTION
 * =============================================================================
 */
void runEI() {

  if (!ei_camera_capture(EI_CLASSIFIER_INPUT_WIDTH,
                          EI_CLASSIFIER_INPUT_HEIGHT)) return;

  ei::signal_t signal;
  signal.total_length =
    EI_CLASSIFIER_INPUT_WIDTH *
    EI_CLASSIFIER_INPUT_HEIGHT;
  signal.get_data = &ei_camera_get_data;

  ei_impulse_result_t result = {0};
  run_classifier(&signal, &result, false);

  tft.fillRect(0, 200, 320, 40, TFT_BLACK);
  tft.setTextSize(2);
  tft.setCursor(10, 205);

#if EI_CLASSIFIER_OBJECT_DETECTION
  for (uint32_t i = 0; i < result.bounding_boxes_count; i++) {
    auto bb = result.bounding_boxes[i];
    if (bb.value > 0.5) {
      tft.printf("%s %.0f%%", bb.label, bb.value * 100);
      return;
    }
  }
  tft.print("No object");
#else
  uint16_t best = 0;
  float best_val = 0;
  for (uint16_t i = 0; i < EI_CLASSIFIER_LABEL_COUNT; i++) {
    if (result.classification[i].value > best_val) {
      best_val = result.classification[i].value;
      best = i;
    }
  }
  tft.printf("%s %.0f%%",
    ei_classifier_inferencing_categories[best],
    best_val * 100);
#endif
}

/* =============================================================================
 * SYSTEM SETUP
 * =============================================================================
 */
void setup() {
  Serial.begin(115200);

  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(FLASH_PIN, OUTPUT);
  digitalWrite(FLASH_PIN, LOW);

  tft.init();
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);

  showSplashScreen();

  ei_camera_init();

  snapshot_buf = (uint8_t*)ps_malloc(
    EI_CAMERA_RAW_FRAME_BUFFER_COLS *
    EI_CAMERA_RAW_FRAME_BUFFER_ROWS *
    EI_CAMERA_FRAME_BYTE_SIZE
  );

  drawMode("LIVE", TFT_GREEN);
}

/* =============================================================================
 * MAIN EXECUTION LOOP
 * =============================================================================
 */
void loop() {
  unsigned long now = millis();
  bool btn = digitalRead(BUTTON_PIN);

  // Mode label blinking handler
  if (now - mode_blink_timer >= MODE_BLINK_INTERVAL) {
    mode_blink_timer = now;
    mode_label_visible = !mode_label_visible;

    if (continuous_mode)      drawMode("CONTINUOUS", TFT_CYAN);
    else if (capture_mode)    drawMode("CAPTURE", TFT_YELLOW);
    else                      drawMode("LIVE", TFT_GREEN);
  }

  // Button press detection
  if (btn == LOW && last_button_state == HIGH) {
    button_press_time = now;
    long_press_handled = false;
  }

  // Long press: toggle continuous mode
  if (btn == LOW && !long_press_handled &&
      now - button_press_time >= LONG_PRESS_TIME) {
    continuous_mode = !continuous_mode;
    capture_mode = false;
    long_press_handled = true;
    continuous_freeze = true;
    continuous_timer = now;
    digitalWrite(FLASH_PIN, continuous_mode ? HIGH : LOW);
  }

  // Short press: toggle capture mode
  if (btn == HIGH && last_button_state == LOW && !long_press_handled) {
    capture_mode = !capture_mode;
    if (capture_mode) runEI();
  }

  last_button_state = btn;

  // Continuous inference state machine
  if (continuous_mode) {
    if (continuous_freeze) {
      if (now - continuous_timer >= FREEZE_TIME) {
        continuous_freeze = false;
        continuous_timer = now;
      }
      return;
    } else {
      camera_fb_t *fb = esp_camera_fb_get();
      if (fb) {
        tft.pushImage(0, 0, 320, 240, (uint16_t*)fb->buf);
        esp_camera_fb_return(fb);
      }
      if (now - continuous_timer >= LIVE_TIME) {
        continuous_freeze = true;
        continuous_timer = now;
        runEI();
      }
      return;
    }
  }

  // Freeze mode
  if (capture_mode) return;

  // Live preview mode
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) return;
  tft.pushImage(0, 0, 320, 240, (uint16_t*)fb->buf);
  esp_camera_fb_return(fb);
}

/* =============================================================================
 * EDGE IMPULSE SENSOR VALIDATION
 * =============================================================================
 */
#if !defined(EI_CLASSIFIER_SENSOR) || EI_CLASSIFIER_SENSOR != EI_CLASSIFIER_SENSOR_CAMERA
#error "Invalid Edge Impulse model: camera sensor required"
#endif
