#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <sys/socket.h>
#include <unistd.h>

namespace fake_api {
void system(const char *) {}
int printf(const char *, ...) { return 0; }
}

char *copy(char *destination, const char *source) {
    return strcpy(destination, source);
}

void indirect_strcpy(char *destination, const char *source) {
    char *(*copy_function)(char *, const char *) = strcpy;
    copy_function(destination, source);
}

void safe_literal_copy() {
    char destination[8];
    strcpy(destination, "ok");
}

void safe_local_literal_copy() {
    char destination[8];
    const char *source = "ok";
    strcpy(destination, source);
}

void exact_fit_literal_copy() {
    char destination[3];
    strcpy(destination, "ok");
}

void undersized_literal_copy() {
    char destination[2];
    strcpy(destination, "ok");
}

void wide_copy(wchar_t *destination, const wchar_t *source) {
    wcscpy(destination, source);
}

void wide_concat(wchar_t *destination, const wchar_t *source) {
    wcscat(destination, source);
}

void narrow_concat(char *destination, const char *source) {
    strcat(destination, source);
}

void current_directory(char *buffer) {
    getwd(buffer);
}

void insecure_temp_names(char *template_name) {
    mktemp(template_name);
    tmpnam(nullptr);
    tempnam(nullptr, "prefix");
}

void dynamic_format(const char *format, int value) {
    printf(format, value);
}

void safe_local_literal_format(int value) {
    const char *format = "%d";
    printf(format, value);
}

void dynamic_file_format(const char *format, int value) {
    fprintf(stderr, format, value);
}

void namespaced_non_api_calls(const char *value) {
    fake_api::system(value);
    fake_api::printf(value);
}

void constant_format(char *buffer, unsigned long size, int value) {
    snprintf(buffer, size, "value=%d", value);
}

void safe_snprintf() {
    char buffer[16];
    snprintf(buffer, sizeof(buffer), "value=%d", 3);
}

void oversized_snprintf() {
    char buffer[8];
    snprintf(buffer, 16, "01234567890123456789");
}

void oversized_snprintf_size_but_short_output() {
    char buffer[8];
    snprintf(buffer, 16, "ok");
}

void memcpy_overflow() {
    char destination[8];
    char source[16] = {};
    memcpy(destination, source, sizeof(source));
}

void dynamic_memcpy_length(std::size_t length) {
    char destination[8];
    char source[16] = {};
    memcpy(destination, source, length);
}

void bounded_dynamic_memcpy(std::size_t length) {
    char destination[8];
    char source[8] = {};
    if (length <= sizeof(destination))
        memcpy(destination, source, length);
}

void oversized_dynamic_memcpy(std::size_t length) {
    char destination[8];
    char source[16] = {};
    if (length > sizeof(destination))
        memcpy(destination, source, length);
}

void exact_fit_memcpy() {
    char destination[8];
    char source[8] = {};
    memcpy(destination, source, sizeof(destination));
}

void memmove_overflow() {
    char destination[8];
    char source[16] = {};
    memmove(destination, source, sizeof(source));
}

void memset_overflow() {
    char destination[8];
    memset(destination, 0, 16);
}

void read_overflow() {
    char destination[8];
    read(0, destination, 16);
}

void recv_overflow() {
    char destination[8];
    recv(1, destination, 16, 0);
}

void dynamic_read_length(std::size_t length) {
    char destination[8];
    read(0, destination, length);
}

void bounded_dynamic_read(std::size_t length) {
    char destination[8];
    if (length <= sizeof(destination))
        read(0, destination, length);
}

void fread_overflow() {
    char destination[8];
    fread(destination, sizeof(char), 16, stdin);
}

void safe_fread() {
    char destination[8];
    fread(destination, sizeof(char), sizeof(destination), stdin);
}

void dynamic_fread_count(std::size_t count) {
    char destination[8];
    fread(destination, sizeof(char), count, stdin);
}

void bounded_dynamic_fread(std::size_t count) {
    char destination[8];
    if (count <= sizeof(destination))
        fread(destination, sizeof(char), count, stdin);
}

void dynamic_snprintf_size(std::size_t size, const char *value) {
    char destination[8];
    snprintf(destination, size, "%s", value);
}

void bounded_dynamic_snprintf(std::size_t size, const char *value) {
    char destination[8];
    if (size <= sizeof(destination))
        snprintf(destination, size, "%s", value);
}

void safe_strncpy(const char *source) {
    char destination[8];
    strncpy(destination, source, sizeof(destination));
}

void bounded_dynamic_strncpy(const char *source, std::size_t count) {
    char destination[8];
    if (count <= sizeof(destination))
        strncpy(destination, source, count);
}

void bounded_copy_with_oversized_count(const char *source) {
    char destination[8];
    strncpy(destination, source, 16);
}

void bounded_concat_with_oversized_count(const char *source) {
    char destination[8] = "x";
    strncat(destination, source, 16);
}

void memccpy_overflow() {
    char destination[8];
    char source[16] = {};
    memccpy(destination, source, 0, sizeof(source));
}

void fixed_system_command() {
    system("printf fixed");
}

void safe_local_literal_command() {
    const char *command = "printf fixed";
    system(command);
}

void dynamic_system_command(const char *command) {
    system(command);
}

void fixed_popen_command() {
    popen("printf fixed", "r");
}

void dynamic_popen_command(const char *command) {
    popen(command, "r");
}

void scan_input(char *buffer) {
    scanf("%s", buffer);
}

void positional_scan_input(char *buffer) {
    scanf("%1$s", buffer);
}

void bounded_scan(char *buffer) {
    scanf("%15s", buffer);
}

void safe_bounded_scan() {
    char buffer[16];
    scanf("%15s", buffer);
}

void mixed_safe_scan_conversions() {
    char token[16];
    char field[16];
    char characters[4];
    scanf("%% %*d %15s %15[^,] %4c", token, field, characters);
}

void safe_positional_scan() {
    char first[2];
    char second[16];
    scanf("%2$15s", first, second);
}

void undersized_positional_scan() {
    char first[16];
    char second[8];
    scanf("%2$15s", first, second);
}

void safe_allocating_scan() {
    char *allocated = nullptr;
    char token[16];
    scanf("%ms %15s", &allocated, token);
}

void safe_sscanf() {
    char token[16];
    sscanf("input", "%15s", token);
}

void undersized_sscanf() {
    char token[8];
    sscanf("input", "%15s", token);
}

void unbounded_fscanf() {
    char token[8];
    fscanf(stdin, "%s", token);
}

void safe_fscanf() {
    char token[16];
    fscanf(stdin, "%15s", token);
}

void safe_single_character_scan() {
    char character;
    scanf("%c", &character);
}

void undersized_character_scan() {
    char characters[3];
    scanf("%4c", characters);
}

void undersized_bounded_scan() {
    char buffer[8];
    scanf("%15s", buffer);
}

void dynamic_scan_format(const char *format, char *buffer) {
    scanf(format, buffer);
}

void safe_local_literal_scan() {
    char buffer[8];
    const char *format = "%7s";
    scanf(format, buffer);
}

void undersized_wide_scan() {
    wchar_t buffer[4];
    scanf("%4ls", buffer);
}

void safe_scanset_containing_percent() {
    char buffer[5];
    scanf("%4[%s]", buffer);
}

void offset_memcpy_overflow() {
    char destination[16];
    char source[9] = {};
    memcpy(destination + 8, source, sizeof(source));
}
