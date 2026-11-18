/*
 * usbfs userspace driver for WCH CH340/CH341 USB-serial adapters.
 *
 * The wire protocol (vendor control transfers 0x5f/0x95/0xa1/0x9a/0xa4) and
 * the baud factor/divisor computation are ported from usb-serial-for-android
 * (https://github.com/mik3y/usb-serial-for-android), Ch34xSerialDriver.java,
 * Copyright 2014 Andreas Butti, Apache-2.0.
 *
 * Copyright (C) 2026 ksu-gnss contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "gnss_usb_dev"

#include "UsbfsCh340.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/usbdevice_fs.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <log/log.h>

#include <algorithm>
#include <vector>

namespace aidl::android::hardware::gnss {

namespace {

constexpr int kCtrlTimeoutMs = 1000;
constexpr int kBulkTimeoutMs = 500;
constexpr int kProbeWindowMs = 2200;

// Line-control bits (Ch34xSerialDriver naming kept for traceability).
constexpr int LCR_ENABLE_RX = 0x80;
constexpr int LCR_ENABLE_TX = 0x40;
constexpr int LCR_CS8 = 0x03;
constexpr int SCL_DTR = 0x20;
constexpr int SCL_RTS = 0x40;

// Small sysfs attribute (e.g. idVendor), trimmed; "" if absent/unreadable.
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

// Find the /dev/bus/usb/<bus>/<dev> node of the first USB device whose
// idVendor/idProduct match. Sysfs entries with ':' are interfaces, not devices.
std::string findUsbDeviceNode(unsigned vid, unsigned pid, unsigned* bus, unsigned* dev) {
    DIR* dir = opendir("/sys/bus/usb/devices");
    if (dir == nullptr) return "";
    std::string best;
    while (struct dirent* e = readdir(dir)) {
        if (e->d_name[0] == '.' || strchr(e->d_name, ':')) continue;
        std::string base = std::string("/sys/bus/usb/devices/") + e->d_name;
        std::string v = readSysfsAttr(base + "/idVendor");
        std::string p = readSysfsAttr(base + "/idProduct");
        if (v.empty() || p.empty()) continue;
        if (strtoul(v.c_str(), nullptr, 16) != vid) continue;
        if (strtoul(p.c_str(), nullptr, 16) != pid) continue;
        std::string b = readSysfsAttr(base + "/busnum");
        std::string d = readSysfsAttr(base + "/devnum");
        if (b.empty() || d.empty()) continue;
        char node[64];
        snprintf(node, sizeof(node), "/dev/bus/usb/%03u/%03u",
                 (unsigned)strtoul(b.c_str(), nullptr, 10),
                 (unsigned)strtoul(d.c_str(), nullptr, 10));
        if (bus) *bus = (unsigned)strtoul(b.c_str(), nullptr, 10);
        if (dev) *dev = (unsigned)strtoul(d.c_str(), nullptr, 10);
        best = node;  // keep scanning so deterministic (last) not needed; take first
        break;
    }
    closedir(dir);
    return best;
}

// Walk the sysfs raw descriptors blob and pick the first bulk IN / OUT
// endpoint addresses and the bulk-in max packet size. CH340 adapters have
// exactly one of each (plus one interrupt-in we do not use).
bool parseEndpoints(const std::string& sysfsDevPath, unsigned char* epIn, unsigned char* epOut,
                    int* epInMax) {
    std::string blob;
    {
        int fd = ::open((sysfsDevPath + "/descriptors").c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) return false;
        char buf[4096];
        ssize_t n;
        while ((n = ::read(fd, buf, sizeof(buf))) > 0) blob.append(buf, (size_t)n);
        ::close(fd);
    }
    if (blob.size() < 18) return false;

    size_t pos = 18;  // skip device descriptor
    while (pos + 2 <= blob.size()) {
        unsigned char len = (unsigned char)blob[pos];
        unsigned char type = (unsigned char)blob[pos + 1];
        if (len < 2 || pos + len > blob.size()) break;
        if (type == 0x05 && len >= 7) {  // endpoint descriptor
            unsigned char addr = (unsigned char)blob[pos + 2];
            unsigned char attr = (unsigned char)blob[pos + 3];
            int maxPacket = (unsigned char)blob[pos + 4] | ((unsigned char)blob[pos + 5] << 8);
            if ((attr & 0x03) == 0x02) {  // bulk
                if (addr & 0x80) {
                    if (*epIn == 0) {
                        *epIn = addr;
                        *epInMax = maxPacket ? maxPacket : 32;
                    }
                } else if (*epOut == 0) {
                    *epOut = addr;
                }
            }
        }
        pos += len;
    }
    return *epIn != 0 && *epOut != 0;
}

}  // namespace

UsbfsCh340::UsbfsCh340(unsigned vid, unsigned pid, int baud)
    : mVid(vid), mPid(pid), mBaud(baud) {}

int UsbfsCh340::controlOut(unsigned char request, int value, int index) {
    struct usbdevfs_ctrltransfer ctrl = {};
    ctrl.bRequestType = 0x40;  // vendor, host-to-device
    ctrl.bRequest = request;
    ctrl.wValue = (unsigned short)(value & 0xffff);
    ctrl.wIndex = (unsigned short)(index & 0xffff);
    ctrl.wLength = 0;
    ctrl.timeout = kCtrlTimeoutMs;
    ctrl.data = nullptr;
    return ioctl(mFd, USBDEVFS_CONTROL, &ctrl);
}

int UsbfsCh340::controlIn(unsigned char request, int value, int index, void* data, int len) {
    struct usbdevfs_ctrltransfer ctrl = {};
    ctrl.bRequestType = 0xc0;  // vendor, device-to-host
    ctrl.bRequest = request;
    ctrl.wValue = (unsigned short)(value & 0xffff);
    ctrl.wIndex = (unsigned short)(index & 0xffff);
    ctrl.wLength = (unsigned short)len;
    ctrl.timeout = kCtrlTimeoutMs;
    ctrl.data = data;
    return ioctl(mFd, USBDEVFS_CONTROL, &ctrl);
}

bool UsbfsCh340::ch340SetBaud(int baudRate) {
    long factor;
    long divisor;

    if (baudRate == 921600) {
        divisor = 7;
        factor = 0xf300;
    } else if (baudRate == 307200) {
        divisor = 7;
        factor = 0xd900;
    } else {
        constexpr long kBaudbaseFactor = 1532620800;
        constexpr int kBaudbaseDivmax = 3;

        factor = kBaudbaseFactor / baudRate;
        divisor = kBaudbaseDivmax;
        while ((factor > 0xfff0) && divisor > 0) {
            factor >>= 3;
            divisor--;
        }
        if (factor > 0xfff0) {
            ALOGE("unsupported baud rate %d", baudRate);
            return false;
        }
        factor = 0x10000 - factor;
    }

    divisor |= 0x0080;  // else ch341a waits until buffer full
    int val1 = (int)(((unsigned long)factor & 0xff00) | (unsigned long)divisor);
    int val2 = (int)((unsigned long)factor & 0xff);
    if (controlOut(0x9a, 0x1312, val1) < 0) {
        ALOGE("set baud #1 failed: %s", strerror(errno));
        return false;
    }
    if (controlOut(0x9a, 0x0f2c, val2) < 0) {
        ALOGE("set baud #2 failed: %s", strerror(errno));
        return false;
    }
    return true;
}

void UsbfsCh340::setControlLines(bool dtr, bool rts) {
    int v = ~((dtr ? SCL_DTR : 0) | (rts ? SCL_RTS : 0));
    controlOut(0xa4, v, 0);
}

bool UsbfsCh340::initChip() {
    unsigned char buf[2];

    // init #1: read chip version, second byte must be 0x00
    if (controlIn(0x5f, 0, 0, buf, 2) != 2 || buf[1] != 0x00) {
        ALOGE("init #1 failed (not a CH340/CH341?)");
        return false;
    }
    // init #2
    if (controlOut(0xa1, 0, 0) < 0) return false;
    // default rate for the init handshake
    if (!ch340SetBaud(9600)) return false;
    // init #4: line-control latch must read back clean
    if (controlIn(0x95, 0x2518, 0, buf, 2) != 2 || buf[1] != 0x00) {
        ALOGE("init #4 failed");
        return false;
    }
    // init #5: 8N1, enable RX/TX
    if (controlOut(0x9a, 0x2518, LCR_ENABLE_RX | LCR_ENABLE_TX | LCR_CS8) < 0) return false;
    // init #6: modem status, contents ignored
    if (controlIn(0x95, 0x0706, 0, buf, 2) != 2) {
        ALOGE("init #6 failed");
        return false;
    }
    // init #7
    if (controlOut(0xa1, 0x501f, 0xd90a) < 0) return false;
    return true;
}

bool UsbfsCh340::open() {
    unsigned bus = 0, dev = 0;
    std::string node = findUsbDeviceNode(mVid, mPid, &bus, &dev);
    if (node.empty()) {
        ALOGE("no USB device %04x:%04x in sysfs", mVid, mPid);
        return false;
    }

    do {
        mFd = ::open(node.c_str(), O_RDWR | O_NOCTTY | O_CLOEXEC);
    } while (mFd < 0 && errno == EINTR);
    if (mFd < 0) {
        ALOGE("open %s failed: %s", node.c_str(), strerror(errno));
        return false;
    }

    // Find the device's sysfs dir for descriptor parsing.
    std::string sysfsDev;
    DIR* dir = opendir("/sys/bus/usb/devices");
    if (dir != nullptr) {
        while (struct dirent* e = readdir(dir)) {
            if (e->d_name[0] == '.' || strchr(e->d_name, ':')) continue;
            std::string base = std::string("/sys/bus/usb/devices/") + e->d_name;
            std::string b = readSysfsAttr(base + "/busnum");
            std::string d = readSysfsAttr(base + "/devnum");
            if (b.empty() || d.empty()) continue;
            if ((unsigned)strtoul(b.c_str(), nullptr, 10) == bus &&
                (unsigned)strtoul(d.c_str(), nullptr, 10) == dev) {
                sysfsDev = base;
                break;
            }
        }
        closedir(dir);
    }
    mEpIn = mEpOut = 0;
    mEpInMaxPacket = 32;
    if (sysfsDev.empty() || !parseEndpoints(sysfsDev, &mEpIn, &mEpOut, &mEpInMaxPacket)) {
        mEpIn = 0x82;
        mEpOut = 0x02;
        mEpInMaxPacket = 32;
        ALOGW("descriptor parse failed; using CH340 defaults ep 0x%02x/0x%02x", mEpIn, mEpOut);
    }

    // Detach any kernel driver (e.g. ch341) then claim interface 0.
    for (int retry = 0; retry < 2; ++retry) {
        int iface = 0;
        if (ioctl(mFd, USBDEVFS_CLAIMINTERFACE, &iface) == 0) break;
        if (errno != EBUSY || retry == 1) {
            ALOGE("claim interface failed: %s", strerror(errno));
            ::close(mFd);
            mFd = -1;
            return false;
        }
        struct usbdevfs_ioctl di = {};
        di.ifno = 0;
        di.ioctl_code = USBDEVFS_DISCONNECT;
        di.data = nullptr;
        ioctl(mFd, USBDEVFS_IOCTL, &di);  // best-effort detach
        ALOGI("detached kernel driver from interface 0, retrying claim");
    }

    if (!initChip()) {
        ioctl(mFd, USBDEVFS_RELEASEINTERFACE, 0);
        ::close(mFd);
        mFd = -1;
        return false;
    }

    mName = "usbfs " + std::string(node);
    mOpen = true;
    ALOGI("opened %s (ep in 0x%02x out 0x%02x max %d)", mName.c_str(), mEpIn, mEpOut,
          mEpInMaxPacket);

    setControlLines(true, true);  // DTR/RTS asserted; some receivers key off DTR

    if (mBaud <= 0) {
        if (!probeBaud()) {
            close();
            return false;
        }
    } else {
        setBaud(mBaud);
    }
    return true;
}

void UsbfsCh340::close() {
    if (mFd >= 0) {
        ioctl(mFd, USBDEVFS_RELEASEINTERFACE, 0);
        ::close(mFd);
        mFd = -1;
    }
    mOpen = false;
}

ssize_t UsbfsCh340::read(char* buf, size_t len) {
    if (mFd < 0) return -1;
    ssize_t n = readPushback(buf, len);
    if (n != 0) return n;

    struct usbdevfs_bulktransfer bulk = {};
    bulk.ep = mEpIn;
    bulk.len = (unsigned int)std::min<size_t>(len, (size_t)mEpInMaxPacket);
    bulk.timeout = kBulkTimeoutMs;
    bulk.data = buf;
    n = ioctl(mFd, USBDEVFS_BULK, &bulk);
    if (n >= 0) return n;
    if (errno == ETIMEDOUT) return 0;  // idle: no data in this window
    return -1;                          // ENODEV/EPIPE/... -> reconnect
}

ssize_t UsbfsCh340::write(const char* buf, size_t len) {
    if (mFd < 0) return -1;
    struct usbdevfs_bulktransfer bulk = {};
    bulk.ep = mEpOut;
    bulk.len = (unsigned int)len;
    bulk.timeout = kCtrlTimeoutMs;
    bulk.data = const_cast<char*>(buf);
    ssize_t n = ioctl(mFd, USBDEVFS_BULK, &bulk);
    if (n < 0) ALOGW("bulk write failed: %s", strerror(errno));
    return n;
}

void UsbfsCh340::setBaud(int baud) {
    if (baud <= 0 || mFd < 0) return;
    if (!ch340SetBaud(baud)) return;
    mBaud = baud;
    ALOGI("baud set to %d", baud);
}

bool UsbfsCh340::probeBaud() {
    static const int kCandidates[] = {9600, 115200, 38400, 4800, 19200, 57600};
    char buf[512];
    for (int rate : kCandidates) {
        if (!ch340SetBaud(rate)) return false;
        std::vector<char> acc;
        for (int waited = 0; waited < kProbeWindowMs; waited += kBulkTimeoutMs) {
            struct usbdevfs_bulktransfer bulk = {};
            bulk.ep = mEpIn;
            bulk.len = sizeof(buf);
            bulk.timeout = kBulkTimeoutMs;
            bulk.data = buf;
            ssize_t n = ioctl(mFd, USBDEVFS_BULK, &bulk);
            if (n > 0) {
                acc.insert(acc.end(), buf, buf + n);
                if (acc.size() > 8192) break;
                if (looksLikeNmea(acc.data(), acc.size())) {
                    mBaud = rate;
                    mPushback = std::move(acc);
                    ALOGI("auto-baud: locked %d baud (%zu buffered bytes)", rate,
                          mPushback.size());
                    return true;
                }
            } else if (n < 0 && errno != ETIMEDOUT) {
                return false;
            }
        }
        ALOGD("auto-baud: %d baud: no NMEA", rate);
    }
    ALOGE("auto-baud: no valid NMEA on any candidate rate");
    return false;
}

// ---- base helpers -----------------------------------------------------------

ssize_t Transport::readPushback(char* buf, size_t len) {
    if (mPushback.empty()) return 0;
    size_t n = std::min(len, mPushback.size());
    memcpy(buf, mPushback.data(), n);
    mPushback.erase(mPushback.begin(), mPushback.begin() + (long)n);
    return (ssize_t)n;
}

}  // namespace aidl::android::hardware::gnss
