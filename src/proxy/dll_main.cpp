// version.dll proxy: forwards every export to the real system DLL via PE
// forwarders, and spawns the bridge on DLL_PROCESS_ATTACH so the loader is
// never blocked on FMOD discovery or HTTP startup.
//
// MSVC's .def parser rejects absolute paths in forwarder targets (the `:` /
// `\` break parsing), so the pragmas below carry the forwarders for MSVC and
// version.def carries them for MinGW (added conditionally in CMakeLists.txt).

#include <windows.h>
#include <thread>
#include <vector>
#include <algorithm>
#include <mutex>
#include <chrono>
#include <wrl/client.h>
#include "fh6/fmod/texture_injector.hpp"

#include "kiero.h"
#include "MinHook.h"
#include <d3d12.h>
#include <dxgi1_4.h>
#include "fh6/log.hpp"

void InitDX12HookThread();

uint64_t EXPECTED_STREAMER_HASH = 0x69F6C569FDFE6B09ULL; // hash for streamer mode logo

struct TrackedTexture {
    Microsoft::WRL::ComPtr<ID3D12Resource> ptr;
    std::chrono::steady_clock::time_point discovered;
};

std::mutex g_TextureMutex;
std::vector<TrackedTexture> g_UnverifiedResources;
std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> g_StreamerModeResources;

uint64_t CalculateFNV1a(const uint8_t* data, size_t length) {
    uint64_t hash = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < length; ++i) {
        hash ^= data[i];
        hash *= 0x100000001b3ull;
    }
    return hash;
}

typedef void(__stdcall* ExecuteCommandLists_t)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
ExecuteCommandLists_t OriginalExecuteCommandLists = nullptr;

// ==============================================================================
// SEH ISOLATION
// ==============================================================================
bool IsTargetTexture(ID3D12Resource* pResource) {
    if (!pResource) return false;
    __try {
        D3D12_RESOURCE_DESC desc = pResource->GetDesc();
        return (desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && 
                desc.Width == 196 && desc.Height == 104 &&
                (desc.Format == DXGI_FORMAT_BC7_UNORM || desc.Format == DXGI_FORMAT_BC7_UNORM_SRGB));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false; 
    }
}

void ProcessFingerprintResult(uint64_t hash, ID3D12Resource* pRes) {
    fh6::log::info("[dx12] fingerprint checksum: 0x{:X}", hash);
    
    if (EXPECTED_STREAMER_HASH == 0 || hash == EXPECTED_STREAMER_HASH) {
        std::lock_guard<std::mutex> lock(g_TextureMutex);
        g_StreamerModeResources.emplace_back(pRes);
        fh6::log::info("[dx12] ---> added to streamer mode array");
    }
}

