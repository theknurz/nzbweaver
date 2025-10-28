#ifndef PARFILES_H
#define PARFILES_H
#include <stdbool.h>
#include <stdint.h>
#include "xmlhandler.h"

struct par_header {
    uint8_t     magic_sequence[8];
    uint64_t    packet_length;
    uint8_t     md5_packet[16];
    uint8_t     recv_set_id[16];
    uint8_t     type[16];
};

struct par_file {
    uint8_t     md5_hash_id[16];
    uint8_t     md5_hash_file[16];
    uint8_t     md5_hash_16k[16];
    uint64_t    file_length;
};

extern unsigned int par_filenames_length;
extern char **par_filenames;

bool get_par2_filenames(char* filename);
bool compare_par2_fields(uint8_t *parType, char *typeCheck, uint8_t len);
bool is_par2_list(char* filename);
#endif