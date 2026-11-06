// A real, self-contained HTTP/1.1 web service (standard library + POSIX
// sockets, no third-party dependencies). It listens on 127.0.0.1 and exposes
// two endpoints:
//
//   GET  /users/{id}   Authorization: Bearer token-ada
//        -> http_get_user: auth -> cache/table lookup -> JSON response
//   POST /orders        Authorization: Bearer token-ada
//                       {"userId":7,"itemId":101,"quantity":2}
//        -> http_post_order: auth -> validate -> reserve stock -> persist order
//
// Session tokens, user records, cache, inventory and orders are real in-memory
// tables; requests are parsed from a socket and replies are well-formed HTTP/1.1
// responses. Run it as:
//   ./test_web_service [port] [max-requests]   # max-requests 0 = forever
//   curl -H 'Authorization: Bearer token-ada' http://127.0.0.1:18080/users/7
//   curl -X POST -H 'Authorization: Bearer token-ada' \
//        -H 'Content-Type: application/json' \
//        -d '{"userId":7,"itemId":101,"quantity":2}' \
//        http://127.0.0.1:18080/orders
//
// The router reaches both handlers through a function-pointer table, so the
// analyzed call graph contains indirect dispatch. Analyze the two HTTP entries
// with:
//   clang_analyzer --format tree --entry http_get_user --entry http_post_order \
//       test_web_service.bc

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cctype>
#include <cerrno>
#include <climits>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <sstream>
#include <string>
#include <vector>

// ===== HTTP types =====
struct HttpRequest {
    std::string method;
    std::string path;
    std::string version;
    std::map<std::string, std::string> headers;  // lower-case keys
    std::string body;

    std::string header(const std::string& name) const {
        std::string key;
        for (char c : name)
            key += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        auto it = headers.find(key);
        return it == headers.end() ? std::string() : it->second;
    }
};

struct HttpResponse {
    int status = 200;
    std::string body;
};

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

// ===== In-memory stores =====
static const std::map<std::string, int>& sessions() {
    // token -> owning user id; 0 is the admin token.
    static const std::map<std::string, int> table = {
        {"token-ada", 7}, {"token-grace", 8}, {"token-admin", 0},
    };
    return table;
}

static std::map<int, UserRecord>& userTable() {
    static std::map<int, UserRecord> table = {
        {7, {7, "Ada Lovelace", "ada@example.com"}},
        {8, {8, "Grace Hopper", "grace@example.com"}},
    };
    return table;
}

static std::map<int, UserRecord>& userCache() {
    static std::map<int, UserRecord> cache;
    return cache;
}

static std::map<int, int>& inventoryTable() {
    // item id -> units in stock
    static std::map<int, int> stock = {{101, 5}, {202, 3}};
    return stock;
}

static std::vector<Order>& orderTable() {
    static std::vector<Order> orders;
    return orders;
}

// ===== JSON helpers =====
static std::string jsonEscape(const std::string& in) {
    std::string out;
    for (char c : in) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out += c;
        }
    }
    return out;
}

/// Read an integer value for `"key"` from a flat JSON object.
static bool jsonIntField(const std::string& body, const std::string& key,
                         int& out) {
    const std::string needle = "\"" + key + "\"";
    size_t pos = body.find(needle);
    if (pos == std::string::npos) return false;
    pos = body.find(':', pos + needle.size());
    if (pos == std::string::npos) return false;
    ++pos;
    while (pos < body.size() && std::isspace(static_cast<unsigned char>(body[pos])))
        ++pos;
    if (pos == body.size() ||
        (body[pos] != '-' && !std::isdigit(static_cast<unsigned char>(body[pos]))))
        return false;
    char* end = nullptr;
    long value = std::strtol(body.c_str() + pos, &end, 10);
    if (!end || value < INT_MIN || value > INT_MAX) return false;
    out = static_cast<int>(value);
    return true;
}

// ===== Shared middleware =====
static void logRequest(const HttpRequest& req) {
    std::printf("%s %s %s\n", req.method.c_str(), req.path.c_str(),
                req.version.c_str());
    std::fflush(stdout);
}

static bool authenticate(const HttpRequest& req) {
    const std::string auth = req.header("Authorization");
    if (auth.rfind("Bearer ", 0) != 0) return false;
    return sessions().count(auth.substr(7)) != 0;
}

static HttpResponse jsonOk(const std::string& payload, int status = 200) {
    HttpResponse res;
    res.status = status;
    res.body = payload;
    return res;
}

