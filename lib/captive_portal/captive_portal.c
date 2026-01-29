#include "captive_portal.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_spiffs.h"
#include "esp_http_server.h"
#include "lwip/sockets.h"
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>

static const char *TAG = "captive_portal";

// ТОЧНО КАК В ВАШЕМ РАБОЧЕМ КОДЕ
typedef struct {
    const char *extension;
    const char *mime_type;
} mime_type_t;

static const mime_type_t mime_types[] = {
    {".html", "text/html"},
    {".htm", "text/html"},
    {".css", "text/css"},
    {".js", "application/javascript"},
    {".json", "application/json"},
    {".png", "image/png"},
    {".jpg", "image/jpeg"},
    {".jpeg", "image/jpeg"},
    {".gif", "image/gif"},
    {".svg", "image/svg+xml"},
    {".ico", "image/x-icon"},
    {".txt", "text/plain"},
    {".pdf", "application/pdf"},
    {".zip", "application/zip"}
};

// Структура пользовательского обработчика
typedef struct custom_handler {
    char uri[64];
    captive_handler_method_t method;
    captive_handler_t handler;
    struct custom_handler *next;
} custom_handler_t;

// Основная структура портала
struct captive_portal_t {
    captive_portal_config_t config;
    httpd_handle_t server;
    TaskHandle_t dns_task;
    bool running;
    custom_handler_t *custom_handlers;
    SemaphoreHandle_t mutex;
    int dns_socket;
    esp_netif_t *ap_netif;
};

// ВСПОМОГАТЕЛЬНЫЕ ФУНКЦИИ ИЗ ВАШЕГО КОДА
static const char *get_mime_type(const char *filename) {
    const char *dot = strrchr(filename, '.');
    if (!dot || dot == filename)
        return "text/plain";

    for (int i = 0; i < sizeof(mime_types) / sizeof(mime_types[0]); i++) {
        if (strcasecmp(dot, mime_types[i].extension) == 0) {
            return mime_types[i].mime_type;
        }
    }

    return "text/plain";
}

static void make_safe_path(char *dest, size_t dest_size, const char *base, const char *path) {
    strncpy(dest, base, dest_size - 1);
    dest[dest_size - 1] = '\0';

    size_t base_len = strlen(dest);

    if (base_len < dest_size - 1 && path[0] != '\0') {
        if (dest[base_len - 1] != '/' && path[0] != '/') {
            if (base_len < dest_size - 2) {
                dest[base_len++] = '/';
                dest[base_len] = '\0';
            }
        }
        strncat(dest, path, dest_size - base_len - 1);
    }
}

// DNS HIJACK ИЗ ВАШЕГО КОДА
static void dns_hijack_task(void *pvParameters) {
    captive_portal_t *portal = (captive_portal_t *)pvParameters;
    struct sockaddr_in server_addr, client_addr;
    socklen_t addr_len = sizeof(client_addr);
    char buffer[512];
    
    portal->dns_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (portal->dns_socket < 0) {
        ESP_LOGE(TAG, "Failed to create DNS socket");
        vTaskDelete(NULL);
        return;
    }
    
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(53);
    server_addr.sin_addr.s_addr = INADDR_ANY;
    
    if (bind(portal->dns_socket, (struct sockaddr *)&server_addr, 
             sizeof(server_addr)) < 0) {
        ESP_LOGE(TAG, "Failed to bind DNS socket");
        close(portal->dns_socket);
        vTaskDelete(NULL);
        return;
    }
    
    struct timeval timeout = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(portal->dns_socket, SOL_SOCKET, SO_RCVTIMEO, 
               &timeout, sizeof(timeout));
    
    ESP_LOGI(TAG, "DNS hijack started");
    
    while (portal->running) {
        int len = recvfrom(portal->dns_socket, buffer, sizeof(buffer), 0,
                          (struct sockaddr *)&client_addr, &addr_len);
        
        if (len > 12) {
            ESP_LOGI(TAG, "DNS query from %s", inet_ntoa(client_addr.sin_addr));
            
            buffer[2] = 0x81;
            buffer[3] = 0x80;
            buffer[7] = 0x01;
            
            memcpy(buffer + len, "\xC0\x0C", 2);
            buffer[len + 2] = 0x00;
            buffer[len + 3] = 0x01;
            buffer[len + 4] = 0x00;
            buffer[len + 5] = 0x01;
            buffer[len + 6] = 0x00;
            buffer[len + 7] = 0x00;
            buffer[len + 8] = 0x00;
            buffer[len + 9] = 0x78;
            buffer[len + 10] = 0x00;
            buffer[len + 11] = 0x04;
            
            buffer[len + 12] = 192;
            buffer[len + 13] = 168;
            buffer[len + 14] = 4;
            buffer[len + 15] = 1;
            
            sendto(portal->dns_socket, buffer, len + 16, 0,
                   (struct sockaddr *)&client_addr, addr_len);
        }
    }
    
    close(portal->dns_socket);
    ESP_LOGI(TAG, "DNS hijack stopped");
    vTaskDelete(NULL);
}

