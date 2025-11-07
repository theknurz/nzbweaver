/*
 * mindweaver.c 
 * 
 * This maintains the Main-Loop, the Output,
 * the IO, the decoding and CRC checking.
 * 
 * It's the hypnotoad of this program.
 * 
 */
#include "mindweaver.h"
#include <termios.h>

// variables:
struct nntp_server *nntp_connections = NULL;
pthread_t          *mw_threads = NULL;
pthread_mutex_t     last_check_block = PTHREAD_MUTEX_INITIALIZER;

struct nntp_server nntp_server_info;
unsigned int mw_max_threads = 0;
bool         mw_quit_download = false;
bool         mw_runLoop = true;

struct thread_user_data *mw_thread_infos = NULL;
struct segment_to_filename *segments_filenames = NULL;

uint64_t        epoch_download_start;
uint64_t        epoch_download_end;
uint64_t        transfered_bytes;

// functions:
void *mw_thread_work (void* arg);
void *mw_draw_display(void* arg);
void mw_handle_sigwinch(int sig);

// for the brief overview:
const char *nzb_status_text[] = {
    "Idle", "Downloading", "Joining", "Done"
};

/// @brief 
/// @param address 
/// @param port 
/// @param username 
/// @param password 
/// @param useSSL 
/// @param threads 
/// @return 
bool mw_connect(char *address, uint16_t port, char *username, char *password, bool useSSL, unsigned int threads) {
    mw_max_threads = threads;

    
    if (!nntp_connections) {
        // copy the data so the caller doesn't have to care about lifetime:
        nntp_server_info.address = strdup(address);
        nntp_server_info.fd_socket = -1;
        nntp_server_info.work_done = false;
        nntp_server_info.password = password ? strdup(password) : NULL;
        nntp_server_info.port = port;
        nntp_server_info.use_ssl = useSSL;
        nntp_server_info.username = username ? strdup(username) : NULL;
        nntp_server_info.connectionID = 0;
        // reserve enough space for all our connections:
        nntp_connections = (struct nntp_server*)calloc(threads, sizeof(struct nntp_server));
        mw_threads = (pthread_t*)calloc(threads, sizeof(pthread_t));
        mw_thread_infos = (struct thread_user_data*)calloc(threads, sizeof(struct thread_user_data));

        if (!mw_prepare_directories())
            return false;
    } else {
        memset(nntp_connections, 0, sizeof(struct nntp_server)*threads);
        memset(mw_threads, 0, sizeof(pthread_t)*threads);
        memset(mw_thread_infos, 0, sizeof(struct thread_user_data)*threads);
    }

    // check if BODY <id> is okay and if first NZB File is a par2, if yes, download and parse it.
    //if (!mw_prepare_download())
    //        return false;

    epoch_download_start = time(NULL);

    // copy the original memory to the thread-connection-infos, and give it a go..
    for (unsigned int i = 0; i < threads; i++) {
        memcpy(&nntp_connections[i], &nntp_server_info, sizeof(struct nntp_server));    // one string, multiple pointers.. 
        nntp_connections[i].connectionID = i;
        mw_thread_infos[i].dbg_article = NULL;
        mw_thread_infos[i].connection = &nntp_connections[i];
        mw_thread_infos[i].downloaded = NULL;
        mw_thread_infos[i].filename = NULL;
        mw_thread_infos[i].filesize = NULL;
        if (pthread_create(&mw_threads[i], NULL, mw_thread_work, (void*)&mw_thread_infos[i]) != 0)
            return false;
        pthread_detach(mw_threads[i]);
        LOG_MESSAGE(false, "started thread %i\n", i);
    }

    // enter key-press-draw-loop
    mw_loop();

    return true;    
}

