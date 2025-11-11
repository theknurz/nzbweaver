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
pthread_mutex_t     mw_last_check_block = PTHREAD_MUTEX_INITIALIZER;
int                 mw_cancel_thresh_pct = 0;
uint64_t            mw_failed_segments = 0;
uint64_t            mw_expected_segments = 0;

struct nntp_server nntp_server_info;
unsigned int mw_max_threads = 0;
bool         mw_quit_download = false;
bool         mw_runLoop = true;

struct thread_user_data *mw_thread_infos = NULL;
struct segment_to_filename *segments_filenames = NULL;

uint64_t        epoch_download_start;
uint64_t        epoch_download_end;
uint64_t        transfered_bytes;

bool            mw_post_ok_operation = true;  

char            *mw_display_name = NULL;

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
    extern struct NZB nzb_tree;
    
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

    if (pair_find(config_downloads, "cancelthreshpct"))
        mw_cancel_thresh_pct = atoi(pair_find(config_downloads, "cancelthreshpct"));

    for (unsigned int curFile = 0; curFile < nzb_tree.max_files; curFile++)
        mw_expected_segments += nzb_tree.files[curFile].segmentsSize;

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
    double  pct_failed = 0.;

    // connect
    if (!nntp_connect(serverInfo)) {
        LOG_MESSAGE(false, "thread %i connection failed");
        userData->connection->work_done = true;
        goto thread_exit;
    }

    // if we have username/password, authenticate
    if (serverInfo->username) {
        if (!nntp_authenticate(serverInfo, serverInfo->username, serverInfo->password)) {
            LOG_MESSAGE(false, "thread %i auth failed\n", userData->connection->connectionID);
            userData->connection->work_done = true;
            goto thread_exit;
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
            free(fullfilePath);
            free (recvBuffer);
            recvBuffer = NULL;
        } else {
            mw_failed_segments++;

            pct_failed = (double)(mw_expected_segments-mw_failed_segments)/(double)mw_expected_segments;
            pct_failed *= 100.;

            if ((mw_cancel_thresh_pct > 0) && ((int)pct_failed < mw_cancel_thresh_pct)) {
                LOG_MESSAGE(false, "Cancel-Threshold (%i) was reached, stopping download.", mw_cancel_thresh_pct);
                mw_quit_download = true;
                userData->connection->work_done = true;
                mw_post_ok_operation = false;   // don't try to assemble..
                goto thread_exit;
            }
        }

        curFile->open_segments--;
        curFile->remaining_segments--;

        LOG_MESSAGE(false, "Thread: %d loop ended, remaining_segments:%d, open_segments:%d, Filename: \"%s\"\n", userData->connection->connectionID,        
            curFile->remaining_segments, curFile->open_segments, curFile->filename);
    }

    if (!curFile && !curSeg)
        LOG_MESSAGE(false, "Thread %i has finished it's work, exiting..", userData->connection->connectionID);
    userData->connection->work_done = true;

thread_exit:
    pthread_mutex_lock(&mw_last_check_block);
    for (unsigned int i=0; i < mw_max_threads; i++) {
        if (nntp_connections[i].work_done == false) {
            am_i_last_thread = false;
            break;
        }
    }

    if (am_i_last_thread)
        mw_runLoop = false;
    pthread_mutex_unlock(&mw_last_check_block);
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

    mw_print_overview();

    if (mw_post_ok_operation) {
        q_printf ("\nDownload finished, assembling segments...\n");
        mw_post_rename();
    } else {
        q_printf ("\nDownload failed.\n");
    }
}

