/*
 * Citire si scriere de fisiere pe cardul SD (bare metal, biblioteca xilffs).
 *
 * Bufferele date acestor functii trebuie sa fie aliniate la 64 de octeti
 * (__attribute__((aligned(64)))), pentru ca controlerul SD copiaza datele
 * direct in memorie.
 */
#ifndef SD_CARD_H
#define SD_CARD_H

#include <stdint.h>

int  sd_mount(void);
void sd_unmount(void);

//citeste tot fisierul in buf (cel mult max_len octeti). 0 = ok, -1 = eroare
int sd_read_file(const char *name, uint8_t *buf, uint32_t max_len, uint32_t *out_len);

//scrie buf intr-un fisier (il suprascrie daca exista). 0 = ok, -1 = eroare
int sd_write_file(const char *name, const uint8_t *buf, uint32_t len);

#endif
