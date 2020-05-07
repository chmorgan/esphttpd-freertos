/*
 * ----------------------------------------------------------------------------
 * "THE BEER-WARE LICENSE" (Revision 42):
 * Jeroen Domburg <jeroen@spritesmods.com> wrote this file. As long as you retain
 * this notice you can do whatever you want with this stuff. If we meet some day,
 * and you think this stuff is worth it, you can buy me a beer in return.
 * ----------------------------------------------------------------------------
 */

/*
This is example code for the esphttpd library. It's a small-ish demo showing off
the server, including WiFi connection management capabilities, some IO etc.
*/
#include "sdkconfig.h"

#include <libesphttpd/esp.h>
#include "libesphttpd/httpd.h"
#include "io.h"

#ifdef CONFIG_ESPHTTPD_USE_ESPFS
#include "espfs.h"
#include "espfs_image.h"
#include "libesphttpd/httpd-espfs.h"
#endif // CONFIG_ESPHTTPD_USE_ESPFS

#include "cgi.h"
#include "libesphttpd/cgiwifi.h"
#include "libesphttpd/cgiflash.h"
#include "libesphttpd/auth.h"
#include "libesphttpd/captdns.h"
#include "libesphttpd/cgiwebsocket.h"
#include "libesphttpd/httpd-freertos.h"
#include "libesphttpd/route.h"
#include "cgi-test.h"

#include "esp_wifi.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

#ifdef ESP32
#include "freertos/event_groups.h"
#include "esp_log.h"

#define CONFIG_STORE_WIFI_TO_NVS
#ifdef CONFIG_STORE_WIFI_TO_NVS
#include "nvs_flash.h"
#define NVS_NAMESPACE "nvs"
nvs_handle my_nvs_handle;
#define NET_CONF_KEY "netconf"
#endif

#include "esp_err.h"
#include "esp_event_loop.h"
#include "esp_event.h"
#include "tcpip_adapter.h"

#if defined CONFIG_EXAMPLE_USE_ETHERNET && CONFIG_EXAMPLE_USE_ETHERNET
#define ETHERNET_ENABLE 1
#include "ethernet_init.h"
#endif

char my_hostname[16] = "esphttpd";

/* The examples use simple WiFi configuration that you can set via
   'make menuconfig'.

   If you'd rather not, just change the below entries to strings with
   the config you want - ie #define DEFAULT_WIFI_STA_SSID "mywifissid"
*/
#define DEFAULT_WIFI_MODE WIFI_MODE_AP
#define DEFAULT_WIFI_STA_SSID CONFIG_EXAMPLE_WIFI_SSID
#define DEFAULT_WIFI_STA_PASS CONFIG_EXAMPLE_WIFI_PASSWORD
#endif

#define TAG "user_main"

#define LISTEN_PORT 80u
#define MAX_CONNECTIONS 32u

static char connectionMemory[sizeof(RtosConnType) * MAX_CONNECTIONS];
static HttpdFreertosInstance httpdFreertosInstance;

//Function that tells the authentication system what users/passwords live on the system.
//This is disabled in the default build; if you want to try it, enable the authBasic line in
//the builtInUrls below.
int myPassFn(HttpdConnData *connData, int no, char *user, int userLen, char *pass, int passLen)
{
	if (no == 0)
	{
		strcpy(user, "admin");
		strcpy(pass, "s3cr3t");
		return 1;
		//Add more users this way. Check against incrementing no for each user added.
		//	} else if (no==1) {
		//		strcpy(user, "user1");
		//		strcpy(pass, "something");
		//		return 1;
	}
	return 0;
}

//Broadcast the uptime in seconds every second over connected websockets
static void websocketBcast(void *arg)
{
	static int ctr = 0;
	char buff[128];
	while (1)
	{
		ctr++;
		sprintf(buff, "Up for %d minutes %d seconds!\n", ctr / 60, ctr % 60);
		cgiWebsockBroadcast(&httpdFreertosInstance.httpdInstance,
							"/websocket/ws.cgi", buff, strlen(buff),
							WEBSOCK_FLAG_NONE);

		vTaskDelay(1000 / portTICK_RATE_MS);
	}
}

//On reception of a message, send "You sent: " plus whatever the other side sent
static void myWebsocketRecv(Websock *ws, char *data, int len, int flags)
{
	int i;
	char buff[128];
	sprintf(buff, "You sent: ");
	for (i = 0; i < len; i++)
		buff[i + 10] = data[i];
	buff[i + 10] = 0;
	cgiWebsocketSend(&httpdFreertosInstance.httpdInstance,
					 ws, buff, strlen(buff), WEBSOCK_FLAG_NONE);
}

