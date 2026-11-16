#pragma once

// ============================================================================
// version.dll proxy (AheadLib style)
//
// Nepgear normally links as winmm.dll. Some targets import version.dll instead,
// so the same hook core can be built against this proxy:
//     build.bat version
//     编译\build_chs_embed.py Nepgear.chs x86 --dll version
// (both forward -p:NepgearProxy=version to MSBuild).
//
// Every export is a trampoline into the real system32\version.dll, so a game
// that calls GetFileVersionInfo / VerQueryValue keeps working while the hooks
// are installed.
// ============================================================================

#include <windows.h>

// ============================================================================
// Export pragmas - x86 vs x64 name decoration differs
// x86: __cdecl names have an underscore prefix (_AheadLib_xxx)
// x64: no underscore prefix (AheadLib_xxx)
// ============================================================================

#ifdef _WIN64

#pragma comment(linker, "/EXPORT:GetFileVersionInfoA=AheadLib_GetFileVersionInfoA")
#pragma comment(linker, "/EXPORT:GetFileVersionInfoByHandle=AheadLib_GetFileVersionInfoByHandle")
#pragma comment(linker, "/EXPORT:GetFileVersionInfoExA=AheadLib_GetFileVersionInfoExA")
#pragma comment(linker, "/EXPORT:GetFileVersionInfoExW=AheadLib_GetFileVersionInfoExW")
#pragma comment(linker, "/EXPORT:GetFileVersionInfoSizeA=AheadLib_GetFileVersionInfoSizeA")
#pragma comment(linker, "/EXPORT:GetFileVersionInfoSizeExA=AheadLib_GetFileVersionInfoSizeExA")
#pragma comment(linker, "/EXPORT:GetFileVersionInfoSizeExW=AheadLib_GetFileVersionInfoSizeExW")
#pragma comment(linker, "/EXPORT:GetFileVersionInfoSizeW=AheadLib_GetFileVersionInfoSizeW")
#pragma comment(linker, "/EXPORT:GetFileVersionInfoW=AheadLib_GetFileVersionInfoW")
#pragma comment(linker, "/EXPORT:VerFindFileA=AheadLib_VerFindFileA")
#pragma comment(linker, "/EXPORT:VerFindFileW=AheadLib_VerFindFileW")
#pragma comment(linker, "/EXPORT:VerInstallFileA=AheadLib_VerInstallFileA")
#pragma comment(linker, "/EXPORT:VerInstallFileW=AheadLib_VerInstallFileW")
#pragma comment(linker, "/EXPORT:VerLanguageNameA=AheadLib_VerLanguageNameA")
#pragma comment(linker, "/EXPORT:VerLanguageNameW=AheadLib_VerLanguageNameW")
#pragma comment(linker, "/EXPORT:VerQueryValueA=AheadLib_VerQueryValueA")
#pragma comment(linker, "/EXPORT:VerQueryValueW=AheadLib_VerQueryValueW")

#else

#pragma comment(linker, "/EXPORT:GetFileVersionInfoA=_AheadLib_GetFileVersionInfoA")
#pragma comment(linker, "/EXPORT:GetFileVersionInfoByHandle=_AheadLib_GetFileVersionInfoByHandle")
#pragma comment(linker, "/EXPORT:GetFileVersionInfoExA=_AheadLib_GetFileVersionInfoExA")
#pragma comment(linker, "/EXPORT:GetFileVersionInfoExW=_AheadLib_GetFileVersionInfoExW")
#pragma comment(linker, "/EXPORT:GetFileVersionInfoSizeA=_AheadLib_GetFileVersionInfoSizeA")
#pragma comment(linker, "/EXPORT:GetFileVersionInfoSizeExA=_AheadLib_GetFileVersionInfoSizeExA")
#pragma comment(linker, "/EXPORT:GetFileVersionInfoSizeExW=_AheadLib_GetFileVersionInfoSizeExW")
#pragma comment(linker, "/EXPORT:GetFileVersionInfoSizeW=_AheadLib_GetFileVersionInfoSizeW")
#pragma comment(linker, "/EXPORT:GetFileVersionInfoW=_AheadLib_GetFileVersionInfoW")
#pragma comment(linker, "/EXPORT:VerFindFileA=_AheadLib_VerFindFileA")
#pragma comment(linker, "/EXPORT:VerFindFileW=_AheadLib_VerFindFileW")
#pragma comment(linker, "/EXPORT:VerInstallFileA=_AheadLib_VerInstallFileA")
#pragma comment(linker, "/EXPORT:VerInstallFileW=_AheadLib_VerInstallFileW")
#pragma comment(linker, "/EXPORT:VerLanguageNameA=_AheadLib_VerLanguageNameA")
#pragma comment(linker, "/EXPORT:VerLanguageNameW=_AheadLib_VerLanguageNameW")
#pragma comment(linker, "/EXPORT:VerQueryValueA=_AheadLib_VerQueryValueA")
#pragma comment(linker, "/EXPORT:VerQueryValueW=_AheadLib_VerQueryValueW")

