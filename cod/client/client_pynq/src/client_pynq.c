/*
 * Clientul TCP (lwIP) care trimite fisierele la Jetson si le primeste inapoi.
 *
 * In bare metal nu avem socket-uri obisnuite. lwIP lucreaza cu functii
 * "callback": ii spun ce functie sa apeleze cand s-a conectat, cand Jetson
 * a confirmat o parte din date si cand au venit date noi.
 *
 * Pasii:
 *   1. client_start(): pregatesc cele 2 fisiere si cer conexiunea
 *   2. on_connected(): incep sa trimit (textul, apoi imaginea)
 *   3. on_sent():      Jetson a confirmat o parte -> trimit mai departe
 *   4. on_recv():      vin fisierele noi -> le pun cap la cap
 *   5. Jetson inchide conexiunea cand a trimis tot -> gata
 */
#include <string.h>
#include <stdio.h>
#include "lwip/tcp.h"
#include "xil_printf.h"
#include "client_pynq.h"

#define RX_POOL_SIZE   (32u * 1024u * 1024u)//memorie pentru fisierele primite
#define TIMEOUT_TICKS  30//30 x 500 ms = 15 s fara activitate

static struct tcp_pcb *pcb;
static volatile ClientState state = CLIENT_IDLE;
static char    err_msg[120];
static Results results;
static int     idle_ticks;

//trimitere: 4 bucati, in ordine: header text, text, header imagine, imagine
static uint8_t  hdr_text[HEADER_SIZE], hdr_img[HEADER_SIZE];
static const uint8_t *chunk_ptr[4];
static uint32_t chunk_len[4];
static int      cur_chunk;
static uint32_t cur_off, total_to_send, total_acked;

//primire
static uint8_t  rx_pool[RX_POOL_SIZE] __attribute__((aligned(64)));
static uint32_t rx_used;
static uint8_t  rx_hdr[HEADER_SIZE];
static uint32_t rx_hdr_got;
static NetFile  rx_file;
static uint32_t rx_got;
static int      rx_in_data;

static int receive_bytes(const uint8_t *data, uint32_t len);

//header-ul unui fisier: tip, eticheta, lungime (big-endian)
static void pack_header(uint8_t *h, uint8_t type, uint8_t tag, uint32_t len)
{
    h[0] = type;
    h[1] = tag;
    h[2] = (uint8_t)(len >> 24);
    h[3] = (uint8_t)(len >> 16);
    h[4] = (uint8_t)(len >> 8);
    h[5] = (uint8_t)len;
}

static void set_error(const char *msg)
{
    if (state == CLIENT_ERROR)
        return;//pastrez prima eroare
    snprintf(err_msg, sizeof(err_msg), "%s", msg);
    state = CLIENT_ERROR;
}

//scot functiile mele de pe conexiune
static void detach(struct tcp_pcb *p)
{
    tcp_arg(p, NULL);
    tcp_err(p, NULL);
    tcp_recv(p, NULL);
    tcp_sent(p, NULL);
    tcp_poll(p, NULL, 0);
}

//inchid conexiunea brusc
static err_t abort_conn(void)
{
    if (pcb) {
        detach(pcb);
        tcp_abort(pcb);
        pcb = NULL;
    }
    return ERR_ABRT;//asa cere lwIP dupa tcp_abort
}

//inchid conexiunea normal
static err_t close_conn(void)
{
    struct tcp_pcb *p = pcb;
    pcb = NULL;
    detach(p);
    if (tcp_close(p) != ERR_OK) {
        tcp_abort(p);
        return ERR_ABRT;
    }
    return ERR_OK;
}

