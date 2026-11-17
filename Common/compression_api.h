#pragma once

#include <windows.h>

#include "lzms_decode.h"

namespace CompressionApi {
namespace Detail {

// On Wine / Winlator the cabinet.dll compression API is a stub: CreateDecompressor
// reports success without creating anything and Decompress just memcpy's the input.
// So decompression always keeps a built-in decoder as a fallback. Define
// NEPGEAR_FORCE_NATIVE_LZMS=1 to force it, which is how it gets tested on Windows.
// (This header is also compiled by Packer/Unpacker with the ANSI code page, so all
// comments here must stay ASCII.)
inline bool ForceNativeLzms() {
    static const bool forced = [] {
        wchar_t value[8] = {};
        const DWORD length = GetEnvironmentVariableW(
            L"NEPGEAR_FORCE_NATIVE_LZMS", value, 8);
        return length > 0 && (value[0] == L'1' || value[0] == L'y' ||
                              value[0] == L'Y' || value[0] == L't');
    }();
    return forced;
}

constexpr DWORD kAlgorithmLzms = 5;
constexpr DWORD kInformationClassBlockSize = 1;

using CompressionHandle = PVOID;
using CreateCompressorFn = BOOL(WINAPI*)(DWORD, PVOID, CompressionHandle*);
using SetCompressorInformationFn = BOOL(WINAPI*)(
    CompressionHandle, DWORD, LPCVOID, SIZE_T);
using CompressFn = BOOL(WINAPI*)(
    CompressionHandle, LPCVOID, SIZE_T, PVOID, SIZE_T, PSIZE_T);
using CloseCompressorFn = BOOL(WINAPI*)(CompressionHandle);

using CreateDecompressorFn = BOOL(WINAPI*)(DWORD, PVOID, CompressionHandle*);
using DecompressFn = BOOL(WINAPI*)(
    CompressionHandle, LPCVOID, SIZE_T, PVOID, SIZE_T, PSIZE_T);
using CloseDecompressorFn = BOOL(WINAPI*)(CompressionHandle);

template <typename Function>
inline Function Resolve(HMODULE module, const char* name) {
    return reinterpret_cast<Function>(GetProcAddress(module, name));
}

} // namespace Detail

class LzmsCompressor {
public:
    LzmsCompressor() = default;
    ~LzmsCompressor() { Close(); }

    LzmsCompressor(const LzmsCompressor&) = delete;
    LzmsCompressor& operator=(const LzmsCompressor&) = delete;

    bool IsSupported() { return EnsureOpen(); }

    bool SetBlockSize(DWORD blockSize) {
        if (!EnsureOpen()) return false;
        return setInformation_(handle_, Detail::kInformationClassBlockSize,
            &blockSize, sizeof(blockSize)) != FALSE;
    }

    bool Compress(
        const void* input, size_t inputSize,
        void* output, size_t outputSize, size_t* compressedSize) {
        if (!compressedSize) {
            SetLastError(ERROR_INVALID_PARAMETER);
            return false;
        }
        *compressedSize = 0;
        if (!EnsureOpen()) return false;
        SIZE_T actualSize = 0;
        const BOOL result = compress_(handle_, input, inputSize, output,
            outputSize, &actualSize);
        *compressedSize = static_cast<size_t>(actualSize);
        return result != FALSE;
    }

    void Close() {
        const DWORD lastError = GetLastError();
        if (handle_ && close_) close_(handle_);
        handle_ = nullptr;
        if (module_) FreeLibrary(module_);
        module_ = nullptr;
        create_ = nullptr;
        setInformation_ = nullptr;
        compress_ = nullptr;
        close_ = nullptr;
        SetLastError(lastError);
    }

private:
    bool LoadApi() {
        if (module_) return true;

        HMODULE module = LoadLibraryW(L"cabinet.dll");
        if (!module) return false;

        const auto create = Detail::Resolve<Detail::CreateCompressorFn>(
            module, "CreateCompressor");
        const auto setInformation =
            Detail::Resolve<Detail::SetCompressorInformationFn>(
                module, "SetCompressorInformation");
        const auto compress = Detail::Resolve<Detail::CompressFn>(
            module, "Compress");
        const auto close = Detail::Resolve<Detail::CloseCompressorFn>(
            module, "CloseCompressor");
        if (!create || !setInformation || !compress || !close) {
            FreeLibrary(module);
            SetLastError(ERROR_PROC_NOT_FOUND);
            return false;
        }

        module_ = module;
        create_ = create;
        setInformation_ = setInformation;
        compress_ = compress;
        close_ = close;
        return true;
    }

