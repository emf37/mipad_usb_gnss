/*
 * Pluggable byte-stream transports for the GNSS HAL.
 *
 * Copyright (C) 2026 ksu-gnss contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace aidl::android::hardware::gnss {

/**
 * A blocking byte stream carrying NMEA-0183 (optionally UBX) between the
 * receiver and the HAL reader thread.
 *
 * Implementations: UsbfsCh340 (usbfs userspace driver, no kernel ch341
 * needed), TtyTransport (kernel serial node), SocketTransport (tcp:// or
 * unix: stream, e.g. the Bluetooth SPP bridge app).
 */
class Transport {
  public:
    virtual ~Transport() = default;

    // Connect + configure (may run the auto-baud probe, up to ~15 s).
    virtual bool open() = 0;
    virtual void close() = 0;
    // Blocking read; returns >0 bytes, 0 on EOF, -1 errno-style error.
    virtual ssize_t read(char* buf, size_t len) = 0;
    virtual ssize_t write(const char* buf, size_t len) = 0;
    // Best-effort line-speed change; no-op where meaningless.
    virtual void setBaud(int baud) = 0;
    // Short description for logs, e.g. "usbfs 1a86:7523 001/002".
    virtual std::string name() const = 0;

    bool isOpen() const { return mOpen; }

  protected:
    bool mOpen = false;
    // Bytes swallowed by the auto-baud probe, replayed on first read().
    std::vector<char> mPushback;

    ssize_t readPushback(char* buf, size_t len);
};

// Configuration resolved once from persist.vendor.gnss_usb.* properties.
struct TransportConfig {
    std::string source;     // usb | tty | tcp | unix | auto
    std::string vidPid;     // "1a86:7523"
    std::string ttyDev;     // explicit tty node name, e.g. "ttyUSB0"
    std::string socketAddr; // "tcp://127.0.0.1:5000" | "unix:/path/to/sock"
    int baud = 9600;        // >0 fixed rate; 0 = auto-probe
    bool verboseLog = false;

    static TransportConfig fromProps();
};

// Runtime source override set via the control server ("SOURCE tcp" etc.).
// Takes precedence over the property until the daemon exits; empty = none.
void setSourceOverride(const std::string& s);

// Parse "vid:pid" (case-insensitive hex). Returns false on malformed input.
bool parseUsbId(const std::string& s, unsigned* vid, unsigned* pid);

// True if buf contains at least one checksum-valid NMEA sentence. Used to
// confirm a candidate baud rate during auto-probe.
bool looksLikeNmea(const char* buf, size_t len);

/**
 * Build a transport per config. "auto" tries usbfs, then kernel tty (the
 * socket backends are only used when explicitly selected). Returns nullptr
 * when nothing could be opened; caller retries with backoff.
 */
std::unique_ptr<Transport> createTransport(const TransportConfig& cfg);

}  // namespace aidl::android::hardware::gnss
