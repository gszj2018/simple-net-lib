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
#include <queue>
#include "io_context.imp.hpp"
#include "io_context.hpp"
#include "common.hpp"
#include "tcp_socket.hpp"


namespace SNL1 {


EventQueue::EventQueue(int cap) :
        capacity_(cap), head_(0), tail_(0), size_(0), q_{},
        closed_(false), finish_(false) {
    assert(cap > 0);
    q_ = allocator_.allocate(capacity_);
}

void EventQueue::put(Event e) {
    std::unique_lock<std::mutex> lock(mutex_);
    cp_.wait(lock, [this]() { return closed_ || size_ < capacity_; });
    if (closed_)return;
    std::construct_at(q_ + tail_, std::move(e));
    tail_ = (tail_ + 1 == capacity_) ? 0 : (tail_ + 1);
    ++size_;
    cg_.notify_one();
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
    cp_.notify_one();
    return e;
}

void EventQueue::close() {
    std::lock_guard<std::mutex> lock(mutex_);
    closed_ = true;
    cp_.notify_all();
    cg_.notify_all();
}

void EventQueue::closeAndDiscard() {
    // stop accepting new event
    close();

    // clear event queue
    while (nullptr != get()) {}

    // mark as finished
    std::lock_guard<std::mutex> lock(mutex_);
    finish_ = true;
}

EventQueue::~EventQueue() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!finish_) {
        panic("EventQueue is not cleanly closed");
    }
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

void MultiThreadPoolExecutor::stop() {
    // mark as closed
    if (closed_.exchange(true, std::memory_order::acq_rel)) return;

    // wait for executor threads exiting
    for (auto &&t: pool_) t.join();
}

MultiThreadPoolExecutor::~MultiThreadPoolExecutor() {
    if (!closed_.load(std::memory_order::acquire)) {
        panic("MultiThreadPoolExecutor is not cleanly closed");
    }
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
    // mark as closed
    if (closed_.exchange(true, std::memory_order::acq_rel)) return;

    // we need to close event queue first
    // otherwise threads may block on get() or put()
    eventQueue_.closeAndDiscard();

    // stop event poller
    poller_.stop();

    // stop executor
    executor_.stop();
}

ContextImpl::~ContextImpl() {
    stop();
    for (;;) {
        int c = objectCnt_.load(std::memory_order::acquire);
        if (c == 0)break;
        Logger::global->log(LOG_WARN, std::to_string(c) + " sessions alive, cannot stop context");
        std::this_thread::sleep_for(std::chrono::seconds(10));
    }
}

std::shared_ptr<Listener> ContextImpl::newTcpServer(int port, int backlog, int &ec) {
    ec = 0;
    sockaddr_in6 addr{};
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

    return std::shared_ptr<Listener>(new Listener(this, fd));
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
        size_(pollSize), events_(pollSize), pending_(0) {
    pending_.reserve(pollSize);

    if ((epfd_ = epoll_create1(EPOLL_CLOEXEC)) < 0) {
        panic(strerror(errno));
    }

    if ((evfd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)) < 0) {
        panic(strerror(errno));
    }

    epoll_(evfd_, EPOLL_CTL_ADD, EPOLLONESHOT | EPOLLIN);

    thr_ = std::thread(&EventPoller::routine_, this);
}

void EventPoller::rearmEvent(int fd, bool in, bool out) {
    if (in || out) {
        uint32_t e = EPOLLONESHOT;
        if (in) e |= EPOLLIN;
        if (out)e |= EPOLLOUT;
        epoll_(fd, EPOLL_CTL_MOD, e);
    }
}

void EventPoller::registerObject(int fd, std::shared_ptr<EventObject> obj, bool in, bool out) {
    std::lock_guard<std::recursive_mutex> lock(mapMutex_);
    if (closed_.load(std::memory_order::acquire)) return;

    if (fdMap_.emplace(fd, std::move(obj)).second) {
        uint32_t e = EPOLLONESHOT;
        if (in) e |= EPOLLIN;
        if (out)e |= EPOLLOUT;
        epoll_(fd, EPOLL_CTL_ADD, e);
    }
}

