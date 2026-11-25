/*
 * Primeste de la PYNQ un fisier text si o imagine BMP, le salveaza in OUTPUT_DIR
 * si trimite rezultatele inapoi.
 *
 * Fisiere create in OUTPUT_DIR:
 *   original_text.txt      textul primit de la PYNQ
 *   original_imagine.bmp   imaginea primita de la PYNQ
 *
 * Etapa curenta: nu exista inca procesare. Ca sa se poata testa si drumul
 * inapoi, serverul trimite la PYNQ copii ale originalelor. Am lasat un comentariu
 * in locul unde trebuie adaugata procesarea.
 *
 * Compilare:  gcc -O2 -Wall -o server jetson_server.c
 * Rulare:     ./server            (port 8080)
 *
 * Codul este scris ca sa fie si C++ valid, ca sa poata fi redenumit .cu
 * si compilat cu nvcc pentru adaugarea kernelurilor CUDA.
 *
 * PROTOCOL
 *
 * Pe retea circula doar fisiere, octet cu octet, exact cum sunt pe card.
 * Fiecare fisier = header de 6 octeti + continutul fisierului.
 *
 *   tip       1 octet   1 = TEXT, 2 = IMAGINE (BMP)
 *   eticheta  1 octet   la imagini: 0 = original, 1 = grayscale, 2 = gaussian,
 *                                   3 = sobel, 4 = binarizare
 *   lungime   4 octeti  cati octeti are fisierul (big-endian)
 *
 *   PYNQ   -> Jetson : fisierul text + imaginea (Jetson asteapta exact 2 fisiere)
 *   Jetson -> PYNQ   : fisierul text + imaginile rezultate, apoi inchide conexiunea, ca PYNQ sa stie ca a primit tot
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>

#define PORT          8080
#define HEADER_SIZE   6
#define MAX_RESULTS   8

//folderul in care salvez fisierele originale si rezultatele
#define OUTPUT_DIR  "/home/utilizator/proiect_p7"//de aflat path-ul cu pwd

enum { FILE_TEXT = 1, FILE_IMAGE = 2 };
enum { TAG_ORIGINAL = 0, TAG_GRAYSCALE = 1, TAG_GAUSSIAN = 2, TAG_SOBEL = 3, TAG_BINARY = 4 };

//un fisier primit sau de trimis, impreuna cu datele din header-ul lui
typedef struct {
    uint8_t  type;
    uint8_t  tag;
    uint32_t len;
    uint8_t *data;
} NetFile;

static int send_all(int fd, const void *buf, size_t len)
{
    const uint8_t *p = (const uint8_t *)buf;
    while (len > 0) {
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if (n <= 0)
            return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static int recv_all(int fd, void *buf, size_t len)
{
    uint8_t *p = (uint8_t *)buf;
    while (len > 0) {
        ssize_t n = recv(fd, p, len, 0);
        if (n <= 0)
            return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

//trimite header-ul (tip, eticheta, lungime) si apoi continutul fisierului
static int send_file(int fd, const NetFile *f)
{
    uint8_t hdr[HEADER_SIZE];
    uint32_t n = htonl(f->len);
    hdr[0] = f->type;
    hdr[1] = f->tag;
    memcpy(hdr + 2, &n, 4);
    if (send_all(fd, hdr, HEADER_SIZE) < 0)
        return -1;
    return send_all(fd, f->data, f->len);
}

//primeste header-ul, apoi exact atatia octeti cati scrie in el
static int recv_file(int fd, NetFile *f)
{
    uint8_t hdr[HEADER_SIZE];
    uint32_t n;
    if (recv_all(fd, hdr, HEADER_SIZE) < 0)
        return -1;
    f->type = hdr[0];
    f->tag  = hdr[1];
    memcpy(&n, hdr + 2, 4);
    f->len  = ntohl(n);
    f->data = (uint8_t *)malloc(f->len > 0 ? f->len : 1);
    if (recv_all(fd, f->data, f->len) < 0) {
        free(f->data);
        f->data = NULL;
        return -1;
    }
    return 0;
}

/*
    astept ca PYNQ sa inchida conexiunea dupa ce a primit tot,
    deci asa stiu sigur ca fisierele au ajuns. 0 = ok, -1 = nu
*/
static int wait_pynq_done(int fd)
{
    struct timeval tv;
    tv.tv_sec  = 15;//astept maxim 15 secunde ca PYNQ sa inchida conexiunea
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    shutdown(fd, SHUT_WR);//ii spun lui PYNQ ca nu mai trimit nimic

    uint8_t tmp[64];
    if (recv(fd, tmp, sizeof(tmp), 0) == 0)
        return 0;//am primit tot pe PYNQ
    return -1;//timeout sau alta eroare
}

