// An asynchronous HTTP/1.1 web service built on Boost.Asio (or standalone
// Asio). Nothing in the request path blocks: accepting, reading the request
// line/headers, reading the body, writing the response, idle timeouts and
// signal handling are all completion-handler driven on an io_context.
//
// It is the async counterpart of tests/test_web_service.cpp and exposes the
// same business endpoints (plus /health and GET /orders):
//
//   GET  /health                -> 200 {"status":"ok",...}          (no auth)
//   GET  /users/{id}            -> 200 {"id":7,"name":...,...}
//   GET  /orders                -> 200 {"orders":[...],"count":n}
//   POST /orders                -> 201 {"orderId":1001,...,"status":"created"}
//        {"userId":7,"itemId":101,"quantity":2}
//   HEAD on any GET route       -> same headers, no body
//
//   Authorization: Bearer token-ada | token-grace | token-admin
//
// Build (Boost >= 1.70, header-only; add -lboost_system for older Boost):
//   g++ -std=c++17 -O2 -pthread asio_http_service.cpp -o asio_http_service
// Build with standalone Asio (think-async, e.g. libasio-dev):
//   g++ -std=c++17 -O2 -pthread -DASIO_HTTP_USE_STANDALONE_ASIO asio_http_service.cpp -o asio_http_service
//
// Run:
//   ./asio_http_service --address 127.0.0.1 --port 18080 --threads 4
//   ./asio_http_service 18080 2          # positional: port, max-requests
//   curl -i -H 'Authorization: Bearer token-ada' http://127.0.0.1:18080/users/7
//   curl -i -X POST -H 'Authorization: Bearer token-ada' -H 'Content-Type: application/json' -d '{"userId":7,"itemId":101,"quantity":2}' http://127.0.0.1:18080/orders
//
// Notes:
//   * HTTP/1.1 keep-alive and pipelined requests are supported, one in-flight
//     read and one in-flight write per connection, all serialized on a strand.
//   * Chunked transfer encoding is rejected with 501; Content-Length bodies
//     up to 1 MiB are supported.
//   * Handlers only touch in-memory tables (guarded by a mutex), so they never
//     block the event loop. Offload slow I/O to a separate pool in production.
//   * --max-requests N makes the server shut down after N responses, which is
//     handy for smoke tests; 0 (default) means serve until SIGINT/SIGTERM.

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <climits>
#include <csignal>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

// ---------------------------------------------------------------------------
// Choose the Asio implementation. Define ASIO_HTTP_USE_STANDALONE_ASIO to
// force think-async's standalone Asio; otherwise auto-detect Boost.Asio first
// and standalone Asio second.
// ---------------------------------------------------------------------------
#if !defined(ASIO_HTTP_USE_STANDALONE_ASIO)
#  if defined(__has_include)
#    if __has_include(<boost/asio.hpp>)
#      define ASIO_HTTP_HAS_BOOST 1
#    endif
#  else
#    define ASIO_HTTP_HAS_BOOST 1
#  endif
#endif

#if defined(ASIO_HTTP_HAS_BOOST)
#  include <boost/asio.hpp>
namespace net = boost::asio;
using error_code = boost::system::error_code;
#else
#  ifndef ASIO_STANDALONE
#    define ASIO_STANDALONE
#  endif
#  include <asio.hpp>
namespace net = asio;
using error_code = std::error_code;
#endif

