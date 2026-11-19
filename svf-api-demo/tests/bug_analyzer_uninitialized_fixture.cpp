#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <cstdint>

struct PartialValue {
    int initialized;
    int untouched;
};

struct NestedValue {
    int first;
    int second;
};

struct OuterValue {
    NestedValue nested;
    int tail;
};

struct Pair {
    int first;
    int second;
};

int global_value;
void retain_pointer(int *pointer);

// --- uninitialized stack reads ------------------------------------------------

int read_uninitialized_scalar() {
    int value;
    return value;
}

long long read_uninitialized_wide_scalar() {
    long long value;
    return value;
}

bool read_uninitialized_bool() {
    bool flag;
    return flag;
}

float read_uninitialized_float() {
    float value;
    return value;
}

char read_uninitialized_array_element() {
    char buffer[4];
    return buffer[2];
}

int read_uninitialized_struct_field() {
    PartialValue value;
    value.initialized = 1;
    return value.untouched;
}

int read_uninitialized_nested_struct_field() {
    OuterValue value;
    value.nested.first = 1;
    return value.nested.second;
}

int read_uninitialized_trailing_struct_field() {
    OuterValue value;
    value.nested.first = 1;
    value.nested.second = 2;
    return value.tail;
}

int read_through_pointer_alias() {
    int value;
    int *pointer = &value;
    return *pointer;
}

int read_multiple_uninitialized_locals() {
    int first;
    int second;
    return first + second;
}

// --- initialized stack reads --------------------------------------------------

int read_initialized_scalar() {
    int value = 17;
    return value;
}

char read_initialized_array_element() {
    char buffer[4] = {};
    return buffer[2];
}

int read_initialized_struct_field() {
    PartialValue value;
    value.initialized = 1;
    value.untouched = 2;
    return value.untouched;
}

int read_after_zero_initialized_struct() {
    PartialValue value{};
    return value.untouched;
}

int read_after_whole_struct_assignment() {
    Pair source;
    source.first = 1;
    source.second = 2;
    Pair destination;
    destination = source;
    return destination.second;
}

int read_after_partial_struct_assignment() {
    PartialValue source;
    source.initialized = 1;
    PartialValue destination;
    destination = source;
    return destination.untouched;
}

int read_after_initialization_through_pointer_alias() {
    int value;
    int *pointer = &value;
    *pointer = 7;
    return value;
}

int read_through_pointer_before_reassignment() {
    int initialized = 19;
    int uninitialized;
    int *pointer = &initialized;
    const int result = *pointer;
    pointer = &uninitialized;
    return result;
}

int read_through_ambiguous_pointer() {
    int first;
    int second;
    int *pointer = std::rand() ? &first : &second;
    return *pointer;
}

int read_before_pointer_escape() {
    int value;
    int result = value;
    retain_pointer(&value);
    return result;
}

char read_through_external_pointer() {
    const char *path = std::getenv("PATH");
    return path ? path[0] : '\0';
}

// --- heap allocations ---------------------------------------------------------

int read_uninitialized_malloc_element() {
    int *values = static_cast<int *>(malloc(2 * sizeof(int)));
    values[0] = 7;
    return values[1];
}

int read_initialized_malloc_element() {
    int *values = static_cast<int *>(malloc(2 * sizeof(int)));
    values[0] = 7;
    return values[0];
}

int read_calloc_zero_initialized_element() {
    int *values = static_cast<int *>(calloc(2, sizeof(int)));
    return values[1];
}

int read_uninitialized_new_scalar() {
    int *value = new int;
    return *value;
}

int read_uninitialized_new_array_element() {
    int *values = new int[3];
    values[0] = 1;
    return values[2];
}

int read_value_initialized_new_scalar() {
    int *value = new int{};
    const int result = *value;
    delete value;
    return result;
}

int read_value_initialized_new_array_element() {
    int *values = new int[3]{};
    const int result = values[2];
    delete[] values;
    return result;
}

int read_uninitialized_aligned_alloc_element() {
    int *values = static_cast<int *>(aligned_alloc(16, 2 * sizeof(int)));
    return values[1];
}

int read_dynamic_sized_malloc_is_unresolved(unsigned long count) {
    int *values = static_cast<int *>(malloc(count));
    return values[0];
}

int read_zero_sized_malloc_is_unresolved() {
    int *values = static_cast<int *>(malloc(0));
    return *values;
}

int read_realloc_result_is_unresolved() {
    int *values = static_cast<int *>(malloc(2 * sizeof(int)));
    values[0] = 1;
    int *resized = static_cast<int *>(realloc(values, 4 * sizeof(int)));
    return resized[1];
}

// --- memset / memcpy / memmove ------------------------------------------------

int read_after_memset() {
    int value;
    memset(&value, 0, sizeof(value));
    return value;
}

char read_after_partial_memset() {
    char buffer[8];
    memset(buffer, 0, 4);
    return buffer[0];
}

char read_after_partial_memset_uninitialized_tail() {
    char buffer[8];
    memset(buffer, 0, 4);
    return buffer[4];
}

int read_after_zero_length_memset() {
    int value;
    memset(&value, 0, 0);
    return value;
}

int read_after_dynamic_length_memset(unsigned long length) {
    int value;
    memset(&value, 0, length);
    return value;
}

int read_after_memset_through_pointer_alias() {
    int value;
    int *pointer = &value;
    memset(pointer, 0, sizeof(value));
    return value;
}

