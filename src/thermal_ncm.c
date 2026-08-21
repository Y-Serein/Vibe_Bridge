#include "thermal_ncm.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "bsp/board_api.h"
#include "dhserver.h"
#include "dnserver.h"
#include "lwip/etharp.h"
#include "lwip/init.h"
#include "lwip/pbuf.h"
#include "lwip/tcp.h"
#include "lwip/timeouts.h"
#include "lwip/udp.h"
#include "netif/ethernet.h"
#include "pico/stdlib.h"
#include "pico/unique_id.h"
#include "hardware/sync.h"
#include "hardware/watchdog.h"
#include "tusb.h"

#include "init.h"
#include "thermal_ota.h"
#include "web_assets.h"

extern volatile uint32_t g_capture_incomplete_frames;
extern volatile uint32_t g_capture_unexpected_heads;
extern volatile uint16_t g_ffc_bg_max_row_jump_adu;
extern volatile uint8_t g_ffc_bg_max_row_jump_line;
extern volatile uint16_t g_ffc_bg_min_row_mean;
extern volatile uint16_t g_ffc_bg_max_row_mean;
extern volatile uint32_t g_capture_packet_heads;
extern volatile uint32_t g_capture_packet_mids;
extern volatile uint32_t g_capture_packet_tails;
extern volatile uint32_t g_capture_packet_invalid;
extern volatile uint16_t g_capture_runtime_resyncs;
extern volatile uint16_t g_capture_last_resync_invalid_run;
extern volatile uint16_t g_capture_max_invalid_run;
extern volatile uint32_t g_capture_complete_sequences;
extern volatile uint32_t g_capture_bad_tail_sequences;
extern volatile uint32_t g_capture_head_after_head;
extern volatile uint32_t g_capture_tail_without_head;
extern volatile uint16_t g_capture_active_mid_count;
extern volatile uint16_t g_capture_last_interrupted_mid_count;
extern volatile uint16_t g_capture_last_tail_mid_count;
extern volatile uint16_t g_capture_max_interrupted_mid_count;
extern volatile uint32_t g_capture_interrupt_mid_0_31;
extern volatile uint32_t g_capture_interrupt_mid_32_63;
extern volatile uint32_t g_capture_interrupt_mid_64_95;
extern volatile uint32_t g_capture_interrupt_mid_96_plus;
extern volatile uint16_t g_capture_last_frame_uniform_rows;
extern volatile uint16_t g_capture_last_frame_uniform_max_run;
extern uint8_t g_capture_raw_diag[];
extern int8_t g_capture_row_phase[];
extern uint16_t g_capture_row_ref_diag[];

#define INIT_IP4(a, b, c, d) { PP_HTONL(LWIP_MAKEU32(a, b, c, d)) }
#define HTTP_CLIENT_COUNT 4
#define HTTP_REQUEST_BYTES 1280
#define HTTP_HEADER_BYTES  640
#define HTTP_PORT          80

uint8_t tud_network_mac_address[6] = {0x48, 0xDA, 0x35, 0x60, 0x00, 0x01};

static ip4_addr_t ipaddr = INIT_IP4(192, 168, 17, 1);
static const ip4_addr_t netmask = INIT_IP4(255, 255, 255, 0);
static const ip4_addr_t gateway = INIT_IP4(0, 0, 0, 0);
static struct netif netif_data;
static struct pbuf* received_frame;
static struct tcp_pcb* http_listener;
static thermal_frame_acquire_fn frame_acquire;
static thermal_frame_release_fn frame_release;
static thermal_frame_acquire_fn adc16_frame_acquire;
static thermal_frame_release_fn adc16_frame_release;
static thermal_frame_cancel_fn adc16_frame_cancel;
static struct sipeed_thermal_meta_v1 frame_meta[2];
static struct sipeed_thermal_meta_v1 adc16_meta;
static uint32_t frame_counter;
static uint32_t adc16_generation;
static volatile uint8_t latest_meta_token = UINT8_MAX;
static uint8_t network_segment = 17;
static uint32_t network_reboot_at_ms;

static dhcp_entry_t dhcp_entries[] = {
    {{0}, INIT_IP4(192, 168, 17, 2), 24 * 60 * 60},
    {{0}, INIT_IP4(192, 168, 17, 3), 24 * 60 * 60},
    {{0}, INIT_IP4(192, 168, 17, 4), 24 * 60 * 60},
};

static dhcp_config_t dhcp_config = {
    .router = INIT_IP4(192, 168, 17, 1),
    .port = 67,
    .dns = INIT_IP4(192, 168, 17, 1),
    .domain = "ir.sipeed.com",
    .num_entry = (int)(sizeof(dhcp_entries) / sizeof(dhcp_entries[0])),
    .entries = dhcp_entries,
};


enum response_kind {
    RESPONSE_NONE,
    RESPONSE_FLASH,
    RESPONSE_FRAME_TEMP8,
    RESPONSE_STREAM_TEMP8,
    RESPONSE_FRAME_ADC16,
    RESPONSE_OTA_UPLOAD,
    RESPONSE_NETWORK_UPDATE,
};

struct http_client {
    struct tcp_pcb* pcb;
    char request[HTTP_REQUEST_BYTES];
    char header[HTTP_HEADER_BYTES];
    uint16_t request_used;
    uint16_t header_used;
    uint16_t header_sent;
    uint8_t header_match;
    uint8_t response;
    bool request_done;
    bool head_only;
    const uint8_t* flash_data;
    uint32_t body_length;
    uint32_t body_sent;
    uint32_t ota_expected;
    uint32_t ota_received;
    uint8_t ota_error;
    bool ota_owner;
    bool ota_v1;
    uint8_t config_body[32];
    uint8_t config_expected;
    uint8_t config_received;
    struct thermal_frame_view frame;
    bool frame_held;
};

static struct http_client http_clients[HTTP_CLIENT_COUNT];
static struct http_client* stream_owner;
static struct http_client* adc16_owner;

