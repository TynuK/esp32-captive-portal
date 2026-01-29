#pragma once

#include "esp_http_server.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct captive_portal_t captive_portal_t;
typedef esp_err_t (*captive_handler_t)(httpd_req_t *req);

typedef enum {
    CAPTIVE_HANDLER_GET,
    CAPTIVE_HANDLER_POST,
    CAPTIVE_HANDLER_PUT,
    CAPTIVE_HANDLER_DELETE
} captive_handler_method_t;

// Конфигурация (из вашего рабочего кода)
typedef struct {
    char ap_ssid[32];
    char ap_password[64];
    uint8_t ap_channel;
    bool ap_hidden;
    uint16_t http_port;
    char web_root_path[32];
} captive_portal_config_t;

// Инициализация
captive_portal_t* captive_portal_init(const captive_portal_config_t *config);

// Запуск/остановка
esp_err_t captive_portal_start(captive_portal_t *portal);
esp_err_t captive_portal_stop(captive_portal_t *portal);
void captive_portal_destroy(captive_portal_t *portal);

// Добавление пользовательских обработчиков
esp_err_t captive_portal_add_handler(captive_portal_t *portal,
                                     const char *uri,
                                     captive_handler_method_t method,
                                     captive_handler_t handler);

// Утилиты
bool captive_portal_is_running(captive_portal_t *portal);

#ifdef __cplusplus
}
#endif