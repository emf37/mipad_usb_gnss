/*
 * Stream-socket transport (tcp://host:port or unix:/path), used to feed NMEA
 * from userspace producers such as the GnssBtBridge Bluetooth-SPP app, gpsd,
 * or an NTRIP client.
 *
 * Copyright (C) 2026 ksu-gnss contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "Transport.h"

namespace aidl::android::hardware::gnss {

class SocketTransport : public Transport {
  public:
    // addr: "tcp://127.0.0.1:5000", "tcp://host:port", or "unix:/path/to/sock".
    explicit SocketTransport(std::string addr);
    ~SocketTransport() override { close(); }

    bool open() override;
    void close() override;
    ssize_t read(char* buf, size_t len) override;
    ssize_t write(const char* buf, size_t len) override;
    void setBaud(int) override {}
    std::string name() const override { return mName; }

  private:
    std::string mAddr;
    int mFd = -1;
    std::string mName;
};

}  // namespace aidl::android::hardware::gnss
