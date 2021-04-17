#include <SPIFFS.h>

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



#include "MotorDrvCtrl.h"


extern String WiFiAddr;

#include <Preferences.h>
extern Preferences preferences; // APSetting.h에 선언된 객체 사용

extern void MC_TrimCenter(int nValue);



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

static esp_err_t stream_handler(httpd_req_t *req){
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
        return res;
    }

    while(true){
        fb = esp_camera_fb_get();
        if (!fb) {
            Serial.printf("Camera capture failed");
            res = ESP_FAIL;
        } else {
            if(fb->format != PIXFORMAT_JPEG){
                bool jpeg_converted = frame2jpg(fb, 80, &_jpg_buf, &_jpg_buf_len);
                esp_camera_fb_return(fb);
                fb = NULL;
                if(!jpeg_converted){
                    Serial.printf("JPEG compression failed");
                    res = ESP_FAIL;
                }
            } else {
                _jpg_buf_len = fb->len;
                _jpg_buf = fb->buf;
            }
        }
        if(res == ESP_OK){
            size_t hlen = snprintf((char *)part_buf, 64, _STREAM_PART, _jpg_buf_len);
            res = httpd_resp_send_chunk(req, (const char *)part_buf, hlen);
        }
        if(res == ESP_OK){
            res = httpd_resp_send_chunk(req, (const char *)_jpg_buf, _jpg_buf_len);
        }
        if(res == ESP_OK){
            res = httpd_resp_send_chunk(req, _STREAM_BOUNDARY, strlen(_STREAM_BOUNDARY));
        }
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
        Serial.printf("MJPG: %uB %ums (%.1ffps), AVG: %ums (%.1ffps)\n"
            ,(uint32_t)(_jpg_buf_len),
            (uint32_t)frame_time, 1000.0 / (uint32_t)frame_time,
            avg_frame_time, 1000.0 / avg_frame_time
        );

        delay(20);
    }

    last_frame = 0;
    return res;
}
/*
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
*/
static esp_err_t index_handler(httpd_req_t *req){
  httpd_resp_set_type(req, "text/html");
  String page;

  page = R"(  
  
  <html>
    <head>
      <meta name="viewport" content="width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=0">
      
      <script>
        var xhttp = new XMLHttpRequest();
      </script>
      
      <script>
        function getsend(arg) { xhttp.open('GET', arg +'?' + new Date().getTime(), true); xhttp.send(); }
      </script>
    
      <script>
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
            const streamEnabled = streamButton.innerHTML === 'Stop Stream';
            if (streamEnabled) {
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
      </script>
  
    
    </head>

    <body>
  
      <p align=center>
            <div id='stream-container'  style='text-align: center;  height:300px; ' class='image-container hidden'>
                <img id='stream' src='' style='width:350px; height:300px; transform:rotate(90deg)'>
            </div>
      </p>
      
      <p align=center>
        <button id='capture-image' style=style=background-color:lightgrey;width:140px;height:40px;>Capture Image</button>
        <button id='toggle-stream' style=style=background-color:lightgrey;width:140px;height:40px;>Start Stream</button>
      </p>
      
     
      <p align=center>
        <button style=width:90px;height:60px; onmousedown=getsend('LF')  ontouchstart=getsend('LF')><b>LF</b></button>&nbsp;
        <button style=width:90px;height:60px  onmousedown=getsend('F')   ontouchstart=getsend('F')><b>F</b></button>&nbsp;
        <button style=width:90px;height:60px  onmousedown=getsend('RF')  ontouchstart=getsend('RF')><b>RF</b></button>
      </p>
      
      <p align=center>
        <button style=width:90px;height:60px; onmousedown=getsend('L')  ontouchstart=getsend('L')><b>L</b></button>&nbsp;
        <button style=background-color:red;width:90px;height:60px  onmousedown=getsend('S') onmouseup=getsend('S')><b>S</b></button>&nbsp;
        <button style=width:90px;height:60px  onmousedown=getsend('R')  ontouchstart=getsend('R')><b>R</b></button>
      </p>
      
      <p align=center>
        <button style=width:90px;height:60px; onmousedown=getsend('LB')  ontouchstart=getsend('LB')><b>LB</b></button>&nbsp;
        <button style=width:90px;height:60px  onmousedown=getsend('B')   ontouchstart=getsend('B')><b>B</b></button>&nbsp;
        <button style=width:90px;height:60px  onmousedown=getsend('RB')  ontouchstart=getsend('RB')><b>RB</b></button>
      </p>
      
      <p align=center>
        <button style=background-color:yellow;width:140px;height:40px onmousedown=getsend('LON')><b>LIGHT ON</b></button>
        <button style=background-color:yellow;width:140px;height:40px onmousedown=getsend('LOFF')><b>LIGHT OFF</b></button>
      </p>
      
      <p align=center>
        <button style=background-color:yellow;width:140px;height:40px onmousedown=getsend('TL')><b>Trim Left</b></button>
        <button style=background-color:yellow;width:140px;height:40px onmousedown=getsend('TR')><b>Trim Right</b></button>
      </p>
    
    </body>
  
  </html>

  )";
  
  return httpd_resp_send(req, &page[0], strlen(&page[0]));
}