    bool EnsureOpen() {
        if (handle_) return true;
        if (!LoadApi()) return false;
        return create_(Detail::kAlgorithmLzms, nullptr, &handle_) != FALSE;
    }

    HMODULE module_ = nullptr;
    Detail::CompressionHandle handle_ = nullptr;
    Detail::CreateCompressorFn create_ = nullptr;
    Detail::SetCompressorInformationFn setInformation_ = nullptr;
    Detail::CompressFn compress_ = nullptr;
    Detail::CloseCompressorFn close_ = nullptr;
};

class LzmsDecompressor {
public:
    LzmsDecompressor() = default;
    ~LzmsDecompressor() { Close(); }

    LzmsDecompressor(const LzmsDecompressor&) = delete;
    LzmsDecompressor& operator=(const LzmsDecompressor&) = delete;

    bool IsSupported() { return EnsureOpen(); }

    bool Decompress(
        const void* input, size_t inputSize,
        void* output, size_t outputSize, size_t* decompressedSize) {
        if (decompressedSize) *decompressedSize = 0;

        // Wine's stub memcpy's the whole input into the output buffer, so only let it
        // near data that cannot overflow it.
        if (!Detail::ForceNativeLzms() && inputSize <= outputSize && EnsureOpen()) {
            SIZE_T actualSize = 0;
            const BOOL result = decompress_(handle_, input, inputSize, output,
                outputSize, &actualSize);
            if (result && actualSize == outputSize) {
                if (decompressedSize) *decompressedSize = static_cast<size_t>(actualSize);
                return true;
            }
        }

        // The system API is unavailable or produced the wrong size: use the built-in decoder.
        if (LzmsNative::Decompress(input, inputSize, output, outputSize)) {
            if (decompressedSize) *decompressedSize = outputSize;
            return true;
        }
        return false;
    }

    void Close() {
        const DWORD lastError = GetLastError();
        if (handle_ && close_) close_(handle_);
        handle_ = nullptr;
        if (module_) FreeLibrary(module_);
        module_ = nullptr;
        create_ = nullptr;
        decompress_ = nullptr;
        close_ = nullptr;
        SetLastError(lastError);
    }

private:
    bool LoadApi() {
        if (module_) return true;

        HMODULE module = LoadLibraryW(L"cabinet.dll");
        if (!module) return false;

        const auto create = Detail::Resolve<Detail::CreateDecompressorFn>(
            module, "CreateDecompressor");
        const auto decompress = Detail::Resolve<Detail::DecompressFn>(
            module, "Decompress");
        const auto close = Detail::Resolve<Detail::CloseDecompressorFn>(
            module, "CloseDecompressor");
        if (!create || !decompress || !close) {
            FreeLibrary(module);
            SetLastError(ERROR_PROC_NOT_FOUND);
            return false;
        }

        module_ = module;
        create_ = create;
        decompress_ = decompress;
        close_ = close;
        return true;
    }

    bool EnsureOpen() {
        if (handle_) return true;
        if (!LoadApi()) return false;
        return create_(Detail::kAlgorithmLzms, nullptr, &handle_) != FALSE;
    }

    HMODULE module_ = nullptr;
    Detail::CompressionHandle handle_ = nullptr;
    Detail::CreateDecompressorFn create_ = nullptr;
    Detail::DecompressFn decompress_ = nullptr;
    Detail::CloseDecompressorFn close_ = nullptr;
};

} // namespace CompressionApi