// ОБРАБОТЧИК СТАТИЧЕСКИХ ФАЙЛОВ ИЗ ВАШЕГО КОДА
static esp_err_t static_file_handler(httpd_req_t *req) {
    captive_portal_t *portal = (captive_portal_t *)req->user_ctx;
    char filepath[256];
    FILE *file = NULL;
    esp_err_t ret = ESP_OK;

    if (strcmp(req->uri, "/") == 0) {
        make_safe_path(filepath, sizeof(filepath), portal->config.web_root_path, "/index.html");
    } else {
        make_safe_path(filepath, sizeof(filepath), portal->config.web_root_path, req->uri);
    }

    struct stat st;
    if (stat(filepath, &st) != 0) {
        ESP_LOGI(TAG, "File not found: %s", req->uri);
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }

    file = fopen(filepath, "rb");
    if (!file) {
        ESP_LOGE(TAG, "Failed to open file: %s", filepath);
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }

    const char *mime_type = get_mime_type(filepath);
    httpd_resp_set_type(req, mime_type);

    ESP_LOGI(TAG, "Serving file: %s (%ld bytes)", req->uri, (long)st.st_size);

    char buffer[512];
    size_t read_bytes;
    size_t total_sent = 0;

    while ((read_bytes = fread(buffer, 1, sizeof(buffer), file)) > 0) {
        if (httpd_resp_send_chunk(req, buffer, read_bytes) != ESP_OK) {
            ret = ESP_FAIL;
            break;
        }
        total_sent += read_bytes;
    }

    fclose(file);

    if (ret == ESP_OK) {
        httpd_resp_send_chunk(req, NULL, 0);
        ESP_LOGD(TAG, "File %s sent successfully (%zu bytes)", req->uri, total_sent);
    } else {
        ESP_LOGE(TAG, "Error sending file %s", req->uri);
    }

    return ret;
}

// CAPTIVE PORTAL ОБРАБОТЧИК ИЗ ВАШЕГО РАБОЧЕГО КОДА (ТОЧНАЯ КОПИЯ!)
static esp_err_t captive_simple_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "Captive handler: %s", req->uri);

    char user_agent[256] = {0};
    size_t user_agent_len = httpd_req_get_hdr_value_len(req, "User-Agent");
    if (user_agent_len > 0) {
        httpd_req_get_hdr_value_str(req, "User-Agent", user_agent, sizeof(user_agent));
        ESP_LOGI(TAG, "User-Agent: %s", user_agent);
    }

    // Android: generate_204, gen_204 - ВОЗВРАЩАЕМ РЕДИРЕКТ!
    if (strstr(req->uri, "generate_204") || strstr(req->uri, "gen_204")) {
        ESP_LOGI(TAG, "Android captive portal detected - returning redirect");
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }

    // Windows: connecttest.txt или ncsi.txt
    if (strstr(req->uri, "connecttest") || strstr(req->uri, "ncsi")) {
        // Проверяем User-Agent
        if (strstr(user_agent, "NCSI") || strstr(req->uri, "ncsi")) {
            ESP_LOGI(TAG, "Windows NCSI detection");
            httpd_resp_set_type(req, "text/plain");
            httpd_resp_send(req, "Microsoft NCSI", 14);
        } else {
            ESP_LOGI(TAG, "Windows connecttest detection");
            httpd_resp_set_type(req, "text/plain");
            httpd_resp_send(req, "Microsoft Connect Test", 21);
        }
        return ESP_OK;
    }

    // iOS/Mac: hotspot-detect.html
    if (strstr(req->uri, "hotspot-detect")) {
        ESP_LOGI(TAG, "iOS hotspot-detect detected");

        // Для iOS возвращаем HTML с редиректом
        const char *ios_response =
            "<!DOCTYPE html>\n"
            "<html>\n"
            "<head>\n"
            "<meta http-equiv=\"refresh\" content=\"0;url=http://192.168.4.1/\">\n"
            "<script>window.location.href='http://192.168.4.1/';</script>\n"
            "</head>\n"
            "<body>\n"
            "Success\n"
            "</body>\n"
            "</html>";

        httpd_resp_set_type(req, "text/html");
        httpd_resp_send(req, ios_response, strlen(ios_response));
        return ESP_OK;
    }

    // iOS: /bag
    if (strstr(req->uri, "bag")) {
        ESP_LOGI(TAG, "iOS bag request detected");
        const char *bag_response =
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
            "<plist version=\"1.0\">\n"
            "<dict>\n"
            "<key>CaptiveNetwork</key>\n"
            "<true/>\n"
            "</dict>\n"
            "</plist>";

        httpd_resp_set_type(req, "text/xml");
        httpd_resp_send(req, bag_response, strlen(bag_response));
        return ESP_OK;
    }

    // Для всех остальных запросов - тоже редирект
    ESP_LOGI(TAG, "Other captive request - redirecting");

    const char *default_redirect =
        "<!DOCTYPE html>\n"
        "<html>\n"
        "<head>\n"
        "<meta http-equiv=\"refresh\" content=\"0;url=http://192.168.4.1/\">\n"
        "<script>window.location.href='http://192.168.4.1/';</script>\n"
        "</head>\n"
        "<body>\n"
        "<p>Redirecting to network login page...</p>\n"
        "</body>\n"
        "</html>";

    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_send(req, default_redirect, strlen(default_redirect));
    return ESP_OK;
}

