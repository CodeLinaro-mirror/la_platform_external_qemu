
#ifndef AEMU_WIN32_FILE
#define AEMU_WIN32_FILE

// Note that we currently just forward to the Ascii version.
// TODO(whollins): Update these to convert utf8 to wchar.
// Old emulator implementation is here: external/qemu/android/android-emu-base/stubs/win32-stubs.c

#include <windows.h>

/* These are wrappers around Win32 functions. When building against the
 * Android emulator, they will treat file names as UTF-8 encoded strings,
 * instead of ANSI ones. */
inline HANDLE aemu_CreateFile(
        LPCTSTR               lpFileName,
        DWORD                 dwDesiredAccess,
        DWORD                 dwShareMode,
        LPSECURITY_ATTRIBUTES lpSecurityAttributes,
        DWORD                 dwCreationDisposition,
        DWORD                 dwFlagsAndAttributes,
        HANDLE                hTemplateFile) {
  return CreateFileA(lpFileName, dwDesiredAccess, dwShareMode, lpSecurityAttributes, dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile);
}

inline DWORD aemu_GetCurrentDirectory(
        DWORD  nBufferLength,
        LPTSTR lpBuffer) {
  return GetCurrentDirectoryA(nBufferLength, lpBuffer);
}

inline DWORD aemu_GetModuleFileName(
        HMODULE hModule,
        LPTSTR  lpFilename,
        DWORD   nSize) {
  return GetModuleFileNameA(hModule, lpFilename, nSize);
}

inline BOOL aemu_GetDiskFreeSpace(
  LPCTSTR  lpRootPathName,
  LPDWORD lpSectorsPerCluster,
  LPDWORD lpBytesPerSector,
  LPDWORD lpNumberOfFreeClusters,
  LPDWORD lpTotalNumberOfClusters
) {
  return GetDiskFreeSpaceA(lpRootPathName, lpSectorsPerCluster, lpBytesPerSector, lpNumberOfFreeClusters, lpTotalNumberOfClusters);
}

#endif // AEMU_WIN32_FILE
