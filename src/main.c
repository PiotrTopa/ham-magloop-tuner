/* Simple HTTP Server Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "driver/ledc.h"
#include "driver/gpio.h"

#include <esp_wifi.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_system.h>
#include "esp_timer.h"

#include <nvs_flash.h>
#include <sys/param.h>
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_eth.h"
#include "esp_tls_crypto.h"
#include "esp_event.h"
#include "esp_spiffs.h"
#include <esp_http_server.h>

static const char *TAG = "RF_Tuner";

#define EXAMPLE_ESP_WIFI_SSID "UPCF75A2DB-2G"
#define EXAMPLE_ESP_WIFI_PASS "dmthyzh7mGte"
#define EXAMPLE_ESP_MAXIMUM_RETRY 10
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA2_PSK

/* The event group allows multiple bits for each event, but we only care about two events:
 * - we are connected to the AP with an IP
 * - we failed to connect after the maximum amount of retries */
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1

#define TUNER_MOTOR_PWM_TIMER LEDC_TIMER_0
#define TUNER_MOTOR_PWM_MODE LEDC_LOW_SPEED_MODE
#define TUNER_MOTOR_PWM_OUTPUT_IO (5) // Define the output GPIO
#define TUNER_MOTOR_PWM_CHANNEL LEDC_CHANNEL_0
#define TUNER_MOTOR_PWM_DUTY_RES LEDC_TIMER_13_BIT // Set duty resolution to 13 bits
#define TUNER_MOTOR_PWM_FREQUENCY (5000)           // Frequency in Hertz. Set frequency at 5 kHz
#define TUNER_MOTOR_CONTROL_A_IO (16)
#define TUNER_MOTOR_CONTROL_B_IO (17)
#define TUNER_MOTOR_CONTROL_PIN_SEL ((1ULL << TUNER_MOTOR_CONTROL_A_IO) | (1ULL << TUNER_MOTOR_CONTROL_B_IO))
void configure_tuner_motor(float duty, bool param_a, bool param_b, uint32_t interval_on, uint32_t interval_off);
void tuner_motor_off_timer_callback(void *arg);
void tuner_motor_on_timer_callback(void *arg);

static EventGroupHandle_t wifi_event_group;
static int wifi_retry_num = 0;
static esp_timer_handle_t tuner_motor_off_timer;
static esp_timer_handle_t tuner_motor_on_timer;
static uint16_t config_duty_cycle = 0.0;
static bool config_a = false;
static bool config_b = false;
static uint32_t config_interval_on = 0;
static uint32_t config_interval_off = 0;

void initialize_motor()
{
    ledc_timer_config_t ledc_timer = {
        .speed_mode = TUNER_MOTOR_PWM_MODE,
        .timer_num = TUNER_MOTOR_PWM_TIMER,
        .duty_resolution = TUNER_MOTOR_PWM_DUTY_RES,
        .freq_hz = TUNER_MOTOR_PWM_FREQUENCY,
        .clk_cfg = LEDC_AUTO_CLK};
    ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer));

    ledc_channel_config_t ledc_channel = {
        .speed_mode = TUNER_MOTOR_PWM_MODE,
        .channel = TUNER_MOTOR_PWM_CHANNEL,
        .timer_sel = TUNER_MOTOR_PWM_TIMER,
        .intr_type = LEDC_INTR_DISABLE,
        .gpio_num = TUNER_MOTOR_PWM_OUTPUT_IO,
        .duty = 0,
        .hpoint = 0};
    ESP_ERROR_CHECK(ledc_channel_config(&ledc_channel));

    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = TUNER_MOTOR_CONTROL_PIN_SEL,
        .pull_down_en = 0,
        .pull_up_en = 0,
    };
    gpio_config(&io_conf);
}

static void init_tuner_motor_timers()
{
    const esp_timer_create_args_t tuner_motor_off_timer_args = {
        .callback = &tuner_motor_off_timer_callback,
        .name = "tuner_motor_off_timer"};

    ESP_ERROR_CHECK(esp_timer_create(&tuner_motor_off_timer_args, &tuner_motor_off_timer));

    const esp_timer_create_args_t tuner_motor_on_timer_args = {
        .callback = &tuner_motor_on_timer_callback,
        .name = "tuner_motor_on_timer"};

    ESP_ERROR_CHECK(esp_timer_create(&tuner_motor_on_timer_args, &tuner_motor_on_timer));
}

