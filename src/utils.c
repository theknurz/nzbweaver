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
        fOut = fopen("/tmp/cnntp.log", "w");
    
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

    if (to_stderr)
        fprintf (stderr, fmtOut);

    if (fmtOut) free (fmtOut);
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
/// @return the filename - caller's the owner
char* contains_filename(const char *haystack) {
    char *rv = NULL;
    const PCRE2_SPTR8 fileQuotRegEx = (PCRE2_SPTR8)"\"(.+)\"";
    const PCRE2_SPTR8 fileReqEx = (PCRE2_SPTR8)"\\b([^\\s]+\\.\\w{3,4})\\b";
    int pcre_err;
    PCRE2_SIZE errOffset;
    static pcre2_code *comp_a = NULL, *comp_b = NULL;

    if (!comp_a || !comp_b) {
        comp_a = pcre2_compile(fileQuotRegEx, PCRE2_ZERO_TERMINATED, 0, &pcre_err, &errOffset, NULL);
        if (!comp_a)
            return NULL;
        comp_b = pcre2_compile(fileReqEx, PCRE2_ZERO_TERMINATED, 0, &pcre_err, &errOffset, NULL);
        if (!comp_b)
            return NULL;
    }

    pcre2_match_data *match_data = pcre2_match_data_create_from_pattern(comp_a, NULL);
    PCRE2_SIZE start_offset = 0;
    // 1st try:
    while (pcre2_match(comp_a, (const unsigned char*)haystack, PCRE2_ZERO_TERMINATED, start_offset, 0, match_data, NULL) >= 0) {
        PCRE2_SIZE *ovector = pcre2_get_ovector_pointer(match_data);
        rv = strndup(haystack+ovector[0], (int)(ovector[1]-ovector[0]));
        start_offset = ovector[1];
    }
    pcre2_match_data_free(match_data);
    if (rv)
        return rv;
    // 2nd try:
    match_data = pcre2_match_data_create_from_pattern(comp_b, NULL);
    start_offset = 0;
    while (pcre2_match(comp_b, (const unsigned char*)haystack, PCRE2_ZERO_TERMINATED, start_offset, 0, match_data, NULL) >= 0) {
        PCRE2_SIZE *ovector = pcre2_get_ovector_pointer(match_data);
        rv = strndup(haystack+ovector[0], (int)(ovector[1]-ovector[0]));
        start_offset = ovector[1];
    }
    pcre2_match_data_free(match_data);
    return rv;
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
    if (strlen(haystack) < strlen(needle))
        return false;

    char *entryP = &haystack[strlen(haystack)-strlen(needle)];
    if(strncasecmp(entryP, needle, strlen(needle)) == 0)    
        return true;

    return false;
}