#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <semaphore.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

constexpr bool kLegacyTelemetryEnabled = false;
constexpr std::size_t kCommandBufferSize = 256;

struct SharedState {
    pthread_mutex_t mutex;
    sem_t gate;
    std::uint64_t sequence;
    std::uint64_t rejected;
};

int open_local_control_socket() {
    return ::socket(AF_UNIX, SOCK_STREAM, 0);
}

int open_tcp_socket() {
    return ::socket(AF_INET, SOCK_STREAM, 0);
}

int open_udp_socket() {
    return ::socket(AF_INET, SOCK_DGRAM, 0);
}

void *map_shared_state(const char *name, std::size_t size) {
    const int descriptor = ::shm_open(name, O_CREAT | O_RDWR, 0600);
    if (descriptor < 0)
        return MAP_FAILED;
    if (::ftruncate(descriptor, static_cast<off_t>(size)) != 0) {
        ::close(descriptor);
        return MAP_FAILED;
    }
    void *region =
        ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, descriptor, 0);
    ::close(descriptor);
    return region;
}

sockaddr_in loopback_address(std::uint16_t port) {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = ::htons(port);
    (void)::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    return address;
}

ssize_t write_local_command(int descriptor, const char *payload) {
    return ::write(descriptor, payload, std::strlen(payload));
}

std::size_t read_local_command(int descriptor, char *buffer,
                               std::size_t capacity) {
    const ssize_t count = ::read(descriptor, buffer, capacity);
    return count > 0 ? static_cast<std::size_t>(count) : 0;
}

void run_local_worker(int command_fd, SharedState *state) {
    char request[kCommandBufferSize]{};
    if (read_local_command(command_fd, request, sizeof(request)) == 0)
        return;

    ::pthread_mutex_lock(&state->mutex);
    state->sequence += 1;
    ::pthread_mutex_unlock(&state->mutex);
    ::sem_post(&state->gate);
    (void)write_local_command(command_fd, "ok\n");
}

void launch_state_dumper() {
    char *arguments[] = {const_cast<char *>("llvm-parser-state-dumper"),
                         nullptr};
    (void)::execvp(arguments[0], arguments);
}

void inspect_worker_memory(pid_t child, SharedState *state) {
    std::uint64_t observed = 0;
    iovec local{&observed, sizeof(observed)};
    iovec remote{&state->sequence, sizeof(observed)};
    (void)::process_vm_readv(child, &local, 1, &remote, 1, 0);
}

void lock_state_file() {
    const int descriptor =
        ::open("/tmp/llvm-parser-cross-process.lock", O_CREAT | O_RDWR, 0600);
    if (descriptor < 0)
        return;
    ::flock kernel_lock{};
    kernel_lock.l_type = F_WRLCK;
    kernel_lock.l_whence = SEEK_SET;
    (void)::fcntl(descriptor, F_SETLK, &kernel_lock);
    ::close(descriptor);
}

void start_local_supervisor() {
    const int listener = open_local_control_socket();
    if (listener < 0)
        return;

    const char *socket_path = "/tmp/llvm-parser-cross-process.sock";
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::strncpy(address.sun_path, socket_path, sizeof(address.sun_path) - 1);
    ::unlink(socket_path);
    (void)::bind(listener, reinterpret_cast<const sockaddr *>(&address),
                 sizeof(address));
    (void)::listen(listener, 8);

    const char *region_name = "/llvm-parser-cross-process";
    void *region = map_shared_state(region_name, sizeof(SharedState));
    if (region == MAP_FAILED) {
        ::close(listener);
        return;
    }
    SharedState *state = static_cast<SharedState *>(region);

    pthread_mutexattr_t attributes;
    ::pthread_mutexattr_init(&attributes);
    ::pthread_mutexattr_setpshared(&attributes, PTHREAD_PROCESS_SHARED);
    ::pthread_mutex_init(&state->mutex, &attributes);
    ::pthread_mutexattr_destroy(&attributes);
    ::sem_init(&state->gate, 1, 0);
    lock_state_file();

    int channel[2]{-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, channel) != 0) {
        ::sem_destroy(&state->gate);
        ::pthread_mutex_destroy(&state->mutex);
        ::munmap(state, sizeof(SharedState));
        ::shm_unlink(region_name);
        ::close(listener);
        return;
    }

    const pid_t child = ::fork();
    if (child == 0) {
        ::close(listener);
        ::close(channel[0]);
        run_local_worker(channel[1], state);
        launch_state_dumper();
        ::_exit(127);
    }

    ::close(channel[1]);
    (void)write_local_command(channel[0], "refresh\n");
    char reply[kCommandBufferSize]{};
    (void)read_local_command(channel[0], reply, sizeof(reply));
    inspect_worker_memory(child, state);

    int status = 0;
    ::waitpid(child, &status, 0);
    ::sem_destroy(&state->gate);
    ::pthread_mutex_destroy(&state->mutex);
    ::munmap(state, sizeof(SharedState));
    ::shm_unlink(region_name);
    ::close(channel[0]);
    ::close(listener);
}

