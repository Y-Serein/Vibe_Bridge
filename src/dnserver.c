#include "dnserver.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "lwip/pbuf.h"
#include "lwip/udp.h"

#define DNS_PACKET_BYTES 512U

static struct udp_pcb *dns_pcb;
static dns_query_proc_t dns_query_proc;
static uint8_t dns_packet[DNS_PACKET_BYTES];

static uint16_t dns_read_u16(const uint8_t *data)
{
    return (uint16_t)(((uint16_t)data[0] << 8) | data[1]);
}

static void dns_write_u16(uint8_t *data, uint16_t value)
{
    data[0] = (uint8_t)(value >> 8);
    data[1] = (uint8_t)value;
}

static int dns_parse_question(const uint8_t *packet, size_t packet_length,
                              char *name, size_t name_capacity,
                              size_t *question_end, uint16_t *query_type,
                              uint16_t *query_class)
{
    size_t cursor = 12;
    size_t name_length = 0;

    if (packet_length < 17 || dns_read_u16(packet + 4) != 1) {
        return -1;
    }

    while (cursor < packet_length) {
        uint8_t label_length = packet[cursor++];

        if (label_length == 0) {
            break;
        }
        if ((label_length & 0xc0U) != 0 || label_length > 63 ||
            cursor + label_length > packet_length) {
            return -1;
        }
        if (name_length != 0) {
            if (name_length + 1 >= name_capacity) {
                return -1;
            }
            name[name_length++] = '.';
        }
        if (name_length + label_length >= name_capacity) {
            return -1;
        }
        for (uint8_t i = 0; i < label_length; i++) {
            char character = (char)packet[cursor++];
            if (character >= 'A' && character <= 'Z') {
                character = (char)(character - 'A' + 'a');
            }
            name[name_length++] = character;
        }
    }

    if (cursor + 4 > packet_length || name_length == 0) {
        return -1;
    }
    name[name_length] = '\0';
    *query_type = dns_read_u16(packet + cursor);
    *query_class = dns_read_u16(packet + cursor + 2);
    *question_end = cursor + 4;
    return 0;
}

static int dns_is_probe_domain(const char *name, ip4_addr_t *address)
{
    return dns_query_proc != NULL && dns_query_proc(name, address);
}

static size_t dns_make_response(uint8_t *packet, size_t packet_length,
                                size_t question_end, int is_probe,
                                uint16_t query_type, uint16_t query_class,
                                const ip4_addr_t *response_address)
{
    uint16_t request_flags = dns_read_u16(packet + 2);
    int answer_ipv4 = is_probe && query_class == 1 &&
                      (query_type == 1 || query_type == 255);
    int known_without_answer = is_probe && query_class == 1;
    uint16_t response_flags = (uint16_t)(0x8000U | (request_flags & 0x0100U));

    if (known_without_answer) {
        response_flags |= 0x0400U;
    } else {
        response_flags |= 0x0005U; /* REFUSED: use another interface/resolver. */
    }

    dns_write_u16(packet + 2, response_flags);
    dns_write_u16(packet + 4, 1);
    dns_write_u16(packet + 6, answer_ipv4 ? 1 : 0);
    dns_write_u16(packet + 8, 0);
    dns_write_u16(packet + 10, 0);

    if (!answer_ipv4) {
        return question_end;
    }
    if (question_end + 16 > DNS_PACKET_BYTES || question_end > packet_length) {
        return 0;
    }

    /* Compressed owner name, A/IN, 30 second TTL, portal IPv4 address. */
    packet[question_end + 0] = 0xc0;
    packet[question_end + 1] = 0x0c;
    packet[question_end + 2] = 0x00;
    packet[question_end + 3] = 0x01;
    packet[question_end + 4] = 0x00;
    packet[question_end + 5] = 0x01;
    packet[question_end + 6] = 0x00;
    packet[question_end + 7] = 0x00;
    packet[question_end + 8] = 0x00;
    packet[question_end + 9] = 0x1e;
    packet[question_end + 10] = 0x00;
    packet[question_end + 11] = 0x04;
    memcpy(packet + question_end + 12, &response_address->addr,
           sizeof(response_address->addr));
    return question_end + 16;
}

static void captive_dns_recv(void *arg, struct udp_pcb *upcb, struct pbuf *p,
                             const ip_addr_t *address, u16_t port)
{
    (void)arg;
    if (p == NULL) {
        return;
    }

    const size_t received = p->tot_len;
    if (received > sizeof(dns_packet)
        || pbuf_copy_partial(p, dns_packet, (u16_t)received, 0)
               != received) {
        pbuf_free(p);
        return;
    }
    pbuf_free(p);

    if (received < 12 || (dns_packet[2] & 0x80U) != 0) {
        return;
    }
    char name[128];
    size_t question_end;
    uint16_t query_type;
    uint16_t query_class;
    if (dns_parse_question(dns_packet, received, name, sizeof(name),
                           &question_end, &query_type, &query_class) != 0) {
        return;
    }

    ip4_addr_t response_address = {0};
    const int is_probe = dns_is_probe_domain(name, &response_address);
    const size_t response_length = dns_make_response(
        dns_packet, received, question_end, is_probe, query_type, query_class,
        &response_address);
    if (response_length == 0) {
        return;
    }

    struct pbuf *response = pbuf_alloc(PBUF_TRANSPORT, response_length,
                                       PBUF_RAM);
    if (response == NULL) {
        return;
    }
    if (pbuf_take(response, dns_packet, response_length) == ERR_OK) {
        udp_sendto(upcb, response, address, port);
    }
    pbuf_free(response);
}

err_t dnserv_init(const ip_addr_t *bind, uint16_t port,
                  dns_query_proc_t query_proc)
{
    dnserv_free();
    dns_pcb = udp_new();
    if (dns_pcb == NULL) {
        return ERR_MEM;
    }
    const err_t error = udp_bind(dns_pcb, bind, port);
    if (error != ERR_OK) {
        dnserv_free();
        return error;
    }
    dns_query_proc = query_proc;
    udp_recv(dns_pcb, captive_dns_recv, NULL);
    return ERR_OK;
}

void dnserv_free(void)
{
    if (dns_pcb != NULL) {
        udp_remove(dns_pcb);
        dns_pcb = NULL;
    }
    dns_query_proc = NULL;
}
