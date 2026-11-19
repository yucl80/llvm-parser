#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <netdb.h>
#include <openssl/ssl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <string>
#include <vector>

namespace asio = boost::asio;
namespace beast_http = boost::beast::http;
using TcpStream = boost::beast::tcp_stream;

int connect_with_retries(const char *host, const char *service) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo *addresses = nullptr;
    if (getaddrinfo(host, service, &hints, &addresses) != 0)
        return -1;

    int connected_socket = -1;
    for (addrinfo *address = addresses; address; address = address->ai_next) {
        for (int attempt = 0; attempt < 3; ++attempt) {
            const int candidate =
                socket(address->ai_family, address->ai_socktype,
                       address->ai_protocol);
            if (candidate < 0)
                continue;
            if (connect(candidate, address->ai_addr, address->ai_addrlen) == 0) {
                connected_socket = candidate;
                break;
            }
            close(candidate);
        }
        if (connected_socket >= 0)
            break;
    }

    freeaddrinfo(addresses);
    return connected_socket;
}

int send_request_fully(int socket, const std::string &request) {
    std::size_t sent = 0;
    while (sent < request.size()) {
        const ssize_t count =
            send(socket, request.data() + sent, request.size() - sent, 0);
        if (count <= 0)
            return -1;
        sent += static_cast<std::size_t>(count);
    }
    return 0;
}

std::string receive_response(int socket) {
    char buffer[4096];
    std::string response;
    while (true) {
        const ssize_t count = recv(socket, buffer, sizeof(buffer), 0);
        if (count <= 0)
            break;
        response.append(buffer, static_cast<std::size_t>(count));
    }
    return response;
}

std::string receive_response_until_closed(int socket) {
    char buffer[4096];
    std::string response;
    ssize_t count = 0;
    while ((count = recv(socket, buffer, sizeof(buffer), 0)) > 0)
        response.append(buffer, static_cast<std::size_t>(count));
    return response;
}

void receive_in_for_condition(int socket, char *buffer) {
    for (int attempt = 0; recv(socket, buffer, 1, 0) > 0; ++attempt) {
        if (attempt > 100)
            break;
    }
}

void send_in_for_increment(int socket, const std::string &request) {
    for (std::size_t offset = 0; offset < request.size();
         (void)send(socket, request.data() + offset++, 1, 0)) {
    }
}

int complete_tls_handshake(SSL *session) {
    for (int attempt = 0; attempt < 5; ++attempt) {
        const int result = SSL_connect(session);
        if (result == 1)
            return 1;

        const int error = SSL_get_error(session, result);
        if (error != SSL_ERROR_WANT_READ && error != SSL_ERROR_WANT_WRITE)
            return result;

        pollfd descriptor{};
        descriptor.fd = SSL_get_fd(session);
        descriptor.events = error == SSL_ERROR_WANT_READ ? POLLIN : POLLOUT;
        if (poll(&descriptor, 1, 1000) <= 0)
            return result;
    }
    return -1;
}

int write_tls_request_fully(SSL *session, const std::string &request) {
    std::size_t sent = 0;
    while (sent < request.size()) {
        const int count = SSL_write(
            session, request.data() + sent,
            static_cast<int>(request.size() - sent));
        if (count <= 0)
            return -1;
        sent += static_cast<std::size_t>(count);
    }
    return 0;
}

void connect_to_asio_endpoints(
    asio::ip::tcp::socket &socket,
    const asio::ip::tcp::resolver::results_type &endpoints) {
    for (const auto &entry : endpoints) {
        boost::system::error_code error;
        socket.connect(entry.endpoint(), error);
        if (!error)
            return;
    }
}

void queue_asio_requests(
    asio::ip::tcp::socket &socket,
    const std::vector<std::string> &requests) {
    for (const std::string &request : requests) {
        asio::async_write(
            socket, asio::buffer(request),
            [](const boost::system::error_code &, std::size_t) {});
    }
}

void write_http_request(TcpStream &stream,
                        beast_http::request<beast_http::string_body> &request,
                        boost::system::error_code &error) {
    beast_http::write(stream, request, error);
}

void write_http_requests(TcpStream &stream,
                         std::vector<beast_http::request<
                             beast_http::string_body>> &requests) {
    for (auto &request : requests) {
        boost::system::error_code error;
        write_http_request(stream, request, error);
        if (error)
            return;
    }
}

int send_once_outside_loop(int socket, const std::string &request) {
    return static_cast<int>(send(socket, request.data(), request.size(), 0));
}

using SendFunction = decltype(&send);

void send_via_local_function_pointer(int socket, const char *data,
                                     std::size_t size) {
    SendFunction sender = nullptr;
    sender = &send;
    for (int attempt = 0; attempt < 2; ++attempt)
        (void)sender(socket, data, size, 0);
}

void send_via_initialized_function_pointer(int socket, const char *data,
                                           std::size_t size) {
    SendFunction sender = &send;
    for (int attempt = 0; attempt < 2; ++attempt)
        (void)sender(socket, data, size, 0);
}

void send_via_callback_parameter(SendFunction callback, int socket,
                                 const char *data, std::size_t size) {
    for (int attempt = 0; attempt < 2; ++attempt)
        (void)callback(socket, data, size, 0);
}

void pass_send_as_callback(int socket, const char *data, std::size_t size) {
    send_via_callback_parameter(&send, socket, data, size);
}

using DeferredCallback = void (*)();
void register_callback(DeferredCallback callback);

void register_network_callback_in_loop() {
    for (int attempt = 0; attempt < 2; ++attempt) {
        register_callback([] {
            (void)send(-1, "", 0, 0);
        });
    }
}

void invoke_callback_in_loop(DeferredCallback callback) {
    for (int attempt = 0; attempt < 2; ++attempt)
        callback();
}

void pass_lambda_to_loop_callback() {
    invoke_callback_in_loop([] {
        (void)send(-1, "", 0, 0);
    });
}

namespace local_network_false_positive {
void send(int, const char *, std::size_t, int) {}
}

void local_send_name_in_loop(int socket) {
    for (int attempt = 0; attempt < 2; ++attempt) {
        local_network_false_positive::send(socket, "x", 1, 0);
    }
}

void never_run_network_calls(int socket) {
    while (false) {
        (void)send(socket, "x", 1, 0);
    }
    for (int attempt = 0; attempt < 0; ++attempt) {
        (void)send(socket, "x", 1, 0);
    }
    if (false) {
        (void)send(socket, "x", 1, 0);
    }
    while (true) {
        break;
        (void)send(socket, "x", 1, 0);
    }
}

