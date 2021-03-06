/* MIT License Copyright (c) 2021, h1zzz */

/* https://datatracker.ietf.org/doc/html/rfc6455 */

#include "websocket.h"

#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>

#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <mbedtls/base64.h>
#include <mbedtls/sha1.h>

#define WEBSOCKET_VERSION "13"
#define WEBSOCKET_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

#define FRAME_FIN 1 << 7
#define FRAME_RSV1 1 << 6
#define FRAME_RSV2 1 << 5
#define FRAME_RSV3 1 << 4
#define FRAME_OPCODE 0xf

#define FRAME_MASK 1 << 7

struct frame_hdr {
    uint8_t fin;
    uint8_t opcode;
    uint8_t mask;
    uint64_t len; /* payload length */
};

/*
 * The request MUST include a header field with the name
 * |Sec-WebSocket-Key|.  The value of this header field MUST be a
 * nonce consisting of a randomly selected 16-byte value that has
 * been base64-encoded (see Section 4 of [RFC4648]).  The nonce
 * MUST be selected randomly for each connection.
 */
static int generate_websocket_key(unsigned char *buf, size_t size)
{
    unsigned char tmp[16];
    size_t i, olen;

    for (i = 0; i < sizeof(tmp); i++)
        tmp[i] = xrand() % 0xff;

    if (mbedtls_base64_encode(buf, size, &olen, tmp, sizeof(tmp)) != 0) {
        fprintf(stderr, "mbedtls_base64_encode error\n");
        return -1;
    }

    return olen;
}

/* https://datatracker.ietf.org/doc/html/rfc6455#section-1.3 */
static int generate_websocket_accept(const unsigned char *ws_key,
                                     unsigned char ac_key[128])
{
    char buf[256] = {0};
    unsigned char output[20];
    int ret;
    mbedtls_sha1_context sha1;
    size_t olen;

    ret = snprintf(buf, sizeof(buf), "%s%s", ws_key, WEBSOCKET_GUID);
    if (ret <= 0 || (size_t)ret >= sizeof(buf)) {
        fprintf(stderr, "websocke-key or GUID length limit\n");
        return -1;
    }

    mbedtls_sha1_init(&sha1);
    mbedtls_sha1_starts(&sha1);
    mbedtls_sha1_update(&sha1, (unsigned char *)buf, (size_t)ret);
    mbedtls_sha1_finish(&sha1, output);
    mbedtls_sha1_free(&sha1);

    if (mbedtls_base64_encode(ac_key, 128, &olen, output, 20) != 0) {
        fprintf(stderr, "mbedtls_base64_encode error\n");
        return -1;
    }

    return (int)olen;
}

static int websocket_handshake(websocket_t *ws, const char *host,
                               const char *path)
{
    unsigned char ws_key[128] = {0}, ac_key[128] = {0};
    char buf[4096] = {0}, *ptr;
    int ret;

    /* Generate sec-websocket-key */
    ret = generate_websocket_key(ws_key, sizeof(ws_key));
    if (ret == -1) {
        fprintf(stderr, "generate_websocket_key error\n");
        return -1;
    }

    /*
     * the WebSocket client's handshake is an HTTP Upgrade request:
     * GET /chat HTTP/1.1
     * Host: server.example.com
     * Upgrade: websocket
     * Connection: Upgrade
     * Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==
     * Origin: http://example.com
     * Sec-WebSocket-Protocol: chat, superchat
     * Sec-WebSocket-Version: 13
     */

    /* Build request message */
    ret = snprintf(buf, sizeof(buf),
                   "GET %s HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\n"
                   "Connection: Upgrade\r\nSec-WebSocket-Key: %s\r\n"
                   "Sec-WebSocket-Version: %s\r\n\r\n",
                   path, host, ws_key, WEBSOCKET_VERSION);
    if (ret <= 0 || (size_t)ret >= sizeof(buf)) {
        fprintf(stderr, "snprintf error\n");
        return -1;
    }

    /* Send websocket handshake request */
    ret = net_write(&ws->net, buf, ret);
    if (ret == -1) {
        fprintf(stderr, "net_write error\n");
        return -1;
    }

    memset(buf, 0, sizeof(buf));

    /* Receive the status returned by the websocket server and parse it */
    /* -1 is to prevent overflow of string operations */
    ret = net_read(&ws->net, buf, sizeof(buf) - 1);
    if (ret == -1) {
        fprintf(stderr, "net_read error\n");
        return -1;
    }

    /* TODO: Strictly check the websocket handshake response */

    /* Check status */
    if (memcmp(buf, "HTTP/1.1 101 Switching Protocols", 32) != 0) {
        fprintf(stderr, "request failed, invalid status: %s\n", buf);
        return -1;
    }

    ret = generate_websocket_accept(ws_key, ac_key);
    if (ret == -1) {
        fprintf(stderr, "generate_websocket_accept error\n");
        return -1;
    }

    ptr = strstr(buf, "Sec-WebSocket-Accept: ");
    if (!ptr) {
        fprintf(
            stderr,
            "the handshake failed and the required Sec-WebSocket-Accept was "
            "not found: %s\n",
            buf);
        return -1;
    }

    ptr += 22; /* strlen("Sec-WebSocket-Accept: "); */

    /* Verify WebSocket-Accept */
    if (memcmp(ptr, ac_key, ret) != 0) {
        fprintf(stderr, "Sec-WebSocket-Accept verification failed: %s\n", buf);
        return -1;
    }

    return 0;
}

