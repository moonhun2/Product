
// Copyright 2015-2016 Espressif Systems (Shanghai) PTE LTD
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
#include "esp_http_server.h"
#include "esp_timer.h"
#include "esp_camera.h"
#include "img_converters.h"
#include "camera_index.h"
#include "Arduino.h"


#include "MoistureDrvCtrl.h"


extern String WiFiAddr;

#include <Preferences.h>
extern Preferences preferences; // APSetting.h에 선언된 객체 사용

String strMoistSetting;
String strError;
int nFPS = 0;

typedef struct {
        size_t size; //number of values used for filtering
        size_t index; //current value index
        size_t count; //value count
        int sum;
        int * values; //array to be filled with values
} ra_filter_t;

typedef struct {
        httpd_req_t *req;
        size_t len;
} jpg_chunking_t;

#define PART_BOUNDARY "123456789000000000000987654321"
static const char* _STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char* _STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char* _STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

static ra_filter_t ra_filter;
httpd_handle_t stream_httpd = NULL;
httpd_handle_t camera_httpd = NULL;

static ra_filter_t * ra_filter_init(ra_filter_t * filter, size_t sample_size){
    memset(filter, 0, sizeof(ra_filter_t));

    filter->values = (int *)malloc(sample_size * sizeof(int));
    if(!filter->values){
        return NULL;
    }
    memset(filter->values, 0, sample_size * sizeof(int));

    filter->size = sample_size;
    return filter;
}

static int ra_filter_run(ra_filter_t * filter, int value){
    if(!filter->values){
        return value;
    }
    filter->sum -= filter->values[filter->index];
    filter->values[filter->index] = value;
    filter->sum += filter->values[filter->index];
    filter->index++;
    filter->index = filter->index % filter->size;
    if (filter->count < filter->size) {
        filter->count++;
    }
    return filter->sum / filter->count;
}

static size_t jpg_encode_stream(void * arg, size_t index, const void* data, size_t len){
    jpg_chunking_t *j = (jpg_chunking_t *)arg;
    if(!index){
        j->len = 0;
    }
    if(httpd_resp_send_chunk(j->req, (const char *)data, len) != ESP_OK){
        return 0;
    }
    j->len += len;
    return len;
}

