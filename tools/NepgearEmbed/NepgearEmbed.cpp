// NepgearEmbed - 把 .chs 补丁数据打进 version.dll 的小工具。
//
// 它不是编译器：exe 里内置一份已经编译好的 version.dll 作为模板，运行时只把
// RCDATA 资源 101 换成由 .chs 逐条目解压得到的原始归档，然后重排 PE 节表另存。
// 这样任何 Windows 机器都能用，不需要 Visual Studio，也不需要 Python。
//
// 模板查找顺序：
//   1. exe 同目录的 template_x86.dll / template_x64.dll（想换新版宿主时放一份即可）
//   2. exe 内置资源（构建时由 编译\build_embed_tool.bat 把两个架构的 DLL 打进去）

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <commdlg.h>   // OPENFILENAMEW

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cwchar>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "../../Common/compression_api.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace {

// ---------------------------------------------------------------- 常量

const DWORD kResourceId = 101;      // 与 hooks/vfs.cpp 的 NEPGEAR_CHS_RESOURCE_ID 一致
const DWORD kRcDataType = 10;       // RT_RCDATA
const int kTemplateX86 = 1;         // exe 内置模板：32 位 version.dll
const int kTemplateX64 = 2;         // exe 内置模板：64 位 version.dll

const int IDC_SRC = 1001;
const int IDC_SRC_BROWSE = 1002;
const int IDC_OUT = 1003;
const int IDC_OUT_BROWSE = 1004;
const int IDC_GO = 1005;
const int IDC_LOG = 1006;
const int IDC_BAR = 1007;
const int IDC_STATUS = 1008;
const int IDC_GAME = 1009;
const int IDC_GAME_BROWSE = 1010;
const int IDC_ARCH_X86 = 1011;
const int IDC_ARCH_X64 = 1012;
const int IDC_ARCH_HINT = 1013;

const UINT WM_APP_LOG = WM_APP + 1;
const UINT WM_APP_PROGRESS = WM_APP + 2;
const UINT WM_APP_DONE = WM_APP + 3;

// ---------------------------------------------------------------- 全局

HWND g_hwnd = nullptr;
HWND g_hSrc = nullptr;
HWND g_hGame = nullptr;
HWND g_hOut = nullptr;
HWND g_hArchX86 = nullptr;
HWND g_hArchX64 = nullptr;
HWND g_hArchHint = nullptr;
HWND g_hLog = nullptr;
HWND g_hBar = nullptr;
HWND g_hStatus = nullptr;
HWND g_hGo = nullptr;
HFONT g_font = nullptr;
double g_scale = 1.0;
volatile LONG g_busy = 0;

uint64_t g_storedTotal = 0;
uint64_t g_rawTotal = 0;

void PostLog(const std::wstring& text);
void PostProgress(int percent, const std::wstring& status);

// ---------------------------------------------------------------- 文本

std::wstring Utf8ToWide(const std::string& text) {
    if (text.empty()) return std::wstring();
    const int need = MultiByteToWideChar(CP_UTF8, 0, text.data(), (int)text.size(), nullptr, 0);
    std::wstring out((size_t)need, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), (int)text.size(), &out[0], need);
    return out;
}

std::wstring Format(const wchar_t* fmt, ...) {
    wchar_t buffer[2048];
    va_list args;
    va_start(args, fmt);
    _vsnwprintf_s(buffer, _countof(buffer), _TRUNCATE, fmt, args);
    va_end(args);
    return std::wstring(buffer);
}

std::wstring FileNameOf(const std::wstring& path) {
    const size_t pos = path.find_last_of(L"\\/");
    return pos == std::wstring::npos ? path : path.substr(pos + 1);
}

std::wstring DirNameOf(const std::wstring& path) {
    const size_t pos = path.find_last_of(L"\\/");
    return pos == std::wstring::npos ? std::wstring() : path.substr(0, pos);
}

std::wstring LowerCase(std::wstring text) {
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] >= L'A' && text[i] <= L'Z') text[i] = (wchar_t)(text[i] + 32);
    }
    return text;
}

// ---------------------------------------------------------------- 文件 IO

bool ReadAllBytes(const std::wstring& path, std::vector<BYTE>& out, std::wstring& err) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        err = Format(L"打不开 %s（错误 %lu）", path.c_str(), GetLastError());
        return false;
    }
    LARGE_INTEGER size = {};
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0) {
        CloseHandle(file);
        err = L"文件为空或无法读取大小：" + path;
        return false;
    }
    out.resize((size_t)size.QuadPart);
    DWORD read = 0;
    const bool ok = ReadFile(file, out.data(), (DWORD)out.size(), &read, nullptr) != FALSE &&
                    (size_t)read == out.size();
    CloseHandle(file);
    if (!ok) {
        err = L"读取失败：" + path;
        return false;
    }
    return true;
}

bool WriteAllBytes(const std::wstring& path, const std::vector<BYTE>& data, std::wstring& err) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        err = Format(L"无法写入 %s（错误 %lu）", path.c_str(), GetLastError());
        return false;
    }
    DWORD written = 0;
    const bool ok = WriteFile(file, data.data(), (DWORD)data.size(), &written, nullptr) != FALSE &&
                    (size_t)written == data.size();
    CloseHandle(file);
    if (!ok) {
        err = L"写入失败：" + path;
        return false;
    }
    return true;
}

// 读 exe 的 PE 头判断位数。架构选错时 version.dll 根本不会被加载，所以只看
// IMAGE_FILE_HEADER.Machine，不信文件名。顺带说明：PE32+ 的可选头魔数是 0x20B，
// 但 Machine 更直接，也覆盖 ARM64 的情况。
bool DetectPeBits(const std::wstring& path, bool& is64, std::wstring& err) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        err = Format(L"打不开 %s（错误 %lu）", path.c_str(), GetLastError());
        return false;
    }

    LARGE_INTEGER fileSize = {};
    GetFileSizeEx(file, &fileSize);

    DWORD read = 0;
    IMAGE_DOS_HEADER dos = {};
    const bool dosOk = ReadFile(file, &dos, sizeof(dos), &read, nullptr) != FALSE &&
                       read == sizeof(dos) && dos.e_magic == IMAGE_DOS_SIGNATURE;
    if (!dosOk) {
        CloseHandle(file);
        err = L"不是有效的 Windows 程序（DOS 头不对）";
        return false;
    }

    LARGE_INTEGER ntPos = {};
    ntPos.QuadPart = dos.e_lfanew;
    if (ntPos.QuadPart <= 0 ||
        ntPos.QuadPart + 4 + (LONGLONG)sizeof(IMAGE_FILE_HEADER) > fileSize.QuadPart) {
        CloseHandle(file);
        err = L"不是有效的 Windows 程序（PE 头位置异常）";
        return false;
    }
    SetFilePointerEx(file, ntPos, nullptr, FILE_BEGIN);

    DWORD signature = 0;
    IMAGE_FILE_HEADER fh = {};
    const bool ntOk = ReadFile(file, &signature, 4, &read, nullptr) != FALSE && read == 4 &&
                      ReadFile(file, &fh, sizeof(fh), &read, nullptr) != FALSE &&
                      read == sizeof(fh);
    CloseHandle(file);
    if (!ntOk || signature != IMAGE_NT_SIGNATURE) {
        err = L"不是有效的 Windows 程序（PE 签名不对）";
        return false;
    }

    switch (fh.Machine) {
    case IMAGE_FILE_MACHINE_I386:
        is64 = false;
        return true;
    case IMAGE_FILE_MACHINE_AMD64:
    case IMAGE_FILE_MACHINE_ARM64:
        is64 = true;
        return true;
    default:
        err = Format(L"不认识的可执行格式（Machine=0x%04X）", (unsigned)fh.Machine);
        return false;
    }
}