__declspec(noinline) bool SafeExecuteFingerprint(
    ID3D12Device* pDevice,
    ID3D12CommandQueue* pQueue,
    ID3D12GraphicsCommandList* pRbCmdList,
    ID3D12CommandAllocator* pRbAllocator,
    ID3D12Resource* pReadbackRes,
    UINT alignedRowPitch,
    UINT64 readbackSize,
    ID3D12Resource* pRes)
{
    __try {
        if (!pRes) return false;
        D3D12_RESOURCE_DESC desc = pRes->GetDesc(); // triggers SEH if pointer is dead
        if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) return false;

        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = pRes;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        pRbCmdList->ResourceBarrier(1, &barrier);

        D3D12_TEXTURE_COPY_LOCATION srcLoc = { pRes, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, 0 };
        D3D12_TEXTURE_COPY_LOCATION dstLoc = { pReadbackRes, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {} };
        dstLoc.PlacedFootprint.Footprint.Format = DXGI_FORMAT_BC7_UNORM;
        dstLoc.PlacedFootprint.Footprint.Width = 196; dstLoc.PlacedFootprint.Footprint.Height = 104;
        dstLoc.PlacedFootprint.Footprint.Depth = 1; dstLoc.PlacedFootprint.Footprint.RowPitch = alignedRowPitch;

        pRbCmdList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);

        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
        pRbCmdList->ResourceBarrier(1, &barrier);
        pRbCmdList->Close();

        ID3D12CommandList* ppLists[] = { pRbCmdList };
        OriginalExecuteCommandLists(pQueue, 1, ppLists);

        ID3D12Fence* pFence = nullptr;
        if (SUCCEEDED(pDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), (void**)&pFence))) {
            HANDLE event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
            if (!event) {
                pFence->Release();
                return false;
            }
            HRESULT signalHr = pQueue->Signal(pFence, 1);
            HRESULT eventHr = SUCCEEDED(signalHr) ? pFence->SetEventOnCompletion(1, event) : signalHr;
            DWORD waitResult = SUCCEEDED(eventHr) ? WaitForSingleObject(event, 5000) : WAIT_FAILED;
            CloseHandle(event);
            if (FAILED(signalHr) || FAILED(eventHr) || waitResult != WAIT_OBJECT_0) {
                pFence->Release();
                return false;
            }
            pFence->Release();
        }

        pRbAllocator->Reset();
        pRbCmdList->Reset(pRbAllocator, nullptr);

        uint8_t* pMapped = nullptr;
        if (SUCCEEDED(pReadbackRes->Map(0, nullptr, (void**)&pMapped))) {
            uint64_t hash = CalculateFNV1a(pMapped, readbackSize);
            pReadbackRes->Unmap(0, nullptr);

            ProcessFingerprintResult(hash, pRes);
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        pRbCmdList->Close();
        pRbAllocator->Reset();
        pRbCmdList->Reset(pRbAllocator, nullptr);
        return false;
    }
}

