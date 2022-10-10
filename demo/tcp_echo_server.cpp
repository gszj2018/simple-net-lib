#include <cstring>
#include <queue>
#include <list>
#include "tcp_socket.hpp"
#include "logger.hpp"


using namespace SNL1;


struct Message {
    std::unique_ptr<char[]> buf;
    size_t offset, size;
};

class EchoSession : public std::enable_shared_from_this<EchoSession> {
public:
    static constexpr int BUF_SIZE = 104857600;

    explicit EchoSession(std::shared_ptr<Connection> net_) :
            net_(std::move(net_)), buf_(std::make_unique<char[]>(BUF_SIZE)) {
    }

    void start() {
        net_->enableHandler([ptr = shared_from_this()](EventType e) {
            ptr->handler_(e);
        }, true, false);
    }

    ~EchoSession() {
        Logger::global->log(LOG_INFO, "session destroyed");
    }

private:
    std::shared_ptr<Connection> net_;
    std::queue<Message> pending_;
    std::unique_ptr<char[]> buf_;

    void handler_(EventType e) {
        int ec;
        if (e & EVENT_IN) {
            for (;;) {
                size_t n = net_->hRead(buf_.get(), BUF_SIZE, ec);
                if (n > 0) {
                    std::unique_ptr<char[]> b = std::make_unique<char[]>(n);
                    memcpy(b.get(), buf_.get(), n);
                    pending_.emplace(std::move(b), 0, n);
                    net_->hSetWrite(true);
                } else {
                    if (ec) {
                        Logger::global->log(LOG_WARN, strerror(ec));
                    }
                    break;
                }
            }
        }
        if (e & EVENT_OUT) {
            while (!pending_.empty()) {
                Message *m = &pending_.front();
                size_t n = net_->hWrite(m->buf.get() + m->offset, m->size - m->offset, ec);
                if (n > 0) {
                    m->offset += n;
                    if (m->offset == m->size)pending_.pop();
                } else {
                    if (ec) {
                        Logger::global->log(LOG_WARN, strerror(ec));
                    }
                    break;
                }
            }
            if (pending_.empty()) net_->hSetWrite(false);
        }

        if (net_->hIsReadClosed()) {
            if (pending_.empty()) {
                Logger::global->log(LOG_INFO, "disconnect client");
                net_->hShutdown(true, true);
            } else if (net_->hIsWriteClosed()) {
                Logger::global->log(LOG_INFO, "client disconnected prematurely");
                pending_ = std::queue<Message>();
            }
        }
    }
};


int main() {
    int port = 12321, ec;
    Context::ignorePipeSignal();
    Context::blockIntSignal();

    Context ctx(4, 1024, 128);
    std::shared_ptr<Listener> listener = ctx.newTcpServer(port, 128, ec);
    if (!listener) {
        panic(strerror(ec));
    }

    listener->enableHandler([listener](EventType e) {
        if (e & EVENT_IN) {
            int ec;
            std::shared_ptr<Connection> session;
            while ((session = listener->hAccept(ec))) {
                Logger::global->log(LOG_INFO, "incoming connection");
                std::shared_ptr<EchoSession> s = std::make_shared<EchoSession>(session);
                s->start();
            }
            if (ec) {
                Logger::global->log(LOG_WARN, strerror(ec));
            }
        }
    });


    Logger::global->log(LOG_INFO, "echo server started on port 12321, CTRL-C to stop");
    Context::waitUntilInterrupt();
    Logger::global->log(LOG_INFO, "server interrupted, exiting...");
    listener.reset();
    ctx.stop();
}

