# mipad_usb_gnss

把外置 **USB GNSS 模组**（CH340/CH341 串口）变成**真实的系统级 GNSS 硬件**——注册标准 AIDL GNSS HAL（`android.hardware.gnss.IGnss/default`），系统获得真正的 **gps provider**：GPS Test 可见卫星列表与 C/N0、高德/百度/Google 地图直接卫星定位，效果与内置 GNSS 芯片一致。

专为 **小米平板7S Pro（violin，玄戒O1，HyperOS / Android 17，BL锁定 + 半持久root）** 打造，同代 HyperOS 设备可参考移植。

```
外置GNSS模组 ──CH340X──USB──┬─ usbfs 用户态驱动（默认，无需内核ch341驱动）
                            ├─ 内核串口 /dev/ttyUSB*
                            └─ tcp:// / unix: socket（蓝牙SPP桥App等）
                                    ↓
                          gnss_usb_hal（AIDL GNSS HAL V4，arm64）
                                    ↓ binder
                          system_server：gps provider + FEATURE_LOCATION_GPS
```

## 核心突破（本机实证的关键机制）

| 机制 | 说明 |
|---|---|
| **servicemanager 的 VINTF 声明缓存刷新** | 框架取 GNSS HAL 走 `waitForVintfService`→`isDeclared`，由 servicemanager 进程内 libvintf 缓存应答。libvintf 以 **`/apex/apex-info-list.xml` 的 mtime** 为缓存失效键（APEX 在线更新机制）——root 下 bind-mount 一个新 mtime 副本即可无损刷新，**无需重启 servicemanager**（实测重启它会黑屏死机） |
| **VINTF fragment 格式** | 本机 libvintf 要求 `<manifest version="8.0">`（meta版本），旧式 `1.0` 会导致整个设备清单解析失败→开机死机 |
| **特性/VINTF overlay** | `nsenter` 进 init 挂载命名空间，bind-mount 叠加 `/vendor/etc/permissions` 与 `/vendor/etc/vintf/manifest`（带完整性校验：缺文件拒绝挂载），软重启后新 system_server 读到 GPS 特性与 HAL 声明 |
| **干净软重启** | `setprop ctl.restart zygote`（实证安全）；RescueParty 每次自动清零+禁用（`persist.sys.enable_rescue_party=false` 等），防止历史崩溃计数触发恢复模式 |
| **半持久root适配** | 守护进程带注册看门狗（服务丢失2秒内补注册）、`ctl.restart` 结果校验回退；BL锁定/重启丢root环境下全程无需整机重启 |

## 安装（半持久root / 每次重启后）

```
激活root → KSU管理器安装 gnss-usb-st zip → 模块卡片点【执行操作】
→ 自动：清RescueParty → 守护启动+注册 → 特性/VINTF注入 → 缓存刷新 → zygote软重启
→ 约90秒系统回来，GNSS就绪
```

持久root设备用 `gnss-usb.zip`（开机自动完成一切，无需点按钮）。

## 配置属性（`persist.vendor.gnss_usb.*`）

| 属性 | 默认 | 说明 |
|---|---|---|
| `source` | `auto` | `usb`(usbfs用户态驱动) / `tty` / `tcp` / `unix` / `auto` |
| `vid_pid` | `1a86:7523` | CH340/CH341系 USB VID:PID |
| `baud` | `115200` | `auto` 自动探测；实测 ATGM336H/双频模组 115200 |
| `tty_dev` | 空 | tty模式显式节点名 |
| `socket_addr` | `tcp://127.0.0.1:5000` | socket源地址（配 GnssBtBridge 蓝牙SPP） |
| `max_rate` | `1000` | 定位周期ms（透传App请求，100ms~65s） |
| `cb_*` | 全开 | 回调分级开关（caps/sysinfo/loc/sv/nmea/loc_min），应急降级用 |
| `log` | `0` | `1`=记录原始NMEA到logcat（tag `gnss_usb`） |

## 双频显示与模组控制（v1.1+）

- **L1/L5 分行显示**：解析 NMEA 4.10 GSV 尾部 signalId，按频段映射载波频率（GPS L1/L2/L5、BDS B1I/B2I/B2a/B1C/B3I、Galileo E1/E5a/E5b/E6、GLONASS G1/G2、QZSS、NavIC），GPS Test 可见同星双频各行
- **守护进程控制口**：`127.0.0.1:5001`（`STATUS` / `SEND <命令>` / `SUB` 实时NMEA推送），App与adb皆可使用：
  ```
  adb shell su -c '(printf "STATUS\n"; sleep 1) | nc 127.0.0.1 5001'
  ```