namespace {

constexpr std::size_t kMaxHeaderBytes = 16 * 1024;
constexpr std::size_t kMaxBodyBytes = 1024 * 1024;
constexpr std::size_t kMaxRequestsPerConnection = 100;
constexpr std::chrono::seconds kIdleTimeout{30};

std::mutex& logMutex() {
    static std::mutex m;
    return m;
}

void logLine(const std::string& line) {
    std::lock_guard<std::mutex> lock(logMutex());
    std::printf("%s\n", line.c_str());
    std::fflush(stdout);
}

std::string lower(std::string s) {
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trim(const std::string& s) {
    const std::size_t first = s.find_first_not_of(" \t");
    if (first == std::string::npos) return std::string();
    const std::size_t last = s.find_last_not_of(" \t");
    return s.substr(first, last - first + 1);
}

// ===== HTTP types =====
struct HttpRequest {
    std::string method;
    std::string target;
    std::string path;
    std::string query;
    std::string version;
    std::map<std::string, std::string> headers;  // lower-case keys
    std::string body;

    std::string header(const std::string& name) const {
        const auto it = headers.find(lower(name));
        return it == headers.end() ? std::string() : it->second;
    }
};

struct HttpResponse {
    int status = 200;
    std::string content_type = "application/json; charset=utf-8";
    std::string extra_headers;  // complete lines, each ending in "\r\n"
    std::string body;
};

const char* reasonPhrase(int status) {
    switch (status) {
        case 200: return "OK";
        case 201: return "Created";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 409: return "Conflict";
        case 413: return "Payload Too Large";
        case 414: return "URI Too Long";
        case 431: return "Request Header Fields Too Large";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 505: return "HTTP Version Not Supported";
        default:  return "Unknown";
    }
}

std::string jsonEscape(const std::string& in) {
    std::string out;
    out.reserve(in.size() + 8);
    char buf[8];
    for (unsigned char c : in) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out;
}

HttpResponse jsonResponse(int status, const std::string& payload) {
    HttpResponse res;
    res.status = status;
    res.body = payload;
    return res;
}

HttpResponse makeError(int status, const std::string& reason) {
    return jsonResponse(status, "{\"error\":\"" + jsonEscape(reason) + "\"}");
}

/// Read an integer value for `"key"` from a flat JSON object.
bool jsonIntField(const std::string& body, const std::string& key, int& out) {
    const std::string needle = "\"" + key + "\"";
    std::size_t pos = body.find(needle);
    if (pos == std::string::npos) return false;
    pos = body.find(':', pos + needle.size());
    if (pos == std::string::npos) return false;
    ++pos;
    while (pos < body.size() &&
           std::isspace(static_cast<unsigned char>(body[pos])))
        ++pos;
    if (pos == body.size() ||
        (body[pos] != '-' && !std::isdigit(static_cast<unsigned char>(body[pos]))))
        return false;
    errno = 0;
    char* end = nullptr;
    const long value = std::strtol(body.c_str() + pos, &end, 10);
    if (errno == ERANGE || !end || value < INT_MIN || value > INT_MAX)
        return false;
    const char* last = body.c_str() + body.size();
    while (end < last && std::isspace(static_cast<unsigned char>(*end))) ++end;
    if (end < last && *end != ',' && *end != '}') return false;
    out = static_cast<int>(value);
    return true;
}

int parsePathId(const std::string& path) {
    const std::string prefix = "/users/";
    if (path.rfind(prefix, 0) != 0) return 0;
    const std::string tail = path.substr(prefix.size());
    if (tail.empty() || tail.find('/') != std::string::npos) return 0;
    for (char c : tail)
        if (!std::isdigit(static_cast<unsigned char>(c))) return 0;
    errno = 0;
    const long id = std::strtol(tail.c_str(), nullptr, 10);
    if (errno == ERANGE || id <= 0 || id > 1000000) return 0;
    return static_cast<int>(id);
}

std::string serializeResponse(const HttpResponse& res, bool keep_alive,
                              bool head_only) {
    std::string out;
    out.reserve(res.body.size() + 256);
    out += "HTTP/1.1 " + std::to_string(res.status) + " " +
           reasonPhrase(res.status) + "\r\n";
    out += "Server: asio-http-service/1.0\r\n";
    out += "Content-Type: " + res.content_type + "\r\n";
    out += "Content-Length: " + std::to_string(res.body.size()) + "\r\n";
    out += res.extra_headers;
    if (keep_alive) {
        out += "Connection: keep-alive\r\n";
        out += "Keep-Alive: timeout=30, max=100\r\n";
    } else {
        out += "Connection: close\r\n";
    }
    out += "\r\n";
    if (!head_only) out += res.body;
    return out;
}

// ===== Business layer (shared state, thread safe) =====
struct UserRecord {
    int id = 0;
    std::string name;
    std::string email;
};

struct Order {
    int id = 0;
    int userId = 0;
    int itemId = 0;
    int quantity = 0;
};

std::string renderUserJson(const UserRecord& user) {
    if (user.id == 0) return "{}";
    return "{\"id\":" + std::to_string(user.id) + ",\"name\":\"" +
           jsonEscape(user.name) + "\",\"email\":\"" +
           jsonEscape(user.email) + "\"}";
}

std::string renderOrderJson(const Order& order) {
    return "{\"orderId\":" + std::to_string(order.id) +
           ",\"userId\":" + std::to_string(order.userId) +
           ",\"itemId\":" + std::to_string(order.itemId) +
           ",\"quantity\":" + std::to_string(order.quantity) +
           ",\"status\":\"created\"}";
}

class Service {
public:
    Service() : started_(std::chrono::steady_clock::now()) {
        // token -> owning user id; 0 is the admin token.
        sessions_ = {{"token-ada", 7}, {"token-grace", 8}, {"token-admin", 0}};
        users_ = {{7, {7, "Ada Lovelace", "ada@example.com"}},
                  {8, {8, "Grace Hopper", "grace@example.com"}}};
        inventory_ = {{101, 5}, {202, 3}};
    }

    HttpResponse handle(const HttpRequest& req) {
        logLine(req.method + " " + req.target + " " + req.version);
        request_count_.fetch_add(1, std::memory_order_relaxed);

        // One short critical section covers the whole in-memory transaction.
        // Every private handler below assumes mu_ is held.
        std::lock_guard<std::mutex> lock(mu_);
        const std::string method = req.method == "HEAD" ? "GET" : req.method;
        bool path_known = false;
        for (const Route* r = routes(); r->handler != nullptr; ++r) {
            const bool matches = r->exact
                                     ? req.path == r->prefix
                                     : req.path.rfind(r->prefix, 0) == 0;
            if (!matches) continue;
            path_known = true;
            if (method == r->method)
                return (this->*r->handler)(req);  // indirect dispatch
        }
        if (path_known) {
            HttpResponse res = makeError(405, "method not allowed");
            res.extra_headers = "Allow: GET, HEAD, POST\r\n";
            return res;
        }
        return makeError(404, "not found");
    }

    std::size_t requestCount() const {
        return request_count_.load(std::memory_order_relaxed);
    }

private:
    using Handler = HttpResponse (Service::*)(const HttpRequest&);
    struct Route {
        const char* method;
        const char* prefix;
        bool exact;
        Handler handler;
    };

    static const Route* routes() {
        static const Route table[] = {
            {"GET", "/health", true, &Service::handleHealth},
            {"GET", "/users/", false, &Service::httpGetUser},
            {"GET", "/orders", true, &Service::listOrders},
            {"POST", "/orders", true, &Service::httpPostOrder},
            {"", "", true, nullptr},
        };
        return table;
    }

    /// Returns the session's user id, or -1 when unauthenticated.
    int authenticatedUser(const HttpRequest& req) const {
        const std::string auth = req.header("Authorization");
        if (auth.rfind("Bearer ", 0) != 0) return -1;
        const auto it = sessions_.find(auth.substr(7));
        return it == sessions_.end() ? -1 : it->second;
    }

    HttpResponse handleHealth(const HttpRequest&) {
        const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
                                 std::chrono::steady_clock::now() - started_)
                                 .count();
        return jsonResponse(200,
                            "{\"status\":\"ok\",\"uptimeSeconds\":" +
                                std::to_string(seconds) + ",\"requests\":" +
                                std::to_string(requestCount()) +
                                ",\"orders\":" +
                                std::to_string(orders_.size()) + "}");
    }

    HttpResponse httpGetUser(const HttpRequest& req) {
        if (authenticatedUser(req) < 0) return makeError(401, "unauthorized");
        const int id = parsePathId(req.path);
        if (id == 0) return makeError(400, "bad user id");
        const int session_user = authenticatedUser(req);
        if (session_user != 0 && session_user != id)
            return makeError(403, "forbidden");

        const auto cached = cache_.find(id);
        if (cached != cache_.end()) return jsonResponse(200, renderUserJson(cached->second));
        const auto it = users_.find(id);
        if (it == users_.end()) return makeError(404, "user not found");
        cache_[id] = it->second;  // fill the cache for the next lookup
        return jsonResponse(200, renderUserJson(it->second));
    }

    bool validateOrderJson(const std::string& body) const {
        int userId = 0, itemId = 0, quantity = 0;
        if (!jsonIntField(body, "userId", userId) ||
            !jsonIntField(body, "itemId", itemId) ||
            !jsonIntField(body, "quantity", quantity))
            return false;
        return userId > 0 && itemId > 0 && quantity > 0 && quantity <= 100;
    }

    HttpResponse httpPostOrder(const HttpRequest& req) {
        if (authenticatedUser(req) < 0) return makeError(401, "unauthorized");
        if (!validateOrderJson(req.body)) return makeError(400, "invalid order");

        Order order;
        jsonIntField(req.body, "userId", order.userId);
        jsonIntField(req.body, "itemId", order.itemId);
        jsonIntField(req.body, "quantity", order.quantity);
        order.id = ++next_order_id_;

        auto stock = inventory_.find(order.itemId);
        if (stock == inventory_.end() || stock->second < order.quantity)
            return makeError(409, "out of stock");
        stock->second -= order.quantity;  // real stock decrement
        orders_.push_back(order);         // real persistence
        logLine("order " + std::to_string(order.id) + " created: user=" +
                std::to_string(order.userId) + " item=" +
                std::to_string(order.itemId) + " qty=" +
                std::to_string(order.quantity) + " (total=" +
                std::to_string(orders_.size()) + ")");
        return jsonResponse(201, renderOrderJson(order));
    }

    HttpResponse listOrders(const HttpRequest& req) {
        if (authenticatedUser(req) < 0) return makeError(401, "unauthorized");
        std::string items = "[";
        for (const Order& order : orders_) {
            if (items.size() > 1) items += ',';
            items += renderOrderJson(order);
        }
        items += ']';
        return jsonResponse(200, "{\"orders\":" + items + ",\"count\":" +
                                     std::to_string(orders_.size()) + "}");
    }

    mutable std::mutex mu_;
    std::atomic<std::size_t> request_count_{0};
    std::chrono::steady_clock::time_point started_;
    std::map<std::string, int> sessions_;
    std::map<int, UserRecord> users_;
    std::map<int, UserRecord> cache_;
    std::map<int, int> inventory_;
    std::vector<Order> orders_;
    int next_order_id_ = 1000;
};

class Server;

// ===== One connection: an async state machine, serialized on a strand =====
class Session : public std::enable_shared_from_this<Session> {
public:
    using tcp = net::ip::tcp;
    using Strand = decltype(net::make_strand(std::declval<net::io_context&>()));

