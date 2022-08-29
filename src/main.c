// https://core.telegram.org/bots/api
// https://datasheetspdf.com/pdf-file/775435/ETC/HC-SR501/1
// https://datasheetspdf.com/pdf-file/775434/ETC/HC-SR501/1

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "esp_http_client.h"
#include "esp_crt_bundle.h"

#include "driver/gpio.h"

#include "lwip/err.h"
#include "lwip/sys.h"

#include "urlencode.h"

#define ESP_WIFI_SSID "" // CHANGEME
#define ESP_WIFI_PASS "" // CHANGEME

#define TG_AUTH_TOKEN ""     // CHANGEME
#define TG_TARGET_CHAT_ID "" // CHANGEME

#define SENSOR_GPIO GPIO_NUM_26

#define BIT_0 (1 << 0)
#define BIT_1 (1 << 4)

#define F_WIFI_CONNECTED BIT_0
#define F_WIFI_NOT_CONNECTED BIT_1

#define FREERTOS_STACK_SIZE 4096

static const char *LTAG = "wifi station";

EventGroupHandle_t wifi_event_group;

// Keeps track of connection retries
const unsigned short max_retry_count = 8;
static unsigned short retry_count = 0;

void event_handler(void *event_handler_arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
  {
    ESP_LOGI(LTAG, "Event: WIFI_EVENT_STA_START");
    ESP_ERROR_CHECK(esp_wifi_connect());
  }
  else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
  {
    ESP_LOGI(LTAG, "Event: WIFI_EVENT_STA_DISCONNECTED");
    if (retry_count < max_retry_count)
    {
      ESP_LOGI(LTAG, "Trying to reconnect");
      retry_count++;
      ESP_ERROR_CHECK(esp_wifi_connect());
    }
    else
    {
      ESP_LOGI(LTAG, "Failed to reconnect too many times");
      xEventGroupSetBits(wifi_event_group, F_WIFI_NOT_CONNECTED);
    }
  }
  else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED)
  {
    ESP_LOGI(LTAG, "Event: WIFI_EVENT_STA_CONNECTED");
    retry_count = 0;
  }
  else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
  {
    ESP_LOGI(LTAG, "Event: IP_EVENT_STA_GOT_IP");
    xEventGroupSetBits(wifi_event_group, F_WIFI_CONNECTED);
  }
}

bool wifi_init_sta()
{
  wifi_event_group = xEventGroupCreate();

  // Initialize
  ESP_LOGI(LTAG, "Initializing");
  ESP_ERROR_CHECK(esp_netif_init());

  ESP_ERROR_CHECK(esp_event_loop_create_default());

  esp_netif_create_default_wifi_sta();

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));

  // Config
  ESP_LOGI(LTAG, "Config");
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

  wifi_config_t wifi_config = {
      .sta = {
          .ssid = ESP_WIFI_SSID,
          .password = ESP_WIFI_PASS,
      },
  };
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

  // Register event handler
  ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, NULL));
  ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, NULL));

  // Start
  ESP_LOGI(LTAG, "Start");
  ESP_ERROR_CHECK(esp_wifi_start());

  EventBits_t uxBits = xEventGroupWaitBits(wifi_event_group, F_WIFI_CONNECTED | F_WIFI_NOT_CONNECTED, pdFALSE, pdFALSE, portMAX_DELAY);
  if ((uxBits & F_WIFI_CONNECTED) != 0)
  {
    return true;
  }
  else
  {
    return false;
  }
}

void send_notification(void *pvParameters)
{
  char *text = pvParameters;

  // Urlencode the text
  int text_urlencoded_max_length = strlen(text) * 2; // TODO: Find a better way to determine the max length of the urlencoded text
  char *text_urlencoded = malloc(text_urlencoded_max_length);
  int text_urlencoded_length = urlencode(text_urlencoded, text_urlencoded_max_length, text);

  // Generate the final url
  char *url = malloc(56 + strlen(TG_AUTH_TOKEN) + strlen(TG_TARGET_CHAT_ID) + text_urlencoded_length);
  sprintf(url, "https://api.telegram.org/bot%s/sendMessage?chat_id=%s&text=%s", TG_AUTH_TOKEN, TG_TARGET_CHAT_ID, text_urlencoded);

  free(text_urlencoded);

  esp_http_client_config_t config = {
      .url = url,
      .crt_bundle_attach = esp_crt_bundle_attach,
  };
  esp_http_client_handle_t client = esp_http_client_init(&config);

  esp_err_t err = esp_http_client_perform(client);
  if (err == ESP_OK)
  {
    ESP_LOGI(LTAG, "HTTP GET Status = %d, content_length = %d",
             esp_http_client_get_status_code(client),
             esp_http_client_get_content_length(client));
  }
  else
  {
    ESP_LOGE(LTAG, "HTTP GET request failed: %s", esp_err_to_name(err));
  }

  // Free resources
  ESP_ERROR_CHECK(esp_http_client_cleanup(client));
  free(url);

  vTaskDelete(NULL);
}

void app_main()
{
  // Initialize NVS
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
  {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);

  ESP_LOGI(LTAG, "Starting ESP32 WiFi Station");
  bool wifi_connected = wifi_init_sta();

  if (!wifi_connected)
  {
    ESP_LOGI(LTAG, "WiFi not connected. Exiting.");
    return;
  }

  ESP_ERROR_CHECK(gpio_set_direction(SENSOR_GPIO, GPIO_MODE_INPUT));

  int oldLevel = gpio_get_level(SENSOR_GPIO);
  while (true)
  {

    int level = gpio_get_level(SENSOR_GPIO);
    printf("Level: %i\n", level);

    if (level == 1 && oldLevel != level)
    {
      ESP_LOGI(LTAG, "Sending movement detected notification");
      xTaskCreate(send_notification, "tgnotif", FREERTOS_STACK_SIZE, (void *)"Movement detected", 2, NULL);
    }

    oldLevel = level;
    vTaskDelay(500 / portTICK_PERIOD_MS);
  }
}