//Websocket connected. Install reception handler and send welcome message.
static void myWebsocketConnect(Websock *ws)
{
	ws->recvCb = myWebsocketRecv;
	cgiWebsocketSend(&httpdFreertosInstance.httpdInstance,
					 ws, "Hi, Websocket!", 14, WEBSOCK_FLAG_NONE);
}

//On reception of a message, echo it back verbatim
void myEchoWebsocketRecv(Websock *ws, char *data, int len, int flags)
{
	printf("EchoWs: echo, len=%d\n", len);
	cgiWebsocketSend(&httpdFreertosInstance.httpdInstance,
					 ws, data, len, flags);
}

//Echo websocket connected. Install reception handler.
void myEchoWebsocketConnect(Websock *ws)
{
	printf("EchoWs: connect\n");
	ws->recvCb = myEchoWebsocketRecv;
}

#define OTA_FLASH_SIZE_K 1024
#define OTA_TAGNAME "generic"

CgiUploadFlashDef uploadParams = {
	.type = CGIFLASH_TYPE_FW,
	.fw1Pos = 0x1000,
	.fw2Pos = ((OTA_FLASH_SIZE_K * 1024) / 2) + 0x1000,
	.fwSize = ((OTA_FLASH_SIZE_K * 1024) / 2) - 0x1000,
	.tagName = OTA_TAGNAME};

/*
This is the main url->function dispatching data struct.
In short, it's a struct with various URLs plus their handlers. The handlers can
be 'standard' CGI functions you wrote, or 'special' CGIs requiring an argument.
They can also be auth-functions. An asterisk will match any url starting with
everything before the asterisks; "*" matches everything. The list will be
handled top-down, so make sure to put more specific rules above the more
general ones. Authorization things (like authBasic) act as a 'barrier' and
should be placed above the URLs they protect.
*/
HttpdBuiltInUrl builtInUrls[] = {
	ROUTE_CGI_ARG("*", cgiRedirectApClientToHostname, "esp8266.nonet"),
	ROUTE_REDIRECT("/", "/index.tpl"),

	ROUTE_TPL("/led.tpl", tplLed),
	ROUTE_TPL("/index.tpl", tplCounter),
	ROUTE_CGI("/led.cgi", cgiLed),

	ROUTE_REDIRECT("/flash", "/flash/index.html"),
	ROUTE_REDIRECT("/flash/", "/flash/index.html"),
	ROUTE_CGI("/flash/flashinfo.json", cgiGetFlashInfo),
	ROUTE_CGI("/flash/setboot", cgiSetBoot),
	ROUTE_CGI_ARG("/flash/upload", cgiUploadFirmware, &uploadParams),
	ROUTE_CGI_ARG("/flash/erase", cgiEraseFlash, &uploadParams),
	ROUTE_CGI("/flash/reboot", cgiRebootFirmware),

	//Routines to make the /wifi URL and everything beneath it work.
	//Enable the line below to protect the WiFi configuration with an username/password combo.
	//	{"/wifi/*", authBasic, myPassFn},

	ROUTE_REDIRECT("/wifi", "/wifi/wifi.tpl"),
	ROUTE_REDIRECT("/wifi/", "/wifi/wifi.tpl"),
	ROUTE_CGI("/wifi/wifiscan.cgi", cgiWiFiScan),
	ROUTE_TPL("/wifi/wifi.tpl", tplWlan),
	ROUTE_CGI("/wifi/connect.cgi", cgiWiFiConnect),
	ROUTE_CGI("/wifi/connstatus.cgi", cgiWiFiConnStatus),
	ROUTE_CGI("/wifi/setmode.cgi", cgiWiFiSetMode),
	ROUTE_CGI("/wifi/startwps.cgi", cgiWiFiStartWps),
	ROUTE_CGI("/wifi/ap", cgiWiFiAPSettings),

	ROUTE_REDIRECT("/websocket", "/websocket/index.html"),
	ROUTE_WS("/websocket/ws.cgi", myWebsocketConnect),
	ROUTE_WS("/websocket/echo.cgi", myEchoWebsocketConnect),

	ROUTE_REDIRECT("/httptest", "/httptest/index.html"),
	ROUTE_REDIRECT("/httptest/", "/httptest/index.html"),
	ROUTE_CGI("/httptest/test.cgi", cgiTestbed),

	ROUTE_FILESYSTEM(),

	ROUTE_END()};

#ifdef ESP32

