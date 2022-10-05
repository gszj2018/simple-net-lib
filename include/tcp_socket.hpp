#ifndef SIMPLE_NET_LIB_TCP_SOCKET_HPP
#define SIMPLE_NET_LIB_TCP_SOCKET_HPP

#include "io_context.hpp"
#include <memory>
#include <mutex>
#include <functional>

namespace SNL1 {


class ContextImpl;

class TcpEndpoint final : private CtxObject,
                          public EventObject,
                          public std::enable_shared_from_this<TcpEndpoint> {

public:
    void enableHandler(std::function<void(EventType)> h);

    void enableWritePolling();

    void setWritePolling(bool w);

    bool isReadClosed() const;

    bool isWriteClosed() const;

    void setReadClosed();

    void setWriteClosed();

    void setBothClosed();

    size_t doSend(const void *buf, size_t len, int &ec);

    size_t doRecv(void *buf, size_t len, int &ec);

    void stop();

    ~TcpEndpoint() override;

    friend class TcpListener;

    friend class ContextImpl;

private:
    int fd_;
    bool sIn_, sOut_, aOut_;
    std::function<void(EventType)> handler_;
    std::recursive_mutex mutex_;

    TcpEndpoint(ContextImpl *ctx, int fd);

    void handleEvent_(EventType type) override;

    void doRearmEvent_();

    void doClose_();

    void terminate_() override;

};

class TcpListener final : private CtxObject,
                          public EventObject,
                          public std::enable_shared_from_this<TcpListener> {
public:
    void enableHandler(std::function<void(EventType)> h);

    std::shared_ptr<TcpEndpoint> doAccept(int &ec);

    void stop();

    ~TcpListener() override;

    friend class ContextImpl;

private:
    int fd_;
    bool sAcc_;
    std::function<void(EventType)> handler_;
    std::recursive_mutex mutex_;

    TcpListener(ContextImpl *ctx, int fd);

    void handleEvent_(EventType type) override;

    void doClose_();

    void terminate_() override;

};


}

#endif //SIMPLE_NET_LIB_TCP_SOCKET_HPP