//pun in coada lwIP cat incape din fisiere, apoi trimit
static err_t send_more(struct tcp_pcb *tpcb)
{
    while (cur_chunk < 4) {
        uint32_t left = chunk_len[cur_chunk] - cur_off;
        if (left == 0) {
            cur_chunk++;
            cur_off = 0;
            continue;
        }
        uint32_t space = tcp_sndbuf(tpcb);//cat loc mai e in bufferul de trimitere
        if (space == 0)
            break;
        if (space > 0xFFFF)
            space = 0xFFFF;
        uint16_t n = (uint16_t)(left < space ? left : space);

        err_t e = tcp_write(tpcb, chunk_ptr[cur_chunk] + cur_off, n, TCP_WRITE_FLAG_COPY);
        if (e == ERR_MEM)
            break;//buffer plin, continui la urmatorul on_sent
        if (e != ERR_OK)
            return e;
        cur_off += n;
    }
    return tcp_output(tpcb);
}

static err_t on_sent(void *arg, struct tcp_pcb *tpcb, uint16_t len)
{
    (void)arg;
    idle_ticks = 0;
    total_acked += len;

    if (state == CLIENT_SENDING && total_acked >= total_to_send) {
        xil_printf("[PYNQ] SUCCES: ambele fisiere au fost trimise la Jetson\r\n");
        xil_printf("[PYNQ] Astept fisierele noi de la Jetson...\r\n");
        state = CLIENT_RECEIVING;
        return ERR_OK;
    }
    if (send_more(tpcb) != ERR_OK) {
        set_error("nu am putut trimite fisierele");
        return abort_conn();
    }
    return ERR_OK;
}

//lwIP o apeleaza la fiecare secunda, reiau trimiterea daca s-a oprit
static err_t on_poll(void *arg, struct tcp_pcb *tpcb)
{
    (void)arg;
    if (state == CLIENT_SENDING && send_more(tpcb) != ERR_OK) {
        set_error("nu am putut trimite fisierele");
        return abort_conn();
    }
    return ERR_OK;
}

static err_t on_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err)
{
    (void)arg;
    (void)err;

    if (p == NULL) {
        //Jetson a inchis conexiunea, asa imi spune ca a trimis toate fisierele
        if (state != CLIENT_RECEIVING || rx_in_data || rx_hdr_got > 0 || results.count == 0)
            set_error("Jetson a inchis conexiunea inainte de final (vezi consola Jetson)");
        else
            state = CLIENT_DONE;
        return close_conn();
    }

    idle_ticks = 0;
    for (struct pbuf *q = p; q != NULL; q = q->next) {
        if (receive_bytes((const uint8_t *)q->payload, q->len) < 0) {
            pbuf_free(p);
            return abort_conn();
        }
    }
    tcp_recved(tpcb, p->tot_len);//ii spun lui lwIP ca am preluat datele
    pbuf_free(p);
    return ERR_OK;
}

static void on_error(void *arg, err_t err)
{
    (void)arg;
    pcb = NULL;//lwIP a inchis deja conexiunea
    if (err == ERR_RST)
        set_error("Jetson a refuzat conexiunea (ruleaza ./server pe Jetson?)");
    else
        set_error("conexiunea cu Jetson s-a intrerupt");
}

static err_t on_connected(void *arg, struct tcp_pcb *tpcb, err_t err)
{
    (void)arg;
    (void)err;
    xil_printf("[PYNQ] Conectat la Jetson, trimit fisierele...\r\n");
    state = CLIENT_SENDING;
    idle_ticks = 0;
    tcp_nagle_disable(tpcb);//trimit imediat, fara sa astept sa se adune date

    if (send_more(tpcb) != ERR_OK) {
        set_error("nu am putut trimite fisierele");
        return abort_conn();
    }
    return ERR_OK;
}

