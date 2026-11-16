// LZMS decompression helper: .chs -> uncompressed .dat
//
// build_chs_embed.py does the same thing through cabinet.dll with no external
// tool, so this is only needed when you want a standalone binary (or want to
// inspect the uncompressed archive by hand).
//
// Compile (Developer Command Prompt for VS 2022):
//   cl /EHsc /O2 编译\decompress_chs.cpp cabinet.lib /Fe:decompress_chs.exe
//
// Usage:
//   decompress_chs.exe Nepgear.chs Nepgear_raw.dat
#include <windows.h>
#include <stdio.h>
#include <vector>
#include <compressapi.h>
#pragma comment(lib, "cabinet.lib")

int wmain(int argc, wchar_t* argv[]) {
    if (argc < 3) { wprintf(L"Usage: %s input.chs output.dat\n", argv[0]); return 1; }
    HANDLE hIn = CreateFileW(argv[1], GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    HANDLE hOut = CreateFileW(argv[2], GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (hIn == INVALID_HANDLE_VALUE || hOut == INVALID_HANDLE_VALUE) { wprintf(L"Open failed\n"); return 1; }
    DWORD br, bw; int count = 0;
    ReadFile(hIn, &count, 4, &br, NULL); WriteFile(hOut, &count, 4, &bw, NULL);
    wprintf(L"Files: %d\n", count);
    for (int i = 0; i < count; i++) {
        int pLen; ReadFile(hIn, &pLen, 4, &br, NULL);
        std::vector<char> path(pLen); ReadFile(hIn, path.data(), pLen, &br, NULL);
        int dSize, sSize; ReadFile(hIn, &dSize, 4, &br, NULL); ReadFile(hIn, &sSize, 4, &br, NULL);
        std::vector<BYTE> raw(sSize); ReadFile(hIn, raw.data(), sSize, &br, NULL);
        DWORD outS = dSize; std::vector<BYTE> outD;
        if (sSize < dSize) {
            DECOMPRESSOR_HANDLE d; SIZE_T a = 0;
            wprintf(L"  Decompress %hs: %d -> %d\n", path.data(), sSize, dSize);
            if (CreateDecompressor(COMPRESS_ALGORITHM_LZMS, NULL, &d)) {
                outD.resize(dSize);
                if (!Decompress(d, raw.data(), sSize, outD.data(), dSize, &a) || a != dSize) { outD = raw; outS = sSize; }
                CloseDecompressor(d);
            } else { outD = raw; outS = sSize; }
        } else { outD = raw; outS = sSize; wprintf(L"  Raw %hs: %d bytes\n", path.data(), sSize); }
        WriteFile(hOut, &pLen, 4, &bw, NULL); WriteFile(hOut, path.data(), pLen, &bw, NULL);
        WriteFile(hOut, &outS, 4, &bw, NULL); WriteFile(hOut, &outS, 4, &bw, NULL);
        WriteFile(hOut, outD.data(), outS, &bw, NULL);
    }
    CloseHandle(hIn); CloseHandle(hOut); wprintf(L"Done -> %s\n", argv[2]); return 0;
}
