#include <esp_log.h>
#include <esp_system.h>
#include <nvs_flash.h>
#include <sys/param.h>
#include <string.h>
#include <errno.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "img_converters.h"  // For fmt2jpg function

#ifndef portTICK_RATE_MS
#define portTICK_RATE_MS portTICK_PERIOD_MS
#endif

#include "esp_camera.h"
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "driver/ledc.h"
#include "sensor.h"  // 摄像头传感器头文件

// AI-Thinker ESP32-CAM pin configuration
#define CAM_PIN_PWDN 32
#define CAM_PIN_RESET -1
#define CAM_PIN_XCLK 0
#define CAM_PIN_SIOD 26
#define CAM_PIN_SIOC 27
#define CAM_PIN_D7 35
#define CAM_PIN_D6 34
#define CAM_PIN_D5 39
#define CAM_PIN_D4 36
#define CAM_PIN_D3 21
#define CAM_PIN_D2 19
#define CAM_PIN_D1 18
#define CAM_PIN_D0 5
#define CAM_PIN_VSYNC 25
#define CAM_PIN_HREF 23
#define CAM_PIN_PCLK 22

static const char *TAG = "example:camera_wifi";

static const size_t JPEG_CHUNK_SIZE = 1024;
static const int MAX_CHUNK_RETRIES = 5;
static const char *CAPTURE_MARKER = "CAPTURE_HANDLER_V2";

// 共享帧缓冲区用于并发流和抓拍
static camera_fb_t *g_last_frame = NULL;
static SemaphoreHandle_t g_frame_mutex = NULL;

#if ESP_CAMERA_SUPPORTED
static camera_config_t camera_config = {
    .pin_pwdn = CAM_PIN_PWDN,
    .pin_reset = CAM_PIN_RESET,
    .pin_xclk = CAM_PIN_XCLK,
    .pin_sccb_sda = CAM_PIN_SIOD,
    .pin_sccb_scl = CAM_PIN_SIOC,
    .pin_d7 = CAM_PIN_D7,
    .pin_d6 = CAM_PIN_D6,
    .pin_d5 = CAM_PIN_D5,
    .pin_d4 = CAM_PIN_D4,
    .pin_d3 = CAM_PIN_D3,
    .pin_d2 = CAM_PIN_D2,
    .pin_d1 = CAM_PIN_D1,
    .pin_d0 = CAM_PIN_D0,
    .pin_vsync = CAM_PIN_VSYNC,
    .pin_href = CAM_PIN_HREF,
    .pin_pclk = CAM_PIN_PCLK,
    .xclk_freq_hz = 20000000,  // 提高到 20MHz 以获得更好的帧率
    .ledc_timer = LEDC_TIMER_0,
    .ledc_channel = LEDC_CHANNEL_0,
    .pixel_format = PIXFORMAT_JPEG,
    .frame_size = FRAMESIZE_VGA,  // 从 SVGA 降到 VGA 以提高帧率
    .jpeg_quality = 12,  // 从 10 提高到 12 以获得更流畅的视频
    .fb_count = 4,
    .fb_location = CAMERA_FB_IN_PSRAM,
    .grab_mode = CAMERA_GRAB_WHEN_EMPTY,
    .sccb_i2c_port = 1,
};

