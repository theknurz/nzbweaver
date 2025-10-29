#include "parfiles.h"

// the magic sequence 
const unsigned char magic_sequence_check[]={"PAR2\0PKT"};
const unsigned char magic_filepacket_check[]={"PAR 2.0\0FileDesc"};
// the filenames:
char **par_filenames = NULL;
unsigned int par_filenames_length = 0;

/*
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
bool get_par2_filenames(char* filename) {
    uint64_t data_pos = 0, data_size;
    char    *parname;
    unsigned int filenameSize = 0;
    struct par_header header;
    struct par_file   file;
    FILE*   io_par2;

    io_par2 = fopen(filename, "rb");
    if (!io_par2)
        return false;

    fseek(io_par2, 0, SEEK_END);
    data_size = ftell(io_par2);
    fseek(io_par2, 0, SEEK_SET);

    while (data_pos < data_size) {
        fread(&header, sizeof(struct par_header), 1, io_par2);
        if (!compare_par2_fields(header.magic_sequence, magic_sequence_check, 8))
            return false;   // is it valid ?
        // we only want filedescription packets.
        if (!compare_par2_fields(header.type, magic_filepacket_check, 16)) {
            fseek(io_par2, header.packet_length-sizeof(struct par_header), SEEK_CUR);
            data_pos += (header.packet_length - sizeof(struct par_header));  // skip this packet it's not the one we want.
            continue;
        }
        // we have filedescription here, reserve some space:
        par_filenames = (char**)realloc(par_filenames, sizeof(char*) * (par_filenames_length+1));
        fread(&file, sizeof(struct par_file), 1, io_par2);
        // file pointer is now at the beginning of the string:
        filenameSize = header.packet_length - (sizeof(struct par_header) + sizeof(struct par_file));
        parname = (char*)calloc(filenameSize, 1);
        fread(parname, 1, filenameSize, io_par2);
        par_filenames[par_filenames_length] = parname;
        data_pos += header.packet_length;  // jump to next packet
        par_filenames_length++; // increase array counter
    }
    fclose (io_par2);

    return true;
}

bool compare_par2_fields(uint8_t *parType, const unsigned char *typeCheck, uint8_t len) {
    for (uint8_t i = 0; i < len; i++) {
        if (parType[i] != (uint8_t)typeCheck[i])
            return false;
    }
    return true;
}

bool is_par2_list(char* filename) {
    for (unsigned int i = 0; i < par_filenames_length; i++) {
        if (strcmp(filename, par_filenames[i]) == 0)
            return true;
    }
    return false;
}