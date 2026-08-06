#pragma once

#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <pqxx/pqxx>
#include <string>
#include <vector>

namespace nuigraph {

// A bounded pool of PostgreSQL connections.
//
// Every request used to construct a fresh pqxx::connection, which means a TCP
// connect, TLS handshake and full authentication round trip before any query
// ran. That is slow under normal load and, more importantly, lets a burst of
// concurrent requests exhaust the server's max_connections and lock out every
// other service sharing that database.
//
// Connections are handed out as leases that return themselves on destruction.
// Once maxSize are outstanding, callers wait rather than opening more, so the
// pool doubles as a cap on how much of the database this service can occupy.
class ConnectionPool {
public:
    ConnectionPool(std::string url, std::size_t maxSize);

    // Borrowed connection. Returns itself to the pool when it goes out of
    // scope, and is dropped instead of reused if the backend closed on us.
    class Lease {
    public:
        Lease(ConnectionPool* pool, std::unique_ptr<pqxx::connection> conn);
        ~Lease();

        Lease(Lease&&) noexcept;
        Lease& operator=(Lease&&) = delete;
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;

        pqxx::connection& get() { return *conn_; }
        operator pqxx::connection&() { return *conn_; }

    private:
        ConnectionPool* pool_;
        std::unique_ptr<pqxx::connection> conn_;
    };

    Lease acquire();

    std::size_t idleCount();

private:
    void release(std::unique_ptr<pqxx::connection> conn);

    std::string url_;
    std::size_t maxSize_;
    std::mutex mutex_;
    std::condition_variable available_;
    std::vector<std::unique_ptr<pqxx::connection>> idle_;
    std::size_t outstanding_ = 0;
};

} // namespace nuigraph