    Session(tcp::socket socket, net::io_context& io, Service& service,
            Server& server)
        : socket_(std::move(socket)),
          strand_(net::make_strand(io)),
          idle_(io),
          service_(service),
          server_(server) {}

    void start();
    void closeNow();  // may be called from any thread

private:
    void armIdle();
    void readHeaders();
    void onHeaders(const error_code& ec, std::size_t bytes);
    bool parseHeaders(std::size_t bytes, int& status, std::string& message);
    void readBody();
    void onBody(const error_code& ec);
    void processRequest();
    void writeResponse(const HttpResponse& res);
    void fail(int status, const std::string& message);
    void onWritten(const error_code& ec);
    void doClose();

    tcp::socket socket_;
    Strand strand_;
    net::steady_timer idle_;
    net::streambuf input_;
    Service& service_;
    Server& server_;
    HttpRequest req_;
    std::string output_;
    std::size_t body_want_ = 0;
    std::size_t requests_on_connection_ = 0;
    bool closing_ = false;
    bool keep_alive_ = true;
    bool head_only_ = false;
};

// ===== Listener: async_accept loop, signal-driven shutdown =====
class Server {
public:
    using tcp = net::ip::tcp;
    using Strand = Session::Strand;

    Server(net::io_context& io, Service& service, const std::string& address,
           unsigned short port, std::size_t max_requests)
        : io_(io),
          acceptor_(io),
          signals_(io, SIGINT, SIGTERM),
          strand_(net::make_strand(io)),
          service_(service),
          address_(address),
          port_(port),
          max_requests_(max_requests) {}