void EventPoller::deregisterObject(int fd) {
    std::lock_guard<std::recursive_mutex> lock(mapMutex_);
    if (closed_.load(std::memory_order::acquire)) return;

    auto it = fdMap_.find(fd);
    if (it != fdMap_.end()) {
        fdMap_.erase(it);
        epoll_(fd, EPOLL_CTL_DEL, EPOLLONESHOT);
    }
}

void EventPoller::stop() {
    // mark as closed
    if (closed_.exchange(true, std::memory_order::acq_rel)) return;

    // use event fd to forcibly awake epoll
    uint64_t v = 1;
    if (write(evfd_, &v, sizeof(uint64_t)) < 0) {
        Logger::global->log(LOG_ERROR, strerror(errno));
    }

    // wait for poller thread exit
    thr_.join();

    // disengage all event receivers
    std::queue<std::shared_ptr<EventObject>> closing;
    std::unique_lock<std::recursive_mutex> lock(mapMutex_);
    for (auto &x: fdMap_) closing.push(x.second);
    fdMap_.clear();
    lock.unlock(); // mutex is no more needed

    // terminate all connections
    while (!closing.empty()) {
        closing.front()->terminate_();
        closing.pop();
    }
}

EventPoller::~EventPoller() {
    if (!closed_.load(std::memory_order::acquire)) {
        panic("EventPoller is not cleanly closed");
    }
    close(epfd_);
    close(evfd_);
}

void EventPoller::routine_() {
    for (;;) {
        int n = epoll_wait(epfd_, events_.data(), size_, -1);
        if (closed_.load(std::memory_order::acquire))break;
        if (n < 0) {
            if (errno != EINTR) {
                Logger::global->log(LOG_WARN, strerror(errno));
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        } else {
            // collect events
            std::unique_lock<std::recursive_mutex> lock(mapMutex_);
            for (int i = 0; i < n; ++i) {
                int fd = events_[i].data.fd;
                if (fd == evfd_) {
                    Logger::global->log(LOG_WARN, "event poller is signalled stopping.");
                    continue;
                }
                auto it = fdMap_.find(fd);
                if (it == fdMap_.end())continue;
                EventType e = 0;
                if (events_[i].events & EPOLLIN) e |= EVENT_IN;
                if (events_[i].events & EPOLLOUT)e |= EVENT_OUT;
                if (e) {
                    pending_.emplace_back([ptr = it->second, e]() {
                        ptr->handleEvent_(e);
                    });
                }
            }

            // call event handlers
            // since no need to access fd map, now unlock mutex
            lock.unlock();
            for (auto &&ev: pending_)
                q_->put(std::move(ev));
            pending_.clear();
        }
    }
}

void EventPoller::epoll_(int fd, int op, uint32_t e) const {
    epoll_event ev{};
    ev.data.fd = fd;
    ev.events = e;
    if (epoll_ctl(epfd_, op, fd, &ev) < 0) {
        Logger::global->log(LOG_WARN, strerror(errno));
    }
}


Context::Context(int threadCnt, int queueCapacity, int pollSize) :
        impl(std::make_unique<ContextImpl>(threadCnt, queueCapacity, pollSize)) {

}

std::shared_ptr<Listener> Context::newTcpServer(int port, int backlog, int &ec) {
    return impl->newTcpServer(port, backlog, ec);
}

void Context::stop() {
    impl->stop();
}

void Context::ignorePipeSignal() {
    struct sigaction act{};
    act.sa_flags = SA_RESTART;
    act.sa_handler = SIG_IGN;
    sigemptyset(&act.sa_mask);
    if (sigaction(SIGPIPE, &act, nullptr) < 0) {
        panic(strerror(errno));
    }
}

void Context::blockIntSignal() {
    sigset_t s{};
    sigemptyset(&s);
    sigaddset(&s, SIGINT);
    int ec;
    if (0 != (ec = pthread_sigmask(SIG_BLOCK, &s, nullptr))) {
        panic(strerror(ec));
    }
}

void Context::waitUntilInterrupt() {
    sigset_t s{};
    sigemptyset(&s);
    sigaddset(&s, SIGINT);
    int sig = 0;
    do {
        sigwait(&s, &sig);
    } while (sig != SIGINT);
}


Context::~Context() = default;

}
