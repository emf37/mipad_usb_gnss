/*
 * usbfs userspace driver for WCH CH340/CH341 USB-serial adapters.
 *
 * Talks to the device through /dev/bus/usb/<bus>/<dev> directly, so no kernel
 * ch341 driver is required (or, if one is bound, it is detached from the
 * interface first). Control-transfer sequence and baud-factor math ported
 * from usb-serial-for-android's Ch34xSerialDriver (Apache-2.0).
 *
 * Copyright (C) 2026 ksu-gnss contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "Transport.h"

#include <string>

namespace aidl::android::hardware::gnss {

class UsbfsCh340 : public Transport {
  public:
    // vid/pid in "1a86:7523" form; baud <= 0 selects auto-probe.
    UsbfsCh340(unsigned vid, unsigned pid, int baud);
    ~UsbfsCh340() override { close(); }

    bool open() override;
    void close() override;
    ssize_t read(char* buf, size_t len) override;
    ssize_t write(const char* buf, size_t len) override;
    void setBaud(int baud) override;
    std::string name() const override { return mName; }

  private:
    // One vendor control transfer; returns transferred length or -1.
    int controlOut(unsigned char request, int value, int index);
    int controlIn(unsigned char request, int value, int index, void* data, int len);

    bool initChip();
    bool ch340SetBaud(int baud);
    void setControlLines(bool dtr, bool rts);
    bool probeBaud();  // auto-baud: try candidates until valid NMEA shows up

    unsigned mVid, mPid;
    int mBaud;
    int mFd = -1;
    unsigned char mEpIn = 0x82;
    unsigned char mEpOut = 0x02;
    int mEpInMaxPacket = 32;
    std::string mName;
};

}  // namespace aidl::android::hardware::gnss