// ---------------------------------------------------------------- .chs -> payload

void AppendI32(std::vector<BYTE>& out, int32_t value) {
    const BYTE bytes[4] = {
        (BYTE)(value & 0xFF), (BYTE)((value >> 8) & 0xFF),
        (BYTE)((value >> 16) & 0xFF), (BYTE)((value >> 24) & 0xFF)};
    out.insert(out.end(), bytes, bytes + 4);
}

bool PeekI32(const std::vector<BYTE>& data, size_t& offset, int32_t& value) {
    if (offset + 4 > data.size()) return false;
    value = (int32_t)((DWORD)data[offset] | ((DWORD)data[offset + 1] << 8) |
                      ((DWORD)data[offset + 2] << 16) | ((DWORD)data[offset + 3] << 24));
    offset += 4;
    return true;
}

// .chs 展开成 VFS 直接读取的未压缩归档布局：
//   int32 count
//   repeat { int32 pathLen, path, int32 size, int32 size, bytes }
bool BuildPayload(const std::vector<BYTE>& chs, std::vector<BYTE>& out, std::wstring& err,
                  const std::function<void(int, int)>& progress) {
    size_t offset = 0;
    int32_t count = 0;
    if (!PeekI32(chs, offset, count) || count <= 0) {
        err = L"这不是有效的 .chs：头部条目数异常";
        return false;
    }

    out.clear();
    out.reserve(chs.size() * 2);
    AppendI32(out, count);

    CompressionApi::LzmsDecompressor decompressor;
    g_storedTotal = 0;
    g_rawTotal = 0;

    for (int32_t i = 0; i < count; ++i) {
        int32_t pathLen = 0;
        if (!PeekI32(chs, offset, pathLen) || pathLen <= 0 ||
            offset + (size_t)pathLen > chs.size()) {
            err = Format(L"第 %d 个条目的路径损坏", i + 1);
            return false;
        }
        const std::string path((const char*)chs.data() + offset, (size_t)pathLen);
        offset += (size_t)pathLen;

        int32_t rawSize = 0;
        int32_t storedSize = 0;
        if (!PeekI32(chs, offset, rawSize) || !PeekI32(chs, offset, storedSize)) {
            err = Format(L"第 %d 个条目的长度字段损坏", i + 1);
            return false;
        }
        if (rawSize < 0 || storedSize < 0 || offset + (size_t)storedSize > chs.size()) {
            err = Format(L"第 %d 个条目的数据越界", i + 1);
            return false;
        }

        const BYTE* source = chs.data() + offset;
        offset += (size_t)storedSize;
        g_storedTotal += (uint64_t)storedSize;

        std::vector<BYTE> plain;
        const BYTE* data = source;
        size_t dataLen = (size_t)storedSize;

        if (storedSize < rawSize) {
            plain.resize((size_t)rawSize);
            size_t actual = 0;
            if (!decompressor.Decompress(source, (size_t)storedSize, plain.data(),
                                         plain.size(), &actual) ||
                actual != (size_t)rawSize) {
                err = L"LZMS 解压失败：" + Utf8ToWide(path);
                return false;
            }
            data = plain.data();
            dataLen = plain.size();
        }

        g_rawTotal += (uint64_t)dataLen;
        AppendI32(out, pathLen);
        out.insert(out.end(), path.begin(), path.end());
        AppendI32(out, (int32_t)dataLen);
        AppendI32(out, (int32_t)dataLen);
        out.insert(out.end(), data, data + dataLen);

        if (progress && ((i % 16) == 0 || i + 1 == count)) progress(i + 1, count);
    }
    return true;
}

// ---------------------------------------------------------------- PE 改写

