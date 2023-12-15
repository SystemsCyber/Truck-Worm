//includes
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "esp_http_server.h"
#include "esp_http_client.h"
#include "esp_ota_ops.h"
#include "esp_log.h"
#include "esp_flash_partitions.h"
#include "esp_partition.h"
#include "esp_image_format.h"
#include "esp_system.h"
#include "esp_spiffs.h"
#include "esp_task_wdt.h"

//defines
#define WIFI_SSID "iosix-eld-1"
#define WIFI_PASS "deadbeef77"
#define FIRMWARE_UPDATE_KEY "123456" 
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define BUFFSIZE 1024
// #define TAG "SPIFFS"
// #define TAG "OTA"

//globals
static const char *TAG = "OTA Update";

//functions
esp_err_t firmware_update_handler(httpd_req_t *req);
esp_err_t get_handler(httpd_req_t *req);

void init_spiffs() {
    ESP_LOGI(TAG, "Initializing SPIFFS");

    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = NULL,
        .max_files = 5,
        .format_if_mount_failed = true
    };

    // Unmount SPIFFS in case it is already mounted
    esp_err_t ret = esp_vfs_spiffs_unregister(NULL);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "SPIFFS unmounted successfully");
    }

    // Try formatting the SPIFFS partition
    ESP_LOGI(TAG, "Formatting SPIFFS");
    ret = esp_spiffs_format(NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to format SPIFFS (%s)", esp_err_to_name(ret));
    }

    // Mount SPIFFS
    ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount or format filesystem");
        } else if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "Failed to find SPIFFS partition");
        } else {
            ESP_LOGE(TAG, "Failed to initialize SPIFFS (%s)", esp_err_to_name(ret));
        }
        return;
    }

    size_t total = 0, used = 0;
    ret = esp_spiffs_info(NULL, &total, &used);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get SPIFFS partition information (%s)", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "Partition size: total: %d, used: %d", total, used);
    }
}

// void print_buffer_hex(const uint8_t *data, size_t size) {
//     for (size_t i = 0; i < size; ++i) {
//         if (i > 0 && i % 16 == 0)
//             printf("\n");
//         printf("%02x ", data[i]);
//     }
//     printf("\n");
// }

void wifi_setup_task(void *pvParameters) {
    // Initialize and start WiFi here
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_netif_init());
    esp_netif_create_default_wifi_ap();

    // Set up the WiFi configuration for AP mode
    wifi_config_t wifi_config = {
        .ap = {
            .ssid = WIFI_SSID,
            .ssid_len = strlen(WIFI_SSID),
            .password = WIFI_PASS,
            .max_connection = 4,  // Maximum number of connections
            .authmode = WIFI_AUTH_WPA_WPA2_PSK
        },
    };
    if (strlen(WIFI_PASS) == 0) {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI("wifi_setup_task", "WiFi AP mode and DHCP server setup complete");
    vTaskDelete(NULL);
}

void perform_ota_update() {
    esp_task_wdt_add(NULL);
    esp_err_t err;
    FILE* f = fopen("/spiffs/firmware.bin", "rb");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open firmware file");
        return;
    }

    const esp_partition_t* update_partition = esp_ota_get_next_update_partition(NULL);
    if (update_partition == NULL) {
        ESP_LOGE(TAG, "No OTA partition found");
        fclose(f);
        return;
    }

    esp_ota_handle_t update_handle;
    err = esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &update_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed (%s)", esp_err_to_name(err));
        fclose(f);
        return;
    }
    ESP_LOGE(TAG, "Updating.....");
    char *ota_write_data = malloc(BUFFSIZE);
    while (!feof(f)) {
        esp_task_wdt_reset();
        size_t bytes_read = fread(ota_write_data, 1, BUFFSIZE, f);
        if (bytes_read > 0) {
            err = esp_ota_write(update_handle, (const void *)ota_write_data, bytes_read);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "esp_ota_write failed (%s)", esp_err_to_name(err));
                esp_ota_abort(update_handle);
                fclose(f);
                return;
            }
        }
    }
    free(ota_write_data);
    fclose(f);

    err = esp_ota_end(update_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed (%s)", esp_err_to_name(err));
        return;
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed (%s)", esp_err_to_name(err));
        return;
    }
    esp_task_wdt_delete(NULL);
    ESP_LOGI(TAG, "OTA update successful, restarting...");
    esp_restart();
}

esp_err_t firmware_update_handler(httpd_req_t *req) {
    char ota_write_data[BUFFSIZE] = { 0 };
    int total_received = 0;
    int received = 0;

    // Open a file on SPIFFS to store the incoming firmware
    FILE *f = fopen("/spiffs/firmware.bin", "wb");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open file for writing");
        return ESP_FAIL;
    }

    while (total_received < req->content_len) {
        received = httpd_req_recv(req, ota_write_data, BUFFSIZE);
        if (received <= 0) {
            if (received == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            ESP_LOGE(TAG, "OTA receive error");
            return ESP_FAIL;
        }
        ESP_LOGE(TAG, "Receiving Data");
        // Write the received data to the file
        fwrite(ota_write_data, sizeof(char), received, f);

        total_received += received;
    }
    ESP_LOGE(TAG, "Closing File");
    fclose(f);
    ESP_LOGE(TAG, "Trying OTA");
    perform_ota_update();
    return ESP_OK;
}

esp_err_t get_handler(httpd_req_t *req) {
    const char* html_response = 
        "<!DOCTYPE html>"
        "<html>"
        "<head><title>ESP32 Firmware Update</title></head>"
        "<body>"
        "<h1>Firmware Update</h1>"
        "<form method=\"post\" action=\"/update-firmware\" enctype=\"multipart/form-data\">"
            "<input type=\"file\" name=\"firmware\" accept=\".bin\">"
            "<input type=\"submit\" value=\"Upload Firmware\">"
        "</form>"
        "</body>"
        "</html>";

    httpd_resp_send(req, html_response, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

void http_server_task(void *pvParameters) {
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    ESP_ERROR_CHECK(httpd_start(&server, &config));

    // Register URI handlers, for example, to serve a webpage and handle firmware uploads
    httpd_uri_t firmware_update_uri = {
        .uri = "/update-firmware",
        .method = HTTP_POST,
        .handler = firmware_update_handler,
       .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &firmware_update_uri);

    // // Implement other URI handlers as needed
    // httpd_uri_t uri_get = {
    //     .uri = "/",
    //     .method = HTTP_GET,
    //     .handler = get_handler,
    //     .user_ctx = NULL
    // };
    // httpd_register_uri_handler(server, &uri_get);

    // Suspend the task, the HTTP server runs in its own thread
    vTaskSuspend(NULL);
}

// void http_client_task(void *pvParameters) {
//     // Set up HTTP client here
//     // Send OTA updates
//     // ...
//     vTaskDelete(NULL); // Delete this task if it's no longer needed
// }

void app_main(void) {
    // Initialize NVS, WiFi, or other components here
    ESP_ERROR_CHECK(nvs_flash_init());

    // Initialize TCP/IP network interface (required for WiFi and HTTP server)
    ESP_ERROR_CHECK(esp_netif_init());

    // Create the event loop (required for system events)
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    //spiffs
    init_spiffs();

    // Start WiFi setup task
    xTaskCreate(&wifi_setup_task, "wifi_setup_task", 16384, NULL, 5, NULL);

    // Start HTTP server task
    xTaskCreate(&http_server_task, "http_server_task", 16384, NULL, 5, NULL);

    // Start HTTP client task
    // xTaskCreate(&http_client_task, "http_client_task", 8192, NULL, 5, NULL);
}