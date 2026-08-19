/**
 * @file    wifi.h
 * @brief   MXCHIP EMW3080 Wi-Fi Connection Manager
 * @author  Alexandru-Ionut Cioc
 * @date    2026
 *
 * Manages Wi-Fi connectivity via the onboard EMW3080 module
 * on the B-U585I-IOT02A Discovery Kit (SPI interface).
 */

#ifndef __WIFI_H
#define __WIFI_H

#include <stdint.h>
#include <stdbool.h>

#include "mx_wifi.h"

/* Wi-Fi status */
typedef enum {
    WIFI_OK = 0,
    WIFI_ERROR_INIT,
    WIFI_ERROR_CONNECT,
    WIFI_ERROR_TIMEOUT,
    WIFI_ERROR_SEND,
} WiFiStatus_t;

/**
 * @brief  Initialize the EMW3080 Wi-Fi module.
 * @retval WIFI_OK on success.
 */
WiFiStatus_t WiFi_Init(void);

/**
 * @brief  Connect to the configured Wi-Fi network.
 * @param  ssid: Network SSID.
 * @param  password: Network password.
 * @retval WIFI_OK on success.
 */
WiFiStatus_t WiFi_Connect(const char *ssid, const char *password);

typedef void (*WiFiTest_Callback_t)(const char *msg, int percent);

/**
 * @brief  Test WiFi credentials with a single fast attempt and short DHCP timeout.
 * @param  ssid: Network SSID.
 * @param  password: Network password.
 * @param  cb: Optional callback to report connection progress.
 * @retval WIFI_OK if connected and acquired IP.
 */
WiFiStatus_t WiFi_TestConnection(const char *ssid, const char *password, WiFiTest_Callback_t cb);

/**
 * @brief  Check if Wi-Fi is currently connected.
 * @retval true if connected.
 */
bool WiFi_IsConnected(void);

/**
 * @brief  Perform an HTTP POST with binary image data.
 * @param  url: Full URL (e.g., "http://192.168.1.100:8000/api/upload")
 * @param  task_id: Task ID to include in the request.
 * @param  data: Image data buffer.
 * @param  data_len: Length of image data in bytes.
 * @retval WIFI_OK on success.
 */
WiFiStatus_t WiFi_HttpPostImage(const char *url, uint32_t task_id,
                                 const uint8_t *data, uint32_t data_len);

/**
 * @brief  Fetch the current time from the server via HTTP GET /api/time.
 * @param  hour, minute, second: Time output.
 * @param  year: 2-digit year (e.g. 26 = 2026).
 * @param  month, day, weekday: Date output (weekday: Monday=1..Sunday=7).
 * @retval WIFI_OK on success, error otherwise.
 */
WiFiStatus_t WiFi_HttpGetTime(uint8_t *hour, uint8_t *minute, uint8_t *second,
                               uint8_t *year, uint8_t *month, uint8_t *day,
                               uint8_t *weekday);

/**
 * @brief  Disconnect and power down the Wi-Fi module.
 */
void WiFi_DeInit(void);

/**
 * @brief  Set the 802.11 station power-save state the ACTIVE power mode
 *         wants (0 = off, 1 = on).
 *
 * Normal operation runs with this OFF: in power-save the station only
 * transmits around DTIM beacon windows, which throttles sustained uploads
 * to a trickle (measured ~1.3 KB/s on a phone hotspot where a laptop got
 * ~1 MB/s) and pins the SPI FLOW line low because the module cannot drain
 * its TX buffer. Small traffic (MQTT keepalives, HTTP headers) hides the
 * problem by fitting inside a single window.
 *
 * The low-power modes (PS-REST/dormant) call this to turn power-save ON
 * deliberately for energy savings. Image uploads temporarily force it off
 * and then restore whatever was requested here, so the energy behaviour is
 * preserved without crippling bulk transfers.
 */
void WiFi_SetPowerSave(uint8_t enable);

/**
 * @brief  Open a TCP socket and connect to a host. (ARCH-02)
 * @param  host: Hostname or IP string.
 * @param  port: Target TCP port.
 * @retval Socket ID (>0) on success, -1 on failure.
 */
int32_t WiFi_TcpConnect(const char *host, uint16_t port);

/**
 * @brief  Get the underlying MXCHIP Wi-Fi object instance.
 *         Useful for direct MX_WIFI API calls.
 */
MX_WIFIObject_t* wifi_obj_get(void);

#endif /* __WIFI_H */
