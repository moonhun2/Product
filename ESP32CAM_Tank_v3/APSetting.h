/*
 * WIFI 설정에 BLUETOOTH 방법을 사용
 * 1. WIFI 접속 전에 BLE의 CALLBACK에 접속이벤트가 발생하면 WIFI 설정을 진행
 * 2. WIFI 접속에 성공하면 BLE의 접속을 끊음.
 * 3. WIFI 설정이 성공하면 이름과 비밀번호를 EEPROM에 저장
 * 
 * 
 * 사용자 액션               BLE
 * WIFI 접속                
 *                          SCAN_START
 *                          AP listing , SCAN_COMPLETE 상태로 변경, AP 리스트 전송
 * AP번호 선택               사용자 선택 AP 저장, 패스워드 입력글 전송 WAIT_PASS 상태로 변경
 * 패스워드 입력              패스워드 저장, WAIT_CONNECT 상태 변경, WIFI 접속
 */



// 환경설정을 위한 내용

#include <Preferences.h>
Preferences preferences;

const char* pref_ssid = "";
const char* pref_pass = "";
String client_wifi_ssid;
String client_wifi_password;

String ssids_array[50];
String network_string;
String connected_string;

//int nTrimCenter = 0;
/*
void APS_SetTrim(int nTrim)   // trim을 preferences 객체를 통해 저장
{
  preferences.putString("nTrimCenter", nTrim);
}

int APS_GetTrim()             // trim을 preference 객체를 통해 읽음
{
  return preferences.getString("nTrimCenter");
}
*/

// 블루투스 사용을 위한 내용
#include <BluetoothSerial.h>
#if !defined(CONFIG_BT_ENABLED) || !defined(CONFIG_BLUEDROID_ENABLED)
#error Bluetooth is not enabled! Please run `make menuconfig` to and enable it
#endif
BluetoothSerial BLE;
bool bluetooth_disconnect = false;


// 타이머를 위한 설정
#define PERIOD_ALARM_BLE  500    // 설정된 시간(밀리초)마다 깜빡임
#define PERIOD_ALARM_WIFI 200    // 설정된 시간(밀리초)마다 깜빡임
#define TIME_SETTING      7000    // AP설정을 위해 BLUETOOTH가 대기하는 시간
#define TIME_WIFI_TRY     10000   // 와이파이 접속 시도 시간
unsigned long ulTimeIPShow  = millis(); // BLE로 현 설정IP를 보여주기 위해 대기해주는 시간

// 알람을 위한 설정
#define PIN_BUILTIN_LED   4      // 내장된 RED LED
#define PIN_RED_LED       33      // 내장된 RED LED
unsigned long ulTime = millis();
bool bShowAlarm = LOW;

// WIFI 처리절차
enum wifi_setup_states { NONE, SCAN_START, SCAN_COMPLETE, SSID_ENTERED, WAIT_PASS, PASS_ENTERED, WAIT_CONNECT, LOGIN_FAILED , IDLE_SHOW_IP, STAY_SHOW_IP};
enum wifi_setup_states wifi_state = NONE;
bool bWiFi_connected = false;


void handler_Alarm(int nAlarmPerion, bool bTurnOff = false)
{
  if(bTurnOff)
  {
    digitalWrite(PIN_BUILTIN_LED, LOW);
    return;
  }
  
  if(millis() - ulTime > nAlarmPerion)  // 알람 led 깜박이기
  {
    bShowAlarm = !bShowAlarm;
    digitalWrite(PIN_BUILTIN_LED, bShowAlarm);
    ulTime = millis();
  }
}

bool init_wifi()
{
  // for WIFI
  String temp_pref_ssid = preferences.getString("pref_ssid");
  String temp_pref_pass = preferences.getString("pref_pass");
  pref_ssid = temp_pref_ssid.c_str();
  pref_pass = temp_pref_pass.c_str();

  Serial.println(pref_ssid);
  Serial.println(pref_pass);

  WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE);

  long start_wifi_millis = millis();
  WiFi.begin(pref_ssid, pref_pass);
  
  while (WiFi.status() != WL_CONNECTED) {
    handler_Alarm(PERIOD_ALARM_WIFI);
    
    delay(200);
    Serial.print(".");
    
    if (millis() - start_wifi_millis > TIME_WIFI_TRY) {
      WiFi.disconnect(true, true);
      return false;
    }
  }

  // for MOTOR CONTROL 
  //nTrimCenter = APS_GetTrim();
  return true;
}