static uint16_t read_be16(const uint8_t* p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static uint32_t read_be32(const uint8_t* p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
           | ((uint32_t)p[2] << 8) | p[3];
}

static uint32_t pixel_crc32(const uint8_t* data, size_t len);

struct __attribute__((packed)) product_network_settings {
    char magic[8];
    uint8_t version;
    uint8_t segment;
    uint8_t reserved[2];
    uint32_t crc32;
};

static void network_apply_segment(uint8_t segment)
{
    network_segment = segment;
    IP4_ADDR(&ipaddr, 192, 168, segment, 1);
    dhcp_config.router = ipaddr;
    dhcp_config.dns = ipaddr;
    for (size_t i = 0; i < sizeof(dhcp_entries) / sizeof(dhcp_entries[0]); i++) {
        IP4_ADDR(&dhcp_entries[i].addr, 192, 168, segment, (uint8_t)(i + 2));
    }
}

static bool network_settings_valid(
    const struct product_network_settings* settings)
{
    return memcmp(settings->magic, "SIPNET1", 8) == 0
           && settings->version == 1 && settings->segment != 0
           && settings->segment != 255
           && pixel_crc32(
                  (const uint8_t*)settings,
                  offsetof(struct product_network_settings, crc32))
                  == settings->crc32;
}

static void network_load_settings(void)
{
    struct product_network_settings settings;
    if (thermal_settings_read(&settings, sizeof(settings))
        && network_settings_valid(&settings)) {
        network_apply_segment(settings.segment);
        return;
    }

    /* Layout v1 stored this record at 0x0ff000. That sector is part of the
     * new OTA image slot, so migrate a valid record before the first upload
     * can erase it. A failed copy is harmless for this boot: use the legacy
     * value now and retry migration after the next reset. */
    if (thermal_settings_read_legacy(&settings, sizeof(settings))
        && network_settings_valid(&settings)) {
        (void)thermal_settings_write(&settings, sizeof(settings));
        network_apply_segment(settings.segment);
        return;
    }

    network_apply_segment(17);
}

static bool network_save_segment(uint8_t segment)
{
    struct product_network_settings settings;
    memset(&settings, 0, sizeof(settings));
    memcpy(settings.magic, "SIPNET1", 8);
    settings.version = 1;
    settings.segment = segment;
    settings.crc32 = pixel_crc32(
        (const uint8_t*)&settings,
        offsetof(struct product_network_settings, crc32));
    return thermal_settings_write(&settings, sizeof(settings));
}

static float canonical_nan(void)
{
    const uint32_t bits = 0x7fc00000u;
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static void init_mac(void)
{
    pico_unique_board_id_t id;
    pico_get_unique_board_id(&id);

    /* Company OUI followed by 20 UID-derived bits. Host and device-side lwIP
     * addresses differ by the U/L bit, as required by TinyUSB's network glue. */
    tud_network_mac_address[0] = 0x48;
    tud_network_mac_address[1] = 0xDA;
    tud_network_mac_address[2] = 0x35;
    tud_network_mac_address[3] = (uint8_t)(0x60 | (id.id[5] & 0x0F));
    tud_network_mac_address[4] = id.id[6];
    tud_network_mac_address[5] = id.id[7];
}

static err_t linkoutput_fn(struct netif* netif, struct pbuf* p)
{
    (void)netif;
    if (!tud_ready()) {
        return ERR_USE;
    }

    if (!tud_network_can_xmit(p->tot_len)) {
        /* Let TinyUSB retire a completed NTB once, but never spin Core1 for
         * milliseconds behind a busy USB endpoint. ERR_MEM leaves lwIP in
         * control of TCP backpressure and retry timing. */
        tud_task();
        if (!tud_network_can_xmit(p->tot_len)) {
            return ERR_MEM;
        }
    }
    tud_network_xmit(p, 0);
    return ERR_OK;
}

static err_t netif_init_cb(struct netif* netif)
{
    netif->mtu = CFG_TUD_NET_MTU;
    netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP
                   | NETIF_FLAG_LINK_UP | NETIF_FLAG_UP;
    netif->name[0] = 'T';
    netif->name[1] = 'H';
    netif->linkoutput = linkoutput_fn;
    netif->output = etharp_output;
    return ERR_OK;
}

static bool dns_name_equal(const char* a, const char* b)
{
    while (*a && *b) {
        char ca = *a++;
        char cb = *b++;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb) return false;
    }
    return *a == *b;
}

static bool captive_dns_query(const char* name, ip4_addr_t* addr)
{
    static const char* const domains[] = {
        "ir.sipeed.com",
        "captive.apple.com",
        "connectivitycheck.gstatic.com",
        "connectivitycheck.android.com",
        "clients3.google.com",
        "www.gstatic.com",
        "www.google.cn",
        "www.googleapis.cn",
        "connect.rom.miui.com",
        "connectivitycheck.platform.hicloud.com",
        "connectivitycheck.platform.hihonorcloud.com",
        "wifi.vivo.com.cn",
        "conn1.oppomobile.com",
        "conn2.oppomobile.com",
        "conn-service-cn-04.allawntech.com",
        "conn-service-cn-05.allawntech.com",
        "connectivity.samsung.com.cn",
        "connectivitycheck.samsung.com",
        "connectivitycheck.meizu.com",
        "www.qualcomm.com",
        "www.qualcomm.cn",
    };
    for (size_t i = 0; i < sizeof(domains) / sizeof(domains[0]); i++) {
        if (dns_name_equal(name, domains[i])) {
            *addr = ipaddr;
            return true;
        }
    }
    return false;
}

static bool request_has_path(const char* request, const char* path)
{
    const char* p = strchr(request, ' ');
    if (!p) return false;
    p++;
    const size_t len = strlen(path);
    return strncmp(p, path, len) == 0
           && (p[len] == ' ' || p[len] == '?' || p[len] == '\0');
}

static bool request_path_has_prefix(const char* request, const char* prefix)
{
    const char* p = strchr(request, ' ');
    return p && strncmp(p + 1, prefix, strlen(prefix)) == 0;
}

static bool ascii_equal_ci(char a, char b)
{
    if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
    if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
    return a == b;
}

static bool request_content_length(const char* request, uint32_t* length)
{
    static const char name[] = "content-length:";
    for (const char* p = request; *p; p++) {
        size_t i = 0;
        while (i < sizeof(name) - 1 && p[i]
               && ascii_equal_ci(p[i], name[i])) {
            i++;
        }
        if (i != sizeof(name) - 1) continue;
        p += i;
        while (*p == ' ' || *p == '\t') p++;
        if (*p < '0' || *p > '9') return false;
        uint32_t value = 0;
        do {
            const uint32_t digit = (uint32_t)(*p++ - '0');
            if (value > (UINT32_MAX - digit) / 10u) return false;
            value = value * 10u + digit;
        } while (*p >= '0' && *p <= '9');
        *length = value;
        return true;
    }
    return false;
}

static bool request_header_value_starts_with(const char* request,
                                             const char* name,
                                             const char* value)
{
    const size_t name_len = strlen(name);
    const size_t value_len = strlen(value);
    for (const char* p = request; *p; p++) {
        size_t i = 0;
        while (i < name_len && p[i] && ascii_equal_ci(p[i], name[i])) i++;
        if (i != name_len || p[i] != ':') continue;
        p += i + 1;
        while (*p == ' ' || *p == '\t') p++;
        i = 0;
        while (i < value_len && p[i]
               && ascii_equal_ci(p[i], value[i])) i++;
        return i == value_len;
    }
    return false;
}

static bool captive_probe_request(const char* request)
{
    static const char* const paths[] = {
        "/hotspot-detect.html",
        "/library/test/success.html",
        "/generate_204",
        "/generate204",
        "/gen_204",
    };
    const char* p = strchr(request, ' ');
    if (p && strncmp(p + 1, "/generate_204_", 14) == 0) return true;
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        if (request_has_path(request, paths[i])) return true;
    }
    return false;
}

static void client_release_frame(struct http_client* client)
{
    if (client->frame_held) {
        if (client->response == RESPONSE_FRAME_ADC16) {
            if (adc16_frame_release) {
                adc16_frame_release(client->frame.token);
            }
        } else if (frame_release) {
            frame_release(client->frame.token);
        }
        client->frame_held = false;
    }
}

static void client_clear(struct http_client* client)
{
    if (stream_owner == client) stream_owner = NULL;
    if (client->ota_owner) thermal_ota_abort();
    client_release_frame(client);
    if (adc16_owner == client) {
        adc16_owner = NULL;
        if (adc16_frame_cancel) adc16_frame_cancel();
    }
    memset(client, 0, sizeof(*client));
}

static void client_abort(struct http_client* client)
{
    struct tcp_pcb* pcb = client->pcb;
    if (pcb) {
        tcp_arg(pcb, NULL);
        tcp_recv(pcb, NULL);
        tcp_sent(pcb, NULL);
        tcp_poll(pcb, NULL, 0);
        tcp_abort(pcb);
    }
    client_clear(client);
}

static bool client_is_stream(const struct http_client* client)
{
    return client->response == RESPONSE_STREAM_TEMP8;
}

static void client_claim_stream(struct http_client* owner)
{
    /* A frame can only be acquired by one client. Browsers do not always
     * finish closing an aborted fetch before opening the replacement stream,
     * so an old quality/mode client could otherwise acquire and discard every
     * newly published frame. Make the newest request the sole stream owner
     * and retire all older streams before changing the producer mode. */
    for (size_t i = 0; i < HTTP_CLIENT_COUNT; i++) {
        struct http_client* client = &http_clients[i];
        if (client != owner && client->pcb && client_is_stream(client)) {
            client_abort(client);
        }
    }
    stream_owner = owner;
}

static void client_claim_adc16(struct http_client* owner)
{
    if (adc16_owner && adc16_owner != owner && adc16_owner->pcb) {
        client_abort(adc16_owner);
    }
    adc16_owner = owner;
}

static bool latest_meta_snapshot(struct sipeed_thermal_meta_v1* out)
{
    for (unsigned attempt = 0; attempt < 2; attempt++) {
        const uint8_t token = latest_meta_token;
        if (token >= 2) return false;
        const uint32_t number = frame_meta[token].frame_number;
        memcpy(out, &frame_meta[token], sizeof(*out));
        __dmb();
        if (number != 0 && number == frame_meta[token].frame_number
            && out->meta_crc32
                   == pixel_crc32((const uint8_t*)out,
                                  SIPEED_THERMAL_META_BYTES - 4u)) {
            return true;
        }
    }
    return false;
}

static void board_serial_string(char* out, size_t bytes)
{
    pico_get_unique_board_id_string(out, bytes);
}