static void save_file(const char *name, const uint8_t *data, uint32_t len)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", OUTPUT_DIR, name);
    FILE *fp = fopen(path, "wb");
    if (!fp) {
        fprintf(stderr, "[Jetson] EROARE: nu pot salva %s\n", path);
        return;
    }
    fwrite(data, 1, len, fp);
    fclose(fp);
    printf("[Jetson] Salvat: %s\n", path);
}

//citire / scriere numere little-endian, asa cum sunt in header-ul BMP
static uint32_t rd32(const uint8_t *p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}
static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}
static void wr16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

/*
    scoate pixelii dintr-un BMP color pe 24 de biti, ca sa aplic filtrele pe ei
    pixelii ies RGB, randuri de sus in jos
    intoarce NULL daca nu e BMP pe 24 de biti
*/
static uint8_t *bmp_to_rgb(const uint8_t *bmp, uint32_t len, uint32_t *out_w, uint32_t *out_h)
{
    if (len < 54 || bmp[0] != 'B' || bmp[1] != 'M' || rd16(bmp + 28) != 24)
        return NULL;

    uint32_t offset = rd32(bmp + 10);//unde incep pixelii in fisier
    uint32_t w      = rd32(bmp + 18);
    int32_t  h      = (int32_t)rd32(bmp + 22);
    int bottom_up   = (h > 0);//de obicei BMP tine randurile de jos in sus
    if (h < 0)
        h = -h;
    uint32_t row    = (w * 3 + 3) & ~3u;//fiecare rand e completat la multiplu de 4

    uint8_t *rgb = (uint8_t *)malloc((size_t)w * h * 3);
    for (int32_t r = 0; r < h; r++) {
        const uint8_t *src = bmp + offset + (size_t)r * row;
        uint8_t *dst = rgb + (size_t)(bottom_up ? h - 1 - r : r) * w * 3;
        for (uint32_t x = 0; x < w; x++) {
            //BGR -> RGB
            dst[3 * x]     = src[3 * x + 2];
            dst[3 * x + 1] = src[3 * x + 1];
            dst[3 * x + 2] = src[3 * x];
        }
    }
    *out_w = w;
    *out_h = (uint32_t)h;
    return rgb;
}

/*
    transforma pixelii intr-un fisier BMP, ca sa il pot salva si trimite
    px: randuri de sus in jos
    ch = 3 (RGB) sau 1 (gri)
*/
static uint8_t *pixels_to_bmp(const uint8_t *px, uint32_t w, uint32_t h, int ch, uint32_t *out_len)
{
    uint32_t row     = (w * ch + 3) & ~3u;//fiecare rand e completat la multiplu de 4
    uint32_t palette = (ch == 1) ? 1024 : 0;//imaginile gri au o paleta de 256 de culori
    uint32_t offset  = 54 + palette;
    uint32_t size    = offset + row * h;

    uint8_t *bmp = (uint8_t *)calloc(size, 1);
    bmp[0] = 'B';
    bmp[1] = 'M';
    wr32(bmp + 2,  size);
    wr32(bmp + 10, offset);
    wr32(bmp + 14, 40);
    wr32(bmp + 18, w);
    wr32(bmp + 22, h);
    wr16(bmp + 26, 1);
    wr16(bmp + 28, (uint16_t)(ch * 8));
    wr32(bmp + 34, row * h);
    if (palette) {
        wr32(bmp + 46, 256);
        for (int i = 0; i < 256; i++)
            bmp[54 + 4 * i] = bmp[55 + 4 * i] = bmp[56 + 4 * i] = (uint8_t)i;
    }
    for (uint32_t r = 0; r < h; r++) {
        const uint8_t *src = px + (size_t)(h - 1 - r) * w * ch;//BMP: ultimul rand primul
        uint8_t *dst = bmp + offset + (size_t)r * row;
        for (uint32_t x = 0; x < w; x++) {
            if (ch == 3) {
                //RGB -> BGR
                dst[3 * x]     = src[3 * x + 2];
                dst[3 * x + 1] = src[3 * x + 1];
                dst[3 * x + 2] = src[3 * x];
            } else {
                dst[x] = src[x];
            }
        }
    }
    *out_len = size;
    return bmp;
}

