#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <ctype.h>
#include <pcre2.h>
#include <pthread.h>
#include "xmlhandler.h"

// non "exported" functions:
char *xml_load_file(char *filename);
char* generate_errno_string(void);
bool parse_config_file(char* buffer);
void parse_config_element(void *userData, const char *name, const char **attribs);

bool parse_nzb_file(char* buffer);
void parse_nzb_start_element(void *userData, const char *name, const char **attribs);
void parse_nzb_end_element(void *userData, const char* name);
void parse_nzb_text_data(void *userData, const XML_Char *s, int len);

void parse_nzb_element_file(const char **attribs, struct NZBFile* file);
void parse_nzb_element_segment(void *userData, const char **attribs);

struct NZBSegment* nzb_tree_find_segment_from_file (struct NZBFile *haystack, unsigned int needle);

// local variables:
struct pair *config_server = NULL;
struct pair *config_downloads = NULL;

struct NZB  nzb_tree = { .files = NULL, .max_files = 0, .name = NULL, .release_size = 0, .current_file = 0, .release_downloaded = 0} ;
bool        parse_is_nzb_head = false;
XML_Parser  nzbParser; 

pthread_mutex_t     nzb_tree_next_mutex = PTHREAD_MUTEX_INITIALIZER;

char *xml_load_file(char *filename) {
    FILE *inFile = NULL;
    char *fileBuffer = NULL;
    long inFileSize = 0;

    if (!filename)
        return false;
    
    if ((inFile = fopen(filename, "r")) == NULL)
        goto error;

    if (fseek(inFile, 0, SEEK_END) == -1)
        goto error;

    inFileSize = ftell(inFile);
    fileBuffer = (char*)calloc(inFileSize+1, 1);
    if (!fileBuffer)
        goto error;

    if (fseek(inFile, 0, SEEK_SET) == -1)
        goto error;


    if (fread(fileBuffer, sizeof(char), inFileSize, inFile) != (size_t)inFileSize)
        goto error;

    fclose(inFile);
    return fileBuffer;

error:
    if (inFile) 
        fclose(inFile);
    return NULL;

}

/// @brief loads/parses the config file.
/// @param filename 
/// @param error_string_opt if NULL no string will be returned in case of error. (owner = caller)
/// @return success
bool xml_load_config(char* filename) {
    return parse_config_file(xml_load_file(filename));
}

char* generate_errno_string(void) {
    const char *fmt = "%s, errno:%i, %s";
    char* rv = NULL;
    ssize_t rvlen = snprintf (NULL, 0, fmt, errno, strerror(errno));
    rv = (char*)malloc(rvlen+1);
    snprintf(rv, rvlen+1, fmt, errno, strerror(errno));
    return rv;
}

void parse_config_element(void *userData, const char *name, const char **attribs) {
    struct pair **curPair = NULL;
    (void)userData;
    if (strcmp(name, "server") == 0)
        curPair = &config_server;
    else if (strcmp(name, "download") == 0)
        curPair = &config_downloads;
    
    if (!curPair)
        return;
    
    for (int idx = 0; attribs[idx]; idx+=2) {
        pair_add(curPair,  (char*)attribs[idx], (char*)attribs[idx+1]);
    }
}

bool parse_config_file(char* buffer) {
    XML_Parser  cfgParser = XML_ParserCreate(NULL);

    if (!buffer)
        return false;

    if (!cfgParser)
        return false;

    XML_SetElementHandler(cfgParser, parse_config_element, NULL);

    if (XML_Parse(cfgParser, buffer, strlen(buffer), true) == XML_STATUS_ERROR)  {
        LOG_MESSAGE(true, "Parse error at line %lu:\n%s\n", XML_GetCurrentLineNumber(cfgParser), XML_ErrorString(XML_GetErrorCode(cfgParser))); 
        XML_ParserFree(cfgParser);
        return false;
    }

    return true;
}

/// @brief returns the length of a pair_array
/// @param keyval 
/// @return the length
unsigned int pair_length(struct pair *keyval) {
    unsigned int rv = 0;

    if (!keyval) 
        return 0;

    do {
        rv++;
    } while ((keyval = keyval->next) != NULL);

    return rv;
}

