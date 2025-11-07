#include "parfiles.h"

// the magic sequence 
const unsigned char magic_sequence_check[]={"PAR2\0PKT"};
const unsigned char magic_filepacket_check[]={"PAR 2.0\0FileDesc"};
const unsigned char magic_mainpacket_check[]={"PAR 2.0\0Main\0\0\0\0"};

// the filenames:
char **par_filenames = NULL;
unsigned int par_filenames_length = 0;

/*
From: https://parchive.sourceforge.net/docs/specifications/parity-volume-spec/article-spec.html#i__134603784_511

Table 87. Packet Header
Length (bytes)	Type	Description
8	byte[8]	Magic sequence. Used to quickly identify location of packets. Value = {'P', 'A', 'R', '2', '\0', 'P', 'K', 'T'} (ASCII)
8	8-byte uint	Length of the entire packet. Must be multiple of 4. (NB: Includes length of header.)
16	MD5 Hash	MD5 Hash of packet. Used as a checksum for the packet. Calculation starts at first byte of Recovery Set ID and ends at last byte of body. Does not include the magic sequence, length field or this field. NB: The MD5 Hash, by its definition, includes the length as if it were appended to the packet.
16	MD5 Hash	Recovery Set ID. All packets that belong together have the same recovery set ID. (See "main packet" for how it is calculated.)
16	byte[16]	Type. Can be anything. All beginning "PAR " (ASCII) are reserved for specification-defined packets. Application-specific packets are recommended to begin with the ASCII name of the client.
?*4	?	Body of Packet. Must be a multiple of 4 bytes.

Table 158. File Descriptor Packet Body Contents
Length (bytes)	Type	Description
16	MD5 Hash	The File ID.
16	MD5 Hash	The MD5 hash of the entire file.
16	MD5 Hash	The MD5-16k. That is, the MD5 hash of the first 16kB of the file.
8	8-byte uint	Length of the file.
?*4	ASCII char array	Name of the file. This array is not guaranteed to be null terminated! Subdirectories are indicated by an HTML-style '/' (a.k.a. the UNIX slash). The filename must be unique.
*/

/// @brief fills the par_filenames array w. the filenames from par2 file.
/// @param filename the filename of the PAR2 to parse.
/// @return true if everything was ok.
bool get_par2_filenames(struct NZBFile *filename, bool *isMain) {
    FILE*   io_par2;
    char*   buffer = NULL;
    size_t  buffersize = 0;
    extern struct NZB nzb_tree;
    bool    rv;

    if (par_filenames)
        return true;    // list was populated before.
    
    for (unsigned int segCnt = 0; segCnt < filename->segmentsSize; segCnt++) {
        char *fullfilepath = mprintfv("%s/%s", nzb_tree.download_destination, filename->segments[segCnt].articleID);
        size_t curFileSize = 0, readFileSize = 0;

        io_par2 = fopen64(fullfilepath, "r");
        if (!io_par2) {
            free (fullfilepath);
            continue;
        }
        
        fseek(io_par2, 0, SEEK_END);
        curFileSize = ftello64(io_par2);
        fseek(io_par2, 0, SEEK_SET);
        buffer = (char*)realloc(buffer, buffersize+curFileSize);
        readFileSize = fread(&buffer[buffersize], 1, curFileSize, io_par2);
        if (readFileSize != curFileSize)
            LOG_MESSAGE(false, "Warning reading PAR2 File: %s", fullfilepath);
        fclose(io_par2);
        buffersize += curFileSize;
        free (fullfilepath);
    }

    rv = get_par2_filenames_from_memory(buffer, buffersize, isMain);
    free (buffer);
    return rv;
}

/// @brief fills the par_filenames array w. the filenames from pointer to par2-memory
/// @param parmem the pointer to memory
/// @param parmemsize how big parmem is.
/// @return true if everything was ok.
bool get_par2_filenames_from_memory(char *parmem, size_t parmemsize, bool *isMain) {
    uint64_t data_pos = 0;
    char    *parname;
    struct par_header *header;
    struct par_file   *file;

    (void)file;

    if (par_filenames)
        return true;    // list was populated before.

    *isMain = false;

    while (data_pos < parmemsize) {
        header = (struct par_header*)&parmem[data_pos];
        file = (struct par_file*)&parmem[data_pos+sizeof(struct par_header)];
        if (!compare_par2_fields(header->magic_sequence, magic_sequence_check, 8))
            return false;   // is it valid ?
        if (compare_par2_fields(header->type, magic_mainpacket_check, 16))
            *isMain = true;
        // we only want filedescription packets.
        if (!compare_par2_fields(header->type, magic_filepacket_check, 16)) {
            data_pos += header->packet_length;  // skip this packet it's not the one we want.
            continue;
        }
        // we have filedescription here, reserve some space:
        par_filenames = (char**)realloc(par_filenames, sizeof(char*) * (par_filenames_length+1));
        parname = strndup(&parmem[data_pos+sizeof(struct par_header)+sizeof(struct par_file)], 
                        header->packet_length-(sizeof(struct par_header)+sizeof(struct par_file)));
        par_filenames[par_filenames_length] = parname;
        data_pos += header->packet_length;  // jump to next packet
        par_filenames_length++; // increase array counter
    }

    return true;
}

/// @brief this is for the sole use to check a string containing a \0
/// @param parType 
/// @param typeCheck 
/// @param len 
/// @return 
bool compare_par2_fields(uint8_t *parType, const unsigned char *typeCheck, uint8_t len) {
    for (uint8_t i = 0; i < len; i++) {
        if (parType[i] != (uint8_t)typeCheck[i])
            return false;
    }
    return true;
}

/// @brief if filename is in our list.
/// @param filename 
/// @return 
bool is_par2_list(char* filename) {
    for (unsigned int i = 0; i < par_filenames_length; i++) {
        char *fnd = strstr(filename, par_filenames[i]);
        if (fnd) return true;
//        if (strcmp(filename, par_filenames[i]) == 0)
//            return true;
    }
    return false;
}