__declspec(noinline) bool SafeExecuteInjection(
    ID3D12Device* pDevice,
    ID3D12CommandQueue* pQueue,
    ID3D12GraphicsCommandList* pCmdList,
    const D3D12_TEXTURE_COPY_LOCATION* pSrcLoc,
    int* pModifications,
    ID3D12Resource** pResources,
    size_t numResources)
{
    __try {
        for (size_t i = 0; i < numResources; i++) {
            ID3D12Resource* pDestRes = pResources[i];
            
            if (!pDestRes) continue; 
            D3D12_RESOURCE_DESC desc = pDestRes->GetDesc();
            if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) continue;

            D3D12_TEXTURE_COPY_LOCATION destLoc = {};
            destLoc.pResource = pDestRes;
            destLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            destLoc.SubresourceIndex = 0;

            D3D12_RESOURCE_BARRIER barrier = {};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = pDestRes;
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            pCmdList->ResourceBarrier(1, &barrier);

            pCmdList->CopyTextureRegion(&destLoc, 0, 0, 0, pSrcLoc, nullptr);

            std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
            pCmdList->ResourceBarrier(1, &barrier);
            
            (*pModifications)++;
        }

        pCmdList->Close();
        
        ID3D12CommandList* ppMyCommandLists[] = { pCmdList };
        OriginalExecuteCommandLists(pQueue, 1, ppMyCommandLists);
        
        ID3D12Fence* pFence = nullptr;
        if (SUCCEEDED(pDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), (void**)&pFence))) {
            HANDLE event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
            if (!event) {
                pFence->Release();
                return false;
            }
            HRESULT signalHr = pQueue->Signal(pFence, 1);
            HRESULT eventHr = SUCCEEDED(signalHr) ? pFence->SetEventOnCompletion(1, event) : signalHr;
            DWORD waitResult = SUCCEEDED(eventHr) ? WaitForSingleObject(event, 5000) : WAIT_FAILED;
            CloseHandle(event);
            if (FAILED(signalHr) || FAILED(eventHr) || waitResult != WAIT_OBJECT_0) {
                pFence->Release();
                return false;
            }
            pFence->Release();
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ==============================================================================
// CREATE SHADER RESOURCE VIEW HOOK
// ==============================================================================
typedef void(__stdcall* CreateShaderResourceView_t)(ID3D12Device*, ID3D12Resource*, const D3D12_SHADER_RESOURCE_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
CreateShaderResourceView_t OriginalCreateSRV = nullptr;

void __stdcall HookedCreateSRV(ID3D12Device* pDevice, ID3D12Resource* pResource, const D3D12_SHADER_RESOURCE_VIEW_DESC* pDesc, D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) 
{
    if (IsTargetTexture(pResource)) {
        static auto last_time = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        
        std::lock_guard<std::mutex> lock(g_TextureMutex);
        
        if (std::chrono::duration_cast<std::chrono::seconds>(now - last_time).count() > 1) {
            g_UnverifiedResources.clear();
            g_StreamerModeResources.clear();
            fh6::log::info("[dx12] cleared old texture pointers (UI reloaded)");
        }
        last_time = now;

        bool found = false;
        for(const auto& t : g_UnverifiedResources) {
            if(t.ptr.Get() == pResource) { found = true; break; }
        }
        
        auto it = std::find_if(g_StreamerModeResources.begin(), g_StreamerModeResources.end(), 
            [&](const Microsoft::WRL::ComPtr<ID3D12Resource>& p) { return p.Get() == pResource; });
            
        if (!found && it == g_StreamerModeResources.end()) {
            g_UnverifiedResources.push_back({pResource, now});
            fh6::log::info("[dx12] found a 196x104 texture @ {}", (void*)pResource);
        }
    }
    OriginalCreateSRV(pDevice, pResource, pDesc, DestDescriptor);
}

// ==============================================================================
// EXECUTE COMMAND LISTS HOOK - navigates SwapChain issues
// ==============================================================================
void __stdcall HookedExecuteCommandLists(ID3D12CommandQueue* pQueue, UINT NumCommandLists, ID3D12CommandList* const* ppCommandLists) {
    
    // only perform injection operations on the DIRECT render queue
    if (pQueue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT) {
        OriginalExecuteCommandLists(pQueue, NumCommandLists, ppCommandLists);
        return;
    }

    ID3D12Device* pDevice = nullptr;
    if (SUCCEEDED(pQueue->GetDevice(__uuidof(ID3D12Device), (void**)&pDevice))) {
        
        // scan for textures
        std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> ready_to_fingerprint;
        {
            auto now = std::chrono::steady_clock::now();
            std::lock_guard<std::mutex> lock(g_TextureMutex);
            for (auto it = g_UnverifiedResources.begin(); it != g_UnverifiedResources.end(); ) {
                if (std::chrono::duration_cast<std::chrono::seconds>(now - it->discovered).count() >= 2) {
                    ready_to_fingerprint.push_back(it->ptr);
                    it = g_UnverifiedResources.erase(it);
                } else {
                    ++it;
                }
            }
        }

        // ==========================================================================
        // 2. GPU READBACK FINGERPRINTING
        // ==========================================================================
        if (!ready_to_fingerprint.empty()) {
            fh6::log::info("[dx12] fingerprinting {} textures...", ready_to_fingerprint.size());
            
            UINT blocksX = (196 + 3) / 4;  
            UINT blocksY = (104 + 3) / 4;  
            UINT alignedRowPitch = ((blocksX * 16) + 255) & ~255; 
            UINT64 readbackSize = alignedRowPitch * blocksY;

            D3D12_HEAP_PROPERTIES rbHeap = { D3D12_HEAP_TYPE_READBACK };
            D3D12_RESOURCE_DESC rbDesc = {};
            rbDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            rbDesc.Width = readbackSize;
            rbDesc.Height = 1; rbDesc.DepthOrArraySize = 1; rbDesc.MipLevels = 1;
            rbDesc.SampleDesc.Count = 1; rbDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            
            ID3D12Resource* pReadbackRes = nullptr;
            if (SUCCEEDED(pDevice->CreateCommittedResource(&rbHeap, D3D12_HEAP_FLAG_NONE, &rbDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, __uuidof(ID3D12Resource), (void**)&pReadbackRes))) {
                
                ID3D12CommandAllocator* pRbAllocator = nullptr;
                ID3D12GraphicsCommandList* pRbCmdList = nullptr;
                
                if (SUCCEEDED(pDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), (void**)&pRbAllocator)) &&
                    SUCCEEDED(pDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, pRbAllocator, nullptr, __uuidof(ID3D12GraphicsCommandList), (void**)&pRbCmdList))) {

                    for (const auto& pRes : ready_to_fingerprint) {
                        SafeExecuteFingerprint(pDevice, pQueue, pRbCmdList, pRbAllocator, pReadbackRes, alignedRowPitch, readbackSize, pRes.Get());
                    }
                    pRbCmdList->Release(); 
                    pRbAllocator->Release();
                }
                pReadbackRes->Release();
            }
        }

        // ==========================================================================
        // 3. INJECTION
        // ==========================================================================
        std::vector<ID3D12Resource*> streamer_copy;
        {
            std::lock_guard<std::mutex> lock(g_TextureMutex);
            streamer_copy.reserve(g_StreamerModeResources.size());
            for (const auto& res : g_StreamerModeResources) {
                streamer_copy.push_back(res.Get());
            }
        }

        if (!streamer_copy.empty()) {
            std::vector<uint8_t> new_pixels;
            int w, h;
            
            if (fh6::TextureInjector::instance().pop_pending_pixels(new_pixels, w, h)) {
                fh6::log::info("[dx12] intercepted ExecuteCommandLists - processing GPU upload...");
                
                UINT blocksX = (w + 3) / 4;  
                UINT blocksY = (h + 3) / 4;  
                UINT tightRowPitch = blocksX * 16; 
                UINT alignedRowPitch = (tightRowPitch + 255) & ~255; 
                UINT64 uploadBufferSize = alignedRowPitch * blocksY;

                const size_t requiredPayloadSize = static_cast<size_t>(tightRowPitch) * blocksY;
                if (w != 196 || h != 104 || new_pixels.size() < requiredPayloadSize) {
                    fh6::log::warn("[dx12] invalid BC7 payload: {}x{}, {} bytes", w, h, new_pixels.size());
                    pDevice->Release();
                    OriginalExecuteCommandLists(pQueue, NumCommandLists, ppCommandLists);
                    return;
                }

                D3D12_HEAP_PROPERTIES uploadHeap = { D3D12_HEAP_TYPE_UPLOAD };
                D3D12_RESOURCE_DESC uploadDesc = {};
                uploadDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                uploadDesc.Width = uploadBufferSize;
                uploadDesc.Height = 1;
                uploadDesc.DepthOrArraySize = 1;
                uploadDesc.MipLevels = 1;
                uploadDesc.Format = DXGI_FORMAT_UNKNOWN;
                uploadDesc.SampleDesc.Count = 1;
                uploadDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                
                ID3D12Resource* pUploadResource = nullptr;
                if (SUCCEEDED(pDevice->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &uploadDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, __uuidof(ID3D12Resource), (void**)&pUploadResource))) {
                    
                    uint8_t* pMapped = nullptr;
                    if (SUCCEEDED(pUploadResource->Map(0, nullptr, (void**)&pMapped))) {
                        for (UINT y = 0; y < blocksY; ++y) {
                            memcpy(pMapped + (y * alignedRowPitch), new_pixels.data() + (y * tightRowPitch), tightRowPitch);
                        }
                        pUploadResource->Unmap(0, nullptr);
                    }

                    ID3D12CommandAllocator* pAllocator = nullptr;
                    ID3D12GraphicsCommandList* pCmdList = nullptr;
                    HRESULT allocHr = pDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), (void**)&pAllocator);
                    HRESULT listHr = SUCCEEDED(allocHr)
                        ? pDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, pAllocator, nullptr, __uuidof(ID3D12GraphicsCommandList), (void**)&pCmdList)
                        : allocHr;
                    if (FAILED(allocHr) || FAILED(listHr) || !pAllocator || !pCmdList) {
                        if (pCmdList) pCmdList->Release();
                        if (pAllocator) pAllocator->Release();
                        pUploadResource->Release();
                        pDevice->Release();
                        OriginalExecuteCommandLists(pQueue, NumCommandLists, ppCommandLists);
                        return;
                    }

                    D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
                    srcLoc.pResource = pUploadResource;
                    srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                    srcLoc.PlacedFootprint.Footprint.Format = DXGI_FORMAT_BC7_UNORM;
                    srcLoc.PlacedFootprint.Footprint.Width = w;
                    srcLoc.PlacedFootprint.Footprint.Height = h;
                    srcLoc.PlacedFootprint.Footprint.Depth = 1;
                    srcLoc.PlacedFootprint.Footprint.RowPitch = alignedRowPitch;

                    int modifications = 0;

                    // pass the vector data into the SEH wrapper
                    if (!SafeExecuteInjection(pDevice, pQueue, pCmdList, &srcLoc, &modifications, streamer_copy.data(), streamer_copy.size())) {
                        fh6::log::warn("[dx12] intercepted crash during injection loop - texture was likely destroyed by the engine");
                    }

                    pUploadResource->Release();
                    pCmdList->Release();
                    pAllocator->Release();
                    
                    fh6::log::info("[dx12] BC7 mass overwrite complete on {} live resources", modifications);
                }
            }
        }
        pDevice->Release();
    }
    
    // call original game render logic to prevent crashes
    OriginalExecuteCommandLists(pQueue, NumCommandLists, ppCommandLists);
}

