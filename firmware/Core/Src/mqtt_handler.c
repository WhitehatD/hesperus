/**
 * @file    mqtt_handler.c
 * @brief   MQTT Client — Bare-metal coreMQTT Integration
 * @author  Alexandru-Ionut Cioc
 * @date    2026
 *
 * Implements MQTT connectivity over the EMW3080 Wi-Fi module's TCP socket API.
 * Uses a simplified MQTT 3.1.1 implementation suitable for bare-metal operation
 * (no RTOS required).
 *
 * The firmware subscribes to `device/stm32/commands` for schedule reception
 * and publishes status updates to `device/stm32/status`.
 *
 * Transport layer: MX_WIFI_Socket_* APIs (TCP over SPI-attached EMW3080).
 *
 * NOTE: This uses a minimal hand-rolled MQTT 3.1.1 client rather than the
 * full coreMQTT library. This avoids the FreeRTOS dependency while keeping
 * the firmware footprint minimal. For production, consider coreMQTT-Agent.
 */

#include "mqtt_handler.h"
#include "firmware_config.h"
#include "debug_log.h"
#include "main.h"
#include "wifi.h"

#include "mx_wifi.h"
#include "mx_wifi_io.h"
#include "mx_address.h"

#include <string.h>
#include <stdio.h>

/* ═══════════════════════════════════════════════════════════════════════════
 *  Module State
 * ═══════════════════════════════════════════════════════════════════════════ */

static int32_t  s_socket       = -1;
static uint8_t  s_connected    = 0;
static uint16_t s_packet_id    = 1;
static uint32_t s_last_ping_tick = 0;  /* rate-limits MQTT_SendPing, see there */

/* Callback registered by application for incoming schedules */
static MQTTScheduleCallback_t s_schedule_callback = NULL;

/* TX/RX buffers */
#define MQTT_TX_BUFFER_SIZE  512
#define MQTT_RX_BUFFER_SIZE  2048  /* Must hold the full schedule JSON */

static uint8_t s_tx_buf[MQTT_TX_BUFFER_SIZE];
static uint8_t s_rx_buf[MQTT_RX_BUFFER_SIZE];

/* ═══════════════════════════════════════════════════════════════════════════
 *  MQTT 3.1.1 Packet Builders (minimal implementation)
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * Encode a remaining length field (MQTT variable-length encoding).
 * Returns number of bytes written to buf.
 */
static int _encode_remaining_length(uint8_t *buf, uint32_t length)
{
    int i = 0;
    do {
        uint8_t byte = length % 128;
        length /= 128;
        if (length > 0) byte |= 0x80;
        buf[i++] = byte;
    } while (length > 0);
    return i;
}

/**
 * Write a UTF-8 string (2-byte length prefix + data).
 */
static int _write_utf8_string(uint8_t *buf, const char *str)
{
    uint16_t len = (uint16_t)strlen(str);
    buf[0] = (uint8_t)(len >> 8);
    buf[1] = (uint8_t)(len & 0xFF);
    memcpy(buf + 2, str, len);
    return 2 + len;
}

/**
 * Build CONNECT packet.
 * Flags: Clean Session, Keep Alive = MQTT_KEEPALIVE_SECONDS.
 */
static int _build_connect_packet(uint8_t *buf, int buf_size)
{
    (void)buf_size;
    int pos = 0;

    /* Fixed header: CONNECT (type 1) */
    buf[pos++] = 0x10;

    /* We'll fill remaining length later */
    int remaining_pos = pos;
    pos += 1;  /* Reserve 1 byte (enough for packets < 128 bytes) */

    /* Variable header: Protocol Name "MQTT" */
    pos += _write_utf8_string(buf + pos, "MQTT");

    /* Protocol Level: 4 (MQTT 3.1.1) */
    buf[pos++] = 0x04;

    /* Connect Flags: Clean Session = 1 */
    uint8_t connect_flags = 0x02;  /* Clean Session */

    /* SEC-02: Conditionally set Username/Password flags */
    int has_username = (MQTT_USERNAME[0] != '\0');
    int has_password = (MQTT_PASSWORD[0] != '\0');
    if (has_username) connect_flags |= 0x80;  /* bit 7: Username Flag */
    if (has_password) connect_flags |= 0x40;  /* bit 6: Password Flag */
    buf[pos++] = connect_flags;

    /* Keep Alive */
    buf[pos++] = (uint8_t)(MQTT_KEEPALIVE_SECONDS >> 8);
    buf[pos++] = (uint8_t)(MQTT_KEEPALIVE_SECONDS & 0xFF);

    /* Payload: Client ID */
    pos += _write_utf8_string(buf + pos, MQTT_CLIENT_ID);

    /* Payload: Username (if set) */
    if (has_username)
        pos += _write_utf8_string(buf + pos, MQTT_USERNAME);

    /* Payload: Password (if set) */
    if (has_password)
        pos += _write_utf8_string(buf + pos, MQTT_PASSWORD);

    /* Patch remaining length */
    buf[remaining_pos] = (uint8_t)(pos - remaining_pos - 1);

    return pos;
}

