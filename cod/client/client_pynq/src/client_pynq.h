/*
 * Clientul TCP (lwIP) care trimite fisierele la Jetson si le primeste inapoi.
 *
 * Pe retea circula doar fisiere. Fiecare fisier = header de 6 octeti
 * (tip, eticheta, lungime pe 4 octeti big-endian) + continutul fisierului.
 */
#ifndef CLIENT_PYNQ_H
#define CLIENT_PYNQ_H

#include <stdint.h>

//tipuri de fisiere
#define FILE_TEXT   1
#define FILE_IMAGE  2

//etichete pentru imagini: ce rezultat e
#define TAG_ORIGINAL   0
#define TAG_GRAYSCALE  1
#define TAG_GAUSSIAN   2
#define TAG_SOBEL      3
#define TAG_BINARY     4

#define HEADER_SIZE  6
#define MAX_RESULTS  8

//un fisier primit sau de trimis, impreuna cu datele din header-ul lui
typedef struct {
    uint8_t  type;
    uint8_t  tag;
    uint32_t len;
    uint8_t *data;
} NetFile;

//fisierele primite de la Jetson
typedef struct {
    NetFile files[MAX_RESULTS];
    int     count;
} Results;

//in ce etapa e clientul
typedef enum {
    CLIENT_IDLE,
    CLIENT_CONNECTING,
    CLIENT_SENDING,
    CLIENT_RECEIVING,
    CLIENT_DONE,//totul a mers, fisierele sunt in client_results()
    CLIENT_ERROR//motivul este in client_error()
} ClientState;

//porneste conexiunea si trimiterea celor 2 fisiere
int client_start(const uint8_t server_ip[4], uint16_t port,
                 const uint8_t *text, uint32_t text_len,
                 const uint8_t *image, uint32_t image_len);

//de apelat la fiecare 500 ms, pentru timeout
void client_tick(void);

ClientState    client_state(void);
const char    *client_error(void);
const Results *client_results(void);

#endif