// ==============================================================================
// INITIALIZATION
// ==============================================================================

extern "C" __declspec(dllexport) void InitializeDX12Hook() {
    std::thread(InitDX12HookThread).detach();
}

void InitDX12HookThread() {
    for (int i = 0; i < 500; ++i) {
        if (kiero::init(kiero::RenderType::D3D12) == kiero::Status::Success) {
            auto srvStatus = kiero::bind(18, (void**)&OriginalCreateSRV, (void*)HookedCreateSRV);
            auto executeStatus = kiero::bind(54, (void**)&OriginalExecuteCommandLists, (void*)HookedExecuteCommandLists);
            if (srvStatus != kiero::Status::Success || executeStatus != kiero::Status::Success ||
                !OriginalCreateSRV || !OriginalExecuteCommandLists) {
                fh6::log::warn("[dx12] hook binding failed: srv={}, execute={}", (int)srvStatus, (int)executeStatus);
                kiero::shutdown();
                continue;
            }
            
            fh6::log::info("[dx12] hooks initialized");
            return; 
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

#ifdef _MSC_VER
#define FWD(name) \
    __pragma(comment(linker, "/EXPORT:" #name "=C:\\Windows\\System32\\version." #name))

FWD(GetFileVersionInfoA)
FWD(GetFileVersionInfoByHandle)
FWD(GetFileVersionInfoExA)
FWD(GetFileVersionInfoExW)
FWD(GetFileVersionInfoSizeA)
FWD(GetFileVersionInfoSizeExA)
FWD(GetFileVersionInfoSizeExW)
FWD(GetFileVersionInfoSizeW)
FWD(GetFileVersionInfoW)
FWD(VerFindFileA)
FWD(VerFindFileW)
FWD(VerInstallFileA)
FWD(VerInstallFileW)
FWD(VerLanguageNameA)
FWD(VerLanguageNameW)
FWD(VerQueryValueA)
FWD(VerQueryValueW)

#undef FWD
#endif

namespace fh6 {
void run_bridge(HMODULE self) noexcept;
} // namespace fh6

extern "C" __declspec(dllexport) void InitializeDX12Hook();

namespace {
DWORD WINAPI bridge_thread(LPVOID self) {
    InitializeDX12Hook(); 
    fh6::run_bridge(static_cast<HMODULE>(self));
    return 0;
}
} // namespace

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) noexcept {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        if (HANDLE t = CreateThread(nullptr, 0, bridge_thread, hModule, 0, nullptr)) CloseHandle(t);
    }
    return TRUE;
}