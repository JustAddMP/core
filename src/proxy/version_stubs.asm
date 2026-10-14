; Export stubs of the version.dll proxy: each jumps through g_real[i], filled by version_proxy.cpp with the
; addresses from the real System32\version.dll (or a fallback that fails with ERROR_PROC_NOT_FOUND).
; A plain jmp keeps every argument register and the stack exactly as the caller left them, whatever the signature.

EXTERN g_real:QWORD

STUB MACRO name, index
name PROC
    jmp QWORD PTR [g_real + index * 8]
name ENDP
ENDM

.code
STUB mc_GetFileVersionInfoA, 0
STUB mc_GetFileVersionInfoByHandle, 1
STUB mc_GetFileVersionInfoExA, 2
STUB mc_GetFileVersionInfoExW, 3
STUB mc_GetFileVersionInfoSizeA, 4
STUB mc_GetFileVersionInfoSizeExA, 5
STUB mc_GetFileVersionInfoSizeExW, 6
STUB mc_GetFileVersionInfoSizeW, 7
STUB mc_GetFileVersionInfoW, 8
STUB mc_VerFindFileA, 9
STUB mc_VerFindFileW, 10
STUB mc_VerInstallFileA, 11
STUB mc_VerInstallFileW, 12
STUB mc_VerLanguageNameA, 13
STUB mc_VerLanguageNameW, 14
STUB mc_VerQueryValueA, 15
STUB mc_VerQueryValueW, 16
END