static void update_status_ind_wifi()
{
	esp_err_t result;
	wifi_mode_t api_wifi_mode;

	result = esp_wifi_get_mode(&api_wifi_mode);
	if (result != ESP_OK)
	{
		ESP_LOGE(TAG, "[%s] Error fetching WiFi mode.", __FUNCTION__);
		return;
	}

	if (api_wifi_mode == WIFI_MODE_NULL)
	{
		set_status_ind_wifi(WIFI_STATE_OFF);
		return;
	}
	if (api_wifi_mode == WIFI_MODE_STA || api_wifi_mode == WIFI_MODE_APSTA)
	{
		wifi_ap_record_t ap_info;
		if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK)
		{
			set_status_ind_wifi(WIFI_STATE_CONN);
			return;
		}
	}
	if (api_wifi_mode == WIFI_MODE_AP || api_wifi_mode == WIFI_MODE_APSTA)
	{
		wifi_sta_list_t sta_list;
		if (esp_wifi_ap_get_sta_list(&sta_list) == ESP_OK)
		{
			if (sta_list.num > 0) // at least one client connected?
			{
				set_status_ind_wifi(WIFI_STATE_CONN);
				return;
			}
		}
	}
	set_status_ind_wifi(WIFI_STATE_IDLE);
}

static esp_err_t app_event_handler(void *ctx, system_event_t *event)
{
	switch (event->event_id)
	{
	case SYSTEM_EVENT_ETH_START:
		tcpip_adapter_set_hostname(TCPIP_ADAPTER_IF_ETH, my_hostname);
		break;
	case SYSTEM_EVENT_STA_START:
		tcpip_adapter_set_hostname(TCPIP_ADAPTER_IF_STA, my_hostname);
		// esp_wifi_connect(); /* Calling this unconditionally would interfere with the WiFi CGI. */
		break;
	case SYSTEM_EVENT_STA_GOT_IP:
	{
		tcpip_adapter_ip_info_t sta_ip_info;
		wifi_config_t sta_conf;
		printf("~~~~~STA~~~~~"
			   "\n");
		if (esp_wifi_get_config(TCPIP_ADAPTER_IF_STA, &sta_conf) == ESP_OK)
		{
			printf("ssid: %s"
				   "\n",
				   sta_conf.sta.ssid);
		}

		if (tcpip_adapter_get_ip_info(TCPIP_ADAPTER_IF_STA, &sta_ip_info) == ESP_OK)
		{
			printf("IP:" IPSTR "\n", IP2STR(&sta_ip_info.ip));
			printf("MASK:" IPSTR "\n", IP2STR(&sta_ip_info.netmask));
			printf("GW:" IPSTR "\n", IP2STR(&sta_ip_info.gw));
		}
		printf("~~~~~~~~~~~~~"
			   "\n");
	}
	break;
	case SYSTEM_EVENT_STA_CONNECTED:
		break;
	case SYSTEM_EVENT_STA_DISCONNECTED:
		/* This is a workaround as ESP32 WiFi libs don't currently auto-reassociate. */
		/* Skip reconnect if disconnect was deliberate or authentication failed.     */
		/* this is now handled by cgiwifi watchdog
		switch (event->event_info.disconnected.reason)
		{
		case WIFI_REASON_ASSOC_LEAVE:
		case WIFI_REASON_AUTH_FAIL:
			break;
		default:
			esp_wifi_connect();
			break;
		}
		*/
		break;
	case SYSTEM_EVENT_AP_START:
	{
		tcpip_adapter_set_hostname(TCPIP_ADAPTER_IF_AP, my_hostname);
		tcpip_adapter_ip_info_t ap_ip_info;
		wifi_config_t ap_conf;
		printf("~~~~~AP~~~~~"
			   "\n");
		if (esp_wifi_get_config(TCPIP_ADAPTER_IF_AP, &ap_conf) == ESP_OK)
		{
			printf("ssid: %s"
				   "\n",
				   ap_conf.ap.ssid);
			if (ap_conf.ap.authmode != WIFI_AUTH_OPEN)
				printf("pass: %s"
					   "\n",
					   ap_conf.ap.password);
		}

		if (tcpip_adapter_get_ip_info(TCPIP_ADAPTER_IF_AP, &ap_ip_info) == ESP_OK)
		{
			printf("IP:" IPSTR "\n", IP2STR(&ap_ip_info.ip));
			printf("MASK:" IPSTR "\n", IP2STR(&ap_ip_info.netmask));
			printf("GW:" IPSTR "\n", IP2STR(&ap_ip_info.gw));
		}
		printf("~~~~~~~~~~~~"
			   "\n");
	}
	break;
	case SYSTEM_EVENT_AP_STACONNECTED:
		ESP_LOGI(TAG, "station:" MACSTR " join,AID=%d",
				 MAC2STR(event->event_info.sta_connected.mac),
				 event->event_info.sta_connected.aid);

		break;
	case SYSTEM_EVENT_AP_STADISCONNECTED:
		ESP_LOGI(TAG, "station:" MACSTR "leave,AID=%d",
				 MAC2STR(event->event_info.sta_disconnected.mac),
				 event->event_info.sta_disconnected.aid);

		break;
	case SYSTEM_EVENT_SCAN_DONE:

		break;
	default:
		break;
	}
#ifdef ETHERNET_ENABLE
	ethernet_handle_system_event(ctx, event);
#endif

	/* Forward event to to the WiFi CGI module */
	cgiWifiEventCb(event);
	update_status_ind_wifi();

	return ESP_OK;
}