/// @brief prepares directories for the download
/// @param  -
/// @return if the directories are present in the FS.
bool mw_prepare_directories(void) {
    struct passwd* userInfo;
    extern char* nzb_file;
    extern struct NZB nzb_tree;

    unsigned int nzbname_len = strlen(nzb_file);
    // download-path exists, so prepare the download path for this nzb:
    char *nzbname_start = nzb_file, *nzbname_end = nzb_file+nzbname_len, *nzbPath = NULL;
    // let's find only the name of the nzb, try to leave the ".nzb" out.s
    nzbname_start = strrchr(nzb_file, '/');
    if ((nzbname_start) && (nzbname_start+1 < nzbname_end)) // valid and smaller than the end ?
        nzbname_start++;    // it points at / if not increased
    if (!nzbname_start)
        nzbname_start = nzb_file;   // haven't found the last '/', use full-string
    if (strcasecmp(&nzbname_end[-4], ".nzb") == 0)
        nzbname_end -= 4;
   
    // check / create directory:
    if (pair_find(config_downloads, "path")) {
        struct stat pdir = { 0 };
        if (stat(pair_find(config_downloads, "path"), &pdir) == 0) {
            if ((pdir.st_mode & S_IFMT) == S_IFDIR) {
                // good path here.
                nzbPath = mprintfv("%s/%.*s/", pair_find(config_downloads, "path"), (int)(nzbname_end-nzbname_start), nzbname_start);
            } else { // yea don't play games here...
                LOG_MESSAGE(true, "%s is not a directory!\n", pair_find(config_downloads, "path"));
                return false;
            }
        }
    } else {
        // NO download-path was given, so create the directory in $HOME
        userInfo = getpwuid(getuid());
        nzbPath = mprintfv("%s/%.*s/", userInfo->pw_dir, (int)(nzbname_end-nzbname_start), nzbname_start);
    }

    // create the destination directory in the download path.
    if (mkdir(nzbPath, S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH) != 0) {
        if (errno != EEXIST) {  // we ignore the error IF the directory exists already.
            LOG_MESSAGE(true, "Couldn't create directory: %s, Errno: %d - %s\n", nzbPath, errno, strerror(errno));
            return false;
        }
    }

    nzb_tree.download_destination = strdup(nzbPath);
    free (nzbPath);
    return true;
}

/// @brief this is the workers-thread function
/// @param arg - pointer to the Userdata
void *mw_thread_work (void* arg) {
    struct thread_user_data *userData = (struct thread_user_data*)arg;
    struct nntp_server *serverInfo = userData->connection;
    struct NZBFile *curFile = NULL;
    struct NZBSegment *curSeg = NULL;
    extern struct NZB nzb_tree;
    char    *recvBuffer = NULL;
    bool    am_i_last_thread = true;

    // connect
    if (!nntp_connect(serverInfo))
        return NULL;


    // if we have username/password, authenticate
    if (serverInfo->username) {
        if (!nntp_authenticate(serverInfo, serverInfo->username, serverInfo->password)) {
            LOG_MESSAGE(false, "thread %i auth failed\n", userData->connection->connectionID);
            return NULL;
        }
    }

    // iterate thru the tree until it returns false.
    while (nzb_tree_next_segment(&curFile, &curSeg)) {
        // write article-id as nzb-release/articleid
        char *fullfilePath;
        fullfilePath = mprintfv ("%s/%s", nzb_tree.download_destination, curSeg->articleID);

        // stop everything ?
        if (mw_quit_download) {
            LOG_MESSAGE(false, "mw_quit_download == true, stopping Thread %i", userData->connection->connectionID);
            break;
        }

        LOG_MESSAGE(false, "%d thread requesting segment-Nr:%d for file \"%s\"\n", userData->connection->connectionID, curSeg->number, curFile->filename);
        
        // set state to downloading and increase open_segments
        curFile->state = NFState_Downloading; 
        curFile->open_segments++;

        // request an article from the server -> this can and will fail!
        if (mw_get_binary_from_article(userData, curFile, curSeg, &recvBuffer)) {
            write_to_file(fullfilePath, (uint8_t*)recvBuffer, curSeg->decoded_bytes);
            free (recvBuffer);
            recvBuffer = NULL;
        }

        if (curFile->open_segments > 0)
             curFile->open_segments--;
        else
            LOG_MESSAGE(false, "SYNC ERROR: %s -> open_segments would have been -1!", curFile->filename);

        curFile->remaining_segments--;

        LOG_MESSAGE(false, "Thread: %d loop ended, remaining_segments:%d, open_segments:%d, Filename: \"%s\"\n", userData->connection->connectionID,        
            curFile->remaining_segments, curFile->open_segments, curFile->filename);
    }

    if (!curFile && !curSeg)
        LOG_MESSAGE(false, "Thread %i has finished it's work, exiting..", userData->connection->connectionID);
    userData->connection->work_done = true;

    pthread_mutex_lock(&last_check_block);
    for (unsigned int i=0; i < mw_max_threads; i++) {
        if (nntp_connections[i].work_done == false) {
            am_i_last_thread = false;
            break;
        }
    }

    if (am_i_last_thread)
        mw_runLoop = false;
    pthread_mutex_unlock(&last_check_block);
    nntp_disconnect(serverInfo);
    pthread_exit(NULL);
}