#endif

// ============================================================================
// Function pointer variables (bound in InitHijack)
// ============================================================================

extern "C" {
PVOID pfnAheadLib_GetFileVersionInfoA;
PVOID pfnAheadLib_GetFileVersionInfoByHandle;
PVOID pfnAheadLib_GetFileVersionInfoExA;
PVOID pfnAheadLib_GetFileVersionInfoExW;
PVOID pfnAheadLib_GetFileVersionInfoSizeA;
PVOID pfnAheadLib_GetFileVersionInfoSizeExA;
PVOID pfnAheadLib_GetFileVersionInfoSizeExW;
PVOID pfnAheadLib_GetFileVersionInfoSizeW;
PVOID pfnAheadLib_GetFileVersionInfoW;
PVOID pfnAheadLib_VerFindFileA;
PVOID pfnAheadLib_VerFindFileW;
PVOID pfnAheadLib_VerInstallFileA;
PVOID pfnAheadLib_VerInstallFileW;
PVOID pfnAheadLib_VerLanguageNameA;
PVOID pfnAheadLib_VerLanguageNameW;
PVOID pfnAheadLib_VerQueryValueA;
PVOID pfnAheadLib_VerQueryValueW;
} // extern "C"

// ============================================================================
// Shared functions
// ============================================================================

HMODULE g_OldModule = NULL;

VOID WINAPI Free()
{
	if (g_OldModule)
	{
		FreeLibrary(g_OldModule);
	}
}

// A missing export must not abort the process: GetFileVersionInfoByHandle is
// undocumented and absent from some builds, and the host game may never call
// it. Warn loudly instead of ExitProcess(-2) and let the remaining forwards
// work.
FARPROC WINAPI GetAddress(PCSTR pszProcName)
{
	FARPROC fpAddress;
	fpAddress = GetProcAddress(g_OldModule, pszProcName);
	if (fpAddress == NULL)
	{
		char buf[384];
		wsprintfA(buf, "[Nepgear] WARNING: Export '%s' not found in version.dll\r\n", pszProcName);
		OutputDebugStringA(buf);
	}
	return fpAddress;
}

BOOL WINAPI InitHijack()
{
	if (g_OldModule) return TRUE;

	TCHAR tzPath[MAX_PATH];
	GetSystemDirectory(tzPath, MAX_PATH);
	lstrcat(tzPath, TEXT("\\version.dll"));

	g_OldModule = LoadLibrary(tzPath);
	if (g_OldModule == NULL)
	{
		MessageBox(NULL, L"LoadLibrary Handle Null (version.dll)", NULL, NULL);
		return FALSE;
	}

	pfnAheadLib_GetFileVersionInfoA = GetAddress("GetFileVersionInfoA");
	pfnAheadLib_GetFileVersionInfoByHandle = GetAddress("GetFileVersionInfoByHandle");
	pfnAheadLib_GetFileVersionInfoExA = GetAddress("GetFileVersionInfoExA");
	pfnAheadLib_GetFileVersionInfoExW = GetAddress("GetFileVersionInfoExW");
	pfnAheadLib_GetFileVersionInfoSizeA = GetAddress("GetFileVersionInfoSizeA");
	pfnAheadLib_GetFileVersionInfoSizeExA = GetAddress("GetFileVersionInfoSizeExA");
	pfnAheadLib_GetFileVersionInfoSizeExW = GetAddress("GetFileVersionInfoSizeExW");
	pfnAheadLib_GetFileVersionInfoSizeW = GetAddress("GetFileVersionInfoSizeW");
	pfnAheadLib_GetFileVersionInfoW = GetAddress("GetFileVersionInfoW");
	pfnAheadLib_VerFindFileA = GetAddress("VerFindFileA");
	pfnAheadLib_VerFindFileW = GetAddress("VerFindFileW");
	pfnAheadLib_VerInstallFileA = GetAddress("VerInstallFileA");
	pfnAheadLib_VerInstallFileW = GetAddress("VerInstallFileW");
	pfnAheadLib_VerLanguageNameA = GetAddress("VerLanguageNameA");
	pfnAheadLib_VerLanguageNameW = GetAddress("VerLanguageNameW");
	pfnAheadLib_VerQueryValueA = GetAddress("VerQueryValueA");
	pfnAheadLib_VerQueryValueW = GetAddress("VerQueryValueW");

	return TRUE;
}