static void prepare_response(struct http_client* client)
{
    const bool is_head = strncmp(client->request, "HEAD ", 5) == 0;
    const bool is_get = strncmp(client->request, "GET ", 4) == 0;
    const bool is_read = is_get || is_head;
    const bool is_write = strncmp(client->request, "POST ", 5) == 0
                          || strncmp(client->request, "PUT ", 4) == 0;
    client->head_only = is_head;
    client->response = RESPONSE_FLASH;
    client->flash_data = thermal_web_index_start;
    client->body_length =
        (uint32_t)thermal_web_asset_size(thermal_web_index_start,
                                         thermal_web_index_end);

    const char* status = "200 OK";
    const char* type = "text/html; charset=utf-8";
    const char* extra = "";
    char dynamic_extra[96];

    if (strncmp(client->request, "PUT ", 4) == 0
        && request_has_path(client->request, "/api/v1/network")) {
        uint32_t content_length = 0;
        if (!request_header_value_starts_with(client->request,
                                              "X-Sipeed-Request", "1")
            || !request_header_value_starts_with(client->request,
                                                 "Content-Type",
                                                 "application/json")
            || !request_content_length(client->request, &content_length)
            || content_length == 0
            || content_length >= sizeof(client->config_body)) {
            status = "400 Bad Request";
            type = "application/json; charset=utf-8";
            static const char bad_request[] =
                "{\"ok\":false,\"error\":{\"code\":\"bad_network_request\",\"message\":\"JSON body and X-Sipeed-Request: 1 required\"}}\n";
            client->flash_data = (const uint8_t*)bad_request;
            client->body_length = sizeof(bad_request) - 1;
        } else {
            client->response = RESPONSE_NETWORK_UPDATE;
            client->config_expected = (uint8_t)content_length;
            client->config_received = 0;
            client->header_used = 0;
            client->header_sent = 0;
            client->body_length = 0;
            client->body_sent = 0;
            return;
        }
    } else if (strncmp(client->request, "POST ", 5) == 0
        && (request_has_path(client->request, "/ota.bin")
            || request_has_path(client->request, "/api/v1/ota"))) {
        const bool v1 = request_has_path(client->request, "/api/v1/ota");
        uint32_t content_length = 0;
        enum thermal_ota_result result = THERMAL_OTA_BAD_SIZE;
        if (network_reboot_at_ms) {
            result = THERMAL_OTA_BUSY;
        } else if ((!v1
             || (request_header_value_starts_with(client->request,
                                                  "X-Sipeed-Request", "1")
                 && request_header_value_starts_with(
                     client->request, "Content-Type",
                     "application/octet-stream")))
            && request_content_length(client->request, &content_length)) {
            result = thermal_ota_begin(content_length);
        }
        if (result == THERMAL_OTA_OK) {
            client->response = RESPONSE_OTA_UPLOAD;
            client->ota_expected = content_length;
            client->ota_received = 0;
            client->ota_error = THERMAL_OTA_OK;
            client->ota_owner = true;
            client->ota_v1 = v1;
            client->header_used = 0;
            client->header_sent = 0;
            client->body_length = 0;
            client->body_sent = 0;
            return;
        }
        status = result == THERMAL_OTA_BUSY
                     ? "409 Conflict"
                     : "400 Bad Request";
        type = "application/json; charset=utf-8";
        const int body_len = snprintf(
            client->request, sizeof(client->request),
            "{\"ok\":false,\"error\":\"%s\"}\n",
            thermal_ota_result_string(result));
        client->flash_data = (const uint8_t*)client->request;
        client->body_length = body_len > 0 ? (uint32_t)body_len : 0;
    } else if (is_read
               && request_has_path(client->request, "/api/v1/device")) {
        type = "application/json; charset=utf-8";
        char serial[2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1];
        board_serial_string(serial, sizeof(serial));
        const int body_len = snprintf(
            client->request, sizeof(client->request),
            "{\"api\":{\"name\":\"sipeed-thermal-http\",\"major\":1,\"minor\":0},"
            "\"device\":{\"model\":\"T160\",\"serial\":\"%s\",\"hardware\":\"1.0\",\"firmware\":\"%s\",\"build\":\"%s\"},"
            "\"sensor\":{\"native_width\":160,\"native_height\":120,\"recommended_rotation\":1},"
            "\"network\":{\"segment\":%u,\"device_ip\":\"192.168.%u.1\",\"configurable\":true,\"config_url\":\"/api/v1/network\"},"
            "\"streaming\":{\"max_stream_clients\":1,\"simultaneous_formats\":false},"
            "\"measurement\":{\"supported\":true,\"center_roi\":{\"width\":5,\"height\":5},\"config_url\":\"/api/v1/measurement\",\"summary_url\":\"/api/v1/thermal\"},"
            "\"super_resolution\":{\"device\":false,\"client_recommended\":true,\"client_max_native_width\":256,\"client_max_native_height\":192},"
            "\"formats\":["
            "{\"id\":\"adc16\",\"width\":160,\"height\":120,\"stride_bytes\":320,\"bits_per_pixel\":16,\"effective_bits\":14,\"temperature_encoding\":\"none\",\"metadata\":\"sipeed-thermal-meta-v1\",\"frame\":\"/frame.adc16\"},"
            "{\"id\":\"temp8\",\"width\":160,\"height\":120,\"stride_bytes\":160,\"bits_per_pixel\":8,\"temperature_encoding\":\"linear\",\"metadata\":\"sipeed-thermal-meta-v1\",\"frame\":\"/frame.temp8\",\"stream\":\"/stream.temp8\"}],"
            "\"ota\":{\"supported\":true,\"format\":\"t160-bin\",\"max_bytes\":%lu,\"rollback\":false,\"upload\":\"/api/v1/ota\",\"status\":\"/api/v1/ota/status\"}}\n",
            serial, PICO_TN160_VERSION, thermal_ota_build_id(),
            (unsigned)network_segment,
            (unsigned)network_segment,
            (unsigned long)thermal_ota_max_image_bytes());
        client->flash_data = (const uint8_t*)client->request;
        client->body_length = body_len > 0 ? (uint32_t)body_len : 0;
    } else if (is_read
               && request_has_path(client->request, "/api/v1/network")) {
        type = "application/json; charset=utf-8";
        const int body_len = snprintf(
            client->request, sizeof(client->request),
            "{\"mode\":\"fixed_private_subnet\",\"segment\":%u,"
            "\"device_ip\":\"192.168.%u.1\",\"prefix_length\":24,"
            "\"dhcp_first\":\"192.168.%u.2\",\"dhcp_last\":\"192.168.%u.4\","
            "\"default_segment\":17}\n",
            (unsigned)network_segment, (unsigned)network_segment,
            (unsigned)network_segment, (unsigned)network_segment);
        client->flash_data = (const uint8_t*)client->request;
        client->body_length = body_len > 0 ? (uint32_t)body_len : 0;
    } else if (is_read
               && request_has_path(client->request,
                                   "/api/v1/measurement")) {
        type = "application/json; charset=utf-8";
        static const char measurement[] =
            "{\"processing\":\"device\",\"revision\":0,\"configurable\":false,"
            "\"defaults\":{\"emissivity\":0.95,\"distance_m\":1.0,"
            "\"reflected_temperature_c\":25.0}}\n";
        client->flash_data = (const uint8_t*)measurement;
        client->body_length = sizeof(measurement) - 1;
    } else if (is_read
               && request_has_path(client->request, "/api/v1/thermal")) {
        type = "application/json; charset=utf-8";
        struct sipeed_thermal_meta_v1 meta;
        if (latest_meta_snapshot(&meta)
            && (meta.flags & SIPEED_META_HLC_TEMPERATURES_VALID)
            && (meta.flags & SIPEED_META_HLC_COORDINATES_VALID)) {
            const int body_len = snprintf(
                client->request, sizeof(client->request),
                "{\"frame_number\":%lu,\"timestamp_ms\":%lu,\"valid\":true,"
                "\"center\":{\"celsius\":%.2f,\"x\":%u,\"y\":%u},"
                "\"minimum\":{\"celsius\":%.2f,\"x\":%u,\"y\":%u},"
                "\"maximum\":{\"celsius\":%.2f,\"x\":%u,\"y\":%u},"
                "\"measurement_config_id\":%lu}\n",
                (unsigned long)meta.frame_number,
                (unsigned long)meta.timestamp_ms,
                (double)meta.center_temperature, (unsigned)meta.center_x,
                (unsigned)meta.center_y, (double)meta.min_temperature,
                (unsigned)meta.min_x, (unsigned)meta.min_y,
                (double)meta.max_temperature, (unsigned)meta.max_x,
                (unsigned)meta.max_y,
                (unsigned long)meta.measurement_config_id);
            client->flash_data = (const uint8_t*)client->request;
            client->body_length = body_len > 0 ? (uint32_t)body_len : 0;
        } else {
            static const char unavailable[] =
                "{\"frame_number\":0,\"timestamp_ms\":0,\"valid\":false}\n";
            client->flash_data = (const uint8_t*)unavailable;
            client->body_length = sizeof(unavailable) - 1;
        }
    } else if (is_read
               && request_has_path(client->request,
                                   "/assets/sr-fast.wasm")) {
        type = "application/wasm";
        client->flash_data = thermal_sr_fast_wasm_start;
        client->body_length =
            (uint32_t)thermal_web_asset_size(thermal_sr_fast_wasm_start,
                                             thermal_sr_fast_wasm_end);
    } else if (is_read && request_has_path(client->request,
                                "/assets/sr-fast-simd.wasm")) {
        type = "application/wasm";
        client->flash_data = thermal_sr_fast_simd_wasm_start;
        client->body_length =
            (uint32_t)thermal_web_asset_size(thermal_sr_fast_simd_wasm_start,
                                             thermal_sr_fast_simd_wasm_end);
    } else if (is_read
               && request_has_path(client->request,
                                   "/assets/sr-fast.bin")) {
        type = "application/octet-stream";
        client->flash_data = thermal_sr_fast_weights_start;
        client->body_length =
            (uint32_t)thermal_web_asset_size(thermal_sr_fast_weights_start,
                                             thermal_sr_fast_weights_end);
    } else if (request_has_path(client->request, "/diag/raw16.bin")) {
        type = "application/octet-stream";
        client->flash_data = g_capture_raw_diag;
        client->body_length = THERMAL_FRAME_IMAGE_BYTES * 2u;
    } else if (request_has_path(client->request, "/diag/rowref16.bin")) {
        type = "application/octet-stream";
        client->flash_data = (const uint8_t*)g_capture_row_ref_diag;
        client->body_length = THERMAL_FRAME_HEIGHT * sizeof(uint16_t);
    } else if (request_has_path(client->request, "/diag/rowphase.bin")) {
        type = "application/octet-stream";
        client->flash_data = (const uint8_t*)g_capture_row_phase;
        client->body_length = THERMAL_FRAME_HEIGHT;
    } else if (request_has_path(client->request, "/diag/rxring.bin")) {
        type = "application/octet-stream";
        client->flash_data = (const uint8_t*)&rxring;
        client->body_length = sizeof(rxring);
    } else if (is_read && request_has_path(client->request,
                                "/api/v1/ota/status")) {
        type = "application/json; charset=utf-8";
        const int body_len = snprintf(
            client->request, sizeof(client->request),
            "{\"state\":\"%s\",\"received_bytes\":%lu,"
            "\"total_bytes\":%lu,\"max_bytes\":%lu,\"build\":\"%s\"}\n",
            thermal_ota_active()
                ? "receiving"
                : thermal_ota_install_pending() ? "reboot_pending" : "idle",
            (unsigned long)thermal_ota_received_bytes(),
            (unsigned long)thermal_ota_expected_bytes(),
            (unsigned long)thermal_ota_max_image_bytes(),
            thermal_ota_build_id());
        client->flash_data = (const uint8_t*)client->request;
        client->body_length = body_len > 0 ? (uint32_t)body_len : 0;
    } else if (request_has_path(client->request, "/ota/status")) {
        type = "application/json; charset=utf-8";
        const int body_len = snprintf(
            client->request, sizeof(client->request),
            "{\"ota\":true,\"build\":\"%s\",\"max_image_bytes\":%lu,"
            "\"calibration_crc32\":\"%08lx\",\"watchdog_reset\":%s,"
            "\"diag_stage\":%lu,\"diag_offset\":\"%08lx\","
            "\"diag_result\":%ld}\n",
            thermal_ota_build_id(),
            (unsigned long)thermal_ota_max_image_bytes(),
            (unsigned long)thermal_ota_calibration_crc32(),
            thermal_ota_last_watchdog_reset() ? "true" : "false",
            (unsigned long)thermal_ota_diag_stage(),
            (unsigned long)thermal_ota_diag_offset(),
            (long)thermal_ota_diag_result());
        client->flash_data = (const uint8_t*)client->request;
        client->body_length = body_len > 0 ? (uint32_t)body_len : 0;
    } else if (request_has_path(client->request, "/ota.bin")
               || request_has_path(client->request, "/api/v1/ota")) {
        status = "405 Method Not Allowed";
        type = "application/json; charset=utf-8";
        static const char method[] =
            "{\"ok\":false,\"error\":\"POST raw .bin required\"}\n";
        client->flash_data = (const uint8_t*)method;
        client->body_length = sizeof(method) - 1;
        extra = "Allow: POST\r\n";
    } else if (captive_probe_request(client->request)) {
        status = "302 Found";
        snprintf(dynamic_extra, sizeof(dynamic_extra),
                 "Location: http://ir.sipeed.com/\r\nPragma: no-cache\r\n");
        extra = dynamic_extra;
        client->body_length = 0;
    } else if (is_head
               && request_has_path(client->request, "/frame.adc16")) {
        const int len = snprintf(
            client->header, sizeof(client->header),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/vnd.sipeed.thermal-frame; version=1; format=adc16\r\n"
            "Content-Length: %u\r\n"
            "Cache-Control: no-store\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Access-Control-Expose-Headers: *\r\n"
            "X-Sipeed-Protocol: 1.0\r\n"
            "X-Sipeed-Format: adc16\r\n"
            "X-Sipeed-Width: %u\r\n"
            "X-Sipeed-Height: %u\r\n"
            "X-Sipeed-Stride-Bytes: %u\r\n"
            "X-Sipeed-Payload-Bytes: %u\r\n"
            "X-Sipeed-Meta-Bytes: %u\r\n"
            "X-Sipeed-Record-Bytes: %u\r\n"
            "Connection: close\r\n\r\n",
            (unsigned)SIPEED_THERMAL_ADC16_WIRE_BYTES,
            (unsigned)THERMAL_FRAME_WIDTH,
            (unsigned)THERMAL_FRAME_HEIGHT,
            (unsigned)(THERMAL_FRAME_WIDTH * 2u),
            (unsigned)THERMAL_FRAME_ADC16_BYTES,
            (unsigned)SIPEED_THERMAL_META_BYTES,
            (unsigned)SIPEED_THERMAL_ADC16_WIRE_BYTES);
        client->header_used = (uint16_t)(len > 0 ? len : 0);
        client->header_sent = 0;
        client->body_length = SIPEED_THERMAL_ADC16_WIRE_BYTES;
        client->body_sent = 0;
        return;
    } else if (is_head
               && (request_has_path(client->request, "/stream.temp8")
                   || request_has_path(client->request, "/frame.temp8"))) {
        const bool stream_head =
            request_has_path(client->request, "/stream.temp8");
        char content_length[40] = "";
        if (!stream_head) {
            snprintf(content_length, sizeof(content_length),
                     "Content-Length: %u\r\n",
                     (unsigned)SIPEED_THERMAL_TEMP8_WIRE_BYTES);
        }
        const int len = snprintf(
            client->header, sizeof(client->header),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/vnd.sipeed.thermal-%s; version=1; format=temp8\r\n"
            "%s"
            "Cache-Control: no-store\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Access-Control-Expose-Headers: *\r\n"
            "X-Sipeed-Protocol: 1.0\r\n"
            "X-Sipeed-Format: temp8\r\n"
            "X-Sipeed-Width: %u\r\n"
            "X-Sipeed-Height: %u\r\n"
            "X-Sipeed-Stride-Bytes: %u\r\n"
            "X-Sipeed-Payload-Bytes: %u\r\n"
            "X-Sipeed-Meta-Bytes: %u\r\n"
            "X-Sipeed-Record-Bytes: %u\r\n"
            "Connection: close\r\n\r\n",
            stream_head ? "stream" : "frame",
            content_length,
            (unsigned)THERMAL_FRAME_WIDTH,
            (unsigned)THERMAL_FRAME_HEIGHT,
            (unsigned)THERMAL_FRAME_WIDTH,
            (unsigned)THERMAL_FRAME_IMAGE_BYTES,
            (unsigned)SIPEED_THERMAL_META_BYTES,
            (unsigned)SIPEED_THERMAL_TEMP8_WIRE_BYTES);
        client->header_used = (uint16_t)(len > 0 ? len : 0);
        client->header_sent = 0;
        client->body_length = stream_head ? 0
                                          : SIPEED_THERMAL_TEMP8_WIRE_BYTES;
        client->body_sent = 0;
        return;
    } else if (is_get
               && request_has_path(client->request, "/stream.temp8")) {
        client->response = RESPONSE_STREAM_TEMP8;
        client_claim_stream(client);
        client->body_length = 0;
        const int len = snprintf(
            client->header, sizeof(client->header),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/vnd.sipeed.thermal-stream; version=1; format=temp8\r\n"
            "Cache-Control: no-store\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Access-Control-Expose-Headers: *\r\n"
            "X-Sipeed-Protocol: 1.0\r\n"
            "X-Sipeed-Format: temp8\r\n"
            "X-Sipeed-Width: %u\r\n"
            "X-Sipeed-Height: %u\r\n"
            "X-Sipeed-Stride-Bytes: %u\r\n"
            "X-Sipeed-Payload-Bytes: %u\r\n"
            "X-Sipeed-Meta-Bytes: %u\r\n"
            "X-Sipeed-Record-Bytes: %u\r\n"
            "Connection: close\r\n\r\n",
            (unsigned)THERMAL_FRAME_WIDTH,
            (unsigned)THERMAL_FRAME_HEIGHT,
            (unsigned)THERMAL_FRAME_WIDTH,
            (unsigned)THERMAL_FRAME_IMAGE_BYTES,
            (unsigned)SIPEED_THERMAL_META_BYTES,
            (unsigned)SIPEED_THERMAL_TEMP8_WIRE_BYTES);
        client->header_used = (uint16_t)(len > 0 ? len : 0);
        client->header_sent = 0;
        client->body_sent = 0;
        return;
    } else if (is_get
               && request_has_path(client->request, "/frame.adc16")) {
        client->response = RESPONSE_FRAME_ADC16;
        client_claim_adc16(client);
        client->body_length = SIPEED_THERMAL_ADC16_WIRE_BYTES;
        const int len = snprintf(
            client->header, sizeof(client->header),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/vnd.sipeed.thermal-frame; version=1; format=adc16\r\n"
            "Content-Length: %u\r\n"
            "Cache-Control: no-store\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Access-Control-Expose-Headers: *\r\n"
            "X-Sipeed-Protocol: 1.0\r\n"
            "X-Sipeed-Format: adc16\r\n"
            "X-Sipeed-Width: %u\r\n"
            "X-Sipeed-Height: %u\r\n"
            "X-Sipeed-Stride-Bytes: %u\r\n"
            "X-Sipeed-Payload-Bytes: %u\r\n"
            "X-Sipeed-Meta-Bytes: %u\r\n"
            "X-Sipeed-Record-Bytes: %u\r\n"
            "Connection: close\r\n\r\n",
            (unsigned)SIPEED_THERMAL_ADC16_WIRE_BYTES,
            (unsigned)THERMAL_FRAME_WIDTH,
            (unsigned)THERMAL_FRAME_HEIGHT,
            (unsigned)(THERMAL_FRAME_WIDTH * 2u),
            (unsigned)THERMAL_FRAME_ADC16_BYTES,
            (unsigned)SIPEED_THERMAL_META_BYTES,
            (unsigned)SIPEED_THERMAL_ADC16_WIRE_BYTES);
        client->header_used = (uint16_t)(len > 0 ? len : 0);
        client->header_sent = 0;
        client->body_sent = 0;
        return;
    } else if (is_get
               && request_has_path(client->request, "/frame.temp8")) {
        if (frame_acquire && frame_acquire(&client->frame)) {
            client->frame_held = true;
            client->response = RESPONSE_FRAME_TEMP8;
            client->body_length = SIPEED_THERMAL_TEMP8_WIRE_BYTES;
            const int len = snprintf(
                client->header, sizeof(client->header),
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: application/vnd.sipeed.thermal-frame; version=1; format=temp8\r\n"
                "Content-Length: %u\r\n"
                "Cache-Control: no-store\r\n"
                "Access-Control-Allow-Origin: *\r\n"
                "Access-Control-Expose-Headers: *\r\n"
                "X-Sipeed-Protocol: 1.0\r\n"
                "X-Sipeed-Format: temp8\r\n"
                "X-Sipeed-Width: %u\r\n"
                "X-Sipeed-Height: %u\r\n"
                "X-Sipeed-Stride-Bytes: %u\r\n"
                "X-Sipeed-Payload-Bytes: %u\r\n"
                "X-Sipeed-Meta-Bytes: %u\r\n"
                "X-Sipeed-Record-Bytes: %u\r\n"
                "Connection: close\r\n\r\n",
                (unsigned)SIPEED_THERMAL_TEMP8_WIRE_BYTES,
                (unsigned)THERMAL_FRAME_WIDTH,
                (unsigned)THERMAL_FRAME_HEIGHT,
                (unsigned)THERMAL_FRAME_WIDTH,
                (unsigned)THERMAL_FRAME_IMAGE_BYTES,
                (unsigned)SIPEED_THERMAL_META_BYTES,
                (unsigned)SIPEED_THERMAL_TEMP8_WIRE_BYTES);
            client->header_used = (uint16_t)(len > 0 ? len : 0);
            client->header_sent = 0;
            client->body_sent = 0;
            return;
        }
        static const char busy[] =
            "{\"ok\":false,\"error\":{\"code\":\"frame_not_ready\","
            "\"message\":\"Thermal frame is not ready\"}}\n";
        status = "503 Service Unavailable";
        type = "application/json; charset=utf-8";
        extra = "Retry-After: 1\r\n";
        client->flash_data = (const uint8_t*)busy;
        client->body_length = sizeof(busy) - 1;
    } else if (request_has_path(client->request, "/favicon.ico")) {
        status = "204 No Content";
        client->body_length = 0;
    } else if (request_path_has_prefix(client->request, "/api/v1/")
               || request_path_has_prefix(client->request, "/frame.")
               || request_path_has_prefix(client->request, "/stream.")) {
        status = "404 Not Found";
        type = "application/json; charset=utf-8";
        static const char not_found[] =
            "{\"ok\":false,\"error\":{\"code\":\"not_found\","
            "\"message\":\"Unsupported endpoint or thermal format\"}}\n";
        client->flash_data = (const uint8_t*)not_found;
        client->body_length = sizeof(not_found) - 1;
    } else if (!request_has_path(client->request, "/")) {
        status = "302 Found";
        snprintf(dynamic_extra, sizeof(dynamic_extra),
                 "Location: http://ir.sipeed.com/\r\n");
        extra = dynamic_extra;
        client->body_length = 0;
    }

    const int len = snprintf(client->header, sizeof(client->header),
                             "HTTP/1.1 %s\r\n"
                             "Content-Type: %s\r\n"
                             "Content-Length: %lu\r\n"
                             "Cache-Control: no-store\r\n"
                             "%s"
                             "%sConnection: close\r\n\r\n",
                             status, type, (unsigned long)client->body_length,
                             is_write ? ""
                                      : "Access-Control-Allow-Origin: *\r\n",
                             extra);
    client->header_used = (uint16_t)(len > 0 ? len : 0);
    client->header_sent = 0;
    client->body_sent = 0;
}