/// @brief this what happens AFTER the network-downloads/decoding
/// @param 
void mw_post_rename(void) {
    unsigned int filecnt;
    extern struct NZB nzb_tree;
    struct NZBFile *smallest_parfile = NULL;
    bool check_repair_ok = true;

    /* here we are: the filename game
     * What I found about usenet, nzb, yenc-headers and par2:
     * 1. IF we have par2, ALWAYS use the files in the FileDescr. Blocks of PAR2 - A L W A Y S
     * 2. IF we don't have par2, prefer yEnc OVER nzb - but check for filename because
     * 3. IF yEnc has not a valid filename use the NZB provided filename.    */
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

    LOG_MESSAGE(false, "Renaming: smallest_parfile %s found, checking par2 for file names.", smallest_parfile == NULL ? "not " : "was");    
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
        LOG_MESSAGE(false, "Assembling file %i out of %i\r", filecnt+1, nzb_tree.max_files);
        mw_join_segments(&nzb_tree.files[filecnt]);
    }

    // we can only do a integrity check if there's a parfile:
    if (smallest_parfile) {
        LOG_MESSAGE(false, "Verifying release with par2.");
        check_repair_ok = mw_post_checkrepair( nzb_tree.rename_files_to == NZBRename_yEnc ? smallest_parfile->yenc_filename : smallest_parfile->filename );
    }

    if (pair_find(config_downloads, "unrarbin") && check_repair_ok) {
        q_printf("Verify/Repair went successful, unpacking.\n");
        mw_unrar();
    }
}

/// @brief 
/// @param firstrarvol 
/// @return 
unsigned int mw_get_rar_volumes(char *firstrarvol, char ***rar_volumes) {
    extern struct NZB nzb_tree;
    const PCRE2_SPTR archRE = (PCRE2_SPTR)"Archive: (.+)";
    FILE *cmdOut = NULL;
    pcre2_code *compRE;
    pcre2_match_data *matchData;
    int pcre_err;
    PCRE2_SIZE startOffset = 0, errOffset;
    char *inBuffer = NULL;
    char temp[128];
    size_t inBuffer_size = 0, read_size;
    char **rv = NULL;
    unsigned int rv_cnt = 0;

    char *fullcmd = mprintfv("%s ltb -v \"%s/%s\"", pair_find(config_downloads, "unrarbin"), nzb_tree.download_destination, firstrarvol);
    cmdOut = popen(fullcmd, "r");
    if (!cmdOut)
        return 0;
       
    while ((read_size = fread(temp, 1, 128, cmdOut)) > 0) {
        inBuffer = (char*)realloc(inBuffer, inBuffer_size+read_size+1);
        memset(&inBuffer[inBuffer_size], 0, read_size+1);
        memcpy(&inBuffer[inBuffer_size], temp, read_size);
        inBuffer_size += read_size;
    }
    pclose(cmdOut);

    compRE = pcre2_compile(archRE, PCRE2_ZERO_TERMINATED, 0, &pcre_err, &errOffset, NULL);
    matchData = pcre2_match_data_create_from_pattern(compRE, NULL);
    startOffset = 0;
    while (pcre2_match(compRE, (const unsigned char*)inBuffer, inBuffer_size, startOffset, 0, matchData, NULL) >= 0) {
        PCRE2_SIZE *ovector = pcre2_get_ovector_pointer(matchData);
        rv = (char**)realloc(rv, sizeof(char*)*(rv_cnt+1));
        rv[rv_cnt] = strndup(&inBuffer[ovector[2]], (int)ovector[3]-ovector[2]);
        rv_cnt++;
        startOffset = ovector[1];
    }
    *rar_volumes = rv;
    return rv_cnt;
}

