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

#include <libesphttpd/esp.h>
#include "libesphttpd/httpd.h"
#include "io.h"
#include "libesphttpd/httpdespfs.h"
#include "cgi.h"
#include "libesphttpd/cgiwifi.h"
#include "libesphttpd/cgiflash.h"
#include "libesphttpd/esp32_httpd_vfs.h"
#include "libesphttpd/auth.h"
#include "libesphttpd/espfs.h"
#include "libesphttpd/captdns.h"
#include "libesphttpd/webpages-espfs.h"
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
#include "esp_event_loop.h"
#include "nvs_flash.h"
#include "tcpip_adapter.h"
#endif

#define TAG "user_main"

/* The examples use simple WiFi configuration that you can set via
   'make menuconfig'.

   If you'd rather not, just change the below entries to strings with
   the config you want - ie #define EXAMPLE_WIFI_SSID "mywifissid"
*/
#define EXAMPLE_WIFI_SSID      CONFIG_EXAMPLE_WIFI_SSID
#define EXAMPLE_WIFI_PASS      CONFIG_EXAMPLE_WIFI_PASSWORD

#ifdef CONFIG_EXAMPLE_FS_TYPE_SPIFFS
// SPIFFS filesystem
#warning "SPIFFS Filesystem Chosen"
#include "esp_vfs.h"
#include "esp_spiffs.h"
#include "esp_partition.h"
#elif defined CONFIG_EXAMPLE_FS_TYPE_FAT
// FAT file-system
#warning "FAT Filesystem Chosen"
#include "esp_vfs.h"
#include "esp_vfs_fat.h"
#include "esp_partition.h"
// Handle of the wear levelling library instance
static wl_handle_t s_wl_handle = WL_INVALID_HANDLE;  // needed by FAT filesystem

#elif defined CONFIG_EXAMPLE_FS_TYPE_LFS
// LittleFS Filesystem
#warning "LittleFS Filesystem Chosen"
#else
#warning "No filesystem Chosen!"
#endif

// Mount path for the internal SPI-Flash file storage partition
#define FS_BASE_PATH	"/spiflash"
const char *base_path = FS_BASE_PATH;
#define FS_PART_NAME	"internalfs"

#define LISTEN_PORT     80u
#define MAX_CONNECTIONS 32u

static char connectionMemory[sizeof(RtosConnType) * MAX_CONNECTIONS];
static HttpdFreertosInstance httpdFreertosInstance;

