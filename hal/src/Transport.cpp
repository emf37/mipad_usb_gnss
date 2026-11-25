/*
 * Transport factory + shared helpers.
 *
 * Copyright (C) 2026 ksu-gnss contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "gnss_usb_dev"

#include "Transport.h"

#include <cutils/properties.h>
#include <log/log.h>

#include <mutex>

#include "SocketTransport.h"
#include "TtyTransport.h"
#include "UsbfsCh340.h"

namespace aidl::android::hardware::gnss {

namespace {
constexpr const char* kPropPrefix = "persist.vendor.gnss_usb.";

int hexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::mutex gSourceOverrideMutex;
std::string gSourceOverride;
}  // namespace

void setSourceOverride(const std::string& s) {
    std::lock_guard<std::mutex> lock(gSourceOverrideMutex);
    gSourceOverride = s;
}

TransportConfig TransportConfig::fromProps() {
    char prop[PROP_VALUE_MAX /* 92 */];
    TransportConfig cfg;

    property_get((std::string(kPropPrefix) + "source").c_str(), prop, "auto");
    cfg.source = prop;
    {
        std::lock_guard<std::mutex> lock(gSourceOverrideMutex);
        if (!gSourceOverride.empty()) cfg.source = gSourceOverride;
    }

    property_get((std::string(kPropPrefix) + "vid_pid").c_str(), prop, "1a86:7523");
    cfg.vidPid = prop;

    property_get((std::string(kPropPrefix) + "tty_dev").c_str(), prop, "");
    cfg.ttyDev = prop;

    property_get((std::string(kPropPrefix) + "socket_addr").c_str(), prop,
                 "tcp://127.0.0.1:5000");
    cfg.socketAddr = prop;

    property_get((std::string(kPropPrefix) + "baud").c_str(), prop, "115200");
    if (!strcmp(prop, "auto") || !strcmp(prop, "0")) {
        cfg.baud = 0;  // probe
    } else {
        cfg.baud = atoi(prop);
        if (cfg.baud <= 0) cfg.baud = 9600;
    }

    property_get((std::string(kPropPrefix) + "log").c_str(), prop, "0");
    cfg.verboseLog = !strcmp(prop, "1") || !strcmp(prop, "true");

    return cfg;
}

bool parseUsbId(const std::string& s, unsigned* vid, unsigned* pid) {
    unsigned v = 0, p = 0;
    int consumed = 0;
    if (sscanf(s.c_str(), "%4x:%4x%n", &v, &p, &consumed) != 2 ||
        (size_t)consumed != s.size()) {
        return false;
    }
    *vid = v;
    *pid = p;
    return true;
}

bool looksLikeNmea(const char* buf, size_t len) {
    for (size_t i = 0; i + 4 < len; ++i) {
        if (buf[i] != '$') continue;
        // find "*hh" terminator within sentence length
        unsigned char ck = 0;
        for (size_t j = i + 1; j < len; ++j) {
            char c = buf[j];
            if (c == '*') {
                if (j + 2 >= len) break;
                int hi = hexVal(buf[j + 1]);
                int lo = hexVal(buf[j + 2]);
                if (hi >= 0 && lo >= 0 && ck == (unsigned char)((hi << 4) | lo)) return true;
                break;
            }
            if (c == '\r' || c == '\n' || c == '$') break;  // malformed; resync
            ck ^= (unsigned char)c;
        }
    }
    return false;
}

std::unique_ptr<Transport> createTransport(const TransportConfig& cfg) {
    std::string source = cfg.source;
    if (source == "tcp" || source.rfind("tcp://", 0) == 0) {
        auto t = std::make_unique<SocketTransport>(cfg.socketAddr);
        return t->open() ? std::move(t) : nullptr;
    }
    if (source == "unix" || source.rfind("unix:", 0) == 0) {
        auto t = std::make_unique<SocketTransport>(cfg.socketAddr);
        return t->open() ? std::move(t) : nullptr;
    }

    unsigned vid = 0, pid = 0;
    if (!parseUsbId(cfg.vidPid, &vid, &pid)) {
        ALOGE("invalid vid_pid '%s'", cfg.vidPid.c_str());
        return nullptr;
    }

    if (source == "usb") {
        auto t = std::make_unique<UsbfsCh340>(vid, pid, cfg.baud);
        return t->open() ? std::move(t) : nullptr;
    }
    if (source == "tty") {
        auto t = std::make_unique<TtyTransport>(cfg.ttyDev, vid, pid, cfg.baud);
        return t->open() ? std::move(t) : nullptr;
    }

    // "auto": usbfs first (works without any kernel driver), then kernel tty.
    {
        auto t = std::make_unique<UsbfsCh340>(vid, pid, cfg.baud);
        if (t->open()) return t;
        ALOGI("auto: usbfs backend unavailable, trying kernel tty");
    }
    {
        auto t = std::make_unique<TtyTransport>(cfg.ttyDev, vid, pid, cfg.baud);
        if (t->open()) return t;
        ALOGI("auto: tty backend unavailable too");
    }
    return nullptr;
}

}  // namespace aidl::android::hardware::gnss
