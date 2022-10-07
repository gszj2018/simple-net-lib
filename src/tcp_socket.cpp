#include "tcp_socket.hpp"
#include "io_context.imp.hpp"
#include <sys/socket.h>


namespace SNL1 {

Listener::Listener(ContextImpl *ctx, int fd) :
        CtxObject(ctx),
        fd_(fd), sAcc_(true), eAcc_(false),
        handler_(nullptr) {
    ctx_->poller_.registerEvent(EventRegOp::REGISTER, fd_, 0);
}

void Listener::handleEvent_(EventType type) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (fd_ < 0)return;
    if (sAcc_ && eAcc_ && (type & EVENT_IN)) handler_(EVENT_IN);

    rearm_();
    if (!sAcc_)close_();
}

void Listener::rearm_() {
    if (sAcc_ && eAcc_) {
        ctx_->poller_.registerEvent(EventRegOp::REARM, fd_, EVENT_IN);
    }
}

void Listener::close_() {
    ctx_->poller_.registerEvent(EventRegOp::DEREGISTER, fd_, 0);
    ctx_->poller_.deregisterObject(fd_);
    close(fd_);
    fd_ = -1;
    handler_ = nullptr;
}

void Listener::terminate_() { stop(); }

Listener::~Listener() { stop(); }

void Listener::stop() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (fd_ < 0)return;
    close_();
}

void Listener::enableHandler(SNL1::AcceptHandler handler) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (fd_ < 0)return;
    handler_ = std::move(handler);
    eAcc_ = true;
    ctx_->poller_.registerObject(fd_, shared_from_this());
    rearm_();
}

void Listener::enableAccept(bool enable) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (fd_ < 0)return;
    eAcc_ = enable;
    rearm_();
}

std::shared_ptr<Connection> Listener::hAccept(int &ec) {
    ec = 0;
    int afd = accept4(fd_, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (afd < 0) {
        if (errno == EINTR)return nullptr;
        if (errno == EAGAIN || errno == EWOULDBLOCK)return nullptr;
        ec = errno;
        return nullptr;
    }

    return std::shared_ptr<Connection>(new Connection(ctx_, afd));
}

void Listener::hSetAccept(bool enable) { eAcc_ = enable; }

void Listener::hShutdown() { sAcc_ = false; }


Connection::Connection(ContextImpl *ctx, int fd) :
        CtxObject(ctx),
        fd_(fd),
        sIn_(true), eIn_(false), sOut_(true), eOut_(false),
        handler_([](EventType) {}) {
    ctx_->poller_.registerEvent(EventRegOp::REGISTER, fd_, 0);
}

void Connection::handleEvent_(EventType type) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (fd_ < 0)return;
    if (sIn_ && eIn_ && (type & EVENT_IN)) handler_(EVENT_IN);
    if (sOut_ && eOut_ && (type & EVENT_OUT)) handler_(EVENT_OUT);

    rearm_();
    if (!sIn_ && !sOut_)close_();
}

void Connection::rearm_() {
    EventType e = 0;
    e |= (sIn_ && eIn_) ? EVENT_IN : 0;
    e |= (sOut_ && eOut_) ? EVENT_OUT : 0;

    if (e)ctx_->poller_.registerEvent(EventRegOp::REARM, fd_, e);
}

void Connection::close_() {
    ctx_->poller_.registerEvent(EventRegOp::DEREGISTER, fd_, 0);
    ctx_->poller_.deregisterObject(fd_);
    close(fd_);
    fd_ = -1;
    handler_ = nullptr;
}

void Connection::terminate_() { stop(); }

Connection::~Connection() { stop(); }

void Connection::stop() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (fd_ < 0)return;
    close_();
}

void Connection::enableHandler(SNL1::DataHandler handler, bool rd, bool wr) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (fd_ < 0)return;
    handler_ = std::move(handler);
    eIn_ = rd, eOut_ = wr;
    ctx_->poller_.registerObject(fd_, shared_from_this());
    rearm_();
}

void Connection::enableRead(bool enable) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (fd_ < 0)return;
    eIn_ = enable;
    rearm_();
}

void Connection::enableWrite(bool enable) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (fd_ < 0)return;
    eOut_ = enable;
    rearm_();
}

size_t Connection::hRead(void *buf, size_t len, int &ec) {
    ec = 0;
    if (!sIn_)return 0;
    if (0 == len)return 0;
    ssize_t n = recv(fd_, buf, len, 0);
    if (n < 0) { // error
        if (errno == EINTR)return 0;
        if (errno == EAGAIN || errno == EWOULDBLOCK)return 0;
        if (errno == EPIPE || errno == ECONNRESET)sIn_ = false;
        ec = errno;
        return 0;
    }
    if (n == 0) { // EOF
        sIn_ = false;
    }
    return n;
}

size_t Connection::hWrite(const void *buf, size_t len, int &ec) {
    ec = 0;
    if (!sOut_)return 0;
    ssize_t n = send(fd_, buf, len, 0);
    if (n < 0) {
        if (errno == EINTR)return 0;
        if (errno == EAGAIN || errno == EWOULDBLOCK)return 0;
        if (errno == EPIPE || errno == ECONNRESET)sOut_ = false;
        ec = errno;
        return 0;
    }
    return n;
}

bool Connection::hIsReadClosed() const { return !sIn_; }

bool Connection::hIsWriteClosed() const { return !sOut_; }

void Connection::hSetRead(bool enable) { eIn_ = enable; }

void Connection::hSetWrite(bool enable) { eOut_ = enable; }

void Connection::hShutdown(bool rd, bool wr) {
    if (rd)sIn_ = false;
    if (wr) {
        if (sOut_)shutdown(fd_, SHUT_WR);
        sOut_ = false;
    }
}

}