static void ota_prepare_result(struct http_client* client,
                               enum thermal_ota_result result,
                               uint32_t crc32)
{
    client->ota_owner = false;
    client->response = RESPONSE_FLASH;
    client->body_sent = 0;
    const bool ok = result == THERMAL_OTA_OK;
    int body_len;
    if (ok) {
        if (client->ota_v1) {
            body_len = snprintf(
                client->request, sizeof(client->request),
                "{\"accepted\":true,\"bytes\":%lu,\"crc32\":\"%08lx\","
                "\"state\":\"verified\",\"rebooting\":true}\n",
                (unsigned long)client->ota_expected,
                (unsigned long)crc32);
        } else {
            body_len = snprintf(
                client->request, sizeof(client->request),
                "{\"ok\":true,\"bytes\":%lu,\"crc32\":\"%08lx\","
                "\"rebooting\":true}\n",
                (unsigned long)client->ota_expected,
                (unsigned long)crc32);
        }
    } else {
        body_len = snprintf(
            client->request, sizeof(client->request),
            "{\"ok\":false,\"error\":\"%s\"}\n",
            thermal_ota_result_string(result));
    }
    client->flash_data = (const uint8_t*)client->request;
    client->body_length = body_len > 0 ? (uint32_t)body_len : 0;
    const int header_len = snprintf(
        client->header, sizeof(client->header),
        "HTTP/1.1 %s\r\n"
        "Content-Type: application/json; charset=utf-8\r\n"
        "Content-Length: %lu\r\n"
        "Cache-Control: no-store\r\n"
        "Connection: close\r\n\r\n",
        ok ? (client->ota_v1 ? "202 Accepted" : "200 OK")
           : "500 Internal Server Error",
        (unsigned long)client->body_length);
    client->header_used = (uint16_t)(header_len > 0 ? header_len : 0);
    client->header_sent = 0;
    if (ok) thermal_ota_schedule_install(1500u);
}