// SCCB 测试函数：确认摄像头上电、时钟就绪并能响应 I2C
static esp_err_t test_sccb_communication(void)
{
    ESP_LOGI(TAG, "=== Testing SCCB/I2C Communication ===");
    
    // 先检查摄像头电源控制
    ESP_LOGI(TAG, "Checking camera power control...");
    gpio_config_t pwr_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << CAM_PIN_PWDN),
        .pull_down_en = 0,
        .pull_up_en = 0,
    };
    gpio_config(&pwr_conf);
    
    // PWDN 拉低以启用摄像头（OV2640: PWDN=0 表示工作，PWDN=1 表示断电）
    gpio_set_level(CAM_PIN_PWDN, 0);
    ESP_LOGI(TAG, "Camera PWDN set to LOW (camera enabled)");
    vTaskDelay(pdMS_TO_TICKS(10));
    
    // 启动 XCLK 时钟（摄像头需要时钟才能响应 I2C）
    ESP_LOGI(TAG, "Starting XCLK clock for camera...");
    ledc_timer_config_t ledc_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = LEDC_TIMER_0,
        .duty_resolution = LEDC_TIMER_1_BIT,
        .freq_hz = 20000000,  // 20MHz
        .clk_cfg = LEDC_AUTO_CLK
    };
    ledc_timer_config(&ledc_timer);
    
    ledc_channel_config_t ledc_channel = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .intr_type = LEDC_INTR_DISABLE,
        .gpio_num = CAM_PIN_XCLK,
        .duty = 1,
        .hpoint = 0
    };
    ledc_channel_config(&ledc_channel);
    ESP_LOGI(TAG, "XCLK clock started at 20MHz on GPIO%d", CAM_PIN_XCLK);
    
    vTaskDelay(pdMS_TO_TICKS(100));  // 等待摄像头启动
    
    // 先测试 GPIO 引脚本身
    ESP_LOGI(TAG, "Testing GPIO pins...");
    
    ESP_LOGI(TAG, "检测到外部上拉电阻，使用输入模式测试");
    
    // 使用输入模式测试（配合外部上拉）
    gpio_config_t io_conf_input = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = ((1ULL << CAM_PIN_SIOD) | (1ULL << CAM_PIN_SIOC)),
        .pull_down_en = 0,
        .pull_up_en = 0,  // 使用外部上拉
    };
    gpio_config(&io_conf_input);
    vTaskDelay(pdMS_TO_TICKS(20));
    
    int sda_idle = gpio_get_level(CAM_PIN_SIOD);
    int scl_idle = gpio_get_level(CAM_PIN_SIOC);
    
    ESP_LOGI(TAG, "GPIO idle state (with external pull-ups) - SDA: %d, SCL: %d", sda_idle, scl_idle);
    
    if (sda_idle != 1 || scl_idle != 1) {
        ESP_LOGE(TAG, "外部上拉电阻未工作！检查：");
        ESP_LOGE(TAG, "  1. 上拉电阻是否连接到 3.3V（不是 GND）");
        ESP_LOGE(TAG, "  2. 摄像头是否将 I2C 引脚拉低");
        ESP_LOGE(TAG, "  3. 上拉电阻阻值是否正确（应该是 4.7kΩ，不是 470Ω）");
    }
    
    // 初始化 I2C（使用外部上拉）
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = CAM_PIN_SIOD,
        .scl_io_num = CAM_PIN_SIOC,
        .sda_pullup_en = GPIO_PULLUP_DISABLE,  // 禁用内部上拉，使用外部上拉
        .scl_pullup_en = GPIO_PULLUP_DISABLE,  // 禁用内部上拉，使用外部上拉
        .master.clk_speed = 50000,  // 50kHz
    };
    
    ESP_LOGI(TAG, "使用外部上拉电阻（4.7kΩ）");
    
    esp_err_t ret = i2c_param_config(I2C_NUM_1, &conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C config failed: 0x%x", ret);
        return ret;
    }
    
    ret = i2c_driver_install(I2C_NUM_1, conf.mode, 0, 0, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C driver install failed: 0x%x", ret);
        return ret;
    }
    
    ESP_LOGI(TAG, "I2C initialized at 50kHz");
    
    // 扫描 I2C 设备
    ESP_LOGI(TAG, "Scanning I2C bus...");
    ESP_LOGI(TAG, "Trying common camera addresses: 0x21, 0x30, 0x3C, 0x60...");
    int devices_found = 0;
    
    // 先尝试常见摄像头地址
    uint8_t camera_addrs[] = {0x21, 0x30, 0x3C, 0x60};
    for (int i = 0; i < 4; i++) {
        uint8_t addr = camera_addrs[i];
        i2c_cmd_handle_t cmd = i2c_cmd_link_create();
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, true);
        i2c_master_stop(cmd);
        
        ret = i2c_master_cmd_begin(I2C_NUM_1, cmd, pdMS_TO_TICKS(100));
        i2c_cmd_link_delete(cmd);
        
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "*** Found camera at address: 0x%02x ***", addr);
            devices_found++;
        } else {
            ESP_LOGD(TAG, "No response from 0x%02x (ret: 0x%x)", addr, ret);
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    
    // 完整扫描
    if (devices_found == 0) {
        ESP_LOGI(TAG, "No camera found at common addresses, doing full scan...");
        for (uint8_t addr = 1; addr < 127; addr++) {
            i2c_cmd_handle_t cmd = i2c_cmd_link_create();
            i2c_master_start(cmd);
            i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, true);
            i2c_master_stop(cmd);
            
            ret = i2c_master_cmd_begin(I2C_NUM_1, cmd, pdMS_TO_TICKS(100));
            i2c_cmd_link_delete(cmd);
            
            if (ret == ESP_OK) {
                ESP_LOGI(TAG, "Found I2C device at address: 0x%02x", addr);
                devices_found++;
            }
            
            // 如果是 OV2640 地址，尝试读取 PID
            if (ret == ESP_OK && addr == 0x30) {
                uint8_t pid_h = 0, pid_l = 0;
                
                // 读取 PID 高字节 (寄存器 0x0A)
                cmd = i2c_cmd_link_create();
                i2c_master_start(cmd);
                i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, true);
                i2c_master_write_byte(cmd, 0x0A, true);
                i2c_master_start(cmd);
                i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_READ, true);
                i2c_master_read_byte(cmd, &pid_h, I2C_MASTER_NACK);
                i2c_master_stop(cmd);
                ret = i2c_master_cmd_begin(I2C_NUM_1, cmd, pdMS_TO_TICKS(100));
                i2c_cmd_link_delete(cmd);
                
                if (ret == ESP_OK) {
                    ESP_LOGI(TAG, "Camera PID high byte: 0x%02x", pid_h);
                    
                    // 读取 PID 低字节 (寄存器 0x0B)
                    cmd = i2c_cmd_link_create();
                    i2c_master_start(cmd);
                    i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, true);
                    i2c_master_write_byte(cmd, 0x0B, true);
                    i2c_master_start(cmd);
                    i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_READ, true);
                    i2c_master_read_byte(cmd, &pid_l, I2C_MASTER_NACK);
                    i2c_master_stop(cmd);
                    ret = i2c_master_cmd_begin(I2C_NUM_1, cmd, pdMS_TO_TICKS(100));
                    i2c_cmd_link_delete(cmd);
                    
                    if (ret == ESP_OK) {
                        ESP_LOGI(TAG, "Camera PID low byte: 0x%02x", pid_l);
                        ESP_LOGI(TAG, "Camera PID: 0x%02x%02x (Expected: 0x2642 for OV2640)", pid_h, pid_l);
                    }
                }
            }
            
            vTaskDelay(pdMS_TO_TICKS(2));
        }
    }
    
    ESP_LOGI(TAG, "I2C scan complete. Found %d device(s)", devices_found);
    
    // 卸载 I2C 驱动（摄像头驱动会重新初始化）
    i2c_driver_delete(I2C_NUM_1);
    
    if (devices_found == 0) {
        ESP_LOGE(TAG, "No I2C devices found! Check:");
        ESP_LOGE(TAG, "  1. Camera cable connection");
        ESP_LOGE(TAG, "  2. Pull-up resistors (4.7k on SDA/SCL)");
        ESP_LOGE(TAG, "  3. Power supply (need 5V/2A)");
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "=== SCCB Test Passed ===");
    return ESP_OK;
}