void scan_wifi_networks()
{
  WiFi.mode(WIFI_STA);
  // WiFi.scanNetworks will return the number of networks found
  int n =  WiFi.scanNetworks();
  if (n == 0) {
    //BLE.println("no networks found");
    BLE.println("WIFI가 발견되지 않습니다.");
  } else {
    BLE.println();
    BLE.print(n);
    //BLE.println(" networks found");
    BLE.println("개의 WIFI가 발견되었습니다.");
    delay(1000);
    for (int i = 0; i < n; ++i) {
      ssids_array[i + 1] = WiFi.SSID(i);
      Serial.print(i + 1);
      Serial.print(": ");
      Serial.println(ssids_array[i + 1]);
      network_string = i + 1;
      network_string = network_string + ": " + WiFi.SSID(i) + " (Strength:" + WiFi.RSSI(i) + ")";
      BLE.println(network_string);
    }
    wifi_state = SCAN_COMPLETE;
  }
}

//////////////////////
////// CALLBACK  /////
//////////////////////

void callbackBT(esp_spp_cb_event_t event, esp_spp_cb_param_t *param){

  if (event == ESP_SPP_SRV_OPEN_EVT) {  // 스캔 절차 시작
    Serial.println("client connected without WIFI");
    wifi_state = SCAN_START;
  }

  if (event == ESP_SPP_DATA_IND_EVT && wifi_state == SCAN_COMPLETE) { // data from phone is SSID
    int client_wifi_ssid_id = BLE.readString().toInt();
    client_wifi_ssid = ssids_array[client_wifi_ssid_id];
    wifi_state = SSID_ENTERED;
  }

  if (event == ESP_SPP_DATA_IND_EVT && wifi_state == WAIT_PASS) { // data from phone is password
    client_wifi_password = BLE.readString();
    client_wifi_password.trim();
    wifi_state = PASS_ENTERED;
  }
}


void callback_show_ip(esp_spp_cb_event_t event, esp_spp_cb_param_t *param)
{
  if (event == ESP_SPP_SRV_OPEN_EVT) {
    wifi_state = IDLE_SHOW_IP;
    Serial.println("client connected with WIFI");
  }

  // IP 확인시에 사용
  if (event == ESP_SPP_DATA_IND_EVT && wifi_state == STAY_SHOW_IP) {  
    if(BLE.readString().toInt() == 1) // 1: exit 2:AP select (재설정)
    {
      wifi_state = NONE;
      bluetooth_disconnect = true;
    }
    else
      wifi_state = SCAN_START;      // BLE SETUP state 절차로 변경
  }

  // IP 재설정시에 사용
  if (event == ESP_SPP_DATA_IND_EVT && wifi_state == SCAN_COMPLETE) { // data from phone is SSID
    int client_wifi_ssid_id = BLE.readString().toInt();
    client_wifi_ssid = ssids_array[client_wifi_ssid_id];
    wifi_state = SSID_ENTERED;
  }

  if (event == ESP_SPP_DATA_IND_EVT && wifi_state == WAIT_PASS) { // data from phone is password
    client_wifi_password = BLE.readString();
    client_wifi_password.trim();
    wifi_state = PASS_ENTERED;
  }

}

void disconnect_bluetooth()
{
  delay(1000);
  Serial.println("BT stopping");
  //BLE.println("Bluetooth disconnecting...");
  BLE.println("블루투스 사용을 종료합니다...");
  delay(1000);
  BLE.flush();
  BLE.disconnect();
  BLE.end();
  Serial.println("BT stopped");
  delay(1000);
  bluetooth_disconnect = false;
}



void APS_init()
{
  pinMode(PIN_RED_LED, OUTPUT); // 알람용 LED
  pinMode(PIN_BUILTIN_LED, OUTPUT); // 알람용 LED
  
  preferences.begin("wifi_access", false);

  if (!init_wifi()) { // Connect to Wi-Fi fails
    BLE.register_callback(callbackBT);
  } else {
    BLE.register_callback(callback_show_ip);
    bWiFi_connected = true;
  }

  BLE.begin("EXPLORER_TRACK");

  
  // timer들 초기화
  ulTimeIPShow = millis();
  ulTime = millis();
}