static esp_err_t LF_handler(httpd_req_t *req){   MC_Move(1);    Serial.println("LF");    httpd_resp_set_type(req, "text/html");    return httpd_resp_send(req, "OK", 2);   }
static esp_err_t F_handler(httpd_req_t *req){    MC_Move(2);    Serial.println("F ");    httpd_resp_set_type(req, "text/html");    return httpd_resp_send(req, "OK", 2);   }
static esp_err_t RF_handler(httpd_req_t *req){   MC_Move(3);    Serial.println("RF");    httpd_resp_set_type(req, "text/html");    return httpd_resp_send(req, "OK", 2);   }
static esp_err_t L_handler(httpd_req_t *req){    MC_Move(4);    Serial.println("L ");    httpd_resp_set_type(req, "text/html");    return httpd_resp_send(req, "OK", 2);   }
static esp_err_t S_handler(httpd_req_t *req){    MC_Move(5);    Serial.println("S ");    httpd_resp_set_type(req, "text/html");    return httpd_resp_send(req, "OK", 2);   }
static esp_err_t R_handler(httpd_req_t *req){    MC_Move(6);    Serial.println("R ");    httpd_resp_set_type(req, "text/html");    return httpd_resp_send(req, "OK", 2);   }
static esp_err_t LB_handler(httpd_req_t *req){   MC_Move(7);    Serial.println("LB");    httpd_resp_set_type(req, "text/html");    return httpd_resp_send(req, "OK", 2);   }
static esp_err_t B_handler(httpd_req_t *req){    MC_Move(8);    Serial.println("B ");    httpd_resp_set_type(req, "text/html");    return httpd_resp_send(req, "OK", 2);   }
static esp_err_t RB_handler(httpd_req_t *req){   MC_Move(9);    Serial.println("RB");    httpd_resp_set_type(req, "text/html");    return httpd_resp_send(req, "OK", 2);   }

static esp_err_t LIGHT_on_handler(httpd_req_t *req){    digitalWrite(PIN_LED, HIGH);    Serial.println("Light ON");    httpd_resp_set_type(req, "text/html");    return httpd_resp_send(req, "OK", 2);  }
static esp_err_t LIGHT_off_handler(httpd_req_t *req){   digitalWrite(PIN_LED, LOW);     Serial.println("Light OFF");   httpd_resp_set_type(req, "text/html");    return httpd_resp_send(req, "OK", 2);  }

static esp_err_t LFjpeg_handler(httpd_req_t *req){    
  Serial.println("LFjpeg");    
  File f = SPIFFS.open("/LF.jpg", "r");
  int filesize = f.size();
  char* buf = new char[filesize+1];
  f.read((uint8_t *)buf, filesize);
  
  httpd_resp_set_type(req, "image/jpg");    
  httpd_resp_send_chunk(req, (const char *)buf, filesize); 

  delete [] buf;
 }



static esp_err_t TL_handler(httpd_req_t *req){   
  int nValue = preferences.getString("nTrimCenter").toInt();  nValue -= 1;
  preferences.putString("nTrimCenter", String(nValue));
  MC_TrimCenter(nValue);
  Serial.println("TL");    httpd_resp_set_type(req, "text/html");    return httpd_resp_send(req, "OK", 2);   }
  
static esp_err_t TR_handler(httpd_req_t *req){   
  int nValue = preferences.getString("nTrimCenter").toInt();  nValue += 1;
  preferences.putString("nTrimCenter", String(nValue));
  MC_TrimCenter(nValue);
  Serial.println("TR");    httpd_resp_set_type(req, "text/html");    return httpd_resp_send(req, "OK", 2);   }


