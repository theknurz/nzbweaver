#ifndef XMLHANDLER_H
#define XMLHANDLER_H
#include <stdbool.h>
#include <expat.h>

#include "utils.h"

// BEG This describes the NZB itself
struct NZBSegment {
    unsigned int    number; // the "number" property
    unsigned int    bytes;  // the "bytes" property
    unsigned int    decoded_bytes; // real(!) binary-size of this segment
    char    *articleID;
};

struct NZBFile {
    char            *filename;          // the filename the articles are part of
    char            *yenc_filename;     // the filename reported back from yBegin
    struct NZBSegment *segments;        // the articles - segments of the file
    unsigned int    segmentsSize;       // size of segments for this file.
    uint64_t        file_size;          // the reported size from the yenc-meta-entry (ybegin)
    uint64_t        download_size;      // the current downloaded size (binary)
    bool            crcOk;              // if a segment fails to load OR fails to verify via crc32
    unsigned int    remaining_segments; // for an easy check like if (!remaining_segments)... 
    unsigned int    open_segments;      // like requested but the request is not finished.
    uint64_t        joined_size;        // for progressbar to "join" all files
    uint8_t         state;              // one of the NZBFile_State
    unsigned int    current_segment;
    char            *final_filename;    // a pointer either to filename or yenc_filename
};

struct NZB {
    char            *name;              // the "release" name
    struct NZBFile  *files;             // description of files.
    unsigned int    max_files;          // size of array
    uint64_t        release_size;
    uint64_t        release_downloaded;
    char            *download_destination;
    unsigned int    current_file;        // this is a 0-based C array-index

};

enum NZBFile_State {
    NFState_Idle = 0,
    NFState_Downloading = 1,
    NFState_Joining,
    NFState_Done
};
// END 

// A simple linked list 
struct pair {
    struct pair *prev, *next;
    char *key, *value;
};

// The *userdata we'll pass to the xml-parser
struct parse_userdata {
    struct NZBFile  *file;
    struct NZBSegment *segment;
};

extern struct pair *config_server;
extern struct pair *config_downloads;

bool xml_load_config(char *filename);
char *xml_load_file(char *filename);
void pair_add(struct pair **keyval, char *key, char *value);
unsigned int pair_length(struct pair *keyval);
struct pair *pair_create(void);
void pair_destroy(struct pair **keyval);
void pair_remove(struct pair **keyval, char *needle);
char* pair_find(struct pair *keyval, char* needle);
void parse_config_start_element(void *userData, const char *name, const char **attribs);
void parse_config_end_element(void *userData, const char *name);
bool nzb_load(char *filename);
bool nzb_tree_next_segment (struct NZBFile **inpFile, struct NZBSegment **inpSeg);
unsigned int nzb_get_binary_position_of_segment (struct NZBFile *file, unsigned int nzbNum);
void cleanup_xmlhandler(void);
unsigned int nzb_get_highest_segment_number (struct NZBFile *file);
struct NZBSegment *nzb_get_segment_by_number (struct NZBFile *file, unsigned int segment_number);
#endif