unsigned long ulWaitingTime = TIME_SETTING;
// 본 함수에서 BLOCK하는 동안 BLE CALLBACK을 기다림
void APS_wait_for_BLE()
{
  while(1)
  {
    handler_Alarm(PERIOD_ALARM_BLE);  // blink the BUILT-IN-RED

    if(wifi_state == NONE)  // BLE를 통한 WIFI 설정 흐름이 진행되지 않음
    {
      if( bWiFi_connected &&                          // WIFI 접속이 유효해도
        (millis() - ulTimeIPShow > ulWaitingTime) )   // 일정시간 대기모드 (다른 AP로 접속설정을 바꾸려는 경우)
      {
        if(bWiFi_connected) // 와이파이가 연결된 상황에서만 블루투스 끊음 (와이파이가 연결되지 않으면 블루투스 작업 필요)
          bluetooth_disconnect = true;  // 블루투스를 끊으라고 명령
        
        Serial.printf("APS_wait_for_BLE wait for WIFI-change signal %d", 1);  Serial.println();
        return;
      }

      Serial.printf("APS_wait_for_BLE wait for WIFI-setting signal %d", 1);  Serial.println();
    }
    else // (wifi_state == SCAN_START || wifi_state == IDLE_SHOW_IP)  // BLE 설정작업 callback 혹은 BLE 보이기 callback 시작
    {
      Serial.printf("APS_wait_for_BLE Progress a sequence of WIFI-setting with BLE %d", 2);    Serial.println();
      return;
    }
  
    Serial.print("*");
    delay(200);
  }
}

bool bExitFlow = false;   // AP세팅을 위한 BLE를 빠져나가기 위한 변수

bool APS_flow_Setting()
{
  handler_Alarm(0,true);  // 알람 led 끄기
  
  while(!bExitFlow)
  {
    if(wifi_state != NONE){
      Serial.printf("state = %d", wifi_state);
      Serial.println();
    }

    if (bluetooth_disconnect)
    {
      disconnect_bluetooth();
      bExitFlow = true;
      Serial.print("bExitFlow-bluetooth_disconnect");
    }
  
    switch (wifi_state)
    {
      //////////////////////////////////////////
      //////// Connect sequence ////////////////
      
      case SCAN_START:
        //BLE.println("Scanning Wi-Fi networks");
        BLE.println("WIFI를 검색합니다.");
        Serial.println("Scanning Wi-Fi networks");
        scan_wifi_networks();
        //BLE.println("Please enter the number for your Wi-Fi");
        BLE.println("연결할 WIFI의 번호를 입력하세요.");
        wifi_state = SCAN_COMPLETE;
        break;

      case SSID_ENTERED:
        //BLE.println("Please enter your Wi-Fi password");
        BLE.println("선택한 WIFI의 비밀번호를 입력하세요");
        Serial.println("Please enter your Wi-Fi password");
        wifi_state = WAIT_PASS;
        break;
  
      case PASS_ENTERED:
        //BLE.println("Please wait for Wi-Fi connection...");
        BLE.println("WIFI에 접속중....");
        Serial.println("Please wait for Wi_Fi connection...");
        wifi_state = WAIT_CONNECT;
        preferences.putString("pref_ssid", client_wifi_ssid);
        preferences.putString("pref_pass", client_wifi_password);
        if (init_wifi()) { // Connected to WiFi
          connected_string = "아래 링크를 브라우저에 입력하세요.\nhttp://";
          connected_string = connected_string + WiFi.localIP().toString();
          BLE.println(connected_string);
          Serial.println(connected_string);
          bluetooth_disconnect = true;
          bWiFi_connected = true;
        } else { // try again
          wifi_state = LOGIN_FAILED;
          bWiFi_connected = false;
        }
        break;
  
      case LOGIN_FAILED:
        BLE.println("WIFI 접속에 실패하였습니다.");
        Serial.println("Wi-Fi connection failed");
        delay(2000);
        wifi_state = SCAN_START;
        bWiFi_connected = false;
        break;

      //////////////////////////////////////////
      //////// Show-IP sequence ////////////////
      
      case IDLE_SHOW_IP:
        connected_string = "아래 링크를 브라우저에 입력하세요.\nhttp://";
        connected_string = connected_string + WiFi.localIP().toString();
        BLE.println(connected_string);
        Serial.println(connected_string);
        //BLE.println("If you want to exit, enter 1 (Want to select, enter any-key).");
        BLE.println("종료하려면 1을, 다른 WIFI에 접속하려면 아무키나 입력하세요.");

        Serial.println("IDLE_SHOW");
        wifi_state = STAY_SHOW_IP;
        break;

      case STAY_SHOW_IP:
        Serial.println("STAY_SHOW_IP");
        break;
        
      case NONE:
        bExitFlow = true;
        Serial.println("NONE");
        break;
    }

    Serial.print("*");
    delay(100);
        
  }

  handler_Alarm(0, true);  // 알람LED 끄기
  return bWiFi_connected;
}


bool APS_Alarm_On()
{
  while(1)
  {
    handler_Alarm(PERIOD_ALARM_WIFI);
    delay(100);
  }
}
