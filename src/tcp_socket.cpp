#include "tcp_socket.hpp"
#include "io_context.imp.hpp"
#include <sys/socket.h>


namespace SNL1 {

TcpListener::TcpListener(ContextImpl *ctx, int fd) :
        CtxObject(ctx), fd_(fd),
        sAcc_(true) {
}

void TcpListener::handleEvent_(EventType type) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (fd_ < 0)return;
    if (sAcc_ && (type & EVENT_IN)) {
        handler_(EVENT_IN);
    }

    if (sAcc_) {
        ctx_->poller_.registerEvent(EventRegOp::REARM, fd_, EVENT_IN, nullptr);
    }
}

void TcpListener::enableHandler(std::function<void(EventType)> h) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (fd_ < 0)return;
    if (!handler_) {
        handler_ = std::move(h);
        ctx_->poller_.registerEvent(EventRegOp::REGISTER, fd_, EVENT_IN, shared_from_this());
    }
}

void TcpListener::stop() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    doClose_();
}

std::shared_ptr<TcpEndpoint> TcpListener::doAccept(int &ec) {
    ec = 0;
    if (!sAcc_)return nullptr;
    int afd = accept4(fd_, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (afd < 0) {
        if (errno == EINTR)return nullptr;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return nullptr;
        ec = errno;
        return nullptr;
    }
    return std::shared_ptr<TcpEndpoint>(new TcpEndpoint(ctx_, afd));
}

void TcpListener::doClose_() {
    if (fd_ < 0)return;
    sAcc_ = false;
    if (handler_) {
        ctx_->poller_.registerEvent(EventRegOp::DEREGISTER, fd_, EVENT_IN, nullptr);
    }
    close(fd_);
    fd_ = -1;
    handler_ = nullptr;
}


TcpListener::~TcpListener() {
    stop();
}

void TcpListener::terminate_() {
    stop();
}


TcpEndpoint::TcpEndpoint(ContextImpl *ctx, int fd) :
        CtxObject(ctx), fd_(fd),
        sIn_(true), sOut_(true), aOut_(false) {}

void TcpEndpoint::handleEvent_(EventType type) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (fd_ < 0)return;
    EventType e = 0;
    if (sIn_) e |= (type & EVENT_IN);
    if (sOut_) e |= (type & EVENT_OUT);
    handler_(e);

    if (!sIn_ && !sOut_) {
        doClose_();
    } else {
        doRearmEvent_();
    }

}

void TcpEndpoint::enableHandler(std::function<void(EventType)> h) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (fd_ < 0)return;
    assert(!handler_);
    if (!handler_) {
        handler_ = std::move(h);
        ctx_->poller_.registerEvent(EventRegOp::REGISTER, fd_, EVENT_IN | EVENT_OUT, shared_from_this());
    }
}

void TcpEndpoint::enableWritePolling() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (fd_ < 0 || !handler_)return;
    aOut_ = true;
    doRearmEvent_();
}

void TcpEndpoint::stop() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    doClose_();
}

void TcpEndpoint::setWritePolling(bool w) {
    aOut_ = w;
}

bool TcpEndpoint::isReadClosed() const {
    return !sIn_;
}

bool TcpEndpoint::isWriteClosed() const {
    return !sOut_;
}

void TcpEndpoint::setReadClosed() {
    sIn_ = false;
}

void TcpEndpoint::setWriteClosed() {
    if (fd_ >= 0 && sOut_) {
        shutdown(fd_, SHUT_WR);
    }
    aOut_ = sOut_ = false;
}

void TcpEndpoint::setBothClosed() {
    setReadClosed();
    setWriteClosed();
}

size_t TcpEndpoint::doRecv(void *buf, size_t len, int &ec) {
    ec = 0;
    if (!sIn_)return 0;
    ssize_t n = recv(fd_, buf, len, 0);
    if (n < 0) {
        if (errno == EINTR) return 0;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        if (errno == EPIPE || errno == ECONNRESET) sIn_ = false;
        ec = errno;
        return 0;
    }
    if (n == 0) {
        sIn_ = false;
    }
    return n;
}

size_t TcpEndpoint::doSend(const void *buf, size_t len, int &ec) {
    ec = 0;
    if (!sOut_)return 0;
    ssize_t n = send(fd_, buf, len, 0);
    if (n < 0) {
        if (errno == EINTR) return 0;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        if (errno == EPIPE || errno == ECONNRESET) sOut_ = false;
        ec = errno;
        return 0;
    }
    return n;
}

void TcpEndpoint::doRearmEvent_() {
    EventType waitFor = 0;
    waitFor |= sIn_ ? EVENT_IN : 0;
    waitFor |= (sOut_ && aOut_) ? EVENT_OUT : 0;

    if (waitFor) {
        ctx_->poller_.registerEvent(EventRegOp::REARM, fd_, waitFor, nullptr);
    }
}

void TcpEndpoint::doClose_() {
    if (fd_ < 0)return;
    aOut_ = sIn_ = sOut_ = false;
    if (handler_) {
        ctx_->poller_.registerEvent(EventRegOp::DEREGISTER, fd_, EVENT_IN | EVENT_OUT, nullptr);
    }
    close(fd_);
    fd_ = -1;
    handler_ = nullptr;
}

TcpEndpoint::~TcpEndpoint() {
    stop();
}

void TcpEndpoint::terminate_() {
    stop();
}


}