int connect_to_collector(std::uint16_t port) {
    const int descriptor = open_tcp_socket();
    if (descriptor < 0)
        return -1;
    const sockaddr_in collector = loopback_address(port);
    if (::connect(descriptor,
                  reinterpret_cast<const sockaddr *>(&collector),
                  sizeof(collector)) != 0) {
        ::close(descriptor);
        return -1;
    }
    return descriptor;
}

ssize_t send_telemetry(int descriptor, const char *line) {
    return ::send(descriptor, line, std::strlen(line), 0);
}

void report_telemetry(std::uint16_t port) {
    const int descriptor = connect_to_collector(port);
    if (descriptor < 0)
        return;
    (void)send_telemetry(descriptor, "{\"event\":\"cache-refresh\"}\n");
    char acknowledgement[64]{};
    (void)::recv(descriptor, acknowledgement, sizeof(acknowledgement), 0);
    (void)::shutdown(descriptor, SHUT_RDWR);
    ::close(descriptor);
}

void publish_udp_heartbeat(std::uint16_t port) {
    const int descriptor = open_udp_socket();
    if (descriptor < 0)
        return;
    const sockaddr_in collector = loopback_address(port);
    timeval receive_timeout{0, 200000};
    (void)::setsockopt(descriptor, SOL_SOCKET, SO_RCVTIMEO, &receive_timeout,
                       sizeof(receive_timeout));
    const char heartbeat[] = "heartbeat";
    (void)::sendto(descriptor, heartbeat, sizeof(heartbeat), 0,
                   reinterpret_cast<const sockaddr *>(&collector),
                   sizeof(collector));
    char buffer[64]{};
    sockaddr_in sender{};
    socklen_t sender_length = sizeof(sender);
    (void)::recvfrom(descriptor, buffer, sizeof(buffer), 0,
                     reinterpret_cast<sockaddr *>(&sender), &sender_length);
    ::close(descriptor);
}

using PayloadSender = ssize_t (*)(int, const void *, std::size_t, int);

ssize_t dispatch_payload(int descriptor, const void *payload, std::size_t size,
                         PayloadSender sender) {
    return sender(descriptor, payload, size, 0);
}

void report_through_dispatch_table(std::uint16_t port) {
    const int descriptor = connect_to_collector(port);
    if (descriptor < 0)
        return;
    PayloadSender sender = &::send;
    (void)dispatch_payload(descriptor, "dispatch", 8, sender);
    ::close(descriptor);
}

void disabled_legacy_collector() {
    if (kLegacyTelemetryEnabled) {
        const int descriptor = open_tcp_socket();
        if (descriptor >= 0) {
            (void)::send(descriptor, "legacy", 6, 0);
            ::close(descriptor);
        }
    }
}

} // namespace

int main() {
    start_local_supervisor();
    report_telemetry(9432);
    publish_udp_heartbeat(9433);
    report_through_dispatch_table(9434);
    disabled_legacy_collector();
    return 0;
}
