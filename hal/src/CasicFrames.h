/*
 * CASIC binary frame builder (AID-INI time/position injection).
 *
 * Frame: BA CE | len_le16 | class | id | payload | ckSum32
 * ckSum = (id<<24)+(class<<16)+len + sum of payload as LE32 words.
 * Per CASIC protocol spec V4 (中科微多模卫星导航接收机协议规范, 2.2/2.14.1).
 *
 * Copyright (C) 2026 ksu-gnss contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

namespace aidl::android::hardware::gnss {

inline uint32_t casicChecksum(uint8_t msgClass, uint8_t msgId, uint16_t len,
                              const uint8_t* payload) {
    uint32_t ck = ((uint32_t)msgId << 24) | ((uint32_t)msgClass << 16) | len;
    for (unsigned i = 0; i + 4 <= len; i += 4) {
        uint32_t w = (uint32_t)payload[i] | ((uint32_t)payload[i + 1] << 8) |
                     ((uint32_t)payload[i + 2] << 16) | ((uint32_t)payload[i + 3] << 24);
        ck += w;
    }
    return ck;
}

inline std::vector<uint8_t> casicFrame(uint8_t msgClass, uint8_t msgId,
                                       const std::vector<uint8_t>& payload) {
    uint16_t len = (uint16_t)payload.size();
    std::vector<uint8_t> f;
    f.reserve(8 + len + 4);
    f.push_back(0xBA);
    f.push_back(0xCE);
    f.push_back((uint8_t)(len & 0xFF));
    f.push_back((uint8_t)(len >> 8));
    f.push_back(msgClass);
    f.push_back(msgId);
    f.insert(f.end(), payload.begin(), payload.end());
    uint32_t ck = casicChecksum(msgClass, msgId, len, payload.data());
    f.push_back((uint8_t)(ck & 0xFF));
    f.push_back((uint8_t)((ck >> 8) & 0xFF));
    f.push_back((uint8_t)((ck >> 16) & 0xFF));
    f.push_back((uint8_t)((ck >> 24) & 0xFF));
    return f;
}

/**
 * AID-INI (0x0B 0x01), 56-byte payload:
 *   0  R8 ecefXOrLat   8  R8 ecefYOrLon  16 R8 ecefZOrAlt
 *  24  R8 tow(sec)     32 R4 freqBias    36 R4 pAcc
 *  40  R4 tAcc         44 R4 fAcc        48 U4 res
 *  52  U2 wn           54 U1 timeSource   55 U1 flags
 * flags: B0 pos valid | B1 time valid | B5 pos is LLA degrees
 */
struct AidIniData {
    bool hasPos = false;
    double latDeg = 0, lonDeg = 0, altM = 0;   // LLA
    bool hasTime = false;
    double towSec = 0;                          // GPS time-of-week
    uint16_t gpsWeek = 0;
    double posAccM2 = 1e6;                      // variance m^2
    double tAccS2 = 9;                          // variance s^2 (~10ns)
};

inline std::vector<uint8_t> buildAidIni(const AidIniData& d) {
    std::vector<uint8_t> p(56, 0);
    auto putR8 = [&](size_t off, double v) { memcpy(&p[off], &v, 8); };
    auto putR4 = [&](size_t off, float v) { memcpy(&p[off], &v, 4); };

    if (d.hasPos) {
        putR8(0, d.latDeg);
        putR8(8, d.lonDeg);
        putR8(16, d.altM);
        float pAcc = (float)d.posAccM2;
        putR4(36, pAcc);
    }
    if (d.hasTime) {
        putR8(24, d.towSec);
        uint16_t wn = d.gpsWeek;
        memcpy(&p[52], &wn, 2);
        p[54] = 0;  // timeSource: 0=unknown source (fine for warm start)
        float tAcc = (float)d.tAccS2;
        putR4(40, tAcc);
    }
    uint8_t flags = 0;
    if (d.hasPos) flags |= 0x01 | 0x20;  // B0 pos valid | B5 LLA format
    if (d.hasTime) flags |= 0x02;        // B1 time valid
    p[55] = flags;

    return casicFrame(0x0B, 0x01, p);
}

}  // namespace aidl::android::hardware::gnss