//Function that tells the authentication system what users/passwords live on the system.
//This is disabled in the default build; if you want to try it, enable the authBasic line in
//the builtInUrls below.
int myPassFn(HttpdConnData *connData, int no, char *user, int userLen, char *pass, int passLen) {
	if (no==0) {
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
static void websocketBcast(void *arg) {
	static int ctr=0;
	char buff[128];
	while(1) {
		ctr++;
		sprintf(buff, "Up for %d minutes %d seconds!\n", ctr/60, ctr%60);
		cgiWebsockBroadcast(&httpdFreertosInstance.httpdInstance,
		                    "/websocket/ws.cgi", buff, strlen(buff),
		                    WEBSOCK_FLAG_NONE);

		vTaskDelay(1000/portTICK_RATE_MS);
	}
}

//On reception of a message, send "You sent: " plus whatever the other side sent
static void myWebsocketRecv(Websock *ws, char *data, int len, int flags) {
	int i;
	char buff[128];
	sprintf(buff, "You sent: ");
	for (i=0; i<len; i++) buff[i+10]=data[i];
	buff[i+10]=0;
	cgiWebsocketSend(&httpdFreertosInstance.httpdInstance,
	                 ws, buff, strlen(buff), WEBSOCK_FLAG_NONE);
}

//Websocket connected. Install reception handler and send welcome message.
static void myWebsocketConnect(Websock *ws) {
	ws->recvCb=myWebsocketRecv;
	cgiWebsocketSend(&httpdFreertosInstance.httpdInstance,
	                 ws, "Hi, Websocket!", 14, WEBSOCK_FLAG_NONE);
}

//On reception of a message, echo it back verbatim
void myEchoWebsocketRecv(Websock *ws, char *data, int len, int flags) {
	printf("EchoWs: echo, len=%d\n", len);
	cgiWebsocketSend(&httpdFreertosInstance.httpdInstance,
	                 ws, data, len, flags);
}

//Echo websocket connected. Install reception handler.
void myEchoWebsocketConnect(Websock *ws) {
	printf("EchoWs: connect\n");
	ws->recvCb=myEchoWebsocketRecv;
}

#define OTA_FLASH_SIZE_K 1024
#define OTA_TAGNAME "generic"

CgiUploadFlashDef uploadParams={
	.type=CGIFLASH_TYPE_FW,
	.fw1Pos=0x1000,
	.fw2Pos=((OTA_FLASH_SIZE_K*1024)/2)+0x1000,
	.fwSize=((OTA_FLASH_SIZE_K*1024)/2)-0x1000,
	.tagName=OTA_TAGNAME
};


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
HttpdBuiltInUrl builtInUrls[]={
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

	ROUTE_REDIRECT("/websocket", "/websocket/index.html"),
	ROUTE_WS("/websocket/ws.cgi", myWebsocketConnect),
	ROUTE_WS("/websocket/echo.cgi", myEchoWebsocketConnect),

	ROUTE_REDIRECT("/httptest", "/httptest/index.html"),
	ROUTE_REDIRECT("/httptest/", "/httptest/index.html"),
	ROUTE_CGI("/httptest/test.cgi", cgiTestbed),

	ROUTE_REDIRECT("/filesystem", "/filesystem/index.html"),
	ROUTE_REDIRECT("/filesystem/", "/filesystem/index.html"),

#ifndef CONFIG_EXAMPLE_FS_TYPE_NONE // If FS not set to 'none' in menuconfig
	// Filesystem POST/PUT handler. Allows only replacing content of one file at "/spiflash/html/writeable_file.txt".
	ROUTE_CGI_ARG("/writeable_file.txt", cgiEspVfsUpload, FS_BASE_PATH "/html/writeable_file.txt"),

	// Filesystem POST/PUT handler.  Allows creating/replacing files anywhere under "/spiflash/html/upload/".  (note trailing slash)
	ROUTE_CGI_ARG("/filesystem/upload.cgi", cgiEspVfsUpload, FS_BASE_PATH "/html/upload/"),

	// Filesystem POST/PUT handler, will fall through if http req is not PUT or POST.
	ROUTE_CGI_ARG("*", cgiEspVfsUpload, FS_BASE_PATH "/html/"),

	// Filesystem GET handler, will fall through if file not found.
	ROUTE_CGI_ARG("*", cgiEspVfsGet, FS_BASE_PATH "/html/"),
#endif
	// espFs filesystem GET handler.
	ROUTE_FILESYSTEM(),

	ROUTE_END()
};


#ifdef ESP32

static EventGroupHandle_t wifi_ap_event_group;
static EventGroupHandle_t wifi_sta_event_group;

/* The event group allows multiple bits for each event,
   but we only care about one event - are we connected
   to the AP with an IP? */
const static int CONNECTED_BIT = BIT0;

static esp_err_t wifi_event_handler(void *ctx, system_event_t *event)
{
    switch(event->event_id) {
    case SYSTEM_EVENT_STA_START:
        /* Calling this unconditionally would interfere with the WiFi CGI. */
        // esp_wifi_connect();
        break;
    case SYSTEM_EVENT_STA_GOT_IP:
        xEventGroupSetBits(wifi_sta_event_group, CONNECTED_BIT);
        break;
    case SYSTEM_EVENT_STA_CONNECTED:
        break;
    case SYSTEM_EVENT_STA_DISCONNECTED:
        xEventGroupClearBits(wifi_sta_event_group, CONNECTED_BIT);
        /* This is a workaround as ESP32 WiFi libs don't currently
           auto-reassociate. */
        /* Skip reconnect if disconnect was deliberate or authentication      *\
        \* failed.                                                            */
        switch(event->event_info.disconnected.reason){
        case WIFI_REASON_ASSOC_LEAVE:
        case WIFI_REASON_AUTH_FAIL:
            break;
        default:
        esp_wifi_connect();
        break;
        }
        break;
    case SYSTEM_EVENT_AP_START:
    {
        tcpip_adapter_ip_info_t ap_ip_info;
        if (tcpip_adapter_get_ip_info(TCPIP_ADAPTER_IF_AP, &ap_ip_info) == 0) {
            ESP_LOGI(TAG, "~~~~~~~~~~~");
            ESP_LOGI(TAG, "IP:" IPSTR, IP2STR(&ap_ip_info.ip));
            ESP_LOGI(TAG, "MASK:" IPSTR, IP2STR(&ap_ip_info.netmask));
            ESP_LOGI(TAG, "GW:" IPSTR, IP2STR(&ap_ip_info.gw));
            ESP_LOGI(TAG, "~~~~~~~~~~~");
        }
    }
        break;
    case SYSTEM_EVENT_AP_STACONNECTED:
        ESP_LOGI(TAG, "station:" MACSTR" join,AID=%d\n",
                MAC2STR(event->event_info.sta_connected.mac),
                event->event_info.sta_connected.aid);
        xEventGroupSetBits(wifi_ap_event_group, CONNECTED_BIT);
        break;
    case SYSTEM_EVENT_AP_STADISCONNECTED:
        ESP_LOGI(TAG, "station:" MACSTR"leave,AID=%d\n",
                MAC2STR(event->event_info.sta_disconnected.mac),
                event->event_info.sta_disconnected.aid);
        xEventGroupClearBits(wifi_ap_event_group, CONNECTED_BIT);
        break;
    case SYSTEM_EVENT_SCAN_DONE:
        break;
    default:
        break;
    }

    /* Forward event to to the WiFi CGI module */
    cgiWifiEventCb(event);

    return ESP_OK;
}


//Simple task to connect to an access point
void ICACHE_FLASH_ATTR init_wifi(bool modeAP) {
	esp_err_t result;

	result = nvs_flash_init();
	if(   result == ESP_ERR_NVS_NO_FREE_PAGES
	   || result == ESP_ERR_NVS_NEW_VERSION_FOUND)
	{
		ESP_LOGI(TAG, "Erasing NVS");
		nvs_flash_erase();
		result = nvs_flash_init();
	}
	ESP_ERROR_CHECK(result);

	wifi_sta_event_group = xEventGroupCreate();
	wifi_ap_event_group = xEventGroupCreate();

	// Initialise wifi configuration CGI
	result = initCgiWifi();
	ESP_ERROR_CHECK(result);

	ESP_ERROR_CHECK( esp_event_loop_init(wifi_event_handler, NULL) );

	wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
	ESP_ERROR_CHECK( esp_wifi_init(&cfg) );
	ESP_ERROR_CHECK( esp_wifi_set_storage(WIFI_STORAGE_RAM) );

	//Go to station mode
	esp_wifi_disconnect();

	if(modeAP) {
		ESP_ERROR_CHECK( esp_wifi_set_mode(WIFI_MODE_AP) );

	wifi_config_t ap_config;
	strcpy((char*)(&ap_config.ap.ssid), "ESP");
	ap_config.ap.ssid_len = 3;
	ap_config.ap.channel = 1;
	ap_config.ap.authmode = WIFI_AUTH_OPEN;
	ap_config.ap.ssid_hidden = 0;
		ap_config.ap.max_connection = 3;
	ap_config.ap.beacon_interval = 100;

	esp_wifi_set_config(WIFI_IF_AP, &ap_config);
	}
	else {
		esp_wifi_set_mode(WIFI_MODE_STA);

	//Connect to the defined access point.
		wifi_config_t config;
		memset(&config, 0, sizeof(config));
		sprintf((char*)config.sta.ssid, "RouterSSID");			// @TODO: Changeme
		sprintf((char*)config.sta.password, "RouterPassword"); 	// @TODO: Changeme
		esp_wifi_set_config(WIFI_IF_STA, &config);
		esp_wifi_connect();
	}

	ESP_ERROR_CHECK( esp_wifi_start() );
}
#endif

static void mount_fs(void)
{
	esp_err_t err = ESP_FAIL;
	int f_bsize=0, f_blocks=0, f_bfree=0;
    esp_partition_t * fs_partition = NULL;

#ifndef CONFIG_EXAMPLE_FS_TYPE_NONE
    ESP_LOGI(TAG, "Mounting Filesystem");
    // To mount device we need name of device partition, define base_path
    // and allow format partition in case if it is new one and was not formated before
#endif

#ifdef CONFIG_EXAMPLE_FS_TYPE_SPIFFS
    // SPIFFS filesystem
    fs_partition = (esp_partition_t *)esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, FS_PART_NAME);
	if (fs_partition == NULL) {
		ESP_LOGE(TAG, "Filesystem partition not found!");
	}
    esp_vfs_spiffs_conf_t conf = {
	    .base_path = FS_BASE_PATH,
	    .partition_label = FS_PART_NAME,
	    .max_files = 5,
	    .format_if_mount_failed = true
    };

    // Use settings defined above to initialize and mount SPIFFS filesystem.
    // Note: esp_vfs_spiffs_register is an all-in-one convenience function.
    err = esp_vfs_spiffs_register(&conf);

	if (err != ESP_OK) {
		if (err == ESP_FAIL) {
			ESP_LOGE(TAG, "Failed to mount or format filesystem");
		} else if (err == ESP_ERR_NOT_FOUND) {
			ESP_LOGE(TAG, "Failed to find SPIFFS partition");
		} else {
			ESP_LOGE(TAG, "Failed to initialize SPIFFS (%s)", esp_err_to_name(err));
		}
		return;
    }

	size_t total = 0, used = 0;
	err = esp_spiffs_info(FS_PART_NAME, &total, &used);
	if (err != ESP_OK) {
    	ESP_LOGE(TAG, "Failed to get SPIFFS partition information (%s)", esp_err_to_name(err));
	} else {
		f_bsize = 256;
		f_blocks = total / 256;
		f_bfree = (total-used) / 256;
	}
	ESP_LOGI(TAG, "Mounted filesystem. Type: SPIFFS");
#elif defined CONFIG_EXAMPLE_FS_TYPE_FAT
    // FAT file-system
    fs_partition = (esp_partition_t *)esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, FS_PART_NAME);
	if (fs_partition == NULL) {
		ESP_LOGE(TAG, "Filesystem partition not found!");
	}
    const esp_vfs_fat_mount_config_t mount_config = {
	    .max_files = 16,
	    .format_if_mount_failed = true,
	    .allocation_unit_size = 0 // will default to wear-leveling size.  Set CONFIG_WL_SECTOR_SIZE = 4096 in menuconfig for performance boost.
    };
    err = esp_vfs_fat_spiflash_mount(FS_BASE_PATH, FS_PART_NAME, &mount_config, &s_wl_handle);
    if (err != ESP_OK) {
    	ESP_LOGE(TAG, "Failed to mount FATFS (%s)", esp_err_to_name(err));
    	return;
    }
	FATFS *fatfs;
	DWORD fre_clust;
	FRESULT res = f_getfree(FS_BASE_PATH, &fre_clust, &fatfs);
	if (res != FR_OK) {
	    ESP_LOGE(TAG, "Failed to get FAT partition information (error: %i)", res);
	} else {
#if FF_MAX_SS == FF_MIN_SS
#define SECSIZE(fs) (FF_MIN_SS)
#else
#define SECSIZE(fs) ((fs)->ssize)
#endif
		f_bsize = fatfs->csize * SECSIZE(fatfs);
		f_blocks = fatfs->n_fatent - 2;
		f_bfree = fre_clust;
	}
	ESP_LOGI(TAG, "Mounted filesystem. Type: FAT");
#elif defined CONFIG_EXAMPLE_FS_TYPE_LFS
    // LittleFS Filesystem
    fs_partition = (esp_partition_t *)esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, FS_PART_NAME);
    if (fs_partition == NULL) {
    	ESP_LOGE("Filesystem partition not found!");
		return;
    }
    const little_flash_config_t little_cfg = {
        .part = fs_partition,
        .base_path = FS_BASE_PATH,
        .open_files = 16,
        .auto_format = true,
        .lookahead = 32
    };
    err = littleFlash_init(&little_cfg);
    if (err != ESP_OK) {
		ESP_LOGE(TAG, "Failed to mount Flash partition as LittleFS.");
		return;
   	}
	uint32_t used = littleFlash_getUsedBlocks();
	f_bsize = littleFlash.lfs_cfg.block_size;
	f_blocks = littleFlash.lfs_cfg.block_count;
	f_bfree = f_blocks - used;
	ESP_LOGI(TAG, "Mounted filesystem. Type: LittleFS");