/**
 * Build SUBSCRIBE packet for a single topic (QoS 0).
 */
static int _build_subscribe_packet(uint8_t *buf, int buf_size, const char *topic)
{
    (void)buf_size;
    int pos = 0;

    /* Fixed header: SUBSCRIBE (type 8, flags 0x02) */
    buf[pos++] = 0x82;

    int remaining_pos = pos;
    pos += 1;  /* Reserve for remaining length */

    /* Packet Identifier */
    buf[pos++] = (uint8_t)(s_packet_id >> 8);
    buf[pos++] = (uint8_t)(s_packet_id & 0xFF);
    s_packet_id++;

    /* Topic Filter + QoS */
    pos += _write_utf8_string(buf + pos, topic);
    buf[pos++] = 0x00;  /* QoS 0 */

    buf[remaining_pos] = (uint8_t)(pos - remaining_pos - 1);

    return pos;
}

/**
 * Build PUBLISH packet (QoS 0 — no ack needed).
 */
static int _build_publish_packet(uint8_t *buf, int buf_size,
                                  const char *topic, const char *payload)
{
    int topic_len   = (int)strlen(topic);
    int payload_len = (int)strlen(payload);
    int remaining   = 2 + topic_len + payload_len;

    if (2 + remaining > buf_size) return -1;

    int pos = 0;

    /* Fixed header: PUBLISH (type 3, QoS 0, no retain) */
    buf[pos++] = 0x30;
    pos += _encode_remaining_length(buf + pos, remaining);

    /* Topic */
    buf[pos++] = (uint8_t)(topic_len >> 8);
    buf[pos++] = (uint8_t)(topic_len & 0xFF);
    memcpy(buf + pos, topic, topic_len);
    pos += topic_len;

    /* Payload */
    memcpy(buf + pos, payload, payload_len);
    pos += payload_len;

    return pos;
}

/**
 * Build PINGREQ packet.
 */
static int _build_pingreq_packet(uint8_t *buf)
{
    buf[0] = 0xC0;  /* PINGREQ */
    buf[1] = 0x00;
    return 2;
}

/**
 * Build DISCONNECT packet.
 */
