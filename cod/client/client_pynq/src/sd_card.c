/*
 * Citire si scriere de fisiere pe cardul SD (bare metal, biblioteca xilffs).
 *
 * Aceleasi functii ca in Laboratorul 8 (f_mount, f_open, f_read, f_write),
 * doar ca fisierul se citeste / se scrie dintr-o singura bucata.
 * Numele fisierelor: cel mult 8 caractere + extensia (de ex. imagine.bmp).
 */
#include "ff.h"
#include "xil_printf.h"
#include "sd_card.h"

static FATFS fatfs;
static FIL   file;

int sd_mount(void)
{
    if (f_mount(&fatfs, "0:/", 1) != FR_OK) {
        xil_printf("[SD] EROARE: nu pot monta cardul (e introdus? e FAT32?)\r\n");
        return -1;
    }
    xil_printf("[SD] Card montat\r\n");
    return 0;
}

void sd_unmount(void)
{
    f_mount(NULL, "0:/", 1);
}

int sd_read_file(const char *name, uint8_t *buf, uint32_t max_len, uint32_t *out_len)
{
    UINT br;
    if (f_open(&file, name, FA_READ) != FR_OK) {
        xil_printf("[SD] EROARE: nu gasesc %s pe card\r\n", name);
        return -1;
    }
    uint32_t size = (uint32_t)f_size(&file);
    if (size > max_len) {
        //nu incape in buffer
        xil_printf("[SD] EROARE: %s e prea mare\r\n", name);
        f_close(&file);
        return -1;
    }
    FRESULT r = f_read(&file, buf, size, &br);
    f_close(&file);
    if (r != FR_OK || br != size) {
        xil_printf("[SD] EROARE la citirea %s\r\n", name);
        return -1;
    }
    *out_len = size;
    return 0;
}

int sd_write_file(const char *name, const uint8_t *buf, uint32_t len)
{
    UINT bw;
    if (f_open(&file, name, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) {
        xil_printf("[SD] EROARE: nu pot crea %s\r\n", name);
        return -1;
    }
    FRESULT r = f_write(&file, buf, len, &bw);
    f_close(&file);//abia acum datele sunt scrise efectiv pe card
    if (r != FR_OK || bw != len) {
        xil_printf("[SD] EROARE la scrierea %s\r\n", name);
        return -1;
    }
    return 0;
}