namespace pe {

const DWORD kBadOffset = 0xFFFFFFFFu;

DWORD AlignUp(DWORD value, DWORD alignment) {
    return alignment ? ((value + alignment - 1) / alignment) * alignment : value;
}

struct Image {
    std::vector<BYTE> raw;
    IMAGE_DOS_HEADER* dos = nullptr;
    IMAGE_FILE_HEADER* fh = nullptr;
    BYTE* opt = nullptr;
    bool pe64 = false;
    IMAGE_SECTION_HEADER* sec = nullptr;
    DWORD nsec = 0;
};

DWORD* OptDword(Image& img, DWORD offset) {
    return reinterpret_cast<DWORD*>(img.opt + offset);
}

DWORD SectionAlignment(Image& img) { return *OptDword(img, 32); }
DWORD FileAlignment(Image& img) { return *OptDword(img, 36); }
DWORD SizeOfHeaders(Image& img) { return *OptDword(img, 60); }

IMAGE_DATA_DIRECTORY* DataDirectory(Image& img, DWORD index) {
    const DWORD count = *OptDword(img, img.pe64 ? 108 : 92);
    if (index >= count) return nullptr;
    const DWORD base = img.pe64 ? 112 : 96;
    return reinterpret_cast<IMAGE_DATA_DIRECTORY*>(img.opt + base + index * 8);
}

bool Parse(std::vector<BYTE>& raw, Image& img, std::wstring& err) {
    if (raw.size() < sizeof(IMAGE_DOS_HEADER)) {
        err = L"模板 DLL 太小，不是有效的 PE";
        return false;
    }
    const size_t ntOffset = (size_t)((IMAGE_DOS_HEADER*)raw.data())->e_lfanew;
    if (ntOffset + 4 + sizeof(IMAGE_FILE_HEADER) > raw.size()) {
        err = L"模板 DLL 的 PE 头越界";
        return false;
    }
    if (*(DWORD*)(raw.data() + ntOffset) != IMAGE_NT_SIGNATURE) {
        err = L"模板 DLL 的 PE 签名不正确";
        return false;
    }

    img.raw = std::move(raw);
    img.dos = (IMAGE_DOS_HEADER*)img.raw.data();
    img.fh = (IMAGE_FILE_HEADER*)(img.raw.data() + (size_t)img.dos->e_lfanew + 4);
    img.opt = (BYTE*)(img.fh + 1);

    const size_t optOffset = (size_t)(img.opt - img.raw.data());
    if (optOffset + img.fh->SizeOfOptionalHeader > img.raw.size()) {
        err = L"模板 DLL 的可选头越界";
        return false;
    }

    const WORD magic = *(WORD*)img.opt;
    if (magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
        img.pe64 = false;
    } else if (magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        img.pe64 = true;
    } else {
        err = L"模板 DLL 的可选头格式无法识别";
        return false;
    }

    img.nsec = img.fh->NumberOfSections;
    const size_t secOffset = optOffset + img.fh->SizeOfOptionalHeader;
    if (secOffset + (size_t)img.nsec * sizeof(IMAGE_SECTION_HEADER) > img.raw.size()) {
        err = L"模板 DLL 的节表越界";
        return false;
    }
    img.sec = (IMAGE_SECTION_HEADER*)(img.raw.data() + secOffset);
    return true;
}

DWORD RvaToOffset(Image& img, DWORD rva) {
    for (DWORD i = 0; i < img.nsec; ++i) {
        const DWORD start = img.sec[i].VirtualAddress;
        const DWORD span = img.sec[i].SizeOfRawData;
        if (rva >= start && rva - start < span) return img.sec[i].PointerToRawData + (rva - start);
    }
    return kBadOffset;
}

struct ResItem {
    std::vector<DWORD> path;
    DWORD codePage = 0;
    std::vector<BYTE> data;
    DWORD dataOffset = 0xFFFFFFFFu;
};

bool WalkResource(Image& img, DWORD resRva, DWORD resSize, DWORD dirOffset,
                  std::vector<DWORD>& path, std::vector<ResItem>& out, std::wstring& err) {
    if (dirOffset + sizeof(IMAGE_RESOURCE_DIRECTORY) > resSize) {
        err = L"资源目录越界";
        return false;
    }
    const DWORD baseOffset = RvaToOffset(img, resRva);
    if (baseOffset == kBadOffset) {
        err = L"资源目录的 RVA 无法映射到文件偏移";
        return false;
    }
    const BYTE* base = img.raw.data() + baseOffset;
    const auto* dir = (const IMAGE_RESOURCE_DIRECTORY*)(base + dirOffset);
    const DWORD total = (DWORD)dir->NumberOfNamedEntries + (DWORD)dir->NumberOfIdEntries;

    for (DWORD i = 0; i < total; ++i) {
        const auto* entry = (const IMAGE_RESOURCE_DIRECTORY_ENTRY*)(dir + 1) + i;
        if (entry->Name & 0x80000000u) {
            err = L"模板 DLL 含有命名资源，本工具暂不支持";
            return false;
        }
        const DWORD id = entry->Name & 0xFFFF;
        const DWORD entryOffset = entry->OffsetToData;

        if (entryOffset & 0x80000000u) {
            path.push_back(id);
            const bool ok = WalkResource(img, resRva, resSize, entryOffset & 0x7FFFFFFFu,
                                         path, out, err);
            path.pop_back();
            if (!ok) return false;
        } else {
            if (entryOffset + sizeof(IMAGE_RESOURCE_DATA_ENTRY) > resSize) {
                err = L"资源数据条目越界";
                return false;
            }
            const auto* dataEntry = (const IMAGE_RESOURCE_DATA_ENTRY*)(base + entryOffset);
            const DWORD dataOffset = RvaToOffset(img, dataEntry->OffsetToData);
            if (dataOffset == kBadOffset || dataOffset + dataEntry->Size > img.raw.size()) {
                err = L"资源数据越界";
                return false;
            }
            ResItem item;
            item.path = path;
            item.path.push_back(id);
            item.codePage = dataEntry->CodePage;
            item.data.assign(img.raw.data() + dataOffset,
                             img.raw.data() + dataOffset + dataEntry->Size);
            out.push_back(std::move(item));
        }
    }
    return true;
}

struct ResNode {
    DWORD id = 0;
    bool isData = false;
    int index = -1;
    std::vector<ResNode*> kids;
    DWORD dirOffset = 0;
    DWORD entryOffset = 0;
    DWORD dataOffset = 0;
    ~ResNode() {
        for (size_t i = 0; i < kids.size(); ++i) delete kids[i];
    }
};

ResNode* ChildFor(ResNode* parent, DWORD id) {
    for (size_t i = 0; i < parent->kids.size(); ++i) {
        if (parent->kids[i]->id == id) return parent->kids[i];
    }
    ResNode* node = new ResNode();
    node->id = id;
    parent->kids.push_back(node);
    return node;
}

void SortKids(ResNode* node) {
    std::sort(node->kids.begin(), node->kids.end(),
              [](const ResNode* a, const ResNode* b) { return a->id < b->id; });
    for (size_t i = 0; i < node->kids.size(); ++i) {
        if (!node->kids[i]->isData) SortKids(node->kids[i]);
    }
}

void LayoutDirectories(ResNode* node, DWORD& cursor) {
    node->dirOffset = cursor;
    cursor += (DWORD)sizeof(IMAGE_RESOURCE_DIRECTORY) +
              (DWORD)node->kids.size() * (DWORD)sizeof(IMAGE_RESOURCE_DIRECTORY_ENTRY);
    for (size_t i = 0; i < node->kids.size(); ++i) {
        if (!node->kids[i]->isData) LayoutDirectories(node->kids[i], cursor);
    }
}

void LayoutEntries(ResNode* node, DWORD& cursor) {
    for (size_t i = 0; i < node->kids.size(); ++i) {
        ResNode* kid = node->kids[i];
        if (kid->isData) {
            kid->entryOffset = cursor;
            cursor += (DWORD)sizeof(IMAGE_RESOURCE_DATA_ENTRY);
        } else {
            LayoutEntries(kid, cursor);
        }
    }
}

void LayoutData(ResNode* node, DWORD& cursor, std::vector<ResItem>& items) {
    for (size_t i = 0; i < node->kids.size(); ++i) {
        ResNode* kid = node->kids[i];
        if (kid->isData) {
            kid->dataOffset = cursor;
            items[(size_t)kid->index].dataOffset = cursor;
            cursor += AlignUp((DWORD)items[(size_t)kid->index].data.size(), 8);
        } else {
            LayoutData(kid, cursor, items);
        }
    }
}

void WriteDirectory(const ResNode* node, BYTE* buffer) {
    auto* dir = (IMAGE_RESOURCE_DIRECTORY*)(buffer + node->dirOffset);
    dir->Characteristics = 0;
    dir->TimeDateStamp = 0;
    dir->MajorVersion = 0;
    dir->MinorVersion = 0;
    dir->NumberOfNamedEntries = 0;
    dir->NumberOfIdEntries = (WORD)node->kids.size();

    auto* entry = (IMAGE_RESOURCE_DIRECTORY_ENTRY*)(dir + 1);
    for (size_t i = 0; i < node->kids.size(); ++i) {
        const ResNode* kid = node->kids[i];
        entry[i].Name = kid->id;
        entry[i].OffsetToData = kid->isData ? kid->entryOffset : (0x80000000u | kid->dirOffset);
    }
    for (size_t i = 0; i < node->kids.size(); ++i) {
        if (!node->kids[i]->isData) WriteDirectory(node->kids[i], buffer);
    }
}

void WriteEntries(const ResNode* node, BYTE* buffer, DWORD sectionRva,
                  const std::vector<ResItem>& items) {
    for (size_t i = 0; i < node->kids.size(); ++i) {
        const ResNode* kid = node->kids[i];
        if (kid->isData) {
            const ResItem& item = items[(size_t)kid->index];
            auto* entry = (IMAGE_RESOURCE_DATA_ENTRY*)(buffer + kid->entryOffset);
            entry->OffsetToData = sectionRva + kid->dataOffset;
            entry->Size = (DWORD)item.data.size();
            entry->CodePage = item.codePage;
            entry->Reserved = 0;
        } else {
            WriteEntries(kid, buffer, sectionRva, items);
        }
    }
}

// 重新序列化整个 .rsrc。sectionRva 参与填写数据条目的 RVA，所以先用 0 求长度，
// 确定新节地址后再构建一次。
bool BuildResourceSection(std::vector<ResItem>& items, DWORD sectionRva,
                          std::vector<BYTE>& out, std::wstring& err) {
    if (items.empty()) {
        err = L"模板 DLL 里没有任何资源";
        return false;
    }

    ResNode* root = new ResNode();
    for (size_t i = 0; i < items.size(); ++i) {
        ResNode* node = root;
        for (size_t k = 0; k < items[i].path.size(); ++k) {
            node = ChildFor(node, items[i].path[k]);
            if (k + 1 == items[i].path.size()) {
                node->isData = true;
                node->index = (int)i;
            }
        }
    }
    SortKids(root);

    DWORD cursor = 0;
    LayoutDirectories(root, cursor);
    LayoutEntries(root, cursor);
    cursor = AlignUp(cursor, 16);
    LayoutData(root, cursor, items);

    out.assign((size_t)cursor, 0);
    WriteDirectory(root, out.data());
    WriteEntries(root, out.data(), sectionRva, items);

    for (size_t i = 0; i < items.size(); ++i) {
        if (items[i].data.empty() || items[i].dataOffset == 0xFFFFFFFFu) continue;
        memcpy(out.data() + items[i].dataOffset, items[i].data.data(), items[i].data.size());
    }

    delete root;
    return true;
}

} // namespace pe