    void run() {
        const tcp::endpoint endpoint(net::ip::make_address(address_), port_);
        acceptor_.open(endpoint.protocol());
        acceptor_.set_option(net::socket_base::reuse_address(true));
        acceptor_.bind(endpoint);
        acceptor_.listen(net::socket_base::max_listen_connections);
        const std::string limit = max_requests_ == 0
                                      ? std::string("unlimited")
                                      : std::to_string(max_requests_);
        logLine("listening on " + address_ + ":" + std::to_string(port_) +
                " (async, max-requests=" + limit + ")");

        // All acceptor/signal operations live on this strand, so stop() never
        // races with an in-flight accept.
        net::post(strand_, [this] {
            signals_.async_wait(net::bind_executor(
                strand_, [this](const error_code& ec, int) {
                    if (!ec) {
                        logLine("signal received, shutting down");
                        stop();
                    }
                }));
            doAccept();
        });
    }

    void registerSession(const std::shared_ptr<Session>& session) {
        net::post(strand_, [this, weak = std::weak_ptr<Session>(session)] {
            sessions_.erase(
                std::remove_if(sessions_.begin(), sessions_.end(),
                               [](const std::weak_ptr<Session>& w) {
                                   return w.expired();
                               }),
                sessions_.end());
            sessions_.push_back(weak);
        });
    }

