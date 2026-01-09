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
#include <dirent.h>

#include "parfiles.h"
#include "nntp.h"
#include "xmlhandler.h"

struct thread_user_data {
    char *filename, *dbg_article;
    uint64_t *filesize;
    uint64_t *downloaded;
    struct nntp_server *connection;
};

enum {
    RENAME_GUESS = 0,
    RENAME_FORCE_NZB = 1,
    RENAME_FORCE_YENC = 2
};

extern unsigned int mw_max_threads;
extern bool mw_draw_no_gui;
extern bool mw_remove_nzb_after_unpack;
extern bool mw_volpar_after_incomplete;
extern int  mw_force_rename;

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
bool mw_post_checkrepair(char *parfile);
void mw_unrar(void);
unsigned int mw_get_rar_volumes(char *firstrarvol, char ***rar_volumes);
bool mw_parse_yenc_header (char *yEncStart, struct NZBFile *curFile, uint64_t **filesize);
bool mw_get_binary_from_article(struct thread_user_data *userData, struct NZBFile *curFile, struct NZBSegment *curSeg, char **buffer);
char* mw_rar_password_provided(void);
bool mw_fetch_recovery_volumes(void);
#endif 