static void network_prepare_result(struct http_client* client)
{
    client->config_body[client->config_received] = '\0';
    unsigned segment = 0;
    int consumed = 0;
    const int fields = sscanf((const char*)client->config_body,
                              " { \"segment\" : %u } %n",
                              &segment, &consumed);
    const char* tail = (const char*)client->config_body + consumed;
    while (*tail == ' ' || *tail == '\t' || *tail == '\r'
           || *tail == '\n') tail++;
    const bool valid = fields == 1 && consumed > 0 && *tail == '\0'
                       && segment >= 1 && segment <= 254;
    const bool saved = valid && network_save_segment((uint8_t)segment);
    const char* status = saved ? "202 Accepted"
                         : valid ? "503 Service Unavailable"
                                 : "400 Bad Request";
    int body_len;
    if (saved) {
        body_len = snprintf(
            client->request, sizeof(client->request),
            "{\"accepted\":true,\"new_ip\":\"192.168.%u.1\","
            "\"rebooting\":true}\n", segment);
        network_reboot_at_ms =
            to_ms_since_boot(get_absolute_time()) + 1500u;
    } else {
        body_len = snprintf(
            client->request, sizeof(client->request),
            "{\"ok\":false,\"error\":{\"code\":\"%s\",\"message\":\"%s\"}}\n",
            valid ? "settings_write_failed" : "invalid_segment",
            valid ? "Unable to persist network setting"
                  : "segment must be an integer from 1 through 254");
    }
    client->response = RESPONSE_FLASH;
    client->flash_data = (const uint8_t*)client->request;
    client->body_length = body_len > 0 ? (uint32_t)body_len : 0;
    client->body_sent = 0;
    const int header_len = snprintf(
        client->header, sizeof(client->header),
        "HTTP/1.1 %s\r\n"
        "Content-Type: application/json; charset=utf-8\r\n"
        "Content-Length: %lu\r\n"
        "Cache-Control: no-store\r\n"
        "Connection: close\r\n\r\n",
        status, (unsigned long)client->body_length);
    client->header_used = (uint16_t)(header_len > 0 ? header_len : 0);
    client->header_sent = 0;
}

