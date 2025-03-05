#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include <sys/socket.h>
#include "esp_eth.h"
#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "lwip/netdb.h"
#include "mqtt_client.h"
#include "cJSON.h"

// Wi-Fi配置
#define ESP_WIFI_STA_SSID CONFIG_ESP_WIFI_STA_SSID
#define ESP_WIFI_STA_PASSWD CONFIG_ESP_WIFI_STA_PASSWD

// MQTT配置
#define MQTT_CLIENT_ID "ESP32-%s"
#define MQTT_USERNAME "ESP32-%s"
#define MQTT_PASSWD "ESP32-%s"
#define MQTT_HOST_URL CONFIG_MQTT_HOST_URL

// MQTT主题
#define MQTT_STATE_TOPIC_FMT "home/device/%s/state"
#define MQTT_COMMAND_TOPIC_FMT "home/device/%s/set"

// 设备ID
char DEVICE_ID[24] = "";

static const char *TAG = "app";

// 定义一个结构体来存储MQTT主题
typedef struct
{
	char client_id[64];
	char username[64];
	char passwd[64];
	char state_topic[64];
	char command_topic[64];
} MqttConfig;

// 全局变量
MqttConfig mqtt_configs;

// MQTT客户端句柄
esp_mqtt_client_handle_t mqtt_client;

// 定义传感器数据结构体
struct Data
{
	float temperature;				 // 当前环境温度（摄氏度）
	float humidity;						 // 当前环境湿度（百分比）
	float pressure;						 // 当前环境气压（帕斯卡）
	float lightIntensity;			 // 当前光照强度（勒克斯）
	float smokeDensity;				 // 当前烟雾浓度（PPM）
	char relayStatus[4];			 // 当前继电器状态（开或关）
	char circuitBreakerStatus; // 当前断路器状态（开或关）
};

// 初始化数据结构实例
struct Data systemData = {
		.temperature = 25.0,
		.humidity = 50.0,
		.pressure = 101325.0,
		.lightIntensity = 300,
		.smokeDensity = 0,
		.relayStatus = {0, 0, 0, 0},
		.circuitBreakerStatus = 0};

// 继电器状态（从继电器模块获取的全局变量）
bool relay_state[4] = {0, 0, 0, 0};

// 断路器状态（全局变量）
bool circuit_breaker_state = 0; // 初始状态为关闭

// 生成随机浮点数
float random_float(float min, float max)
{
	return min + (esp_random() % 1000) / 1000.0f * (max - min);
}

// 设置MQTT配置
void setup_mqtt_configs(MqttConfig *configs, const char *id)
{
	assert(configs != NULL);
	assert(id != NULL);
	snprintf(configs->client_id, sizeof(configs->client_id), MQTT_CLIENT_ID, id);
	snprintf(configs->username, sizeof(configs->username), MQTT_USERNAME, id);
	snprintf(configs->passwd, sizeof(configs->passwd), MQTT_PASSWD, id);
	snprintf(configs->state_topic, sizeof(configs->state_topic), MQTT_STATE_TOPIC_FMT, id);
	snprintf(configs->command_topic, sizeof(configs->command_topic), MQTT_COMMAND_TOPIC_FMT, id);
}

