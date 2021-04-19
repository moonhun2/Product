// analogRead문제 (ADC2를 wifi와 같이 사용못하는) 문제를 해결하기 위해서
// ADC2
#include "esp32-hal-adc.h" // needed for adc pin reset
#include "soc/sens_reg.h" // needed for adc pin reset
uint64_t reg_b1, reg_b2, reg_b3; // Used to store Pin registers  before WIFI
uint64_t reg_f1, reg_f2, reg_f3; // Used to store Pin registers  after WIFI

#include <Preferences.h>
extern Preferences preferences; // APSetting.h에 선언된 객체 사용


#define PIN_MOIST_SENSOR  14
#define PIN_SOL_VALVE     15

#define PIN_LED 4

//#define VALUE_MAX_MOISTSENSOR 600
#define VALUE_MOISTSENSOR_WET 2100
#define VALUE_MOISTSENSOR_DRY 4095

int nMoistRate = 50;  // 수분 설정 비율(%)


/* 서보관련 초기화  */
void MC_Init() {

  pinMode(PIN_SOL_VALVE, OUTPUT);

//  Fill_pump();
  
}

void MC_SaveRegister(bool bWiFiUsing) {

  if(bWiFiUsing)  // wifi를 사용중인 상태
  {
    // Save Pin Registers : Do this before begin Wifi
    reg_f1 = READ_PERI_REG(SENS_SAR_START_FORCE_REG);
    reg_f2 = READ_PERI_REG(SENS_SAR_READ_CTRL2_REG);
    reg_f3 = READ_PERI_REG(SENS_SAR_MEAS_START2_REG);
  }
  else
  {
    // Save Pin Registers : Do this before begin Wifi
    reg_b1 = READ_PERI_REG(SENS_SAR_START_FORCE_REG);
    reg_b2 = READ_PERI_REG(SENS_SAR_READ_CTRL2_REG);
    reg_b3 = READ_PERI_REG(SENS_SAR_MEAS_START2_REG);
  }
  
}


void MC_TrimMoisture(int nValue)  // 퍼센트
{
  if(nValue < 0)
    nValue = 0;

  else if(nValue > 100)
    nValue = 100;

  nMoistRate = nValue;
}
/*
void Fill_pump()  // 초기에 펌프를 움직여서 물을 채움
{
  for(int nIdx = 0 ; nIdx < 10 ; nIdx++)
  {
    if(nIdx % 2)
    {
      digitalWrite(PIN_SOL_VALVE, LOW);
      Serial.println(" Valve opened");
    }
    else
    {
      digitalWrite(PIN_SOL_VALVE, HIGH);
      Serial.println(" Valve closed");
    }

    delay(2000);
  }
}*/

/* 수분보충 */
bool  bPrevValve = 0;
bool  bValveOpen = false; // 밸브를 열지 말지 결정
int nSoilWater = 0;
int nSoilRate = 0;

void MC_Watered() {

  // Set the register-state that is not use wifi
//  WRITE_PERI_REG(SENS_SAR_START_FORCE_REG, reg_b1);  // Change to the state of None-Wifi
  WRITE_PERI_REG(SENS_SAR_READ_CTRL2_REG, reg_b2);
//  WRITE_PERI_REG(SENS_SAR_MEAS_START2_REG, reg_b3);

  nSoilWater = analogRead(PIN_MOIST_SENSOR);  // 물이 없을때 4095 / 물이 많을때 2100정도가 나옴

  // Set the register-state that is using wifi
//  WRITE_PERI_REG(SENS_SAR_START_FORCE_REG, reg_f1);  // Change to the state of Wifi
//  WRITE_PERI_REG(SENS_SAR_READ_CTRL2_REG, reg_f2);
//  WRITE_PERI_REG(SENS_SAR_MEAS_START2_REG, reg_f3);
  
  //nSoilRate = nSoilWater / VALUE_MOISTSENSOR_WET * 100;  // 센서의 최대값 대비 몇퍼센트인지  
  nSoilRate = map(nSoilWater, VALUE_MOISTSENSOR_WET, VALUE_MOISTSENSOR_DRY, 100, 0);  // 센서의 최대값 대비 몇퍼센트인지  

  bValveOpen = ((nSoilRate < nMoistRate) ? true : false);

  if(bValveOpen) // 흙이 건조함
      digitalWrite(PIN_SOL_VALVE, LOW);
  else
      digitalWrite(PIN_SOL_VALVE, HIGH);

  if(bPrevValve != bValveOpen)  // 밸브 상태가 변하면 디버깅 정보 출력
  {
    String strInfo = String(nSoilWater) + " / " + String(nSoilRate) + " / " + String(nMoistRate) + " : ";
    Serial.print(strInfo);
    
    if(bValveOpen) // 흙이 건조함
    {
        Serial.println("Valve opened");
    }
    else
    {
        Serial.println("Valve closed");
    }

    Serial.print(nMoistRate);
  }
      
  bPrevValve = bValveOpen;
  //delay(1000);
}
