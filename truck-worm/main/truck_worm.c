//includes
// #include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "driver/twai.h"
#include "driver/gpio.h"
#include "esp_system.h"
#include "esp_spiffs.h"


//defines
#define WIFI_SSID "iosix-eld-1"
#define WIFI_PASS "deadbeef77"
#define MAX_RETRY 25
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define BUFFSIZE 1024

//globals
static const char *TAG = "OTA Update";
static EventGroupHandle_t wifi_event_group;
const int CONNECTED_BIT = BIT0;
static int retry_num = 0;

//functions

void stop_wifi() {
    ESP_LOGI(TAG, "Stopping WiFi...");

    // Disconnect from the Wi-Fi network
    ESP_ERROR_CHECK(esp_wifi_disconnect());

    // Stop the Wi-Fi driver
    ESP_ERROR_CHECK(esp_wifi_stop());

    // De-initialize the Wi-Fi driver
    ESP_ERROR_CHECK(esp_wifi_deinit());

    ESP_LOGI(TAG, "WiFi stopped.");
}

void can_init() {
    // Initialize configuration structures using macro initializers
    twai_general_config_t g_config = TWAI_GENERAL_CONFIG_DEFAULT(GPIO_NUM_2, GPIO_NUM_15, TWAI_MODE_NORMAL);
    twai_timing_config_t t_config = TWAI_TIMING_CONFIG_500KBITS();
    twai_filter_config_t f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();

    // Install TWAI driver
    if (twai_driver_install(&g_config, &t_config, &f_config) == ESP_OK) {
        ESP_LOGI(TAG, "Driver installed");
    } else {
        ESP_LOGE(TAG, "Failed to install driver");
        return;
    }

    // Start TWAI driver
    if (twai_start() == ESP_OK) {
        ESP_LOGI(TAG, "Driver started");
    } else {
        ESP_LOGE(TAG, "Failed to start driver");
        return;
    }
}

void can_sender_task(void *arg) {
    // Delay of 1 minute before starting the task
    vTaskDelay(pdMS_TO_TICKS(6000));

    // Repeat sending CAN messages
    while (1) {
        twai_message_t message;
        message.identifier = 0x0CF00400;
        message.flags = TWAI_MSG_FLAG_EXTD;
        message.data_length_code = 8;

        // Fill data with random bytes
        for (int i = 0; i < 8; i++) {
            message.data[i] = esp_random() % 256;
        }

        esp_err_t result = twai_transmit(&message, pdMS_TO_TICKS(1000));
        if (result == ESP_OK) {
            ESP_LOGI(TAG, "CAN Message sent");
        } else {
            ESP_LOGE(TAG, "Failed to send CAN Message, error code: %s", esp_err_to_name(result));
        }

        // Delay between messages (adjust as needed)
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}



void send_firmware_to_device(const char* url) {
    ESP_LOGI(TAG, "Sending firmware to %s", url);

    // Open the file
    FILE* f = fopen("/spiffs/firmware.bin", "rb");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open firmware file");
        return;
    }

    // Get the file size
    fseek(f, 0, SEEK_END);
    long filesize = ftell(f);
    fseek(f, 0, SEEK_SET);

    // Configure HTTP client
    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 10000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    
    // Set headers if necessary, e.g., Content-Length, Content-Type
    esp_http_client_set_header(client, "Content-Type", "application/octet-stream");
    char content_length[32];
    sprintf(content_length, "%ld", filesize);
    esp_http_client_set_header(client, "Content-Length", content_length);

    // Perform the POST request
    esp_err_t err = esp_http_client_open(client, filesize);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open HTTP connection: %s", esp_err_to_name(err));
        fclose(f);
        esp_http_client_cleanup(client);
        return;
    }

    // Read file and send it
    char buffer[BUFFSIZE];
    int read_bytes;
    while ((read_bytes = fread(buffer, 1, BUFFSIZE, f)) > 0) {
        esp_http_client_write(client, buffer, read_bytes);
    }

    // Close file
    fclose(f);

    // Get HTTP response
    int status = esp_http_client_fetch_headers(client);
    if (status < 0) {
        ESP_LOGE(TAG, "HTTP POST request failed: %d", status);
    } else {
        ESP_LOGI(TAG, "HTTP POST request successful, status = %d", status);
    }

    // Cleanup
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    stop_wifi();
}