// 设备注册
static void mqtt_app_register()
{
	// 设备标识符
	char device_identifier[64];
	snprintf(device_identifier, sizeof(device_identifier), "device_%s", DEVICE_ID);

	// 设备信息
	cJSON *device = cJSON_CreateObject();
	cJSON_AddStringToObject(device, "name", "综合环境监测控制设备");
	cJSON_AddStringToObject(device, "model", "6S8Y91");
	cJSON_AddStringToObject(device, "manufacturer", "若甫科技有限公司");
	cJSON *identifiers = cJSON_CreateArray();
	cJSON_AddItemToArray(identifiers, cJSON_CreateString(device_identifier));
	cJSON_AddItemToObject(device, "identifiers", identifiers);

	// 传感器注册
	const char *sensor_names[] = {"温度", "湿度", "气压", "光照", "烟感"};
	const char *sensor_key[] = {"temperature", "humidity", "pressure", "lightIntensity", "smokeDensity"};
	const char *sensor_units[] = {"°C", "%", "hPa", "lx", "ppm"};
	for (int i = 0; i < 5; i++)
	{
		cJSON *config = cJSON_CreateObject();
		char unique_id[64], name[64], state_topic[128], value_template[128];
		snprintf(unique_id, sizeof(unique_id), "device_%s_sensor%d", DEVICE_ID, i);
		snprintf(name, sizeof(name), "%s", sensor_names[i]);
		snprintf(state_topic, sizeof(state_topic), "home/device/%s/state", DEVICE_ID);
		snprintf(value_template, sizeof(value_template), "{{ value_json.%s }}", sensor_key[i]);

		cJSON_AddStringToObject(config, "name", name);
		cJSON_AddStringToObject(config, "unique_id", unique_id);
		cJSON_AddStringToObject(config, "state_topic", state_topic);
		cJSON_AddStringToObject(config, "value_template", value_template);
		cJSON_AddStringToObject(config, "unit_of_measurement", sensor_units[i]);
		cJSON_AddItemToObject(config, "device", cJSON_Duplicate(device, 1));

		// 发布注册消息
		char register_topic[128];
		snprintf(register_topic, sizeof(register_topic), "homeassistant/sensor/%s/config", unique_id);
		char *config_str = cJSON_PrintUnformatted(config);
		esp_mqtt_client_publish(mqtt_client, register_topic, config_str, 0, 1, 0);

		ESP_LOGI(TAG, "Register command: %s", config_str);
		cJSON_free(config_str);
		cJSON_Delete(config);
	}

	// 继电器注册
	for (int i = 1; i <= 4; i++)
	{
		cJSON *config = cJSON_CreateObject();
		char unique_id[64], name[64], state_topic[128], command_topic[128], value_template[128], command_template[128];
		snprintf(unique_id, sizeof(unique_id), "device_%s_relay_%d", DEVICE_ID, i);
		snprintf(name, sizeof(name), "继电器 %d", i);
		snprintf(state_topic, sizeof(state_topic), "home/device/%s/state", DEVICE_ID);
		snprintf(command_topic, sizeof(command_topic), "home/device/%s/set", DEVICE_ID);
		snprintf(value_template, sizeof(value_template), "{{ value_json.relayStatus_%d }}", i);
		snprintf(command_template, sizeof(command_template), "{ \"dalay_%d\": {{ value }} }", i);

		cJSON_AddStringToObject(config, "name", name);
		cJSON_AddStringToObject(config, "unique_id", unique_id);
		cJSON_AddStringToObject(config, "state_topic", state_topic);
		cJSON_AddStringToObject(config, "command_topic", command_topic);
		cJSON_AddStringToObject(config, "value_template", value_template);
		cJSON_AddStringToObject(config, "command_template", command_template);
		cJSON_AddNumberToObject(config, "payload_on", 1);
		cJSON_AddNumberToObject(config, "payload_off", 0);
		cJSON_AddItemToObject(config, "device", cJSON_Duplicate(device, 1));

		// 发布注册消息
		char register_topic[128];
		snprintf(register_topic, sizeof(register_topic), "homeassistant/switch/%s/config", unique_id);
		char *config_str = cJSON_PrintUnformatted(config);
		esp_mqtt_client_publish(mqtt_client, register_topic, config_str, 0, 1, 0);
		ESP_LOGI(TAG, "Register command: %s", config_str);
		cJSON_free(config_str);
		cJSON_Delete(config);
	}

	// 断路器注册
	cJSON *cb_config = cJSON_CreateObject();
	char cb_unique_id[64], cb_state_topic[128], cb_command_topic[128];
	snprintf(cb_unique_id, sizeof(cb_unique_id), "home/device_%s_circuit_breaker", DEVICE_ID);
	snprintf(cb_state_topic, sizeof(cb_state_topic), "home/device/%s/state", DEVICE_ID);
	snprintf(cb_command_topic, sizeof(cb_command_topic), "home/device/%s/set", DEVICE_ID);

	cJSON_AddStringToObject(cb_config, "name", "断路器");
	cJSON_AddStringToObject(cb_config, "unique_id", cb_unique_id);
	cJSON_AddStringToObject(cb_config, "state_topic", cb_state_topic);
	cJSON_AddStringToObject(cb_config, "command_topic", cb_command_topic);
	cJSON_AddStringToObject(cb_config, "value_template", "{{ value_json.circuitBreakerStatus }}");
	cJSON_AddStringToObject(cb_config, "command_template", "{ \"circuit_breaker\": {{ value }} }");
	cJSON_AddNumberToObject(cb_config, "payload_on", 1);
	cJSON_AddNumberToObject(cb_config, "payload_off", 0);
	cJSON_AddItemToObject(cb_config, "device", cJSON_Duplicate(device, 1));

	char cb_register_topic[128];
	snprintf(cb_register_topic, sizeof(cb_register_topic), "homeassistant/switch/%s/config", cb_unique_id);
	char *cb_config_str = cJSON_PrintUnformatted(cb_config);
	esp_mqtt_client_publish(mqtt_client, cb_register_topic, cb_config_str, 0, 1, 0);
	ESP_LOGI(TAG, "Register command: %s", cb_config_str);
	cJSON_free(cb_config_str);
	cJSON_Delete(cb_config);

	// 释放设备对象
	cJSON_Delete(device);
}