/// @brief adds a key/value pair to the list
/// @param keyval 
/// @param key 
/// @param value 
void pair_add(struct pair **keyval, char *key, char *value) {
    struct pair *w = *keyval, *newpair;

    newpair = (struct pair*)calloc(1, sizeof(struct pair));
    newpair->key = strdup(key);
    newpair->value = strdup(value);
    newpair->next = NULL;
    newpair->prev = NULL;

    if (w) {
        while (w->next)
            w = w->next;
        w->next = newpair;
        newpair->prev = w;
    } else {
        w = newpair;
        w->prev = NULL;
        *keyval = w;        
    }
}

/// @brief deletes the pair-list
/// @param keyval 
void pair_destroy(struct pair **keyval) {
    struct pair *last = *keyval;

    if (!last)  // list empty ?
        return;

    while (last->next)
        last = last->next;

    do {
        if (last->key)
            free (last->key);
        if (last->value)
            free (last->value);
    } while ((last = last->prev) != NULL);
}

/// @brief removes one pair
/// @param keyval 
/// @param needle 
void pair_remove(struct pair **keyval, char *needle) {
    struct pair *haystack = *keyval;
    struct pair *fnd = NULL;

    while (haystack) {
        if (haystack->key) {
            if (strcasecmp(haystack->key, needle) == 0) {
                fnd = haystack;
                break;
            }
        }
    }

    if (fnd) {
        if (fnd->prev) {    // we're not the first element (here: haystack)
            fnd->prev->next = fnd->next;
        } else {    // we are the first element, update w. new entry-pointer
            *keyval = fnd->next;
            (*keyval)->prev = NULL;
        }

        if (fnd->next)
            fnd->next->prev = fnd->prev;
        else
            fnd->prev = NULL;

        // remove allocations:
        if (fnd->key) free (fnd->key);
        if (fnd->value) free (fnd->value);
        free (fnd);

        // just for the sake of knowing:
        fnd = NULL;
    }
}

/// @brief searches a pair for  a key
/// @param keyval the list
/// @param needle the neede to look up
/// @return NULL if not found, !NULL if found, but you're not the owner of the memory.
char* pair_find(struct pair *keyval, char* needle) {
    char *rv = NULL;
    struct pair *fnd = NULL, *current = keyval;

    if (!keyval)
        return NULL;

    do {
        if (strcmp(current->key, needle) == 0) {
            fnd = current;
            break;
        }
    } while ((current = current->next) != NULL);

    if (fnd)
        rv = current->value;

    return rv;
}

/// @brief loads a nzb File and parses it.
/// @param filename 
/// @return success
bool nzb_load(char *filename) {
    char *buffer = xml_load_file(filename);
    nzbParser = XML_ParserCreate(NULL);
    struct parse_userdata my_userdata =  { .file = NULL, .segment = NULL };

    if (!buffer)
        return false;

    if (!nzbParser)
        return false;

    nzb_tree.name = strdup(filename);
    
    XML_SetElementHandler(nzbParser, parse_nzb_start_element, parse_nzb_end_element);
    XML_SetCharacterDataHandler(nzbParser, parse_nzb_text_data);
    XML_SetUserData(nzbParser, &my_userdata);
    if (XML_Parse(nzbParser, buffer, strlen(buffer), true) == XML_STATUS_ERROR)  {
        LOG_MESSAGE(true, "Parse error at line %lu:\n%s\n", XML_GetCurrentLineNumber(nzbParser), XML_ErrorString(XML_GetErrorCode(nzbParser)));       
        XML_ParserFree(nzbParser);
        return false;
    }
    return true;
}