    void onRequestServed() {
        if (max_requests_ != 0 &&
            served_.fetch_add(1, std::memory_order_relaxed) + 1 >= max_requests_)
            stop();
    }

    bool shuttingDown() const {
        return shutdown_.load(std::memory_order_relaxed);
    }

    void stop() {
        if (shutdown_.exchange(true)) return;
        net::post(strand_, [this] {
            error_code ec;
            acceptor_.close(ec);
            signals_.cancel();
            for (const auto& weak : sessions_)
                if (auto session = weak.lock()) session->closeNow();
            sessions_.clear();
            logLine("server stopped, requests served: " +
                    std::to_string(service_.requestCount()));
        });
    }

private:
    void doAccept() {
        if (shuttingDown()) return;
        acceptor_.async_accept(net::bind_executor(
            strand_, [this](const error_code& ec, tcp::socket socket) {
                if (ec) {
                    if (ec != net::error::operation_aborted)
                        logLine("accept: " + ec.message());
                    return;
                }
                auto session = std::make_shared<Session>(std::move(socket), io_,
                                                         service_, *this);
                registerSession(session);
                session->start();
                doAccept();
            }));
    }

    net::io_context& io_;
    tcp::acceptor acceptor_;
    net::signal_set signals_;
    Strand strand_;
    Service& service_;
    std::string address_;
    unsigned short port_;
    std::size_t max_requests_;
    std::atomic<std::size_t> served_{0};
    std::atomic<bool> shutdown_{false};
    std::vector<std::weak_ptr<Session>> sessions_;  // strand-confined
};

// ===== Session implementation (needs the complete Server type) =====
void Session::start() {
    readHeaders();
}

void Session::closeNow() {
    net::post(strand_, [self = shared_from_this()] { self->doClose(); });
}

void Session::armIdle() {
    if (closing_) return;
    idle_.expires_after(kIdleTimeout);
    idle_.async_wait(net::bind_executor(
        strand_, [self = shared_from_this()](const error_code& ec) {
            if (!ec && !self->closing_) {
                logLine("idle timeout, closing connection");
                self->doClose();
            }
        }));
}

void Session::readHeaders() {
    if (closing_) return;
    req_ = HttpRequest();
    body_want_ = 0;
    head_only_ = false;
    armIdle();
    net::async_read_until(
        socket_, input_, "\r\n\r\n",
        net::bind_executor(strand_,
                           [self = shared_from_this()](const error_code& ec,
                                                       std::size_t bytes) {
                               self->onHeaders(ec, bytes);
                           }));
}

void Session::onHeaders(const error_code& ec, std::size_t bytes) {
    if (ec || closing_) {
        doClose();
        return;
    }
    if (bytes > kMaxHeaderBytes) {
        fail(431, "request header fields too large");
        return;
    }
    int status = 0;
    std::string message;
    if (!parseHeaders(bytes, status, message)) {
        fail(status, message);
        return;
    }
    if (std::min(input_.size(), body_want_) < body_want_)
        readBody();
    else
        onBody(error_code());
}

bool Session::parseHeaders(std::size_t /*bytes*/, int& status,
                           std::string& message) {
    status = 400;
    message = "malformed request";

    std::istream stream(&input_);
    std::string line;
    if (!std::getline(stream, line)) return false;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    std::istringstream request_line(line);
    std::string extra;
    if (!(request_line >> req_.method >> req_.target >> req_.version) ||
        (request_line >> extra))
        return false;
    if (req_.version != "HTTP/1.1" && req_.version != "HTTP/1.0") {
        status = 505;
        message = "HTTP version not supported";
        return false;
    }
    if (req_.target.empty() || req_.target[0] != '/' ||
        req_.target.size() > 8192 || req_.method.size() > 32) {
        status = 414;
        message = "request target too long";
        return false;
    }
    const std::size_t q = req_.target.find('?');
    req_.path = req_.target.substr(0, q);
    if (q != std::string::npos) req_.query = req_.target.substr(q + 1);

    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) break;  // blank line terminates the header block
        const std::size_t colon = line.find(':');
        if (colon == 0 || colon == std::string::npos) return false;
        std::string key = lower(trim(line.substr(0, colon)));
        const std::string value = trim(line.substr(colon + 1));
        if (key.empty()) return false;
        auto it = req_.headers.find(key);
        if (it == req_.headers.end()) {
            req_.headers.emplace(std::move(key), value);
        } else {
            if (key == "content-length" && it->second != value) {
                message = "conflicting Content-Length";
                return false;
            }
            it->second += ", " + value;
        }
    }
    head_only_ = req_.method == "HEAD";

    const std::string transfer = lower(req_.header("Transfer-Encoding"));
    if (!transfer.empty()) {
        status = 501;
        message = "chunked/other transfer encoding is not supported";
        return false;
    }
    const std::string length = trim(req_.header("Content-Length"));
    if (!length.empty()) {
        if (length.find_first_not_of("0123456789") != std::string::npos)
            return false;
        errno = 0;
        char* end = nullptr;
        const unsigned long long value = std::strtoull(length.c_str(), &end, 10);
        if (errno == ERANGE || !end || *end != '\0' ||
            value > kMaxBodyBytes) {
            status = 413;
            message = "payload too large";
            return false;
        }
        body_want_ = static_cast<std::size_t>(value);
    }

    const std::string connection = lower(req_.header("Connection"));
    keep_alive_ = req_.version == "HTTP/1.1";
    if (connection.find("close") != std::string::npos)
        keep_alive_ = false;
    else if (connection.find("keep-alive") != std::string::npos)
        keep_alive_ = true;
    return true;
}