/// @brief retrieves an article
/// @param articleID 
/// @param binSize 
/// @return 
bool mw_get_binary_from_article(struct thread_user_data *userData, struct NZBFile *curFile, struct NZBSegment *curSeg, char **buffer) {
    extern struct NZB nzb_tree;
    char *recvBuffer = NULL;
    struct nntp_server *serverInfo = userData->connection;

    if (!curFile || !curSeg) {
        *buffer = NULL;
        return false;
    }

    // request an article from the server -> this can and will fail!
    if (nntp_get_article(serverInfo, curSeg->articleID, &recvBuffer)) {
        LOG_MESSAGE(false, "%d thread fetched segment-Nr:%d for file \"%s\"\n", userData->connection->connectionID, curSeg->number, curFile->filename);
        userData->dbg_article = curSeg->articleID;
        userData->filename = curFile->filename;
        userData->downloaded = &curFile->download_size;     
        userData->filesize = &curFile->file_size;
        char *yEncStart, *yEncEnd, *binary = NULL;
        size_t binarySize = 0;
        int crcOk;

        // get the begin of the header (the =y<begin/part/end>\s is stripped!)
        if (nntp_get_yenc_header_begin_end(recvBuffer, &yEncStart, &yEncEnd)) {
            // do a check for the filename!
            if (curSeg->number == 1) {
                if (!mw_parse_yenc_header(yEncStart, curFile, &userData->filesize)) {
                    LOG_MESSAGE(false, "%d thread failed to parse segments-Nr:%d yEncHeader: %.10s", userData->connection->connectionID, curSeg->number, yEncStart);
                    goto error;
                }
            }
   
            // decode w. rapidyenc & return binary size:
            binarySize = nntp_decode_yenc(yEncStart, &binary, &crcOk);
            if (binarySize == 0) {
                curFile->crcOk = false;     // yea this isn't ok.
                goto error;
            }

            if (!crcOk) curFile->crcOk = false;
            
            curSeg->decoded_bytes = binarySize;
            *userData->downloaded += binarySize;
            nzb_tree.release_downloaded += curSeg->bytes;   // because the release_size is from nzb too, not from yenc-header!!
            *buffer = binary;
            free (recvBuffer);
        }
    } else {
        LOG_MESSAGE (false, "FAILED: Request of segment Nr: %d for File \"%s\"\n", curSeg->number, curFile->filename);
        curFile->crcOk = false; // with some failed segments, the file cannot be ok.
        goto error;
    }    

    return true;
error:
    if (recvBuffer) free(recvBuffer);
    *buffer = NULL;
    return false;
}

bool mw_parse_yenc_header (char *yEncStart, struct NZBFile *curFile, uint64_t **filesize) {
    // We have to check the filename here because the NZBs filename COULD be 
    // obfuscated and not a real filename but gibberish.
    struct pair *yBeginMeta = NULL;        
    yBeginMeta = nntp_get_yenc_meta(yEncStart);

    // filename provided by yenc.
    if (pair_find(yBeginMeta, "name"))
        curFile->yenc_filename = strdup(pair_find(yBeginMeta, "name"));
    else
        curFile->yenc_filename = NULL;

    // binary size! 
    if (pair_find(yBeginMeta, "size"))
        *(*filesize) = atoi(pair_find(yBeginMeta, "size"));
    else 
        *filesize = NULL;

    return true;
}