// 初始化摄像头，包含上电时序与默认图像参数
static esp_err_t init_camera(void)
{
    // 先测试 SCCB 通信
    esp_err_t ret = test_sccb_communication();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SCCB test failed, camera initialization aborted");
        return ret;
    }
    
    // 手动复位摄像头电源
    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = (1ULL << CAM_PIN_PWDN);
    io_conf.pull_down_en = 0;
    io_conf.pull_up_en = 0;
    gpio_config(&io_conf);
    
    ESP_LOGI(TAG, "Power cycling camera...");
    // 电源复位序列：先关闭再打开
    gpio_set_level(CAM_PIN_PWDN, 1);  // 关闭摄像头电源
    vTaskDelay(300 / portTICK_PERIOD_MS);
    gpio_set_level(CAM_PIN_PWDN, 0);  // 打开摄像头电源
    vTaskDelay(300 / portTICK_PERIOD_MS);
    
    ESP_LOGI(TAG, "Initializing camera with SCCB freq 50kHz...");
    ESP_LOGI(TAG, "Camera pins: SIOD=%d SIOC=%d PWDN=%d", CAM_PIN_SIOD, CAM_PIN_SIOC, CAM_PIN_PWDN);
    
    esp_err_t err = esp_camera_init(&camera_config);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Camera Init Failed: 0x%x", err);
        ESP_LOGE(TAG, "Check: 1) Power supply (need 5V/2A) 2) Camera cable connection 3) Try external 5V power");
        return err;
    }
    
    ESP_LOGI(TAG, "Camera initialized successfully!");
    
    // 获取传感器对象并调整设置
    sensor_t *s = esp_camera_sensor_get();
    if (s != NULL) {
        ESP_LOGI(TAG, "Adjusting camera sensor settings...");
        
        // 调整曝光和增益
        s->set_brightness(s, 0);     // 亮度：-2 到 2，0 为默认
        s->set_contrast(s, 0);       // 对比度：-2 到 2
        s->set_saturation(s, 0);     // 饱和度：-2 到 2
        s->set_special_effect(s, 0); // 特效：0=无
        s->set_whitebal(s, 1);       // 白平衡：1=开启
        s->set_awb_gain(s, 1);       // 自动白平衡增益：1=开启
        s->set_wb_mode(s, 0);        // 白平衡模式：0=自动
        s->set_exposure_ctrl(s, 1);  // 自动曝光：1=开启
        s->set_aec2(s, 0);           // 自动曝光DSP：0=关闭以避免过曝
        s->set_ae_level(s, 0);       // 曝光级别：-2 到 2，0 为默认
        s->set_aec_value(s, 300);    // 手动曝光值：0-1200
        s->set_gain_ctrl(s, 1);      // 自动增益：1=开启
        s->set_agc_gain(s, 0);       // 增益：0-30
        s->set_gainceiling(s, (gainceiling_t)2); // 增益上限：0-6
        s->set_bpc(s, 0);            // 黑点补偿：0=关闭
        s->set_wpc(s, 1);            // 白点补偿：1=开启
        s->set_raw_gma(s, 1);        // Gamma校正：1=开启
        s->set_lenc(s, 1);           // 镜头校正：1=开启
        s->set_hmirror(s, 0);        // 水平镜像：0=关闭
        s->set_vflip(s, 0);          // 垂直翻转：0=关闭
        s->set_dcw(s, 1);            // 下采样：1=开启
        s->set_colorbar(s, 0);       // 彩条测试：0=关闭
        
        ESP_LOGI(TAG, "Camera sensor settings adjusted");
    } else {
        ESP_LOGW(TAG, "Failed to get camera sensor");
    }
    
    return ESP_OK;
}
#endif