int websocket_connect(websocket_t *ws, const char *host, uint16_t port,
                      const char *path, int tls, const struct proxy *proxy)
{
    int ret;

    memset(ws, 0, sizeof(websocket_t));

    /* Connect to server */
    ret = net_connect(&ws->net, host, port, proxy);
    if (ret == -1) {
        net_close(&ws->net);
        fprintf(stderr, "net_connect error\n");
        return -1;
    }

    if (tls) {
        ret = net_tls_handshake(&ws->net);
        if (ret == -1) {
            net_close(&ws->net);
            fprintf(stderr, "net_tls_handshake error\n");
            return -1;
        }
    }

    ret = websocket_handshake(ws, host, path);
    if (ret == -1) {
        net_close(&ws->net);
        fprintf(stderr, "websocket_handshake error\n");
        return -1;
    }

    return 0;
}

/*
 *   0                   1                   2                   3
 *  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 * +-+-+-+-+-------+-+-------------+-------------------------------+
 * |F|R|R|R| opcode|M| Payload len |    Extended payload length    |
 * |I|S|S|S|  (4)  |A|     (7)     |             (16/64)           |
 * |N|V|V|V|       |S|             |   (if payload len==126/127)   |
 * | |1|2|3|       |K|             |                               |
 * +-+-+-+-+-------+-+-------------+ - - - - - - - - - - - - - - - +
 * |     Extended payload length continued, if payload len == 127  |
 * + - - - - - - - - - - - - - - - +-------------------------------+
 * |                               |Masking-key, if MASK set to 1  |
 * +-------------------------------+-------------------------------+
 * | Masking-key (continued)       |          Payload Data         |
 * +-------------------------------- - - - - - - - - - - - - - - - +
 * :                     Payload Data continued ...                :
 * + - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - +
 * |                     Payload Data continued ...                |
 * +---------------------------------------------------------------+
 */

static int websocket_read_frame_hdr(websocket_t *ws, struct frame_hdr *hdr)
{
    unsigned char buf[8] = {0};
    int ret;
    uint64_t len;

    ret = net_readn(&ws->net, buf, 2);
    if (ret == -1) {
        fprintf(stderr, "net_readn error\n");
        return -1;
    }

    hdr->fin = buf[0] & FRAME_FIN;
    hdr->opcode = buf[0] & FRAME_OPCODE;

    /*
     * MUST be 0 unless an extension is negotiated that defines meanings
     * for non-zero values.  If a nonzero value is received and none of
     * the negotiated extensions defines the meaning of such a nonzero
     * value, the receiving endpoint MUST _Fail the WebSocket Connection_.
     */
    if ((buf[0] & (FRAME_RSV1 | FRAME_RSV2 | FRAME_RSV3)) != 0) {
        fprintf(stderr, "RSVx reserved field, must be 0\n");
        return -1;
    }

    hdr->mask = buf[1] & FRAME_MASK;
    /* 0x7f = 0111 1111, Take the value of the lower seven bits */
    /* if 0-125, that is the payload length. */
    len = buf[1] & 0x7f;

    /* Multibyte length quantities are expressed in network byte order. */

    if (len == 126) {
        /*
         * If 126, the following 2 bytes interpreted as a 16-bit unsigned
         * integer are the payload length.
         */
        ret = net_readn(&ws->net, buf, 2);
        if (ret == -1) {
            fprintf(stderr, "net_readn error\n");
            return -1;
        }
        len = ntohs(*(uint16_t *)buf);
    } else if (len == 127) {
        /*
         * If 127, the following 8 bytes interpreted as a 64-bit unsigned
         * integer (the most significant bit MUST be 0) are the payload length.
         */
        ret = net_readn(&ws->net, buf, 8);
        if (ret == -1) {
            fprintf(stderr, "net_readn error\n");
            return -1;
        }
        len = ((uint64_t)ntohl((*(uint64_t *)buf) & 0xffffffff)) << 32;
        len |= ntohl((uint32_t)((*(uint64_t *)buf) >> 32));
    }

    hdr->len = len;

    return 0;
}

