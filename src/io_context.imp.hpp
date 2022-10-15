#ifndef SIMPLE_NET_LIB_IO_CONTEXT_IMP_HPP
#define SIMPLE_NET_LIB_IO_CONTEXT_IMP_HPP

#include "common.hpp"
#include "io_context.hpp"
#include <mutex>
#include <thread>
#include <condition_variable>
#include <functional>
#include <cassert>
#include <atomic>
#include <shared_mutex>
#include <sys/epoll.h>
#include <unordered_map>
#include <list>


namespace SNL1 {

using Event = std::function<void()>;


class EventQueue final : private DisableCopy {

public:
    explicit EventQueue(int cap);

    void put(Event e);

    Event get();

    void close();

    void closeAndDiscard();

    ~EventQueue();


private:
    const int capacity_;
    int head_, tail_, size_;
    std::mutex mutex_;
    std::condition_variable cp_, cg_;
    Event *q_;
    std::allocator<Event> allocator_;
    bool closed_, finish_;
};


class MultiThreadPoolExecutor final : private DisableCopy {
public:

    MultiThreadPoolExecutor(int threadCnt, EventQueue *q);

    void stop();

    ~MultiThreadPoolExecutor();

private:
    EventQueue *q_;
    std::atomic_bool closed_;
    std::vector<std::thread> pool_;

    void routine_();
};


class EventPoller final {
public:
    explicit EventPoller(EventQueue *q, int pollSize);

    void rearmEvent(int fd, bool in, bool out);

    void registerObject(int fd, std::shared_ptr<EventObject> obj, bool in, bool out);

    void deregisterObject(int fd);

    void stop();

    ~EventPoller();

private:
    EventQueue *q_;
    int epfd_, evfd_;
    std::atomic_bool closed_;
    std::unordered_map<int, std::shared_ptr<EventObject>> fdMap_;
    std::recursive_mutex mapMutex_;

    int size_;
    std::unique_ptr<epoll_event[]> events_;

    std::thread thr_;

    void routine_();

    void epoll_(int fd, int op, uint32_t e) const;

};


class ContextImpl final : private DisableCopy {
public:
    std::atomic_bool closed_;
    std::atomic_int objectCnt_;
    EventQueue eventQueue_;
    MultiThreadPoolExecutor executor_;
    EventPoller poller_;

    ContextImpl(int threadCnt_, int queueCapacity_, int pollSize_);

    std::shared_ptr<Listener> newTcpServer(int port, int backlog, int &ec);

    void stop();

    ~ContextImpl();

};


}

#endif //SIMPLE_NET_LIB_IO_CONTEXT_IMP_HPP
