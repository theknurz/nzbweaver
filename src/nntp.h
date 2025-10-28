#ifndef NNTP_H
#define NNTP_H
#include <stdbool.h>
#include <stdint.h>
#include <openssl/ssl.h>
#include <openssl/err.h>

#ifndef NDEBUG
#define dbgprint(...) fprintf(stderr, __VA_ARGS__);
#endif

enum NNTP_STATE {
    NNTP_WELCOME = 0,
    NNTP_USERNAME = 1,
    NNTP_PASSWORD,
    NNTP_FETCH,
    NNTP_BODY,
    NNTP_IDLE = 255
};

struct nntp_query {
    int status, state;  // status = replied code from server, state = the state we're in RIGHT NOW.
    char *request, *reply; // request as text, reply as text
    bool expect_multiline; // if the reply is a multiline-reply
};

struct nntp_map {
    int state;
    char* fmt;
    bool multiline;
};

struct nntp_message_header {
    char    *name;
    char    **values;
    uint8_t valueSize;
};

struct nntp_server {
    char        *address;
    uint16_t    port;
    bool        work_done;
    int         fd_socket;
    bool        use_ssl;
    char        *username, *password;
    SSL         *ssl_fd_socket;
    uint16_t    connectionID;
    int         nntp_server_state;
};

bool nntp_connect(struct nntp_server *connection);
bool nntp_is_reply_done(char *reply, bool is_multiline);
uint64_t nntp_read(struct nntp_server *connection, char **reply, bool *isOk);
bool nntp_write(struct nntp_server *connection, int req_nntpstate, ...);
bool nntp_get_article(struct nntp_server *connection, char *aID, char **buffer);
int nntp_get_code_from_string(char *buffer, bool *isOk);
bool nntp_authenticate(struct nntp_server *connection, char *username, char *password);
// bool nntp_get_binary_from_article (char *bufferIn, char **binaryOut, size_t *outSize, int *crcOK);
struct pair *nntp_get_yenc_meta (char *yencLine);
bool nntp_get_yenc_header_begin_end(char *encoded_buffer, char **yenc_data_begin, char **yenc_data_end);
size_t nntp_decode_yenc (char *encoded_header_buffer, char **outBinary, int *isCRCOk);
#endif