#endif
	if (fs_partition != NULL)
	{
		ESP_LOGI(TAG, "Mounted on partition '%s' [size: %d; offset: 0x%6X; %s]", fs_partition->label, fs_partition->size, fs_partition->address, (fs_partition->encrypted)?"ENCRYPTED":"");
		if (err == ESP_OK) {
			ESP_LOGI(TAG, "----------------");
			ESP_LOGI(TAG, "Filesystem size: %d B", f_blocks * f_bsize);
			ESP_LOGI(TAG, "           Used: %d B", (f_blocks * f_bsize) - (f_bfree * f_bsize));
			ESP_LOGI(TAG, "           Free: %d B", f_bfree * f_bsize);
			ESP_LOGI(TAG, "----------------");
		}
	}
}

//Main routine. Initialize stdout, the I/O, filesystem and the webserver and we're done.
#if ESP32
void app_main(void) {
#else
void user_init(void) {
#endif

#ifndef ESP32
	uart_div_modify(0, UART_CLK_FREQ / 115200);
#endif

	ioInit();
// FIXME: Re-enable this when capdns is fixed for esp32
//	captdnsInit();

	espFsInit((void*)(webpages_espfs_start));

	mount_fs();

	tcpip_adapter_init();
	httpdFreertosInit(&httpdFreertosInstance,
	                  builtInUrls,
	                  LISTEN_PORT,
	                  connectionMemory,
	                  MAX_CONNECTIONS,
	                  HTTPD_FLAG_NONE);
	httpdFreertosStart(&httpdFreertosInstance);

	init_wifi(true); // Supply false for STA mode

	xTaskCreate(websocketBcast, "wsbcast", 3000, NULL, 3, NULL);

	printf("\nReady\n");
}