// 发送JSON数据
void send_json(struct Data *data)
{
	// 创建cJSON根对象
	cJSON *root = cJSON_CreateObject();
	if (root == NULL)
	{
		ESP_LOGE(TAG, "Failed to create JSON object");
		return;
	}

	char buffer[16]; // 缓冲区用于格式化字符串

	// 处理并格式化值为两位小数
	snprintf(buffer, sizeof(buffer), "%.2f", data->temperature);
	cJSON_AddStringToObject(root, "temperature", buffer);

	snprintf(buffer, sizeof(buffer), "%.2f", data->humidity);
	cJSON_AddStringToObject(root, "humidity", buffer);

	snprintf(buffer, sizeof(buffer), "%.2f", data->pressure);
	cJSON_AddStringToObject(root, "pressure", buffer);

	snprintf(buffer, sizeof(buffer), "%.2f", data->lightIntensity);
	cJSON_AddStringToObject(root, "lightIntensity", buffer);

	snprintf(buffer, sizeof(buffer), "%.2f", data->smokeDensity);
	cJSON_AddStringToObject(root, "smokeDensity", buffer);

	// 继电器状态
	for (int i = 0; i < 4; ++i)
	{
		char relayKey[16]; // 生成"relayStatus_1", "relayStatus_2"等
		sprintf(relayKey, "relayStatus_%d", i + 1);
		cJSON_AddNumberToObject(root, relayKey, relay_state[i]);
	}

	// 断路器状态
	cJSON_AddNumberToObject(root, "circuitBreakerStatus", data->circuitBreakerStatus ? 1 : 0);

	// 将JSON对象转换为字符串
	char *json_str = cJSON_Print(root);
	if (json_str == NULL)
	{
		ESP_LOGE(TAG, "Failed to print JSON object");
		cJSON_Delete(root);
		return;
	}

	// 打印JSON字符串
	ESP_LOGI(TAG, "JSON: %s", json_str);

	// 发送JSON字符串
	esp_mqtt_client_publish(mqtt_client, mqtt_configs.state_topic, json_str, 0, 1, 0);

	// 释放资源
	cJSON_Delete(root);
	free(json_str);
}

// 记录非零错误
static void log_error_if_nonzero(const char *message, int error_code)
{
	if (error_code != 0)
	{
		ESP_LOGE(TAG, "Last error %s: 0x%x", message, error_code);
	}
}

// 解析并打印JSON数据
void parse_and_print_json(const char *json_str)
{
	// 解析JSON字符串
	cJSON *json = cJSON_Parse(json_str);
	if (json == NULL)
	{
		const char *error_ptr = cJSON_GetErrorPtr();
		if (error_ptr != NULL)
		{
			fprintf(stderr, "Error before: %s\n", error_ptr);
		}
		return;
	}

	// 处理继电器状态
	for (int i = 0; i < 4; i++)
	{
		char key[10];
		snprintf(key, sizeof(key), "dalay_%d", i + 1); // 构造键名"dalay_1"至"dalay_4"

		cJSON *dalay = cJSON_GetObjectItemCaseSensitive(json, key);
		if (dalay && cJSON_IsNumber(dalay))
		{
			relay_state[i] = dalay->valueint;
			ESP_LOGI(TAG, "Relay %d 被控制，当前状态: %s", i + 1, relay_state[i] ? "开" : "关");
		}
	}

	// 处理断路器状态
	cJSON *circuit_breaker = cJSON_GetObjectItemCaseSensitive(json, "circuit_breaker");
	if (circuit_breaker && cJSON_IsNumber(circuit_breaker))
	{
		circuit_breaker_state = circuit_breaker->valueint;
		ESP_LOGI(TAG, "断路器被控制，当前状态: %s", circuit_breaker_state ? "开" : "关");
	}

	// 释放JSON解析对象
	cJSON_Delete(json);
}

