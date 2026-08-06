#include "storage/ConnectionPool.hpp"

#include <chrono>
#include <stdexcept>
#include <utility>

namespace nuigraph {

ConnectionPool::ConnectionPool(std::string url, std::size_t maxSize)
    : url_(std::move(url)), maxSize_(maxSize == 0 ? 1 : maxSize) {}

ConnectionPool::Lease::Lease(ConnectionPool* pool, std::unique_ptr<pqxx::connection> conn)
    : pool_(pool), conn_(std::move(conn)) {}

ConnectionPool::Lease::Lease(Lease&& other) noexcept
    : pool_(other.pool_), conn_(std::move(other.conn_)) {
    other.pool_ = nullptr;
}

ConnectionPool::Lease::~Lease() {
    if (pool_) {
        pool_->release(std::move(conn_));
    }
}

ConnectionPool::Lease ConnectionPool::acquire() {
    std::unique_lock<std::mutex> lock(mutex_);

    for (;;) {
        while (!idle_.empty()) {
            auto conn = std::move(idle_.back());
            idle_.pop_back();
            // A pooled connection can be closed by a server restart or an idle
            // timeout between uses, so check before handing it out.
            if (conn && conn->is_open()) {
                ++outstanding_;
                return Lease(this, std::move(conn));
            }
        }

        if (outstanding_ < maxSize_) {
            ++outstanding_;
            lock.unlock();
            try {
                auto conn = std::make_unique<pqxx::connection>(url_);
                return Lease(this, std::move(conn));
            } catch (...) {
                lock.lock();
                --outstanding_;
                available_.notify_one();
                throw;
            }
        }

        // At capacity: wait for someone to hand a connection back rather than
        // opening one the database may not have room for.
        if (!available_.wait_for(lock, std::chrono::seconds(5),
                                 [this] { return !idle_.empty() || outstanding_ < maxSize_; })) {
            throw std::runtime_error("timed out waiting for a database connection");
        }
    }
}

void ConnectionPool::release(std::unique_ptr<pqxx::connection> conn) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (outstanding_ > 0) {
        --outstanding_;
    }
    if (conn && conn->is_open()) {
        idle_.push_back(std::move(conn));
    }
    available_.notify_one();
}

std::size_t ConnectionPool::idleCount() {
    std::lock_guard<std::mutex> lock(mutex_);
    return idle_.size();
}

} // namespace nuigraph
