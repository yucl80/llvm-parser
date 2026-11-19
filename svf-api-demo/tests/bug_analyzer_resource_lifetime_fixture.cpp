#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <dirent.h>
#include <dlfcn.h>
#include <memory>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

void *global_resource;
int resource_sink;

void malloc_leak() {
    void *resource = malloc(32);
    resource_sink += resource != nullptr;
}

void calloc_leak() {
    void *resource = calloc(4, 16);
    resource_sink += resource != nullptr;
}

void strdup_leak() {
    char *resource = strdup("owned string");
    resource_sink += resource != nullptr;
}

void realloc_null_result_leak() {
    void *resource = realloc(nullptr, 64);
    resource_sink += resource != nullptr;
}

void reallocarray_leak() {
    void *resource = reallocarray(nullptr, 4, 16);
    resource_sink += resource != nullptr;
}

void scalar_new_leak() {
    int *resource = new int(7);
    resource_sink += *resource;
}

void array_new_leak() {
    int *resource = new int[4]{1, 2, 3, 4};
    resource_sink += resource[0];
}

void conditional_path_leak(bool allocate) {
    if (allocate) {
        void *resource = malloc(24);
        resource_sink += resource != nullptr;
    }
}

void early_return_leak(bool return_early) {
    void *resource = malloc(24);
    if (return_early)
        return;
    free(resource);
}

void overwrite_and_release() {
    void *resource = malloc(16);
    resource = malloc(32);
    free(resource);
}

void malloc_delete_mismatch() {
    delete static_cast<int *>(malloc(sizeof(int)));
}

void new_free_mismatch() {
    int *resource = new int;
    free(resource);
}

void file_release_mismatch() {
    FILE *resource = popen("printf data", "r");
    fclose(resource);
}

void double_release() {
    void *resource = malloc(32);
    free(resource);
    free(resource);
}

void overwrite_in_loop_then_release(int count) {
    void *resource = nullptr;
    for (int i = 0; i < count; ++i)
        resource = malloc(16);
    free(resource);
}

void checked_realloc_cleanup() {
    void *resource = malloc(32);
    void *resized = realloc(resource, 64);
    if (resized)
        free(resized);
    else
        free(resource);
}

void *allocate_resource_wrapper() {
    return malloc(32);
}

void wrapped_resource_leak() {
    void *resource = allocate_resource_wrapper();
    resource_sink += resource != nullptr;
}

void release_resource_wrapper(void *resource) {
    free(resource);
}

void release_resource_wrapper_with_branch(void *resource, bool observe) {
    if (observe)
        resource_sink += resource != nullptr;
    free(resource);
}

void conditional_release_wrapper(void *resource, bool release) {
    if (release)
        free(resource);
}

void matching_release_wrapper() {
    release_resource_wrapper(malloc(32));
}

void matching_release_wrapper_with_branch() {
    release_resource_wrapper_with_branch(malloc(32), true);
}

void conditional_release_wrapper_leak() {
    conditional_release_wrapper(malloc(32), false);
}

int acquire_descriptor_wrapper() {
    const int descriptor = open("input.txt", O_RDONLY);
    if (descriptor < 0)
        return -1;
    return descriptor;
}

void wrapped_descriptor_leak() {
    const int descriptor = acquire_descriptor_wrapper();
    resource_sink += descriptor;
}

void transfer_resource_to_global() {
    global_resource = malloc(32);
}

void returned_resource_is_transferred() {
    global_resource = malloc(32);
}

void *return_resource() {
    return allocate_resource_wrapper();
}

void file_stream_leak() {
    FILE *resource = fopen("input.txt", "r");
    resource_sink += resource != nullptr;
}

void temporary_file_leak() {
    FILE *resource = tmpfile();
    resource_sink += resource != nullptr;
}

void pipe_stream_leak() {
    FILE *resource = popen("printf data", "r");
    resource_sink += resource != nullptr;
}

void file_descriptor_leak() {
    int resource = open("input.txt", O_RDONLY);
    resource_sink += resource;
}

void openat_descriptor_leak() {
    int resource = openat(AT_FDCWD, "input.txt", O_RDONLY);
    resource_sink += resource;
}

void created_file_descriptor_leak() {
    int resource = creat("output.txt", 0600);
    resource_sink += resource;
}

void socket_leak() {
    int resource = socket(AF_INET, SOCK_STREAM, 0);
    resource_sink += resource >= 0;
}

void duplicated_descriptor_leak(int original) {
    const int duplicate = dup(original);
    resource_sink += duplicate;
}

