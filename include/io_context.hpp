#ifndef SIMPLE_NET_LIB_IO_CONTEXT_HPP
#define SIMPLE_NET_LIB_IO_CONTEXT_HPP

#include "common.hpp"
#include <memory>

namespace SNL1 {

using EventType = unsigned int;
constexpr unsigned EVENT_IN = 1;
constexpr unsigned EVENT_OUT = 2;

class ContextImpl;

class TcpListener;

class TcpEndpoint;

class CtxObject : private DisableCopy {
public:
    class ContextImpl *const ctx_;

    explicit CtxObject(ContextImpl *ctx);

    ~CtxObject();

};

class EventObject {
public:
    virtual ~EventObject();

    friend class EventPoller;

private:
    virtual void handleEvent_(EventType type) = 0;

    virtual void terminate_() = 0;

};


class Context final {

public:
    Context(int threadCnt, int queueCapacity, int pollSize);

    std::shared_ptr<TcpListener> createTcpServer(int port, int &ec, int backlog);

    void stop();

    ~Context();

    static void ignorePipeSignal();

private:
    std::unique_ptr<ContextImpl> impl;
};

}

#endif //SIMPLE_NET_LIB_IO_CONTEXT_HPP