// MQTT客户端事件处理函数
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
	ESP_LOGD(TAG, "Event dispatched from event loop base=%s, event_id=%" PRIi32 "", base, event_id);
	esp_mqtt_event_handle_t event = event_data;
	esp_mqtt_client_handle_t client = event->client;
	int msg_id;
	switch ((esp_mqtt_event_id_t)event_id)
	{
	// MQTT连接
	case MQTT_EVENT_CONNECTED:
		ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
		// 发送注册消息
		mqtt_app_register();
		// 订阅控制主题
		msg_id = esp_mqtt_client_subscribe(client, mqtt_configs.command_topic, 0);
		ESP_LOGI(TAG, "Sent subscribe successful, msg_id=%d", msg_id);
		break;
	// MQTT断开连接
	case MQTT_EVENT_DISCONNECTED:
		ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
		break;
	// MQTT已订阅
	case MQTT_EVENT_SUBSCRIBED:
		ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
		break;
	// MQTT取消订阅
	case MQTT_EVENT_UNSUBSCRIBED:
		ESP_LOGI(TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
		break;
	// MQTT已发布
	case MQTT_EVENT_PUBLISHED:
		ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
		break;
	// MQTT数据接收
	case MQTT_EVENT_DATA:
		ESP_LOGI(TAG, "MQTT_EVENT_DATA");
		printf("TOPIC=%.*s\r\n", event->topic_len, event->topic);
		printf("DATA=%.*s\r\n", event->data_len, event->data);
		// 确保数据是以null结尾的字符串
		char *json_data = (char *)malloc(event->data_len + 1);
		if (json_data == NULL)
		{
			ESP_LOGE(TAG, "Failed to allocate memory for JSON data");
			break;
		}
		memcpy(json_data, event->data, event->data_len);
		json_data[event->data_len] = '\0'; // 添加字符串终止符
		parse_and_print_json(json_data);
		// 读取断路器状态
		systemData.circuitBreakerStatus = circuit_breaker_state;
		// 打包并发送数据
		send_json(&systemData);
		free(json_data);
		break;
	// MQTT错误
	case MQTT_EVENT_ERROR:
		ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
		if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT)
		{
			log_error_if_nonzero("reported from esp-tls", event->error_handle->esp_tls_last_esp_err);
			log_error_if_nonzero("reported from tls stack", event->error_handle->esp_tls_stack_err);
			log_error_if_nonzero("captured as transport's socket errno", event->error_handle->esp_transport_sock_errno);
			ESP_LOGI(TAG, "Last errno string (%s)", strerror(event->error_handle->esp_transport_sock_errno));
		}
		break;
	default:
		ESP_LOGI(TAG, "Other event id:%d", event->event_id);
		break;
	}
}

// 启动MQTT客户端
static void mqtt_app_start(void)
{
	esp_mqtt_client_config_t mqtt_cfg = {
			.broker.address.uri = MQTT_HOST_URL,
			.credentials.client_id = mqtt_configs.client_id,
			.credentials.username = mqtt_configs.username,
			.credentials.authentication.password = mqtt_configs.passwd,
	};

	mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
	// 注册事件处理函数
	esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
	// 启动MQTT客户端
	esp_mqtt_client_start(mqtt_client);
}

// Wi-Fi事件回调函数
void WIFI_CallBack(void *event_handler_arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
	static uint8_t connect_count = 0;
	// Wi-Fi启动事件
	if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
	{
		ESP_LOGI("WIFI_EVENT", "WIFI_EVENT_STA_START");
		ESP_ERROR_CHECK(esp_wifi_connect());
	}
	// Wi-Fi断开事件
	if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
	{
		ESP_LOGI("WIFI_EVENT", "WIFI_EVENT_STA_DISCONNECTED");
		connect_count++;
		if (connect_count < 6)
		{
			vTaskDelay(1000 / portTICK_PERIOD_MS);
			ESP_ERROR_CHECK(esp_wifi_connect());
		}
		else
		{
			ESP_LOGI("WIFI_EVENT", "WIFI_EVENT_STA_DISCONNECTED 10 times");
		}
	}
	// Wi-Fi获取IP事件
	if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
	{
		ESP_LOGI("WIFI_EVENT", "WIFI_EVENT_STA_GOT_IP");
		ip_event_got_ip_t *info = (ip_event_got_ip_t *)event_data;
		ESP_LOGI("WIFI_EVENT", "got ip:" IPSTR "", IP2STR(&info->ip_info.ip));
		// 创建MQTT客户端
		mqtt_app_start();
	}
}