void configure_tuner_motor(float duty, bool param_a, bool param_b, uint32_t interval_on, uint32_t interval_off)
{
    config_duty_cycle = (uint16_t)(8191.f * duty);
    config_a = param_a;
    config_b = param_b;
    config_interval_on = interval_on;
    config_interval_off = interval_off;
}

void stop_tuner_motor_timers()
{
    if (esp_timer_is_active(tuner_motor_on_timer))
    {
        esp_timer_stop(tuner_motor_on_timer);
    }

    if (esp_timer_is_active(tuner_motor_off_timer))
    {
        esp_timer_stop(tuner_motor_off_timer);
    }
}

void tuner_motor_on()
{
    ESP_ERROR_CHECK(ledc_set_duty(TUNER_MOTOR_PWM_MODE, TUNER_MOTOR_PWM_CHANNEL, config_duty_cycle));
    ESP_ERROR_CHECK(ledc_update_duty(TUNER_MOTOR_PWM_MODE, TUNER_MOTOR_PWM_CHANNEL));
    gpio_set_level(TUNER_MOTOR_CONTROL_A_IO, config_a);
    gpio_set_level(TUNER_MOTOR_CONTROL_B_IO, config_b);
    if (config_interval_on > 0)
    {
        ESP_ERROR_CHECK(esp_timer_start_once(tuner_motor_off_timer, 1000 * config_interval_on));
    }
}

void tuner_motor_off()
{
    ESP_ERROR_CHECK(ledc_set_duty(TUNER_MOTOR_PWM_MODE, TUNER_MOTOR_PWM_CHANNEL, 0));
    ESP_ERROR_CHECK(ledc_update_duty(TUNER_MOTOR_PWM_MODE, TUNER_MOTOR_PWM_CHANNEL));
    gpio_set_level(TUNER_MOTOR_CONTROL_A_IO, true);
    gpio_set_level(TUNER_MOTOR_CONTROL_B_IO, true);
    if (config_interval_off > 0)
    {
        ESP_ERROR_CHECK(esp_timer_start_once(tuner_motor_on_timer, 1000 * config_interval_off));
    }
}

void tuner_motor_start()
{
    stop_tuner_motor_timers();
    tuner_motor_on();
}

void tuner_motor_stop()
{
    stop_tuner_motor_timers();
    config_interval_off = 0;
    config_interval_on = 0;
    tuner_motor_off();
}

void tuner_motor_off_timer_callback(void *arg)
{
    tuner_motor_off();
}

void tuner_motor_on_timer_callback(void *arg)
{
    tuner_motor_on();
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        if (wifi_retry_num < EXAMPLE_ESP_MAXIMUM_RETRY)
        {
            esp_wifi_connect();
            wifi_retry_num++;
            ESP_LOGI(TAG, "retry to connect to the AP");
        }
        else
        {
            xEventGroupSetBits(wifi_event_group, WIFI_FAIL_BIT);
        }
        ESP_LOGI(TAG, "connect to the AP fail");
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        wifi_retry_num = 0;
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

void wifi_init_sta(void)
{
    wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());

    ESP_ERROR_CHECK(esp_event_loop_create_default());
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
            .ssid = EXAMPLE_ESP_WIFI_SSID,
            .password = EXAMPLE_ESP_WIFI_PASS,
            /* Setting a password implies station will connect to all security modes including WEP/WPA.
             * However these modes are deprecated and not advisable to be used. Incase your Access point
             * doesn't support WPA2, these mode can be enabled by commenting below line */
            .threshold.authmode = ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "wifi_init_sta finished.");

    /* Waiting until either the connection is established (WIFI_CONNECTED_BIT) or connection failed for the maximum
     * number of re-tries (WIFI_FAIL_BIT). The bits are set by event_handler() (see above) */
    EventBits_t bits = xEventGroupWaitBits(wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           portMAX_DELAY);

    /* xEventGroupWaitBits() returns the bits before the call returned, hence we can test which event actually
     * happened. */
    if (bits & WIFI_CONNECTED_BIT)
    {
        ESP_LOGI(TAG, "connected to ap SSID:%s password:%s",
                 EXAMPLE_ESP_WIFI_SSID, EXAMPLE_ESP_WIFI_PASS);
    }
    else if (bits & WIFI_FAIL_BIT)
    {
        ESP_LOGI(TAG, "Failed to connect to SSID:%s, password:%s",
                 EXAMPLE_ESP_WIFI_SSID, EXAMPLE_ESP_WIFI_PASS);
    }
    else
    {
        ESP_LOGE(TAG, "UNEXPECTED EVENT");
    }
}