// 处理单帧抓拍请求，返回一张 JPEG 图片
static esp_err_t send_jpeg_chunked(httpd_req_t *req, const uint8_t *data, size_t length)
{
    size_t remaining = length;
    const uint8_t *ptr = data;

    while (remaining > 0) {
        size_t to_send = remaining > JPEG_CHUNK_SIZE ? JPEG_CHUNK_SIZE : remaining;
        int retries = 0;
        esp_err_t res;

        do {
            res = httpd_resp_send_chunk(req, (const char *)ptr, to_send);
            if (res == ESP_OK) {
                break;
            }

            if ((errno == EAGAIN || errno == EWOULDBLOCK) && (++retries <= MAX_CHUNK_RETRIES)) {
                ESP_LOGW(TAG, "Chunk send EAGAIN, retry %d/%d", retries, MAX_CHUNK_RETRIES);
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }

            ESP_LOGE(TAG, "[%s] Chunk send failed: %s (errno=%d, remaining=%u)", CAPTURE_MARKER, esp_err_to_name(res), errno, remaining);
            return res;
        } while (retries <= MAX_CHUNK_RETRIES);

        ptr += to_send;
        remaining -= to_send;
    }

    return httpd_resp_send_chunk(req, NULL, 0);
}

// 处理单帧抓拍请求,返回一张 JPEG 图片（直接获取新帧，带重试机制）
static esp_err_t jpg_httpd_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "[%s] *** CAPTURE REQUEST RECEIVED ***", CAPTURE_MARKER);
    ESP_LOGI(TAG, "[%s] Free heap: %lu bytes", CAPTURE_MARKER, (unsigned long)esp_get_free_heap_size());
    
    // 简单直接：每次都获取新帧，带重试机制
    camera_fb_t *fb = NULL;
    int attempts = 0;
    const int max_attempts = 10;
    
    while (!fb && attempts < max_attempts) {
        fb = esp_camera_fb_get();
        if (!fb) {
            ESP_LOGW(TAG, "[%s] Camera capture failed, retry %d/%d", CAPTURE_MARKER, attempts + 1, max_attempts);
            vTaskDelay(pdMS_TO_TICKS(50));
            attempts++;
        }
    }
    
    if (!fb) {
        ESP_LOGE(TAG, "[%s] Camera capture failed after %d attempts", CAPTURE_MARKER, max_attempts);
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_set_status(req, "503 Service Unavailable");
        const char *msg = "Camera busy - please try again";
        httpd_resp_send(req, msg, strlen(msg));
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "[%s] Camera capture success on attempt %d: %u bytes", CAPTURE_MARKER, attempts + 1, fb->len);
    ESP_LOGI(TAG, "[%s] Image format: %d, width: %d, height: %d", CAPTURE_MARKER, fb->format, fb->width, fb->height);

    // 发送 JPEG 数据
    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture.jpg");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    ESP_LOGI(TAG, "[%s] Calling httpd_resp_send with %u bytes...", CAPTURE_MARKER, fb->len);
    esp_err_t res = httpd_resp_send(req, (const char *)fb->buf, fb->len);
    
    if (res == ESP_OK) {
        ESP_LOGI(TAG, "[%s] *** IMAGE SENT SUCCESSFULLY *** (%u bytes)", CAPTURE_MARKER, fb->len);
    } else {
        ESP_LOGE(TAG, "[%s] *** SEND FAILED *** Error: %s (errno=%d)", CAPTURE_MARKER, esp_err_to_name(res), errno);
    }

    esp_camera_fb_return(fb);
    ESP_LOGI(TAG, "========================================");
    return res;
}

