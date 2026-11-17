#pragma once

// LZMS 解码器（纯 C++，不依赖 cabinet.dll）
//
// 背景：Windows 8 起 LZMS 只能通过 cabinet.dll 的压缩 API 使用。Wine / Winlator
// 上那套 API 是空实现（CreateDecompressor 直接返回成功、Decompress 只做 memcpy），
// 于是容器里既转不了 .chs，DLL 运行时也解不开压缩过的归档。这里按 LZMS 的格式
// 自己实现一份解码器，Windows 与 Wine 走同一套代码。
//
// LZMS 没有官方文档；格式细节参考 wimlib（LGPL-2.1+）的 lzms_decompress.c 及其
// 注释，此处是独立的 C++ 重写，只保留算法规格本身。
//
// 数据流形态：一条从前往后的二进制区间编码流（range coder），一条从后往前的
// Huffman / verbatim 位流，两者都按 16 位小端单元读取、单元内高位在前。

#include <cstdint>
#include <cstring>
#include <new>

namespace LzmsNative {

namespace detail {

// ---------------------------------------------------------------- 格式常量

constexpr unsigned kProbBits = 6;
constexpr unsigned kProbDenominator = 1u << kProbBits;
constexpr uint32_t kInitialProbability = 48;
constexpr uint64_t kInitialRecentBits = 0x0000000055555555ull;

constexpr unsigned kNumMainProbs = 16;
constexpr unsigned kNumMatchProbs = 32;
constexpr unsigned kNumLzProbs = 64;
constexpr unsigned kNumLzRepProbs = 64;
constexpr unsigned kNumDeltaProbs = 64;
constexpr unsigned kNumDeltaRepProbs = 64;

constexpr unsigned kNumLzReps = 3;
constexpr unsigned kNumDeltaReps = 3;

constexpr unsigned kNumLiteralSyms = 256;
constexpr unsigned kNumLengthSyms = 54;
constexpr unsigned kNumDeltaPowerSyms = 8;
constexpr unsigned kMaxNumOffsetSyms = 799;
constexpr unsigned kMaxNumSyms = 799;

constexpr unsigned kLiteralRebuildFreq = 1024;
constexpr unsigned kLzOffsetRebuildFreq = 1024;
constexpr unsigned kLengthRebuildFreq = 512;
constexpr unsigned kDeltaOffsetRebuildFreq = 1024;
constexpr unsigned kDeltaPowerRebuildFreq = 512;

constexpr unsigned kMaxCodewordLength = 15;
constexpr uint32_t kMaxBufferSize = 1073741824u;

constexpr int32_t kX86IdWindowSize = 65535;
constexpr int32_t kX86MaxTranslationOffset = 1023;

// ---------------------------------------------------------------- 槽位表

// 偏移槽位表：offset slot => 基值（末项为哨兵，等于最大偏移 + 1）
static const uint32_t kOffsetSlotBase[800] = {
    0x00000001, 0x00000002, 0x00000003, 0x00000004, 0x00000005, 0x00000006, 0x00000007, 0x00000008,
    0x00000009, 0x0000000d, 0x00000011, 0x00000015, 0x00000019, 0x0000001d, 0x00000021, 0x00000025,
    0x00000029, 0x0000002d, 0x00000035, 0x0000003d, 0x00000045, 0x0000004d, 0x00000055, 0x0000005d,
    0x00000065, 0x00000075, 0x00000085, 0x00000095, 0x000000a5, 0x000000b5, 0x000000c5, 0x000000d5,
    0x000000e5, 0x000000f5, 0x00000105, 0x00000125, 0x00000145, 0x00000165, 0x00000185, 0x000001a5,
    0x000001c5, 0x000001e5, 0x00000205, 0x00000225, 0x00000245, 0x00000265, 0x00000285, 0x000002a5,
    0x000002c5, 0x000002e5, 0x00000325, 0x00000365, 0x000003a5, 0x000003e5, 0x00000425, 0x00000465,
    0x000004a5, 0x000004e5, 0x00000525, 0x00000565, 0x000005a5, 0x000005e5, 0x00000625, 0x00000665,
    0x000006a5, 0x00000725, 0x000007a5, 0x00000825, 0x000008a5, 0x00000925, 0x000009a5, 0x00000a25,
    0x00000aa5, 0x00000b25, 0x00000ba5, 0x00000c25, 0x00000ca5, 0x00000d25, 0x00000da5, 0x00000e25,
    0x00000ea5, 0x00000f25, 0x00000fa5, 0x00001025, 0x000010a5, 0x000011a5, 0x000012a5, 0x000013a5,
    0x000014a5, 0x000015a5, 0x000016a5, 0x000017a5, 0x000018a5, 0x000019a5, 0x00001aa5, 0x00001ba5,
    0x00001ca5, 0x00001da5, 0x00001ea5, 0x00001fa5, 0x000020a5, 0x000021a5, 0x000022a5, 0x000023a5,
    0x000024a5, 0x000026a5, 0x000028a5, 0x00002aa5, 0x00002ca5, 0x00002ea5, 0x000030a5, 0x000032a5,
    0x000034a5, 0x000036a5, 0x000038a5, 0x00003aa5, 0x00003ca5, 0x00003ea5, 0x000040a5, 0x000042a5,
    0x000044a5, 0x000046a5, 0x000048a5, 0x00004aa5, 0x00004ca5, 0x00004ea5, 0x000050a5, 0x000052a5,
    0x000054a5, 0x000056a5, 0x000058a5, 0x00005aa5, 0x00005ca5, 0x00005ea5, 0x000060a5, 0x000064a5,
    0x000068a5, 0x00006ca5, 0x000070a5, 0x000074a5, 0x000078a5, 0x00007ca5, 0x000080a5, 0x000084a5,
    0x000088a5, 0x00008ca5, 0x000090a5, 0x000094a5, 0x000098a5, 0x00009ca5, 0x0000a0a5, 0x0000a4a5,
    0x0000a8a5, 0x0000aca5, 0x0000b0a5, 0x0000b4a5, 0x0000b8a5, 0x0000bca5, 0x0000c0a5, 0x0000c4a5,
    0x0000c8a5, 0x0000cca5, 0x0000d0a5, 0x0000d4a5, 0x0000d8a5, 0x0000dca5, 0x0000e0a5, 0x0000e4a5,
    0x0000eca5, 0x0000f4a5, 0x0000fca5, 0x000104a5, 0x00010ca5, 0x000114a5, 0x00011ca5, 0x000124a5,
    0x00012ca5, 0x000134a5, 0x00013ca5, 0x000144a5, 0x00014ca5, 0x000154a5, 0x00015ca5, 0x000164a5,
    0x00016ca5, 0x000174a5, 0x00017ca5, 0x000184a5, 0x00018ca5, 0x000194a5, 0x00019ca5, 0x0001a4a5,
    0x0001aca5, 0x0001b4a5, 0x0001bca5, 0x0001c4a5, 0x0001cca5, 0x0001d4a5, 0x0001dca5, 0x0001e4a5,
    0x0001eca5, 0x0001f4a5, 0x0001fca5, 0x000204a5, 0x00020ca5, 0x000214a5, 0x00021ca5, 0x000224a5,
    0x000234a5, 0x000244a5, 0x000254a5, 0x000264a5, 0x000274a5, 0x000284a5, 0x000294a5, 0x0002a4a5,
    0x0002b4a5, 0x0002c4a5, 0x0002d4a5, 0x0002e4a5, 0x0002f4a5, 0x000304a5, 0x000314a5, 0x000324a5,
    0x000334a5, 0x000344a5, 0x000354a5, 0x000364a5, 0x000374a5, 0x000384a5, 0x000394a5, 0x0003a4a5,
    0x0003b4a5, 0x0003c4a5, 0x0003d4a5, 0x0003e4a5, 0x0003f4a5, 0x000404a5, 0x000414a5, 0x000424a5,
    0x000434a5, 0x000444a5, 0x000454a5, 0x000464a5, 0x000474a5, 0x000484a5, 0x000494a5, 0x0004a4a5,
    0x0004b4a5, 0x0004c4a5, 0x0004e4a5, 0x000504a5, 0x000524a5, 0x000544a5, 0x000564a5, 0x000584a5,
    0x0005a4a5, 0x0005c4a5, 0x0005e4a5, 0x000604a5, 0x000624a5, 0x000644a5, 0x000664a5, 0x000684a5,
    0x0006a4a5, 0x0006c4a5, 0x0006e4a5, 0x000704a5, 0x000724a5, 0x000744a5, 0x000764a5, 0x000784a5,
    0x0007a4a5, 0x0007c4a5, 0x0007e4a5, 0x000804a5, 0x000824a5, 0x000844a5, 0x000864a5, 0x000884a5,
    0x0008a4a5, 0x0008c4a5, 0x0008e4a5, 0x000904a5, 0x000924a5, 0x000944a5, 0x000964a5, 0x000984a5,
    0x0009a4a5, 0x0009c4a5, 0x0009e4a5, 0x000a04a5, 0x000a24a5, 0x000a44a5, 0x000a64a5, 0x000aa4a5,
    0x000ae4a5, 0x000b24a5, 0x000b64a5, 0x000ba4a5, 0x000be4a5, 0x000c24a5, 0x000c64a5, 0x000ca4a5,
    0x000ce4a5, 0x000d24a5, 0x000d64a5, 0x000da4a5, 0x000de4a5, 0x000e24a5, 0x000e64a5, 0x000ea4a5,
    0x000ee4a5, 0x000f24a5, 0x000f64a5, 0x000fa4a5, 0x000fe4a5, 0x001024a5, 0x001064a5, 0x0010a4a5,
    0x0010e4a5, 0x001124a5, 0x001164a5, 0x0011a4a5, 0x0011e4a5, 0x001224a5, 0x001264a5, 0x0012a4a5,
    0x0012e4a5, 0x001324a5, 0x001364a5, 0x0013a4a5, 0x0013e4a5, 0x001424a5, 0x001464a5, 0x0014a4a5,
    0x0014e4a5, 0x001524a5, 0x001564a5, 0x0015a4a5, 0x0015e4a5, 0x001624a5, 0x001664a5, 0x0016a4a5,
    0x0016e4a5, 0x001724a5, 0x001764a5, 0x0017a4a5, 0x0017e4a5, 0x001824a5, 0x001864a5, 0x0018a4a5,
    0x0018e4a5, 0x001924a5, 0x001964a5, 0x0019e4a5, 0x001a64a5, 0x001ae4a5, 0x001b64a5, 0x001be4a5,
    0x001c64a5, 0x001ce4a5, 0x001d64a5, 0x001de4a5, 0x001e64a5, 0x001ee4a5, 0x001f64a5, 0x001fe4a5,
    0x002064a5, 0x0020e4a5, 0x002164a5, 0x0021e4a5, 0x002264a5, 0x0022e4a5, 0x002364a5, 0x0023e4a5,
    0x002464a5, 0x0024e4a5, 0x002564a5, 0x0025e4a5, 0x002664a5, 0x0026e4a5, 0x002764a5, 0x0027e4a5,
    0x002864a5, 0x0028e4a5, 0x002964a5, 0x0029e4a5, 0x002a64a5, 0x002ae4a5, 0x002b64a5, 0x002be4a5,
    0x002c64a5, 0x002ce4a5, 0x002d64a5, 0x002de4a5, 0x002e64a5, 0x002ee4a5, 0x002f64a5, 0x002fe4a5,
    0x003064a5, 0x0030e4a5, 0x003164a5, 0x0031e4a5, 0x003264a5, 0x0032e4a5, 0x003364a5, 0x0033e4a5,
    0x003464a5, 0x0034e4a5, 0x003564a5, 0x0035e4a5, 0x003664a5, 0x0036e4a5, 0x003764a5, 0x0037e4a5,
    0x003864a5, 0x0038e4a5, 0x003964a5, 0x0039e4a5, 0x003a64a5, 0x003ae4a5, 0x003b64a5, 0x003be4a5,
    0x003c64a5, 0x003ce4a5, 0x003d64a5, 0x003de4a5, 0x003ee4a5, 0x003fe4a5, 0x0040e4a5, 0x0041e4a5,
    0x0042e4a5, 0x0043e4a5, 0x0044e4a5, 0x0045e4a5, 0x0046e4a5, 0x0047e4a5, 0x0048e4a5, 0x0049e4a5,
    0x004ae4a5, 0x004be4a5, 0x004ce4a5, 0x004de4a5, 0x004ee4a5, 0x004fe4a5, 0x0050e4a5, 0x0051e4a5,
    0x0052e4a5, 0x0053e4a5, 0x0054e4a5, 0x0055e4a5, 0x0056e4a5, 0x0057e4a5, 0x0058e4a5, 0x0059e4a5,
    0x005ae4a5, 0x005be4a5, 0x005ce4a5, 0x005de4a5, 0x005ee4a5, 0x005fe4a5, 0x0060e4a5, 0x0061e4a5,
    0x0062e4a5, 0x0063e4a5, 0x0064e4a5, 0x0065e4a5, 0x0066e4a5, 0x0067e4a5, 0x0068e4a5, 0x0069e4a5,
    0x006ae4a5, 0x006be4a5, 0x006ce4a5, 0x006de4a5, 0x006ee4a5, 0x006fe4a5, 0x0070e4a5, 0x0071e4a5,
    0x0072e4a5, 0x0073e4a5, 0x0074e4a5, 0x0075e4a5, 0x0076e4a5, 0x0077e4a5, 0x0078e4a5, 0x0079e4a5,
    0x007ae4a5, 0x007be4a5, 0x007ce4a5, 0x007de4a5, 0x007ee4a5, 0x007fe4a5, 0x0080e4a5, 0x0081e4a5,
    0x0082e4a5, 0x0083e4a5, 0x0084e4a5, 0x0085e4a5, 0x0086e4a5, 0x0087e4a5, 0x0088e4a5, 0x0089e4a5,
    0x008ae4a5, 0x008be4a5, 0x008ce4a5, 0x008de4a5, 0x008fe4a5, 0x0091e4a5, 0x0093e4a5, 0x0095e4a5,
    0x0097e4a5, 0x0099e4a5, 0x009be4a5, 0x009de4a5, 0x009fe4a5, 0x00a1e4a5, 0x00a3e4a5, 0x00a5e4a5,
    0x00a7e4a5, 0x00a9e4a5, 0x00abe4a5, 0x00ade4a5, 0x00afe4a5, 0x00b1e4a5, 0x00b3e4a5, 0x00b5e4a5,
    0x00b7e4a5, 0x00b9e4a5, 0x00bbe4a5, 0x00bde4a5, 0x00bfe4a5, 0x00c1e4a5, 0x00c3e4a5, 0x00c5e4a5,
    0x00c7e4a5, 0x00c9e4a5, 0x00cbe4a5, 0x00cde4a5, 0x00cfe4a5, 0x00d1e4a5, 0x00d3e4a5, 0x00d5e4a5,
    0x00d7e4a5, 0x00d9e4a5, 0x00dbe4a5, 0x00dde4a5, 0x00dfe4a5, 0x00e1e4a5, 0x00e3e4a5, 0x00e5e4a5,
    0x00e7e4a5, 0x00e9e4a5, 0x00ebe4a5, 0x00ede4a5, 0x00efe4a5, 0x00f1e4a5, 0x00f3e4a5, 0x00f5e4a5,
    0x00f7e4a5, 0x00f9e4a5, 0x00fbe4a5, 0x00fde4a5, 0x00ffe4a5, 0x0101e4a5, 0x0103e4a5, 0x0105e4a5,
    0x0107e4a5, 0x0109e4a5, 0x010be4a5, 0x010de4a5, 0x010fe4a5, 0x0111e4a5, 0x0113e4a5, 0x0115e4a5,
    0x0117e4a5, 0x0119e4a5, 0x011be4a5, 0x011de4a5, 0x011fe4a5, 0x0121e4a5, 0x0123e4a5, 0x0125e4a5,
    0x0127e4a5, 0x0129e4a5, 0x012be4a5, 0x012de4a5, 0x012fe4a5, 0x0131e4a5, 0x0133e4a5, 0x0135e4a5,
    0x0137e4a5, 0x013be4a5, 0x013fe4a5, 0x0143e4a5, 0x0147e4a5, 0x014be4a5, 0x014fe4a5, 0x0153e4a5,
    0x0157e4a5, 0x015be4a5, 0x015fe4a5, 0x0163e4a5, 0x0167e4a5, 0x016be4a5, 0x016fe4a5, 0x0173e4a5,
    0x0177e4a5, 0x017be4a5, 0x017fe4a5, 0x0183e4a5, 0x0187e4a5, 0x018be4a5, 0x018fe4a5, 0x0193e4a5,
    0x0197e4a5, 0x019be4a5, 0x019fe4a5, 0x01a3e4a5, 0x01a7e4a5, 0x01abe4a5, 0x01afe4a5, 0x01b3e4a5,
    0x01b7e4a5, 0x01bbe4a5, 0x01bfe4a5, 0x01c3e4a5, 0x01c7e4a5, 0x01cbe4a5, 0x01cfe4a5, 0x01d3e4a5,
    0x01d7e4a5, 0x01dbe4a5, 0x01dfe4a5, 0x01e3e4a5, 0x01e7e4a5, 0x01ebe4a5, 0x01efe4a5, 0x01f3e4a5,
    0x01f7e4a5, 0x01fbe4a5, 0x01ffe4a5, 0x0203e4a5, 0x0207e4a5, 0x020be4a5, 0x020fe4a5, 0x0213e4a5,
    0x0217e4a5, 0x021be4a5, 0x021fe4a5, 0x0223e4a5, 0x0227e4a5, 0x022be4a5, 0x022fe4a5, 0x0233e4a5,
    0x0237e4a5, 0x023be4a5, 0x023fe4a5, 0x0243e4a5, 0x0247e4a5, 0x024be4a5, 0x024fe4a5, 0x0253e4a5,
    0x0257e4a5, 0x025be4a5, 0x025fe4a5, 0x0263e4a5, 0x0267e4a5, 0x026be4a5, 0x026fe4a5, 0x0273e4a5,
    0x0277e4a5, 0x027be4a5, 0x027fe4a5, 0x0283e4a5, 0x0287e4a5, 0x028be4a5, 0x028fe4a5, 0x0293e4a5,
    0x0297e4a5, 0x029be4a5, 0x029fe4a5, 0x02a3e4a5, 0x02a7e4a5, 0x02abe4a5, 0x02afe4a5, 0x02b3e4a5,
    0x02bbe4a5, 0x02c3e4a5, 0x02cbe4a5, 0x02d3e4a5, 0x02dbe4a5, 0x02e3e4a5, 0x02ebe4a5, 0x02f3e4a5,
    0x02fbe4a5, 0x0303e4a5, 0x030be4a5, 0x0313e4a5, 0x031be4a5, 0x0323e4a5, 0x032be4a5, 0x0333e4a5,
    0x033be4a5, 0x0343e4a5, 0x034be4a5, 0x0353e4a5, 0x035be4a5, 0x0363e4a5, 0x036be4a5, 0x0373e4a5,
    0x037be4a5, 0x0383e4a5, 0x038be4a5, 0x0393e4a5, 0x039be4a5, 0x03a3e4a5, 0x03abe4a5, 0x03b3e4a5,
    0x03bbe4a5, 0x03c3e4a5, 0x03cbe4a5, 0x03d3e4a5, 0x03dbe4a5, 0x03e3e4a5, 0x03ebe4a5, 0x03f3e4a5,
    0x03fbe4a5, 0x0403e4a5, 0x040be4a5, 0x0413e4a5, 0x041be4a5, 0x0423e4a5, 0x042be4a5, 0x0433e4a5,
    0x043be4a5, 0x0443e4a5, 0x044be4a5, 0x0453e4a5, 0x045be4a5, 0x0463e4a5, 0x046be4a5, 0x0473e4a5,
    0x047be4a5, 0x0483e4a5, 0x048be4a5, 0x0493e4a5, 0x049be4a5, 0x04a3e4a5, 0x04abe4a5, 0x04b3e4a5,
    0x04bbe4a5, 0x04c3e4a5, 0x04cbe4a5, 0x04d3e4a5, 0x04dbe4a5, 0x04e3e4a5, 0x04ebe4a5, 0x04f3e4a5,
    0x04fbe4a5, 0x0503e4a5, 0x050be4a5, 0x0513e4a5, 0x051be4a5, 0x0523e4a5, 0x052be4a5, 0x0533e4a5,
    0x053be4a5, 0x0543e4a5, 0x054be4a5, 0x0553e4a5, 0x055be4a5, 0x0563e4a5, 0x056be4a5, 0x0573e4a5,
    0x057be4a5, 0x0583e4a5, 0x058be4a5, 0x0593e4a5, 0x059be4a5, 0x05a3e4a5, 0x05abe4a5, 0x05b3e4a5,
    0x05bbe4a5, 0x05c3e4a5, 0x05cbe4a5, 0x05d3e4a5, 0x05dbe4a5, 0x05e3e4a5, 0x05ebe4a5, 0x05f3e4a5,
    0x05fbe4a5, 0x060be4a5, 0x061be4a5, 0x062be4a5, 0x063be4a5, 0x064be4a5, 0x065be4a5, 0x465be4a5,
};

// 偏移槽位表：offset slot => 额外的 verbatim 位数
static const uint8_t kExtraOffsetBits[799] = {
    0, 0, 0, 0, 0, 0, 0, 0, 2, 2, 2, 2, 2, 2, 2, 2,
    2, 3, 3, 3, 3, 3, 3, 3, 4, 4, 4, 4, 4, 4, 4, 4,
    4, 4, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5,
    5, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6,
    7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
    7, 7, 7, 7, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8,
    8, 8, 8, 8, 8, 8, 8, 8, 9, 9, 9, 9, 9, 9, 9, 9,
    9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9,
    9, 9, 9, 9, 9, 9, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10,
    10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10,
    10, 10, 10, 10, 10, 10, 10, 11, 11, 11, 11, 11, 11, 11, 11, 11,
    11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11,
    11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 12,
    12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12,
    12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12,
    12, 12, 12, 12, 12, 12, 12, 12, 12, 13, 13, 13, 13, 13, 13, 13,
    13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13,
    13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13,
    13, 13, 13, 13, 13, 13, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14,
    14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14,
    14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14,
    14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14,
    14, 14, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15,
    15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15,
    15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15,
    15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15,
    15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 16, 16, 16, 16, 16,
    16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16,
    16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16,
    16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16,
    16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16,
    16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 17, 17, 17, 17, 17,
    17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17,
    17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17,
    17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17,
    17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17,
    17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17,
    18, 18, 18, 18, 18, 18, 18, 18, 18, 18, 18, 18, 18, 18, 18, 18,
    18, 18, 18, 18, 18, 18, 18, 18, 18, 18, 18, 18, 18, 18, 18, 18,
    18, 18, 18, 18, 18, 18, 18, 18, 18, 18, 18, 18, 18, 18, 18, 18,
    18, 18, 18, 18, 18, 18, 18, 18, 18, 18, 18, 18, 18, 18, 18, 18,
    18, 18, 18, 18, 18, 18, 18, 18, 18, 18, 18, 18, 18, 18, 18, 18,
    18, 18, 18, 18, 18, 18, 18, 18, 18, 18, 18, 18, 18, 18, 18, 19,
    19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19,
    19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19,
    19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19,
    19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19,
    19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19,
    19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19,
    19, 19, 19, 19, 19, 19, 19, 19, 20, 20, 20, 20, 20, 20, 30,
};

// 长度槽位表：length slot => 基值（末项为哨兵）
static const uint32_t kLengthSlotBase[55] = {
    0x00000001, 0x00000002, 0x00000003, 0x00000004, 0x00000005, 0x00000006, 0x00000007, 0x00000008,
    0x00000009, 0x0000000a, 0x0000000b, 0x0000000c, 0x0000000d, 0x0000000e, 0x0000000f, 0x00000010,
    0x00000011, 0x00000012, 0x00000013, 0x00000014, 0x00000015, 0x00000016, 0x00000017, 0x00000018,
    0x00000019, 0x0000001a, 0x0000001b, 0x0000001d, 0x0000001f, 0x00000021, 0x00000023, 0x00000027,
    0x0000002b, 0x0000002f, 0x00000033, 0x00000037, 0x0000003b, 0x00000043, 0x0000004b, 0x00000053,
    0x0000005b, 0x0000006b, 0x0000007b, 0x0000008b, 0x0000009b, 0x000000ab, 0x000000cb, 0x000000eb,
    0x0000012b, 0x000001ab, 0x000002ab, 0x000004ab, 0x000008ab, 0x000108ab, 0x400108ab,
};

// 长度槽位表：length slot => 额外的 verbatim 位数
static const uint8_t kExtraLengthBits[54] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2,
    2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 4, 5, 5, 6,
    7, 8, 9, 10, 16, 30,
};

// ---------------------------------------------------------------- 位流

inline uint32_t ReadLe16(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

inline uint32_t ReadLe32(const uint8_t* p) {
    uint32_t v = 0;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

inline void WriteLe32(uint8_t* p, uint32_t v) {
    std::memcpy(p, &v, sizeof(v));
}

inline uint64_t ReadLe64(const uint8_t* p) {
    uint64_t v = 0;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

// 反向位流：从缓冲区尾部往前，每次取一个 16 位小端单元，单元内高位在前。
class BackwardBits {
public:
    void Init(const uint8_t* begin, size_t size) {
        begin_ = begin;
        next_ = begin + size;
        buf_ = 0;
        count_ = 0;
    }

    void Ensure(unsigned numBits) {
        while (count_ < numBits && count_ + 16 <= 64 && (next_ - begin_) >= 2) {
            next_ -= 2;
            const uint64_t unit = ReadLe16(next_);
            buf_ |= unit << (48 - count_);
            count_ += 16;
        }
        // 数据不足时按 0 补齐：buf_ 的低位本来就是 0，这里只需让计数够用。
        if (count_ < numBits) count_ = numBits;
    }

    uint32_t Peek(unsigned numBits) const {
        if (!numBits) return 0;
        return (uint32_t)(buf_ >> (64 - numBits));
    }

    void Remove(unsigned numBits) {
        buf_ <<= numBits;
        count_ -= numBits;
    }

    uint32_t Read(unsigned numBits) {
        Ensure(numBits);
        const uint32_t bits = Peek(numBits);
        Remove(numBits);
        return bits;
    }

private:
    const uint8_t* begin_ = nullptr;
    const uint8_t* next_ = nullptr;
    uint64_t buf_ = 0;
    unsigned count_ = 0;
};

// 正向的区间解码器
class RangeDecoder {
public:
    void Init(const uint8_t* in, size_t size) {
        range_ = 0xFFFFFFFFu;
        code_ = (ReadLe16(in) << 16) | ReadLe16(in + 2);
        next_ = in + 4;
        end_ = in + size;
    }

    template <typename Probabilities>
    int DecodeBit(uint32_t* state, uint32_t numStates, Probabilities& probs) {
        // 取出当前状态对应的概率条目，并先把状态左移（若解出 1 再补低位）
        uint32_t& zeroBits = probs[*state].zeroBits;
        uint64_t& recentBits = probs[*state].recentBits;
        *state = (*state << 1) & (numStates - 1);

        uint32_t prob = zeroBits;
        prob += (uint32_t)(prob - 1) >> 31;   // 0/64 -> 1/64
        prob -= (prob >> kProbBits);          // 64/64 -> 63/64

        if (!(range_ & 0xFFFF0000u)) {
            range_ <<= 16;
            code_ <<= 16;
            if (next_ != end_) {
                code_ |= ReadLe16(next_);
                next_ += 2;
            }
        }

        const uint32_t bound = (range_ >> kProbBits) * prob;
        int bit;
        if (code_ < bound) {
            range_ = bound;
            bit = 0;
        } else {
            range_ -= bound;
            code_ -= bound;
            bit = 1;
            *state |= 1;
        }

        const int32_t delta = (int32_t)(recentBits >> (kProbDenominator - 1)) - bit;
        zeroBits = (uint32_t)((int32_t)zeroBits + delta);
        recentBits = (recentBits << 1) | (uint64_t)bit;
        return bit;
    }

private:
    uint32_t range_ = 0;
    uint32_t code_ = 0;
    const uint8_t* next_ = nullptr;
    const uint8_t* end_ = nullptr;
};

// ---------------------------------------------------------------- Huffman

struct ProbabilityEntry {
    uint32_t zeroBits;
    uint64_t recentBits;
};

struct ProbabilityTable {
    void Init() {
        ProbabilityEntry* all = &main[0];
        const size_t total = sizeof(ProbabilityTable) / sizeof(ProbabilityEntry);
        for (size_t i = 0; i < total; ++i) {
            all[i].zeroBits = kInitialProbability;
            all[i].recentBits = kInitialRecentBits;
        }
    }

    ProbabilityEntry main[kNumMainProbs];
    ProbabilityEntry match[kNumMatchProbs];
    ProbabilityEntry lz[kNumLzProbs];
    ProbabilityEntry delta[kNumDeltaProbs];
    ProbabilityEntry lzRep[kNumLzReps - 1][kNumLzRepProbs];
    ProbabilityEntry deltaRep[kNumDeltaReps - 1][kNumDeltaRepProbs];
};

// 自适应 Huffman 码：每解若干个符号重建一次，重建后频率减半。
//
// 码字按 LZMS 的规定生成：符号按 (频率升序, 符号值升序) 排序，用优先队列建树且
// 同频时优先合并叶子；码长从长到短依次分配给排序后的符号，最后按符号值顺序生成
// 规范码字。
class HuffmanCode {
public:
    void Init(unsigned numSyms, unsigned rebuildFreq) {
        numSyms_ = numSyms;
        rebuildFreq_ = rebuildFreq;
        for (unsigned i = 0; i < numSyms_; ++i) freqs_[i] = 1;
        Build();
    }

    // 解一个符号，失败返回 -1。
    int Decode(BackwardBits& bits) {
        uint32_t code = 0;
        for (unsigned len = 1; len <= kMaxCodewordLength; ++len) {
            code = (code << 1) | bits.Read(1);
            const uint32_t count = lenCounts_[len];
            if (!count) continue;
            const uint32_t first = firstCode_[len];
            if (code >= first && code - first < count) {
                const unsigned symbol = ordered_[base_[len] + (code - first)];
                if (symbol >= numSyms_) return -1;
                ++freqs_[symbol];
                if (--untilRebuild_ == 0) Rebuild();
                return (int)symbol;
            }
        }
        return -1;
    }

private:
    void Rebuild() {
        Build();
        for (unsigned i = 0; i < numSyms_; ++i) freqs_[i] = (freqs_[i] >> 1) + 1;
    }

    void Build() {
        // 1. 按 (频率, 符号值) 升序排序（频率恒 >= 1，不会有空符号）
        for (unsigned i = 0; i < numSyms_; ++i) sorted_[i] = i;
        for (unsigned i = 1; i < numSyms_; ++i) {  // 插入排序，规模 <= 799
            const unsigned sym = sorted_[i];
            const uint32_t freq = freqs_[sym];
            unsigned j = i;
            while (j > 0 && (freqs_[sorted_[j - 1]] > freq || (freqs_[sorted_[j - 1]] == freq && sorted_[j - 1] > sym))) {
                sorted_[j] = sorted_[j - 1];
                --j;
            }
            sorted_[j] = sym;
        }

        // 2. 建树：叶子 0..n-1 已按频率升序，内部节点按生成顺序编号
        const unsigned n = numSyms_;
        for (unsigned i = 0; i < n; ++i) leafFreq_[i] = freqs_[sorted_[i]];

        const unsigned lastLeaf = n - 1;
        unsigned i = 0;   // 下一个待合并的叶子
        unsigned b = 0;   // 下一个待合并的内部节点
        unsigned e = 0;   // 下一个可用的内部节点槽位
        do {
            uint32_t newFreq;
            if (i + 1 <= lastLeaf && (b == e || leafFreq_[i + 1] <= nodeFreq_[b])) {
                newFreq = leafFreq_[i] + leafFreq_[i + 1];
                i += 2;
            } else if (b + 2 <= e && (i > lastLeaf || nodeFreq_[b + 1] < leafFreq_[i])) {
                newFreq = nodeFreq_[b] + nodeFreq_[b + 1];
                nodeParent_[b] = (uint16_t)e;
                nodeParent_[b + 1] = (uint16_t)e;
                b += 2;
            } else {
                newFreq = leafFreq_[i] + nodeFreq_[b];
                nodeParent_[b] = (uint16_t)e;
                ++i;
                ++b;
            }
            nodeFreq_[e] = newFreq;
        } while (++e < lastLeaf);

        // 3. 统计每种码长的码字数量
        for (unsigned len = 0; len <= kMaxCodewordLength; ++len) lenCounts_[len] = 0;
        lenCounts_[1] = 2;
        const unsigned root = n - 2;
        nodeDepth_[root] = 0;
        for (int node = (int)root - 1; node >= 0; --node) {
            const unsigned parent = nodeParent_[node];
            const unsigned depth = nodeDepth_[parent] + 1;
            unsigned len = depth;
            if (len >= kMaxCodewordLength) {
                len = kMaxCodewordLength;
                do {
                    --len;
                } while (len > 0 && lenCounts_[len] == 0);
            }
            --lenCounts_[len];
            lenCounts_[len + 1] += 2;
            nodeDepth_[node] = depth;
        }

        // 4. 码长按从长到短分配给排序后的符号
        unsigned idx = 0;
        for (int len = (int)kMaxCodewordLength; len >= 1; --len) {
            unsigned count = lenCounts_[len];
            while (count--) lens_[sorted_[idx++]] = (uint8_t)len;
        }

        // 5. 生成规范码字
        firstCode_[0] = 0;
        firstCode_[1] = 0;
        for (unsigned len = 2; len <= kMaxCodewordLength; ++len)
            firstCode_[len] = (firstCode_[len - 1] + lenCounts_[len - 1]) << 1;

        uint32_t nextCode[kMaxCodewordLength + 1];
        for (unsigned len = 0; len <= kMaxCodewordLength; ++len)
            nextCode[len] = firstCode_[len];
        for (unsigned sym = 0; sym < n; ++sym) codes_[sym] = nextCode[lens_[sym]]++;

        // 6. 解码用的按 (码长, 符号值) 有序表
        unsigned pos = 0;
        for (unsigned len = 1; len <= kMaxCodewordLength; ++len) {
            base_[len] = pos;
            for (unsigned sym = 0; sym < n; ++sym) {
                if (lens_[sym] == len) ordered_[pos++] = sym;
            }
        }

        untilRebuild_ = rebuildFreq_;
    }

    unsigned numSyms_ = 0;
    unsigned rebuildFreq_ = 0;
    unsigned untilRebuild_ = 0;
    uint32_t freqs_[kMaxNumSyms] = {};
    uint32_t leafFreq_[kMaxNumSyms] = {};
    uint32_t nodeFreq_[kMaxNumSyms] = {};
    uint32_t nodeDepth_[kMaxNumSyms] = {};
    uint16_t nodeParent_[kMaxNumSyms] = {};
    unsigned sorted_[kMaxNumSyms] = {};
    unsigned ordered_[kMaxNumSyms] = {};
    uint8_t lens_[kMaxNumSyms] = {};
    uint32_t codes_[kMaxNumSyms] = {};
    uint32_t lenCounts_[kMaxCodewordLength + 2] = {};
    uint32_t firstCode_[kMaxCodewordLength + 1] = {};
    uint32_t base_[kMaxCodewordLength + 1] = {};
};

// 诊断用：解码失败时的退出位置与已产出字节数（只在调试入口里读取）
inline int& LastErrorCode() { static int code = 0; return code; }
inline size_t& LastProgress() { static size_t progress = 0; return progress; }

// ---------------------------------------------------------------- 槽位换算

inline unsigned GetSlot(uint32_t value, const uint32_t* slotBase, unsigned numSlots) {
    unsigned l = 0;
    unsigned r = numSlots - 1;
    for (;;) {
        const unsigned slot = (l + r) / 2;
        if (value >= slotBase[slot]) {
            if (value < slotBase[slot + 1]) return slot;
            l = slot + 1;
        } else {
            if (slot == 0) return 0;
            r = slot - 1;
        }
    }
}

inline unsigned GetNumOffsetSlots(size_t uncompressedSize) {
    if (uncompressedSize < 2) return 0;
    return 1 + GetSlot((uint32_t)(uncompressedSize - 1), kOffsetSlotBase, kMaxNumOffsetSyms);
}

// ---------------------------------------------------------------- x86 过滤器

inline const uint8_t* FindNextOpcode(const uint8_t* p) {
    for (;;) {
        const uint8_t b = *p;
        if (b == 0x48 || b == 0x4C || b == 0xE8 || b == 0xE9 || b == 0xF0 || b == 0xFF) break;
        ++p;
    }
    return p;
}

// 把 x86 指令里的相对地址翻译成绝对地址（undo=false），或反过来（undo=true）。
inline const uint8_t* TranslateIfNeeded(uint8_t* data, uint8_t* p, int32_t* lastX86Pos,
                                        int32_t* lastTargetUsages, bool undo) {
    int32_t maxTransOffset = kX86MaxTranslationOffset;
    int32_t opcodeNBytes = 0;
    int32_t i = 0;
    uint16_t target16 = 0;

    if (p[0] >= 0xF0) {
        if (p[0] & 0x0F) {
            if (p[1] == 0x15) {
                opcodeNBytes = 2;
                goto haveOpcode;
            }
        } else {
            if (p[1] == 0x83 && p[2] == 0x05) {
                opcodeNBytes = 3;
                goto haveOpcode;
            }
        }
    } else if (p[0] <= 0x4C) {
        if ((p[2] & 0x07) == 0x05) {
            if (p[1] == 0x8D || (p[1] == 0x8B && !(p[0] & 0x04) && !(p[2] & 0xF0))) {
                opcodeNBytes = 3;
                goto haveOpcode;
            }
        }
    } else {
        if (p[0] & 0x01) {
            p += 4;   // 0xE9：不翻译
        } else {
            opcodeNBytes = 1;
            maxTransOffset >>= 1;
            goto haveOpcode;
        }
    }
    return p + 1;

haveOpcode:
    i = (int32_t)(p - data);
    p += opcodeNBytes;
    if (undo) {
        if (i - *lastX86Pos <= maxTransOffset) {
            const uint32_t n = ReadLe32(p);
            WriteLe32(p, n - (uint32_t)i);
        }
        target16 = (uint16_t)(i + (int32_t)ReadLe16(p));
    } else {
        target16 = (uint16_t)(i + (int32_t)ReadLe16(p));
        if (i - *lastX86Pos <= maxTransOffset) {
            const uint32_t n = ReadLe32(p);
            WriteLe32(p, n + (uint32_t)i);
        }
    }

    i += opcodeNBytes + 4 - 1;
    if (i - lastTargetUsages[target16] <= kX86IdWindowSize) *lastX86Pos = i;
    lastTargetUsages[target16] = i;
    return p + 4;
}

inline void X86Filter(uint8_t* data, int32_t size, int32_t* lastTargetUsages, bool undo) {
    if (size <= 17) return;

    for (int32_t i = 0; i < 65536; ++i) lastTargetUsages[i] = -kX86IdWindowSize - 1;

    int32_t lastX86Pos = -kX86MaxTranslationOffset - 1;

    // 缓冲区末尾 16 字节内不可能有可翻译的指令，先覆写一个字节当哨兵，事后还原。
    uint8_t* p = data + 1;
    uint8_t* tailPtr = data + size - 16;
    const uint8_t saved = tailPtr[8];
    tailPtr[8] = 0xE8;
    for (;;) {
        p = const_cast<uint8_t*>(FindNextOpcode(p));
        if (p >= tailPtr) break;
        p = const_cast<uint8_t*>(TranslateIfNeeded(data, p, &lastX86Pos, lastTargetUsages, undo));
    }
    tailPtr[8] = saved;
}

// ---------------------------------------------------------------- 解码状态

struct State {
    ProbabilityTable probs;

    HuffmanCode literal;
    HuffmanCode lzOffset;
    HuffmanCode length;
    HuffmanCode deltaOffset;
    HuffmanCode deltaPower;

    void InitCards(size_t outSize) {
        const unsigned numOffsetSlots = GetNumOffsetSlots(outSize);
        literal.Init(kNumLiteralSyms, kLiteralRebuildFreq);
        lzOffset.Init(numOffsetSlots, kLzOffsetRebuildFreq);
        length.Init(kNumLengthSyms, kLengthRebuildFreq);
        deltaOffset.Init(numOffsetSlots, kDeltaOffsetRebuildFreq);
        deltaPower.Init(kNumDeltaPowerSyms, kDeltaPowerRebuildFreq);
    }
};

} // namespace detail

// 解一个 LZMS 块。成功返回 true。
inline bool DecompressBlock(const void* in, size_t inSize, void* out, size_t outSize,
                            int32_t* lastTargetUsages) {
    using namespace detail;

    if ((inSize & 1) || inSize < 4) { LastErrorCode() = __LINE__; return false; }
    if (outSize == 0 || outSize > (size_t)kMaxBufferSize) { LastErrorCode() = __LINE__; return false; }

    uint8_t* const outBegin = static_cast<uint8_t*>(out);
    uint8_t* outNext = outBegin;
    uint8_t* const outEnd = outBegin + outSize;

    BackwardBits bits;
    bits.Init(static_cast<const uint8_t*>(in), inSize);

    RangeDecoder rd;
    rd.Init(static_cast<const uint8_t*>(in), inSize);

    State state;
    state.probs.Init();
    state.InitCards(outSize);

    // 三个最近使用的 LZ 偏移，以及三个最近的 delta (power, raw offset) 对。
    // 队列多留一格用于"延迟插入"。
    uint32_t recentLzOffsets[kNumLzReps + 1];
    uint64_t recentDeltaPairs[kNumDeltaReps + 1];

    // 上一项类型：0 字面量，1 LZ 匹配，2 delta 匹配。用于处理延迟插入。
    unsigned prevItemType = 0;

    uint32_t mainState = 0;
    uint32_t matchState = 0;
    uint32_t lzState = 0;
    uint32_t deltaState = 0;
    uint32_t lzRepStates[kNumLzReps - 1] = {};
    uint32_t deltaRepStates[kNumDeltaReps - 1] = {};

    for (int i = 0; i < (int)kNumLzReps + 1; ++i) recentLzOffsets[i] = (uint32_t)i + 1;
    for (int i = 0; i < (int)kNumDeltaReps + 1; ++i) recentDeltaPairs[i] = (uint64_t)i + 1;

    while (outNext != outEnd) {
        detail::LastProgress() = (size_t)(outNext - outBegin);
        if (!rd.DecodeBit(&mainState, kNumMainProbs, state.probs.main)) {
            // 字面量
            const int literal = state.literal.Decode(bits);
            if (literal < 0) { LastErrorCode() = __LINE__; return false; }
            *outNext++ = (uint8_t)literal;
            prevItemType = 0;
            continue;
        }

        if (!rd.DecodeBit(&matchState, kNumMatchProbs, state.probs.match)) {
            // LZ 匹配
            uint32_t offset;
            if (!rd.DecodeBit(&lzState, kNumLzProbs, state.probs.lz)) {
                // 显式偏移
                int slot = state.lzOffset.Decode(bits);
                if (slot < 0) { LastErrorCode() = __LINE__; return false; }
                offset = kOffsetSlotBase[slot] + bits.Read(kExtraOffsetBits[slot]);
                recentLzOffsets[3] = recentLzOffsets[2];
                recentLzOffsets[2] = recentLzOffsets[1];
                recentLzOffsets[1] = recentLzOffsets[0];
            } else if (!rd.DecodeBit(&lzRepStates[0], kNumLzRepProbs, state.probs.lzRep[0])) {
                offset = recentLzOffsets[0 + (prevItemType & 1)];
                recentLzOffsets[0 + (prevItemType & 1)] = recentLzOffsets[0];
            } else if (!rd.DecodeBit(&lzRepStates[1], kNumLzRepProbs, state.probs.lzRep[1])) {
                offset = recentLzOffsets[1 + (prevItemType & 1)];
                recentLzOffsets[1 + (prevItemType & 1)] = recentLzOffsets[1];
                recentLzOffsets[1] = recentLzOffsets[0];
            } else {
                offset = recentLzOffsets[2 + (prevItemType & 1)];
                recentLzOffsets[2 + (prevItemType & 1)] = recentLzOffsets[2];
                recentLzOffsets[2] = recentLzOffsets[1];
                recentLzOffsets[1] = recentLzOffsets[0];
            }
            recentLzOffsets[0] = offset;
            prevItemType = 1;

            int slot = state.length.Decode(bits);
            if (slot < 0) { LastErrorCode() = __LINE__; return false; }
            uint32_t length = kLengthSlotBase[slot];
            const unsigned extra = kExtraLengthBits[slot];
            if (extra) length += bits.Read(extra);

            if (offset > (uint32_t)(outNext - outBegin)) { LastErrorCode() = __LINE__; return false; }
            if (length > (uint32_t)(outEnd - outNext)) { LastErrorCode() = __LINE__; return false; }
            const uint8_t* src = outNext - offset;
            for (uint32_t k = 0; k < length; ++k) outNext[k] = src[k];
            outNext += length;
            continue;
        }

        // delta 匹配
        uint32_t power;
        uint32_t rawOffset;
        uint64_t pair;
        if (!rd.DecodeBit(&deltaState, kNumDeltaProbs, state.probs.delta)) {
            const int powerSym = state.deltaPower.Decode(bits);
            if (powerSym < 0) { LastErrorCode() = __LINE__; return false; }
            power = (uint32_t)powerSym;
            const int slot = state.deltaOffset.Decode(bits);
            if (slot < 0) { LastErrorCode() = __LINE__; return false; }
            rawOffset = kOffsetSlotBase[slot] + bits.Read(kExtraOffsetBits[slot]);

            pair = ((uint64_t)power << 32) | rawOffset;
            recentDeltaPairs[3] = recentDeltaPairs[2];
            recentDeltaPairs[2] = recentDeltaPairs[1];
            recentDeltaPairs[1] = recentDeltaPairs[0];
        } else if (!rd.DecodeBit(&deltaRepStates[0], kNumDeltaRepProbs, state.probs.deltaRep[0])) {
            pair = recentDeltaPairs[0 + (prevItemType >> 1)];
            recentDeltaPairs[0 + (prevItemType >> 1)] = recentDeltaPairs[0];
            power = (uint32_t)(pair >> 32);
            rawOffset = (uint32_t)pair;
        } else if (!rd.DecodeBit(&deltaRepStates[1], kNumDeltaRepProbs, state.probs.deltaRep[1])) {
            pair = recentDeltaPairs[1 + (prevItemType >> 1)];
            recentDeltaPairs[1 + (prevItemType >> 1)] = recentDeltaPairs[1];
            recentDeltaPairs[1] = recentDeltaPairs[0];
            power = (uint32_t)(pair >> 32);
            rawOffset = (uint32_t)pair;
        } else {
            pair = recentDeltaPairs[2 + (prevItemType >> 1)];
            recentDeltaPairs[2 + (prevItemType >> 1)] = recentDeltaPairs[2];
            recentDeltaPairs[2] = recentDeltaPairs[1];
            recentDeltaPairs[1] = recentDeltaPairs[0];
            power = (uint32_t)(pair >> 32);
            rawOffset = (uint32_t)pair;
        }
        recentDeltaPairs[0] = pair;
        prevItemType = 2;

        int slot = state.length.Decode(bits);
        if (slot < 0) { LastErrorCode() = __LINE__; return false; }
        uint32_t length = kLengthSlotBase[slot];
        const unsigned extra = kExtraLengthBits[slot];
        if (extra) length += bits.Read(extra);

        if (power > 31) { LastErrorCode() = __LINE__; return false; }
        const uint32_t span = 1u << power;
        const uint32_t offset = rawOffset << power;
        if ((offset >> power) != rawOffset) { LastErrorCode() = __LINE__; return false; }
        if ((uint64_t)offset + span > (uint64_t)(outNext - outBegin)) { LastErrorCode() = __LINE__; return false; }
        if (length > (uint32_t)(outEnd - outNext)) { LastErrorCode() = __LINE__; return false; }

        const uint8_t* matchPtr = outNext - offset;
        for (uint32_t k = 0; k < length; ++k) {
            *outNext = (uint8_t)(*matchPtr + *(outNext - span) - *(matchPtr - span));
            ++outNext;
            ++matchPtr;
        }
    }

    if (outSize > 17) {
        if (!lastTargetUsages) { LastErrorCode() = __LINE__; return false; }
        X86Filter(outBegin, (int32_t)outSize, lastTargetUsages, true);
    }
    return true;
}

// 单块入口：自己分配 x86 过滤器要用的临时表
inline bool DecompressBlock(const void* in, size_t inSize, void* out, size_t outSize) {
    int32_t* usage = new (std::nothrow) int32_t[65536];
    if (!usage) return false;
    const bool ok = DecompressBlock(in, inSize, out, outSize, usage);
    delete[] usage;
    return ok;
}

// 解一份 LZMS 数据。
//
// 走 cabinet.dll 压出来的缓冲区是一个容器：magic + 原始大小 + 块大小，后面是
// 「u32 压缩长度 + 块数据」的序列，每块独立压缩、独立做 x86 还原。裸块（没有
// 容器头）也能直接解。
inline bool Decompress(const void* in, size_t inSize, void* out, size_t outSize) {
    const uint8_t* src = static_cast<const uint8_t*>(in);

    if (inSize < 24 || detail::ReadLe32(src) != 0xC0E5510Au)
        return DecompressBlock(in, inSize, out, outSize);

    const uint64_t rawSize = detail::ReadLe64(src + 8);
    const uint64_t blockSize = detail::ReadLe64(src + 16);
    if (rawSize != (uint64_t)outSize || blockSize == 0) return false;

    int32_t* usage = new (std::nothrow) int32_t[65536];
    if (!usage) return false;

    uint8_t* dst = static_cast<uint8_t*>(out);
    size_t pos = 24;
    uint64_t done = 0;
    bool ok = true;

    while (done < rawSize) {
        if (pos + 4 > inSize) { ok = false; break; }
        const uint32_t compressedSize = detail::ReadLe32(src + pos);
        pos += 4;
        if ((uint64_t)pos + compressedSize > (uint64_t)inSize) { ok = false; break; }

        const uint64_t remain = rawSize - done;
        const size_t rawLen = (size_t)(remain < blockSize ? remain : blockSize);

        // 压缩后不比原始小的时候，压缩 API 会把这一块原样存下来。
        if (compressedSize >= rawLen) {
            std::memcpy(dst + done, src + pos, rawLen);
        } else if (!DecompressBlock(src + pos, compressedSize, dst + done, rawLen, usage)) {
            ok = false;
            break;
        }
        pos += compressedSize;
        done += rawLen;
    }

    delete[] usage;
    return ok;
}

} // namespace LzmsNative