- **GnssBtBridge v1.1 模组控制页**：双模组切换
  - 中科微 ATGM/CASIC：1/2/5/10Hz、星座选择、冷/热启动、版本查询、FLASH保存（自动计算异或校验和）
  - 博通 BK166X：1/5/10/20Hz（`$POLCFGNAV`）、双频/单频频点掩码（`$POLCFGSYS`）、冷/热启、配置保存（响应 `$OK/$FAIL` 实时回显）
  - 实时状态栏：传输源/会话/卫星数(在用)/fix/精度/坐标 + NMEA与指令回显日志
- **息屏供电测试**：`tools/suspend_test.sh <分钟>`（判读OTG休眠供电与fix恢复时间）

## 构建复现（Windows + Git Bash，经代理）

```bash
bash fetch_deps.sh all && bash fetch_deps.sh ndk   # NDK r27c / build-tools / AOSP V4 aidl源码
bash build_hal.sh        # dist/gnss-usb-st-*.zip（含HAL二进制）
bash build_bridge.sh     # dist/GnssBtBridge-*.apk（蓝牙SPP桥，可选，需JDK）
```

- AIDL 绑定：SDK `aidl --lang=ndk --structured --stability=vintf --version=4` 从 AOSP 冻结源生成
- NDK r27c 直接 clang++ 编译，静态 libc++，产物仅依赖系统库
- 服务注册 API（`AServiceManager_addService` 等）NDK 不导出，运行时 dlsym 设备真实 `libbinder_ndk.so`

## 踩坑记录（给后来者）

1. `stop; start` 式软重启在本机引导挂死；杀 servicemanager 直接黑屏——都不可用
2. `cp -a` 拷贝 overlay 源目录时若源正是已挂载的 overlay 自身，先 `rm` 会把线上内容洗掉（曾导致指纹/wifi特性消失、WifiP2pService 连环崩溃）
3. HyperOS 投屏开启时若 WiFi Direct 特性缺失，`WifiDisplayAdapter` 会 crash system_server（RescueParty 连环自救→恢复模式）——注入时附带自愈声明
4. RescueParty 计数持久化，历史崩溃会让后续任何重启升级自救级别；本模块所有重启入口前置清理
5. `service list` 有服务 ≠ 框架会用：`isDeclared` 的声明检查是独立的一道门

## 产物

| 文件 | 说明 |
|---|---|
| `dist/gnss-usb-st-*.zip` | KSU 模块（半持久root，按钮一键激活） |
| `dist/gnss-usb-*.zip` | KSU 模块（持久root，开机全自动） |
| `dist/GnssBtBridge-*.apk` | 蓝牙 SPP 桥接 + 模组控制 App（可选） |
| `tools/diagnose.sh` | 上机一键自检（USB/服务/provider/avc/崩溃取证） |

## 蓝牙 SPP 桥接（HyperOS 注意事项）

配对 HC-04/HC-05 类串口模块后，App 内选设备 → 连接，即建立 `TCP 127.0.0.1:5000 ↔ SPP` 管道；
HAL 侧配 `persist.vendor.gnss_usb.transport=tcp://127.0.0.1:5000` 即可把蓝牙 GNSS 当系统定位源。

HyperOS 会把息屏后的 App 整体冻结（freezeUid reason=screen_off），桥接需一次性豁免（root）：

```bash
su -c 'dumpsys deviceidle whitelist +io.github.ksugnss.btbridge'
su -c 'cmd appops set io.github.ksugnss.btbridge RUN_ANY_IN_BACKGROUND allow'
```

v2.2 起声明 `BLUETOOTH_SCAN(neverForLocation)`——Android 12+ 缺它时 `cancelDiscovery()` 抛
SecurityException 会导致 SPP 永远连不上。

## 致谢与许可

- HAL 主体移植自 [aselafernando/gnss-aidl-serial](https://github.com/aselafernando/gnss-aidl-serial)（Apache-2.0，Android 17 实证）
- CH340 用户态协议移植自 [usb-serial-for-android](https://github.com/mik3y/usb-serial-for-android)（Apache-2.0）
- AIDL 接口定义来自 AOSP `hardware/interfaces`
- 本项目代码：Apache-2.0，各文件保留上游版权声明
