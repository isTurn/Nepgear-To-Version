; ============================================================================
; ProxyVersion_x64.asm - x64 trampolines for the version.dll proxy
;
; MASM64 does not support __declspec(naked) or inline asm in C++, so the
; exports declared in ProxyVersion.h (#pragma comment(linker, "/EXPORT:..."))
; land on these stubs. Each one jumps through the pointer bound by
; InitHijack() to the real system32\version.dll.
; ============================================================================

EXTERN pfnAheadLib_GetFileVersionInfoA:QWORD
EXTERN pfnAheadLib_GetFileVersionInfoByHandle:QWORD
EXTERN pfnAheadLib_GetFileVersionInfoExA:QWORD
EXTERN pfnAheadLib_GetFileVersionInfoExW:QWORD
EXTERN pfnAheadLib_GetFileVersionInfoSizeA:QWORD
EXTERN pfnAheadLib_GetFileVersionInfoSizeExA:QWORD
EXTERN pfnAheadLib_GetFileVersionInfoSizeExW:QWORD
EXTERN pfnAheadLib_GetFileVersionInfoSizeW:QWORD
EXTERN pfnAheadLib_GetFileVersionInfoW:QWORD
EXTERN pfnAheadLib_VerFindFileA:QWORD
EXTERN pfnAheadLib_VerFindFileW:QWORD
EXTERN pfnAheadLib_VerInstallFileA:QWORD
EXTERN pfnAheadLib_VerInstallFileW:QWORD
EXTERN pfnAheadLib_VerLanguageNameA:QWORD
EXTERN pfnAheadLib_VerLanguageNameW:QWORD
EXTERN pfnAheadLib_VerQueryValueA:QWORD
EXTERN pfnAheadLib_VerQueryValueW:QWORD

MAKE_TRAMPOLINE MACRO funcName
AheadLib_&funcName PROC
    jmp QWORD PTR [pfnAheadLib_&funcName]
AheadLib_&funcName ENDP
ENDM

_TEXT SEGMENT

MAKE_TRAMPOLINE GetFileVersionInfoA
MAKE_TRAMPOLINE GetFileVersionInfoByHandle
MAKE_TRAMPOLINE GetFileVersionInfoExA
MAKE_TRAMPOLINE GetFileVersionInfoExW
MAKE_TRAMPOLINE GetFileVersionInfoSizeA
MAKE_TRAMPOLINE GetFileVersionInfoSizeExA
MAKE_TRAMPOLINE GetFileVersionInfoSizeExW
MAKE_TRAMPOLINE GetFileVersionInfoSizeW
MAKE_TRAMPOLINE GetFileVersionInfoW
MAKE_TRAMPOLINE VerFindFileA
MAKE_TRAMPOLINE VerFindFileW
MAKE_TRAMPOLINE VerInstallFileA
MAKE_TRAMPOLINE VerInstallFileW
MAKE_TRAMPOLINE VerLanguageNameA
MAKE_TRAMPOLINE VerLanguageNameW
MAKE_TRAMPOLINE VerQueryValueA
MAKE_TRAMPOLINE VerQueryValueW

_TEXT ENDS

END