// WILDCARD HANDLER ИЗ ВАШЕГО КОДА (с добавлением пользовательских обработчиков)
static esp_err_t wildcard_handler(httpd_req_t *req) {
    captive_portal_t *portal = (captive_portal_t *)req->user_ctx;
    
    ESP_LOGI(TAG, "Wildcard handler: %s", req->uri);

    // Быстрая проверка на стандартные файлы
    if (strcmp(req->uri, "/") == 0 ||
        strcmp(req->uri, "/index.html") == 0 ||
        strcmp(req->uri, "/styles.css") == 0 ||
        strcmp(req->uri, "/script.js") == 0) {
        return static_file_handler(req);
    }

    // Проверяем пользовательские обработчики ПЕРВЫМИ
    if (portal->mutex) {
        xSemaphoreTake(portal->mutex, portMAX_DELAY);
    }
    
    custom_handler_t *handler = portal->custom_handlers;
    while (handler) {
        if ((handler->method == CAPTIVE_HANDLER_GET && req->method == HTTP_GET) ||
            (handler->method == CAPTIVE_HANDLER_POST && req->method == HTTP_POST) ||
            (handler->method == CAPTIVE_HANDLER_PUT && req->method == HTTP_PUT) ||
            (handler->method == CAPTIVE_HANDLER_DELETE && req->method == HTTP_DELETE)) {
            
            if (strcmp(req->uri, handler->uri) == 0) {
                if (portal->mutex) {
                    xSemaphoreGive(portal->mutex);
                }
                return handler->handler(req);
            }
        }
        handler = handler->next;
    }
    
    if (portal->mutex) {
        xSemaphoreGive(portal->mutex);
    }

    // Проверяем, является ли это captive portal проверкой
    const char *captive_keywords[] = {
        "generate_204", "gen_204",         // Android
        "ncsi", "connecttest", "redirect", // Windows
        "hotspot", "bag",                  // iOS
        "success.txt", "canonical",        // Другие iOS
        "apple-touch",                     // Иконки
        "iphonesubmissions", "WebObjects"  // iOS
    };

    for (int i = 0; i < sizeof(captive_keywords) / sizeof(captive_keywords[0]); i++) {
        if (strstr(req->uri, captive_keywords[i])) {
            return captive_simple_handler(req);
        }
    }

    // Если не нашли - пробуем как статический файл
    return static_file_handler(req);
}

// Инициализация SPIFFS
static esp_err_t init_spiffs(const char *base_path) {
    ESP_LOGI(TAG, "Initializing SPIFFS");

    esp_vfs_spiffs_conf_t conf = {
        .base_path = base_path,
        .partition_label = NULL,
        .max_files = 10,
        .format_if_mount_failed = true
    };

    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize SPIFFS (%s)", esp_err_to_name(ret));
        return ret;
    }

    size_t total = 0, used = 0;
    esp_spiffs_info(NULL, &total, &used);
    ESP_LOGI(TAG, "SPIFFS: total=%zu, used=%zu", total, used);

    return ESP_OK;
}