int read_after_memcpy_from_initialized_source() {
    int source = 9;
    int destination;
    memcpy(&destination, &source, sizeof(destination));
    return destination;
}

int read_after_memmove_from_initialized_source() {
    int source = 9;
    int destination;
    memmove(&destination, &source, sizeof(destination));
    return destination;
}

int read_after_memcpy_through_pointer_alias() {
    int source = 3;
    int destination;
    int *source_pointer = &source;
    int *destination_pointer = &destination;
    memcpy(destination_pointer, source_pointer, sizeof(int));
    return destination;
}

int copy_uninitialized_source() {
    int source;
    int destination;
    memcpy(&destination, &source, sizeof(destination));
    return 0;
}

int copy_partially_initialized_source() {
    char source[4];
    source[0] = 1;
    char destination[4];
    memcpy(destination, source, sizeof(destination));
    return 0;
}

int read_initialized_prefix_after_partial_copy() {
    char source[4];
    source[0] = 1;
    char destination[4];
    memcpy(destination, source, sizeof(destination));
    return destination[0];
}

int copy_uninitialized_heap_source() {
    int *source = static_cast<int *>(malloc(sizeof(int)));
    int destination;
    memcpy(&destination, source, sizeof(destination));
    return 0;
}

int copy_zero_length_is_noop() {
    char source[4];
    char destination[4];
    memcpy(destination, source, 0);
    return 0;
}

// --- control flow -------------------------------------------------------------

int read_after_partial_branch(bool condition) {
    int value;
    if (condition)
        value = 1;
    return value;
}

int read_after_all_paths_initialize(bool condition) {
    int value;
    if (condition)
        value = 1;
    else
        value = 2;
    return value;
}

int read_after_loop_that_may_not_run(int count) {
    int value;
    for (int i = 0; i < count; ++i)
        value = i;
    return value;
}

int read_after_loop_with_initialized_value(int count) {
    int value = 0;
    for (int i = 0; i < count; ++i)
        value = i;
    return value;
}

// --- objects the analyzer cannot reason about --------------------------------

int read_uninitialized_global() {
    return global_value;
}

int read_function_parameter(int *pointer) {
    return *pointer;
}

int read_beyond_object_is_unresolved() {
    int values[2] = {};
    int *pointer = values;
    return pointer[2];
}

int read_before_object_is_unresolved() {
    int values[2] = {};
    return *(values - 1);
}

int read_dynamic_index_is_unresolved() {
    int values[4];
    int index = std::rand() % 4;
    return values[index];
}

void *read_on_worker_thread(void *argument) {
    const int value = *static_cast<int *>(argument);
    return reinterpret_cast<void *>(static_cast<std::uintptr_t>(value));
}

int read_escaped_object_is_suppressed() {
    int value;
    pthread_t worker;
    pthread_create(&worker, nullptr, read_on_worker_thread, &value);
    pthread_join(worker, nullptr);
    return value;
}

int main() {
    bool condition = true;
    unsigned long length = 4;
    int count = 3;
    int arguments[2] = {1, 2};
    int result = 0;

    result += read_uninitialized_scalar();
    result += read_uninitialized_wide_scalar();
    result += read_uninitialized_bool();
    result += read_uninitialized_float();
    result += read_uninitialized_array_element();
    result += read_uninitialized_struct_field();
    result += read_uninitialized_nested_struct_field();
    result += read_uninitialized_trailing_struct_field();
    result += read_through_pointer_alias();
    result += read_multiple_uninitialized_locals();
    result += read_before_pointer_escape();
    result += read_initialized_scalar();
    result += read_initialized_array_element();
    result += read_initialized_struct_field();
    result += read_after_zero_initialized_struct();
    result += read_after_whole_struct_assignment();
    result += read_after_partial_struct_assignment();
    result += read_after_initialization_through_pointer_alias();
    result += read_through_ambiguous_pointer();
    result += read_through_external_pointer();
    result += read_uninitialized_malloc_element();
    result += read_initialized_malloc_element();
    result += read_calloc_zero_initialized_element();
    result += read_uninitialized_new_scalar();
    result += read_uninitialized_new_array_element();
    result += read_uninitialized_aligned_alloc_element();
    result += read_dynamic_sized_malloc_is_unresolved(length);
    result += read_zero_sized_malloc_is_unresolved();
    result += read_realloc_result_is_unresolved();
    result += read_after_memset();
    result += read_after_partial_memset();
    result += read_after_partial_memset_uninitialized_tail();
    result += read_after_zero_length_memset();
    result += read_after_dynamic_length_memset(length);
    result += read_after_memset_through_pointer_alias();
    result += read_after_memcpy_from_initialized_source();
    result += read_after_memmove_from_initialized_source();
    result += read_after_memcpy_through_pointer_alias();
    result += copy_uninitialized_source();
    result += copy_partially_initialized_source();
    result += copy_uninitialized_heap_source();
    result += copy_zero_length_is_noop();
    result += read_after_partial_branch(condition);
    result += read_after_all_paths_initialize(condition);
    result += read_after_loop_that_may_not_run(count);
    result += read_after_loop_with_initialized_value(count);
    result += read_uninitialized_global();
    result += read_function_parameter(arguments);
    result += read_beyond_object_is_unresolved();
    result += read_before_object_is_unresolved();
    result += read_dynamic_index_is_unresolved();
    result += read_escaped_object_is_suppressed();
    return result == -1;
}