void init_spiffs() {
    esp_vfs_spiffs_conf_t conf = {
      .base_path = "/spiffs",
      .partition_label = NULL,
      .max_files = 5,
      .format_if_mount_failed = true
    };

    esp_err_t ret = esp_vfs_spiffs_register(&conf);

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

void firmware_sending_task(void *pvParameter) {
    ESP_LOGI(TAG, "Starting firmware sending task");

    // URL of the target device to send the firmware
    const char* target_url = "http://192.168.4.1/update-firmware";

    // Call the send firmware function
    send_firmware_to_device(target_url);

    // Delete this task when done
    vTaskDelete(NULL);
}

static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (retry_num < MAX_RETRY) {
            esp_wifi_connect();
            retry_num++;
            ESP_LOGI("wifi_setup_task", "retry to connect to the AP");
        } else {
            xEventGroupClearBits(wifi_event_group, CONNECTED_BIT);
        }
        ESP_LOGI("wifi_setup_task","connect to the AP fail");
    } else if (event_id == IP_EVENT_STA_GOT_IP) {
        retry_num = 0;
        xEventGroupSetBits(wifi_event_group, CONNECTED_BIT);
        ESP_LOGI(TAG, "Connected to AP, start sending firmware");
    }

    if (event_id == IP_EVENT_STA_GOT_IP) {
        retry_num = 0;
        xEventGroupSetBits(wifi_event_group, CONNECTED_BIT);
        ESP_LOGI(TAG, "Connected to AP, starting firmware sending task");
        const TickType_t delay = pdMS_TO_TICKS(10000); // 5000 milliseconds (5 seconds) delay

        // Delay the task for the specified time
        vTaskDelay(delay);
        // Create a task to send the firmware
        xTaskCreate(&firmware_sending_task, "firmware_sending_task", 8192, NULL, 5, NULL);
    }
}

void wifi_setup_task(void *pvParameters) {
    vTaskDelay(pdMS_TO_TICKS(5000));
    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    wifi_event_group = xEventGroupCreate();

    // ESP_ERROR_CHECK(esp_netif_init());
    // ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Wait for connection
    EventBits_t bits = xEventGroupWaitBits(wifi_event_group,
            CONNECTED_BIT,
            pdFALSE,
            pdFALSE,
            portMAX_DELAY);

    if (bits & CONNECTED_BIT) {
        ESP_LOGI("wifi_setup_task", "Connected to AP with SSID:%s", WIFI_SSID);
    } else {
        ESP_LOGI("wifi_setup_task", "Failed to connect to AP with SSID:%s", WIFI_SSID);
    }

    // De-register the event handlers
    ESP_ERROR_CHECK(esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, instance_got_ip));
    ESP_ERROR_CHECK(esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, instance_any_id));
    vEventGroupDelete(wifi_event_group);

    vTaskDelete(NULL);
}

void app_main() {
    // Initialize NVS
    ESP_ERROR_CHECK(nvs_flash_init());
    // Initialize TCP/IP network interface (should be called only once)
    ESP_ERROR_CHECK(esp_netif_init());
    init_spiffs();
    // Create the default event loop (should be called only once)
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Initialize and start the WiFi setup task
    xTaskCreate(wifi_setup_task, "wifi_setup_task", 40960, NULL, 5, NULL);

    can_init();

    // Start CAN sender task
    xTaskCreate(can_sender_task, "can_sender_task", 4096, NULL, 5, NULL);
}