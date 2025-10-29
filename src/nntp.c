#include <string.h>
#include <pcre2.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <ctype.h>
#include <sys/socket.h>
#include <unistd.h>

#include <rapidyenc.h>

#include "xmlhandler.h"
#include "nntp.h"

// settings:
const unsigned int  NNTP_READBUFSIZE = 1024;

// non exported vars:
struct sockaddr_in nntp_server_adress;
bool isResolved = false;

// a nntp-"map":
struct nntp_map nntp_query_state[] = {
    { .state = NNTP_WELCOME, .fmt = NULL, .multiline = false },
    { .state = NNTP_USERNAME, .fmt = "AUTHINFO USER %s\r\n", .multiline = false },
    { .state = NNTP_PASSWORD, .fmt = "AUTHINFO PASS %s\r\n", .multiline = false },
    { .state = NNTP_FETCH, .fmt = "ARTICLE <%s>\r\n", .multiline = true },
    { .state = NNTP_BODY, .fmt = "BODY <%s>\r\n", .multiline = true }
};

/// @brief connects to adress:port, remembers SSL for later TLS() usage ?
/// @param adress FQDN/IP
/// @param port 1-65535
/// @param ssl true/false
/// @return true if connection is okay, false ... 
bool nntp_connect(struct nntp_server *connection) {
    char cport[7] = { 0 };
    struct addrinfo *addrInfos = NULL;
    struct addrinfo hintResolv;
    int resolvOk;
    bool isOk;
    extern SSL_CTX* ssl_ctx;

    snprintf(&cport[0], 6, "%u", connection->port);
    memset(&hintResolv, 0, sizeof(struct addrinfo));
    hintResolv.ai_family = AF_UNSPEC;
    hintResolv.ai_socktype = SOCK_STREAM;
    hintResolv.ai_flags = AI_NUMERICSERV;
    hintResolv.ai_protocol = 0;

    resolvOk = getaddrinfo(connection->address, cport, &hintResolv, &addrInfos);
    if (resolvOk != 0) {
        fprintf (stderr, "Error while resolving: %s\n", gai_strerror(resolvOk));
        return false;
    }

    // if the first entry is already null, this is'nt going to get anywhere..
    if (addrInfos != NULL) {
        do {
            connection->fd_socket = socket(addrInfos->ai_family, addrInfos->ai_socktype, addrInfos->ai_protocol);
            if (connection->fd_socket == -1)
                continue;
            if (connection->use_ssl) {
                connection->ssl_fd_socket=SSL_new(ssl_ctx);
                SSL_set_fd(connection->ssl_fd_socket, connection->fd_socket);
            } 
            if (connect(connection->fd_socket, addrInfos->ai_addr, addrInfos->ai_addrlen) != -1) {
                if (connection->use_ssl) {
                    if (SSL_connect(connection->ssl_fd_socket) <= 0) {
                        ERR_print_errors_fp(stderr);
                        continue;
                    }
                }
                break;
            }
        } while((addrInfos = addrInfos->ai_next) != NULL);
    } else {
        dbgprint("failed to resolv %s:%i\n", connection->address, connection->port);
        return false;
    }

    freeaddrinfo(addrInfos);
    connection->nntp_server_state = NNTP_WELCOME;

    // read the welcome from the server to finish connection!
    char *welcomeMsg = NULL;
    nntp_read(connection, &welcomeMsg, &isOk);

    if (isOk && welcomeMsg) {
        // dbgprint("Welcome Message: %s\n", welcomeMsg);
        free (welcomeMsg);
        return true;
    }
    
    dbgprint("Server returned error:%s\n", welcomeMsg)
    free (welcomeMsg);
    return false;
}

/// @brief checks if the *reply ends in NNTP style
/// @param reply the string to check
/// @param is_multiline if this is a multiline reply
/// @return true = done, false ... 
bool nntp_is_reply_done(char* reply, bool is_multiline) {
    if (!reply)
        return false;

    if (is_multiline) {
        if (strlen(reply) < 5)
            return false;
        if (strncmp(&reply[strlen(reply)-5], "\r\n.\r\n", 5) == 0)
            return true;
    } else {
        if (strlen(reply) < 2)
            return false;
        if (strncmp(&reply[strlen(reply)-2], "\r\n", 2) == 0)
            return true;
    }
    return false;
}

