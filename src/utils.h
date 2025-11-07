#ifndef LOG_H
#define LOG_H
#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <pcre2.h>
#include <string.h>
#include <errno.h>

#define LOG_MESSAGE(to_stderr, fmt, ...) logMessage(__FILE__, __LINE__, to_stderr, fmt, ##__VA_ARGS__)

char* mprintfv(char *fmt, ...);
void logMessage(char *file, unsigned int line, bool to_stderr, char *fmt, ...);
const char *convert_value_to_human(uint64_t value, double *human);
bool contains_filename(const char *haystack, char **dupstr);
void q_printf(char* fmt, ...);
bool string_ends_width(char* haystack, char* needle);
bool write_to_file(char* filepath, uint8_t *buffer, uint64_t size);
#endif 