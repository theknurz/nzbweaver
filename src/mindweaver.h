#ifndef MINDWEAVER_H
#define MINDWEAVER_H
#include <stdio.h>
#include <stdlib.h>
#include <pwd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <rapidyenc.h>
#include <locale.h>

#include "parfiles.h"
#include "nntp.h"
#include "xmlhandler.h"

struct thread_user_data {
    char *filename, *dbg_article;
    uint64_t *filesize;
    uint64_t *downloaded;
    struct nntp_server *connection;
};

extern unsigned int mw_max_threads;
extern bool mw_draw_no_gui;

bool mw_connect(char *address, uint16_t port, char *username, char *password, bool useSSL, unsigned int threads);
void mw_loop(void);
void mw_quit_and_clean(void);
bool mw_prepare_directories(void);
void mw_join_segments(struct NZBFile *curFile);
void mw_logMessage(char* file, int line, char *fmt, ...);
void mw_SIGUSR(int sig);
void mw_print_overview(void);
char* mw_val_to_hr_string(uint64_t valIn);
void mw_post_rename(void);
void mw_post_checkrepair(void);
bool mw_parse_yenc_header (char *yEncStart, struct NZBFile *curFile, uint64_t **filesize);
bool mw_prepare_download(void);
bool mw_get_binary_from_article(struct thread_user_data *userData, struct NZBFile *curFile, struct NZBSegment *curSeg, char **buffer);
char* mw_set_filename(struct NZBFile* curFile);
#endif 