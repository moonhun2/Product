//Viral Science www.youtube.com/c/viralscience  www.viralsciencecreativity.com
//ESP32 Camera Surveillance Car

#include "esp_camera.h"
#include <WiFi.h>

#include "APSetting.h"    // BLUETOOTH로 WIFI 설정

//
// WARNING!!! Make sure that you have either selected ESP32 Wrover Module,
//            or another board which has PSRAM enabled
//
// Adafruit ESP32 Feather

// Select camera model
//#define CAMERA_MODEL_WROVER_KIT
//#define CAMERA_MODEL_M5STACK_PSRAM
#define CAMERA_MODEL_AI_THINKER
/*
char* ssid = "dsp-v30";   //Enter SSID WIFI Name
char* password = "ekf98765";   //Enter WIFI Password
*/
IPAddress local_IP(192, 168, 43, 101);
// Set your Gateway IP address
// Adresse du routeur ou de la box internet
IPAddress gateway(192, 168, 43, 1);
IPAddress subnet(255, 255, 255, 0);
IPAddress primaryDNS(210,220,163,82);   //optional
IPAddress secondaryDNS(219,250,36,130); //optional

#if defined(CAMERA_MODEL_WROVER_KIT)
#define PWDN_GPIO_NUM    -1
#define RESET_GPIO_NUM   -1
#define XCLK_GPIO_NUM    21
#define SIOD_GPIO_NUM    26
#define SIOC_GPIO_NUM    27

#define Y9_GPIO_NUM      35
#define Y8_GPIO_NUM      34
#define Y7_GPIO_NUM      39
#define Y6_GPIO_NUM      36
#define Y5_GPIO_NUM      19
#define Y4_GPIO_NUM      18
#define Y3_GPIO_NUM       5
#define Y2_GPIO_NUM       4
#define VSYNC_GPIO_NUM   25
#define HREF_GPIO_NUM    23
#define PCLK_GPIO_NUM    22


#elif defined(CAMERA_MODEL_AI_THINKER)
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

#else
#error "Camera model not selected"
#endif



/*
 * typedef enum {
    FRAMESIZE_QQVGA,    // 160x120
    FRAMESIZE_QQVGA2,   // 128x160
    FRAMESIZE_QCIF,     // 176x144
    FRAMESIZE_HQVGA,    // 240x176
    FRAMESIZE_QVGA,     // 320x240
    FRAMESIZE_CIF,      // 400x296
    FRAMESIZE_VGA,      // 640x480
    FRAMESIZE_SVGA,     // 800x600
    FRAMESIZE_XGA,      // 1024x768
    FRAMESIZE_SXGA,     // 1280x1024
    FRAMESIZE_UXGA,     // 1600x1200
    FRAMESIZE_QXGA,     // 2048*1536
    FRAMESIZE_INVALID
} framesize_t;

*/


// use configuration
extern Preferences preferences; // APSetting.h에 선언된 객체 사용

// GPIO Setting

extern String WiFiAddr ="";

void startCameraServer();

void MC_Init();
void MC_SaveRegister(bool bWiFiUsing);
void MC_Watered();


/* 인터럽트 타이머 설정 관련 */

// 하드웨어 타이머 생성
hw_timer_t * timer = NULL;

// triggering state 
volatile boolean bTrigger = false;

// timer handler
void IRAM_ATTR onTimer(){
  bTrigger = true;
}

// Make timer and enable it. 
void SetTimerInterrupt(unsigned long ulMicroSecond){    
  
  /* 1 tick take 1/(80MHZ/80) = 1us so we set divider 80 and count up */
  timer = timerBegin(0, 80, true);

  /* Attach onTimer function to our timer */
  timerAttachInterrupt(timer, &onTimer, true);

  /* Set alarm to call onTimer function every second 1 tick is 1us
  => 1 second is 1000000us */
  /* Repeat the alarm (third parameter) */
  timerAlarmWrite(timer, ulMicroSecond, true);

  /* Start an alarm */
  timerAlarmEnable(timer);
}

void setup() {
  
  Serial.begin(115200);

  MC_Init();    // motion control 초기화
  MC_SaveRegister(false);
  
  Serial.setDebugOutput(true);
  Serial.println();

  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  //config.xclk_freq_hz = 20000000;
  config.xclk_freq_hz = 10000000;
  config.pixel_format = PIXFORMAT_JPEG;
  //init with high specs to pre-allocate larger buffers
  if(psramFound()){
    //config.frame_size = FRAMESIZE_UXGA;
    config.frame_size = FRAMESIZE_VGA;
    config.jpeg_quality = 10;
    config.fb_count = 2;
    //config.fb_count = 3;
    Serial.println("psram founded");
  } else {
    config.frame_size = FRAMESIZE_SVGA;
    config.jpeg_quality = 12;
    config.fb_count = 1;
    Serial.println("psram Not founded");
  }

  // camera init
  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed with error 0x%x", err);
    return;
  }

  //drop down frame size for higher initial frame rate
  sensor_t * s = esp_camera_sensor_get();
  s->set_framesize(s, FRAMESIZE_CIF);

  Serial.println("Get into APS_init()");
  APS_init();   // AP setting 초기화

  APS_wait_for_BLE();
  
  if(APS_flow_Setting())  // WiFi에 접속하면 true
    startCameraServer();
  else
    APS_Alarm_On();       // WiFi 접속에 문제가 생겨서 빨간 LED를 점멸
  
  Serial.print("Camera Ready! Use 'http://");
  Serial.print(WiFi.localIP());
  WiFiAddr = WiFi.localIP().toString();
  Serial.println("' to connect");

  MC_SaveRegister(true);

  SetTimerInterrupt(1000000);// 1초에 한번씩 타이머 인터럽트
}




unsigned long ulTimeReset = millis(); // 네트웍이 끊어지는 문제로 30분마다 리셋
unsigned long ulTimeGetInfo = millis(); // 네트웍이 끊어지는 문제로 30분마다 리셋

#define TIME_GET_WATER 1000
#define TIME_RESET  1800000           // 1000 * 60 * 30
//#define TIME_RESET  30000           // 1000 * 60 * 60

void loop() {
  
  if(bTrigger)
  {
    //Serial.println("\n\n Timer interrupted. \n\n");
    
    if( (millis() - ulTimeReset) > TIME_RESET)
    {
      ulTimeReset = millis(); // 타이머 초기화
      preferences.putString("pref_reset", "SW_Reset");  // SW 리셋이 진행중임을 기록
      Serial.println("\n\n restart ESP \n\n");
      ESP.restart();
    }
  
    if( (millis() - ulTimeGetInfo) > TIME_GET_WATER)
    {
      //APS_GetWiFiStatus();
      ulTimeGetInfo = millis(); // 타이머 초기화
      MC_Watered();
    }

    bTrigger = !bTrigger;
  }
}
