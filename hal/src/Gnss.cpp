/*
 * USB/serial NMEA GNSS HAL for KernelSU deployment.
 *
 * Derived from gnss-aidl-serial (https://github.com/aselafernando/gnss-aidl-serial),
 * Copyright (C) 2026 Asela Fernando, Apache-2.0. The reader thread here adds
 * indefinite transport re-acquisition (device plugged after boot / replug).
 *
 * Copyright (C) 2026 ksu-gnss contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "gnss_usb"

#include "Gnss.h"

#include "AGnss.h"
#include "AGnssRil.h"
#include "GnssAntennaInfo.h"
#include "GnssConfiguration.h"
#include "GnssDebug.h"
#include "GnssMeasurementInterface.h"
#include "GnssPowerIndication.h"
#include "GnssVisibilityControl.h"

#include <android/binder_ibinder.h>
#include <cutils/properties.h>
#include <log/log.h>

#include <unistd.h>

namespace aidl::android::hardware::gnss {

using ndk::ScopedAStatus;

namespace {
constexpr const char* kPropPrefix = "persist.vendor.gnss_usb.";

// Slow background rate used while no session is active.
constexpr unsigned short kIdleRateMs = 10000;
// Re-acquisition retry period when no transport is available yet.
constexpr int kRetryDelayMs = 2000;

void sleepMs(int ms) {
    usleep((useconds_t)ms * 1000);
}
}  // namespace

Gnss::Gnss() {
    mGnssConfiguration = ndk::SharedRefBase::make<GnssConfiguration>();
    mGnssPowerIndication = ndk::SharedRefBase::make<GnssPowerIndication>();

    // Wire the parser's outputs to the AIDL callbacks.
    mParser.setLocationCallback([this](const GnssLocation& loc) { reportLocation(loc); });
    mParser.setSvStatusCallback(
            [this](const std::vector<IGnssCallback::GnssSvInfo>& svs) { reportSvStatus(svs); });
    mParser.setNmeaCallback(
            [this](int64_t ts, const std::string& s) { reportNmea(ts, s); });

    char prop[PROP_VALUE_MAX];

    std::string key = std::string(kPropPrefix) + "time_sync";
    if (property_get(key.c_str(), prop, "") != 0) {
        mParser.setTimeSyncThreshold(atol(prop));
    }

    // Initial fix interval from persist.vendor.gnss_usb.max_rate.
    unsigned short period = 1000;
    key = std::string(kPropPrefix) + "max_rate";
    if (property_get(key.c_str(), prop, "") != 0) {
        unsigned long rate = strtoul(prop, nullptr, 10);
        if (rate > 0 && rate < 66) {
            period = (unsigned short)(rate * 1000);
        } else if (rate >= 250 && rate < 65536) {
            period = (unsigned short)rate;
        }
    }
    mFixIntervalMs = period;

    key = std::string(kPropPrefix) + "log";
    char logProp[PROP_VALUE_MAX] = "0";
    property_get(key.c_str(), logProp, "0");
    mVerboseRaw = !strcmp(logProp, "1") || !strcmp(logProp, "true");
}

Gnss::~Gnss() {
    close();
}

unsigned short Gnss::clampRateMs(int minIntervalMs) {
    if (minIntervalMs < 100) return 100;
    if (minIntervalMs > 65000) return 65000;
    return (unsigned short)minIntervalMs;
}

// ---- lifecycle --------------------------------------------------------------

ScopedAStatus Gnss::setCallback(const std::shared_ptr<IGnssCallback>& callback) {
    ALOGI("framework call: setCallback (caller uid %d)", AIBinder_getCallingUid());
    if (callback == nullptr) {
        return ScopedAStatus::fromExceptionCode(EX_NULL_POINTER);
    }
    {
        std::lock_guard<std::mutex> lock(mCallbackMutex);
        mCallback = callback;
    }

    // 回调分级投放（HyperOS 兼容性排查）：全部可用属性开关，默认最小集。
    //   cb_caps=1   连接时上报能力(仅SCHEDULING)   [默认1]
    //   cb_sysinfo=1 连接时上报系统信息            [默认0]
    //   cb_loc=1    上报定位                       [默认1]
    //   cb_loc_min=1 定位仅含 lat/lon/时间/水平精度（应急降级开关，默认0=完整字段）
    //   cb_sv=1     上报卫星状态                   [默认0]
    //   cb_nmea=1   上报原始NMEA                   [默认0]
    char prop[PROP_VALUE_MAX];
    auto on = [&](const char* key, const char* def) {
        property_get((std::string(kPropPrefix) + key).c_str(), prop, def);
        return !strcmp(prop, "1") || !strcmp(prop, "true");
    };
    if (on("cb_caps", "1")) {
        callback->gnssSetCapabilitiesCb(IGnssCallback::CAPABILITY_SCHEDULING);
        ALOGI("sent capabilities (SCHEDULING)");
    }
    if (on("cb_sysinfo", "1")) {
        IGnssCallback::GnssSystemInfo info;
        info.yearOfHw = 2023;
        callback->gnssSetSystemInfoCb(info);
        ALOGI("sent system info");
    }
    return ScopedAStatus::ok();
}

ScopedAStatus Gnss::start() {
    ALOGI("framework call: start (threadpool serving OK)");
    if (mSessionActive.exchange(true)) {
        return ScopedAStatus::ok();  // already running
    }

    if (!mThreadRunning.exchange(true)) {
        mThread = std::thread(&Gnss::readerThreadLoop, this);
    }

    reportStatus(IGnssCallback::GnssStatusValue::SESSION_BEGIN);
    sendUbxRate(mFixIntervalMs);
    ALOGI("session started, fix interval %u ms", mFixIntervalMs.load());
    return ScopedAStatus::ok();
}

ScopedAStatus Gnss::stop() {
    ALOGI("framework call: stop");
    if (!mSessionActive.exchange(false)) {
        return ScopedAStatus::ok();
    }
    sendUbxRate(kIdleRateMs);
    reportStatus(IGnssCallback::GnssStatusValue::SESSION_END);
    ALOGI("session stopped");
    return ScopedAStatus::ok();
}

ScopedAStatus Gnss::close() {
    ALOGI("framework call: close");
    stop();
    mThreadRunning = false;
    teardownTransport();  // unblocks the reader's blocking read()
    if (mThread.joinable()) {
        mThread.join();
    }
    std::lock_guard<std::mutex> lock(mCallbackMutex);
    mCallback = nullptr;
    return ScopedAStatus::ok();
}

// ---- reader thread ----------------------------------------------------------

void Gnss::readerThreadLoop() {
    const TransportConfig cfg = TransportConfig::fromProps();

    while (mThreadRunning) {
        std::unique_ptr<Transport> t = createTransport(cfg);
        if (t == nullptr) {
            ALOGI("no transport yet (source=%s); retrying every %d ms", cfg.source.c_str(),
                  kRetryDelayMs);
            for (int i = 0; i < kRetryDelayMs / 100 && mThreadRunning; ++i) sleepMs(100);
            continue;
        }

        {
            std::lock_guard<std::mutex> lock(mTransportMutex);
            mTransport = std::move(t);
            mTransportName = mTransport->name();
        }
        ALOGI("transport online: %s", mTransportName.c_str());
        sendUbxRate(mSessionActive ? mFixIntervalMs.load() : kIdleRateMs);

        char buf[512];
        while (mThreadRunning) {
            ssize_t n;
            {
                std::lock_guard<std::mutex> lock(mTransportMutex);
                if (mTransport == nullptr) break;
                n = mTransport->read(buf, sizeof(buf));
            }

            if (n > 0) {
                if (mVerboseRaw) {
                    std::string s(buf, (size_t)n);
                    ALOGI("raw[%zd]: %s", n, s.c_str());
                }
                if (mSessionActive) {
                    mParser.addBytes(buf, (size_t)n);
                }
            } else if (n == 0) {
                continue;  // idle poll window (usbfs timeout)
            } else {
                ALOGW("transport %s lost; reconnecting", mTransportName.c_str());
                break;
            }
        }

        teardownTransport();
        if (mThreadRunning) sleepMs(500);
    }
    ALOGI("reader thread exiting");
}

void Gnss::teardownTransport() {
    std::lock_guard<std::mutex> lock(mTransportMutex);
    if (mTransport != nullptr) {
        mTransport->close();
        mTransport.reset();
    }
}

// ---- UBX CFG-RATE (best-effort; only u-blox receivers react) ----------------

void Gnss::sendUbxRate(unsigned short periodMs) {
    unsigned char buff[14] = {0xB5, 0x62, 0x06, 0x08, 0x06, 0x00};
    buff[6] = periodMs & 0xFF;
    buff[7] = (periodMs >> 8) & 0xFF;
    buff[8] = 1;  // navRate
    buff[9] = 0;
    buff[10] = 1;  // timeRef = GPS time
    buff[11] = 0;
    unsigned char ckA = 0, ckB = 0;
    for (int i = 2; i < 12; ++i) {
        ckA += buff[i];
        ckB += ckA;
    }
    buff[12] = ckA;
    buff[13] = ckB;

    std::lock_guard<std::mutex> lock(mTransportMutex);
    if (mTransport != nullptr) {
        mTransport->write((const char*)buff, sizeof(buff));
    }
}

// ---- report helpers ---------------------------------------------------------

namespace {
// 读 persist.vendor.gnss_usb.<key>，默认 def("0"/"1")
bool propOn(const char* key, const char* def) {
    char prop[PROP_VALUE_MAX];
    property_get((std::string("persist.vendor.gnss_usb.") + key).c_str(), prop, def);
    return !strcmp(prop, "1") || !strcmp(prop, "true");
}
}  // namespace

void Gnss::reportLocation(const GnssLocation& locationIn) {
    if (!propOn("cb_loc", "1")) return;
    std::lock_guard<std::mutex> lock(mCallbackMutex);
    if (!mCallback) return;

    GnssLocation location = locationIn;
    if (propOn("cb_loc_min", "0")) {
        // 最小定位：只保留经纬度/时间/水平精度，规避 HyperOS 对其它字段的兼容问题
        constexpr uint32_t kMinFlags = GnssLocation::HAS_LAT_LONG | GnssLocation::HAS_HORIZONTAL_ACCURACY;
        location.gnssLocationFlags = kMinFlags;
        location.verticalAccuracyMeters = 0;
        location.speedAccuracyMetersPerSecond = 0;
        location.bearingAccuracyDegrees = 0;
        location.altitudeMeters = 0;
        location.speedMetersPerSec = 0;
        location.bearingDegrees = 0;
    }
    mCallback->gnssLocationCb(location);
}

void Gnss::reportSvStatus(const std::vector<IGnssCallback::GnssSvInfo>& svList) {
    // 框架的 startSvStatus/stopSvStatus 开关 + 属性总闸（默认开：实测全级稳定）
    if (!mSvStatusEnabled) return;
    if (!propOn("cb_sv", "1")) return;
    std::lock_guard<std::mutex> lock(mCallbackMutex);
    if (mCallback) mCallback->gnssSvStatusCb(svList);
}

void Gnss::reportNmea(int64_t timestampMs, const std::string& nmea) {
    if (!mNmeaEnabled) return;
    if (!propOn("cb_nmea", "1")) return;
    std::lock_guard<std::mutex> lock(mCallbackMutex);
    if (mCallback) mCallback->gnssNmeaCb(timestampMs, nmea);
}

void Gnss::reportStatus(IGnssCallback::GnssStatusValue status) {
    std::lock_guard<std::mutex> lock(mCallbackMutex);
    if (mCallback) mCallback->gnssStatusCb(status);
}

// ---- scheduling & toggles ---------------------------------------------------

ScopedAStatus Gnss::setPositionMode(const IGnss::PositionModeOptions& options) {
    unsigned short rate = clampRateMs(options.minIntervalMs);
    mFixIntervalMs = rate;
    if (mSessionActive) {
        sendUbxRate(rate);
    }
    ALOGI("setPositionMode: minInterval=%d ms -> rate=%u ms", options.minIntervalMs, rate);
    return ScopedAStatus::ok();
}

ScopedAStatus Gnss::startSvStatus() {
    mSvStatusEnabled = true;
    return ScopedAStatus::ok();
}
ScopedAStatus Gnss::stopSvStatus() {
    mSvStatusEnabled = false;
    return ScopedAStatus::ok();
}
ScopedAStatus Gnss::startNmea() {
    mNmeaEnabled = true;
    return ScopedAStatus::ok();
}
ScopedAStatus Gnss::stopNmea() {
    mNmeaEnabled = false;
    return ScopedAStatus::ok();
}

// ---- assistance injection (no-ops for a bare serial receiver) ---------------

ScopedAStatus Gnss::injectTime(int64_t, int64_t, int) {
    return ScopedAStatus::ok();
}
ScopedAStatus Gnss::injectLocation(const GnssLocation&) {
    return ScopedAStatus::ok();
}
ScopedAStatus Gnss::injectBestLocation(const GnssLocation&) {
    return ScopedAStatus::ok();
}
ScopedAStatus Gnss::deleteAidingData(IGnss::GnssAidingData) {
    return ScopedAStatus::ok();
}

// ---- extensions -------------------------------------------------------------

ScopedAStatus Gnss::getExtensionGnssConfiguration(std::shared_ptr<IGnssConfiguration>* out) {
    *out = mGnssConfiguration;
    return ScopedAStatus::ok();
}

ScopedAStatus Gnss::getExtensionGnssPowerIndication(
        std::shared_ptr<IGnssPowerIndication>* out) {
    *out = mGnssPowerIndication;
    return ScopedAStatus::ok();
}

ScopedAStatus Gnss::getExtensionGnssMeasurement(
        std::shared_ptr<IGnssMeasurementInterface>* out) {
    *out = ndk::SharedRefBase::make<GnssMeasurementInterface>();
    return ScopedAStatus::ok();
}

ScopedAStatus Gnss::getExtensionAGnss(std::shared_ptr<IAGnss>* out) {
    *out = ndk::SharedRefBase::make<AGnss>();
    return ScopedAStatus::ok();
}

ScopedAStatus Gnss::getExtensionAGnssRil(std::shared_ptr<IAGnssRil>* out) {
    *out = ndk::SharedRefBase::make<AGnssRil>();
    return ScopedAStatus::ok();
}

ScopedAStatus Gnss::getExtensionGnssVisibilityControl(
        std::shared_ptr<visibility_control::IGnssVisibilityControl>* out) {
    *out = ndk::SharedRefBase::make<visibility_control::GnssVisibilityControl>();
    return ScopedAStatus::ok();
}

ScopedAStatus Gnss::getExtensionGnssDebug(std::shared_ptr<IGnssDebug>* out) {
    *out = ndk::SharedRefBase::make<GnssDebug>();
    return ScopedAStatus::ok();
}

ScopedAStatus Gnss::getExtensionGnssAntennaInfo(std::shared_ptr<IGnssAntennaInfo>* out) {
    *out = ndk::SharedRefBase::make<GnssAntennaInfo>();
    return ScopedAStatus::ok();
}

// Unsupported extensions must fail the getter: the framework's wrappers only
// check the returned status (checkAidlStatus), not the pointer, and wrap a
// null binder — system_server then SIGSEGVs on wrapper->setCallback (upstream
// gnss-aidl-serial note, seen with IGnssPsds during GnssNative init). A
// non-OK status makes the framework treat the extension as absent.
ScopedAStatus Gnss::getExtensionPsds(std::shared_ptr<IGnssPsds>* out) {
    *out = nullptr;
    return ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}
ScopedAStatus Gnss::getExtensionGnssBatching(std::shared_ptr<IGnssBatching>* out) {
    *out = nullptr;
    return ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}
ScopedAStatus Gnss::getExtensionGnssGeofence(std::shared_ptr<IGnssGeofence>* out) {
    *out = nullptr;
    return ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}
ScopedAStatus Gnss::getExtensionGnssNavigationMessage(
        std::shared_ptr<IGnssNavigationMessageInterface>* out) {
    *out = nullptr;
    return ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}
ScopedAStatus Gnss::getExtensionMeasurementCorrections(
        std::shared_ptr<measurement_corrections::IMeasurementCorrectionsInterface>* out) {
    *out = nullptr;
    return ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

}  // namespace aidl::android::hardware::gnss