/// @brief tries to guess the right filename
/// @param curFile - the current File you're joining.
/// @return either a pointer to a valid string or NULL in panic.
char* mw_set_filename(struct NZBFile* curFile) {
    extern struct NZB nzb_tree;
   
    // some sanity checks 
    if ((nzb_tree.rename_files_to == NZBRename_undefined || nzb_tree.rename_files_to == NZBRename_yEnc) && !curFile->yenc_filename)
        return NULL;
    if (nzb_tree.rename_files_to == NZBRename_NZB && !curFile->filename)
        return NULL;
    
   
    return NULL;
}

/// @brief The last thing we do for sure: Join every segment to files.
/// @param curFile the current file to join
/// @param trueFileName it's true name, revealed by par2 or the yEnc-Header.
void mw_join_segments(struct NZBFile *curFile) {
    int fd_complete_file = -1;
    extern struct NZB nzb_tree;
    char *finalName;

    if (!curFile->yenc_filename) {
        LOG_MESSAGE(true, "cannot join %s, missing yenc_filename", curFile->filename);
        return;
    }

    finalName = nzb_tree.rename_files_to == NZBRename_NZB ?  curFile->filename : curFile->yenc_filename;

    char *release_file_name = mprintfv("%s/%s", nzb_tree.download_destination, finalName);

    if (!curFile) {
        LOG_MESSAGE(false, "mw_join_segments: *arg == NULL\n");
        return;
    }

    curFile->state = NFState_Joining;

    fd_complete_file = open(release_file_name, O_CREAT | O_TRUNC | O_WRONLY, S_IRUSR | S_IWUSR); 
    if (fd_complete_file == -1) {
        LOG_MESSAGE(true, "(JOIN)Couldn't create %s, errno %i (%s)", release_file_name, errno, strerror(errno));
        free (release_file_name);
        return;
    }

    for (unsigned int i = 0; i < curFile->segmentsSize; i++) {
        if (curFile->segments[i].decoded_bytes == 0)
            continue;

        char *segmentFile = mprintfv("%s%s", nzb_tree.download_destination, curFile->segments[i].articleID);
        char *joinbuffer;
        int fd_join = open(segmentFile, O_RDONLY);
        
        if (fd_join == -1) {
            LOG_MESSAGE(false, "Segment %d (%s) for File %s MISSING.", curFile->segments[i].number, curFile->segments[i].articleID, curFile->yenc_filename);
            unlink(segmentFile);
            free (segmentFile);
            continue;   // yea, a part could be missing, jump to next
        }
        
        joinbuffer = (char*)calloc(curFile->segments[i].decoded_bytes, 1);
        ssize_t read_in_bytes = read(fd_join, joinbuffer, curFile->segments[i].decoded_bytes);
        ssize_t wrote_out_bytes = write(fd_complete_file, joinbuffer, curFile->segments[i].decoded_bytes);
        curFile->joined_size += wrote_out_bytes;
        close(fd_join);
        LOG_MESSAGE(false, "Segment %d (%s) for File %s written: %i of %i bytes.", curFile->segments[i].number, curFile->segments[i].articleID, curFile->yenc_filename, wrote_out_bytes, read_in_bytes);
        unlink(segmentFile);
        free(segmentFile);
        free(joinbuffer);
    }

    curFile->state = NFState_Done;
    // last file ?
    if (nzb_tree.current_file == nzb_tree.max_files) {
        mw_runLoop = false;
        LOG_MESSAGE(false, "End of NZB-Queue reached, post-processing...");
    }
    return;
}

/// @brief cleans up (the most) stuff
/// @param  
void mw_quit_and_clean(void) {
    mw_quit_download = true;

    for (unsigned int i = 0; i < mw_max_threads; i++)
        pthread_join(mw_threads[i], NULL);

    // this is just to pacify valgrind :P
    if (mw_threads) free (mw_threads);
    if (nntp_connections) free (nntp_connections);
    if (mw_thread_infos) free (mw_thread_infos);
}