// Инициализация портала
captive_portal_t* captive_portal_init(const captive_portal_config_t *config) {
    captive_portal_t *portal = calloc(1, sizeof(captive_portal_t));
    if (!portal) {
        ESP_LOGE(TAG, "Failed to allocate portal");
        return NULL;
    }

    // Копируем конфигурацию
    if (config) {
        memcpy(&portal->config, config, sizeof(captive_portal_config_t));
    } else {
        // Значения по умолчанию из вашего кода
        strcpy(portal->config.ap_ssid, "ESP32-Captive-Portal");
        portal->config.ap_password[0] = '\0';
        portal->config.ap_channel = 1;
        portal->config.ap_hidden = false;
        portal->config.http_port = 80;
        strcpy(portal->config.web_root_path, "/spiffs");
    }

    portal->mutex = xSemaphoreCreateMutex();
    if (!portal->mutex) {
        ESP_LOGW(TAG, "Failed to create mutex, continuing without it");
    }

    ESP_LOGI(TAG, "Captive portal initialized");
    return portal;
}

// Запуск портала (настройка сети как в вашем коде)
esp_err_t captive_portal_start(captive_portal_t *portal) {
    if (!portal || portal->running) {
        return ESP_FAIL;
    }

    // Инициализируем SPIFFS
    if (init_spiffs(portal->config.web_root_path) != ESP_OK) {
        ESP_LOGW(TAG, "SPIFFS not available, using minimal web interface");
    }

    // Создаем сетевой интерфейс
    portal->ap_netif = esp_netif_create_default_wifi_ap();
    if (!portal->ap_netif) {
        ESP_LOGE(TAG, "Failed to create AP network interface");
        return ESP_FAIL;
    }

    // Настраиваем IP адрес
    esp_netif_ip_info_t ip_info;
    IP4_ADDR(&ip_info.ip, 192, 168, 4, 1);
    IP4_ADDR(&ip_info.gw, 192, 168, 4, 1);
    IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);
    esp_netif_dhcps_stop(portal->ap_netif);
    esp_netif_set_ip_info(portal->ap_netif, &ip_info);
    esp_netif_dhcps_start(portal->ap_netif);

    // Настраиваем WiFi (как в вашем коде)
    wifi_config_t wifi_config = {0};
    strcpy((char *)wifi_config.ap.ssid, portal->config.ap_ssid);
    wifi_config.ap.ssid_len = strlen(portal->config.ap_ssid);
    wifi_config.ap.channel = portal->config.ap_channel;
    wifi_config.ap.authmode = portal->config.ap_password[0] ? 
                             WIFI_AUTH_WPA_WPA2_PSK : WIFI_AUTH_OPEN;
    wifi_config.ap.ssid_hidden = portal->config.ap_hidden;
    wifi_config.ap.max_connection = 4;
    wifi_config.ap.beacon_interval = 100;
    
    if (portal->config.ap_password[0]) {
        strcpy((char *)wifi_config.ap.password, portal->config.ap_password);
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Конфигурация HTTP сервера (как в вашем коде)
    httpd_config_t server_config = HTTPD_DEFAULT_CONFIG();
    server_config.server_port = portal->config.http_port;
    server_config.max_uri_handlers = 20;
    server_config.lru_purge_enable = true;
    server_config.uri_match_fn = httpd_uri_match_wildcard;
    
    // УВЕЛИЧЬТЕ ЭТИ НАСТРОЙКИ:
    server_config.max_open_sockets = 7;
    server_config.recv_wait_timeout = 5;
    server_config.send_wait_timeout = 5;
    server_config.stack_size = 4096;

    ESP_LOGI(TAG, "Starting web server on port %d", server_config.server_port);

    // Пробуем запустить сервер (как в вашем коде)
    esp_err_t ret;
    int retry_count = 0;

    for (retry_count = 0; retry_count < 3; retry_count++) {
        ret = httpd_start(&portal->server, &server_config);
        if (ret == ESP_OK) {
            break;
        }

        ESP_LOGW(TAG, "Failed to start server (attempt %d): %s",
                 retry_count + 1, esp_err_to_name(ret));
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start server after %d attempts", retry_count);
        return ret;
    }

    // Регистрируем wildcard handler
    httpd_register_uri_handler(portal->server, &(httpd_uri_t){
        .uri = "/*",
        .method = HTTP_GET,
        .handler = wildcard_handler,
        .user_ctx = portal
    });

    httpd_register_uri_handler(portal->server, &(httpd_uri_t){
        .uri = "/*",
        .method = HTTP_POST,
        .handler = wildcard_handler,
        .user_ctx = portal
    });

    httpd_register_uri_handler(portal->server, &(httpd_uri_t){
        .uri = "/*",
        .method = HTTP_PUT,
        .handler = wildcard_handler,
        .user_ctx = portal
    });

    httpd_register_uri_handler(portal->server, &(httpd_uri_t){
        .uri = "/*",
        .method = HTTP_DELETE,
        .handler = wildcard_handler,
        .user_ctx = portal
    });

    // Запускаем DNS hijack
    portal->running = true;
    xTaskCreate(dns_hijack_task, "dns_hijack", 4096, portal, 5, &portal->dns_task);

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "Captive Portal Started!");
    ESP_LOGI(TAG, "WiFi SSID: %s", portal->config.ap_ssid);
    ESP_LOGI(TAG, "IP Address: " IPSTR, IP2STR(&ip_info.ip));
    ESP_LOGI(TAG, "HTTP Port: %d", portal->config.http_port);
    ESP_LOGI(TAG, "========================================");

    return ESP_OK;
}