static int _build_disconnect_packet(uint8_t *buf)
{
    buf[0] = 0xE0;  /* DISCONNECT */
    buf[1] = 0x00;
    return 2;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  TCP Transport Helpers
 * ═══════════════════════════════════════════════════════════════════════════ */

static int32_t _tcp_send(const uint8_t *data, int len)
{
    if (s_socket < 0) return -1;
    /* SEC-08: Ensure '0' is passed as the flags parameter to MX_WIFI_Socket_send.
     * The last argument is 'flags' (e.g., MSG_DONTWAIT), not a timeout! */
    return MX_WIFI_Socket_send(wifi_obj_get(), s_socket, (uint8_t *)data, len, 0);
}

static int32_t _tcp_recv(uint8_t *buf, int max_len, int timeout_ms)
{
    if (s_socket < 0) return -1;
    uint32_t start_tick = HAL_GetTick();
    int32_t ret = 0;
    do {
        /* Use 0x08 (MSG_DONTWAIT in lwIP) to return immediately if no data */
        ret = MX_WIFI_Socket_recv(wifi_obj_get(), s_socket, buf, max_len, 0x08);
        if (ret > 0) return ret;
        
        if (timeout_ms > 0) {
            MX_WIFI_IO_YIELD(wifi_obj_get(), 5);
        }
    } while ((HAL_GetTick() - start_tick) < (uint32_t)timeout_ms);
    return ret;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Public API
 * ═══════════════════════════════════════════════════════════════════════════ */

int MQTT_Init(const MQTTConfig_t *config)
{
    LOG_INFO(TAG_MQTT, "Connecting to broker %s:%u...",
             config->broker_host, config->broker_port);

    /* ── Open TCP socket ─────────────────────────────── */

    s_socket = WiFi_TcpConnect(config->broker_host, config->broker_port);

    if (s_socket < 0)
    {
        LOG_ERROR(TAG_MQTT, "TCP connect failed to %s:%u",
                  config->broker_host, config->broker_port);
        return -1;
    }

    LOG_DEBUG(TAG_MQTT, "TCP connected, sending CONNECT packet...");

    /* ── Send MQTT CONNECT ───────────────────────────── */

    int pkt_len = _build_connect_packet(s_tx_buf, MQTT_TX_BUFFER_SIZE);
    if (_tcp_send(s_tx_buf, pkt_len) < pkt_len)
    {
        LOG_ERROR(TAG_MQTT, "Failed to send CONNECT");
        MX_WIFI_Socket_close(wifi_obj_get(), s_socket);
        s_socket = -1;
        return -1;
    }

    /* Wait for CONNACK */
    int32_t recv_len = _tcp_recv(s_rx_buf, 4, MQTT_CONNECT_TIMEOUT_MS);
    if (recv_len < 4 || s_rx_buf[0] != 0x20 || s_rx_buf[3] != 0x00)
    {
        LOG_ERROR(TAG_MQTT, "CONNACK error (recv=%ld, rc=%d)",
                  (long)recv_len, (recv_len >= 4) ? s_rx_buf[3] : -1);
        MX_WIFI_Socket_close(wifi_obj_get(), s_socket);
        s_socket = -1;
        return -1;
    }

    s_connected = 1;
    LOG_INFO(TAG_MQTT, "Connected to broker OK (session clean)");
    return 0;
}

int MQTT_SubscribeCommands(MQTTScheduleCallback_t callback)
{
    if (!s_connected) return -1;

    s_schedule_callback = callback;

    LOG_INFO(TAG_MQTT, "Subscribing to '%s'...", MQTT_TOPIC_COMMANDS);

    int pkt_len = _build_subscribe_packet(
        s_tx_buf, MQTT_TX_BUFFER_SIZE, MQTT_TOPIC_COMMANDS);

    if (_tcp_send(s_tx_buf, pkt_len) < pkt_len)
    {
        LOG_ERROR(TAG_MQTT, "Failed to send SUBSCRIBE");
        return -1;
    }

    /* Wait for SUBACK */
    int32_t recv_len = _tcp_recv(s_rx_buf, 5, MQTT_CONNECT_TIMEOUT_MS);
    if (recv_len < 5 || s_rx_buf[0] != 0x90)
    {
        LOG_ERROR(TAG_MQTT, "SUBACK error (recv=%ld)", (long)recv_len);
        return -1;
    }

    LOG_INFO(TAG_MQTT, "Subscribed OK (QoS granted: %d)", s_rx_buf[4]);
    return 0;
}

int MQTT_PublishStatus(const char *status_json)
{
    if (!s_connected) return -1;

    LOG_DEBUG(TAG_MQTT, "Publishing status: %.64s...", status_json);

    int pkt_len = _build_publish_packet(
        s_tx_buf, MQTT_TX_BUFFER_SIZE,
        MQTT_TOPIC_STATUS, status_json);

    if (pkt_len < 0 || _tcp_send(s_tx_buf, pkt_len) < pkt_len)
    {
        LOG_ERROR(TAG_MQTT, "Failed to publish status");
        return -1;
    }

    return 0;
}

int MQTT_PublishLog(const char *log_text)
{
    if (!s_connected) return -1;

    int pkt_len = _build_publish_packet(
        s_tx_buf, MQTT_TX_BUFFER_SIZE,
        MQTT_TOPIC_LOGS, log_text);

    if (pkt_len < 0 || _tcp_send(s_tx_buf, pkt_len) < pkt_len)
    {
        /* DO NOT call LOG_ERROR here to prevent recursive logging loops */
        return -1;
    }

    return 0;
}

void MQTT_ProcessLoop(void)
{
    if (!s_connected) return;

    /* Non-blocking receive with short timeout */
    int32_t recv_len = _tcp_recv(s_rx_buf, MQTT_RX_BUFFER_SIZE, MQTT_RECV_TIMEOUT_MS);

    if (recv_len <= 0) return;  /* No data available */

    uint8_t pkt_type = (s_rx_buf[0] >> 4) & 0x0F;

    switch (pkt_type)
    {
        case 3:  /* PUBLISH */
        {
            /*
             * Parse PUBLISH packet:
             *   Byte 0: Fixed header (type + flags)
             *   Byte 1+: Remaining length
             *   Then: 2-byte topic length + topic + payload
             */
            int pos = 1;

            /* Decode remaining length (SEC-03: max 4 bytes per MQTT 3.1.1 §2.2.3) */
            uint32_t remaining = 0;
            uint32_t multiplier = 1;
            uint8_t byte;
            int rl_bytes = 0;
            do {
                if (pos >= recv_len || rl_bytes >= 4) {
                    LOG_WARN(TAG_MQTT, "Malformed PUBLISH: truncated/overlong remaining length");
                    return;
                }
                byte = s_rx_buf[pos++];
                remaining += (byte & 0x7F) * multiplier;
                multiplier *= 128;
                rl_bytes++;
            } while (byte & 0x80);

            /* Bounds-check: remaining must fit within received data */
            if (remaining > (uint32_t)(recv_len - pos))
            {
                LOG_WARN(TAG_MQTT, "PUBLISH remaining=%lu exceeds recv=%ld, clamping",
                         (unsigned long)remaining, (long)(recv_len - pos));
                remaining = (uint32_t)(recv_len - pos);
            }

            /* Topic length — bounds check */
            if (pos + 2 > recv_len) {
                LOG_WARN(TAG_MQTT, "PUBLISH truncated: no topic length");
                return;
            }
            uint16_t topic_len = (s_rx_buf[pos] << 8) | s_rx_buf[pos + 1];
            pos += 2;

            /* Clamp topic_len to prevent overread */
            if (topic_len > remaining - 2 || topic_len > (uint16_t)(recv_len - pos))
            {
                LOG_WARN(TAG_MQTT, "PUBLISH topic_len=%u exceeds bounds, dropping", topic_len);
                break;
            }

            /* Topic string (for logging only) */
            char topic_str[64] = {0};
            if (topic_len < sizeof(topic_str))
            {
                memcpy(topic_str, s_rx_buf + pos, topic_len);
            }
            pos += topic_len;

            /* Payload — bounds-checked */
            uint32_t payload_len = remaining - 2 - topic_len;
            if (payload_len > (uint32_t)(recv_len - pos))
            {
                LOG_WARN(TAG_MQTT, "PUBLISH payload_len=%lu exceeds buffer, clamping",
                         (unsigned long)payload_len);
                payload_len = (uint32_t)(recv_len - pos);
            }
            char *payload = (char *)(s_rx_buf + pos);

            LOG_INFO(TAG_MQTT, "Received PUBLISH on '%s' (%lu bytes)",
                     topic_str, (unsigned long)payload_len);

            /* If this is a schedule command, invoke the callback */
            if (s_schedule_callback != NULL &&
                strncmp(topic_str, MQTT_TOPIC_COMMANDS, topic_len) == 0)
            {
                s_schedule_callback(payload, payload_len);
            }
            break;
        }

        case 13:  /* PINGRESP */
            LOG_DEBUG(TAG_MQTT, "PINGRESP received");
            break;

        default:
            LOG_DEBUG(TAG_MQTT, "Received packet type %d (%ld bytes)",
                      pkt_type, (long)recv_len);
            break;
    }
}

/**
 * @brief  Send a PINGREQ to keep the connection alive.
 *         Should be called periodically from the main loop.
 */
void MQTT_SendPing(void)
{
    if (!s_connected) return;

    /* 2026-08-23: RATE LIMIT. This used to fire on every call, and
     * WiFi_HttpPostImage calls it after EVERY chunk plus every retry. With
     * 1024-byte chunks a 10KB image meant 11+ pings in a couple of seconds
     * (observed live: 12 x "PINGREQ sent" inside ~2s with no PINGRESP in
     * between). Two costs, both real:
     *   - every ping is a full MIPC round trip on the module's SINGLE
     *     command channel, competing with the upload it is supposed to be
     *     protecting, so it actively slowed the transfer it rode along with;
     *   - it buried the actual upload story in the log.
     * The broker keepalive is MQTT_KEEPALIVE_SECONDS (60s); pinging more
     * often than roughly half that buys nothing. Callers can keep saying
     * "keep the link alive" as often as they like - this is now idempotent
     * and only puts a packet on the wire when one is actually due. */
    const uint32_t now = HAL_GetTick();
    const uint32_t min_gap_ms = (MQTT_KEEPALIVE_SECONDS * 1000u) / 2u;

    if (s_last_ping_tick != 0u && (now - s_last_ping_tick) < min_gap_ms)
        return;

    s_last_ping_tick = now;

    int pkt_len = _build_pingreq_packet(s_tx_buf);
    _tcp_send(s_tx_buf, pkt_len);
    LOG_DEBUG(TAG_MQTT, "PINGREQ sent");
}

void MQTT_Disconnect(void)
{
    if (!s_connected) return;

    LOG_INFO(TAG_MQTT, "Disconnecting from broker...");

    int pkt_len = _build_disconnect_packet(s_tx_buf);
    _tcp_send(s_tx_buf, pkt_len);

    MX_WIFI_Socket_close(wifi_obj_get(), s_socket);
    s_socket    = -1;
    s_connected = 0;

    LOG_INFO(TAG_MQTT, "Disconnected");
}

uint8_t MQTT_IsConnected(void)
{
    return s_connected;
}