/// @brief extract the necessary data from the xml-nzb attribs, trys to read a filename.
/// @param attribs 
/// @param file 
void parse_nzb_element_file(const char **attribs, struct NZBFile* file) {
    memset(file, 0, sizeof (struct NZBFile));
    file->crcOk = true;
    file->remaining_segments = 0xFFFF;

    // we care about subject, everything else is kinda unnecessary for us:
    for (int i=0; attribs[i]; i+=2) {
        if (strcmp(attribs[i], "subject") == 0) {
            file->filename = contains_filename(attribs[i+1]);
            break;
        }
    }

    // ok it could be that the file is quoted, so let's unquote that:
    if ((file->filename[0] == '\"') && (file->filename[strlen(file->filename)-1] == '\"')) {
        char *tmpstr = file->filename;
        file->filename = strndup(&tmpstr[1], strlen(&tmpstr[1])-1);
        free (tmpstr);
    }
}

/// @brief parses the "<segment..> stuff of a node"
/// @param userData 
/// @param attribs 
void parse_nzb_element_segment(void *userData, const char **attribs) {
    struct parse_userdata *data = (struct parse_userdata*)userData;

    struct NZBFile *nzbfile = data->file;
    unsigned int bytes = 0, number = 0;
    for (int i = 0; attribs[i]; i+=2) {
        if (strcmp(attribs[i], "bytes") == 0) {
            bytes = atoi(attribs[i+1]);
        } else if (strcmp(attribs[i], "number") == 0) {
            number = atoi(attribs[i+1]);
        }
    }
    nzb_tree.release_size += bytes;
    nzbfile->file_size += bytes;
    // we only download stuff, so bytes > 0 AND number is a 1-based index.
    if ((bytes > 0) && (number > 0)) {
        nzbfile->segments = (struct NZBSegment*)realloc(nzbfile->segments, sizeof(struct NZBSegment) * (nzbfile->segmentsSize+1));
        nzbfile->segments[nzbfile->segmentsSize].articleID = NULL;
        nzbfile->segments[nzbfile->segmentsSize].bytes = bytes;
        nzbfile->segments[nzbfile->segmentsSize].number = number;
        data->segment = &nzbfile->segments[nzbfile->segmentsSize];
        nzbfile->segmentsSize++;
    }
}

/// @brief cb for the start of a node.
/// @param userData the parse-userdata-struct
/// @param name of the node
/// @param attribs pointer to array of attribs.
void parse_nzb_start_element(void *userData, const char *name, const char **attribs) {
    struct parse_userdata *data = (struct parse_userdata*)userData;

    if (strcmp(name, "head") == 0) {
        parse_is_nzb_head = true;
    } else if (strcmp(name, "file") == 0) {
        // begins a "<file..>":
        nzb_tree.files = (struct NZBFile*)realloc(nzb_tree.files, sizeof(struct NZBFile)*(nzb_tree.max_files+1));
        parse_nzb_element_file(attribs, &nzb_tree.files[nzb_tree.max_files]);
        data->file = &nzb_tree.files[nzb_tree.max_files];
        nzb_tree.max_files++;
    } else if (strcmp(name, "segment") == 0) {
        parse_nzb_element_segment(userData, attribs);
    }
}

/// @brief cb for the end of a node.
/// @param userData the parse-userdata-struct
/// @param name of the node
void parse_nzb_end_element(void *userData, const char* name) {
    struct parse_userdata *data = (struct parse_userdata*)userData;    
    if (strcmp(name, "head") == 0) {
        parse_is_nzb_head = false;
    } else if (strcmp(name, "file") == 0) {
        data->file->remaining_segments = data->file->segmentsSize;
        data->file = NULL; // after file/>
    } else if (strcmp(name, "segment") == 0) {
        data->segment = NULL; // after segment/>
    }
}

/// @brief this reads the articleIDs from the text-area of the node.
/// @param userData the parse-userdata-struct
/// @param s the text itself.
/// @param len the length of the text
void parse_nzb_text_data(void *userData, const XML_Char *s, int len) {
    struct parse_userdata *data = (struct parse_userdata*)userData;    
    bool isBlankText = true;
    char *text = strndup(s, len);

    if (data->segment) {
        if (text) {
            for (int c = 0; c < len; c++) {
                if (!isspace(text[c])) {
                    isBlankText = false;
                    break;
                }
            }
            free (text);            
            if (!isBlankText)
                data->segment->articleID = strndup(s, len);
        }
    }
}

