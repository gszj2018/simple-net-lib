#include <cstring>
#include "tcp_socket.hpp"
#include "logger.hpp"

using namespace SNL1;


const char *GREET_STR = "Hello, world!\n";
const size_t GREET_LEN = strlen(GREET_STR);

int main() {
    int port = 12322, ec;
    Context::ignorePipeSignal();
    Context::blockIntSignal();

    Context ctx(4, 1024, 1024);
    std::shared_ptr<Listener> listener = ctx.newTcpServer(port, 128, ec);
    if (!listener) {
        panic(strerror(ec));
    }

    listener->enableHandler([listener](EventType e) {
        if (e & EVENT_IN) {
            int ec;
            std::shared_ptr<Connection> session;
            while ((session = listener->hAccept(ec))) {
                Logger::global->log(SNL1::LOG_INFO, "incoming connection");
                session->enableHandler([session, offset = size_t(0)](EventType e)mutable {
                    int ec;
                    if (e & EVENT_OUT) {
                        size_t n = session->hWrite(GREET_STR + offset, GREET_LEN - offset, ec);
                        if (n > 0) {
                            offset += n;
                        } else {
                            if (ec) {
                                Logger::global->log(SNL1::LOG_WARN, strerror(ec));
                                session->hShutdown(true, true);
                            }
                        }
                        if (offset == GREET_LEN) {
                            Logger::global->log(SNL1::LOG_INFO, "served one user");
                            session->hShutdown(true, true);
                        }
                    }
                }, false, true);
            }
            if (ec) {
                Logger::global->log(LOG_WARN, strerror(ec));
            }
        }
    });


    Logger::global->log(LOG_INFO, "HelloWorld server started on port 12322, CTRL-C to stop");
    Context::waitUntilInterrupt();
    Logger::global->log(LOG_INFO, "server interrupted, exiting...");
    listener.reset();
    ctx.stop();
    return 0;
}
