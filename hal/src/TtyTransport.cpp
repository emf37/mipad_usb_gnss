#define LOG_TAG "gnss_usb_dev"

#include "TtyTransport.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>

#include <log/log.h>

#include <algorithm>
#include <vector>

namespace aidl::android::hardware::gnss {

namespace {

speed_t baudConst(int b) {
    switch (b) {
        case 4800: return B4800;
        case 9600: return B9600;
        case 19200: return B19200;
        case 38400: return B38400;
        case 57600: return B57600;
        case 115200: return B115200;
        case 230400: return B230400;
        case 460800: return B460800;
        case 921600: return B921600;
        default: return 0;
    }
}

// Small sysfs attribute, trimmed; "" if absent/unreadable.
std::string readSysfsAttr(const std::string& path) {
    int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return "";
    char buf[32];
    ssize_t n = ::read(fd, buf, sizeof(buf) - 1);
    ::close(fd);
    if (n <= 0) return "";
    buf[n] = '\0';
    std::string s(buf);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return s;
}

// True if the tty's ancestor USB device has the given idVendor/idProduct.
bool ttyMatchesUsbId(const std::string& name, unsigned vid, unsigned pid) {
    std::string node = "/sys/class/tty/" + name + "/device";
    for (int depth = 0; depth < 4; ++depth) {
        std::string v = readSysfsAttr(node + "/idVendor");
        std::string p = readSysfsAttr(node + "/idProduct");
        if (!v.empty() && !p.empty()) {
            return strtoul(v.c_str(), nullptr, 16) == vid &&
                   strtoul(p.c_str(), nullptr, 16) == pid;
        }
        node += "/..";
    }
    return false;
}

std::string findUsbSerialDevice(unsigned vid, unsigned pid) {
    DIR* dir = opendir("/sys/class/tty");
    if (dir == nullptr) return "";
    std::vector<std::string> matches;
    while (struct dirent* e = readdir(dir)) {
        if (e->d_name[0] == '.') continue;
        if (ttyMatchesUsbId(e->d_name, vid, pid)) matches.emplace_back(e->d_name);
    }
    closedir(dir);
    if (matches.empty()) return "";
    std::sort(matches.begin(), matches.end());
    return matches.front();
}

// Non-blocking drain used by the auto-baud probe.
ssize_t readNibble(int fd, char* buf, size_t len, int timeoutMs) {
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(fd, &rfds);
    struct timeval tv = {timeoutMs / 1000, (timeoutMs % 1000) * 1000};
    int r = select(fd + 1, &rfds, nullptr, nullptr, &tv);
    if (r <= 0) return 0;
    return ::read(fd, buf, len);
}

}  // namespace

TtyTransport::TtyTransport(std::string ttyName, unsigned vid, unsigned pid, int baud)
    : mTtyName(std::move(ttyName)), mVid(vid), mPid(pid), mBaud(baud) {}

std::string TtyTransport::resolveDevicePath() {
    if (!mTtyName.empty()) return "/dev/" + mTtyName;
    std::string name = findUsbSerialDevice(mVid, mPid);
    if (name.empty()) return "";
    return "/dev/" + name;
}

bool TtyTransport::applyTermios(int baud) {
    speed_t speed = baudConst(baud);
    if (speed == 0) {
        ALOGE("unsupported baud %d", baud);
        return false;
    }
    struct termios ios;
    if (tcgetattr(mFd, &ios) != 0) {
        ALOGW("tcgetattr failed: %s (non-tty?)", strerror(errno));
        return true;  // raw fd, nothing to configure
    }
    ios.c_lflag = 0;                    // raw: no ECHO/ICANON
    ios.c_oflag &= ~ONLCR;
    ios.c_iflag &= ~(ICRNL | INLCR);
    ios.c_iflag |= IGNCR;
    ios.c_cflag = speed | CS8 | CLOCAL | CREAD;
    ios.c_cc[VMIN] = 1;
    ios.c_cc[VTIME] = 0;
    tcsetattr(mFd, TCSANOW, &ios);
    return true;
}

bool TtyTransport::open() {
    std::string path = resolveDevicePath();
    if (path.empty()) {
        ALOGE("no tty for %04x:%04x and no explicit node", mVid, mPid);
        return false;
    }
    do {
        mFd = ::open(path.c_str(), O_RDWR | O_NOCTTY | O_CLOEXEC);
    } while (mFd < 0 && errno == EINTR);
    if (mFd < 0) {
        ALOGE("open %s failed: %s", path.c_str(), strerror(errno));
        return false;
    }

    int rate = mBaud > 0 ? mBaud : 9600;
    if (!applyTermios(rate)) {
        close();
        return false;
    }

    mName = "tty " + path;
    mOpen = true;
    ALOGI("opened %s @ %d baud", mName.c_str(), rate);

    if (mBaud <= 0) {
        // Auto-baud probe: retune until checksum-valid NMEA appears.
        static const int kCandidates[] = {9600, 115200, 38400, 4800, 19200, 57600};
        char buf[512];
        for (int cand : kCandidates) {
            if (!applyTermios(cand)) continue;
            std::vector<char> acc;
            for (int waited = 0; waited < 2200; waited += 200) {
                ssize_t n = readNibble(mFd, buf, sizeof(buf), 200);
                if (n > 0) {
                    acc.insert(acc.end(), buf, buf + n);
                    if (acc.size() > 8192) break;
                    if (looksLikeNmea(acc.data(), acc.size())) {
                        mBaud = cand;
                        mPushback = std::move(acc);
                        ALOGI("auto-baud: locked %d baud", cand);
                        return true;
                    }
                }
            }
            ALOGD("auto-baud: %d baud: no NMEA", cand);
        }
        ALOGE("auto-baud failed on %s", path.c_str());
        close();
        return false;
    }
    return true;
}

void TtyTransport::close() {
    if (mFd >= 0) {
        ::close(mFd);
        mFd = -1;
    }
    mOpen = false;
}

ssize_t TtyTransport::read(char* buf, size_t len) {
    if (mFd < 0) return -1;
    ssize_t n = readPushback(buf, len);
    if (n != 0) return n;
    do {
        n = ::read(mFd, buf, len);
    } while (n < 0 && errno == EINTR);
    if (n == 0) return -1;  // EOF -> treat as transport loss
    return n;
}

ssize_t TtyTransport::write(const char* buf, size_t len) {
    if (mFd < 0) return -1;
    size_t done = 0;
    while (done < len) {
        ssize_t n = ::write(mFd, buf + done, len - done);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        done += (size_t)n;
    }
    return (ssize_t)done;
}

void TtyTransport::setBaud(int baud) {
    if (baud <= 0 || mFd < 0) return;
    if (applyTermios(baud)) mBaud = baud;
}

}  // namespace aidl::android::hardware::gnss