void Session::readBody() {
    armIdle();
    const std::size_t remaining = body_want_ - input_.size();
    net::async_read(
        socket_, input_, net::transfer_exactly(remaining),
        net::bind_executor(strand_, [self = shared_from_this()](
                                        const error_code& ec, std::size_t) {
            self->onBody(ec);
        }));
}

void Session::onBody(const error_code& ec) {
    if (ec || closing_) {
        doClose();
        return;
    }
    if (body_want_ > 0) {
        req_.body.resize(body_want_);
        const std::size_t copied = net::buffer_copy(
            net::buffer(req_.body.data(), body_want_), input_.data());
        input_.consume(copied);
        if (copied != body_want_) {
            doClose();
            return;
        }
    }
    processRequest();
}

void Session::processRequest() {
    // In-memory handlers are non-blocking; a slow backend would be posted to
    // a worker pool and the reply resumed on this strand.
    writeResponse(service_.handle(req_));
}

void Session::fail(int status, const std::string& message) {
    keep_alive_ = false;
    writeResponse(makeError(status, message));
}

void Session::writeResponse(const HttpResponse& res) {
    if (closing_) return;
    const bool keep = keep_alive_ && !server_.shuttingDown() &&
                      requests_on_connection_ + 1 < kMaxRequestsPerConnection;
    keep_alive_ = keep;
    output_ = serializeResponse(res, keep, head_only_);
    armIdle();
    net::async_write(
        socket_, net::buffer(output_),
        net::bind_executor(strand_, [self = shared_from_this()](
                                        const error_code& ec, std::size_t) {
            self->onWritten(ec);
        }));
}

void Session::onWritten(const error_code& ec) {
    ++requests_on_connection_;
    if (ec || closing_) {
        doClose();
        return;
    }
    server_.onRequestServed();
    if (!keep_alive_ || server_.shuttingDown()) {
        doClose();
        return;
    }
    readHeaders();  // next request on this keep-alive connection
}

void Session::doClose() {
    if (closing_) return;
    closing_ = true;
    error_code ec;
    socket_.shutdown(tcp::socket::shutdown_both, ec);
    socket_.close(ec);
    idle_.cancel(ec);
}

