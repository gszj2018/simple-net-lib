#include "logger.hpp"
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/eventfd.h>
#include <netdb.h>
#include <unistd.h>
#include <signal.h>
#include <cstring>
#include <thread>
#include <chrono>
#include "io_context.imp.hpp"
#include "io_context.hpp"
#include "common.hpp"
#include "tcp_socket.hpp"


namespace SNL1 {


EventQueue::EventQueue(int cap) :
        capacity_(cap), head_(0), tail_(0), size_(0), q_{},
        closed_(false) {
    assert(cap > 0);
    std::unique_lock<std::mutex> lock(mutex_);
    q_ = allocator_.allocate(capacity_);
}

void EventQueue::put(Event e) {
    std::unique_lock<std::mutex> lock(mutex_);
    cp_.wait(lock, [this]() { return closed_ || size_ < capacity_; });
    if (closed_)return;
    std::construct_at(q_ + tail_, std::move(e));
    tail_ = (tail_ + 1 == capacity_) ? 0 : (tail_ + 1);
    ++size_;
    cg_.notify_all();
}

Event EventQueue::get() {
    std::unique_lock<std::mutex> lock(mutex_);
    cg_.wait(lock, [this]() { return closed_ || size_ > 0; });
    if (size_ == 0) {
        return nullptr;
    }
    Event e{std::move(q_[head_])};
    std::destroy_at(q_ + head_);
    head_ = (head_ + 1 == capacity_) ? 0 : (head_ + 1);
    --size_;
    cp_.notify_all();
    return e;
}

void EventQueue::close() {
    std::unique_lock<std::mutex> lock(mutex_);
    closed_ = true;
    cp_.notify_all();
    cg_.notify_all();
}

void EventQueue::closeAndDiscard() {
    close();
    while (nullptr != get()) {}
}

EventQueue::~EventQueue() {
    closeAndDiscard();
    std::unique_lock<std::mutex> lock(mutex_);
    allocator_.deallocate(q_, (size_t) capacity_);
}


MultiThreadPoolExecutor::MultiThreadPoolExecutor(int threadCnt, EventQueue *q) :
        q_(q), closed_(false) {
    assert(threadCnt > 0);
    pool_.reserve((size_t) threadCnt);
    for (int i = 0; i < threadCnt; ++i) {
        pool_.emplace_back(&MultiThreadPoolExecutor::routine_, this);
    }
}

void MultiThreadPoolExecutor::close() {
    closed_.store(true, std::memory_order_release);
}

MultiThreadPoolExecutor::~MultiThreadPoolExecutor() {
    close();
}

void MultiThreadPoolExecutor::routine_() {
    while (!closed_.load(std::memory_order_acquire)) {
        auto h = q_->get();
        if (!h)break;
        h();
    }
}

ContextImpl::ContextImpl(int threadCnt_, int queueCapacity_, int pollSize_) :
        closed_(false), objectCnt_(0),
        eventQueue_(queueCapacity_), executor_(threadCnt_, &eventQueue_),
        poller_(&eventQueue_, pollSize_) {
}

void ContextImpl::stop() {
    closed_.store(true, std::memory_order::release);
    poller_.stop();
    executor_.close();
    eventQueue_.closeAndDiscard();
}

ContextImpl::~ContextImpl() {
    using namespace std::chrono_literals;
    stop();
    while (objectCnt_.load(std::memory_order::acquire) > 0) {
        Logger::global->log(LOG_WARN, "sessions are still alive, cannot stop context");
        std::this_thread::sleep_for(30s);
    }
}

std::shared_ptr<TcpListener> ContextImpl::createTcpServer(int port, int &ec, int backlog) {
    ec = 0;
    sockaddr_in6 addr{};
    memset(&addr, 0, sizeof(sockaddr_in6));
    addr.sin6_family = AF_INET6;
    addr.sin6_addr = IN6ADDR_ANY_INIT;
    addr.sin6_port = htons((uint16_t) port);
    int fd, val1 = 1, val0 = 0;
    if ((fd = socket(AF_INET6, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0)) < 0) {
        ec = errno;
        return nullptr;
    }

    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &val1, sizeof(int)) < 0) {
        ec = errno;
        close(fd);
        return nullptr;
    }

    if (setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &val0, sizeof(int)) < 0) {
        ec = errno;
        close(fd);
        return nullptr;
    }

    if (bind(fd, (sockaddr *) &addr, sizeof(sockaddr_in6)) < 0) {
        ec = errno;
        close(fd);
        return nullptr;
    }

    if (listen(fd, backlog) < 0) {
        ec = errno;
        close(fd);
        return nullptr;
    }

    return std::shared_ptr<TcpListener>(new TcpListener(this, fd));
}


