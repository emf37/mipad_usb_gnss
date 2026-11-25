/*
 * Copyright (C) 2006 The Android Open Source Project
 * Copyright (C) 2026 Asela Fernando
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "gnss_serial_nmea"

#include "NmeaParser.h"

#include <aidl/android/hardware/gnss/ElapsedRealtime.h>
#include <aidl/android/hardware/gnss/GnssConstellationType.h>
#include <log/log.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <sys/time.h>

namespace aidl::android::hardware::gnss {

using GnssConstellationType = ::aidl::android::hardware::gnss::GnssConstellationType;

namespace {

// ---- minimal field tokenizer over a single sentence ------------------------

constexpr int kMaxTokens = 32;

struct Token {
    const char* p = "";
    const char* end = "";
    bool empty() const { return p >= end; }
    int len() const { return static_cast<int>(end - p); }
};

struct Tokenizer {
    int count = 0;
    Token tokens[kMaxTokens];

    Tokenizer(const char* p, const char* end) {
        // optional leading '$'
        if (p < end && p[0] == '$') p += 1;
        // strip trailing CR/LF
        if (end > p && end[-1] == '\n') {
            end -= 1;
            if (end > p && end[-1] == '\r') end -= 1;
        }
        // strip "*XX" checksum
        if (end >= p + 3 && end[-3] == '*') end -= 3;

        while (p < end && count < kMaxTokens) {
            const char* q = static_cast<const char*>(memchr(p, ',', end - p));
            if (q == nullptr) q = end;
            tokens[count].p = p;
            tokens[count].end = q;
            count += 1;
            p = (q < end) ? q + 1 : q;
        }
    }

    Token get(int index) const {
        if (index < 0 || index >= count) return Token{};
        return tokens[index];
    }
};

int str2int(const char* p, const char* end) {
    int result = 0;
    for (; p < end; p++) {
        int c = *p - '0';
        if ((unsigned)c >= 10) return -1;
        result = result * 10 + c;
    }
    return result;
}

double str2float(const char* p, const char* end) {
    size_t len = end - p;
    char temp[32];
    if (len == 0 || len >= sizeof(temp)) return 0.;
    memcpy(temp, p, len);
    temp[len] = '\0';
    return strtod(temp, nullptr);
}

// NMEA "ddmm.mmmm" -> decimal degrees
double convertFromHHMM(const Token& tok) {
    double val = str2float(tok.p, tok.end);
    int degrees = (int)(std::floor(val) / 100);
    double minutes = val - degrees * 100.;
    return degrees + minutes / 60.0;
}

// Map a GSV/GSA talker id (e.g. "GP", "GL", "GA", "GB"/"BD", "GQ") to a
// constellation, and translate the NMEA satellite number to a GNSS svid.
GnssConstellationType constellationForTalker(const char* talker) {
    if (!memcmp(talker, "GP", 2)) return GnssConstellationType::GPS;
    if (!memcmp(talker, "GL", 2)) return GnssConstellationType::GLONASS;
    if (!memcmp(talker, "GA", 2)) return GnssConstellationType::GALILEO;
    if (!memcmp(talker, "GB", 2) || !memcmp(talker, "BD", 2))
        return GnssConstellationType::BEIDOU;
    if (!memcmp(talker, "GQ", 2)) return GnssConstellationType::QZSS;
    return GnssConstellationType::UNKNOWN;
}

int64_t bootTimeNanos() {
    struct timespec ts;
    clock_gettime(CLOCK_BOOTTIME, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}

// NMEA 4.10 signal-id -> carrier frequency (Hz) per constellation.
// Dual-frequency receivers (e.g. ATGM336H-5N, BK166X) emit one GSV burst per
// band, distinguished by the trailing signalId field (0 = all/legacy single).
// Returns 0 for unknown: caller keeps HAS_CARRIER_FREQUENCY off so entries
// aggregate like the legacy single-band behavior.
int64_t carrierForSignalId(GnssConstellationType c, int signalId) {
    constexpr int64_t kL1 = 1575420000LL;   // GPS L1 / Galileo E1 / BDS B1C / QZSS L1
    constexpr int64_t kL5 = 1176450000LL;   // GPS L5 / Galileo E5a / BDS B2a / QZSS L5 / NavIC L5
    constexpr int64_t kL2 = 1227600000LL;   // GPS L2 / QZSS L2
    constexpr int64_t kB1I = 1561098000LL;  // BDS B1I
    constexpr int64_t kB2I = 1207140000LL;  // BDS B2I / Galileo E5b
    constexpr int64_t kB3I = 1268520000LL;  // BDS B3I
    constexpr int64_t kE6 = 1278750000LL;   // Galileo E6
    constexpr int64_t kGloL1 = 1602000000LL;  // GLONASS L1 FDMA nominal (k=0)
    constexpr int64_t kGloL2 = 1246000000LL;  // GLONASS L2 FDMA nominal (k=0)

    if (signalId <= 0) return 0;
    switch (c) {
        case GnssConstellationType::GPS:
        case GnssConstellationType::QZSS:
            // 1 L1C/A | 2 L1P | 5 L2P | 6/7 L2C | 8/9 L5 | 10/11 L1C
            if (signalId == 1 || signalId == 2 || signalId == 10 || signalId == 11) return kL1;
            if (signalId == 5 || signalId == 6 || signalId == 7) return kL2;
            if (signalId == 8 || signalId == 9) return kL5;
            return 0;
        case GnssConstellationType::GLONASS:
            // 1/2 L1 | 3/4 L2
            if (signalId == 1 || signalId == 2) return kGloL1;
            if (signalId == 3 || signalId == 4) return kGloL2;
            return 0;
        case GnssConstellationType::GALILEO:
            // 1/2 E5a | 3/4/5 E5b | 6 E6C | 7/8/9 E1
            if (signalId == 1 || signalId == 2) return kL5;
            if (signalId == 3 || signalId == 4 || signalId == 5) return kB2I;
            if (signalId == 6) return kE6;
            if (signalId == 7 || signalId == 8 || signalId == 9) return kL1;
            return 0;
        case GnssConstellationType::BEIDOU:
            // 1/2/3 B1I | 4/5/6 B2I | 7 B3I | 8 B1C | 9 B2a
            if (signalId >= 1 && signalId <= 3) return kB1I;
            if (signalId == 4 || signalId == 5 || signalId == 6) return kB2I;
            if (signalId == 7) return kB3I;
            if (signalId == 8) return kL1;
            if (signalId == 9) return kL5;
            return 0;
        case GnssConstellationType::IRNSS:
            if (signalId == 1) return kL5;             // L5 SPS
            if (signalId == 9) return 2492027500LL;    // S band
            return 0;
        default:
            return 0;
    }
}

void stampElapsedRealtime(GnssLocation* loc) {
    loc->elapsedRealtime.flags = ElapsedRealtime::HAS_TIMESTAMP_NS |
                                 ElapsedRealtime::HAS_TIME_UNCERTAINTY_NS;
    loc->elapsedRealtime.timestampNs = bootTimeNanos();
    loc->elapsedRealtime.timeUncertaintyNs = 0;
}

}  // namespace

void NmeaParser::reset() {
    mPos = 0;
    mOverflow = false;
    mUtcYear = mUtcMon = mUtcDay = -1;
    mHaveGsa = false;
    mFix = GnssLocation{};
    mSvs.clear();
    mBucketSeenMs.clear();
    mUsedPrns.clear();
    mUsedConsts.clear();
}

void NmeaParser::addBytes(const char* data, size_t len) {
    for (size_t i = 0; i < len; i++) addChar(data[i]);
}

void NmeaParser::addChar(char c) {
    if (mOverflow) {
        mOverflow = (c != '\n');
        return;
    }
    if (mPos >= kMaxNmeaSize) {
        mOverflow = true;
        mPos = 0;
        return;
    }
    mLine[mPos++] = c;
    if (c == '\n') {
        parseSentence();
        mPos = 0;
    }
}

void NmeaParser::parseSentence() {
    if (mPos < 9) return;
    mLine[mPos] = 0;

    struct timeval tv;
    gettimeofday(&tv, nullptr);
    const int64_t nowMs = (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;

    // Raw sentence to the framework (gnssNmeaCb) regardless of type.
    if (mNmeaCb) mNmeaCb(nowMs, std::string(mLine, mPos));

    Tokenizer tzer(mLine, mLine + mPos);
    Token id = tzer.get(0);
    if (id.len() < 5) return;

    const char* talker = id.p;       // e.g. "GP"
    const char* type = id.p + 2;     // e.g. "GGA"

    bool sendFix = false;

    if (!memcmp(type, "GGA", 3)) {
        // field 1 (UTC time) is unused: RMC carries both date and time
        Token tokLat = tzer.get(2), tokLatH = tzer.get(3);
        Token tokLon = tzer.get(4), tokLonH = tzer.get(5);
        Token tokFix = tzer.get(6);
        Token tokAcc = tzer.get(8);
        Token tokAlt = tzer.get(9);

        int fix = str2int(tokFix.p, tokFix.end);
        if (fix > 0) {
            if (tokLat.len() >= 6 && tokLon.len() >= 6) {
                double lat = convertFromHHMM(tokLat);
                if (!tokLatH.empty() && tokLatH.p[0] == 'S') lat = -lat;
                double lon = convertFromHHMM(tokLon);
                if (!tokLonH.empty() && tokLonH.p[0] == 'W') lon = -lon;
                mFix.gnssLocationFlags |= GnssLocation::HAS_LAT_LONG;
                mFix.latitudeDegrees = lat;
                mFix.longitudeDegrees = lon;
            }
            if (!tokAlt.empty()) {
                mFix.gnssLocationFlags |= GnssLocation::HAS_ALTITUDE;
                mFix.altitudeMeters = str2float(tokAlt.p, tokAlt.end);
            }
        }
        if (!mHaveGsa && !tokAcc.empty()) {
            double acc = str2float(tokAcc.p, tokAcc.end);
            if (acc <= 99.0 && fix > 0) {
                mFix.gnssLocationFlags |= GnssLocation::HAS_HORIZONTAL_ACCURACY;
                mFix.horizontalAccuracyMeters = acc;
            }
            sendFix = true;
        }
    } else if (!memcmp(type, "GSA", 3)) {
        Token tokMode = tzer.get(2);  // 1=no fix, 2=2D, 3=3D
        Token tokHdop = tzer.get(16);
        int mode = str2int(tokMode.p, tokMode.end);
        if (mode == 2) mFix.gnssLocationFlags &= ~GnssLocation::HAS_ALTITUDE;
        if (!tokHdop.empty() && mode > 1) {
            double acc = str2float(tokHdop.p, tokHdop.end);
            if (acc <= 99.0) {
                mFix.gnssLocationFlags |= GnssLocation::HAS_HORIZONTAL_ACCURACY;
                mFix.horizontalAccuracyMeters = acc;
            }
        }
        // Dual-constellation receivers send one GSA per system each epoch;
        // later ones (often mode=1 with empty lists for unused systems) must
        // not wipe earlier ones' used satellites. Accumulate per epoch with
        // the trailing GNSS system id (ATGM: 1=GPS 2=GLONASS 4=BDS) so
        // GSV marking can match constellation-aware.
        if (mode >= 2) {
            int sysId = str2int(tzer.get(18).p, tzer.get(18).end);
            GnssConstellationType gsaConst = GnssConstellationType::UNKNOWN;
            if (sysId == 1) gsaConst = GnssConstellationType::GPS;
            else if (sysId == 2) gsaConst = GnssConstellationType::GLONASS;
            else if (sysId == 4) gsaConst = GnssConstellationType::BEIDOU;
            else if (sysId == 8) gsaConst = GnssConstellationType::GALILEO;
            for (int i = 0; i < 12; i++) {
                Token t = tzer.get(3 + i);
                if (t.empty()) continue;
                int prn = str2int(t.p, t.end);
                if (prn <= 0) continue;
                mUsedPrns.push_back(prn);
                mUsedConsts.push_back(gsaConst);
            }
        }
        mHaveGsa = true;
        sendFix = true;
    } else if (!memcmp(type, "GSV", 3)) {
        GnssConstellationType constellation = constellationForTalker(talker);
        int numMessages = str2int(tzer.get(1).p, tzer.get(1).end);
        int msgNumber = str2int(tzer.get(2).p, tzer.get(2).end);
        int svsInView = str2int(tzer.get(3).p, tzer.get(3).end);

        // Groups in THIS message follow from the burst arithmetic: a burst of
        // numSv satellites carries min(4, numSv-(msgNo-1)*4) here. Deriving
        // groups from non-empty-field counting misreads the trailing NMEA 4.10
        // signalId as a phantom PRN on short messages (observed: ATGM's
        // "...,42,37,309,29,1*4C" -> satellite "PRN 1" leaking into a
        // carrier-0 bucket that is never erased -> list grows to the cap).
        int expected = svsInView - (msgNumber - 1) * 4;
        if (expected < 0) expected = 0;
        if (expected > 4) expected = 4;
        int signalId = str2int(tzer.get(4 + expected * 4).p, tzer.get(4 + expected * 4).end);
        int64_t carrierHz = carrierForSignalId(constellation, signalId);

        if (msgNumber == 1) {
            // Start of a (constellation, band) burst: drop previous entries for
            // that same constellation+carrier only. carrier==0 entries form the
            // single legacy "unknown band" bucket, preserving old firmware
            // behavior where every constellation had exactly one burst.
            mBucketSeenMs[{(int)constellation, carrierHz}] = bootTimeNanos() / 1000000;
            mSvs.erase(std::remove_if(mSvs.begin(), mSvs.end(),
                                      [&](const GnssSvInfo& s) {
                                          return s.constellation == constellation &&
                                                 s.carrierFrequencyHz == carrierHz;
                                      }),
                       mSvs.end());
        }

        for (int i = 0; i < expected; i++) {
            int base = 4 + i * 4;
            Token tokPrn = tzer.get(base);
            if (tokPrn.empty()) continue;
            int prn = str2int(tokPrn.p, tokPrn.end);
            if (prn <= 0) continue;

            GnssSvInfo sv;
            sv.svid = prn;
            sv.constellation = constellation;
            Token tokElev = tzer.get(base + 1);
            Token tokAzim = tzer.get(base + 2);
            Token tokSnr = tzer.get(base + 3);
            sv.elevationDegrees = tokElev.empty() ? 0 : str2float(tokElev.p, tokElev.end);
            sv.azimuthDegrees = tokAzim.empty() ? 0 : str2float(tokAzim.p, tokAzim.end);
            sv.cN0Dbhz = tokSnr.empty() ? 0 : str2float(tokSnr.p, tokSnr.end);
            sv.basebandCN0DbHz = sv.cN0Dbhz;
            sv.svFlag = 0;
            if (carrierHz != 0) {
                sv.carrierFrequencyHz = carrierHz;
                sv.svFlag |= (int)IGnssCallback::GnssSvFlags::HAS_CARRIER_FREQUENCY;
            } else {
                sv.carrierFrequencyHz = 0;
            }
            for (size_t u = 0; u < mUsedPrns.size(); u++) {
                if (mUsedPrns[u] != prn) continue;
                if (mUsedConsts[u] == GnssConstellationType::UNKNOWN ||
                    mUsedConsts[u] == constellation) {
                    sv.svFlag |= (int)IGnssCallback::GnssSvFlags::USED_IN_FIX;
                    break;
                }
            }
            if ((int)mSvs.size() < kMaxSvs) mSvs.push_back(sv);
        }

        // Mark dirty; flushed once per NMEA epoch (on RMC/GGA, which arrive
        // before this epoch's GSV burst). Emitting per-burst caused the
        // framework sky view to flicker with partially-stale lists several
        // times per second.
        mSvsDirty = true;
        (void)svsInView;
        (void)numMessages;
        (void)msgNumber;
    } else if (!memcmp(type, "RMC", 3)) {
        Token tokTime = tzer.get(1);
        Token tokStatus = tzer.get(2);  // A=valid, V=warning
        Token tokLat = tzer.get(3), tokLatH = tzer.get(4);
        Token tokLon = tzer.get(5), tokLonH = tzer.get(6);
        Token tokSpeed = tzer.get(7);
        Token tokBearing = tzer.get(8);
        Token tokDate = tzer.get(9);

        if (!tokStatus.empty() && tokStatus.p[0] == 'A') {
            // date (ddmmyy)
            if (tokDate.len() == 6) {
                mUtcDay = str2int(tokDate.p, tokDate.p + 2);
                mUtcMon = str2int(tokDate.p + 2, tokDate.p + 4);
                mUtcYear = str2int(tokDate.p + 4, tokDate.p + 6) + 2000;
            }
            // time + date -> epoch ms
            if (mUtcYear > 0 && tokTime.len() >= 6) {
                struct tm tm = {};
                tm.tm_hour = str2int(tokTime.p, tokTime.p + 2);
                tm.tm_min = str2int(tokTime.p + 2, tokTime.p + 4);
                tm.tm_sec = (int)str2float(tokTime.p + 4, tokTime.end);
                tm.tm_year = mUtcYear - 1900;
                tm.tm_mon = mUtcMon - 1;
                tm.tm_mday = mUtcDay;
                tm.tm_isdst = -1;
                time_t gmt = timegm(&tm);

                // Module-time sanity: some receivers ship a stale RTC epoch
                // (observed: ATGM336H reporting 2020-11-26 while fixed, dead
                // backup battery / firmware epoch bug) or a legacy 10-bit GPS
                // week counter (SiRF era, off by exact N*1024 weeks). Any of
                // those produces timestamps years away from the system clock,
                // which apps reject ("No Fix"). Trust the system clock when
                // the receiver date is off by more than 90 days; otherwise
                // apply the classic 1024-week rollover shift if it lands
                // within a year of the system clock.
                if (gmt > 0) {
                    time_t sysNow = time(nullptr);
                    time_t delta = gmt - sysNow;
                    if (delta < 0) delta = -delta;
                    constexpr time_t k90Days = 90LL * 24 * 3600;
                    constexpr time_t kRolloverPeriodSec = 1024LL * 7 * 24 * 3600;
                    if (delta > k90Days) {
                        // try rollover correction first (only if it helps)
                        time_t corrected = gmt;
                        if (corrected < sysNow) {
                            while (corrected < sysNow - k90Days) corrected += kRolloverPeriodSec;
                        }
                        time_t delta2 = corrected - sysNow;
                        if (delta2 < 0) delta2 = -delta2;
                        if (delta2 <= k90Days) {
                            gmt = corrected;
                            if (!mRolloverLogged) {
                                ALOGI("GPS week rollover: %ld -> %ld", (long)gmt,
                                      (long)corrected);
                                mRolloverLogged = true;
                            }
                        } else {
                            // unfixable stale RTC (e.g. 2020 epoch): fall back
                            // to the system clock for the location timestamp
                            if (!mRolloverLogged) {
                                ALOGI("receiver date implausible (%ld, sys %ld): "
                                      "using system clock",
                                      (long)gmt, (long)sysNow);
                                mRolloverLogged = true;
                            }
                            gmt = sysNow;
                        }
                    }
                }

                mFix.timestampMillis = (int64_t)gmt * 1000;

                if (mTimeSyncThreshold > 0) {
                    long diff = (long)(time(nullptr) - gmt);
                    if (diff < -mTimeSyncThreshold || diff > mTimeSyncThreshold) {
                        struct timeval newtv = {gmt, 0};
                        if (settimeofday(&newtv, nullptr) == 0) {
                            ALOGD("system clock steered to GPS UTC time");
                        }
                    }
                }
            }
            if (tokLat.len() >= 6 && tokLon.len() >= 6) {
                double lat = convertFromHHMM(tokLat);
                if (!tokLatH.empty() && tokLatH.p[0] == 'S') lat = -lat;
                double lon = convertFromHHMM(tokLon);
                if (!tokLonH.empty() && tokLonH.p[0] == 'W') lon = -lon;
                mFix.gnssLocationFlags |= GnssLocation::HAS_LAT_LONG;
                mFix.latitudeDegrees = lat;
                mFix.longitudeDegrees = lon;
            }
            if (!tokBearing.empty()) {
                mFix.gnssLocationFlags |= GnssLocation::HAS_BEARING;
                mFix.bearingDegrees = str2float(tokBearing.p, tokBearing.end);
            }
            if (!tokSpeed.empty()) {
                mFix.gnssLocationFlags |= GnssLocation::HAS_SPEED;
                mFix.speedMetersPerSec =
                        str2float(tokSpeed.p, tokSpeed.end) * (1.852 / 3.6);  // knots -> m/s
            }
        }
    } else if (!memcmp(type, "VTG", 3)) {
        Token tokStatus = tzer.get(9);
        if (!tokStatus.empty() && tokStatus.p[0] != 'N') {
            Token tokBearing = tzer.get(1);  // true track, degrees
            Token tokSpeed = tzer.get(5);    // speed over ground, knots
            if (!tokBearing.empty()) {
                mFix.gnssLocationFlags |= GnssLocation::HAS_BEARING;
                mFix.bearingDegrees = str2float(tokBearing.p, tokBearing.end);
            }
            if (!tokSpeed.empty()) {
                mFix.gnssLocationFlags |= GnssLocation::HAS_SPEED;
                mFix.speedMetersPerSec =
                        str2float(tokSpeed.p, tokSpeed.end) * (1.852 / 3.6);
            }
        }
    }

    // RMC opens a new epoch: flush last epoch's completed satellite list
    // exactly once (smooth 1Hz sky view) and reset the per-epoch used set.
    if (!memcmp(type, "RMC", 3)) {
        mUsedPrns.clear();
        mUsedConsts.clear();
        // Evict (constellation, carrier) buckets whose GSV burst has not
        // restarted for 5 s: receivers drop bands after the acquisition/AGNSS
        // phase and those entries are never erased by the msg-1 wipe, pinning
        // the list at kMaxSvs with zombies (observed: 81 live + 47 stale).
        int64_t nowMs = bootTimeNanos() / 1000000;
        mSvs.erase(std::remove_if(mSvs.begin(), mSvs.end(),
                                  [&](const GnssSvInfo& s) {
                                      auto it = mBucketSeenMs.find(
                                              {(int)s.constellation, s.carrierFrequencyHz});
                                      return it == mBucketSeenMs.end() ||
                                             nowMs - it->second > 5000;
                                  }),
                   mSvs.end());
        for (auto it = mBucketSeenMs.begin(); it != mBucketSeenMs.end();) {
            if (nowMs - it->second > 5000) it = mBucketSeenMs.erase(it);
            else ++it;
        }
        if (mSvsDirty && mSvStatusCb) {
            mSvStatusCb(mSvs);
            mSvsDirty = false;
        }
    }

    if (sendFix && (mFix.gnssLocationFlags & GnssLocation::HAS_LAT_LONG) && mLocationCb) {
        stampElapsedRealtime(&mFix);
        mLocationCb(mFix);
        // clear per-cycle accumulation; keep date for the next RMC
        mFix.gnssLocationFlags = 0;
    }
}

}  // namespace aidl::android::hardware::gnss