// ===== CLI =====
struct Config {
    std::string address = "127.0.0.1";
    unsigned short port = 18080;
    unsigned threads = 1;
    std::size_t max_requests = 0;
};

void printUsage(const char* program) {
    std::printf(
        "Usage: %s [port] [max-requests]\n"
        "       %s [--address ADDRESS] [--port PORT] [--threads N] "
        "[--max-requests N]\n"
        "\n"
        "  --address       bind address            (default 127.0.0.1)\n"
        "  --port          listen port             (default 18080)\n"
        "  --threads       io_context threads      (default 1)\n"
        "  --max-requests  stop after N replies, 0 = forever (default 0)\n"
        "  -h, --help      show this help\n",
        program, program);
}

bool parseNumber(const std::string& text, unsigned long long limit,
                 unsigned long long& out) {
    if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos)
        return false;
    errno = 0;
    char* end = nullptr;
    const unsigned long long value = std::strtoull(text.c_str(), &end, 10);
    if (errno == ERANGE || !end || *end != '\0' || value > limit) return false;
    out = value;
    return true;
}

enum class ParseResult { Ok, Help, Error };

ParseResult parseArgs(int argc, char** argv, Config& cfg, std::string& error) {
    int positional = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") return ParseResult::Help;
        auto value = [&](std::string& out) {
            if (i + 1 >= argc) {
                error = "missing value for " + arg;
                return false;
            }
            out = argv[++i];
            return true;
        };
        if (arg == "--address" || arg == "--port" || arg == "--threads" ||
            arg == "--max-requests") {
            std::string text;
            if (!value(text)) return ParseResult::Error;
            unsigned long long number = 0;
            if (arg == "--address") {
                cfg.address = text;
                continue;
            }
            const unsigned long long limit = arg == "--port"        ? 65535
                                             : arg == "--threads"   ? 256
                                                                    : 1000000000ULL;
            if (!parseNumber(text, limit, number) ||
                (arg == "--port" && number == 0) ||
                (arg == "--threads" && number == 0)) {
                error = "invalid value for " + arg + ": " + text;
                return ParseResult::Error;
            }
            if (arg == "--port")
                cfg.port = static_cast<unsigned short>(number);
            else if (arg == "--threads")
                cfg.threads = static_cast<unsigned>(number);
            else
                cfg.max_requests = static_cast<std::size_t>(number);
            continue;
        }
        if (!arg.empty() && arg[0] != '-' && positional < 2) {
            unsigned long long number = 0;
            if (!parseNumber(arg, positional == 0 ? 65535 : 1000000000ULL,
                             number) ||
                (positional == 0 && number == 0)) {
                error = "invalid positional argument: " + arg;
                return ParseResult::Error;
            }
            if (positional++ == 0)
                cfg.port = static_cast<unsigned short>(number);
            else
                cfg.max_requests = static_cast<std::size_t>(number);
            continue;
        }
        error = "unknown argument: " + arg;
        return ParseResult::Error;
    }
    return ParseResult::Ok;
}

}  // namespace

int main(int argc, char** argv) {
    Config cfg;
    std::string error;
    const ParseResult parsed = parseArgs(argc, argv, cfg, error);
    if (parsed == ParseResult::Help) {
        printUsage(argv[0]);
        return 0;
    }
    if (parsed == ParseResult::Error) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        printUsage(argv[0]);
        return 1;
    }

#ifdef SIGPIPE
    std::signal(SIGPIPE, SIG_IGN);
#endif

    try {
        net::io_context io;
        Service service;
        Server server(io, service, cfg.address, cfg.port, cfg.max_requests);
        server.run();  // synchronous setup, then all work is posted to the io_context

        std::vector<std::thread> pool;
        pool.reserve(cfg.threads > 0 ? cfg.threads - 1 : 0);
        for (unsigned i = 1; i < cfg.threads; ++i) {
            pool.emplace_back([&io] {
                try {
                    io.run();
                } catch (const std::exception& e) {
                    logLine(std::string("worker error: ") + e.what());
                }
            });
        }
        try {
            io.run();
        } catch (const std::exception& e) {
            logLine(std::string("io_context error: ") + e.what());
        }
        for (auto& thread : pool) thread.join();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "fatal: %s\n", e.what());
        return 1;
    }
    return 0;
}
