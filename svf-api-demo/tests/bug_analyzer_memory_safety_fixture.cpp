#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/socket.h>
#include <unistd.h>

struct Record {
    int values[2];
};

struct FlexibleRecord {
    int length;
    char data[0];
};

namespace custom {
void free(void *) {}
}

int null_pointer_read() {
    int *pointer = nullptr;
    return *pointer;
}

void null_pointer_write() {
    *static_cast<volatile int *>(nullptr) = 1;
}

int null_pointer_read_through_alias() {
    int *pointer = nullptr;
    int *alias = pointer;
    return *alias;
}

int null_pointer_on_guarded_branch(int *pointer) {
    if (pointer == nullptr)
        return *pointer;
    return 0;
}

int divide_by_zero(int value) {
    return value / 0;
}

unsigned int remainder_by_zero(unsigned int value) {
    return value % 0;
}

int divide_by_input(int numerator, int denominator) {
    return numerator / denominator;
}

int out_of_bounds_read() {
    int values[3] = {};
    return values[3];
}

int out_of_bounds_negative_index() {
    int values[3] = {};
    return values[-1];
}

int unreachable_out_of_bounds_label() {
    goto exit;
unreachable:
    {
        int values[1];
        return values[1];
    }
exit:
    return 0;
}

void out_of_bounds_write() {
    int values[3] = {};
    values[3] = 1;
}

int out_of_bounds_nested_array() {
    int values[2][3] = {};
    return values[1][3];
}

int out_of_bounds_struct_member() {
    Record record{};
    return record.values[2];
}

int out_of_bounds_pointer_read() {
    int values[3] = {};
    int *cursor = values;
    return cursor[3];
}

int out_of_bounds_pointer_before_base() {
    int values[3] = {};
    int *cursor = values;
    return cursor[-1];
}

void out_of_bounds_pointer_write() {
    int values[3] = {};
    int *cursor = values;
    cursor[3] = 1;
}

int out_of_bounds_heap_read() {
    int *values = new int[2]{};
    return *(values + 2);
}

void flexible_array_within_allocated_extent() {
    auto *record = static_cast<FlexibleRecord *>(
        malloc(sizeof(FlexibleRecord) + 8));
    record->data[4] = 'x';
    free(record);
}

int in_bounds_read() {
    int values[3] = {};
    return values[2];
}

void in_bounds_write() {
    int values[3] = {};
    values[2] = 1;
}

int in_bounds_pointer_read() {
    int values[3] = {};
    int *cursor = values;
    return cursor[2];
}

int in_bounds_negative_pointer_offset() {
    int values[3] = {};
    int *cursor = &values[1];
    return cursor[-1];
}

int *return_stack_address() {
    int local = 1;
    return &local;
}

char *return_stack_array() {
    char buffer[8] = {};
    return buffer;
}

int use_after_free_read() {
    int *pointer = static_cast<int *>(malloc(sizeof(int)));
    free(pointer);
    return *pointer;
}

void use_after_free_write() {
    int *pointer = static_cast<int *>(malloc(sizeof(int)));
    free(pointer);
    *pointer = 1;
}

int use_after_free_through_alias() {
    int *pointer = static_cast<int *>(malloc(sizeof(int)));
    int *alias = pointer;
    free(pointer);
    return *alias;
}

int use_after_free_across_branch(bool return_early) {
    int *pointer = static_cast<int *>(malloc(sizeof(int)));
    free(pointer);
    if (return_early)
        return 0;
    return *pointer;
}

int use_after_free_on_conditional_path(bool release) {
    int *pointer = static_cast<int *>(malloc(sizeof(int)));
    if (release)
        free(pointer);
    return *pointer;
}

void double_free_across_branch(bool release_twice) {
    int *pointer = static_cast<int *>(malloc(sizeof(int)));
    free(pointer);
    if (release_twice)
        free(pointer);
}

void use_after_free_atomic() {
    int *pointer = static_cast<int *>(malloc(sizeof(int)));
    free(pointer);
    __atomic_fetch_add(pointer, 1, __ATOMIC_SEQ_CST);
}

int use_after_free_through_select(bool choose_freed) {
    int *freed = static_cast<int *>(malloc(sizeof(int)));
    int live = 0;
    int *pointer = choose_freed ? freed : &live;
    free(freed);
    return *pointer;
}

void out_of_bounds_atomic_rmw() {
    int values[1] = {};
    __atomic_fetch_add(values + 1, 1, __ATOMIC_SEQ_CST);
}

void out_of_bounds_atomic_cmpxchg() {
    int values[1] = {};
    int expected = 0;
    __atomic_compare_exchange_n(values + 1, &expected, 1, false,
                                __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}

int *return_freed_pointer() {
    int *pointer = static_cast<int *>(malloc(sizeof(int)));
    free(pointer);
    return pointer;
}

void double_free_malloc() {
    void *pointer = malloc(16);
    free(pointer);
    free(pointer);
}

void double_delete() {
    int *pointer = new int;
    delete pointer;
    delete pointer;
}

void free_null_pointer() {
    free(nullptr);
}

void matching_free() {
    void *pointer = malloc(16);
    free(pointer);
}

void namespaced_free_is_not_deallocator() {
    int *pointer = static_cast<int *>(malloc(sizeof(int)));
    custom::free(pointer);
    *pointer = 1;
    free(pointer);
}

int safe_malloc_after_null_check() {
    int *pointer = static_cast<int *>(malloc(sizeof(int)));
    if (!pointer)
        return 0;
    const int value = *pointer;
    free(pointer);
    return value;
}

void memcpy_overflow() {
    char destination[8];
    char source[16] = {};
    memcpy(destination, source, sizeof(source));
}

void offset_memcpy_overflow() {
    char destination[16];
    char source[9] = {};
    memcpy(destination + 8, source, sizeof(source));
}

void memmove_overflow() {
    char destination[8];
    char source[16] = {};
    memmove(destination, source, sizeof(source));
}

void read_overflow() {
    char destination[8];
    read(0, destination, 16);
}

void recv_overflow() {
    char destination[8];
    recv(0, destination, 16, 0);
}

void snprintf_overflow() {
    char destination[8];
    snprintf(destination, 16, "%s", "text");
}

void unbounded_literal_copy() {
    char destination[2];
    strcpy(destination, "too long");
}

void exact_fit_memcpy() {
    char destination[8];
    char source[8] = {};
    memcpy(destination, source, sizeof(destination));
}

void safe_snprintf() {
    char destination[16];
    snprintf(destination, sizeof(destination), "%s", "text");
}

void safe_literal_copy() {
    char destination[16];
    strcpy(destination, "text");
}

int main() {
    return 0;
}
