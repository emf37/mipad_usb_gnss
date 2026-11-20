/*
 * IGnssPsds implementation: framework-driven assistance data injection.
 *
 * The framework downloads PSDS/AGNSS data from its configured server and
 * pushes it via injectPsdsData(). We check the blob for a CASIC frame
 * signature (BA CE sync) and write matching frames to the module; non-CASIC
 * payloads (e.g. Qualcomm XTRA) are logged and dropped safely — the module
 * can't parse them, but garbage never reaches the UART.
 *
 * Copyright (C) 2026 ksu-gnss contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <aidl/android/hardware/gnss/BnGnssPsds.h>
#include <aidl/android/hardware/gnss/IGnssPsdsCallback.h>
#include <aidl/android/hardware/gnss/PsdsType.h>

#include <log/log.h>

#include <functional>
#include <memory>
#include <mutex>

namespace aidl::android::hardware::gnss {

class GnssPsds : public BnGnssPsds {
  public:
    // writeRaw: called with binary payloads destined for the module transport
    // (no CRLF — CASIC frames are binary).
    using WriteRawFn = std::function<bool(const uint8_t*, size_t)>;

    explicit GnssPsds(WriteRawFn write) : mWrite(std::move(write)) {}

    ndk::ScopedAStatus setCallback(
            const std::shared_ptr<IGnssPsdsCallback>& callback) override {
        std::lock_guard<std::mutex> lock(mMutex);
        mCallback = callback;
        return ndk::ScopedAStatus::ok();
    }

    ndk::ScopedAStatus injectPsdsData(PsdsType type,
                                      const std::vector<uint8_t>& data) override {
        if (data.empty()) {
            ALOGW("injectPsdsData: empty blob (type=%d)", (int)type);
            return ndk::ScopedAStatus::ok();
        }

        // Log what the framework sends so we can identify the format on device
        ALOGI("injectPsdsData: type=%d size=%zu head=%02x%02x%02x%02x%02x%02x%02x%02x",
              (int)type, data.size(),
              data.size() > 0 ? data[0] : 0, data.size() > 1 ? data[1] : 0,
              data.size() > 2 ? data[2] : 0, data.size() > 3 ? data[3] : 0,
              data.size() > 4 ? data[4] : 0, data.size() > 5 ? data[5] : 0,
              data.size() > 6 ? data[6] : 0, data.size() > 7 ? data[7] : 0);

        // CASIC frame validation: scan for BA CE sync, verify at least one
        // complete well-formed frame before writing anything.
        size_t casicFrames = 0;
        size_t i = 0;
        while (i + 6 <= data.size()) {
            if (data[i] == 0xBA && data[i + 1] == 0xCE) {
                uint16_t len = data[i + 2] | (data[i + 3] << 8);
                size_t frameLen = 6 + len + 4;
                if (i + frameLen <= data.size()) {
                    casicFrames++;
                    i += frameLen;
                    continue;
                }
            }
            i++;
        }

        if (casicFrames == 0) {
            ALOGI("injectPsdsData: no CASIC frames found (likely XTRA/other "
                  "format) — dropping safely, module untouched");
            return ndk::ScopedAStatus::ok();
        }

        ALOGI("injectPsdsData: %zu CASIC frames detected, writing to module",
              casicFrames);
        if (mWrite && mWrite(data.data(), data.size())) {
            ALOGI("injectPsdsData: wrote %zu bytes", data.size());
        } else {
            ALOGW("injectPsdsData: write failed (transport down?)");
        }
        return ndk::ScopedAStatus::ok();
    }

    /** Ask the framework to fetch PSDS data (call on session start for cold start). */
    void requestDownload(PsdsType type) {
        std::lock_guard<std::mutex> lock(mMutex);
        if (mCallback) {
            mCallback->downloadRequestCb(type);
            ALOGI("requested PSDS download (type=%d)", (int)type);
        }
    }

  private:
    WriteRawFn mWrite;
    std::mutex mMutex;
    std::shared_ptr<IGnssPsdsCallback> mCallback;
};

}  // namespace aidl::android::hardware::gnss