/// @brief reads from a socket.
/// @param reply initialize with a pointer to a uint8_t *p=NULL (realloc)
/// @return size read.
uint64_t nntp_read(struct nntp_server *connection, char **reply, bool *isOk) {
    uint64_t bufSize = 0;
    char buffer[BUFSIZ], *bfReply = *reply;
    ssize_t bytes_read = 0;
    bool has_return_code = true;

    memset(&buffer[0], 0, BUFSIZ);

    while ((bytes_read = connection->use_ssl ? SSL_read(connection->ssl_fd_socket, &buffer[0], BUFSIZ) : read(connection->fd_socket, &buffer[0], BUFSIZ)  ) > 0) {
        bfReply = (char*)realloc(bfReply, bufSize+bytes_read+1);
        memset(&bfReply[bufSize], 0, bytes_read+1);
        memcpy(&bfReply[bufSize], &buffer[0], bytes_read);
        bufSize += bytes_read;
        // if the request failed, there's only 1 line reply from server:
        if (has_return_code) {
            nntp_get_code_from_string(buffer, isOk);
            if (*isOk == false) 
                break;
            has_return_code = false; // do the codecheck only once!
        }
        if (nntp_is_reply_done(bfReply, nntp_query_state[connection->nntp_server_state].multiline)) {
            break;
        }
    }

    // On error, -1 is returned, and errno is set to indicate the error.
    if (bytes_read == -1) {
        dbgprint("read(...) returned error: (%i) %s\n", h_errno, gai_strerror(h_errno));
        free (bfReply);
        *reply = NULL;
        return 0;
    }

    *reply = bfReply;
    return bufSize;
}

/// @brief Writes a command from the nntp_query_"map" to the server
/// @param req_nntpstate the state we'll hjave to look up
/// @param  VA_LIST w. param[s]
/// @return if vsnprintf(..) generated string was sent..
bool nntp_write(struct nntp_server *connection, int req_nntpstate, ...) {
    char *arg;  // this is kinda ok, because vsnprintf is used AND the input is formatted by the nntp-format-article-id.
    int argSize = 0;
    ssize_t writesize;
    va_list vp;

    connection->nntp_server_state = req_nntpstate;

    arg = (char*)malloc(256);
    
    va_start(vp, req_nntpstate);
    argSize = vsnprintf(arg, 255, nntp_query_state[req_nntpstate].fmt, vp);
    va_end(vp);

    writesize = connection->use_ssl ? SSL_write(connection->ssl_fd_socket, arg, argSize) : write(connection->fd_socket, arg, argSize);
    free (arg);

    if (writesize != argSize)
        return false;

    
    return true;
}

/// @brief gets the int-value for the return code for a reply from server.
/// @param buffer the string to work
/// @param isOk Mandatory Valid Pointer!
/// @return the RC from server or -1 on error.
int nntp_get_code_from_string(char *buffer, bool *isOk) {
    int rv = 0;
    char tstr[4] = { 0 };

    if (!buffer)
        return -1;

    if (strlen(buffer) < 3)
        return -1;
    
    strncat(&tstr[0], buffer, 3);
    rv = atoi(&tstr[0]);
    if ((rv < 100) || (rv > 399))
        *isOk = false;
    else
        *isOk = true;

    return rv;
}

/// @brief 
/// @param aID 
/// @param buffer 
/// @return 
bool nntp_get_article(struct nntp_server *connection, char *aID, char **buffer) {
    bool isOk;
    char *read_buffer = NULL;

    // valid input ?
    if (!aID)
        return false;

    // write ARTICLE <ID> to server
    if (!nntp_write(connection, NNTP_FETCH, aID))
        return false;
    
    // get the reply from server.
    nntp_read(connection, &read_buffer, &isOk);

    // did we read anything ?
    if (!read_buffer)
        return false;

    // was the request fulfilled ok by the server ?
    if (!isOk) {
        free (read_buffer);
        return false;
    }

    *buffer = read_buffer;
    return true;
}

/// @brief sends/auths 
/// @param username 
/// @param password 
/// @return if success
bool nntp_authenticate(struct nntp_server *connection, char *username, char *password) {
    char *reply = NULL;
    bool isOk;

    if (!nntp_write(connection, NNTP_USERNAME, username))
        return false;

    nntp_read(connection, &reply, &isOk);
    if (reply) 
        free (reply);

    if (!isOk)
        return false;

    if (!password)  // if there's only a username, but no password..
        return true;

    if (!nntp_write(connection, NNTP_PASSWORD, password))
        return false;

    reply = NULL;
    nntp_read(connection, &reply, &isOk);
    if (reply) 
        free(reply);
    if (!isOk)
        return false;


    return true;
}