/// @brief 
/// @param  
void mw_unrar(void) {
    extern struct NZB nzb_tree;
    DIR*    dirList = NULL;
    struct dirent   *file_in_dest_dir;
    const PCRE2_SPTR partRE = (PCRE2_SPTR8)"part([0-9]+)";
    pcre2_code *compRE;
    pcre2_match_data *matchData;
    int pcre_err;
    PCRE2_SIZE errOffset;
    char**  rar_volumes = NULL;
    unsigned int rar_volumes_len = 0;

    dirList = opendir(nzb_tree.download_destination);
    if (!dirList) {
        LOG_MESSAGE(false, "Couldn't opendir(%s), errno: %i, %s", nzb_tree.download_destination, errno, strerror(errno));
        return;
    }    

    compRE = pcre2_compile(partRE, PCRE2_ZERO_TERMINATED, 0, &pcre_err, &errOffset, NULL);
    matchData = pcre2_match_data_create_from_pattern(compRE, NULL);

    while ((file_in_dest_dir = readdir(dirList)) != NULL) {
        if (file_in_dest_dir->d_type != DT_REG)
            continue;
        if (string_ends_width(file_in_dest_dir->d_name, ".rar")) {
            // if there's a part[..] it's a multipart and we only have to work on part01
            if (pcre2_match(compRE, (const unsigned char*)file_in_dest_dir->d_name, PCRE2_ZERO_TERMINATED, 0, 0, matchData, NULL)) {
                PCRE2_SIZE *ovector = pcre2_get_ovector_pointer(matchData);
                char* numStr = strndup(&file_in_dest_dir->d_name[ovector[2]], ovector[3]-ovector[2]);
                unsigned int partNum = atoi(numStr);
                if (partNum == 1) {
                    char *syscmd = mprintfv("%s x -idq -o+ \"%s/%s\" \"%s\"", pair_find(config_downloads, "unrarbin"), nzb_tree.download_destination, file_in_dest_dir->d_name, nzb_tree.download_destination);
                    char **tmp_volumes = NULL;
                    int syscmd_rc;
                    unsigned int tmp_volume_size;

                    tmp_volume_size = mw_get_rar_volumes(file_in_dest_dir->d_name, &tmp_volumes); // keep the rar volume-names in mem.
                    syscmd_rc = system(syscmd);
                    if (WIFEXITED(syscmd_rc))
                        syscmd_rc = WEXITSTATUS(syscmd_rc);
                    if (syscmd_rc != 0) {
                        LOG_MESSAGE(true, "%s returned %i", syscmd, syscmd_rc);
                        if (tmp_volumes && (tmp_volume_size > 0)) {
                            for (unsigned int c = 0; c < tmp_volume_size; c++)
                                free (tmp_volumes[c]);
                        }
                        free (tmp_volumes);
                    } else {
                        LOG_MESSAGE(false, "unrared %s/%s, continuing.", nzb_tree.download_destination, file_in_dest_dir->d_name);
                        // copy the volumes of the rar, to delete it later.
                        if (tmp_volumes && (tmp_volume_size > 0)) {
                            rar_volumes = (char**)realloc(rar_volumes, sizeof(char*)*(rar_volumes_len+tmp_volume_size));
                            memcpy(&rar_volumes[rar_volumes_len], tmp_volumes, sizeof(char*)*tmp_volume_size);
                            rar_volumes_len += tmp_volume_size;
                        }
                    }
                    free (syscmd);
                }
            }
        }
    }

    closedir(dirList);

    for (unsigned int c = 0; c < rar_volumes_len; c++) {
        char *fullpath = mprintfv("%s", rar_volumes[c]);
        if (unlink(fullpath) != 0)
            LOG_MESSAGE(false, "Failed to remove %s, errno %i, %s", fullpath, errno, strerror(errno));
        else
            LOG_MESSAGE(false, "Removed %s", fullpath);
        free (rar_volumes[c]);
    }

    dirList = opendir(nzb_tree.download_destination);
    if (dirList) {
        while ((file_in_dest_dir = readdir(dirList)) != NULL) {
            if ((file_in_dest_dir->d_type == DT_REG) && (string_ends_width(file_in_dest_dir->d_name, ".par2"))) {
                char *unlinkfile = mprintfv("%s/%s", nzb_tree.download_destination, file_in_dest_dir->d_name);
                if (unlink(unlinkfile) != 0)
                    LOG_MESSAGE(false, "Failed to remove:%s, errno %i, %s", unlinkfile, errno, strerror(errno));
                else
                    LOG_MESSAGE(false, "Removed:%s", unlinkfile);
                free (unlinkfile);
            }
        }
        closedir (dirList);
    }

    // last, but not least

    free (rar_volumes);
}