static err_t http_sent(void* arg, struct tcp_pcb* pcb, uint16_t len);
static err_t http_poll(void* arg, struct tcp_pcb* pcb);
static void http_error(void* arg, err_t err);
static err_t http_recv(void* arg, struct tcp_pcb* pcb, struct pbuf* p,
                       err_t err);

static err_t client_send(struct http_client* client)
{
    struct tcp_pcb* pcb = client->pcb;
    if (client->response == RESPONSE_FRAME_ADC16
        && !client->frame_held) {
        if (adc16_frame_acquire
            && adc16_frame_acquire(&client->frame)) {
            client->frame_held = true;
        } else {
            return ERR_OK;
        }
    }
    while (client->header_sent < client->header_used) {
        const uint16_t room = tcp_sndbuf(pcb);
        if (!room) break;
        uint16_t len = (uint16_t)(client->header_used - client->header_sent);
        if (len > room) len = room;
        const err_t err = tcp_write(pcb, client->header + client->header_sent,
                                    len, TCP_WRITE_FLAG_COPY);
        if (err != ERR_OK) break;
        client->header_sent += len;
    }

    const bool is_stream = client_is_stream(client);
    if (is_stream && stream_owner != client) return ERR_OK;
    if (client->header_sent == client->header_used && is_stream
        && !client->frame_held && frame_acquire
        && frame_acquire(&client->frame)) {
        client->frame_held = true;
        client->body_length = SIPEED_THERMAL_TEMP8_WIRE_BYTES;
        client->body_sent = 0;
    }
    if (client->header_sent == client->header_used && !client->head_only) {
        while (client->body_sent < client->body_length) {
            uint16_t room = tcp_sndbuf(pcb);
            if (!room) break;
            const uint8_t* src;
            uint32_t left;
            if (client->response == RESPONSE_FRAME_TEMP8
                       || client->response == RESPONSE_STREAM_TEMP8) {
                if (client->body_sent < THERMAL_FRAME_IMAGE_BYTES) {
                    src = client->frame.pixels + client->body_sent;
                    left = THERMAL_FRAME_IMAGE_BYTES - client->body_sent;
                } else {
                    const uint32_t meta_off =
                        client->body_sent - THERMAL_FRAME_IMAGE_BYTES;
                    src = (const uint8_t*)client->frame.meta + meta_off;
                    left = SIPEED_THERMAL_META_BYTES - meta_off;
                }
            } else if (client->response == RESPONSE_FRAME_ADC16) {
                if (client->body_sent < THERMAL_FRAME_ADC16_BYTES) {
                    src = client->frame.pixels + client->body_sent;
                    left = THERMAL_FRAME_ADC16_BYTES - client->body_sent;
                } else {
                    const uint32_t meta_off =
                        client->body_sent - THERMAL_FRAME_ADC16_BYTES;
                    src = (const uint8_t*)client->frame.meta + meta_off;
                    left = SIPEED_THERMAL_META_BYTES - meta_off;
                }
            } else {
                src = client->flash_data + client->body_sent;
                left = client->body_length - client->body_sent;
            }
            uint16_t len = left > room ? room : (uint16_t)left;
            if (len > TCP_MSS) len = TCP_MSS;
            const err_t err = tcp_write(pcb, src, len, TCP_WRITE_FLAG_COPY);
            if (err != ERR_OK) break;
            client->body_sent += len;
        }
    }

    tcp_output(pcb);
    if (is_stream) {
        if (client->frame_held
            && client->body_sent == client->body_length) {
            client_release_frame(client);
            client->body_length = 0;
            client->body_sent = 0;
        }
        return ERR_OK;
    }

    const bool complete = client->header_sent == client->header_used
                          && (client->head_only
                              || client->body_sent == client->body_length);
    if (complete) {
        client_release_frame(client);
        tcp_arg(pcb, NULL);
        tcp_recv(pcb, NULL);
        tcp_sent(pcb, NULL);
        tcp_poll(pcb, NULL, 0);
        const err_t err = tcp_close(pcb);
        if (err == ERR_OK) {
            client_clear(client);
            return ERR_OK;
        }
        tcp_arg(pcb, client);
        tcp_recv(pcb, http_recv);
        tcp_sent(pcb, http_sent);
        tcp_err(pcb, http_error);
        tcp_poll(pcb, http_poll, 8);
    }
    return ERR_OK;
}

static err_t http_sent(void* arg, struct tcp_pcb* pcb, uint16_t len)
{
    (void)pcb;
    (void)len;
    return client_send((struct http_client*)arg);
}

static err_t http_poll(void* arg, struct tcp_pcb* pcb)
{
    (void)pcb;
    struct http_client* client = (struct http_client*)arg;
    if (client->response == RESPONSE_OTA_UPLOAD
        || client->response == RESPONSE_NETWORK_UPDATE) return ERR_OK;
    if (client->request_done) return client_send(client);
    client_abort(client);
    return ERR_ABRT;
}

static void http_error(void* arg, err_t err)
{
    (void)err;
    struct http_client* client = (struct http_client*)arg;
    if (client) client_clear(client);
}

