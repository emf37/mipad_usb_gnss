/*
 * Kernel-serial-node transport (/dev/ttyUSB*, /dev/ttyACM*, ...).
 *
 * Device discovery by USB vid:pid via sysfs ancestor walk (port of the
 * gnss-aidl-serial SerialDevice logic, Apache-2.0) so it works no matter
 * which driver name the node gets.
 *
 * Copyright (C) 2026 ksu-gnss contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "Transport.h"

namespace aidl::android::hardware::gnss {

class TtyTransport : public Transport {
  public:
    // ttyName: explicit node ("ttyUSB0") or empty to scan by vid:pid.
    TtyTransport(std::string ttyName, unsigned vid, unsigned pid, int baud);
    ~TtyTransport() override { close(); }

    bool open() override;
    void close() override;
    ssize_t read(char* buf, size_t len) override;
    ssize_t write(const char* buf, size_t len) override;
    void setBaud(int baud) override;
    std::string name() const override { return mName; }

  private:
    std::string resolveDevicePath();
    bool applyTermios(int baud);

    std::string mTtyName;
    unsigned mVid, mPid;
    int mBaud;
    int mFd = -1;
    std::string mName;
};

}  // namespace aidl::android::hardware::gnss