static HttpResponse makeError(int status, const char* reason) {
    return jsonOk("{\"error\":\"" + jsonEscape(reason) + "\"}", status);
}

static const char* reasonPhrase(int status) {
    switch (status) {
        case 200: return "OK";
        case 201: return "Created";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 409: return "Conflict";
        default:  return "Internal Server Error";
    }
}

static int parsePathId(const HttpRequest& req) {
    const std::string prefix = "/users/";
    if (req.path.rfind(prefix, 0) != 0) return 0;
    const std::string tail = req.path.substr(prefix.size());
    if (tail.empty() || tail.find('/') != std::string::npos) return 0;
    char* end = nullptr;
    long id = std::strtol(tail.c_str(), &end, 10);
    if (!end || *end != '\0' || id <= 0 || id > 1000000) return 0;
    return static_cast<int>(id);
}

// ===== Entry 1: GET /users/{id} =====
static bool readUserCache(int id, UserRecord& out) {
    std::map<int, UserRecord>& cache = userCache();
    auto it = cache.find(id);
    if (it == cache.end()) return false;  // real cache miss
    out = it->second;
    return true;
}

static UserRecord queryUserTable(int id) {
    std::map<int, UserRecord>& table = userTable();
    auto it = table.find(id);
    if (it == table.end()) return UserRecord{};  // 404 marker
    userCache()[id] = it->second;                // fill cache for next lookup
    return it->second;
}

static UserRecord loadUser(int id) {
    UserRecord cached;
    if (readUserCache(id, cached)) return cached;
    return queryUserTable(id);
}

static bool authorizeUser(const HttpRequest& req, int id) {
    const std::string auth = req.header("Authorization");
    if (auth.rfind("Bearer ", 0) != 0) return false;
    auto it = sessions().find(auth.substr(7));
    return it != sessions().end() && (it->second == id || it->second == 0);
}

static std::string renderUserJson(const UserRecord& user) {
    if (user.id == 0) return "{}";
    return "{\"id\":" + std::to_string(user.id) + ",\"name\":\"" +
           jsonEscape(user.name) + "\",\"email\":\"" +
           jsonEscape(user.email) + "\"}";
}

HttpResponse http_get_user(const HttpRequest& req) {
    logRequest(req);
    if (!authenticate(req)) return makeError(401, "unauthorized");
    const int id = parsePathId(req);
    if (id == 0) return makeError(400, "bad user id");
    if (!authorizeUser(req, id)) return makeError(403, "forbidden");
    const UserRecord user = loadUser(id);
    if (user.id == 0) return makeError(404, "user not found");
    return jsonOk(renderUserJson(user));
}

// ===== Entry 2: POST /orders =====
static bool validateOrderJson(const std::string& body) {
    int userId = 0, itemId = 0, quantity = 0;
    if (!jsonIntField(body, "userId", userId) ||
        !jsonIntField(body, "itemId", itemId) ||
        !jsonIntField(body, "quantity", quantity))
        return false;
    return userId > 0 && itemId > 0 && quantity > 0 && quantity <= 100;
}

static Order decodeOrder(const HttpRequest& req) {
    Order order;
    jsonIntField(req.body, "userId", order.userId);
    jsonIntField(req.body, "itemId", order.itemId);
    jsonIntField(req.body, "quantity", order.quantity);
    static int nextOrderId = 1000;
    order.id = ++nextOrderId;
    return order;
}

static bool reserveInventory(const Order& order) {
    std::map<int, int>& stock = inventoryTable();
    auto it = stock.find(order.itemId);
    if (it == stock.end() || it->second < order.quantity) return false;
    it->second -= order.quantity;  // real stock decrement
    return true;
}

static void publishOrderCreated(const Order& order) {
    orderTable().push_back(order);  // real persistence
    std::printf("order %d created: user=%d item=%d qty=%d (total=%zu)\n",
                order.id, order.userId, order.itemId, order.quantity,
                orderTable().size());
    std::fflush(stdout);
}

static std::string renderOrderJson(const Order& order) {
    return "{\"orderId\":" + std::to_string(order.id) + ",\"userId\":" +
           std::to_string(order.userId) + ",\"itemId\":" +
           std::to_string(order.itemId) + ",\"quantity\":" +
           std::to_string(order.quantity) + ",\"status\":\"created\"}";
}