/// @brief runs validate/repair for releases w. par2 files
/// @param parfile 
/// @return 
bool mw_post_checkrepair(char *parfile) {
    extern struct NZB nzb_tree;
    extern struct pair *config_downloads;
    char *syscheckcmd = NULL;
    char oldcwd[PATH_MAX];
    int sysRc = 0;
    uint8_t par2Rc;

    // if par2bin is not filled.. 
    if (!pair_find(config_downloads, "par2bin"))
        return true;    // true bc. try to unrar

    getcwd(oldcwd, PATH_MAX);
    //seems we have to chdir() into the directory where the par2 files are:
    if (chdir(nzb_tree.download_destination) != 0) {
        LOG_MESSAGE(true, "Could not change to: %s!", nzb_tree.download_destination);
        return false;
    }

    /*  return codes:
    0: Success.
    1: Repairable damage found.
    2: Irreparable damage found.
    3: Invalid commandline arguments.
    4: Parity file unusable.
    5: Repair failed.
    6: IO error.
    7: Internal error.
    8: Out of memory.
    */

    syscheckcmd = mprintfv("%s v \"%s/%s\" 2>&1 > /dev/null", pair_find(config_downloads, "par2bin"), nzb_tree.download_destination, parfile);
    sysRc = system(syscheckcmd);
    free (syscheckcmd);

    if (WIFEXITED(sysRc))
        par2Rc = WEXITSTATUS(sysRc);
    else  {
        LOG_MESSAGE(true, "par2 had some error => !WIFEXITED\n");
        return false;
    }

    if (par2Rc == 0) {
        q_printf("\nDownload finished, par2 verify finished without any error found to repair.\n");
        return true;
    } else if (par2Rc == 1) {
        q_printf("\nDownload finished, but par2verify reported repairable errors, starting repair.\n");
        syscheckcmd = mprintfv("%s r \"%s/%s\" 2>&1 > /dev/null", pair_find(config_downloads, "par2bin"), nzb_tree.download_destination, parfile);
        sysRc = system(syscheckcmd);
        free (syscheckcmd);
        if (WEXITSTATUS(sysRc) != 0) {
            LOG_MESSAGE(false, "par2repair reported error.");
            return false;
        }
        return true;
    }
    return false;
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
    static unsigned int maxLineLen = 0;
    char *overview_text, *sizeString, *dlString, *speedString;
    double pct_done = 0., pct_failed = 0.;
    unsigned int curLineLen;

    if (nzb_tree.release_size > 0) {
        pct_done = (double)nzb_tree.release_downloaded / (double)nzb_tree.release_size;
        pct_done *= 100.;
    }

    if (mw_expected_segments > 0) {
        pct_failed = (double)(mw_expected_segments-mw_failed_segments)/(double)mw_expected_segments;
        pct_failed *= 100.;
    }

    sizeString = strdup(mw_val_to_hr_string(nzb_tree.release_size));
    dlString = strdup(mw_val_to_hr_string(nzb_tree.release_downloaded));
    speedString = strdup(mw_val_to_hr_string(nzb_tree.release_downloaded / secondspassed));

    overview_text = mprintfv("NZB:%s, Size:%s, Downloaded: %s (%.2f%%) (%.2f%%), Speed:%s/s", 
                    nzb_tree.display_name, sizeString, dlString, pct_done, pct_failed, speedString);  
    
    free (sizeString);
    free (dlString);
    free (speedString);

    curLineLen = strlen(overview_text);

    if (maxLineLen < curLineLen)
        maxLineLen = curLineLen;

    printf (overview_text);
    free (overview_text);

    while (curLineLen < maxLineLen) {
        printf (" ");
        curLineLen++;
    }
    printf ("\r");
}