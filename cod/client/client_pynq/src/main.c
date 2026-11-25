/*
 * Client bare metal pe PYNQ-Z1.
 *
 *   1. porneste Ethernet-ul cu IP fix 192.168.0.10
 *   2. citeste de pe card text.txt si imagine.bmp
 *   3. le trimite la Jetson (192.168.0.3, portul 8080)
 *   4. primeste fisierele noi si le scrie pe card: r_text.txt, r_orig.bmp
 *      (mai tarziu r_gray.bmp, r_gauss.bmp, r_sobel.bmp, r_binar.bmp)
 *   5. afiseaza in HTerm (115200 baud) ce s-a intamplat
 *
 * Inlocuieste main.c din exemplul "lwIP Echo Server". Fisierele platform*
 * din exemplu raman (pornesc timerul si intreruperile pentru lwIP).
 */
#include "xparameters.h"
#include "netif/xadapter.h"
#include "platform.h"
#include "lwip/etharp.h"
#include "platform_config.h"
#include "xil_printf.h"
#include "lwip/init.h"
#include "lwip/tcp.h"
#include "sd_card.h"
#include "client_pynq.h"

#define JETSON_PORT  8080
static const uint8_t JETSON_IP[4] = { 192, 168, 0, 3 };

//variabile cerute de fisierele platform* din exemplul lwIP
extern volatile int TcpFastTmrFlag;
extern volatile int TcpSlowTmrFlag;
static struct netif server_netif;
struct netif *echo_netif;
void tcp_fasttmr(void);
void tcp_slowtmr(void);

//bufferele pentru fisierele citite de pe card (aliniate, cerinta controlerului SD)
static uint8_t text_buf[1024 * 1024]      __attribute__((aligned(64)));
static uint8_t image_buf[8 * 1024 * 1024] __attribute__((aligned(64)));

//un pas lwIP: ceasurile TCP si ARP + pachetele venite pe Ethernet
static void lwip_step(void)
{
    static int slow_count = 0;//numar ticurile de 500 ms ca sa stiu cand a trecut o secunda
    if (TcpFastTmrFlag) {
        tcp_fasttmr();
        TcpFastTmrFlag = 0;
    }
    if (TcpSlowTmrFlag) {
        tcp_slowtmr();
        TcpSlowTmrFlag = 0;
        client_tick();//verific timeout-ul la fiecare 500 ms
        slow_count++;
        if (slow_count == 2) {
            etharp_tmr();//o data pe secunda: repet intrebarea ARP daca Jetson nu a raspuns inca
            slow_count = 0;
        }
    }
    xemacif_input(echo_netif);
}

//numele de pe card pentru fiecare fisier primit
static const char *result_name(const NetFile *f)
{
    if (f->type == FILE_TEXT)
        return "0:/r_text.txt";
    switch (f->tag) {
    case TAG_GRAYSCALE:
        return "0:/r_gray.bmp";
    case TAG_GAUSSIAN:
        return "0:/r_gauss.bmp";
    case TAG_SOBEL:
        return "0:/r_sobel.bmp";
    case TAG_BINARY:
        return "0:/r_binar.bmp";
    default:
        return "0:/r_orig.bmp";
    }
}

int main(void)
{
    ip_addr_t ipaddr, netmask, gw;
    unsigned char mac[] = { 0x00, 0x0a, 0x35, 0x00, 0x01, 0x02 };
    uint32_t text_len, image_len;
    const Results *res;
    int ok = 0;

    echo_netif = &server_netif;//inainte de init_platform, timerul il foloseste
    init_platform();
    xil_printf("\r\n Proiect P7: PYNQ -> Jetson \r\n");

    //pornesc Ethernet-ul
    IP4_ADDR(&ipaddr,  192, 168, 0, 10);
    IP4_ADDR(&netmask, 255, 255, 255, 0);
    IP4_ADDR(&gw,      192, 168, 0, 1);
    lwip_init();
    xemac_add(echo_netif, &ipaddr, &netmask, &gw, mac, PLATFORM_EMAC_BASEADDR);
    netif_set_default(echo_netif);
#ifndef SDT
    platform_enable_interrupts();
#endif
    netif_set_up(echo_netif);
    xil_printf("[RETEA] Ethernet pornit, IP 192.168.0.10\r\n");

    //citesc cele 2 fisiere de pe card
    if (sd_mount() != 0 || sd_read_file("0:/text.txt", text_buf, sizeof(text_buf), &text_len) != 0 || sd_read_file("0:/imagine.bmp", image_buf, sizeof(image_buf), &image_len) != 0)
        goto end;
    xil_printf("[SD] SUCCES: am citit text.txt (%d octeti) si imagine.bmp (%d octeti)\r\n", (int)text_len, (int)image_len);

    //le trimit la Jetson si astept fisierele noi
    if (client_start(JETSON_IP, JETSON_PORT, text_buf, text_len, image_buf, image_len) != 0) {
        xil_printf("[PYNQ] EROARE: nu pot porni conexiunea\r\n");
        goto end;
    }
    while (client_state() != CLIENT_DONE && client_state() != CLIENT_ERROR)
        lwip_step();

    if (client_state() == CLIENT_ERROR) {
        xil_printf("[PYNQ] EROARE: %s\r\n", client_error());
        goto end;
    }
    res = client_results();
    xil_printf("[PYNQ] SUCCES: am primit %d fisiere noi de la Jetson\r\n", res->count);

    //scriu fisierele noi pe card
    ok = 1;
    for (int i = 0; i < res->count; i++) {
        const char *name = result_name(&res->files[i]);
        if (sd_write_file(name, res->files[i].data, res->files[i].len) == 0)
            xil_printf("[SD] Salvat pe card: %s\r\n", name + 3);//name + 3 sare peste "0:/"
        else
            ok = 0;
    }
    if (ok)
        xil_printf("[PYNQ] SUCCES: fisierele noi au fost salvate pe cardul SD\r\n");

end:
    sd_unmount();

    //las lwIP ~2 secunde sa inchida curat conexiunea
    for (int ticks = 0; ticks < 4; ) {
        if (TcpSlowTmrFlag)
            ticks++;
        lwip_step();
    }
    xil_printf(" %s \r\n", ok ? "GATA: totul a mers" : "GATA: cu erori");
    cleanup_platform();
    return 0;
}