// Добавление пользовательского обработчика
esp_err_t captive_portal_add_handler(captive_portal_t *portal,
                                     const char *uri,
                                     captive_handler_method_t method,
                                     captive_handler_t handler) {
    if (!portal || !uri || !handler) {
        return ESP_ERR_INVALID_ARG;
    }

    if (portal->mutex) {
        xSemaphoreTake(portal->mutex, portMAX_DELAY);
    }

    // Проверяем, не существует ли уже
    custom_handler_t *existing = portal->custom_handlers;
    while (existing) {
        if (strcmp(existing->uri, uri) == 0 && existing->method == method) {
            if (portal->mutex) {
                xSemaphoreGive(portal->mutex);
            }
            ESP_LOGW(TAG, "Handler already exists for %s", uri);
            return ESP_FAIL;
        }
        existing = existing->next;
    }

    // Добавляем новый
    custom_handler_t *new_handler = malloc(sizeof(custom_handler_t));
    if (!new_handler) {
        if (portal->mutex) {
            xSemaphoreGive(portal->mutex);
        }
        return ESP_ERR_NO_MEM;
    }

    strncpy(new_handler->uri, uri, sizeof(new_handler->uri) - 1);
    new_handler->uri[sizeof(new_handler->uri) - 1] = '\0';
    new_handler->method = method;
    new_handler->handler = handler;
    new_handler->next = portal->custom_handlers;
    portal->custom_handlers = new_handler;

    if (portal->mutex) {
        xSemaphoreGive(portal->mutex);
    }

    ESP_LOGI(TAG, "Added custom handler for %s", uri);
    return ESP_OK;
}

// Остановка портала
esp_err_t captive_portal_stop(captive_portal_t *portal) {
    if (!portal || !portal->running) {
        return ESP_FAIL;
    }

    portal->running = false;
    vTaskDelay(100 / portTICK_PERIOD_MS);

    if (portal->server) {
        httpd_stop(portal->server);
        portal->server = NULL;
    }

    if (portal->ap_netif) {
        esp_netif_destroy(portal->ap_netif);
        portal->ap_netif = NULL;
    }

    esp_wifi_stop();

    ESP_LOGI(TAG, "Captive portal stopped");
    return ESP_OK;
}

// Освобождение ресурсов
void captive_portal_destroy(captive_portal_t *portal) {
    if (!portal) return;

    if (portal->running) {
        captive_portal_stop(portal);
    }

    if (portal->mutex) {
        xSemaphoreTake(portal->mutex, portMAX_DELAY);
    }
    
    custom_handler_t *handler = portal->custom_handlers;
    while (handler) {
        custom_handler_t *next = handler->next;
        free(handler);
        handler = next;
    }
    
    if (portal->mutex) {
        xSemaphoreGive(portal->mutex);
        vSemaphoreDelete(portal->mutex);
    }

    free(portal);
    ESP_LOGI(TAG, "Captive portal destroyed");
}

// Утилиты
bool captive_portal_is_running(captive_portal_t *portal) {
    return portal ? portal->running : false;
}