/// @brief prints the overview every n seconds.
/// @param  
void mw_loop(void) {
    extern struct NZB nzb_tree;

    q_printf ("Starting download...\n");

    while (mw_runLoop) {
        mw_print_overview();
        usleep(100*1000);   // 10 updates per second ?
    }

    q_printf ("\nDownload finished, assembling segments...\n");
    
    mw_post_rename();
    mw_post_checkrepair();

}

/// @brief this what happens AFTER the network-downloads/decoding
/// @param 
void mw_post_rename(void) {
    unsigned int filecnt;
    extern struct NZB nzb_tree;
    struct NZBFile *smallest_parfile = NULL;

    /* here we are: the filename game
     * What I found about usenet, nzb, yenc-headers and par2:
     * IF we have par2, ALWAYS use the files in the FileDescr. Blocks of PAR2 - A L W A Y S
     * IF we don't have par2, prefer yEnc OVER nzb - but check for filename because
     * IF yEnc has not a valid filename use the NZB provided filename.    */
    for (filecnt = 0; filecnt < nzb_tree.max_files; filecnt++) {
        struct NZBFile *curFile = &nzb_tree.files[filecnt];
        if ((string_ends_width(curFile->filename, ".par2")) || (string_ends_width(curFile->yenc_filename, ".par2"))) {
            if (!smallest_parfile) {
                smallest_parfile = curFile;
                continue;
            }
            if (smallest_parfile->file_size > curFile->file_size)
                smallest_parfile = curFile;
        }
    }

    if (smallest_parfile->segmentsSize > 1)
        smallest_parfile = NULL;    // the "main" segment shouldn't be larger than 1 message.

    LOG_MESSAGE(true, "Renaming: smallest_parfile %s found, checking par2 for file names.", smallest_parfile == NULL ? "not " : "was");    
    if (smallest_parfile) {
        bool wasMainParFile;
        get_par2_filenames(smallest_parfile, &wasMainParFile);
        nzb_tree.rename_files_to = NZBRename_undefined;

        for (filecnt = 0; filecnt < nzb_tree.max_files; filecnt++) {
            struct NZBFile *curFile = &nzb_tree.files[filecnt];
            if (is_par2_list(curFile->yenc_filename)) {
                nzb_tree.rename_files_to = NZBRename_yEnc;
                break;
            }
            if (is_par2_list(curFile->filename)) {
                nzb_tree.rename_files_to = NZBRename_NZB;
                break;
            }
        }
    }

    for (filecnt = 0; filecnt < nzb_tree.max_files; filecnt++) {
        LOG_MESSAGE(true, "Assembling file %i out of %i\r", filecnt+1, nzb_tree.max_files);
        mw_join_segments(&nzb_tree.files[filecnt]);
    }
}

