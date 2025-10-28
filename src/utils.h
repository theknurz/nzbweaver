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

void logMessage(char *file, unsigned int line, bool to_stderr, char *fmt, ...);
const char *convert_value_to_human(uint64_t value, double *human);
char* contains_filename(const char *haystack);
void q_printf(char* fmt, ...);
bool string_ends_width(char* haystack, char* needle);

#define LOG_MESSAGE(to_stderr, fmt, ...) logMessage(__FILE__, __LINE__, to_stderr, fmt, ##__VA_ARGS__)
#endif 