//TCP livreaza datele in bucati oarecare, le pun cap la cap in fisiere
static int receive_bytes(const uint8_t *data, uint32_t len)
{
    while (len > 0) {
        if (!rx_in_data) {
            //strang cei 6 octeti de header
            uint32_t n = HEADER_SIZE - rx_hdr_got;
            if (n > len)
                n = len;
            memcpy(rx_hdr + rx_hdr_got, data, n);
            rx_hdr_got += n;
            data += n;
            len -= n;
            if (rx_hdr_got < HEADER_SIZE)
                break;

            rx_hdr_got   = 0;
            rx_file.type = rx_hdr[0];
            rx_file.tag  = rx_hdr[1];
            rx_file.len  = ((uint32_t)rx_hdr[2] << 24) | ((uint32_t)rx_hdr[3] << 16) | ((uint32_t)rx_hdr[4] << 8) | rx_hdr[5];
            if (rx_file.len > RX_POOL_SIZE - rx_used || results.count == MAX_RESULTS) {
                set_error("fisierele de la Jetson nu incap in memorie");
                return -1;
            }
            rx_file.data = rx_pool + rx_used;
            rx_used += (rx_file.len + 63u) & ~63u;//urmatorul fisier incepe aliniat
            rx_got = 0;
            rx_in_data = 1;
        }

        //strang continutul fisierului
        uint32_t n = rx_file.len - rx_got;
        if (n > len)
            n = len;
        memcpy(rx_file.data + rx_got, data, n);
        rx_got += n;
        data += n;
        len -= n;

        if (rx_got == rx_file.len) {
            //fisier complet
            rx_in_data = 0;
            results.files[results.count++] = rx_file;
            xil_printf("[PYNQ] Primit de la Jetson: %s (%d octeti)\r\n", rx_file.type == FILE_TEXT ? "fisierul text" : "o imagine", (int)rx_file.len);
        }
    }
    return 0;
}

int client_start(const uint8_t server_ip[4], uint16_t port, const uint8_t *text, uint32_t text_len, const uint8_t *image, uint32_t image_len)
{
    memset(&results, 0, sizeof(results));
    rx_used = rx_hdr_got = rx_got = 0;
    rx_in_data = 0;
    idle_ticks = 0;

    //cele 2 fisiere, fiecare cu header-ul lui in fata
    pack_header(hdr_text, FILE_TEXT, 0, text_len);
    pack_header(hdr_img, FILE_IMAGE, TAG_ORIGINAL, image_len);
    chunk_ptr[0] = hdr_text;
    chunk_len[0] = HEADER_SIZE;
    chunk_ptr[1] = text;
    chunk_len[1] = text_len;
    chunk_ptr[2] = hdr_img;
    chunk_len[2] = HEADER_SIZE;
    chunk_ptr[3] = image;
    chunk_len[3] = image_len;
    cur_chunk = 0;
    cur_off = 0;
    total_to_send = 2 * HEADER_SIZE + text_len + image_len;
    total_acked = 0;

    //ii spun lui lwIP ce functii sa apeleze
    pcb = tcp_new();
    tcp_err(pcb, on_error);
    tcp_recv(pcb, on_recv);
    tcp_sent(pcb, on_sent);
    tcp_poll(pcb, on_poll, 2);//la fiecare 2 x 500 ms

    ip_addr_t addr;
    IP_ADDR4(&addr, server_ip[0], server_ip[1], server_ip[2], server_ip[3]);
    xil_printf("[PYNQ] Ma conectez la Jetson %d.%d.%d.%d ...\r\n",
               server_ip[0], server_ip[1], server_ip[2], server_ip[3]);
    state = CLIENT_CONNECTING;
    if (tcp_connect(pcb, &addr, port, on_connected) != ERR_OK)
        return -1;
    return 0;
}

void client_tick(void)
{
    if (state != CLIENT_CONNECTING && state != CLIENT_SENDING && state != CLIENT_RECEIVING)
        return;
    if (++idle_ticks < TIMEOUT_TICKS)
        return;

    //15 secunde fara nicio activitate
    int connecting = (state == CLIENT_CONNECTING);
    abort_conn();
    if (connecting)
        set_error("Jetson nu raspunde (verifica cablul, IP-ul si ca ruleaza ./server)");
    else
        set_error("Jetson nu mai trimite nimic (timeout)");
}

ClientState client_state(void)
{
    return state;
}

const char *client_error(void)
{
    return err_msg;
}

const Results *client_results(void)
{
    return &results;
}