/// @brief finds a segment from a file using it's number.
/// @param haystack the File we're looking our segment for.
/// @param needle the number (NZB-1-based !!!!) we're looking for
/// @return the found segment, or NULL if the segment isn't available.
struct NZBSegment* nzb_tree_find_segment_from_file (struct NZBFile *haystack, unsigned int needle) {
    struct NZBSegment* rv = NULL;
    unsigned int segit = 0;

    while (segit < haystack->segmentsSize) {
        if (haystack->segments[segit].number == needle) {
            rv = &haystack->segments[segit];
            break;
        }
        segit++;
    }

    return rv;
}

/// @brief gets the next segment in line, returning the filename and the articleID
/// @param curFile - pointer to pointer of current file processing
/// @param curSeg - poiner to pointer of current segment processing
/// @return true = more segments/filenames
bool nzb_tree_next_segment (struct NZBFile **inpFile, struct NZBSegment **inpSeg) {
    struct NZBFile *curFile;    

    if (nzb_tree.current_file >= nzb_tree.max_files) {     // file idx out of bounds ?
        *inpFile = NULL;
        *inpSeg = NULL;
        return false;
    }

    // this a valid file here:
    pthread_mutex_lock(&nzb_tree_next_mutex);
    curFile = &nzb_tree.files[nzb_tree.current_file];    
    if (curFile->current_segment < curFile->segmentsSize) {   // are there segments left ?
        *inpFile = curFile;
        *inpSeg = &curFile->segments[curFile->current_segment];
        curFile->current_segment++; // for next caller..
        pthread_mutex_unlock(&nzb_tree_next_mutex);
        return true;
    } 

    // increment current_file:
    nzb_tree.current_file++;
    if (nzb_tree.current_file < nzb_tree.max_files) { // still valid file ?
        curFile = &nzb_tree.files[nzb_tree.current_file];   // ... get next file
        *inpFile = curFile;
        *inpSeg = curFile->segments;
        curFile->current_segment++;
        pthread_mutex_unlock(&nzb_tree_next_mutex);
        return true;
    } else {    // nah we're at the end after incrementing!
        *inpFile = NULL;
        *inpSeg = NULL;
        pthread_mutex_unlock(&nzb_tree_next_mutex);
        return false;
    }
}

/// @brief returns the binary position of the segment with the (1-based)nzbnum
/// @param file the file structure
/// @param nzbNum the nzb-number
/// @return the binary position (if nzbnum is 1 this returns 0)
unsigned int nzb_get_binary_position_of_segment (struct NZBFile *file, unsigned int nzbNum) {
    unsigned int rv = 0;

    if (nzbNum == 1)
        return 0;

    for (unsigned int i = 0; i < file->segmentsSize; i++) {
        if (file->segments[i].number == nzbNum)
            break;
        rv += file->segments[i].bytes;
    }
        
    return rv;
}

unsigned int nzb_get_highest_segment_number (struct NZBFile *file) {
    unsigned int rv = 0;

    for (unsigned int i = 0; i < file->segmentsSize; i++) {
        if (rv < file->segments[i].number)
            rv = file->segments[i].number;
    }

    return rv;
}

struct NZBSegment *nzb_get_segment_by_number (struct NZBFile *file, unsigned int segment_number) {
    struct NZBSegment *rv = NULL;

    for (unsigned int i = 0; i < file->segmentsSize; i++) {
        if (file->segments[i].number == segment_number) {
            rv = &file->segments[i];
            break;
        }
    }

    return rv;
}

/// @brief cleans up any resources.
/// @param  
void cleanup_xmlhandler(void) {
    for (unsigned int i = 0; i < nzb_tree.max_files; i++) {
        for (unsigned int j = 0; j < nzb_tree.files[i].segmentsSize; j++) {
            free (nzb_tree.files[i].segments[j].articleID);
        }
        free (nzb_tree.files[i].segments);
    }
    free (nzb_tree.files);
}