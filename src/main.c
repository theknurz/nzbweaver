#include <stdio.h>
#include <stdlib.h>
#include <pwd.h>
#include <unistd.h>
#include <sys/types.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <rapidyenc.h>

#include "mindweaver.h"
#include "nntp.h"
#include "xmlhandler.h"
#include "utils.h"
#include "parfiles.h"

// local variables
SSL_CTX *ssl_ctx;

char* app_name = NULL;
char* cfg_file = NULL;
char* nzb_file = NULL;

int max_threads = -1;
bool quiet_output = false;

// some predefines
const char* def_configname = ".nzbweaver.cfg";
const int def_max_threads = 25;

// functions
void parse_user_args(int argc, char **argv);
void print_help(void);
void print_config(void);
void cleanup(void);
void init_connection(void);

int main(int argc, char **argv) {
    struct passwd* userInfo;

    setlocale(LC_ALL, "");
    atexit(cleanup);

    // copy basic information like our path..
    app_name = strdup(argv[0]);
    userInfo = getpwuid(getuid());
    cfg_file = mprintfv("%s/%s", userInfo->pw_dir, def_configname);

    // check if user has passed nzb-file (at least..)
    parse_user_args(argc, argv);                
    if (!nzb_file)
        return EXIT_FAILURE;

    // load it!
    if (!nzb_load(nzb_file)) {
        LOG_MESSAGE(true, "NZB File loading failed!\n");
        return EXIT_FAILURE;
    }
    // the nzb-file is everything we need to know from the user, load config:        
    if (!xml_load_config(cfg_file)) {
        LOG_MESSAGE(true, "Error opening config file\n");
        return EXIT_FAILURE;
    }

    // set values for max. threads:
    if (max_threads != -1) { // threads was not set in cmd line
        if (pair_find(config_server, "connections"))    // was threads set in config ?
            max_threads = atoi(pair_find(config_server, "connections"));
        else
            max_threads = def_max_threads;  // no, use default value
    }

    // initialize this sweet fast tool:
    rapidyenc_decode_init();
    rapidyenc_crc_init();

    // and initialize everything to connecto the server
    init_connection();  
    return EXIT_SUCCESS;
}

void parse_user_args(int argc, char **argv) {
    const char *options="c:t:n:hsqr";
    int opt;
    
    while ((opt = getopt(argc, argv, options)) != -1) {
        switch (opt) {
            case 'c':   // config file
                cfg_file = strdup(optarg);
                break;
            case 't':   // max. threads 
                mw_max_threads = atoi(optarg);
                break;
            case 'n':   // the nzb file
                nzb_file = strdup(optarg);
                break;
            case 's': // prints the example-config
                print_config();
                exit (EXIT_SUCCESS);
                break;
            case 'q': // no output
                quiet_output = true;
                break;
            case 'r':
                mw_remove_nzb_after_unpack = true;
                break;
            case 'h':   // help
                if (nzb_file) free (nzb_file);
                print_help();
                return;
        }
    }

    if (!nzb_file) {
        LOG_MESSAGE(true, "Error: You have to pass the name of a NZB file with %s -n <file>\n", app_name);
    }
}

void print_help(void) {
    printf ("%s - nzb downloader\n\n", app_name);
    printf ("-c FILE\t\t-\tConfig File Name [%s]\n", cfg_file);
    printf ("-t NUMBER\t-\tMax. Connections To Use [%i]\n", max_threads);
    printf ("-n FILE\t\t-\tNZB File, Mandatory.\n");
    printf ("-q\t\t-\tQuiet!\n");
    printf ("-s\t\t-\tPrint default-config (%s -s > ~/.nzbweaver.cfg)\n", app_name);
    printf ("-r\t\t-\tRemove NZB file after unpacking\n");
}

void print_config(void) {
    printf ("<config>\n");
    printf ("<server address=\"best.news.server.com\" port=\"119\" ssl=\"false\" connection=\"5\" username=\"username\" password=\"password\"/>\n");
    printf ("<download path=\"/my/drive/Downloads/\" unrarbin=\"/usr/bin/unrar\" par2bin=\"/usr/bin/par2\" cancelthreshpct=\"90\"/>\n");
    printf ("</config>\n");
}

void cleanup(void) {
    if (app_name) free (app_name);
    if (cfg_file) free (cfg_file);
    if (nzb_file) free (nzb_file);

    cleanup_xmlhandler();
}


void init_connection(void) {
    int port = 0;
    bool useSSL = false;
    char *address = NULL, *username = NULL, *password = NULL;

    if (pair_find(config_server, "port")) {
        port = atoi(pair_find(config_server, "port"));
    }

    if (pair_find(config_server, "ssl")) {
        if (strcasecmp(pair_find(config_server, "ssl"), "true") == 0)
            useSSL = true;
    }
    address = pair_find(config_server, "address");
    if (!address) {
        LOG_MESSAGE(true, "No Server-Address was given in configuration, exiting.\n");
        exit (EXIT_FAILURE);
    }
    username = pair_find(config_server, "username");
    password = pair_find(config_server, "password");

    if ((port <= 1) || (port > 65535)) {
        LOG_MESSAGE(true, "Port seems to be out of bounds:%i\n", port);
        exit (EXIT_FAILURE);
    }

    if (useSSL) {
        SSL_library_init();
        SSL_load_error_strings();
        OpenSSL_add_all_algorithms();

        ssl_ctx = SSL_CTX_new(TLS_client_method());
        SSL_CTX_set_verify(ssl_ctx, SSL_VERIFY_NONE, NULL);
    }

    if (mw_max_threads == 0) {  // was -t passed ?
        if (pair_find(config_server, "connections"))
            max_threads = atoi(pair_find(config_server, "connections"));
        else
            max_threads = def_max_threads;
    } else
        max_threads = mw_max_threads;

    mw_connect(address, port, username, password, useSSL, max_threads);
}