/*
    // no par2 was found, it means to prefer the yEnc name over the nzb name
    if (!smallest_parfile) {
        for (filecnt = 0; filecnt < nzb_tree.max_files; filecnt++) {
            struct NZBFile *curFile = &nzb_tree.files[filecnt];
            // now let's check if one of two possible strings has a filename in it.
            char *nzb_name = NULL;
            contains_filename(nzb_tree.files[filecnt].filename, &nzb_name);
            char *yenc_name = NULL;
            contains_filename(nzb_tree.files[filecnt].yenc_filename, &yenc_name);
            char *oldFullName;
            char *newFullName;

            // only use nzb's filename if no yenc_filename was set (should never happen)!
            if (!yenc_name && nzb_name) {
                oldFullName = mprintfv("%s/%s", nzb_tree.download_destination, curFile->yenc_filename);
                newFullName = mprintfv("%s/%s", nzb_tree.download_destination, curFile->filename);
                rename(oldFullName, newFullName);
                free (oldFullName);
                free (newFullName);
            }
        }
    } else {
        bool is_yenc_name = true;
        char *oldFullName;
        char *newFullName;
        bool isMainPar2;
        // we have written using the yBegin name!
        char *par2File = mprintfv("%s/%s", nzb_tree.download_destination, smallest_parfile->yenc_filename);
        if (!get_par2_filenames(par2File, &isMainPar2)) {
            LOG_MESSAGE(false, "Couldn't extract any file names from par2: %s", par2File);
        } else
            LOG_MESSAGE(false, "Found %i files in par2, comparing with yEnc/NZB Field...", par_filenames_length);
        free (par2File);            
        par2File = NULL;
        // now just find a filename that don't end in par2 and look it up in the file table we've found!
        for (filecnt = 0; filecnt < nzb_tree.max_files; filecnt++) {
            struct NZBFile *testfile = &nzb_tree.files[filecnt];
            if ((!string_ends_width(testfile->filename, ".par2")) && (!string_ends_width(testfile->yenc_filename, ".par2"))) {
                // ok we've found one - now let's check w. the list of par2
                if (is_par2_list(testfile->filename))
                    is_yenc_name = false;
                else if (is_par2_list(testfile->yenc_filename))
                    is_yenc_name = true;
                LOG_MESSAGE(false, "Filename-Field validated as: %s", is_yenc_name == true ? "yenc_filename" : "filename");
            }
        }
        // now, we should know if the par referes to a filenamein ybegin-name or nzb-name.
        if (!is_yenc_name) {
            for (filecnt = 0; filecnt < nzb_tree.max_files; filecnt++) {
                struct NZBFile *curFile = &nzb_tree.files[filecnt];
                oldFullName = mprintfv("%s/%s", nzb_tree.download_destination, curFile->yenc_filename);
                newFullName = mprintfv("%s/%s", nzb_tree.download_destination, curFile->filename);
                int check = rename(oldFullName, newFullName);
                if (check == -1)
                    LOG_MESSAGE(false, "Couldn't rename from %s to %s: errno %i, %s", oldFullName, newFullName, errno, strerror(errno));
                else
                    LOG_MESSAGE(false, "Renamed \"%s\" to \"%s\"", curFile->yenc_filename, curFile->filename);
                free (oldFullName);
                free (newFullName);
            }
        }
    }
*/

void mw_post_checkrepair(void) {
    extern struct NZB nzb_tree;
    extern struct pair *config_downloads;
    char *syscheckcmd = NULL;
    char oldcwd[PATH_MAX];

    // if par2bin is not filled.. 
    if (!pair_find(config_downloads, "par2bin"))
        return;

    getcwd(oldcwd, PATH_MAX);
    //seems we have to chdir() into the directory where the par2 files are:
    if (!chdir(nzb_tree.download_destination)) {
        LOG_MESSAGE(true, "Could not change to: %s!", nzb_tree.download_destination);
        return;
    }
    
}

/// @brief 
/// @param valIn 
/// @return  static, so not thread-safe.
char* mw_val_to_hr_string(uint64_t valIn) {
    static char rv[256];
    double cvt;
    const char* hrUnit = convert_value_to_human(valIn, &cvt);

    snprintf(rv, 255, "%.2f%s", cvt, hrUnit);
    rv[255] = 0;
    return rv;
}

/// @brief a simple overview whats happening.
/// @param  
void mw_print_overview(void) {
    extern struct NZB nzb_tree;
    uint64_t now = time(NULL);
    uint64_t secondspassed = (now - epoch_download_start) == 0 ? 1 : now - epoch_download_start;    
        
    printf ("NZB:%.15s\tSize:%s\t", nzb_tree.name, mw_val_to_hr_string(nzb_tree.release_size));
    printf ("Downloaded:%s ", mw_val_to_hr_string(nzb_tree.release_downloaded));
    if (nzb_tree.release_size > 0) {
        double pct_done = (double)nzb_tree.release_downloaded / (double)nzb_tree.release_size;
        pct_done *= 100.;
        printf ("(%.2f%%)\t", pct_done);
    }
    printf ("Speed:%s/s\tThreads:%i\r", mw_val_to_hr_string(nzb_tree.release_downloaded / secondspassed), mw_max_threads);
}

