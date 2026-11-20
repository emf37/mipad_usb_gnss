/*
 * USB/serial NMEA GNSS HAL for KernelSU deployment.
 *
 * Derived from gnss-aidl-serial (https://github.com/aselafernando/gnss-aidl-serial),
 * Copyright (C) 2026 Asela Fernando, Apache-2.0: the AIDL surface, NMEA parser
 * and extension handling are kept; the serial layer is replaced by the
 * pluggable Transport backends (usbfs CH340 / kernel tty / socket).
 *
 * Copyright (C) 2026 ksu-gnss contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <aidl/android/hardware/gnss/BnGnss.h>
#include <aidl/android/hardware/gnss/BnGnssConfiguration.h>
#include <aidl/android/hardware/gnss/BnGnssPowerIndication.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>

#include "ControlServer.h"
#include "CasicFrames.h"
#include "GnssPsds.h"
#include "NmeaParser.h"
#include "Transport.h"

namespace aidl::android::hardware::gnss {

/**
 * NMEA GNSS HAL fed by a Transport byte stream. See Transport.h for the
 * backends and the persist.vendor.gnss_usb.* configuration properties.
 */
class Gnss : public BnGnss {
  public:
    Gnss();
    ~Gnss();

    // Core lifecycle
    ndk::ScopedAStatus setCallback(const std::shared_ptr<IGnssCallback>& callback) override;
    ndk::ScopedAStatus start() override;
    ndk::ScopedAStatus stop() override;
    ndk::ScopedAStatus close() override;

    // Position scheduling
    ndk::ScopedAStatus setPositionMode(const IGnss::PositionModeOptions& options) override;

    // Reporting toggles (V2+)
    ndk::ScopedAStatus startSvStatus() override;
    ndk::ScopedAStatus stopSvStatus() override;
    ndk::ScopedAStatus startNmea() override;
    ndk::ScopedAStatus stopNmea() override;

    // Assistance injection — accepted but unused by a bare serial receiver.
    ndk::ScopedAStatus injectTime(int64_t timeMs, int64_t timeReferenceMs,
                                  int uncertaintyMs) override;
    ndk::ScopedAStatus injectLocation(const GnssLocation& location) override;
    ndk::ScopedAStatus injectBestLocation(const GnssLocation& location) override;
    ndk::ScopedAStatus deleteAidingData(IGnss::GnssAidingData aidingDataFlags) override;

    // Non-nullable extensions (minimal implementations).
    ndk::ScopedAStatus getExtensionGnssConfiguration(
            std::shared_ptr<IGnssConfiguration>* out) override;
    ndk::ScopedAStatus getExtensionGnssPowerIndication(
            std::shared_ptr<IGnssPowerIndication>* out) override;
    ndk::ScopedAStatus getExtensionGnssMeasurement(
            std::shared_ptr<IGnssMeasurementInterface>* out) override;
    ndk::ScopedAStatus getExtensionAGnss(std::shared_ptr<IAGnss>* out) override;
    ndk::ScopedAStatus getExtensionAGnssRil(std::shared_ptr<IAGnssRil>* out) override;
    ndk::ScopedAStatus getExtensionGnssVisibilityControl(
            std::shared_ptr<visibility_control::IGnssVisibilityControl>* out) override;
    ndk::ScopedAStatus getExtensionGnssDebug(std::shared_ptr<IGnssDebug>* out) override;
    ndk::ScopedAStatus getExtensionGnssAntennaInfo(
            std::shared_ptr<IGnssAntennaInfo>* out) override;

    // PSDS/AGNSS injection via framework channel (validated, CASIC-only)
    ndk::ScopedAStatus getExtensionPsds(std::shared_ptr<IGnssPsds>* out) override;
    ndk::ScopedAStatus getExtensionGnssBatching(
            std::shared_ptr<IGnssBatching>* out) override;
    ndk::ScopedAStatus getExtensionGnssGeofence(
            std::shared_ptr<IGnssGeofence>* out) override;
    ndk::ScopedAStatus getExtensionGnssNavigationMessage(
            std::shared_ptr<IGnssNavigationMessageInterface>* out) override;
    ndk::ScopedAStatus getExtensionMeasurementCorrections(
            std::shared_ptr<measurement_corrections::IMeasurementCorrectionsInterface>* out)
            override;

  private:
    void readerThreadLoop();
    void teardownTransport();
    void sendUbxRate(unsigned short periodMs);
    bool sendRawCommand(const std::string& payload);   // control server hook
    std::string statusJson();                          // control server hook
    void sendAidIni();                                 // CASIC warm-start injection
    void saveLastFixPos(double lat, double lon, double alt);
    void loadLastFixPos();

    void reportLocation(const GnssLocation& location);
    void reportSvStatus(const std::vector<IGnssCallback::GnssSvInfo>& svList);
    void reportNmea(int64_t timestampMs, const std::string& nmea);
    void reportStatus(IGnssCallback::GnssStatusValue status);

    static unsigned short clampRateMs(int minIntervalMs);

    std::mutex mCallbackMutex;
    std::shared_ptr<IGnssCallback> mCallback;

    std::mutex mTransportMutex;
    std::unique_ptr<Transport> mTransport;
    std::string mTransportName;
    bool mVerboseRaw = false;

    // control server + live status snapshot
    std::unique_ptr<ControlServer> mControl;
    std::mutex mStatusMutex;
    int mSatCount = 0;          // sv list size of last report
    int mSatsUsed = 0;          // USED_IN_FIX count
    int mFixType = 0;           // 0=none 1=2D 2=3D (from GGA)
    double mLastLat = 0, mLastLon = 0;
    float mLastAcc = 0;
    std::string mLastNmea;

    NmeaParser mParser;

    std::thread mThread;
    std::atomic<bool> mThreadRunning{false};  // reader thread alive
    std::atomic<bool> mSessionActive{false};  // start()/stop() session

    std::atomic<bool> mNmeaEnabled{true};
    std::atomic<bool> mSvStatusEnabled{true};
    std::atomic<unsigned short> mFixIntervalMs{1000};

    std::shared_ptr<IGnssConfiguration> mGnssConfiguration;
    std::shared_ptr<IGnssPowerIndication> mGnssPowerIndication;
    std::shared_ptr<GnssPsds> mGnssPsds;

    // CASIC AID-INI warm-start assistance (framework injectTime/injectBestLocation)
    AidIniData mAidIni;
};

}  // namespace aidl::android::hardware::gnss