// 把 payload 写进模板 DLL 的 RCDATA 101，输出新的 DLL。
bool RewriteDll(std::vector<BYTE>& templateBytes, const std::vector<BYTE>& payload,
                const std::wstring& outPath, bool want64, std::wstring& err) {
    pe::Image img;
    if (!pe::Parse(templateBytes, img, err)) return false;
    if (img.pe64 != want64) {
        err = want64 ? L"这份模板不是 64 位 DLL，生成不了 x64 产物"
                     : L"这份模板不是 32 位 DLL，生成不了 x86 产物";
        return false;
    }

    IMAGE_DATA_DIRECTORY* resDir = pe::DataDirectory(img, 2);
    if (!resDir || resDir->VirtualAddress == 0 || resDir->Size == 0) {
        err = L"模板 DLL 没有资源目录";
        return false;
    }

    std::vector<pe::ResItem> items;
    std::vector<DWORD> path;
    if (!pe::WalkResource(img, resDir->VirtualAddress, resDir->Size, 0, path, items, err)) {
        return false;
    }

    bool replaced = false;
    for (size_t i = 0; i < items.size(); ++i) {
        const std::vector<DWORD>& p = items[i].path;
        if (p.size() >= 2 && p[0] == kRcDataType && p[1] == kResourceId) {
            items[i].data = payload;
            items[i].codePage = 0;
            replaced = true;
            break;
        }
    }
    if (!replaced) {
        pe::ResItem item;
        item.path.push_back(kRcDataType);
        item.path.push_back(kResourceId);
        item.path.push_back(1033);
        item.codePage = 0;
        item.data = payload;
        items.push_back(std::move(item));
    }

    // 第一遍只为量出节长度（RVA 用 0 占位）。
    std::vector<BYTE> probe;
    if (!pe::BuildResourceSection(items, 0, probe, err)) return false;
    const DWORD rsrcSize = (DWORD)probe.size();
    std::vector<BYTE>().swap(probe);

    DWORD rsrcIndex = (DWORD)-1;
    for (DWORD i = 0; i < img.nsec; ++i) {
        if (resDir->VirtualAddress >= img.sec[i].VirtualAddress &&
            resDir->VirtualAddress - img.sec[i].VirtualAddress < img.sec[i].SizeOfRawData) {
            rsrcIndex = i;
            break;
        }
    }
    if (rsrcIndex == (DWORD)-1) {
        err = L"定位不到模板 DLL 的资源节";
        return false;
    }

    const DWORD secAlign = pe::SectionAlignment(img);
    const DWORD fileAlign = pe::FileAlignment(img);
    const DWORD headersSize = pe::SizeOfHeaders(img);

    struct SecCopy {
        IMAGE_SECTION_HEADER hdr;
        DWORD sourceIndex = 0;
        DWORD sourceRawPtr = 0;
        DWORD sourceRawSize = 0;
        DWORD oldVa = 0;
        DWORD oldVSize = 0;
        bool moved = false;
    };

    std::vector<SecCopy> keep;
    for (DWORD i = 0; i < img.nsec; ++i) {
        SecCopy copy;
        copy.hdr = img.sec[i];
        copy.sourceIndex = i;
        copy.sourceRawPtr = img.sec[i].PointerToRawData;
        copy.sourceRawSize = img.sec[i].SizeOfRawData;
        copy.oldVa = img.sec[i].VirtualAddress;
        copy.oldVSize = img.sec[i].Misc.VirtualSize;
        keep.push_back(copy);
    }
    std::sort(keep.begin(), keep.end(), [](const SecCopy& a, const SecCopy& b) {
        return a.hdr.VirtualAddress < b.hdr.VirtualAddress;
    });

    size_t rsrcSlot = 0;
    for (size_t i = 0; i < keep.size(); ++i) {
        if (keep[i].sourceIndex == rsrcIndex) {
            rsrcSlot = i;
            break;
        }
    }

    // 资源节留在原地址，只换大小；只有它后面的节需要顺移。这样 SizeOfImage
    // 基本不变，加载后的地址空间不会被白白撑大。
    DWORD cursor = pe::AlignUp(headersSize, secAlign);
    for (size_t i = 0; i < keep.size(); ++i) {
        SecCopy& s = keep[i];
        DWORD va = pe::AlignUp(cursor, secAlign);
        if (va < s.oldVa) va = s.oldVa;
        s.hdr.VirtualAddress = va;
        s.moved = (va != s.oldVa);
        if (s.sourceIndex == rsrcIndex) {
            s.hdr.Misc.VirtualSize = rsrcSize;
            s.hdr.SizeOfRawData = pe::AlignUp(rsrcSize, fileAlign);
        }
        cursor = va + pe::AlignUp(s.hdr.Misc.VirtualSize, secAlign);
    }
    const DWORD newSizeOfImage = pe::AlignUp(cursor, secAlign);

    // 资源节的最终地址定了，正式构建一次（长度与探测时一致）。
    std::vector<BYTE> section;
    if (!pe::BuildResourceSection(items, keep[rsrcSlot].hdr.VirtualAddress, section, err)) {
        return false;
    }

    // 节被挪动后，可选头里所有落在该节原地址范围内的 RVA 都要平移。
    auto relocateRva = [&](DWORD rva) -> DWORD {
        for (size_t i = 0; i < keep.size(); ++i) {
            const SecCopy& s = keep[i];
            if (!s.moved) continue;
            if (rva >= s.oldVa && rva - s.oldVa < s.oldVSize) {
                return s.hdr.VirtualAddress + (rva - s.oldVa);
            }
        }
        return rva;
    };

    const DWORD dirCount = *pe::OptDword(img, img.pe64 ? 108 : 92);
    for (DWORD i = 0; i < dirCount; ++i) {
        IMAGE_DATA_DIRECTORY* dir = pe::DataDirectory(img, i);
        if (dir && dir->VirtualAddress) dir->VirtualAddress = relocateRva(dir->VirtualAddress);
    }

    // 文件布局：按 VA 顺序重新排 raw 数据。
    DWORD fileCursor = pe::AlignUp(headersSize, fileAlign);
    for (size_t i = 0; i < keep.size(); ++i) {
        keep[i].hdr.PointerToRawData = pe::AlignUp(fileCursor, fileAlign);
        fileCursor = keep[i].hdr.PointerToRawData +
                     pe::AlignUp(keep[i].hdr.SizeOfRawData, fileAlign);
    }

    for (size_t i = 0; i < keep.size(); ++i) img.sec[i] = keep[i].hdr;

    *pe::OptDword(img, 56) = newSizeOfImage;
    resDir->VirtualAddress = keep[rsrcSlot].hdr.VirtualAddress;
    resDir->Size = rsrcSize;

    std::vector<BYTE> output((size_t)fileCursor, 0);
    memcpy(output.data(), img.raw.data(), headersSize);
    for (size_t i = 0; i < keep.size(); ++i) {
        // 目标节可能比原来小（补丁体积变化），按新尺寸截断，否则会写穿缓冲区。
        const DWORD copySize = keep[i].hdr.SizeOfRawData < keep[i].sourceRawSize
                                   ? keep[i].hdr.SizeOfRawData
                                   : keep[i].sourceRawSize;
        memcpy(output.data() + keep[i].hdr.PointerToRawData,
               img.raw.data() + keep[i].sourceRawPtr, copySize);
    }
    const DWORD rsrcRawPtr = keep[rsrcSlot].hdr.PointerToRawData;
    memcpy(output.data() + rsrcRawPtr, section.data(), section.size());


    const std::wstring tempPath = outPath + L".tmp";
    if (!WriteAllBytes(tempPath, output, err)) {
        DeleteFileW(tempPath.c_str());
        return false;
    }
    if (!MoveFileExW(tempPath.c_str(), outPath.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        err = Format(L"无法覆盖 %s（游戏是不是还在运行？错误 %lu）", outPath.c_str(), GetLastError());
        DeleteFileW(tempPath.c_str());
        return false;
    }
    return true;
}

// 把完整的 DLL 裁成「骨架」：RCDATA 101 只留一个 1 字节的占位条目。
// 转换时这份资源总会被用户的 .chs 整体覆盖，模板里存 23 MB 纯属浪费。
// 构建脚本用它从当前的 Release\version.dll 现裁模板，保证模板与源码同步。
bool StripTemplate(const std::wstring& inPath, const std::wstring& outPath, std::wstring& err) {
    std::vector<BYTE> bytes;
    if (!ReadAllBytes(inPath, bytes, err)) return false;
    if (bytes.size() < sizeof(IMAGE_DOS_HEADER)) {
        err = L"输入文件太小，不是有效的 PE";
        return false;
    }
    const size_t ntOffset = (size_t)((const IMAGE_DOS_HEADER*)bytes.data())->e_lfanew;
    if (ntOffset + 6 > bytes.size()) {
        err = L"输入文件的 PE 头越界";
        return false;
    }
    const WORD machine = *(const WORD*)(bytes.data() + ntOffset + 4);
    if (machine != IMAGE_FILE_MACHINE_I386 && machine != IMAGE_FILE_MACHINE_AMD64) {
        err = Format(L"不支持的 PE 架构 %#x", (unsigned)machine);
        return false;
    }
    const bool is64 = (machine == IMAGE_FILE_MACHINE_AMD64);

    // 结构与 BuildPayload 产出的完全一致：1 个条目、1 字节数据。内容无所谓，
    // 保留合法结构只是为了让骨架自己也能被解析出来。
    std::vector<BYTE> payload;
    static const char kStubPath[] = "stub";
    static const size_t kStubPathLen = sizeof(kStubPath) - 1;
    AppendI32(payload, 1);
    AppendI32(payload, (int32_t)kStubPathLen);
    payload.insert(payload.end(), kStubPath, kStubPath + kStubPathLen);
    AppendI32(payload, 1);
    AppendI32(payload, 1);
    payload.push_back(0);

    return RewriteDll(bytes, payload, outPath, is64, err);
}

bool LoadTemplate(bool want64, std::vector<BYTE>& out, std::wstring& err,
                  std::wstring& source) {
    const wchar_t* arch = want64 ? L"x64" : L"x86";

    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    const std::wstring external =
        DirNameOf(exePath) + (want64 ? L"\\template_x64.dll" : L"\\template_x86.dll");
    if (GetFileAttributesW(external.c_str()) != INVALID_FILE_ATTRIBUTES) {
        if (!ReadAllBytes(external, out, err)) return false;
        source = L"外置 " + FileNameOf(external);
        return true;
    }

    HMODULE module = GetModuleHandleW(nullptr);
    HRSRC res = FindResourceW(module, MAKEINTRESOURCEW(want64 ? kTemplateX64 : kTemplateX86),
                              L"TEMPLATE");
    if (!res) {
        err = Format(L"exe 里没有内置 %s 模板，同目录也没有 template_%s.dll", arch, arch);
        return false;
    }
    const DWORD size = SizeofResource(module, res);
    HGLOBAL handle = LoadResource(module, res);
    const void* data = handle ? LockResource(handle) : nullptr;
    if (!data || size == 0) {
        err = Format(L"内置 %s 模板读取失败", arch);
        return false;
    }
    out.assign((const BYTE*)data, (const BYTE*)data + size);
    source = Format(L"内置模板 (%s)", arch);
    return true;
}

bool RunConvert(const std::wstring& chsPath, const std::wstring& outPath, bool want64,
                std::wstring& message) {
    std::wstring err;
    std::vector<BYTE> tpl;
    std::wstring tplSource;

    PostLog(want64 ? L"[1/4] 载入 x64 模板 DLL" : L"[1/4] 载入 x86 模板 DLL");
    if (!LoadTemplate(want64, tpl, err, tplSource)) {
        message = err;
        return false;
    }
    PostLog(Format(L"      %s，%.2f MB", tplSource.c_str(), tpl.size() / 1048576.0));

    PostLog(L"[2/4] 读取 " + FileNameOf(chsPath));
    std::vector<BYTE> chs;
    if (!ReadAllBytes(chsPath, chs, err)) {
        message = err;
        return false;
    }
    PostLog(Format(L"      %.2f MB", chs.size() / 1048576.0));

    PostLog(L"[3/4] 解压条目");
    std::vector<BYTE> payload;
    const bool built = BuildPayload(chs, payload, err, [](int done, int total) {
        PostProgress(total ? done * 100 / total : 0, Format(L"解压 %d / %d", done, total));
    });
    if (!built) {
        message = err;
        return false;
    }
    PostProgress(100, L"解压完成");
    PostLog(Format(L"      %.2f MB 压缩 -> %.2f MB 展开", g_storedTotal / 1048576.0,
                   g_rawTotal / 1048576.0));

    PostLog(L"[4/4] 写入内嵌资源并另存 DLL");
    if (!RewriteDll(tpl, payload, outPath, want64, err)) {
        message = err;
        return false;
    }

    uint64_t size = 0;
    WIN32_FILE_ATTRIBUTE_DATA info = {};
    if (GetFileAttributesExW(outPath.c_str(), GetFileExInfoStandard, &info)) {
        size = ((uint64_t)info.nFileSizeHigh << 32) | info.nFileSizeLow;
    }

    PostProgress(100, L"完成");
    PostLog(Format(L"      已生成 %s（%llu 字节）", outPath.c_str(), (unsigned long long)size));
    message = Format(L"生成成功：%s（%s，%llu 字节）", FileNameOf(outPath).c_str(),
                     want64 ? L"x64" : L"x86", (unsigned long long)size);
    return true;
}

// ---------------------------------------------------------------- GUI

void PostLog(const std::wstring& text) {
    if (!g_hwnd) return;
    PostMessageW(g_hwnd, WM_APP_LOG, 0, (LPARAM)new std::wstring(text));
}

void PostProgress(int percent, const std::wstring& status) {
    if (!g_hwnd) return;
    PostMessageW(g_hwnd, WM_APP_PROGRESS, (WPARAM)percent,
                 (LPARAM)(status.empty() ? nullptr : new std::wstring(status)));
}

void AppendLogLine(const std::wstring& text) {
    const int length = GetWindowTextLengthW(g_hLog);
    SendMessageW(g_hLog, EM_SETSEL, (WPARAM)length, (LPARAM)length);
    const std::wstring line = text + L"\r\n";
    SendMessageW(g_hLog, EM_REPLACESEL, FALSE, (LPARAM)line.c_str());
    SendMessageW(g_hLog, EM_SCROLLCARET, 0, 0);
}

int Scale(int value) { return (int)(value * g_scale + 0.5); }

struct Job {
    std::wstring source;
    std::wstring output;
    bool is64 = false;
};

DWORD WINAPI WorkerProc(LPVOID param) {
    std::unique_ptr<Job> job((Job*)param);
    std::wstring message;
    const bool ok = RunConvert(job->source, job->output, job->is64, message);
    PostMessageW(g_hwnd, WM_APP_DONE, ok ? 1 : 0, (LPARAM)new std::wstring(message));
    return 0;
}

void SetBusy(bool busy) {
    InterlockedExchange(&g_busy, busy ? 1 : 0);
    EnableWindow(g_hGo, busy ? FALSE : TRUE);
    EnableWindow(g_hSrc, busy ? FALSE : TRUE);
    EnableWindow(g_hGame, busy ? FALSE : TRUE);
    EnableWindow(g_hOut, busy ? FALSE : TRUE);
    EnableWindow(g_hArchX86, busy ? FALSE : TRUE);
    EnableWindow(g_hArchX64, busy ? FALSE : TRUE);
    EnableWindow(GetDlgItem(g_hwnd, IDC_SRC_BROWSE), busy ? FALSE : TRUE);
    EnableWindow(GetDlgItem(g_hwnd, IDC_GAME_BROWSE), busy ? FALSE : TRUE);
    EnableWindow(GetDlgItem(g_hwnd, IDC_OUT_BROWSE), busy ? FALSE : TRUE);
    SetWindowTextW(g_hGo, busy ? L"处理中..." : L"开始生成");
}

std::wstring GetText(HWND control) {
    const int length = GetWindowTextLengthW(control);
    std::wstring text((size_t)length + 1, L'\0');
    GetWindowTextW(control, &text[0], length + 1);
    text.resize((size_t)length);
    return text;
}

void StartConversion() {
    if (InterlockedCompareExchange(&g_busy, 1, 0) != 0) return;

    std::wstring source = GetText(g_hSrc);
    std::wstring output = GetText(g_hOut);

    if (source.empty()) {
        InterlockedExchange(&g_busy, 0);
        MessageBoxW(g_hwnd, L"请先选择 .chs 文件，或者把 .chs 直接拖进窗口。",
                    L"Nepgear 补丁打包器", MB_ICONINFORMATION);
        return;
    }
    if (GetFileAttributesW(source.c_str()) == INVALID_FILE_ATTRIBUTES) {
        InterlockedExchange(&g_busy, 0);
        MessageBoxW(g_hwnd, L"找不到这个 .chs 文件。", L"Nepgear 补丁打包器", MB_ICONWARNING);
        return;
    }
    if (output.empty()) {
        output = DirNameOf(source) + L"\\version.dll";
        SetWindowTextW(g_hOut, output.c_str());
    }

    SetWindowTextW(g_hLog, L"");
    SendMessageW(g_hBar, PBM_SETPOS, 0, 0);
    SetWindowTextW(g_hStatus, L"开始");

    const bool is64 = SendMessageW(g_hArchX64, BM_GETCHECK, 0, 0) == BST_CHECKED;

    SetBusy(true);
    auto* job = new Job();
    job->source = source;
    job->output = output;
    job->is64 = is64;
    HANDLE thread = CreateThread(nullptr, 0, WorkerProc, job, 0, nullptr);
    if (!thread) {
        delete job;
        SetBusy(false);
        MessageBoxW(g_hwnd, L"无法创建后台线程。", L"Nepgear 补丁打包器", MB_ICONERROR);
        return;
    }
    CloseHandle(thread);
}

// 选中游戏主程序后立刻读位数并把单选框拨过去，用户仍然可以手动改回来
// （有些游戏的主程序和外层启动器位数不一样）。
void ApplyGameExe(const std::wstring& path) {
    SetWindowTextW(g_hGame, path.c_str());

    bool is64 = false;
    std::wstring err;
    if (!DetectPeBits(path, is64, err)) {
        SetWindowTextW(g_hArchHint, (L"读不出位数：" + err).c_str());
        return;
    }

    SendMessageW(is64 ? g_hArchX64 : g_hArchX86, BM_SETCHECK, BST_CHECKED, 0);
    SendMessageW(is64 ? g_hArchX86 : g_hArchX64, BM_SETCHECK, BST_UNCHECKED, 0);
    SetWindowTextW(g_hArchHint, is64 ? L"检测到 64 位，将生成 x64 的 version.dll"
                                     : L"检测到 32 位，将生成 x86 的 version.dll");
}

void PickGame() {
    wchar_t path[MAX_PATH] = L"";
    OPENFILENAMEW dialog = {};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = g_hwnd;
    dialog.lpstrFilter = L"游戏主程序 (*.exe)\0*.exe\0所有文件\0*.*\0";
    dialog.lpstrFile = path;
    dialog.nMaxFile = MAX_PATH;
    dialog.lpstrTitle = L"选择游戏的 EXE（用它的位数决定生成哪个架构）";
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    if (!GetOpenFileNameW(&dialog)) return;

    ApplyGameExe(path);
}

void PickSource() {
    wchar_t path[MAX_PATH] = L"";
    OPENFILENAMEW dialog = {};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = g_hwnd;
    dialog.lpstrFilter = L"Nepgear 补丁数据 (*.chs)\0*.chs\0所有文件\0*.*\0";
    dialog.lpstrFile = path;
    dialog.nMaxFile = MAX_PATH;
    dialog.lpstrTitle = L"选择 .chs 文件";
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    if (!GetOpenFileNameW(&dialog)) return;

    SetWindowTextW(g_hSrc, path);
    SetWindowTextW(g_hOut, (DirNameOf(path) + L"\\version.dll").c_str());
}

void PickOutput() {
    wchar_t path[MAX_PATH] = L"version.dll";
    OPENFILENAMEW dialog = {};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = g_hwnd;
    dialog.lpstrFilter = L"动态链接库 (*.dll)\0*.dll\0所有文件\0*.*\0";
    dialog.lpstrFile = path;
    dialog.nMaxFile = MAX_PATH;
    dialog.lpstrTitle = L"另存为";
    dialog.lpstrDefExt = L"dll";
    dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    if (!GetSaveFileNameW(&dialog)) return;
    SetWindowTextW(g_hOut, path);
}

bool EndsWith(const std::wstring& text, const wchar_t* suffix) {
    const size_t length = wcslen(suffix);
    return text.size() >= length && text.compare(text.size() - length, length, suffix) == 0;
}

void AcceptDrop(HDROP drop) {
    const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
    std::wstring gamePath;
    for (UINT i = 0; i < count; ++i) {
        wchar_t path[MAX_PATH] = L"";
        if (!DragQueryFileW(drop, i, path, MAX_PATH)) continue;
        const std::wstring lower = LowerCase(FileNameOf(path));
        if (EndsWith(lower, L".chs")) {
            SetWindowTextW(g_hSrc, path);
            SetWindowTextW(g_hOut, (DirNameOf(path) + L"\\version.dll").c_str());
        } else if (EndsWith(lower, L".exe")) {
            gamePath = path;
        }
    }
    if (!gamePath.empty()) ApplyGameExe(gamePath);
    DragFinish(drop);
}

HWND CreateControl(const wchar_t* className, const wchar_t* text, DWORD style,
                   int x, int y, int w, int h, int id) {
    HWND control = CreateWindowExW(0, className, text, WS_CHILD | WS_VISIBLE | style,
                                   Scale(x), Scale(y), Scale(w), Scale(h), g_hwnd,
                                   (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
    if (control && g_font) SendMessageW(control, WM_SETFONT, (WPARAM)g_font, TRUE);
    return control;
}

void BuildLayout() {
    NONCLIENTMETRICSW metrics = {};
    metrics.cbSize = sizeof(metrics);
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0);
    LOGFONTW fontSpec = metrics.lfMessageFont;
    fontSpec.lfHeight = -(int)(12 * g_scale + 0.5);
    wcscpy_s(fontSpec.lfFaceName, L"Microsoft YaHei UI");
    g_font = CreateFontIndirectW(&fontSpec);

    CreateControl(L"STATIC", L"源文件", SS_LEFT, 14, 18, 56, 20, -1);
    g_hSrc = CreateControl(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL, 74, 14, 496, 26, IDC_SRC);
    CreateControl(L"BUTTON", L"浏览...", BS_PUSHBUTTON, 586, 14, 100, 26, IDC_SRC_BROWSE);

    CreateControl(L"STATIC", L"游戏 EXE", SS_LEFT, 14, 56, 56, 20, -1);
    g_hGame = CreateControl(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL, 74, 52, 496, 26, IDC_GAME);
    CreateControl(L"BUTTON", L"浏览...", BS_PUSHBUTTON, 586, 52, 100, 26, IDC_GAME_BROWSE);

    CreateControl(L"STATIC", L"输出到", SS_LEFT, 14, 94, 56, 20, -1);
    g_hOut = CreateControl(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL, 74, 90, 496, 26, IDC_OUT);
    CreateControl(L"BUTTON", L"另存为...", BS_PUSHBUTTON, 586, 90, 100, 26, IDC_OUT_BROWSE);

    g_hArchX86 = CreateControl(L"BUTTON", L"32 位 (x86)", BS_AUTORADIOBUTTON | WS_GROUP,
                               74, 126, 106, 22, IDC_ARCH_X86);
    g_hArchX64 = CreateControl(L"BUTTON", L"64 位 (x64)", BS_AUTORADIOBUTTON,
                               188, 126, 106, 22, IDC_ARCH_X64);
    g_hArchHint = CreateControl(L"STATIC", L"没选游戏 EXE 时，按这里选的位数生成",
                                SS_LEFT | SS_ENDELLIPSIS, 306, 129, 380, 18, IDC_ARCH_HINT);
    SendMessageW(g_hArchX86, BM_SETCHECK, BST_CHECKED, 0);

    g_hGo = CreateControl(L"BUTTON", L"开始生成", BS_DEFPUSHBUTTON, 14, 160, 120, 30, IDC_GO);
    g_hBar = CreateControl(PROGRESS_CLASSW, L"", PBS_SMOOTH, 146, 164, 540, 22, IDC_BAR);
    SendMessageW(g_hBar, PBM_SETRANGE32, 0, 100);
    g_hStatus = CreateControl(L"STATIC", L"把 .chs 拖进窗口，顺便把游戏 EXE 拖进来可以自动选位数",
                              SS_LEFT | SS_ENDELLIPSIS, 14, 198, 672, 18, IDC_STATUS);

    g_hLog = CreateControl(L"EDIT", L"",
                           WS_BORDER | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL,
                           14, 222, 672, 268, IDC_LOG);
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE:
        g_hwnd = hwnd;
        BuildLayout();
        DragAcceptFiles(hwnd, TRUE);
        return 0;

    case WM_DROPFILES:
        AcceptDrop((HDROP)wParam);
        return 0;

    case WM_APP_LOG: {
        std::unique_ptr<std::wstring> text((std::wstring*)lParam);
        if (text) AppendLogLine(*text);
        return 0;
    }

    case WM_APP_PROGRESS: {
        std::unique_ptr<std::wstring> status((std::wstring*)lParam);
        SendMessageW(g_hBar, PBM_SETPOS, (WPARAM)wParam, 0);
        if (status) SetWindowTextW(g_hStatus, status->c_str());
        return 0;
    }

    case WM_APP_DONE: {
        std::unique_ptr<std::wstring> text((std::wstring*)lParam);
        SetBusy(false);
        const bool ok = wParam != 0;
        if (text) AppendLogLine(ok ? *text : (L"失败：" + *text));
        if (!ok) {
            MessageBoxW(hwnd, text ? text->c_str() : L"未知错误", L"生成失败", MB_ICONERROR);
        }
        return 0;
    }

    case WM_COMMAND: {
        const int id = LOWORD(wParam);
        if (id == IDC_SRC_BROWSE) PickSource();
        else if (id == IDC_GAME_BROWSE) PickGame();
        else if (id == IDC_OUT_BROWSE) PickOutput();
        else if (id == IDC_ARCH_X86 || id == IDC_ARCH_X64) {
            SetWindowTextW(g_hArchHint, L"按手动选择的位数生成");
        } else if (id == IDC_GO) StartConversion();
        return 0;
    }

    case WM_CLOSE:
        if (g_busy) {
            MessageBoxW(hwnd, L"还在处理，等它完成再关。", L"Nepgear 补丁打包器", MB_ICONINFORMATION);
            return 0;
        }
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        if (g_font) DeleteObject(g_font);
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

} // namespace

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int) {
    // Optional: with arguments it converts silently and opens no window.
    // Double-clicking (no arguments) still shows the GUI.
    {
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (argv && argc >= 5 && std::wstring(argv[1]) == L"--lzms-raw") {
            // 调试入口：解一个裸 LZMS 块，结果写文件，状态另存 .status
            std::vector<BYTE> in;
            std::wstring err;
            const size_t rawSize = (size_t)_wtoi64(argv[3]);
            std::vector<BYTE> out(rawSize ? rawSize : 1);
            bool ok = ReadAllBytes(argv[2], in, err);
            if (ok) {
                ok = LzmsNative::Decompress(in.data(), in.size(), out.data(), rawSize);
            }
            if (ok) WriteAllBytes(argv[4], out, err);
            char status[256] = {};
            sprintf_s(status, "ok=%d code=%d progress=%llu raw=%llu in=%llu\r\n",
                      ok ? 1 : 0, LzmsNative::detail::LastErrorCode(),
                      (unsigned long long)LzmsNative::detail::LastProgress(),
                      (unsigned long long)rawSize, (unsigned long long)in.size());
            const std::wstring statusPath = std::wstring(argv[4]) + L".status";
            std::vector<BYTE> text(status, status + strlen(status));
            WriteAllBytes(statusPath, text, err);
            LocalFree(argv);
            return ok ? 0 : 1;
        }
        if (argv && argc >= 4 && std::wstring(argv[1]) == L"--strip") {
            // 构建用：把带完整补丁数据的 DLL 裁成骨架模板，不弹窗。
            const std::wstring stripIn = argv[2];
            const std::wstring stripOut = argv[3];
            LocalFree(argv);
            std::wstring message;
            return StripTemplate(stripIn, stripOut, message) ? 0 : 1;
        }
        if (argv && argc >= 2) {
            // NepgearEmbed.exe <in.chs> [out.dll] [--x64] [--game <game.exe>]
            // --game 里那个 exe 的位数会覆盖 --x64/--x86，和界面上的行为一致。
            std::wstring source;
            std::wstring output;
            std::wstring game;
            bool is64 = false;
            for (int i = 1; i < argc; ++i) {
                const std::wstring arg = argv[i];
                if (arg == L"--x64") {
                    is64 = true;
                } else if (arg == L"--x86") {
                    is64 = false;
                } else if (arg == L"--game" && i + 1 < argc) {
                    game = argv[++i];
                } else if (source.empty()) {
                    source = arg;
                } else if (output.empty()) {
                    output = arg;
                }
            }
            if (!game.empty()) {
                bool gameIs64 = false;
                std::wstring detectError;
                if (DetectPeBits(game, gameIs64, detectError)) is64 = gameIs64;
            }
            if (output.empty()) output = DirNameOf(source) + L"\\version.dll";
            LocalFree(argv);
            std::wstring message;
            return RunConvert(source, output, is64, message) ? 0 : 1;
        }
        if (argv) LocalFree(argv);
    }

    SetProcessDPIAware();
    {
        HDC screen = GetDC(nullptr);
        g_scale = GetDeviceCaps(screen, LOGPIXELSX) / 96.0;
        ReleaseDC(nullptr, screen);
        if (g_scale < 1.0) g_scale = 1.0;
    }

    INITCOMMONCONTROLSEX controls = {};
    controls.dwSize = sizeof(controls);
    controls.dwICC = ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&controls);

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"NepgearEmbedWindow";
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hIconSm = wc.hIcon;
    RegisterClassExW(&wc);

    RECT frame = {0, 0, Scale(700), Scale(522)};
    AdjustWindowRectEx(&frame, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE, 0);

    HWND hwnd = CreateWindowExW(
        0, wc.lpszClassName, L"Nepgear 补丁打包器    .chs  →  version.dll",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, frame.right - frame.left, frame.bottom - frame.top,
        nullptr, nullptr, instance, nullptr);
    if (!hwnd) return 1;

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    return 0;
}