//adauga un fisier in lista celor de trimis inapoi la PYNQ
static void add_result(NetFile *out, int *n, uint8_t type, uint8_t tag, uint8_t *data, uint32_t len)
{
    out[*n].type = type;
    out[*n].tag  = tag;
    out[*n].data = data;
    out[*n].len  = len;
    (*n)++;
}

static void handle_client(int fd)
{
    NetFile text, image, in, out[MAX_RESULTS];
    int n_out = 0;
    memset(&text, 0, sizeof(text));
    memset(&image, 0, sizeof(image));

    //primesc fisiere pana am si textul si imaginea
    while (!text.data || !image.data) {
        if (recv_file(fd, &in) < 0) {
            fprintf(stderr, "[Jetson] EROARE: nu am primit ambele fisiere de la PYNQ\n");
            free(text.data);
            free(image.data);
            return;
        }
        if (in.type == FILE_TEXT) {
            text = in;
            printf("[Jetson] Primit fisierul text (%u octeti)\n", in.len);
        } else {
            image = in;
            printf("[Jetson] Primita imaginea BMP (%u octeti)\n", in.len);
        }
    }
    printf("[Jetson] SUCCES: am primit ambele fisiere de la PYNQ\n");

    //salvam originalele in OUTPUT_DIR
    save_file("original_text.txt", text.data, text.len);
    save_file("original_imagine.bmp", image.data, image.len);

    /*
        LOC PROCESARE: aici se adauga procesarea textului si a imaginii
        textul: transform literele mici in litere mari
        imaginea: o transform in pixeli cu bmp_to_rgb(), aplic filtrele, din fiecare
        rezultat fac un BMP cu pixels_to_bmp(), il salvez cu save_file("rezultat_....bmp", ...)
        si il adaug cu add_result() cu eticheta potrivita (TAG_GRAYSCALE) ca sa fie trimis inapoi la PYNQ.
        momentan trimit copii ale originalelor ca sa testez drumul intors
    */
    uint8_t *text_out = (uint8_t *)malloc(text.len > 0 ? text.len : 1);
    memcpy(text_out, text.data, text.len);
    add_result(out, &n_out, FILE_TEXT, 0, text_out, text.len);

    uint32_t w, h;
    uint8_t *rgb = bmp_to_rgb(image.data, image.len, &w, &h);
    if (rgb) {
        uint32_t bmp_len;
        uint8_t *bmp = pixels_to_bmp(rgb, w, h, 3, &bmp_len);
        add_result(out, &n_out, FILE_IMAGE, TAG_ORIGINAL, bmp, bmp_len);
        free(rgb);
    } else {
        fprintf(stderr, "[Jetson] EROARE: imaginea nu este BMP color pe 24 de biti\n");
    }

    //trimitem inapoi fisierele rezultate la PYNQ
    printf("[Jetson] Trimit fisierele inapoi la PYNQ...\n");
    int ok = 1;
    for (int i = 0; i < n_out && ok; i++) {
        if (send_file(fd, &out[i]) < 0)
            ok = 0;
    }
    if (ok && wait_pynq_done(fd) == 0)
        printf("[Jetson] SUCCES: fisierele au fost trimise inapoi la PYNQ (%d fisiere)\n", n_out);
    else
        fprintf(stderr, "[Jetson] EROARE: fisierele nu au ajuns inapoi la PYNQ\n");

    free(text.data);
    free(image.data);
    for (int i = 0; i < n_out; i++)
        free(out[i].data);
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);//mesajele apar imediat in consola

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(PORT);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(server_fd, 1) < 0) {
        perror("[Jetson] EROARE la pornirea serverului");
        return 1;
    }
    printf("[Jetson] Server pornit pe portul %d\n", PORT);
    printf("[Jetson] Fisierele se salveaza in: %s\n", OUTPUT_DIR);

    for (;;) {
        int fd = accept(server_fd, NULL, NULL);
        if (fd < 0)
            continue;

        //trimitem headerele mici imediat(low latency)
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

        printf("\n[Jetson] Client conectat\n");
        handle_client(fd);
        close(fd);
        printf("[Jetson] Client deconectat\n");
    }
}