/// @brief gets the metadata from an yenc-header. prepared by nntp_get_yenc_header_begin_end(..) - this doesn't to checks - just extracts key/pair!
/// @param yencline
/// @return list of pair with key=value
struct pair *nntp_get_yenc_meta (char *yencLine) {
    struct pair *rv = NULL;
    char *keybegin, *keyend, *valuestart, *valueend, *curyEndPos;
    char *termKey, *termVal;    // a null terminated string 
    char *headerLine;

    // find the end of this header-line
    if (strstr(yencLine, "\r\n") == NULL) {
        return NULL;
    }

    headerLine = strndup (yencLine, strstr(yencLine, "\r\n")-yencLine);
    if (strchr(headerLine, ' ') == NULL)
        return NULL;

    curyEndPos = strchr(headerLine, ' ')+1;
    
    while (*curyEndPos) { 
        keybegin = curyEndPos;
        if (strchr(curyEndPos, '=') == NULL) 
            break;
        keyend = strchr(curyEndPos, '=');
        termKey = strndup (keybegin, keyend-keybegin);
        valuestart = keyend+1;
        valueend = strchr(valuestart, ' ');
        if (!valueend)
            valueend = strchr(valuestart, '\r');
        if (!valueend)
            valueend = strchr(valuestart, 0);   // end of string 
        if (*valuestart == '\"')
            valuestart++;
        if (*valueend == '\"')
            valueend--;
        termVal = strndup(valuestart, valueend-valuestart);
        pair_add(&rv, termKey, termVal);
        free (termKey);
        free (termVal);

        curyEndPos = valueend+1;
    }
    free (headerLine);

    return rv;
}

/// @brief Decodes a yenc encoded Header & Body 
/// @param encoded_header_buffer - starting with =ybegin
/// @param outBinary pointer to pointer for the output buffer - caller owned!
/// @param isCRCOk -1 = crc not ok, 0 = crc entry not found, 1 = crc ok
/// @return the size of the decoded buffer
size_t nntp_decode_yenc (char *encoded_header_buffer, char **outBinary, int *isCRCOk) {
    size_t binSize = 0;
    *outBinary = NULL;
    *isCRCOk = false;
    const PCRE2_SPTR8 RE_yEnc_Start = (PCRE2_SPTR8)"=ybegin (.+)\r\n";
    const PCRE2_SPTR8 RE_yEnc_End = (PCRE2_SPTR8)"=yend (.+)\r\n";
    const PCRE2_SPTR8 RE_yEnc_Part = (PCRE2_SPTR8)"=ypart (.+)\r\n";
    int pcre_err;
    PCRE2_SIZE errOffset;
    struct pair *meta_begin = NULL, *meta_part = NULL, *meta_end = NULL;
    char *bufferOut = NULL, *pair_crc32 = NULL;
    size_t enc_data_start, enc_data_end;
    uint32_t crc32;

    // sanity checks:
    if (!isCRCOk)
        return 0;

    /* prepare statements it, but once is enough */
    static pcre2_code *comp_re_start = NULL, *comp_re_end = NULL, *comp_re_part = NULL;
    if ((!comp_re_start) || (!comp_re_end) || (!comp_re_part)) {
        comp_re_start = pcre2_compile(RE_yEnc_Start, PCRE2_ZERO_TERMINATED, 0, &pcre_err, &errOffset, NULL);
        if (!comp_re_start)
            return false;
        comp_re_end = pcre2_compile(RE_yEnc_End, PCRE2_ZERO_TERMINATED, 0, &pcre_err, &errOffset, NULL);
        if (!comp_re_end)
            return false;
        comp_re_part = pcre2_compile(RE_yEnc_Part, PCRE2_ZERO_TERMINATED, 0, &pcre_err, &errOffset, NULL);
        if (!comp_re_part)
            return false;
    }

    /* process the headers: */
    pcre2_match_data *match_data = pcre2_match_data_create_from_pattern(comp_re_start, NULL);
    if (pcre2_match(comp_re_start, (const unsigned char*)encoded_header_buffer, PCRE2_ZERO_TERMINATED, 0, 0, match_data, NULL) >= 0) {   
        PCRE2_SIZE *ovector = pcre2_get_ovector_pointer(match_data);
        meta_begin = nntp_get_yenc_meta(&encoded_header_buffer[ovector[0]]);
        enc_data_start = ovector[3]+2;
    }

    if (pair_find(meta_begin, "part")) {
        match_data = pcre2_match_data_create_from_pattern(comp_re_part, NULL);
        if (pcre2_match(comp_re_part, (const unsigned char*)encoded_header_buffer, PCRE2_ZERO_TERMINATED, 0, 0, match_data, NULL) >= 0) {   
            PCRE2_SIZE *ovector = pcre2_get_ovector_pointer(match_data);
            meta_part = nntp_get_yenc_meta(&encoded_header_buffer[ovector[0]]);
            enc_data_start = ovector[3]+2; // skip \r\n
        }
    }

    match_data = pcre2_match_data_create_from_pattern(comp_re_end, NULL);
    if (pcre2_match(comp_re_end, (const unsigned char*)encoded_header_buffer, PCRE2_ZERO_TERMINATED, 0, 0, match_data, NULL) >= 0) {   
        PCRE2_SIZE *ovector = pcre2_get_ovector_pointer(match_data);
        meta_end = nntp_get_yenc_meta(&encoded_header_buffer[ovector[0]]);
        enc_data_end = ovector[0];
    }

    // with all the crlf and stuff, it's enought to reserve input encoded size as output-buffer:
    bufferOut = (char*)calloc(enc_data_end-enc_data_start, 1);
    binSize = rapidyenc_decode(&encoded_header_buffer[enc_data_start], bufferOut, enc_data_end-enc_data_start);
    *outBinary = bufferOut;

    // we have a "(p)crc" entry in the end-pair ?
    pair_crc32 = pair_find(meta_end, "crc32");
    if (!pair_crc32)
        pair_crc32 = pair_find(meta_end, "pcrc32");

    if (pair_crc32) {
        crc32 = rapidyenc_crc(bufferOut, binSize, 0);
        if (crc32 != strtoul(pair_crc32, NULL, 16))
            *isCRCOk = -1;
        else
            *isCRCOk = 1;
    } else 
        *isCRCOk = 0;

    // free everything
    pair_destroy (&meta_begin);
    pair_destroy (&meta_end);
    pair_destroy (&meta_part);

    return binSize;
}