static err_t http_recv(void* arg, struct tcp_pcb* pcb, struct pbuf* p, err_t err)
{
    struct http_client* client = (struct http_client*)arg;
    if (!p) {
        client_abort(client);
        return ERR_ABRT;
    }
    if (err != ERR_OK) {
        pbuf_free(p);
        return err;
    }

    for (struct pbuf* part = p; part; part = part->next) {
        const uint8_t* data = (const uint8_t*)part->payload;
        uint16_t i = 0;
        while (i < part->len) {
            if (!client->request_done) {
                const uint8_t byte = data[i++];
                if ((size_t)client->request_used + 1
                    < sizeof(client->request)) {
                    client->request[client->request_used++] = (char)byte;
                    client->request[client->request_used] = '\0';
                }
                static const char end[] = "\r\n\r\n";
                if (byte == (uint8_t)end[client->header_match]) {
                    client->header_match++;
                    if (client->header_match == sizeof(end) - 1) {
                        client->request_done = true;
                        prepare_response(client);
                    }
                } else {
                    client->header_match = byte == '\r' ? 1 : 0;
                }
                continue;
            }

            if (client->response == RESPONSE_NETWORK_UPDATE) {
                uint16_t take = (uint16_t)(part->len - i);
                const uint8_t remaining =
                    client->config_expected - client->config_received;
                if (take > remaining) take = remaining;
                memcpy(client->config_body + client->config_received,
                       data + i, take);
                client->config_received += (uint8_t)take;
                i += take;
                if (client->config_received == client->config_expected) {
                    network_prepare_result(client);
                    break;
                }
                if (!take) break;
                continue;
            }
            if (client->response != RESPONSE_OTA_UPLOAD) break;
            const uint32_t remaining =
                client->ota_expected - client->ota_received;
            uint16_t take = (uint16_t)(part->len - i);
            if (take > remaining) take = (uint16_t)remaining;
            if (take) {
                if (client->ota_error == THERMAL_OTA_OK) {
                    const enum thermal_ota_result result =
                        thermal_ota_write(data + i, take);
                    if (result != THERMAL_OTA_OK) {
                        client->ota_error = (uint8_t)result;
                    }
                }
                client->ota_received += take;
                i += take;
            }
            if (client->ota_received == client->ota_expected) {
                uint32_t crc32 = 0;
                enum thermal_ota_result result =
                    (enum thermal_ota_result)client->ota_error;
                if (result == THERMAL_OTA_OK) {
                    result = thermal_ota_finish(&crc32);
                } else {
                    thermal_ota_abort();
                }
                ota_prepare_result(client, result, crc32);
                break;
            }
            if (!take) break;
        }
    }
    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    if (client->request_done && client->response != RESPONSE_OTA_UPLOAD
        && client->response != RESPONSE_NETWORK_UPDATE) {
        if (client->response == RESPONSE_NONE) prepare_response(client);
        return client_send(client);
    }
    return ERR_OK;
}

static err_t http_accept(void* arg, struct tcp_pcb* pcb, err_t err)
{
    (void)arg;
    if (err != ERR_OK) return err;
    for (size_t i = 0; i < HTTP_CLIENT_COUNT; i++) {
        struct http_client* client = &http_clients[i];
        if (!client->pcb) {
            memset(client, 0, sizeof(*client));
            client->pcb = pcb;
            tcp_arg(pcb, client);
            tcp_recv(pcb, http_recv);
            tcp_sent(pcb, http_sent);
            tcp_err(pcb, http_error);
            tcp_poll(pcb, http_poll, 8);
            tcp_nagle_disable(pcb);
            return ERR_OK;
        }
    }
    tcp_abort(pcb);
    return ERR_ABRT;
}

bool tud_network_recv_cb(const uint8_t* src, uint16_t size)
{
    if (received_frame) return false;
    if (size) {
        received_frame = pbuf_alloc(PBUF_RAW, size, PBUF_POOL);
        if (!received_frame) {
            /* Drop on transient pool exhaustion. Returning false here would
             * retain the NCM datagram with no later recv_renew() trigger. */
            return true;
        }
        if (pbuf_take(received_frame, src, size) != ERR_OK) {
            pbuf_free(received_frame);
            received_frame = NULL;
            return true;
        }
    }
    return true;
}

uint16_t tud_network_xmit_cb(uint8_t* dst, void* ref, uint16_t arg)
{
    (void)arg;
    struct pbuf* p = (struct pbuf*)ref;
    return pbuf_copy_partial(p, dst, p->tot_len, 0);
}

void tud_network_init_cb(void)
{
    if (received_frame) {
        pbuf_free(received_frame);
        received_frame = NULL;
    }
}

void thermal_ncm_note_frame(uint8_t token, const uint8_t* frame,
                            bool calibrated)
{
    if (token >= 2 || !frame) return;
    struct sipeed_thermal_meta_v1* meta = &frame_meta[token];
    const float invalid = canonical_nan();
    const uint32_t temp_low_bits =
        read_be32(&frame[THERMAL_FRAME_IMAGE_BYTES + 55]);
    const uint32_t temp_high_bits =
        read_be32(&frame[THERMAL_FRAME_IMAGE_BYTES + 59]);
    float temp_low;
    float temp_high;
    memcpy(&temp_low, &temp_low_bits, sizeof(temp_low));
    memcpy(&temp_high, &temp_high_bits, sizeof(temp_high));
    const int16_t center_x10 =
        (int16_t)read_be16(&frame[THERMAL_FRAME_IMAGE_BYTES + 49]);
    const int16_t min_x10 =
        (int16_t)read_be16(&frame[THERMAL_FRAME_IMAGE_BYTES + 63]);
    const int16_t max_x10 =
        (int16_t)read_be16(&frame[THERMAL_FRAME_IMAGE_BYTES + 65]);
    const uint8_t min_x = frame[THERMAL_FRAME_IMAGE_BYTES + 67];
    const uint8_t min_y = frame[THERMAL_FRAME_IMAGE_BYTES + 68];
    const uint8_t max_x = frame[THERMAL_FRAME_IMAGE_BYTES + 69];
    const uint8_t max_y = frame[THERMAL_FRAME_IMAGE_BYTES + 70];
    const uint16_t sensor_vtemp =
        read_be16(&frame[THERMAL_FRAME_IMAGE_BYTES + 1]);

    memset(meta, 0, sizeof(*meta));
    memcpy(meta->magic, "SITHMETA", 8);
    meta->version_major = 1;
    meta->version_minor = 0;
    meta->meta_bytes = sizeof(*meta);
    meta->flags = SIPEED_META_IMAGE_VALID
                  | SIPEED_META_PAYLOAD_CRC_VALID
                  | SIPEED_META_SOURCE_FRAME_VALID;
    if (calibrated) meta->flags |= SIPEED_META_CALIBRATED;
    meta->frame_number = ++frame_counter;
    meta->source_frame_number = meta->frame_number;
    meta->timestamp_ms = to_ms_since_boot(get_absolute_time());
    meta->payload_bytes = THERMAL_FRAME_IMAGE_BYTES;
    meta->payload_crc32 = pixel_crc32(&frame[1], THERMAL_FRAME_IMAGE_BYTES);
    meta->width = THERMAL_FRAME_WIDTH;
    meta->height = THERMAL_FRAME_HEIGHT;
    meta->stride_bytes = THERMAL_FRAME_WIDTH;
    meta->pixel_format = SIPEED_THERMAL_PIXEL_TEMP8;
    meta->effective_bits = 8;
    meta->display_rotation = 1;
    meta->gain_mode = 0;
    meta->range_method = 3;
    meta->quant_max = 254;
    meta->temperature_scale = invalid;
    meta->temperature_offset = invalid;
    meta->center_temperature = invalid;
    meta->min_temperature = invalid;
    meta->max_temperature = invalid;
    meta->center_x = UINT16_MAX;
    meta->center_y = UINT16_MAX;
    meta->min_x = UINT16_MAX;
    meta->min_y = UINT16_MAX;
    meta->max_x = UINT16_MAX;
    meta->max_y = UINT16_MAX;
    meta->detector_temperature = invalid;
    meta->source_low = invalid;
    meta->source_high = invalid;
    meta->nuc_table_index = UINT16_MAX;
    meta->ffc_count = frame[THERMAL_FRAME_IMAGE_BYTES + 24];

    if (isfinite(temp_low) && isfinite(temp_high)
        && temp_high > temp_low) {
        meta->temperature_scale = (temp_high - temp_low) / 254.0f;
        meta->temperature_offset = temp_low;
        meta->source_low = temp_low;
        meta->source_high = temp_high;
        meta->flags |= SIPEED_META_TEMP_TRANSFORM_VALID
                       | SIPEED_META_SOURCE_RANGE_VALID;
    }
    if (center_x10 != INT16_MAX && min_x10 != INT16_MAX
        && max_x10 != INT16_MAX && min_x < THERMAL_FRAME_WIDTH
        && min_y < THERMAL_FRAME_HEIGHT && max_x < THERMAL_FRAME_WIDTH
        && max_y < THERMAL_FRAME_HEIGHT) {
        meta->center_temperature = center_x10 / 10.0f;
        meta->min_temperature = min_x10 / 10.0f;
        meta->max_temperature = max_x10 / 10.0f;
        meta->center_x = THERMAL_FRAME_WIDTH / 2u;
        meta->center_y = THERMAL_FRAME_HEIGHT / 2u;
        meta->min_x = min_x;
        meta->min_y = min_y;
        meta->max_x = max_x;
        meta->max_y = max_y;
        meta->flags |= SIPEED_META_HLC_TEMPERATURES_VALID
                       | SIPEED_META_HLC_COORDINATES_VALID;
    }
    if (sensor_vtemp > 0 && sensor_vtemp <= 16383u) {
        meta->detector_temperature =
            25.0f + ((int32_t)sensor_vtemp - 8192) / 70.0f;
        meta->flags |= SIPEED_META_DETECTOR_TEMPERATURE_VALID;
    }
    meta->meta_crc32 = pixel_crc32((const uint8_t*)meta,
                                   SIPEED_THERMAL_META_BYTES - 4u);
    __dmb();
    latest_meta_token = token;
}