void accepted_socket_leak(int listener) {
    sockaddr_storage peer{};
    socklen_t peer_size = sizeof(peer);
    const int connection = accept(
        listener, reinterpret_cast<sockaddr *>(&peer), &peer_size);
    resource_sink += connection >= 0;
}

void matching_malloc_free() {
    free(malloc(32));
}

void matching_new_delete() {
    int *resource = new int(1);
    delete resource;
}

void matching_new_array_delete() {
    int *resource = new int[4]{};
    delete[] resource;
}

void matching_file_close() {
    fclose(fopen("input.txt", "r"));
}

void matching_temporary_file_close() {
    fclose(tmpfile());
}

void matching_pipe_close() {
    pclose(popen("printf data", "r"));
}

void matching_descriptor_close() {
    close(open("input.txt", 0));
}

void matching_socket_close() {
    close(socket(AF_INET, SOCK_STREAM, 0));
}

void matching_fdopen_close() {
    const int descriptor = open("input.txt", O_RDONLY);
    FILE *stream = fdopen(descriptor, "r");
    if (stream)
        fclose(stream);
    else
        close(descriptor);
}

void unique_ptr_releases_allocation() {
    std::unique_ptr<int> value(new int(7));
    resource_sink += *value;
}

void release_each_loop_iteration(int count) {
    for (int i = 0; i < count; ++i)
        free(malloc(16));
}

void mmap_leak() {
    void *resource = mmap(nullptr, 4096, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    resource_sink += resource != MAP_FAILED;
}

void directory_stream_leak() {
    DIR *directory = opendir(".");
    resource_sink += directory != nullptr;
}

void dynamic_library_leak() {
    void *library = dlopen("missing-library.so", RTLD_LAZY);
    resource_sink += library != nullptr;
}

void mkstemp_descriptor_leak() {
    char name[] = "/tmp/resource-lifetime-XXXXXX";
    const int descriptor = mkstemp(name);
    resource_sink += descriptor;
}

void indirect_acquisition_leak() {
    char *(*duplicate)(const char *) = strdup;
    char *resource = duplicate("indirect resource");
    resource_sink += resource != nullptr;
}

void indirect_release_wrapper_free(void *resource) {
    free(resource);
}

void indirect_release_matching() {
    void (*release)(void *) = indirect_release_wrapper_free;
    release(malloc(32));
}

void matching_mmap_unmap() {
    void *resource = mmap(nullptr, 4096, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (resource != MAP_FAILED)
        munmap(resource, 4096);
}

void matching_directory_close() {
    closedir(opendir("."));
}

void matching_dynamic_library_close() {
    dlclose(dlopen("missing-library.so", RTLD_LAZY));
}

void cross_block_release_each_iteration(int count) {
    for (int i = 0; i < count; ++i) {
        void *resource = malloc(16);
        resource_sink += resource != nullptr;
        free(resource);
    }
}

int main() {
    malloc_leak();
    calloc_leak();
    strdup_leak();
    realloc_null_result_leak();
    reallocarray_leak();
    scalar_new_leak();
    array_new_leak();
    conditional_path_leak(true);
    early_return_leak(true);
    overwrite_and_release();
    malloc_delete_mismatch();
    new_free_mismatch();
    file_release_mismatch();
    double_release();
    overwrite_in_loop_then_release(2);
    checked_realloc_cleanup();
    wrapped_resource_leak();
    matching_release_wrapper();
    matching_release_wrapper_with_branch();
    conditional_release_wrapper_leak();
    wrapped_descriptor_leak();
    returned_resource_is_transferred();
    transfer_resource_to_global();
    (void)return_resource();
    file_stream_leak();
    temporary_file_leak();
    pipe_stream_leak();
    file_descriptor_leak();
    openat_descriptor_leak();
    created_file_descriptor_leak();
    socket_leak();
    duplicated_descriptor_leak(STDIN_FILENO);
    matching_malloc_free();
    matching_new_delete();
    matching_new_array_delete();
    matching_file_close();
    matching_temporary_file_close();
    matching_pipe_close();
    matching_descriptor_close();
    matching_socket_close();
    matching_fdopen_close();
    unique_ptr_releases_allocation();
    release_each_loop_iteration(2);
    mmap_leak();
    directory_stream_leak();
    dynamic_library_leak();
    mkstemp_descriptor_leak();
    indirect_acquisition_leak();
    indirect_release_matching();
    matching_mmap_unmap();
    matching_directory_close();
    matching_dynamic_library_close();
    cross_block_release_each_iteration(2);
    return resource_sink;
}
