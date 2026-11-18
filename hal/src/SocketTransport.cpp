#define LOG_TAG "gnss_usb_dev"

#include "SocketTransport.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <log/log.h>

namespace aidl::android::hardware::gnss {

SocketTransport::SocketTransport(std::string addr) : mAddr(std::move(addr)) {}

bool SocketTransport::open() {
    bool isUnix = mAddr.rfind("unix:", 0) == 0;
    std::string hostport = isUnix ? mAddr.substr(5) : mAddr;

    if (isUnix) {
        struct sockaddr_un addr = {};
        addr.sun_family = AF_UNIX;
        if (hostport.empty() || hostport.size() >= sizeof(addr.sun_path)) {
            ALOGE("bad unix socket path '%s'", hostport.c_str());
            return false;
        }
        strncpy(addr.sun_path, hostport.c_str(), sizeof(addr.sun_path) - 1);
        mFd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (mFd < 0 || connect(mFd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
            ALOGE("connect %s failed: %s", mAddr.c_str(), strerror(errno));
            if (mFd >= 0) ::close(mFd);
            mFd = -1;
            return false;
        }
    } else {
        std::string host = hostport, port = "5000";
        if (hostport.rfind("tcp://", 0) == 0) hostport = hostport.substr(6);
        auto colon = hostport.rfind(':');
        if (colon != std::string::npos) {
            host = hostport.substr(0, colon);
            port = hostport.substr(colon + 1);
        }
        struct addrinfo hints = {}, *res = nullptr;
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        int rc = getaddrinfo(host.c_str(), port.c_str(), &hints, &res);
        if (rc != 0 || res == nullptr) {
            ALOGE("resolve %s:%s failed: %s", host.c_str(), port.c_str(), gai_strerror(rc));
            return false;
        }
        mFd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (mFd < 0 || connect(mFd, res->ai_addr, res->ai_addrlen) != 0) {
            ALOGE("connect %s:%s failed: %s", host.c_str(), port.c_str(), strerror(errno));
            freeaddrinfo(res);
            if (mFd >= 0) ::close(mFd);
            mFd = -1;
            return false;
        }
        freeaddrinfo(res);
    }

    mName = "socket " + mAddr;
    mOpen = true;
    ALOGI("connected %s", mName.c_str());
    return true;
}

void SocketTransport::close() {
    if (mFd >= 0) {
        ::close(mFd);
        mFd = -1;
    }
    mOpen = false;
}

ssize_t SocketTransport::read(char* buf, size_t len) {
    if (mFd < 0) return -1;
    ssize_t n = readPushback(buf, len);
    if (n != 0) return n;
    do {
        n = ::recv(mFd, buf, len, 0);
    } while (n < 0 && errno == EINTR);
    if (n == 0) return -1;  // peer closed -> transport loss
    return n;
}

ssize_t SocketTransport::write(const char* buf, size_t len) {
    if (mFd < 0) return -1;
    return ::send(mFd, buf, len, MSG_NOSIGNAL);
}

}  // namespace aidl::android::hardware::gnss
