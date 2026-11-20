/*
 * Local control server for the companion app (and power users via adb).
 *
 * Listens on 127.0.0.1:5001, line protocol:
 *   STATUS        -> one JSON line with transport/session/fix/satellite snapshot
 *   SEND <text>   -> write <text> + CRLF to the GNSS transport (module commands)
 *   SUB / UNSUB   -> toggle live NMEA forwarding to this client ("NMEA <line>")
 *
 * Copyright (C) 2026 ksu-gnss contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace aidl::android::hardware::gnss {

class ControlServer {
  public:
    using StatusFn = std::function<std::string()>;          // JSON snapshot
    using SendFn = std::function<bool(const std::string&)>;  // raw write + CRLF

    ControlServer(StatusFn status, SendFn send) : mStatus(std::move(status)), mSend(std::move(send)) {}
    ~ControlServer() { stop(); }

    void start(int port);
    void stop();

    // Broadcast one raw NMEA line to subscribed clients (called from reader).
    void broadcastNmea(const std::string& line);

  private:
    void acceptLoop();
    void clientLoop(int fd);

    StatusFn mStatus;
    SendFn mSend;

    int mListenFd = -1;
    std::thread mAcceptThread;
    std::atomic<bool> mRunning{false};

    std::mutex mClientsMutex;
    std::vector<int> mClients;  // subscribed fds
};

}  // namespace aidl::android::hardware::gnss
