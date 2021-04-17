#include <Servo.h>

#include <Preferences.h>
extern Preferences preferences; // APSetting.h에 선언된 객체 사용

float fRateL = 1; //100 %
float fRateR = 1;

  
// 서보 이동 각도   LF    FRONT RF    LEFT  STOP  RIGHT   LB    BACK  RB
int nLeft[]   = { 30,   80,  80,   -80,   0,    80,     -30,  -80,  -80};   // +가 전진 / -가 후진
int nRight[]  = { -80, -80,  -30,  -80,   0,    80,     80,   80,   30  };  // -가 전진 / +가 후진

#define PIN_SERVO_L 14
#define PIN_SERVO_R 15

#define PIN_LED 4



Servo servo_Left;                   // 서보 컨트롤 객체 (변수)
Servo servo_Right;

/* 서보관련 초기화  */
void MC_Init() {
  pinMode(PIN_LED, OUTPUT); //Light

  //initialize
  digitalWrite(PIN_LED, LOW);

  servo_Left.attach(PIN_SERVO_L, 2);
  servo_Right.attach(PIN_SERVO_R, 3);
  
}

// 넘어오는값은 도는 속도 100%에서 얼마나 빼줄지에 대한 백분률 값
void MC_TrimCenter(int nValue)  // 값이 -이면 왼쪽의 100%에서 빼주고 +이면 오른쪽 100%에서 빼줌
{
  //int nValue = preferences.getString("nTrimCenter").toInt();
  if(nValue < 0)
    fRateL = 1.0f - (float)abs(nValue) / 100.0f;

  else if(nValue > 0)
    fRateR = 1.0f - (float)abs(nValue) / 100.0f;

  Serial.print("nValue : ");
  Serial.println(nValue);
  
  Serial.print("fRateL : ");
  Serial.println(fRateL);
  Serial.print("fRateR : ");
  Serial.println(fRateR);
  
}

/* 명령에 따른 서보 회전 */
void MC_Move(int nCmdMove) {
  
  servo_Left.write(90+(float)nLeft[nCmdMove-1]* fRateL);
  servo_Right.write(90+(float)nRight[nCmdMove-1]* fRateR);
  
  Serial.print("Left Move:");
  Serial.print(90+(float)nLeft[nCmdMove-1]* fRateL);
  Serial.print("Right Move:");
  Serial.println(90+(float)nRight[nCmdMove-1]* fRateR);

  delay(10);
}