void thermal_ncm_note_adc16(
    const uint8_t* adc16_pixels,
    const struct sipeed_thermal_meta_v1* source_meta)
{
    if (!adc16_pixels || !source_meta || source_meta->frame_number == 0) {
        return;
    }

    const float invalid = canonical_nan();
    memset(&adc16_meta, 0, sizeof(adc16_meta));
    memcpy(adc16_meta.magic, "SITHMETA", 8);
    adc16_meta.version_major = 1;
    adc16_meta.version_minor = 0;
    adc16_meta.meta_bytes = sizeof(adc16_meta);
    adc16_meta.flags = SIPEED_META_IMAGE_VALID
                       | SIPEED_META_PAYLOAD_CRC_VALID
                       | SIPEED_META_SOURCE_FRAME_VALID;
    adc16_meta.frame_number = source_meta->frame_number;
    adc16_meta.source_frame_number = source_meta->frame_number;
    adc16_meta.timestamp_ms = source_meta->timestamp_ms;
    adc16_meta.payload_bytes = THERMAL_FRAME_ADC16_BYTES;
    adc16_meta.payload_crc32 =
        pixel_crc32(adc16_pixels, THERMAL_FRAME_ADC16_BYTES);
    adc16_meta.width = THERMAL_FRAME_WIDTH;
    adc16_meta.height = THERMAL_FRAME_HEIGHT;
    adc16_meta.stride_bytes = THERMAL_FRAME_WIDTH * 2u;
    adc16_meta.pixel_format = SIPEED_THERMAL_PIXEL_ADC16;
    adc16_meta.effective_bits = 14;
    adc16_meta.display_rotation = source_meta->display_rotation;
    adc16_meta.gain_mode = source_meta->gain_mode;
    adc16_meta.range_method = 0;
    adc16_meta.quant_max = 0;
    adc16_meta.temperature_scale = invalid;
    adc16_meta.temperature_offset = invalid;
    adc16_meta.center_temperature = invalid;
    adc16_meta.min_temperature = invalid;
    adc16_meta.max_temperature = invalid;
    adc16_meta.center_x = UINT16_MAX;
    adc16_meta.center_y = UINT16_MAX;
    adc16_meta.min_x = UINT16_MAX;
    adc16_meta.min_y = UINT16_MAX;
    adc16_meta.max_x = UINT16_MAX;
    adc16_meta.max_y = UINT16_MAX;
    adc16_meta.detector_temperature = invalid;
    adc16_meta.source_low = invalid;
    adc16_meta.source_high = invalid;
    adc16_meta.nuc_table_index = UINT16_MAX;
    adc16_meta.ffc_count = source_meta->ffc_count;
    adc16_meta.ffc_state = source_meta->ffc_state;
    adc16_meta.shutter_state = source_meta->shutter_state;
    if (source_meta->flags & SIPEED_META_DETECTOR_TEMPERATURE_VALID) {
        adc16_meta.detector_temperature = source_meta->detector_temperature;
        adc16_meta.flags |= SIPEED_META_DETECTOR_TEMPERATURE_VALID;
    }
    adc16_meta.flags |= source_meta->flags
                        & (SIPEED_META_FFC_ACTIVE
                           | SIPEED_META_SHUTTER_CLOSED);
    adc16_meta.meta_crc32 = pixel_crc32(
        (const uint8_t*)&adc16_meta, SIPEED_THERMAL_META_BYTES - 4u);
    __dmb();
    adc16_generation++;
}

const struct sipeed_thermal_meta_v1* thermal_ncm_meta(uint8_t token)
{
    return token < 2 ? &frame_meta[token] : NULL;
}

const struct sipeed_thermal_meta_v1* thermal_ncm_adc16_meta(void)
{
    return adc16_meta.frame_number ? &adc16_meta : NULL;
}

void thermal_ncm_init(thermal_frame_acquire_fn acquire,
                      thermal_frame_release_fn release,
                      thermal_frame_acquire_fn adc16_acquire,
                      thermal_frame_release_fn adc16_release,
                      thermal_frame_cancel_fn adc16_cancel)
{
    frame_acquire = acquire;
    frame_release = release;
    adc16_frame_acquire = adc16_acquire;
    adc16_frame_release = adc16_release;
    adc16_frame_cancel = adc16_cancel;
    init_mac();
    network_load_settings();
    tusb_init();
    lwip_init();

    netif_data.hwaddr_len = sizeof(tud_network_mac_address);
    memcpy(netif_data.hwaddr, tud_network_mac_address,
           sizeof(tud_network_mac_address));
    netif_data.hwaddr[0] ^= 0x02;
    netif_add(&netif_data, &ipaddr, &netmask, &gateway, NULL, netif_init_cb,
              ethernet_input);
    netif_set_default(&netif_data);
    netif_set_up(&netif_data);
    netif_set_link_up(&netif_data);

    if (dhserv_init(&dhcp_config) != ERR_OK) {
        printf("NCM DHCP init failed\r\n");
    }
    if (dnserv_init(IP_ADDR_ANY, 53, captive_dns_query) != ERR_OK) {
        printf("NCM DNS init failed\r\n");
    }

    struct tcp_pcb* pcb = tcp_new_ip_type(IPADDR_TYPE_V4);
    if (pcb && tcp_bind(pcb, IP_ANY_TYPE, HTTP_PORT) == ERR_OK) {
        http_listener = tcp_listen_with_backlog(pcb, HTTP_CLIENT_COUNT);
        if (http_listener) tcp_accept(http_listener, http_accept);
    } else if (pcb) {
        tcp_close(pcb);
    }
    printf("NCM portal: http://192.168.%u.1/\r\n",
           (unsigned)network_segment);
}

void thermal_ncm_task(void)
{
    static uint32_t last_stream_frame_counter;
    static uint32_t last_adc16_generation;

    tud_task();
    if (received_frame) {
        struct pbuf* p = received_frame;
        received_frame = NULL;
        if (ethernet_input(p, &netif_data) != ERR_OK) pbuf_free(p);
        tud_network_recv_renew();
    }
    sys_check_timeouts();
    thermal_ota_task();
    if (network_reboot_at_ms
        && (int32_t)(to_ms_since_boot(get_absolute_time())
                     - network_reboot_at_ms) >= 0) {
        watchdog_reboot(0, 0, 10);
        while (true) __wfi();
    }

    /* A stream request remains open for its whole lifetime. Wake it only
     * when the producer publishes a new frame; tcp_sent() handles the rest
     * of a partially queued frame as host acknowledgements free sndbuf. */
    if (frame_counter != last_stream_frame_counter) {
        last_stream_frame_counter = frame_counter;
        for (size_t i = 0; i < HTTP_CLIENT_COUNT; i++) {
            struct http_client* client = &http_clients[i];
            if (client->pcb && client->request_done
                && client->response == RESPONSE_STREAM_TEMP8
                && client == stream_owner
                && !client->frame_held) {
                client_send(client);
            }
        }
    }
    if (adc16_generation != last_adc16_generation) {
        last_adc16_generation = adc16_generation;
        if (adc16_owner && adc16_owner->pcb
            && adc16_owner->request_done
            && adc16_owner->response == RESPONSE_FRAME_ADC16
            && !adc16_owner->frame_held) {
            client_send(adc16_owner);
        }
    }
}

static uint32_t pixel_crc32(const uint8_t* data, size_t len)
{
    static const uint32_t table[16] = {
        0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu,
        0x76DC4190u, 0x6B6B51F4u, 0x4DB26158u, 0x5005713Cu,
        0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu,
        0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu,
    };
    uint32_t crc = UINT32_MAX;
    while (len--) {
        crc ^= *data++;
        crc = (crc >> 4) ^ table[crc & 0x0Fu];
        crc = (crc >> 4) ^ table[crc & 0x0Fu];
    }
    return ~crc;
}