HttpResponse http_post_order(const HttpRequest& req) {
    logRequest(req);
    if (!authenticate(req)) return makeError(401, "unauthorized");
    if (!validateOrderJson(req.body)) return makeError(400, "invalid order");
    const Order order = decodeOrder(req);
    if (!reserveInventory(order)) return makeError(409, "out of stock");
    publishOrderCreated(order);
    return jsonOk(renderOrderJson(order), 201);
}

// ===== Router: indirect dispatch through a handler table =====
using HttpHandler = HttpResponse (*)(const HttpRequest&);

struct Route {
    const char* method;
    const char* prefix;
    HttpHandler handler;
};

static const Route kRoutes[] = {
    {"GET", "/users/", http_get_user},
    {"POST", "/orders", http_post_order},
};

HttpResponse dispatch(const HttpRequest& req) {
    for (const Route& route : kRoutes) {
        if (req.method == route.method && req.path.rfind(route.prefix, 0) == 0)
            return route.handler(req);  // indirect call to an HTTP entry
    }
    return makeError(404, "not found");
}

// ===== HTTP/1.1 transport =====
static int createServer(unsigned short port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int reuse = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0 ||
        ::listen(fd, 8) < 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

static bool readHttpRequest(int fd, HttpRequest& req) {
    std::string raw;
    char buf[4096];
    size_t headerEnd = std::string::npos;
    while ((headerEnd = raw.find("\r\n\r\n")) == std::string::npos) {
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        raw.append(buf, static_cast<size_t>(n));
        if (raw.size() > 65536) return false;
    }

    const size_t lineEnd = raw.find("\r\n");
    if (lineEnd == std::string::npos) return false;
    std::istringstream requestLine(raw.substr(0, lineEnd));
    if (!(requestLine >> req.method >> req.path >> req.version)) return false;

    size_t pos = lineEnd + 2;
    while (pos < headerEnd) {
        const size_t eol = raw.find("\r\n", pos);
        if (eol == std::string::npos || eol > headerEnd) break;
        const size_t colon = raw.find(':', pos);
        if (colon != std::string::npos && colon < eol) {
            std::string key = raw.substr(pos, colon - pos);
            for (char& c : key)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            std::string value = raw.substr(colon + 1, eol - colon - 1);
            const size_t first = value.find_first_not_of(" \t");
            value = first == std::string::npos
                        ? std::string()
                        : value.substr(first, value.find_last_not_of(" \t") - first + 1);
            req.headers[key] = value;
        }
        pos = eol + 2;
    }

    req.body = raw.substr(headerEnd + 4);
    size_t want = 0;
    auto it = req.headers.find("content-length");
    if (it != req.headers.end())
        want = static_cast<size_t>(std::strtoul(it->second.c_str(), nullptr, 10));
    if (want > (1u << 20)) return false;
    while (req.body.size() < want) {
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        req.body.append(buf, static_cast<size_t>(n));
    }
    req.body.resize(want);
    return true;
}

static void writeHttpResponse(int fd, const HttpResponse& res) {
    const std::string payload =
        "HTTP/1.1 " + std::to_string(res.status) + " " +
        reasonPhrase(res.status) + "\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: " + std::to_string(res.body.size()) + "\r\n"
        "Connection: close\r\n\r\n" + res.body;
    size_t sent = 0;
    while (sent < payload.size()) {
        ssize_t n = ::send(fd, payload.data() + sent, payload.size() - sent, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        sent += static_cast<size_t>(n);
    }
}

int main(int argc, char** argv) {
    std::signal(SIGPIPE, SIG_IGN);
    const unsigned short port =
        argc > 1 ? static_cast<unsigned short>(std::atoi(argv[1])) : 18080;
    const int maxRequests = argc > 2 ? std::atoi(argv[2]) : 0;  // 0 = forever

    const int serverFd = createServer(port);
    if (serverFd < 0) {
        std::perror("listen");
        return 1;
    }
    std::printf("listening on 127.0.0.1:%u\n", port);
    std::fflush(stdout);

    for (int served = 0; maxRequests == 0 || served < maxRequests; ++served) {
        const int clientFd = ::accept(serverFd, nullptr, nullptr);
        if (clientFd < 0) {
            if (errno == EINTR) continue;
            break;
        }
        HttpRequest req;
        if (readHttpRequest(clientFd, req)) {
            const HttpResponse res = dispatch(req);
            writeHttpResponse(clientFd, res);
        }
        ::close(clientFd);
    }
    ::close(serverFd);
    return 0;
}