// 处理 MJPEG 流请求，持续输出多帧 JPEG（简化版，不共享帧）
static esp_err_t jpg_stream_httpd_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Stream request opened");
    static const char *STREAM_CONTENT_TYPE = "multipart/x-mixed-replace; boundary=frame";
    static const char *STREAM_BOUNDARY = "--frame\r\n";
    static const char *STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

    esp_err_t res = httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
    if (res != ESP_OK) {
        return res;
    }

    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate, proxy-revalidate");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
    httpd_resp_set_hdr(req, "Expires", "0");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    camera_fb_t *fb = NULL;
    char part_buf[64];

    while (true) {
        fb = esp_camera_fb_get();
        if (!fb) {
            ESP_LOGE(TAG, "Camera capture failed (stream)");
            res = ESP_FAIL;
            break;
        }

        size_t hlen = snprintf(part_buf, sizeof(part_buf), STREAM_PART, fb->len);
        if (httpd_resp_send_chunk(req, STREAM_BOUNDARY, strlen(STREAM_BOUNDARY)) != ESP_OK ||
            httpd_resp_send_chunk(req, part_buf, hlen) != ESP_OK ||
            httpd_resp_send_chunk(req, (const char *)fb->buf, fb->len) != ESP_OK ||
            httpd_resp_send_chunk(req, "\r\n", 2) != ESP_OK) {
            ESP_LOGE(TAG, "Stream send failed (errno=%d)", errno);
            res = ESP_FAIL;
            esp_camera_fb_return(fb);
            break;
        }

        esp_camera_fb_return(fb);
        fb = NULL;
        vTaskDelay(pdMS_TO_TICKS(33));  // ~30 FPS
    }

    if (fb) {
        esp_camera_fb_return(fb);
    }

    httpd_resp_send_chunk(req, NULL, 0);  // end streaming
    ESP_LOGI(TAG, "Stream request closed");
    return res;
}
// 简单的前端页面，嵌入视频流与抓拍按钮（真正并发版本）
static const char *html_content =
"<!DOCTYPE html>"
"<html lang=\"en\">"
"<head>"
"<meta charset=\"utf-8\">"
"<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
"<title>ESP32 Camera Feed</title>"
"<style>"
"body{margin:0;padding:24px;background:#f4f6f8;font-family:Arial,Helvetica,sans-serif;text-align:center;}"
"h1{margin-bottom:20px;color:#0f172a;}"
"#stream{max-width:100%;height:auto;border:4px solid #fff;border-radius:8px;box-shadow:0 4px 18px rgba(15,23,42,0.25);}"
".actions{margin-top:18px;}"
".btn{display:inline-block;padding:10px 18px;margin:0 6px;border-radius:6px;background:#2563eb;color:#fff;text-decoration:none;font-weight:600;border:none;cursor:pointer;}"
".btn:hover{background:#1d4ed8;}"
".btn:disabled{background:#94a3b8;cursor:not-allowed;}"
"</style>"
"</head>"
"<body>"
"<h1>ESP32 Camera Feed</h1>"
"<img id=\"stream\" src=\"/stream\" alt=\"Live Camera Stream\">"
"<div class=\"actions\">"
"<button id=\"captureBtn\" class=\"btn\">Capture Still Photo</button>"
"<span id=\"status\" style=\"margin-left:10px;color:#666;\"></span>"
"</div>"
"<script>"
"document.getElementById('captureBtn').addEventListener('click',function(){"
"var btn=this;var status=document.getElementById('status');"
"btn.disabled=true;btn.textContent='Capturing...';status.textContent='';"
"console.log('Starting capture request (concurrent)...');"
"fetch('/capture',{method:'GET',cache:'no-cache'})"
".then(function(response){"
"console.log('Response:',response.status);"
"if(!response.ok){throw new Error('HTTP '+response.status);}"
"return response.blob();"
"})"
".then(function(blob){"
"console.log('Blob size:',blob.size);"
"if(blob.size<100){throw new Error('Image too small');}"
"var url=URL.createObjectURL(blob);"
"var win=window.open(url,'_blank');"
"if(!win){status.textContent='Please allow pop-ups';}"
"else{status.textContent='Photo captured!';}"
"btn.disabled=false;btn.textContent='Capture Still Photo';"
"})"
".catch(function(err){"
"console.error('Error:',err);"
"status.textContent='Error: '+err.message;"
"btn.disabled=false;btn.textContent='Capture Still Photo';"
"});"
"});"
"</script>"
"</body>"
"</html>";