/// @brief if the first file in the NZB is a .par2 file, get that - check if BODY <articleID> is possible? 
/// @param  
bool mw_prepare_download(void) {
    extern struct NZB nzb_tree;
    char        *buffer = NULL;
    bool        isOk, is_mainPar;
    char        *yEncStart, *yEncEnd, *binary = NULL;
    size_t      binarySize = 0;
    int         crcOk;
    struct NZBFile *smallestpar = NULL;

    // do we have files in the nzb ?
    if (nzb_tree.max_files == 0) {
        LOG_MESSAGE(true, "NZB didn't contain any files, exiting.");
        return false;
    }

    // find the smallest par2 file, if any.
    for (unsigned int filecnt = 0; filecnt < nzb_tree.max_files; filecnt++) {
        if ((string_ends_width(nzb_tree.files[filecnt].filename, ".par2")) || (string_ends_width(nzb_tree.files[filecnt].yenc_filename, ".par2"))) {
            if (!smallestpar) {
                smallestpar = &nzb_tree.files[filecnt];
                continue;
            }
            
            if (smallestpar->file_size > nzb_tree.files[filecnt].file_size)
                smallestpar = &nzb_tree.files[filecnt];
        }
    }

    // any PAR2 at all ?
    if (!smallestpar)
        return true;

    // ONLY support for 1-segmented-par. (aka "main")
    if (smallestpar->segmentsSize > 1)
        return true;

    // blah connect, auth..
    nntp_server_info.use_body = false;
    if (!nntp_connect(&nntp_server_info)) {
        LOG_MESSAGE(true, "Server check/prepare failed, exiting!");
        return false;
    }

    if (!nntp_authenticate(&nntp_server_info, nntp_server_info.username, nntp_server_info.password)) {
        LOG_MESSAGE(true, "Authentication failed!");
        nntp_disconnect(&nntp_server_info);
        return false;
    }

    // test if BODY <ID> works
    if (!nntp_write(&nntp_server_info, NNTP_BODY, smallestpar->segments[0].articleID)) {
        nntp_disconnect(&nntp_server_info);
        LOG_MESSAGE(true, "Couldn't send request to server.");
        return false;
    }

    nntp_read(&nntp_server_info, &buffer, &isOk);

    if (!isOk) {
        nntp_server_info.use_body = false;      // no body <id> use article <id>
        // still try to read article!
        if (buffer) free (buffer);
        buffer = NULL;
        nntp_disconnect(&nntp_server_info);
        return false;
    } else {
        nntp_disconnect(&nntp_server_info);        
        if (nntp_get_yenc_header_begin_end(buffer, &yEncStart, &yEncEnd)) {
            binarySize = nntp_decode_yenc(yEncStart, &binary, &crcOk);
            get_par2_filenames_from_memory(binary, binarySize, &is_mainPar);

            // right now, we only can check if the NZB-Name is 
        }
        nntp_server_info.use_body = true;
    }

    LOG_MESSAGE(false, "preDownload tasks done.");

    /*
    if (has_first_par2) {
        bool isMainPar;
        LOG_MESSAGE(false, "First file in NZB was really a par2!");
        // sweet...
        if (nntp_get_yenc_header_begin_end(buffer, &yEncStart, &yEncEnd)) {
            binarySize = nntp_decode_yenc(yEncStart, &binary, &crcOk);
            // now get the par2-list.
            if (get_par2_filenames_from_memory(binary, binarySize, &isMainPar))
                nzb_tree.skip_recovery = true;  // we want only data, no recovery files.
            for(unsigned int filecnt = 0 ; filecnt < nzb_tree.max_files; filecnt++) {
                // we only have the nzb's entry.
                if (!string_ends_width(nzb_tree.files[filecnt].filename, ".par2")) {
                    if (is_par2_list(nzb_tree.files[filecnt].filename)) 
                        nzb_tree.rename_files_to = NZBRename_NZB;
                    else
                        nzb_tree.rename_files_to = NZBRename_yEnc;
                    break;
                }
            }
            if (nzb_tree.rename_files_to == NZBRename_NZB) {
                char *destfirstpar = mprintfv("%s/%s", nzb_tree.download_destination, nzb_tree.files[0].filename);
                write_to_file(destfirstpar, (uint8_t*)binary, binarySize);
            }
            if (binary) free (binary);
            if (buffer) free (buffer);
        }
    } else {
        nzb_tree.skip_recovery = false;     // yeah get everything.
        nzb_tree.rename_files_to = NZBRename_undefined;       // see xmlhandler.h
    }
    */ 

    nntp_disconnect(&nntp_server_info);
    return true;
}