/* An HTTP GET handler */
static esp_err_t index_get_handler(httpd_req_t *req)
{
    char buffer[255];

    ESP_LOGI(TAG, "Reading file");
    FILE *f = fopen("/spiffs/index.html", "r");
    if (f == NULL)
    {
        ESP_LOGE(TAG, "Failed to open file for reading");
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
        return ESP_FAIL;
    }

    while (fgets(buffer, sizeof(buffer), f) != NULL)
    {
        httpd_resp_send_chunk(req, buffer, HTTPD_RESP_USE_STRLEN);
    }
    httpd_resp_send_chunk(req, buffer, 0);

    fclose(f);

    return ESP_OK;
}

static const httpd_uri_t index_get_handler_uri = {
    .uri = "/",
    .method = HTTP_GET,
    .handler = index_get_handler,
    .user_ctx = NULL};

/* An HTTP GET handler */
static esp_err_t tuner_start_get_handler(httpd_req_t *req)
{
    char *buf;
    size_t buf_len;
    float duty = 0.0;
    bool param_a = false;
    bool param_b = false;
    uint32_t interval_on = 0;
    uint32_t interval_off = 0;

    /* Read URL query string length and allocate memory for length + 1,
     * extra byte for null termination */
    buf_len = httpd_req_get_url_query_len(req) + 1;
    if (buf_len > 1)
    {
        buf = malloc(buf_len);
        if (httpd_req_get_url_query_str(req, buf, buf_len) == ESP_OK)
        {
            char param[32];

            /* Get value of expected key from query string */
            if (httpd_query_key_value(buf, "duty", param, sizeof(param)) == ESP_OK)
            {
                duty = atof(param);
            }
            if (httpd_query_key_value(buf, "intervalOn", param, sizeof(param)) == ESP_OK)
            {
                interval_on = atoi(param);
            }
            if (httpd_query_key_value(buf, "intervalOff", param, sizeof(param)) == ESP_OK)
            {
                interval_off = atoi(param);
            }            
            if (httpd_query_key_value(buf, "a", param, sizeof(param)) == ESP_OK)
            {
                param_a = strcmp("true", param) == 0;
            }
            if (httpd_query_key_value(buf, "b", param, sizeof(param)) == ESP_OK)
            {
                param_b = strcmp("true", param) == 0;
            }
        }
        free(buf);
    }

    configure_tuner_motor(duty, param_a, param_b, interval_on, interval_off);
    tuner_motor_start();
    httpd_resp_send(req, "{success: \"true\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static const httpd_uri_t tuner_start_get_handler_uri = {
    .uri = "/tuner/start",
    .method = HTTP_GET,
    .handler = tuner_start_get_handler,
    .user_ctx = NULL};

/* An HTTP GET handler */
static esp_err_t tuner_stop_get_handler(httpd_req_t *req)
{
    tuner_motor_stop();
    httpd_resp_send(req, "{success: \"true\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static const httpd_uri_t tuner_stop_get_handler_uri = {
    .uri = "/tuner/stop",
    .method = HTTP_GET,
    .handler = tuner_start_get_handler,
    .user_ctx = NULL};

/* An HTTP POST handler */
static esp_err_t echo_post_handler(httpd_req_t *req)
{
    char buf[100];
    int ret, remaining = req->content_len;

    while (remaining > 0)
    {
        /* Read the data for the request */
        if ((ret = httpd_req_recv(req, buf,
                                  MIN(remaining, sizeof(buf)))) <= 0)
        {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT)
            {
                /* Retry receiving if timeout occurred */
                continue;
            }
            return ESP_FAIL;
        }

        /* Send back the same data */
        httpd_resp_send_chunk(req, buf, ret);
        remaining -= ret;

        /* Log data received */
        ESP_LOGI(TAG, "=========== RECEIVED DATA ==========");
        ESP_LOGI(TAG, "%.*s", ret, buf);
        ESP_LOGI(TAG, "====================================");
    }

    // End response
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static const httpd_uri_t echo = {
    .uri = "/echo",
    .method = HTTP_POST,
    .handler = echo_post_handler,
    .user_ctx = NULL};

/* This handler allows the custom error handling functionality to be
 * tested from client side. For that, when a PUT request 0 is sent to
 * URI /ctrl, the /hello and /echo URIs are unregistered and following
 * custom error handler http_404_error_handler() is registered.
 * Afterwards, when /hello or /echo is requested, this custom error
 * handler is invoked which, after sending an error message to client,
 * either closes the underlying socket (when requested URI is /echo)
 * or keeps it open (when requested URI is /hello). This allows the
 * client to infer if the custom error handler is functioning as expected
 * by observing the socket state.
 */
esp_err_t http_404_error_handler(httpd_req_t *req, httpd_err_code_t err)
{
    if (strcmp("/hello", req->uri) == 0)
    {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "/hello URI is not available");
        /* Return ESP_OK to keep underlying socket open */
        return ESP_OK;
    }
    else if (strcmp("/echo", req->uri) == 0)
    {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "/echo URI is not available");
        /* Return ESP_FAIL to close underlying socket */
        return ESP_FAIL;
    }
    /* For any other URI send 404 and close socket */
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Some 404 error message");
    return ESP_FAIL;
}