/// @brief sets _begin/_end to the begin (first char {'='}) of the yEnc-Header Block and to the CRLF after =yEnd
/// @param yenc_data_begin 
/// @param yenc_data_end 
/// @return false if not yenc
bool nntp_get_yenc_header_begin_end(char *encoded_buffer, char **yenc_data_begin, char **yenc_data_end) {
    const PCRE2_SPTR8 RE_yEnc_Start = (PCRE2_SPTR8)"(=ybegin\\s)(.+)\r\n";
    const PCRE2_SPTR8 RE_yEnc_End = (PCRE2_SPTR8)"(=yend\\s)(.+)\r\n";
    const PCRE2_SPTR8 RE_yEnc_Part = (PCRE2_SPTR8)"(=ypart)(.+)\r\n";
    int pcre_err;
    PCRE2_SIZE errOffset;

    /* prepare statements it, but once is enough */
    static pcre2_code *comp_re_start = NULL, *comp_re_end = NULL, *comp_re_part = NULL;
    if ((!comp_re_start) || (!comp_re_end) || (!comp_re_part)) {
        comp_re_start = pcre2_compile(RE_yEnc_Start, PCRE2_ZERO_TERMINATED, 0, &pcre_err, &errOffset, NULL);
        if (!comp_re_start)
            return false;
        comp_re_end = pcre2_compile(RE_yEnc_End, PCRE2_ZERO_TERMINATED, 0, &pcre_err, &errOffset, NULL);
        if (!comp_re_end)
            return false;
        comp_re_part = pcre2_compile(RE_yEnc_Part, PCRE2_ZERO_TERMINATED, 0, &pcre_err, &errOffset, NULL);
        if (!comp_re_part)
            return false;
    }

    /* idk if that could be should be static too ? */
    pcre2_match_data *match_data = pcre2_match_data_create_from_pattern(comp_re_start, NULL);
    if (pcre2_match(comp_re_start, (const unsigned char*)encoded_buffer, PCRE2_ZERO_TERMINATED, 0, 0, match_data, NULL) >= 0) {   
        PCRE2_SIZE *ovector = pcre2_get_ovector_pointer(match_data);
        *yenc_data_begin = &encoded_buffer[ovector[2]];
    } else
        *yenc_data_begin = NULL;

    match_data = pcre2_match_data_create_from_pattern(comp_re_start, NULL);
    if (pcre2_match(comp_re_end, (const unsigned char*)encoded_buffer, PCRE2_ZERO_TERMINATED, 0, 0, match_data, NULL) >= 0) {   
        PCRE2_SIZE *ovector = pcre2_get_ovector_pointer(match_data);
        *yenc_data_end = &encoded_buffer[ovector[3]];
    } else
        *yenc_data_end = NULL;

    if (!*yenc_data_begin || !*yenc_data_end)
        return false;

    return true;
}