void init_wifi(bool factory_defaults)
{
	wifi_mode_t old_mode;
	wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
	ESP_ERROR_CHECK(esp_wifi_init(&cfg));

	// Try to get WiFi configuration from NVS?
#if defined(CONFIG_STORE_WIFI_TO_NVS)
	ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_FLASH)); // WIFI_STORAGE_FLASH or WIFI_STORAGE_RAM
#else														   // don't save WiFi config to NVS
	ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
#endif
	ESP_ERROR_CHECK(esp_wifi_get_mode(&old_mode));

	if (factory_defaults)
	{
		old_mode = DEFAULT_WIFI_MODE;
		esp_wifi_set_mode(old_mode);
	}

	if (old_mode == WIFI_MODE_APSTA || old_mode == WIFI_MODE_STA)
	{
		ESP_ERROR_CHECK(esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT20)); // set 20MHz bandwidth, for better range
		//// STA settings
		wifi_config_t factory_sta_config = {
			.sta = {
				.ssid = DEFAULT_WIFI_STA_SSID,
				.password = DEFAULT_WIFI_STA_PASS,
				.sort_method = WIFI_CONNECT_AP_BY_SIGNAL}};
		wifi_config_t sta_stored_config;

		ESP_ERROR_CHECK(esp_wifi_get_config(ESP_IF_WIFI_STA, &sta_stored_config));
		sta_stored_config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;

		if (factory_defaults && strlen((char *)factory_sta_config.sta.ssid) != 0)
		{
			// load factory default STA config
			ESP_LOGI(TAG, "Using factory-default WiFi STA configuration, ssid: %s", factory_sta_config.sta.ssid);
			ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &factory_sta_config));
		}
		else if (strlen((char *)sta_stored_config.sta.ssid) != 0)
		{
			ESP_LOGI(TAG, "Using WiFi STA configuration from NVS, ssid: %s", sta_stored_config.sta.ssid);
			ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &sta_stored_config));
		}
		else
		{
			ESP_LOGW(TAG, "No WiFi STA configuration available");
			if (old_mode == WIFI_MODE_APSTA)
				esp_wifi_set_mode(WIFI_MODE_AP); // remove STA mode
			if (old_mode == WIFI_MODE_STA)
				esp_wifi_set_mode(WIFI_MODE_NULL); // remove STA mode
		}
	}

	if (old_mode == WIFI_MODE_APSTA || old_mode == WIFI_MODE_AP)
	{
		ESP_ERROR_CHECK(esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW_HT20)); // set 20MHz bandwidth, for better range
		//// AP settings
		wifi_config_t factory_ap_config;
		{
			strncpy((char *)(&factory_ap_config.ap.ssid), "ESP", (sizeof((wifi_ap_config_t *)0)->ssid));
			factory_ap_config.ap.ssid_len = 0; // 0: use null termination to determine size
			factory_ap_config.ap.channel = 6;
			factory_ap_config.ap.authmode = WIFI_AUTH_OPEN; //WIFI_AUTH_WPA_WPA2_PSK; //WIFI_AUTH_OPEN;
			//strncpy((char *)(&factory_ap_config.ap.password), DEFAULT_WIFI_AP_PASS, (sizeof((wifi_ap_config_t *)0)->password));
			factory_ap_config.ap.ssid_hidden = 0;
			factory_ap_config.ap.max_connection = 4;
			factory_ap_config.ap.beacon_interval = 100;
		}
		wifi_config_t ap_stored_config;
		ESP_ERROR_CHECK(esp_wifi_get_config(ESP_IF_WIFI_AP, &ap_stored_config));

		if (factory_defaults && strlen((char *)factory_ap_config.ap.ssid) != 0)
		{
			// load factory default STA config
			ESP_LOGI(TAG, "Using factory-default WiFi AP configuration, ssid: %s", factory_ap_config.ap.ssid);
			ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_AP, &factory_ap_config));
		}
		else if (strlen((char *)ap_stored_config.ap.ssid) != 0)
		{
			ESP_LOGI(TAG, "Using WiFi AP configuration from NVS, ssid: %s", ap_stored_config.ap.ssid);
			ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_AP, &ap_stored_config));
		}
		else
		{
			ESP_LOGW(TAG, "No WiFi AP configuration available");
			if (old_mode == WIFI_MODE_APSTA)
				esp_wifi_set_mode(WIFI_MODE_STA); // remove AP mode
			if (old_mode == WIFI_MODE_AP)
				esp_wifi_set_mode(WIFI_MODE_NULL); // remove AP mode
		}
	}

	ESP_ERROR_CHECK(esp_wifi_start());
}
#endif

