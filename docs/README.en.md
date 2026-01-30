# xDRM Display Engine

A high-performance DRM (Direct Rendering Manager) display engine designed specifically for the Rockchip RK3588 platform.

Built on the Linux DRM Atomic API, xDRM provides a multi-threaded, low-latency display rendering engine. It is capable of driving multiple display interfaces (MIPI-DSI and HDMI) simultaneously while supporting advanced features such as real-time Sensor cropping simulation, dynamic layout switching, and robust HDMI hot-plug detection, ensuring stability for industrial applications.

## I. Features

* **Atomic Modesetting**: Utilizes the modern DRM Atomic API to implement atomic commits for multi-plane properties, effectively eliminating screen tearing.
* **Multi-Display Support**: Supports driving three independent display connectors simultaneously:
    * Panel (Primary Screen, DSI-1)
    * EVF (Electronic Viewfinder, DSI-2)
    * HDMI (External Monitor)
* **Dynamic Hot-Plug**: Robust HDMI connection management. Uses a state machine mechanism to automatically detect plug/unplug events, ensuring the main program remains stable and recovers automatically if HDMI is disconnected unexpectedly.
* **Sensor Simulation**: Simulates image sensor behavior under different resolution modes (e.g., switching between `1440x1080` and `1280x720`) by modifying Plane Source Crop and CRTC Layout coordinates in real-time.
* **Performance Monitoring**: Built-in real-time FPS counters and frame rate fluctuation statistics for each display pipeline.
* **Test Pattern Generation**: CPU-based real-time pattern rendering used to verify the data integrity of the display link.

## II. Build & Run

Requires a Linux environment with `libdrm` installed (included by default in the RK3588 BSP).

1. **Build the project using CMake:**

```sh
# cmake
mkdir build
cd build
cmake ..
make

# or use build.sh (includes clang-format diff output)
./build.sh
```

2. **Run the program:**

*Note: Accessing DRM devices typically requires root privileges or membership in the `video` group.*

```sh
chmod 777 xdrm
./xdrm
```

## III. Configuration

Currently, system configuration is handled via macros and constant definitions in `src/main.cpp` and `src/xdrm/xdrm.h`. You can adjust these parameters according to your specific hardware topology.

### 3.1 Connector & CRTC Mapping

Modify the connector IDs in `main.cpp` based on your Device Tree configuration (you can check this via `modetest -M rockchip`).

```c
// Example initialization in main.cpp
int fd = xDRM_Init(&panel, CONN_ID_DSI1, CRTC_ID_DSI1, PLANE_ID_DSI1, ...);
```

### 3.2 Sensor Mode Simulation

The program simulates different sensor readout modes by toggling the `large_sensor_mode` variable. You can adjust resolutions and crop offsets in the main loop:

```c
// Mode A: 1440x1080 (Center Crop)
const int MODE_A_W = 1440;
const int MODE_A_X = 240;

// Mode B: 1280x720 (Center Crop)
const int MODE_B_W = 1280;
const int MODE_B_X = 320;
```

## IV. How it Works

1. **Initialization**: The system opens `/dev/dri/card0`, initializes the `modeset_dev` structure, and allocates Dumb Buffers for direct memory access.
2. **Threading Model**:
    * **Main Loop**: Responsible for generating image patterns and pushing them to the back buffers of each screen.
    * **Display Threads**: Three independent threads (`panel_func`, `evf_func`, `hdmi_func`) manage their respective VSync cycles.
3. **Render Loop**:
    * `xDRM_Draw` enters a `poll()` loop waiting for DRM VBlank events.
    * When a VBlank occurs, the Page Flip Handler triggers to swap front/back buffers.
    * The system uses the Atomic API to commit new Framebuffer IDs and Plane properties (SRC/CRTC coordinates) in a non-blocking manner, realizing dynamic layout updates.
4. **Hot-Plug State Machine**:
    * The HDMI thread runs a polling loop (`xDRM_Check_Connection`) when in a disconnected state.
    * Upon detecting a connection, it executes the full Atomic initialization sequence.
    * If the cable is unplugged, a VBlank timeout mechanism triggers a cleanup sequence, safely releasing resources and resetting the state to "listening" without affecting the operation of other screens.