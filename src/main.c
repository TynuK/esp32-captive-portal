#include "captive_portal.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include <string.h>

static const char *TAG = "main";

// Пример пользовательских обработчиков
static esp_err_t api_status_handler(httpd_req_t *req) {
    const char *response = "{\"status\":\"ok\",\"portal\":\"working\"}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, response, strlen(response));
    return ESP_OK;
}

static esp_err_t api_config_handler(httpd_req_t *req) {
    // Обработка POST запроса конфигурации
    char buf[256];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    
    if (ret > 0) {
        buf[ret] = '\0';
        ESP_LOGI(TAG, "Received config: %s", buf);
    }
    
    const char *response = "{\"result\":\"success\"}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, response, strlen(response));
    return ESP_OK;
}

void app_main(void) {
    ESP_LOGI(TAG, "Starting captive portal");
    
    // Инициализация NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    
    // Инициализация сетевого стека
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    
    // Инициализация WiFi
    wifi_init_config_t wifi_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_config));
    
    // Конфигурация портала
    captive_portal_config_t config = {0};
    strcpy(config.ap_ssid, "ESP32-Portal");
    //strcpy(config.ap_password, "12345678");  // С паролем (минимум 8 символов)
    config.ap_password[0] = '\0'; // Без пароля (открытая сеть) Первый символ = нуль
    config.ap_channel = 1;
    config.ap_hidden = false;
    config.http_port = 80;
    strcpy(config.web_root_path, "/spiffs");
    
    // Инициализация портала
    captive_portal_t *portal = captive_portal_init(&config);
    if (!portal) {
        ESP_LOGE(TAG, "Failed to init portal");
        return;
    }
    
    // Добавляем пользовательские обработчики
    captive_portal_add_handler(portal, "/api/status", 
                               CAPTIVE_HANDLER_GET, 
                               api_status_handler);
    
    captive_portal_add_handler(portal, "/api/config", 
                               CAPTIVE_HANDLER_POST, 
                               api_config_handler);
    
    // Запускаем портал
    if (captive_portal_start(portal) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start portal");
        captive_portal_destroy(portal);
        return;
    }
    
    ESP_LOGI(TAG, "Portal is running");
    
    // Вечный цикл
    while (1) {
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
}