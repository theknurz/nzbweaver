/*
 * debug/helper stuff
 */
#include "utils.h"

const char *fmtFile = "%s %s%s";

const uint8_t value_conversion_table_size = 5;
const char *value_conversion_table[] = {
    "B", "KB", "MB", "GB", "TB"
};

/// @brief Logs a Message w. filenumber/linenumber - NOT! 
/// @param file __FILE__ for GCC
/// @param line __LINE__ for GCC
/// @param to_stderr 
/// @param fmt 
/// @param  
void logMessage(char *file, unsigned int line, bool to_stderr, char *fmt, ...) {
    static FILE    *fOut = NULL;
    char fmtTime[256], *fmtOut = NULL, *fmtCaller = NULL;
    size_t outLen = 0;
    time_t tnow = time(NULL);
    struct tm *restTm = localtime(&tnow);
    va_list ap;
    char lfChk[2] = { 0 };

    (void)file;
    (void)line;

    if (!fOut)
        fOut = fopen("/tmp/nzbweaver.log", "w");
    
    strftime(fmtTime, 255, "[%T] # ", restTm);      // # as seperator ? kinda meh...

    // format the user's args:
    va_start(ap, fmt);
    outLen = vsnprintf(NULL, 0, fmt, ap);
    fmtCaller = (char*)calloc(outLen+2, 1);
    va_end(ap);

    va_start(ap, fmt);
    vsprintf(fmtCaller, fmt, ap);
    va_end(ap);

    // LF check:
    if (fmt[strlen(fmt)-1] != '\n')
        lfChk[0] = '\n';

    // now, format our string for the output w. blackjack and hookers!
    outLen = snprintf(NULL, 0, fmtFile, fmtTime, fmtCaller, lfChk);
    fmtOut = (char*)calloc(outLen+2, 1);
    sprintf(fmtOut, fmtFile, fmtTime, fmtCaller, lfChk);

    if (fOut) {
        fprintf(fOut, fmtOut);
        fflush(fOut);
    }

#ifndef NDEBUG
    if (to_stderr)
        fprintf (stderr, fmtOut);
#endif

    if (fmtOut) free (fmtOut);
    if (fmtCaller) free (fmtCaller);
}

/// @brief converts to kb/mb/..
/// @param value 
/// @param human 
/// @return pointer to kb/mb/string - DONT FREE
const char *convert_value_to_human(uint64_t value, double *human) {
    uint8_t idx = 0;
    double hrVal = (double)value;

    while (idx < value_conversion_table_size) {
        if (hrVal < 1024.) {
            *human = hrVal;
            return value_conversion_table[idx];
        }
        hrVal /= 1024.;
        idx++;
    }

    // return bytes again!
    *human = (double)value;
    return value_conversion_table[0];
}

/// @brief searches for filename in haystack
/// @param haystack 
/// @param dupstr - if !NULL, found string will be strndup()ed.
/// @return true if a filename was found, false if no filename was found
bool contains_filename(const char *haystack, char **dupstr) {
    // (["?].+\..+["?])|([\S]+\.[\S]+)|(.+\.[0-9]{3,})\s
    const PCRE2_SPTR8 fileExtEnd = (PCRE2_SPTR8)"(\\.\\w+[\\s|\"])";
    static pcre2_code *comp_a = NULL;
    static pcre2_match_data *match_data = NULL;
    int pcre_err;
    PCRE2_SIZE errOffset;
    char* fname_start;

    if (!comp_a) {
        comp_a = pcre2_compile(fileExtEnd, PCRE2_ZERO_TERMINATED, 0, &pcre_err, &errOffset, NULL);
        if (!comp_a)
            return false;
        match_data = pcre2_match_data_create_from_pattern(comp_a, NULL);
    }

    if (pcre2_match(comp_a, (const unsigned char*)haystack, PCRE2_ZERO_TERMINATED, 0, 0, match_data, NULL) >= 0) {
        // .ext found!
        PCRE2_SIZE *ovector = pcre2_get_ovector_pointer(match_data);

        if (haystack[ovector[1]-1] == '"') {
            fname_start = strchr(haystack, '"');
            if (!fname_start)
                fname_start = (char*)haystack;
            else
                fname_start++;
        } else {
            fname_start = (char*)haystack;
        }

        if (dupstr) 
            *dupstr = strndup(fname_start, (int)(&haystack[ovector[1]]-fname_start)-1 );
        return true;
    }
    return false;
}

/// @brief a print that respects the quiet flag
/// @param fmt 
/// @param  
void q_printf(char* fmt, ...) {
    extern bool quiet_output;
    va_list ap;

    if (quiet_output)
        return;

    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}

bool string_ends_width(char* haystack, char* needle) {
    if (!haystack || !needle)
        return false;
        
    if (strlen(haystack) < strlen(needle))
        return false;

    char *entryP = &haystack[strlen(haystack)-strlen(needle)];
    if(strncasecmp(entryP, needle, strlen(needle)) == 0)    
        return true;

    return false;
}

/// @brief writes memory to a file - w. errorchecks.
/// @param filepath 
/// @param size 
/// @return 
bool write_to_file(char* filepath, uint8_t *buffer, uint64_t size) {
    uint64_t written = 0;
    FILE*   f_write = NULL;

    f_write = fopen(filepath, "wb");
    if (!f_write) {
        LOG_MESSAGE(true, "Could not write %s to disk, errno (%i), %s", filepath, errno, strerror(errno));
        return false;
    }
    
    written = fwrite(buffer, 1, size, f_write);
    if (written != size) {
        LOG_MESSAGE(true, "Only wrote %d out of %d to file %s, errno (%i), %s", written, size, filepath, errno, strerror(errno));
        fclose (f_write);
        return false;
    }

    fflush(f_write);
    fclose(f_write);
    return true;
}

/// @brief allocs and sprintf(...)
/// @param fmt the Format.
/// @param the VA_LIST
/// @return pointer to memory, OWNED BY CALLER.
char* mprintfv(char *fmt, ...) {
    va_list ap;
    size_t len = 0;
    char *rv = NULL;

    va_start(ap, fmt);
    len = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);

    if (!len)
        return NULL;

    rv = (char*)calloc(len+2, 1);
    va_start (ap, fmt);
    vsprintf(rv, fmt, ap);
    va_end(ap);

    return rv;
    
}