// ============================================================================
// x86: naked asm trampolines (x64 lives in ProxyVersion_x64.asm)
// ============================================================================

#ifndef _WIN64

EXTERN_C __declspec(naked) void __cdecl AheadLib_GetFileVersionInfoA(void)
{
	__asm jmp pfnAheadLib_GetFileVersionInfoA;
}

EXTERN_C __declspec(naked) void __cdecl AheadLib_GetFileVersionInfoByHandle(void)
{
	__asm jmp pfnAheadLib_GetFileVersionInfoByHandle;
}

EXTERN_C __declspec(naked) void __cdecl AheadLib_GetFileVersionInfoExA(void)
{
	__asm jmp pfnAheadLib_GetFileVersionInfoExA;
}

EXTERN_C __declspec(naked) void __cdecl AheadLib_GetFileVersionInfoExW(void)
{
	__asm jmp pfnAheadLib_GetFileVersionInfoExW;
}

EXTERN_C __declspec(naked) void __cdecl AheadLib_GetFileVersionInfoSizeA(void)
{
	__asm jmp pfnAheadLib_GetFileVersionInfoSizeA;
}

EXTERN_C __declspec(naked) void __cdecl AheadLib_GetFileVersionInfoSizeExA(void)
{
	__asm jmp pfnAheadLib_GetFileVersionInfoSizeExA;
}

EXTERN_C __declspec(naked) void __cdecl AheadLib_GetFileVersionInfoSizeExW(void)
{
	__asm jmp pfnAheadLib_GetFileVersionInfoSizeExW;
}

EXTERN_C __declspec(naked) void __cdecl AheadLib_GetFileVersionInfoSizeW(void)
{
	__asm jmp pfnAheadLib_GetFileVersionInfoSizeW;
}

EXTERN_C __declspec(naked) void __cdecl AheadLib_GetFileVersionInfoW(void)
{
	__asm jmp pfnAheadLib_GetFileVersionInfoW;
}

EXTERN_C __declspec(naked) void __cdecl AheadLib_VerFindFileA(void)
{
	__asm jmp pfnAheadLib_VerFindFileA;
}

EXTERN_C __declspec(naked) void __cdecl AheadLib_VerFindFileW(void)
{
	__asm jmp pfnAheadLib_VerFindFileW;
}

EXTERN_C __declspec(naked) void __cdecl AheadLib_VerInstallFileA(void)
{
	__asm jmp pfnAheadLib_VerInstallFileA;
}

EXTERN_C __declspec(naked) void __cdecl AheadLib_VerInstallFileW(void)
{
	__asm jmp pfnAheadLib_VerInstallFileW;
}

EXTERN_C __declspec(naked) void __cdecl AheadLib_VerLanguageNameA(void)
{
	__asm jmp pfnAheadLib_VerLanguageNameA;
}

EXTERN_C __declspec(naked) void __cdecl AheadLib_VerLanguageNameW(void)
{
	__asm jmp pfnAheadLib_VerLanguageNameW;
}

EXTERN_C __declspec(naked) void __cdecl AheadLib_VerQueryValueA(void)
{
	__asm jmp pfnAheadLib_VerQueryValueA;
}

EXTERN_C __declspec(naked) void __cdecl AheadLib_VerQueryValueW(void)
{
	__asm jmp pfnAheadLib_VerQueryValueW;
}

#endif // !_WIN64