static esp_err_t capture_handler(httpd_req_t *req){
    camera_fb_t * fb = NULL;
    esp_err_t res = ESP_OK;
    int64_t fr_start = esp_timer_get_time();

    fb = esp_camera_fb_get();
    if (!fb) {
        Serial.printf("Camera capture failed");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture.jpg");

    size_t fb_len = 0;
    if(fb->format == PIXFORMAT_JPEG){
        fb_len = fb->len;
        res = httpd_resp_send(req, (const char *)fb->buf, fb->len);
    } else {
        jpg_chunking_t jchunk = {req, 0};
        res = frame2jpg_cb(fb, 80, jpg_encode_stream, &jchunk)?ESP_OK:ESP_FAIL;
        httpd_resp_send_chunk(req, NULL, 0);
        fb_len = jchunk.len;
    }
    esp_camera_fb_return(fb);
    int64_t fr_end = esp_timer_get_time();
    Serial.printf("JPG: %uB %ums", (uint32_t)(fb_len), (uint32_t)((fr_end - fr_start)/1000));
    return res;
}

void stopStreamServer();
void startStreamServer();

static esp_err_t stream_handler(httpd_req_t *req){
  
  Serial.println("stream_handler started");
  strError = "stream_started";
  
  camera_fb_t * fb = NULL;
  esp_err_t res = ESP_OK;
  size_t _jpg_buf_len = 0;
  uint8_t * _jpg_buf = NULL;
  char * part_buf[64];

  static int64_t last_frame = 0;
  if(!last_frame) {
      last_frame = esp_timer_get_time();
  }

  res = httpd_resp_set_type(req, _STREAM_CONTENT_TYPE);
  if(res != ESP_OK){
    strError = "httpd_resp_set_type failed";
    return res;
  }

  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*"); //Added.
  
  while(true){
      fb = esp_camera_fb_get();
      if (!fb) {
          Serial.printf("Camera capture failed");
          res = ESP_FAIL;
          strError = "Camera capture failed";
      } else {
          if(fb->format != PIXFORMAT_JPEG){
              bool jpeg_converted = frame2jpg(fb, 80, &_jpg_buf, &_jpg_buf_len);
              esp_camera_fb_return(fb);
              fb = NULL;
              if(!jpeg_converted){
                  Serial.printf("JPEG compression failed");
                  res = ESP_FAIL;
                  strError = "JPEG compression failed";
              }
          } else {
              _jpg_buf_len = fb->len;
              _jpg_buf = fb->buf;
          }
      }

      // Send header
      if(res == ESP_OK)
      {
          size_t hlen = snprintf((char *)part_buf, 64, _STREAM_PART, _jpg_buf_len);
          res = httpd_resp_send_chunk(req, (const char *)part_buf, hlen);

          delay(5);
          
          if(res != ESP_OK)
            strError = "Err to send Header " + String(hlen);
      }
      
      // Send Data
      if(res == ESP_OK)
      {
          res = httpd_resp_send_chunk(req, (const char *)_jpg_buf, _jpg_buf_len);

          delay(5);

          if(res == ESP_ERR_INVALID_ARG)
            strError = "ESP_ERR_INVALID_ARG " + String(_jpg_buf_len);
          else if(res == ESP_ERR_HTTPD_RESP_HDR)
            strError = "ESP_ERR_HTTPD_RESP_HDR " + String(_jpg_buf_len);
          else if(res == ESP_ERR_HTTPD_RESP_SEND)
            strError = "ESP_ERR_HTTPD_RESP_SEND " + String(_jpg_buf_len);
          else if(res == ESP_ERR_HTTPD_INVALID_REQ)
            strError = "ESP_ERR_HTTPD_INVALID_REQ " + String(_jpg_buf_len);
      }
      
      // Send Boundary
      if(res == ESP_OK)
      {
          res = httpd_resp_send_chunk(req, _STREAM_BOUNDARY, strlen(_STREAM_BOUNDARY));

          delay(5);
          
          if(res != ESP_OK)
            strError = "Err to send boundry ";
      }
      
      // free probe
      if(fb){
          esp_camera_fb_return(fb);
          fb = NULL;
          _jpg_buf = NULL;
      } else if(_jpg_buf){
          free(_jpg_buf);
          _jpg_buf = NULL;
      }
      
      if(res != ESP_OK){
          break;
      }
      
      int64_t fr_end = esp_timer_get_time();

      int64_t frame_time = fr_end - last_frame;
      last_frame = fr_end;
      frame_time /= 1000;
      uint32_t avg_frame_time = ra_filter_run(&ra_filter, frame_time);
      nFPS = 1000.0 / avg_frame_time;

      Serial.printf("MJPG: %uB %ums (%.1ffps), AVG: %ums (%.1ffps) : %d\n"
          ,(uint32_t)(_jpg_buf_len),
          (uint32_t)frame_time, 1000.0 / (uint32_t)frame_time,
          avg_frame_time, (float)nFPS,
          _jpg_buf
      );
      
  }

  Serial.println("\nExit stream_handler");
  
  last_frame = 0;

  return res;
}

static esp_err_t cmd_handler(httpd_req_t *req){
    char*  buf;
    size_t buf_len;
    char variable[32] = {0,};
    char value[32] = {0,};

    buf_len = httpd_req_get_url_query_len(req) + 1;
    if (buf_len > 1) {
        buf = (char*)malloc(buf_len);
        if(!buf){
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
        if (httpd_req_get_url_query_str(req, buf, buf_len) == ESP_OK) {
            if (httpd_query_key_value(buf, "var", variable, sizeof(variable)) == ESP_OK &&
                httpd_query_key_value(buf, "val", value, sizeof(value)) == ESP_OK) {
            } else {
                free(buf);
                httpd_resp_send_404(req);
                return ESP_FAIL;
            }
        } else {
            free(buf);
            httpd_resp_send_404(req);
            return ESP_FAIL;
        }
        free(buf);
    } else {
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }

    int val = atoi(value);
    sensor_t * s = esp_camera_sensor_get();
    int res = 0;

    if(!strcmp(variable, "framesize")) {
        if(s->pixformat == PIXFORMAT_JPEG) res = s->set_framesize(s, (framesize_t)val);
    }
    else if(!strcmp(variable, "quality")) res = s->set_quality(s, val);
    else if(!strcmp(variable, "contrast")) res = s->set_contrast(s, val);
    else if(!strcmp(variable, "brightness")) res = s->set_brightness(s, val);
    else if(!strcmp(variable, "saturation")) res = s->set_saturation(s, val);
    else if(!strcmp(variable, "gainceiling")) res = s->set_gainceiling(s, (gainceiling_t)val);
    else if(!strcmp(variable, "colorbar")) res = s->set_colorbar(s, val);
    else if(!strcmp(variable, "awb")) res = s->set_whitebal(s, val);
    else if(!strcmp(variable, "agc")) res = s->set_gain_ctrl(s, val);
    else if(!strcmp(variable, "aec")) res = s->set_exposure_ctrl(s, val);
    else if(!strcmp(variable, "hmirror")) res = s->set_hmirror(s, val);
    else if(!strcmp(variable, "vflip")) res = s->set_vflip(s, val);
    else if(!strcmp(variable, "awb_gain")) res = s->set_awb_gain(s, val);
    else if(!strcmp(variable, "agc_gain")) res = s->set_agc_gain(s, val);
    else if(!strcmp(variable, "aec_value")) res = s->set_aec_value(s, val);
    else if(!strcmp(variable, "aec2")) res = s->set_aec2(s, val);
    else if(!strcmp(variable, "dcw")) res = s->set_dcw(s, val);
    else if(!strcmp(variable, "bpc")) res = s->set_bpc(s, val);
    else if(!strcmp(variable, "wpc")) res = s->set_wpc(s, val);
    else if(!strcmp(variable, "raw_gma")) res = s->set_raw_gma(s, val);
    else if(!strcmp(variable, "lenc")) res = s->set_lenc(s, val);
    else if(!strcmp(variable, "special_effect")) res = s->set_special_effect(s, val);
    else if(!strcmp(variable, "wb_mode")) res = s->set_wb_mode(s, val);
    else if(!strcmp(variable, "ae_level")) res = s->set_ae_level(s, val);
    else {
        res = -1;
    }

    if(res){
        return httpd_resp_send_500(req);
    }

    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t status_handler(httpd_req_t *req){
    static char json_response[1024];

    sensor_t * s = esp_camera_sensor_get();
    char * p = json_response;
    *p++ = '{';

    p+=sprintf(p, "\"framesize\":%u,", s->status.framesize);
    p+=sprintf(p, "\"quality\":%u,", s->status.quality);
    p+=sprintf(p, "\"brightness\":%d,", s->status.brightness);
    p+=sprintf(p, "\"contrast\":%d,", s->status.contrast);
    p+=sprintf(p, "\"saturation\":%d,", s->status.saturation);
    p+=sprintf(p, "\"special_effect\":%u,", s->status.special_effect);
    p+=sprintf(p, "\"wb_mode\":%u,", s->status.wb_mode);
    p+=sprintf(p, "\"awb\":%u,", s->status.awb);
    p+=sprintf(p, "\"awb_gain\":%u,", s->status.awb_gain);
    p+=sprintf(p, "\"aec\":%u,", s->status.aec);
    p+=sprintf(p, "\"aec2\":%u,", s->status.aec2);
    p+=sprintf(p, "\"ae_level\":%d,", s->status.ae_level);
    p+=sprintf(p, "\"aec_value\":%u,", s->status.aec_value);
    p+=sprintf(p, "\"agc\":%u,", s->status.agc);
    p+=sprintf(p, "\"agc_gain\":%u,", s->status.agc_gain);
    p+=sprintf(p, "\"gainceiling\":%u,", s->status.gainceiling);
    p+=sprintf(p, "\"bpc\":%u,", s->status.bpc);
    p+=sprintf(p, "\"wpc\":%u,", s->status.wpc);
    p+=sprintf(p, "\"raw_gma\":%u,", s->status.raw_gma);
    p+=sprintf(p, "\"lenc\":%u,", s->status.lenc);
    p+=sprintf(p, "\"hmirror\":%u,", s->status.hmirror);
    p+=sprintf(p, "\"dcw\":%u,", s->status.dcw);
    p+=sprintf(p, "\"colorbar\":%u", s->status.colorbar);
    *p++ = '}';
    *p++ = 0;
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, json_response, strlen(json_response));
}

// setInterval(          function(){  getInform();  }   , 1000        );  // 주기적 호출을 일단 뺀다.
        
static esp_err_t index_handler(httpd_req_t *req){
  httpd_resp_set_type(req, "text/html");

  String page;

  page = R"(
  
  <html>
    <head>
      <meta name="viewport" content="width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=0">
      
      <script>
        var xhttp = new XMLHttpRequest();

        function getsend(arg) { 
          xhttp.open('GET', arg +'?' + new Date().getTime(), true); 
          xhttp.send(); 
        }

        
          
        function getInform() {    
          xhttp.onreadystatechange = function() {      
            if (this.readyState == 4 && this.status == 200) {  
              if(this.responseText != "OK")
                document.getElementById("ADCValue").innerHTML =        this.responseText;      
            }    
          };    
          
          xhttp.open("GET", "readADC", true);    xhttp.send();  
        }

        window.addEventListener('beforeunload', function (event) {
          stopStream();
        });


        document.addEventListener('DOMContentLoaded', function (event) {
          var baseHost = document.location.origin;
          var streamUrl = baseHost + ':81';
        
          const view = document.getElementById('stream');
          const viewContainer = document.getElementById('stream-container');
          const streamButton = document.getElementById('toggle-stream');
          const captureButton = document.getElementById('capture-image');
          
          function stopStream() {
            streamButton.innerHTML = 'Start Stream';
            view.src = ``;
            window.stop();
          }
        
          function startStream() {
            streamButton.innerHTML = 'Stop Stream';
            view.src = `${streamUrl}/stream`;
            show(viewContainer);
          }
        
          streamButton.onclick = function() {
            if (streamButton.innerHTML == 'Stop Stream') {
              stopStream();
            } else {
              startStream();
            }
          }

          captureButton.onclick = function() {
            view.src = `${streamUrl}/capture`;
            show(viewContainer);
          }
      
        }) 



        navigator.vibrate = navigator.vibrate || navigator.webkitVibrate || navigator.mozVibrate || navigator.msVibrate;
        
        function vibrate() {
          if (navigator.vibrate) 
            navigator.vibrate(20000); // 진동을 울리게 한다. 1000ms = 1초
          else
            alert("진동을 지원하지 않는 기종 입니다.");
        }
        
        function vibratearray() {
          if (navigator.vibrate) 
            navigator.vibrate([100,30,100,30,100,200,200,30,200,30,200,200,100,30,100,30,100]);
          else
            alert("진동을 지원하지 않는 기종 입니다.");
        }
        
        function stopAlarm() {
          navigator.vibrate(0);
        }

      </script>
    
    </head>
  
    <body>
  
      <p align=center>
            <div id='stream-container' style='text-align: center;' class='image-container hidden'>
                <img id='stream' src=''>
            </div>
      </p>
      
      <p align=center>
        <button id='capture-image' style=width:140px;height:40px;>Capture Image</button>
        <button id='toggle-stream' style=width:140px;height:40px;>Start Stream</button><br><br>
      </p>
      
      <p align=center>
        <button style=width:140px;height:40px; onmousedown=getsend('LON')><b>LIGHT ON</b></button>
        <button style=width:140px;height:40px; onmousedown=getsend('LOFF')><b>LIGHT OFF</b></button>
      </p>
      
      <p align=center>
        <button style=width:140px;height:40px; onmousedown=getsend('TL')><b>Trim Left</b></button>
        <button style=width:140px;height:40px; onmousedown=getsend('TR')><b>Trim Right</b></button>
      </p>

      <br>
      <button id='get_inform' style=width:140px;height:40px; onmousedown=getInform()>Information</button><br>
      Moisture : <span id="ADCValue">0</span><br>
      <button style=width:140px;height:40px; onmousedown=getsend('ResetCam')><b>ResetCam</b></button>

      <button style=width:140px;height:40px; onmousedown=vibrate()><b>Alarm</b></button>
      <button style=width:140px;height:40px; onmousedown=vibratearray()><b>SOS</b></button>
      <button style=width:140px;height:40px; onmousedown=stopAlarm()><b>Stop alarm</b></button>
    </body>
  
  </html>

  )";
  
  return httpd_resp_send(req, &page[0], strlen(&page[0]));
}


static esp_err_t LIGHT_on_handler(httpd_req_t *req){    digitalWrite(PIN_LED, HIGH);    Serial.println("Light ON");    httpd_resp_set_type(req, "text/html");    return httpd_resp_send(req, "OK", 2);  }
static esp_err_t LIGHT_off_handler(httpd_req_t *req){   digitalWrite(PIN_LED, LOW);     Serial.println("Light OFF");   httpd_resp_set_type(req, "text/html");    return httpd_resp_send(req, "OK", 2);  }

static esp_err_t TL_handler(httpd_req_t *req)
{   
  int nValue = preferences.getString("nTrimMoist").toInt();  nValue -= 1; strMoistSetting = String(nValue);
  preferences.putString("nTrimMoist", String(nValue));
  MC_TrimMoisture(nValue);
  Serial.println("TL");    httpd_resp_set_type(req, "text/html");    return httpd_resp_send(req, "OK", 2);   
}

static esp_err_t TR_handler(httpd_req_t *req)
{   
  int nValue = preferences.getString("nTrimMoist").toInt();  nValue += 1; strMoistSetting = String(nValue);
  preferences.putString("nTrimMoist", String(nValue));
  MC_TrimMoisture(nValue);
  Serial.println("TR");    httpd_resp_set_type(req, "text/html");    return httpd_resp_send(req, "OK", 2);   
}

static esp_err_t readADC_handler(httpd_req_t *req)
{   
  String strInfo = String(nSoilWater) + "/" + String(nSoilRate) + "/" + String(nMoistRate) + " <br>Valve : " + String(bValveOpen) + "<br> FPS : "  + String(nFPS) + "<br> State : "  + strError;
  httpd_resp_set_type(req, "text/html");    return httpd_resp_send(req, &strInfo[0], strlen(&strInfo[0]));
}

static esp_err_t ResetCam_handler(httpd_req_t *req)
{
  preferences.putString("pref_reset", "SW_Reset");  // SW 리셋이 진행중임을 기록
  ESP.restart();
  Serial.println("ResetCam");    httpd_resp_set_type(req, "text/html");    return httpd_resp_send(req, "ResetCam...", 11);  
}

/*
void stopStreamServer(){
  
    Serial.printf("Stopping stream server");
    if (stream_httpd) {
        httpd_stop(stream_httpd);
    }

}

void startStreamServer(){

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 10;
   httpd_uri_t stream_uri = {
        .uri       = "/stream",
        .method    = HTTP_GET,
        .handler   = stream_handler,
        .user_ctx  = NULL
    };

    config.server_port += 1;
    config.ctrl_port += 1;
    
    Serial.printf("Starting stream server on port: '%d'", config.server_port);
    if (httpd_start(&stream_httpd, &config) == ESP_OK) {
        httpd_register_uri_handler(stream_httpd, &stream_uri);
    }
}*/

void startCameraServer(){

    // motion trim-center initialize
    int nValue = preferences.getString("nTrimMoist").toInt(); strMoistSetting = String(nValue);
    
    preferences.putString("nTrimMoist", String(nValue));
    MC_TrimMoisture(nValue);


    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 10;

    
    
    httpd_uri_t LIGHT_on_uri = {
        .uri       = "/LON",
        .method    = HTTP_GET,
        .handler   = LIGHT_on_handler,
        .user_ctx  = NULL
    };
    
    httpd_uri_t LIGHT_off_uri = {
        .uri       = "/LOFF",
        .method    = HTTP_GET,
        .handler   = LIGHT_off_handler,
        .user_ctx  = NULL
    };

    httpd_uri_t TL_uri   = {        .uri       = "/TL",         .method    = HTTP_GET,        .handler   = TL_handler,         .user_ctx  = NULL };
    httpd_uri_t TR_uri   = {        .uri       = "/TR",         .method    = HTTP_GET,        .handler   = TR_handler,         .user_ctx  = NULL };


    httpd_uri_t readADC_uri = {
        .uri       = "/readADC",
        .method    = HTTP_GET,
        .handler   = readADC_handler,
        .user_ctx  = NULL
    };

    httpd_uri_t ResetCam_uri = {
        .uri       = "/ResetCam",
        .method    = HTTP_GET,
        .handler   = ResetCam_handler,
        .user_ctx  = NULL
    };


    httpd_uri_t index_uri = {
        .uri       = "/",
        .method    = HTTP_GET,
        .handler   = index_handler,
        .user_ctx  = NULL
    };
/*
    httpd_uri_t status_uri = {
        .uri       = "/status",
        .method    = HTTP_GET,
        .handler   = status_handler,
        .user_ctx  = NULL
    };

    httpd_uri_t cmd_uri = {
        .uri       = "/control",
        .method    = HTTP_GET,
        .handler   = cmd_handler,
        .user_ctx  = NULL
    };
*/
    
   
    ra_filter_init(&ra_filter, 20);
    Serial.printf("Starting web server on port: '%d'", config.server_port);
    if (httpd_start(&camera_httpd, &config) == ESP_OK) {
        httpd_register_uri_handler(camera_httpd, &index_uri);

        httpd_register_uri_handler(camera_httpd, &LIGHT_on_uri);
        httpd_register_uri_handler(camera_httpd, &LIGHT_off_uri);
        
        httpd_register_uri_handler(camera_httpd, &TL_uri);
        httpd_register_uri_handler(camera_httpd, &TR_uri);

        httpd_register_uri_handler(camera_httpd, &readADC_uri);

        httpd_register_uri_handler(camera_httpd, &ResetCam_uri);
    }

    //startStreamServer();
    
    httpd_uri_t stream_uri = {
        .uri       = "/stream",
        .method    = HTTP_GET,
        .handler   = stream_handler,
        .user_ctx  = NULL
    };

    httpd_uri_t capture_uri = {
        .uri       = "/capture",
        .method    = HTTP_GET,
        .handler   = capture_handler,
        .user_ctx  = NULL
    };

    config.server_port += 1;
    config.ctrl_port += 1;
    
    Serial.printf("Starting stream server on port: '%d' '%d'", config.server_port, config.ctrl_port);
    if (httpd_start(&stream_httpd, &config) == ESP_OK) {
        httpd_register_uri_handler(stream_httpd, &stream_uri);
        httpd_register_uri_handler(stream_httpd, &capture_uri);
    }

}
