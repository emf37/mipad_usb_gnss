#define LOG_TAG "gnss_usb"

#include "Gnss.h"

#include <android/binder_ibinder.h>
#include <android/binder_status.h>
#include <dlfcn.h>
#include <log/log.h>
#include <unistd.h>

#include <atomic>
#include <string>
#include <thread>

using ::aidl::android::hardware::gnss::Gnss;

namespace {

// libbinder_ndk's service registration entry points are platform-only: the
// NDK import stub does not export them, but every device's real
// /system/lib64/libbinder_ndk.so does. Resolve them at runtime.
struct BinderNdkApi {
    binder_status_t (*addService)(AIBinder* binder, const char* instance) = nullptr;
    AIBinder* (*checkService)(const char* instance) = nullptr;
    AIBinder* (*waitForDeclaredService)(const char* instance) = nullptr;
    bool (*isDeclared)(const char* instance) = nullptr;
    void (*setThreadPoolMaxThreadCount)(uint32_t numThreads) = nullptr;
    void (*joinThreadPool)() = nullptr;
};

bool loadBinderNdk(BinderNdkApi* api) {
    // Ask libbinder to treat this process as vendor-side so that the VINTF
    // stability mark on our HAL binder is not downgraded to system stability.
    // Best effort: the lib may not exist on post-HIDL builds.
    void* vendorBinder = dlopen("libbinder_vendor.so", RTLD_NOW);
    ALOGI("libbinder_vendor.so %s", vendorBinder != nullptr ? "loaded" : "not available");

    void* handle = dlopen("libbinder_ndk.so", RTLD_NOW);
    if (handle == nullptr) {
        ALOGE("dlopen libbinder_ndk.so failed: %s", dlerror());
        return false;
    }
    api->addService = (binder_status_t(*)(AIBinder*, const char*))dlsym(
            handle, "AServiceManager_addService");
    api->checkService =
            (AIBinder * (*)(const char*))dlsym(handle, "AServiceManager_checkService");
    api->waitForDeclaredService =
            (AIBinder * (*)(const char*))dlsym(handle, "AServiceManager_waitForDeclaredService");
    api->isDeclared = (bool (*)(const char*))dlsym(handle, "AServiceManager_isDeclared");
    api->setThreadPoolMaxThreadCount =
            (void (*)(uint32_t))dlsym(handle, "ABinderProcess_setThreadPoolMaxThreadCount");
    api->joinThreadPool = (void (*)())dlsym(handle, "ABinderProcess_joinThreadPool");
    if (api->addService == nullptr || api->setThreadPoolMaxThreadCount == nullptr ||
        api->joinThreadPool == nullptr) {
        ALOGE("missing symbols in libbinder_ndk.so (addService=%p check=%p declared=%p join=%p)",
              api->addService, api->checkService, api->waitForDeclaredService,
              api->joinThreadPool);
        return false;
    }
    return true;
}

// Keeps the service registered across soft reboots: killing zygote does not
// touch servicemanager, but a `stop; start` soft reboot restarts servicemanager
// and wipes its registry — re-add our instance whenever it disappears.
void registrationWatchdog(const BinderNdkApi& ndk, const std::shared_ptr<Gnss>& gnss,
                          const std::string& instance) {
    for (;;) {
        sleep(2);
        if (ndk.checkService != nullptr) {
            AIBinder* existing = ndk.checkService(instance.c_str());
            if (existing != nullptr) {
                AIBinder_decStrong(existing);
                continue;
            }
        }
        binder_status_t st = ndk.addService(gnss->asBinder().get(), instance.c_str());
        ALOGI("watchdog: re-registered %s (status %d)", instance.c_str(), st);
    }
}

}  // namespace

int main() {
    ALOGI("USB NMEA GNSS AIDL HAL starting");

    BinderNdkApi ndk;
    if (!loadBinderNdk(&ndk)) {
        return EXIT_FAILURE;
    }

    ndk.setThreadPoolMaxThreadCount(4);

    std::shared_ptr<Gnss> gnss = ndk::SharedRefBase::make<Gnss>();
    const std::string instance = std::string(Gnss::descriptor) + "/default";

    binder_status_t status = ndk.addService(gnss->asBinder().get(), instance.c_str());
    if (status != STATUS_OK) {
        ALOGE("failed to register %s (status %d)", instance.c_str(), status);
        return EXIT_FAILURE;
    }
    ALOGI("registered %s", instance.c_str());

    // Decisive probe for the framework's declared-service lookup: the exact
    // check GnssJni performs. null => our VINTF fragment is not being merged
    // by libvintf; non-null => declaration is fine and any framework rejection
    // lies elsewhere (e.g. binder stability).
    if (ndk.waitForDeclaredService != nullptr) {
        AIBinder* declared = ndk.waitForDeclaredService(instance.c_str());
        ALOGI("waitForDeclaredService(%s) -> %s", instance.c_str(),
              declared != nullptr ? "VISIBLE (declaration merged)" : "NULL (declaration NOT seen)");
        if (declared != nullptr) AIBinder_decStrong(declared);
    } else {
        ALOGI("waitForDeclaredService symbol not dlsym-able; isDeclared=%s",
              ndk.isDeclared != nullptr ? "available" : "missing");
    }
    if (ndk.isDeclared != nullptr) {
        ALOGI("isDeclared(%s) -> %s", instance.c_str(),
              ndk.isDeclared(instance.c_str()) ? "YES" : "NO");
    }

    std::thread watchdog(registrationWatchdog, std::cref(ndk), gnss, instance);
    watchdog.detach();

    // Dedicated binder-serving threads. On some environments the max-count
    // global above doesn't take effect for non-platform callers (registration
    // logs "Thread Pool max thread count is 0"); explicit joins guarantee
    // incoming HAL transactions can always be served regardless.
    for (int i = 0; i < 3; ++i) {
        std::thread server([&ndk]() { ndk.joinThreadPool(); });
        server.detach();
    }

    ALOGI("serving binder traffic (main + 3 pool threads)");
    ndk.joinThreadPool();
    return EXIT_FAILURE;  // joinThreadPool should never return
}