//Main routine. Initialize stdout, the I/O, filesystem and the webserver and we're done.
#if ESP32
void app_main(void)
{
#else
void user_init(void)
{
#endif

#ifndef ESP32
	uart_div_modify(0, UART_CLK_FREQ / 115200);
#endif

	ioInit();
	// FIXME: Re-enable this when capdns is fixed for esp32
	//	captdnsInit();
	esp_err_t err;
	// Init NVS
	err = nvs_flash_init();
	if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
	{
		// NVS partition was truncated and needs to be erased
		// Retry nvs_flash_init
		ESP_LOGW(TAG, "NVS invalid, reformatting... ");
		ESP_ERROR_CHECK(nvs_flash_erase());
		err = nvs_flash_init();
	}
	ESP_ERROR_CHECK(err); // will reboot if error!

	uint32_t net_configured = 0; // value will default to 0, if not set yet in NVS
	ESP_LOGI(TAG, "Opening NVS handle ");
	err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &my_nvs_handle);
	if (err != ESP_OK)
	{
		ESP_LOGE(TAG, "Error (%s) opening NVS handle!\n", esp_err_to_name(err));
	}
	else
	{

		// Read NVS
		ESP_LOGI(TAG, "Reading network initialization from NVS");
		err = nvs_get_u32(my_nvs_handle, NET_CONF_KEY, &net_configured);
		switch (err)
		{
		case ESP_OK:
			ESP_LOGI(TAG, "nvs init = %d", net_configured);
			break;
		case ESP_ERR_NVS_NOT_FOUND:
			ESP_LOGI(TAG, "nvs init not found, initializing now.");
			break;
		default:
			ESP_LOGE(TAG, "Error (%s) reading!\n", esp_err_to_name(err));
		}
	}

#ifdef CONFIG_ESPHTTPD_USE_ESPFS
	EspFsConfig espfs_conf = {
		.memAddr = espfs_image_bin,
	};
	EspFs *fs = espFsInit(&espfs_conf);
	httpdRegisterEspfs(fs);
#endif // CONFIG_ESPHTTPD_USE_ESPFS

	tcpip_adapter_init();
	httpdFreertosInit(&httpdFreertosInstance,
					  builtInUrls,
					  LISTEN_PORT,
					  connectionMemory,
					  MAX_CONNECTIONS,
					  HTTPD_FLAG_NONE);
	httpdFreertosStart(&httpdFreertosInstance);

	xTaskCreate(websocketBcast, "wsbcast", 3000, NULL, 3, NULL);

	initCgiWifi(); // Initialise wifi configuration CGI

	ESP_ERROR_CHECK(esp_event_loop_init(app_event_handler, NULL));

	init_wifi(!net_configured); // Start Wifi, restore factory wifi settings if not initialized
	startCgiWifi();				// will try to restore STA connection on boot
#ifdef MY_ETH_ENABLE
	init_ethernet();
#endif

	if (!net_configured)
	{ // If wasn't initialized, now we are initialized.  Write it to NVS.
		net_configured = 1;
		ESP_LOGI(TAG, "Writing init to NVS");
		ESP_ERROR_CHECK(nvs_set_u32(my_nvs_handle, NET_CONF_KEY, net_configured));
		// After setting any values, nvs_commit() must be called to ensure changes are written to flash storage.
		ESP_LOGI(TAG, "Committing updates in NVS");
		ESP_ERROR_CHECK(nvs_commit(my_nvs_handle));
		// Close NVS
		//nvs_close(my_nvs_handle); - don't close if handle is shared by cgi_NVS
	}

	printf("\nReady\n");
}
