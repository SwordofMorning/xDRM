[![English](https://img.shields.io/badge/English-README-blue)](docs/README.en.md)

# xDRM Display Engine

一个专为 Rockchip RK3588 平台设计的高性能 DRM (Direct Rendering Manager) 显示工程。

xDRM 基于 Linux DRM Atomic API 构建，提供了一个多线程、低延迟的显示渲染引擎。它能够同时驱动多个显示接口(MIPI-DSI 和 HDMI)，并支持实时 Sensor 裁剪模拟、动态布局切换以及 HDMI 热插拔检测等高级功能，确保工业级应用的稳定性。

## 一、功能特性

* **Atomic Modesetting**: 采用现代 DRM Atomic API，实现多平面属性的原子性提交，杜绝画面撕裂。
* **多屏异显/同显**: 支持同时驱动三个独立的显示连接器：
    * Panel (主屏, DSI-1)
    * EVF (电子取景器, DSI-2)
    * HDMI (外部监视器)
* **动态热插拔**: 稳健的 HDMI 连接管理。采用状态机机制自动检测插入/拔出事件，确保在 HDMI 意外断开时主程序不崩溃，并能自动恢复。
* **Sensor 模拟**: 通过实时修改 Plane 的源坐标 (Source Crop) 和显示坐标 (CRTC Layout)，模拟图像传感器在不同分辨率模式下的视野裁剪行为(如 `1440x1080` 与 `1280x720` 的切换)。
* **性能监控**: 内置各显示通路的实时 FPS 计数器和帧率波动统计。
* **测试图样生成**: 基于 CPU 的实时 Pattern 渲染，用于验证显示链路的数据完整性。

## 二、构建与运行

需要安装了 `libdrm` 的 Linux 环境(RK3588 BSP 默认包含)。

1. 使用 CMake 构建工程:

```sh
# cmake
mkdir build
cd build
cmake ..
make

# or build.sh (包含clang-format输出diff文件)
./build.sh
```

2. 运行程序:

注意: 访问 DRM 设备通常需要 root 权限或 `video` 组权限。

```sh
chmod 777 xdrm
```

## 三、配置说明

目前，系统的配置通过 `src/main.cpp` 和 `src/xdrm/xdrm.h` 中的宏与常量定义。您可以根据具体的硬件拓扑修改这些参数。

### 3.1 连接器与 CRTC 映射

根据您的设备树 (Device Tree) 配置，在 `main.cpp` 中修改连接器 ID (通过 `modetest -M rockchip` 查看)。

```c
// main.cpp 中的初始化示例
int fd = xDRM_Init(&panel, CONN_ID_DSI1, CRTC_ID_DSI1, PLANE_ID_DSI1, ...);
```

### 3.2 Sensor 模式模拟

程序通过切换 `large_sensor_mode` 变量来模拟 Sensor 的不同读出模式。您可以在主循环中调整分辨率和裁剪偏移量：

```c
// Mode A: 1440x1080 (中心裁剪)
const int MODE_A_W = 1440;
const int MODE_A_X = 240;

// Mode B: 1280x720 (中心裁剪)
const int MODE_B_W = 1280;
const int MODE_B_X = 320;
```

## 四、工作原理

1. **初始化**: 系统打开 `/dev/dri/card0` 并初始化 `modeset_dev` 结构体，分配 Dumb Buffer 用于直接内存访问。
2. **线程模型**:
    * **主循环**: 负责生成图像 Pattern 并推送到各屏幕的后备缓冲区。
    * **显示线程**: 三个独立线程 (`panel_func`, `evf_func`, `hdmi_func`) 分别管理各自的 VSync 周期。
3. **渲染循环**:
    * `xDRM_Draw` 进入 `poll()` 循环等待 DRM VBlank 事件。
    * VBlank 到来时，触发 Page Flip Handler 交换前/后缓冲区。
    * 使用 Atomic API 以非阻塞方式提交新的 Framebuffer ID 和 Plane 属性(SRC/CRTC 坐标)，实现动态布局更新。
4. **热插拔状态机**:
    * HDMI 线程在未连接状态下运行轮询循环 (`xDRM_Check_Connection`)。
    * 检测到连接后，执行完整的 Atomic 初始化序列。
    * 如果线缆被拔出，VBlank 超时机制会触发清理序列，安全释放资源并将状态重置为“监听中”，且不影响其他屏幕的运行。