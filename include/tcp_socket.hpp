#ifndef SIMPLE_NET_LIB_TCP_SOCKET_HPP
#define SIMPLE_NET_LIB_TCP_SOCKET_HPP

#include "io_context.hpp"
#include <memory>
#include <mutex>
#include <functional>

namespace SNL1 {


class ContextImpl;

using DataHandler = std::function<void(EventType)>;
using AcceptHandler = std::function<void(EventType)>;

class Connection final : private CtxObject,
                         public EventObject,
                         public std::enable_shared_from_this<Connection> {

public:
    size_t hRead(void *buf, size_t len, int &ec);

    size_t hWrite(const void *buf, size_t len, int &ec);

    bool hIsReadClosed() const;

    bool hIsWriteClosed() const;

    void hSetRead(bool enable);

    void hSetWrite(bool enable);

    void hShutdown(bool rd, bool wr);

    void enableHandler(DataHandler handler, bool rd, bool wr);

    void enableRead(bool enable);

    void enableWrite(bool enable);

    void stop();

    ~Connection() override;

    friend class Listener;

private:
    int fd_;
    bool sIn_, eIn_, sOut_, eOut_;
    DataHandler handler_;
    std::recursive_mutex mutex_;

    Connection(ContextImpl *ctx, int fd);

    void handleEvent_(EventType type) override;

    void rearm_();

    void close_();

    void terminate_() override;

};


class Listener final : private CtxObject,
                       public EventObject,
                       public std::enable_shared_from_this<Listener> {
public:
    std::shared_ptr<Connection> hAccept(int &ec);

    void hSetAccept(bool enable);

    void hShutdown();

    void enableHandler(AcceptHandler handler);

    void enableAccept(bool enable);

    void stop();

    ~Listener() override;

    friend class ContextImpl;

private:
    int fd_;
    bool sAcc_, eAcc_;
    AcceptHandler handler_;
    std::recursive_mutex mutex_;

    Listener(ContextImpl *ctx, int fd);

    void handleEvent_(EventType type) override;

    void rearm_();

    void close_();

    void terminate_() override;

};


}

#endif //SIMPLE_NET_LIB_TCP_SOCKET_HPP