CtxObject::CtxObject(ContextImpl *ctx) :
        ctx_(ctx) {
    assert(!ctx_->closed_.load(std::memory_order::acquire));
    ctx_->objectCnt_.fetch_add(1, std::memory_order::acq_rel);
}

CtxObject::~CtxObject() {
    ctx_->objectCnt_.fetch_sub(1, std::memory_order::acq_rel);
}

EventObject::~EventObject() = default;

EventPoller::EventPoller(EventQueue *q, int pollSize) :
        q_(q),
        closed_(false),
        size_(pollSize),
        events_(std::make_unique<epoll_event[]>(pollSize)) {
    if ((epfd_ = epoll_create1(0)) < 0) {
        panic(strerror(errno));
    }

    if ((evfd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)) < 0) {
        panic(strerror(errno));
    }

    {
        epoll_event ev{};
        std::memset(&ev, 0, sizeof(epoll_event));
        ev.data.fd = evfd_;
        ev.events = EPOLLONESHOT | EPOLLIN;
        if (epoll_ctl(epfd_, EPOLL_CTL_ADD, evfd_, &ev) < 0) {
            panic(strerror(errno));
        }
    }

    thr_ = std::thread(&EventPoller::routine_, this);
}


void EventPoller::registerEvent(EventRegOp op, int fd, EventType type, std::shared_ptr<EventObject> ptr) {
    if (op != EventRegOp::REARM) {
        std::lock_guard<std::recursive_mutex> lock(mapMutex_);
        if (closed_.load(std::memory_order::acquire)) return;
        if (op == EventRegOp::REGISTER) {
            fdMap_[fd] = std::move(ptr);
        } else if (op == EventRegOp::DEREGISTER) {
            fdMap_.erase(fd);
        }
    } else {
        if (closed_.load(std::memory_order::acquire)) return;
    }
    constexpr int EPOLL_OP_MAP[] = {EPOLL_CTL_ADD, EPOLL_CTL_MOD, EPOLL_CTL_DEL};
    int epOp = EPOLL_OP_MAP[static_cast<int>(op)];
    epoll_event ev{};
    std::memset(&ev, 0, sizeof(epoll_event));
    ev.data.fd = fd;
    ev.events = EPOLLONESHOT;
    ev.events |= (type & EVENT_IN) ? EPOLLIN : 0;
    ev.events |= (type & EVENT_OUT) ? EPOLLOUT : 0;
    if (epoll_ctl(epfd_, epOp, fd, &ev) < 0) {
        Logger::global->log(LOG_WARN, strerror(errno));
    }
}

void EventPoller::stop() {
    closed_.store(true, std::memory_order::release);
    {
        std::lock_guard<std::recursive_mutex> lock(mapMutex_);
        for (auto &x: fdMap_) {
            x.second->terminate_();
        }
        fdMap_.clear();
    }
    {
        uint64_t v = 1;
        write(evfd_, &v, sizeof(uint64_t));
    }
}

EventPoller::~EventPoller() {
    stop();
    thr_.join();
    close(epfd_);
    close(evfd_);
}

void EventPoller::routine_() {
    for (;;) {
        int n = epoll_wait(epfd_, events_.get(), size_, -1);
        if (closed_.load(std::memory_order::acquire))break;
        if (n < 0) {
            Logger::global->log(LOG_WARN, strerror(errno));
        } else {
            std::lock_guard<std::recursive_mutex> lock(mapMutex_);
            for (int i = 0; i < n; ++i) {
                int fd = events_[i].data.fd;
                if (fd == evfd_) {
                    Logger::global->log(LOG_WARN, "event poller is signalled stopping.");
                    continue;
                }
                auto it = fdMap_.find(fd);
                if (it == fdMap_.end())continue;
                EventType type = 0;
                type |= (events_[i].events & EPOLLIN) ? EVENT_IN : 0;
                type |= (events_[i].events & EPOLLOUT) ? EVENT_OUT : 0;
                q_->put([ptr = it->second, type]() {
                    ptr->handleEvent_(type);
                });
            }
        }
    }
}


Context::Context(int threadCnt, int queueCapacity, int pollSize) :
        impl(std::make_unique<ContextImpl>(threadCnt, queueCapacity, pollSize)) {

}

std::shared_ptr<TcpListener> Context::createTcpServer(int port, int &ec, int backlog) {
    return impl->createTcpServer(port, ec, backlog);
}

void Context::stop() {
    impl->stop();
}

void Context::ignorePipeSignal() {
    struct sigaction act{};
    memset(&act, 0, sizeof(act));
    act.sa_flags = SA_RESTART;
    act.sa_handler = SIG_IGN;
    sigemptyset(&act.sa_mask);
    if (sigaction(SIGPIPE, &act, nullptr) < 0) {
        panic(strerror(errno));
    }
}

Context::~Context() = default;

}