static esp_err_t index_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "HTTP GET /");
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, html_content, strlen(html_content));
}

// 启动 HTTP 服务器并注册各个路由
void start_camera_server(void) {
    // 初始化帧共享互斥锁
    if (!g_frame_mutex) {
        g_frame_mutex = xSemaphoreCreateMutex();
        if (!g_frame_mutex) {
            ESP_LOGE(TAG, "Failed to create frame mutex");
            return;
        }
        ESP_LOGI(TAG, "Frame mutex created for concurrent access");
    }
    
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 5120;
    config.max_open_sockets = 12;  // 增加到12以支持并发流和抓拍 (LWIP_MAX_SOCKETS=16, 内部占用3)
    config.backlog_conn = 8;
    config.recv_wait_timeout = 30;
    config.send_wait_timeout = 60;  // 增加发送超时到60秒，避免大图片发送时超时
    // 禁用 keep-alive，避免长时间占用 sockets（有助于并发抓拍）
    config.keep_alive_enable = false;
    config.keep_alive_idle = 5;
    config.keep_alive_interval = 5;
    config.keep_alive_count = 3;
    config.lru_purge_enable = true;
    httpd_handle_t server = NULL;

    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t index_uri = {
            .uri = "/",
            .method = HTTP_GET,
            .handler = index_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &index_uri);

        httpd_uri_t camera_uri = {
            .uri = "/capture",
            .method = HTTP_GET,
            .handler = jpg_httpd_handler,
            .user_ctx = NULL
        };
        ESP_LOGI(TAG, "Registered /capture handler");
        httpd_register_uri_handler(server, &camera_uri);

        httpd_uri_t stream_uri = {
            .uri = "/stream",
            .method = HTTP_GET,
            .handler = jpg_stream_httpd_handler,
            .user_ctx = NULL
        };
        ESP_LOGI(TAG, "Registered /stream handler");
        httpd_register_uri_handler(server, &stream_uri);
    }
}

// WiFi 事件处理器
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
        ESP_LOGI(TAG, "WiFi started, connecting...");
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "Disconnected from WiFi, retrying...");
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Got IP Address: " IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "=====================================================");
        ESP_LOGI(TAG, "Camera web server available at: http://" IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "=====================================================");
    }
}

// 配置 ESP32 连接到 WiFi（STA 模式）
static void wifi_init_sta(void)
{
    // 修改为你的 WiFi 名称和密码
    const char* WIFI_SSID = "Xiaomi_CA43";      // ← 改成你的 WiFi 名称
    const char* WIFI_PASSWORD = "shi123456";  // ← 改成你的 WiFi 密码

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // 注册事件处理器
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        NULL));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = "",
            .password = "",
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    
    // 复制 SSID 和密码
    strncpy((char*)wifi_config.sta.ssid, WIFI_SSID, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char*)wifi_config.sta.password, WIFI_PASSWORD, sizeof(wifi_config.sta.password) - 1);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi STA mode initialized. Connecting to SSID: %s", WIFI_SSID);
}

// 应用入口：初始化存储、网络与摄像头
void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());

    wifi_init_sta();  // 改为 STA 模式连接 WiFi

#if ESP_CAMERA_SUPPORTED
    if (ESP_OK != init_camera())
    {
        return;
    }

    start_camera_server();
#else
    ESP_LOGE(TAG, "Camera support is not available for this chip");
    return;
#endif
}


