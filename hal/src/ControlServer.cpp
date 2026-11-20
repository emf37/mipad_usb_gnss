#define LOG_TAG "gnss_usb"

#include "ControlServer.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <log/log.h>

#include <algorithm>

namespace aidl::android::hardware::gnss {

namespace {
int hexChar(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
}  // namespace

void ControlServer::start(int port) {
    if (mRunning.exchange(true)) return;

    mListenFd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (mListenFd < 0) {
        ALOGE("control: socket failed: %s", strerror(errno));
        mRunning = false;
        return;
    }
    int one = 1;
    setsockopt(mListenFd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // local only
    addr.sin_port = htons((uint16_t)port);
    if (bind(mListenFd, (struct sockaddr*)&addr, sizeof(addr)) != 0 ||
        listen(mListenFd, 4) != 0) {
        ALOGE("control: bind/listen 127.0.0.1:%d failed: %s", port, strerror(errno));
        ::close(mListenFd);
        mListenFd = -1;
        mRunning = false;
        return;
    }
    ALOGI("control server listening on 127.0.0.1:%d", port);
    mAcceptThread = std::thread(&ControlServer::acceptLoop, this);
}

void ControlServer::stop() {
    mRunning = false;
    if (mListenFd >= 0) {
        ::shutdown(mListenFd, SHUT_RDWR);
        ::close(mListenFd);
        mListenFd = -1;
    }
    if (mAcceptThread.joinable()) mAcceptThread.join();
    std::lock_guard<std::mutex> lock(mClientsMutex);
    for (int fd : mClients) ::close(fd);
    mClients.clear();
}

void ControlServer::acceptLoop() {
    while (mRunning) {
        int fd = ::accept(mListenFd, nullptr, nullptr);
        if (fd < 0) {
            if (!mRunning) break;
            continue;
        }
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        std::thread(&ControlServer::clientLoop, this, fd).detach();
    }
}

void ControlServer::clientLoop(int fd) {
    bool subscribed = false;
    char buf[1024];
    std::string pending;
    bool open = true;
    while (open && mRunning) {
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        pending.append(buf, (size_t)n);

        size_t nl;
        while ((nl = pending.find('\n')) != std::string::npos) {
            std::string line = pending.substr(0, nl);
            pending.erase(0, nl + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();

            if (line == "STATUS") {
                std::string s = mStatus ? mStatus() : std::string("{}");
                s += "\n";
                if (::send(fd, s.data(), s.size(), MSG_NOSIGNAL) < 0) open = false;
            } else if (line.rfind("SEND ", 0) == 0) {
                std::string payload = line.substr(5);
                bool ok = mSend ? mSend(payload) : false;
                std::string r = ok ? "OK\n" : "ERR\n";
                if (::send(fd, r.data(), r.size(), MSG_NOSIGNAL) < 0) open = false;
            } else if (line.rfind("SENDBIN ", 0) == 0) {
                // hex-encoded binary write (AGNSS frames, CASIC payloads etc.)
                std::string hex = line.substr(8);
                std::string bin;
                bin.reserve(hex.size() / 2);
                for (size_t i = 0; i + 1 < hex.size(); i += 2) {
                    int hi = hexChar(hex[i]);
                    int lo = hexChar(hex[i + 1]);
                    if (hi < 0 || lo < 0) {
                        bin.clear();
                        break;
                    }
                    bin.push_back((char)((hi << 4) | lo));
                }
                bool ok = !bin.empty() && mSend ? mSend(bin) : false;
                std::string r = ok ? "OK " + std::to_string(bin.size()) + "B\n" : "ERR\n";
                if (::send(fd, r.data(), r.size(), MSG_NOSIGNAL) < 0) open = false;
            } else if (line == "SUB") {
                subscribed = true;
                std::lock_guard<std::mutex> lock(mClientsMutex);
                mClients.push_back(fd);
                if (::send(fd, "OK\n", 3, MSG_NOSIGNAL) < 0) open = false;
            } else if (line == "UNSUB") {
                subscribed = false;
                std::lock_guard<std::mutex> lock(mClientsMutex);
                mClients.erase(std::remove(mClients.begin(), mClients.end(), fd),
                               mClients.end());
                if (::send(fd, "OK\n", 3, MSG_NOSIGNAL) < 0) open = false;
            }
        }
    }
    if (subscribed) {
        std::lock_guard<std::mutex> lock(mClientsMutex);
        mClients.erase(std::remove(mClients.begin(), mClients.end(), fd), mClients.end());
    }
    ::close(fd);
}

void ControlServer::broadcastNmea(const std::string& line) {
    std::string msg = "NMEA " + line + "\n";
    std::lock_guard<std::mutex> lock(mClientsMutex);
    for (auto it = mClients.begin(); it != mClients.end();) {
        if (::send(*it, msg.data(), msg.size(), MSG_NOSIGNAL) < 0) {
            ::close(*it);
            it = mClients.erase(it);
        } else {
            ++it;
        }
    }
}

}  // namespace aidl::android::hardware::gnss