static int websocket_skip_remaining(websocket_t *ws)
{
    char buf[1024];
    size_t n;
    int ret;

    while (ws->remaining > 0) {
        n = ws->remaining > sizeof(buf) ? sizeof(buf) : (size_t)ws->remaining;
        ret = net_readn(&ws->net, buf, n);
        if (ret == -1) {
            fprintf(stderr, "net_readn error\n");
            return -1;
        }
        ws->remaining -= ret;
    }

    return 0;
}

int websocket_recv(websocket_t *ws, int *type, void *buf, size_t n)
{
    struct frame_hdr hdr;
    unsigned char mask_key[4];
    int ret;

    /* Skip the remaining unread data */
    if (ws->remaining > 0) {
        ret = websocket_skip_remaining(ws);
        if (ret == -1) {
            fprintf(stderr, "websocket_skip_remaing error\n");
            return -1;
        }
    }

    ret = websocket_read_frame_hdr(ws, &hdr);
    if (ret == -1) {
        fprintf(stderr, "websocket_read_frame_hdr error\n");
        return -1;
    }

    /*
     * For the current project, the size of the data transmitted in one frame is
     * sufficient, and the frame transmission is not supported for the time
     * being
     */
    if (hdr.fin == 0) {
        fprintf(stderr, "no support continuation frame\n");
        return -1;
    }

    if (type)
        *type = hdr.opcode;

    ws->remaining = hdr.len;

    /*
     * Masking-key: 0 or 4 bytes
     * All frames sent from the client to the server are masked by a
     * 32-bit value that is contained within the frame.  This field is
     * present if the mask bit is set to 1 and is absent if the mask bit
     * is set to 0.
     * See Section 5.3 for further information on client-to-server masking.
     */
    if (hdr.mask) {
        ret = net_readn(&ws->net, mask_key, sizeof(mask_key));
        if (ret == -1) {
            fprintf(stderr, "net_readn error\n");
            return -1;
        }
    }

    /* TODO: https://datatracker.ietf.org/doc/html/rfc6455#section-5.3 */
    n = n > ws->remaining ? (size_t)ws->remaining : n;
    ret = net_readn(&ws->net, buf, n);
    if (ret == -1) {
        fprintf(stderr, "net_readn error\n");
        return -1;
    }

    ws->remaining -= ret;

    return ret;
}

int websocket_send(websocket_t *ws, int type, const void *buf, size_t n)
{
    uint8_t header[14] = {0}, *mask_key;
    uint8_t payload[2048];
    const uint8_t *ptr;
    size_t i, len = 0;
    int ret, nwrite;

    header[0] |= FRAME_FIN;           /* set fin */
    header[0] |= (unsigned char)type; /* opcode */

    /* All frames sent from client to server have this bit set to 1 */
    header[1] |= FRAME_MASK; /* set mask */

    if (n <= 125) {
        header[1] |= n; /* payload length */
        len = 2;
    } else if (n <= 0xffff) {
        header[1] |= 126; /* payload length */
        *(uint16_t *)&header[2] = htons((uint16_t)n);
        len = 4;
    } else {
        header[1] |= 127; /* payload length */
        /*
         * Because n is a size_t type, it has only 32 bits, and the higher 32
         * bits are always 1
         */
        *(uint32_t *)&header[2] = htonl(0);
        *(uint32_t *)&header[6] = htonl((uint32_t)(n & 0xffffffff));
        len = 10;
    }

    /* set mask key */
    mask_key = &header[len];
    for (i = 0; i < 4; i++) {
        mask_key[i] = (uint8_t)(xrand() % 0xff);
        len++;
    }

    ret = net_write(&ws->net, header, len); /* send header */
    if (ret == -1) {
        fprintf(stderr, "net_write error\n");
        return -1;
    }

    /* transformed data and send payload */
    nwrite = 0;
    len = 0;
    ptr = buf;
    for (i = 0; i < n; i++) {
        payload[len++] = ptr[i] ^ mask_key[i % 4];
        if (len < sizeof(payload) && (i + 1) < n) {
            /* The buffer is not full and there is still data */
            continue;
        }
        ret = net_write(&ws->net, payload, len);
        if (ret == -1) {
            fprintf(stderr, "net_write error\n");
            return -1;
        }
        len = 0; /* Reset the buffer length and start filling again */
        nwrite += ret;
    }

    return nwrite;
}

void websocket_close(websocket_t *ws)
{
    websocket_send(ws, WEBSOCKET_CLOSE, NULL, 0);
    net_close(&ws->net);
}