void startCameraServer(){
  // Initialize SPIFFS
  if(!SPIFFS.begin(true)){
    Serial.println("An Error has occurred while mounting SPIFFS");
    return;
  }
  
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 20;

    
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

    httpd_uri_t LF_uri  = {        .uri       = "/LF",          .method    = HTTP_GET,        .handler   = LF_handler,         .user_ctx  = NULL  };
    httpd_uri_t F_uri    = {        .uri       = "/F",          .method    = HTTP_GET,        .handler   = F_handler,          .user_ctx  = NULL };
    httpd_uri_t RF_uri   = {        .uri       = "/RF",         .method    = HTTP_GET,        .handler   = RF_handler,         .user_ctx  = NULL };
    httpd_uri_t L_uri    = {        .uri       = "/L",          .method    = HTTP_GET,        .handler   = L_handler,          .user_ctx  = NULL };
    httpd_uri_t S_uri    = {        .uri       = "/S",          .method    = HTTP_GET,        .handler   = S_handler,          .user_ctx  = NULL };
    httpd_uri_t R_uri    = {        .uri       = "/R",          .method    = HTTP_GET,        .handler   = R_handler,          .user_ctx  = NULL };
    httpd_uri_t LB_uri   = {        .uri       = "/LB",         .method    = HTTP_GET,        .handler   = LB_handler,         .user_ctx  = NULL };
    httpd_uri_t B_uri    = {        .uri       = "/B",          .method    = HTTP_GET,        .handler   = B_handler,          .user_ctx  = NULL };
    httpd_uri_t RB_uri   = {        .uri       = "/RB",         .method    = HTTP_GET,        .handler   = RB_handler,         .user_ctx  = NULL };

    httpd_uri_t TL_uri   = {        .uri       = "/TL",         .method    = HTTP_GET,        .handler   = TL_handler,         .user_ctx  = NULL };
    httpd_uri_t TR_uri   = {        .uri       = "/TR",         .method    = HTTP_GET,        .handler   = TR_handler,         .user_ctx  = NULL };


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
    httpd_uri_t capture_uri = {
        .uri       = "/capture",
        .method    = HTTP_GET,
        .handler   = capture_handler,
        .user_ctx  = NULL
    };

   httpd_uri_t stream_uri = {
        .uri       = "/stream",
        .method    = HTTP_GET,
        .handler   = stream_handler,
        .user_ctx  = NULL
    };

   //httpd_uri_t LFjpeg_uri = {        .uri       = "/LFjpeg",        .method    = HTTP_GET,        .handler   = LFjpeg_handler,        .user_ctx  = NULL    };
   


    ra_filter_init(&ra_filter, 20);
    Serial.printf("Starting web server on port: '%d'", config.server_port);
    if (httpd_start(&camera_httpd, &config) == ESP_OK) {
        httpd_register_uri_handler(camera_httpd, &index_uri);

        httpd_register_uri_handler(camera_httpd, &LIGHT_on_uri);
        httpd_register_uri_handler(camera_httpd, &LIGHT_off_uri);
        
        httpd_register_uri_handler(camera_httpd, &LF_uri);
        httpd_register_uri_handler(camera_httpd, &F_uri);
        httpd_register_uri_handler(camera_httpd, &RF_uri);
        httpd_register_uri_handler(camera_httpd, &L_uri);
        httpd_register_uri_handler(camera_httpd, &S_uri);
        httpd_register_uri_handler(camera_httpd, &R_uri);
        httpd_register_uri_handler(camera_httpd, &LB_uri);
        httpd_register_uri_handler(camera_httpd, &B_uri);
        httpd_register_uri_handler(camera_httpd, &RB_uri);

        httpd_register_uri_handler(camera_httpd, &TL_uri);
        httpd_register_uri_handler(camera_httpd, &TR_uri);
    }

    config.server_port += 1;
    config.ctrl_port += 1;
    Serial.printf("Starting stream server on port: '%d'", config.server_port);
    if (httpd_start(&stream_httpd, &config) == ESP_OK) {
        httpd_register_uri_handler(stream_httpd, &stream_uri);
        httpd_register_uri_handler(stream_httpd, &capture_uri);
    }

    // motion trim-center initialize
    int nValue = preferences.getString("nTrimCenter").toInt();
    preferences.putString("nTrimCenter", String(nValue));
    MC_TrimCenter(nValue);
}
