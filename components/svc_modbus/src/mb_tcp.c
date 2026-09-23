#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>

#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include "esp_check.h"

#include "mb_proto.h"
#include "mb_tcp.h"
#include "svc_modbus.h"

static const char *TAG = "mb_tcp";

#define CONNECT_TOUT_MS   3000
#define IO_TOUT_MS        1000
#define MBAP_LEN          7      /* tid(2) + proto(2) + len(2) + unit(1) */

static char     s_host[40];
static uint16_t s_port;
static int      s_sock = -1;
static bool     s_up;
static uint16_t s_tid;

esp_err_t mb_tcp_start(const char *host, uint16_t port)
{
    ESP_RETURN_ON_FALSE(host && host[0], ESP_ERR_INVALID_ARG, TAG, "no host");

    mb_tcp_stop();
    strlcpy(s_host, host, sizeof(s_host));
    s_port = port ? port : 502;
    s_up   = true;

    ESP_LOGI(TAG, "TCP master targeting %s:%u (connects on first request)",
             s_host, s_port);
    return ESP_OK;
}

esp_err_t mb_tcp_stop(void)
{
    if (s_sock >= 0) {
        close(s_sock);
        s_sock = -1;
    }
    s_up = false;
    return ESP_OK;
}

bool mb_tcp_is_up(void)
{
    return s_up;
}

bool mb_tcp_is_connected(void)
{
    return s_sock >= 0;
}

static void drop_socket(void)
{
    if (s_sock >= 0) {
        close(s_sock);
        s_sock = -1;
    }
}

/** Opens the socket if it is not already up. */
static esp_err_t ensure_socket(void)
{
    if (s_sock >= 0) {
        return ESP_OK;
    }

    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", s_port);

    const struct addrinfo hints = {
        .ai_family   = AF_INET,
        .ai_socktype = SOCK_STREAM,
    };
    struct addrinfo *res = NULL;

    if (getaddrinfo(s_host, port_str, &hints, &res) != 0 || !res) {
        ESP_LOGW(TAG, "cannot resolve %s", s_host);
        return ESP_ERR_NOT_FOUND;
    }

    const int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock < 0) {
        freeaddrinfo(res);
        return ESP_FAIL;
    }

    const struct timeval tv = {
        .tv_sec  = CONNECT_TOUT_MS / 1000,
        .tv_usec = (CONNECT_TOUT_MS % 1000) * 1000,
    };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if (connect(sock, res->ai_addr, res->ai_addrlen) != 0) {
        ESP_LOGW(TAG, "connect to %s:%u failed (errno %d)", s_host, s_port, errno);
        close(sock);
        freeaddrinfo(res);
        return ESP_ERR_NOT_FOUND;
    }
    freeaddrinfo(res);

    /* Modbus exchanges are tiny and latency-sensitive; Nagle would sit on
     * every request waiting for more to send. */
    const int one = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    const struct timeval io_tv = {
        .tv_sec  = IO_TOUT_MS / 1000,
        .tv_usec = (IO_TOUT_MS % 1000) * 1000,
    };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &io_tv, sizeof(io_tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &io_tv, sizeof(io_tv));

    s_sock = sock;
    ESP_LOGI(TAG, "connected to %s:%u", s_host, s_port);
    return ESP_OK;
}

/** Reads exactly `want` bytes, or fails. */
static esp_err_t recv_exact(uint8_t *dst, size_t want)
{
    size_t got = 0;

    while (got < want) {
        const int n = recv(s_sock, dst + got, want - got, 0);
        if (n <= 0) {
            return (n == 0) ? ESP_ERR_INVALID_STATE : ESP_ERR_TIMEOUT;
        }
        got += (size_t)n;
    }
    return ESP_OK;
}

static esp_err_t transact_once(uint8_t unit, uint8_t fn, uint16_t addr,
                               uint16_t count, const uint16_t *wr_data, void *out)
{
    uint8_t adu[MB_TCP_ADU_MAX];

    const size_t pdu_len = mb_build_request(&adu[MBAP_LEN], fn, addr, count, wr_data);
    ESP_RETURN_ON_FALSE(pdu_len, ESP_ERR_NOT_SUPPORTED, TAG, "function %u", fn);

    const uint16_t tid = ++s_tid;
    const uint16_t len = (uint16_t)(pdu_len + 1);   /* unit id + PDU */

    adu[0] = (uint8_t)(tid >> 8);
    adu[1] = (uint8_t)(tid & 0xFF);
    adu[2] = 0;                       /* protocol id: 0 == Modbus */
    adu[3] = 0;
    adu[4] = (uint8_t)(len >> 8);
    adu[5] = (uint8_t)(len & 0xFF);
    adu[6] = unit;

    const size_t adu_len = MBAP_LEN + pdu_len;
    if (send(s_sock, adu, adu_len, 0) != (int)adu_len) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t head[MBAP_LEN];
    ESP_RETURN_ON_ERROR(recv_exact(head, MBAP_LEN), TAG, "no header");

    const uint16_t resp_tid = (uint16_t)((head[0] << 8) | head[1]);
    const uint16_t resp_len = (uint16_t)((head[4] << 8) | head[5]);

    if (resp_len < 2 || resp_len > MB_PDU_MAX + 1) {
        ESP_LOGW(TAG, "bogus MBAP length %u", resp_len);
        return ESP_ERR_INVALID_RESPONSE;
    }

    uint8_t pdu[MB_PDU_MAX];
    ESP_RETURN_ON_ERROR(recv_exact(pdu, resp_len - 1), TAG, "short body");

    /* A mismatched transaction id means a stale reply is still in flight;
     * the socket is out of step, so drop it rather than desynchronise. */
    if (resp_tid != tid) {
        ESP_LOGW(TAG, "transaction id %u, expected %u", resp_tid, tid);
        return ESP_ERR_INVALID_STATE;
    }

    return mb_parse_response(pdu, resp_len - 1, fn, count, out);
}

esp_err_t mb_tcp_transact(uint8_t unit, uint8_t fn, uint16_t addr,
                          uint16_t count, const uint16_t *wr_data, void *out)
{
    ESP_RETURN_ON_FALSE(s_up, ESP_ERR_INVALID_STATE, TAG, "TCP is down");

    /* One reconnect attempt: gateways drop idle sockets, and rediscovering
     * that on every poll should not surface as a user-visible failure. */
    for (int attempt = 0; attempt < 2; attempt++) {
        ESP_RETURN_ON_ERROR(ensure_socket(), TAG, "connect");

        const esp_err_t err = transact_once(unit, fn, addr, count, wr_data, out);
        if (err != ESP_ERR_INVALID_STATE && err != ESP_ERR_TIMEOUT) {
            return err;         /* success, or a protocol-level failure */
        }

        ESP_LOGW(TAG, "socket lost (%s), reconnecting", esp_err_to_name(err));
        drop_socket();
    }

    return ESP_ERR_TIMEOUT;
}
