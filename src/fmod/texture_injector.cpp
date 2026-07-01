#include "fh6/fmod/texture_injector.hpp"
#include "fh6/subprocess.hpp"
#include "fh6/net/http_get.hpp"
#include "fh6/log.hpp"
#include <thread>
#include <fstream>
#include <filesystem>
#include <windows.h>

// stb headers
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "stb_image_resize2.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

namespace fh6 {

void TextureInjector::update_artwork_url(const std::string& url) {
    std::shared_ptr<worker::WorkerClient> local_worker;
    ConfigStore* local_config = nullptr;
    DependencyManager* local_deps = nullptr;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        local_worker = worker_;
        local_config = config_store_;
        local_deps = deps_;
    }
    
    is_processing_.store(true);
    
    // grab a unique ticket for this job
    uint64_t my_job_id = ++latest_job_id_;

    std::thread([this, url, my_job_id, local_worker, local_config, local_deps]() {
        // only clear the processing flag if this thread is still the newest job
        struct ProcessingGuard {
            std::atomic<bool>& flag;
            std::atomic<uint64_t>& latest;
            uint64_t mine;
            ~ProcessingGuard() { 
                if (latest.load() == mine) {
                    flag.store(false); 
                }
            }
        } guard{is_processing_, latest_job_id_, my_job_id};

        try {
            // if user already skipped the song, abort immediately
            if (latest_job_id_.load() != my_job_id) return; 

            char dll_path[MAX_PATH];
            GetModuleFileNameA(nullptr, dll_path, MAX_PATH);
            std::filesystem::path game_dir = std::filesystem::path(dll_path).parent_path();
            
            std::filesystem::path temp_dir_path = std::filesystem::temp_directory_path();
            
            std::string raw_path = (temp_dir_path / ("fh6_raw_" + std::to_string(my_job_id))).string(); 
            std::string png_path = (temp_dir_path / ("fh6_png_" + std::to_string(my_job_id) + ".png")).string();
            std::string dds_path = (temp_dir_path / ("fh6_png_" + std::to_string(my_job_id) + ".dds")).string(); 
            std::string temp_dir_str = temp_dir_path.string();

            if (!temp_dir_str.empty() && (temp_dir_str.back() == '\\' || temp_dir_str.back() == '/')) {
                temp_dir_str.pop_back();
            }

            struct FileCleanupGuard {
                std::string r, p, d;
                ~FileCleanupGuard() {
                    std::error_code ec;
                    if (!r.empty()) std::filesystem::remove(r, ec);
                    if (!p.empty()) std::filesystem::remove(p, ec);
                    if (!d.empty()) std::filesystem::remove(d, ec);
                }
            } file_guard{raw_path, png_path, dds_path};

            bool has_valid_source = false;
            bool is_default_artwork = false;

            if (!url.empty()) {
                log::info("[dx12] job {}: delegating artwork download to worker process: {}", my_job_id, url);
                
                if (local_worker) {
                    // IPC call blocks until the worker finishes downloading to raw_path
                    has_valid_source = local_worker->download_file(url, raw_path);
                    
                    if (!has_valid_source) {
                        log::warn("[dx12] job {}: worker failed to download artwork - falling back to default", my_job_id);
                    } else {
                        log::info("[dx12] job {}: worker successfully downloaded artwork", my_job_id);
                    }
                } else {
                    log::error("[dx12] job {}: WorkerClient is not attached to TextureInjector - falling back", my_job_id);
                }
            }

            if (latest_job_id_.load() != my_job_id) return;

            if (!has_valid_source) {
                std::filesystem::path default_art = game_dir / "fh6-radio" / "assets" / "default_artwork.png";
                if (std::filesystem::exists(default_art)) {
                    std::error_code ec;
                    std::filesystem::copy_file(default_art, raw_path, std::filesystem::copy_options::overwrite_existing, ec);
                    if (!ec) {
                        has_valid_source = true;
                        is_default_artwork = true;
                    } else {
                        log::warn("[dx12] job {}: failed to copy default artwork", my_job_id);
                    }
                } else {
                    log::warn("[dx12] job {}: no default artwork found at {} - skipping texture update", my_job_id, default_art.string());
                    return; 
                }
            }

            // don't saturate the CPU with FFmpeg if user skipped
            if (latest_job_id_.load() != my_job_id) return; 

             if (!local_config || !local_deps) {
                log::error("[dx12] job {}: missing ConfigStore or DependencyManager", my_job_id);
                return;
            }

            Config cfg = local_config->snapshot(); // get thread-safe config
            
            // resolve path using the DependencyManager
            std::string texconv_path = local_deps->resolve(Tool::texconv, "").string();

            if (texconv_path.empty()) {
                log::error("[dx12] job {}: missing texconv - cannot process artwork", my_job_id);
                return;
            }

            log::info("[dx12] job {}: resizing artwork natively using stb...", my_job_id);

            // load the raw image
            int width, height, channels;
            unsigned char* img_data = stbi_load(raw_path.c_str(), &width, &height, &channels, 4);
            if (!img_data) {
                log::warn("[dx12] job {}: failed to load raw image", my_job_id);
                return;
            }

            // calculate aspect-ratio preserving dimensions
            // wait until the DX12 hook discovers the target UI size
            int target_h = 0;
            const auto height_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while ((target_h = target_height_.load()) == 0) {
                if (latest_job_id_.load() != my_job_id) return;
                if (std::chrono::steady_clock::now() >= height_deadline) {
                    log::warn("[dx12] job {}: timed out waiting for target texture height", my_job_id);
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }

            if (target_h != 104 && target_h != 196 && target_h != 208 && target_h != 392) {
                log::warn("[dx12] job {}: unsupported target texture height {}", my_job_id, target_h);
                return;
            }
            
            // dynamically scale based on what height was discovered
            int target_w = (target_h == 208 || target_h == 392) ? 392 : 196;
            int square_size = (target_w == 392) ? 208 : 104;
            
            // border settings
            int border_thickness = is_default_artwork ? 0 : (square_size / 26);
            
            // calculate scale to fill the 104x104 square
            float scale = std::max((float)square_size / width, (float)square_size / height);
            int new_w = std::max(1, (int)(width * scale));
            int new_h = std::max(1, (int)(height * scale));

            // resize image using STB
            std::vector<unsigned char> resized_data(new_w * new_h * 4);
            stbir_resize_uint8_linear(img_data, width, height, 0, resized_data.data(), new_w, new_h, 0, STBIR_RGBA);
            stbi_image_free(img_data);

            // create transparent padded canvas to what DX12 requested
            std::vector<unsigned char> padded_data(target_w * target_h * 4, 0);

            // calculate source offsets to center-crop
            int src_offset_x = (new_w - square_size) / 2;
            int src_offset_y = (new_h - square_size) / 2;

            // calculate the average color of the cropped square
            long long total_r = 0, total_g = 0, total_b = 0;
            for (int y = 0; y < square_size; ++y) {
                for (int x = 0; x < square_size; ++x) {
                    int src_idx = ((y + src_offset_y) * new_w + (x + src_offset_x)) * 4;
                    total_r += resized_data[src_idx];
                    total_g += resized_data[src_idx + 1];
                    total_b += resized_data[src_idx + 2];
                }
            }
            
            int pixel_count = square_size * square_size;
            unsigned char avg_r = (unsigned char)(total_r / pixel_count);
            unsigned char avg_g = (unsigned char)(total_g / pixel_count);
            unsigned char avg_b = (unsigned char)(total_b / pixel_count);

            // border shade
            float brightness_factor = 0.6f; 
            unsigned char b_r = (unsigned char)(avg_r * brightness_factor);
            unsigned char b_g = (unsigned char)(avg_g * brightness_factor);
            unsigned char b_b = (unsigned char)(avg_b * brightness_factor);
            unsigned char b_a = 255;

            // stretches the y-axis if target_h is 196
            for (int y = 0; y < target_h; ++y) {
                for (int x = 0; x < square_size; ++x) {
                    int dst_idx = (y * target_w + x) * 4; 

                    // map the current y back to the 104-pixel source space
                    int orig_y = (y * square_size) / target_h; 

                    if (x < border_thickness || x >= square_size - border_thickness ||
                        orig_y < border_thickness || orig_y >= square_size - border_thickness) {
                        
                        padded_data[dst_idx]   = b_r;
                        padded_data[dst_idx+1] = b_g;
                        padded_data[dst_idx+2] = b_b;
                        padded_data[dst_idx+3] = b_a;
                    } else {
                        int src_idx = ((orig_y + src_offset_y) * new_w + (x + src_offset_x)) * 4;
                        padded_data[dst_idx]   = resized_data[src_idx];
                        padded_data[dst_idx+1] = resized_data[src_idx+1];
                        padded_data[dst_idx+2] = resized_data[src_idx+2];
                        padded_data[dst_idx+3] = resized_data[src_idx+3];
                    }
                }
            }

            stbi_write_png(png_path.c_str(), target_w, target_h, 4, padded_data.data(), target_w * 4);

            log::info("[dx12] job {}: compressing to BC7 with texconv...", my_job_id);

            // export to the dynamic size requested
            std::string texconv_cmd = "\"" + texconv_path + "\" -f BC7_UNORM -w " + std::to_string(target_w) + " -h " + std::to_string(target_h) + " -m 1 -pmalpha -gpu 0 -y -o \"" + temp_dir_str + "\" \"" + png_path + "\"";
            std::vector<char> cmd_buf(texconv_cmd.begin(), texconv_cmd.end());
            cmd_buf.push_back('\0');
            
            STARTUPINFOA si = { sizeof(si) };
            si.dwFlags = STARTF_USESHOWWINDOW;
            si.wShowWindow = SW_HIDE;
            PROCESS_INFORMATION pi = {};
            
            if (CreateProcessA(nullptr, cmd_buf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
                DWORD wait = WaitForSingleObject(pi.hProcess, 30000);
                DWORD exit_code = 1;
                if (wait == WAIT_TIMEOUT) {
                    fh6::subprocess::kill_process_tree(pi.dwProcessId); 
                    log::warn("[dx12] job {}: image pipeline timed out", my_job_id);
                } else {
                    GetExitCodeProcess(pi.hProcess, &exit_code);
                }
                CloseHandle(pi.hProcess);
                CloseHandle(pi.hThread);
                if (wait == WAIT_TIMEOUT || exit_code != 0) {
                    log::warn("[dx12] job {}: image pipeline failed with exit code {}", my_job_id, exit_code);
                    return;
                }
            } else {
                log::warn("[dx12] job {}: failed to launch pipeline", my_job_id);
                return;
            }

            // don't lock the mutex and push pixels if stale
            if (latest_job_id_.load() != my_job_id) return; 

            std::ifstream in_dds(dds_path, std::ios::binary | std::ios::ate);
            if (in_dds) {
                std::streamsize dds_size = in_dds.tellg();
                if (dds_size > 128) { 
                    in_dds.seekg(0, std::ios::beg);
                    std::vector<char> dds_data(dds_size);
                    if (in_dds.read(dds_data.data(), dds_size)) {
                        size_t header_size = 128;
                        uint32_t fourcc;
                        std::memcpy(&fourcc, dds_data.data() + 84, 4);
                        if (fourcc == 0x30315844) { header_size = 148; }

                        if (static_cast<size_t>(dds_size) > header_size) {
                            size_t payload_size = dds_size - header_size;
                            std::vector<uint8_t> bc7_payload(payload_size);
                            std::memcpy(bc7_payload.data(), dds_data.data() + header_size, payload_size);

                            std::lock_guard<std::mutex> lock(mtx_);
                            width_ = target_w; 
                            height_ = target_h;
                            pending_pixels_ = std::move(bc7_payload); 
                            has_new_image_ = true;
                            log::info("[dx12] job {} complete", my_job_id);
                        }
                    }
                }
                in_dds.close();
            } else {
                log::warn("[dx12] job {}: failed to read DDS", my_job_id);
            }
        } catch (const std::exception& e) {
            log::warn("[dx12] job {}: artwork pipeline failed: {}", my_job_id, e.what());
        } catch (...) {
            log::warn("[dx12] job {}: artwork pipeline failed with unknown exception", my_job_id);
        }
    }).detach();
}

bool TextureInjector::pop_pending_pixels(std::vector<uint8_t>& out_pixels, int& out_width, int& out_height) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (!has_new_image_) return false;

    out_pixels = std::move(pending_pixels_);
    out_width = width_;
    out_height = height_;
    has_new_image_ = false;
    return true;
}

} // namespace fh6