// Wi-Fi初始化
static void wifi_sta_init(void)
{
	ESP_ERROR_CHECK(esp_netif_init());

	// 注册Wi-Fi启动事件
	ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_STA_START, WIFI_CallBack, NULL, NULL));
	// 注册Wi-Fi断开事件
	ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, WIFI_CallBack, NULL, NULL));
	// 注册IP获取事件
	ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, WIFI_CallBack, NULL, NULL));

	// 初始化STA设备
	esp_netif_create_default_wifi_sta();

	// 初始化Wi-Fi
	wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
	ESP_ERROR_CHECK(esp_wifi_init(&cfg));

	// 配置STA设备
	wifi_config_t sta_config = {
			.sta = {
					.ssid = ESP_WIFI_STA_SSID,
					.password = ESP_WIFI_STA_PASSWD,
					.bssid_set = false,
			},
	};
	ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));

	// 启动Wi-Fi
	ESP_ERROR_CHECK(esp_wifi_start());

	// 配置省电模式
	ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
}

void app_main(void)
{
	ESP_LOGI(TAG, "启动...");

	// 初始化NVS
	esp_err_t ret = nvs_flash_init();
	if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
	{
		ESP_ERROR_CHECK(nvs_flash_erase());
		ret = nvs_flash_init();
	}
	ESP_ERROR_CHECK(ret);

	// 创建默认事件循环
	ESP_ERROR_CHECK(esp_event_loop_create_default());

	// 配置并启动Wi-Fi
	wifi_sta_init();

	// 生成设备ID
	uint8_t mac[6];
	esp_read_mac(mac, ESP_MAC_WIFI_STA);
	snprintf(DEVICE_ID, sizeof(DEVICE_ID), "%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
	ESP_LOGI(TAG, "设备ID: %s", DEVICE_ID);

	// 设置MQTT主题
	setup_mqtt_configs(&mqtt_configs, DEVICE_ID);

	// 打印初始化后的结构体属性
	ESP_LOGI(TAG, "温度: %.2f", systemData.temperature);
	ESP_LOGI(TAG, "湿度: %.2f", systemData.humidity);
	ESP_LOGI(TAG, "气压: %.2f", systemData.pressure);
	ESP_LOGI(TAG, "光照强度: %.2f", systemData.lightIntensity);
	ESP_LOGI(TAG, "烟雾浓度: %.2f", systemData.smokeDensity);
	ESP_LOGI(TAG, "继电器状态: %s", systemData.relayStatus);
	ESP_LOGI(TAG, "断路器状态: %c", systemData.circuitBreakerStatus);

	// 模拟传感器初始化
	ESP_LOGI(TAG, "MQ2 传感器初始化");
	ESP_LOGI(TAG, "BH1750 传感器初始化");
	ESP_LOGI(TAG, "BME280 传感器初始化");
	ESP_LOGI(TAG, "继电器初始化");
	ESP_LOGI(TAG, "断路器初始化");

	for (;;)
	{
		// 数据采集延迟
		vTaskDelay(pdMS_TO_TICKS(10000));

		// 生成随机传感器数据
		systemData.temperature = random_float(10.0f, 35.0f);		 // 温度范围 10°C ~ 35°C
		systemData.humidity = random_float(30.0f, 90.0f);				 // 湿度范围 30% ~ 90%
		systemData.pressure = random_float(950.0f, 1050.0f);		 // 气压范围 950 hPa ~ 1050 hPa
		systemData.lightIntensity = random_float(0.0f, 1000.0f); // 光照强度范围 0 ~ 1000 lux
		systemData.smokeDensity = random_float(0.0f, 100.0f);		 // 烟雾浓度范围 0 ~ 100 PPM

		// 更新断路器状态
		systemData.circuitBreakerStatus = circuit_breaker_state;

		// 打包并发送数据
		send_json(&systemData);
	}
}
