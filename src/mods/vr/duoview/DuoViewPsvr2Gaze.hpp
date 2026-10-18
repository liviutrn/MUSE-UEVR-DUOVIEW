// DuoViewPsvr2Gaze.hpp — PSVR2 Toolkit CAPI gaze reader (Windows-only).
//
// Reads the raw eye-gaze ray from the PSVR2 Toolkit's native CAPI DLL.
// This is the *unstabilized* source; the DuoViewGaze stabilizer (deadband,
// adaptive smoothing, grid quantization) sits between this reader and the
// foveal projection, because raw gaze visibly shook the foveal image.
//
// ABI: BnuuySolutions/PSVR2Toolkit projects/common/hmd2_gaze.h (328 bytes).
// The DLL is loaded dynamically and never redistributed; if it is absent,
// read() returns nullopt and the fovea falls back to fixed center.
#pragma once

#ifdef _WIN32

#include <windows.h>

#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>

#ifdef _MSC_VER
#pragma comment(lib, "Advapi32.lib")
#endif

namespace uevr::duoview::psvr2 {

enum class Bool : uint32_t { no, yes };
struct Vec2 { float x, y; };
struct Vec3 { float x, y, z; };
struct Lens { Vec3 left, right; };
enum class EnabledEye : uint8_t { left, right, both };
struct WearableEye {
    Bool origin_valid; Vec3 origin_mm;
    Bool direction_valid; Vec3 direction;
    Bool pupil_diameter_valid; float pupil_diameter_mm;
    Bool pupil_position_valid; Vec2 pupil_position;
    Bool guide_valid; Vec2 guide;
    Bool blink_valid; Bool blink;
};
struct Wearable {
    int64_t timestamp; uint32_t frame_counter;
    WearableEye left, right;
    Bool combined_origin_valid; Vec3 combined_origin;
    Bool combined_direction_valid; Vec3 combined_direction;
    Bool convergence_distance_valid;
};
struct Foveated {
    int64_t timestamp; uint32_t frame_counter; uint32_t tracking_state;
    Vec3 left, right, combined; float convergence_distance_mm;
};
struct Status {
    uint8_t magic[2]; uint16_t version; uint32_t size;
    float exposure_l, exposure_r;
    uint32_t led_status, exposure_counter_l, exposure_counter_r, led_counter;
    int32_t dsp_result; Lens lens; uint32_t calibration_id; Vec3 gaze_origin;
    EnabledEye eye; uint8_t motor_sequence, motor_strength;
    Wearable raw; Foveated tracked;
};
static_assert(sizeof(Status) == 0x148, "PSVR2 Toolkit CAPI gaze ABI mismatch");

using InitFn = int(__cdecl*)();
using ReadFn = bool(__cdecl*)(Status*, uint32_t);

// Gaze ray in HMD-local space, X-right / Y-up / Z-forward.
// (Toolkit native basis is X-left; we flip X on the way out.)
struct Ray { float x, y, z; };

// Validate one status packet and extract the combined gaze ray.
// Rejects: bad magic/size, invalid direction, blinks, degenerate vectors.
inline std::optional<Ray> calibrated_ray(const Status& s) {
    if (s.size != sizeof(Status) || s.magic[0] != 'G' || s.magic[1] != 'S' ||
        s.raw.combined_direction_valid != Bool::yes ||
        (s.raw.left.blink_valid == Bool::yes && s.raw.left.blink == Bool::yes) ||
        (s.raw.right.blink_valid == Bool::yes && s.raw.right.blink == Bool::yes)) {
        return std::nullopt;
    }
    const auto& d = s.raw.combined_direction;
    const float norm = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
    if (!std::isfinite(norm) || norm < 0.5f || norm > 1.5f || d.z < 0.10f) {
        return std::nullopt;
    }
    return Ray{-d.x / norm, d.y / norm, d.z / norm};
}

class Reader {
public:
    Reader() = default;
    Reader(const Reader&) = delete;
    Reader& operator=(const Reader&) = delete;

    // Returns the latest gaze ray, or nullopt when unavailable/invalid.
    // Thread-safe; may be called from the game thread once per frame.
    std::optional<Ray> read() {
        std::scoped_lock lock{mutex_};
        const uint64_t now = GetTickCount64();
        if (!read_fn_ && now >= next_attempt_) {
            initialize();
            next_attempt_ = now + 1000; // retry at most once per second
        }
        if (!read_fn_) {
            ++unavailable_;
            return std::nullopt;
        }
        Status candidate{};
        if (!read_fn_(&candidate, 0)) {
            ++unavailable_;
            return std::nullopt;
        }
        const auto ray = calibrated_ray(candidate);
        if (ray) { ++fresh_; } else { ++invalid_; }
        return ray;
    }

    struct Counters { uint64_t fresh, invalid, unavailable; };
    Counters counters() const {
        return {fresh_.load(std::memory_order_relaxed),
                invalid_.load(std::memory_order_relaxed),
                unavailable_.load(std::memory_order_relaxed)};
    }

private:
    void initialize() {
        if (!module_) {
            module_ = GetModuleHandleW(L"psvr2_toolkit_capi.dll");
            wchar_t path[32768]{};
            const DWORD n = GetEnvironmentVariableW(L"PSVR2TK_CAPI_PATH", path, 32768);
            if (!module_ && n && n < 32768) {
                module_ = LoadLibraryExW(path, nullptr,
                    LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
            }
            if (!module_) try {
                std::ifstream file(std::filesystem::temp_directory_path() / "psvr2tk_capi_path.txt");
                std::string dir;
                if (file && std::getline(file, dir) && !dir.empty()) {
                    const auto f = std::filesystem::path(dir) / "psvr2_toolkit_capi.dll";
                    module_ = LoadLibraryExW(f.c_str(), nullptr,
                        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
                }
            } catch (...) {}
            if (!module_) {
                wchar_t steam[32768]{}; DWORD bytes = sizeof(steam);
                if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath",
                        RRF_RT_REG_SZ, nullptr, steam, &bytes) == ERROR_SUCCESS) {
                    const std::wstring f = std::wstring(steam) +
                        L"\\steamapps\\common\\PlayStation VR2 App\\SteamVR_Plug-In\\bin\\win64\\psvr2_toolkit_capi.dll";
                    module_ = LoadLibraryExW(f.c_str(), nullptr,
                        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
                }
            }
            if (!module_) {
                module_ = LoadLibraryW(L"psvr2_toolkit_capi.dll");
            }
        }
        if (!module_) {
            return;
        }
        const auto init = (InitFn)GetProcAddress(module_, "psvr2_toolkit_init");
        read_fn_ = (ReadFn)GetProcAddress(module_, "psvr2_toolkit_gaze_status");
        if (init && read_fn_) {
            init();
        } else {
            read_fn_ = nullptr;
        }
    }

    std::mutex mutex_;
    HMODULE module_{};
    ReadFn read_fn_{};
    uint64_t next_attempt_{};
    std::atomic<uint64_t> fresh_{}, invalid_{}, unavailable_{};
};

} // namespace uevr::duoview::psvr2

#endif // _WIN32