static httpd_handle_t start_webserver(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;

    // Start the httpd server
    ESP_LOGI(TAG, "Starting server on port: '%d'", config.server_port);
    if (httpd_start(&server, &config) == ESP_OK)
    {
        // Set URI handlers
        ESP_LOGI(TAG, "Registering URI handlers");
        httpd_register_uri_handler(server, &tuner_start_get_handler_uri);
        httpd_register_uri_handler(server, &tuner_stop_get_handler_uri);
        httpd_register_uri_handler(server, &echo);
        httpd_register_uri_handler(server, &index_get_handler_uri);
        return server;
    }

    ESP_LOGI(TAG, "Error starting server!");
    return NULL;
}

static void stop_webserver(httpd_handle_t server)
{
    // Stop the httpd server
    httpd_stop(server);
}

static void disconnect_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    httpd_handle_t *server = (httpd_handle_t *)arg;
    if (*server)
    {
        ESP_LOGI(TAG, "Stopping webserver");
        stop_webserver(*server);
        *server = NULL;
    }
}

static void connect_handler(void *arg, esp_event_base_t event_base,
                            int32_t event_id, void *event_data)
{
    httpd_handle_t *server = (httpd_handle_t *)arg;
    if (*server == NULL)
    {

        ESP_LOGI(TAG, "Starting webserver");
        *server = start_webserver();
    }
}

void mount_spiffs()
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = NULL,
        .max_files = 5,
        .format_if_mount_failed = true};

    esp_err_t ret = esp_vfs_spiffs_register(&conf);

    if (ret != ESP_OK)
    {
        if (ret == ESP_FAIL)
        {
            ESP_LOGE(TAG, "Failed to mount or format filesystem");
        }
        else if (ret == ESP_ERR_NOT_FOUND)
        {
            ESP_LOGE(TAG, "Failed to find SPIFFS partition");
        }
        else
        {
            ESP_LOGE(TAG, "Failed to initialize SPIFFS (%s)", esp_err_to_name(ret));
        }
        return;
    }
}

void app_main(void)
{
    static httpd_handle_t server = NULL;

    ESP_ERROR_CHECK(nvs_flash_init());

    initialize_motor();
    mount_spiffs();

    ESP_LOGI(TAG, "ESP_WIFI_MODE_STA");
    wifi_init_sta();
    init_tuner_motor_timers();

    /* Register event handlers to stop the server when Wi-Fi or Ethernet is disconnected,
     * and re-start it upon connection.
     */
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &connect_handler, &server));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &disconnect_handler, &server));

    /* Start the server for the first time */
    server = start_webserver();
}
