// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dbghelp.h>
#include <cstdio>
#include <cstdint>
namespace online { namespace crash {
// Allocate/load everything before an exception; never touch engine objects in
// the handler. A small dump includes thread stacks, not the full game memory.
static wchar_t directory[1024]={};
static decltype(&MiniDumpWriteDump) writeDump=nullptr;
static LPTOP_LEVEL_EXCEPTION_FILTER previous=nullptr;
static volatile LONG entered=0;
static LONG WINAPI Handle(EXCEPTION_POINTERS* exception) {
    if(InterlockedExchange(&entered,1)) return EXCEPTION_CONTINUE_SEARCH;
    SYSTEMTIME time; GetLocalTime(&time);
    wchar_t stem[1200],path[1240];
    swprintf_s(stem,L"%scoop-crash-%04u%02u%02u-%02u%02u%02u-%lu",directory,time.wYear,time.wMonth,time.wDay,time.wHour,time.wMinute,time.wSecond,GetCurrentProcessId());
    swprintf_s(path,L"%s.dmp",stem);
    bool dumped=false;
    HANDLE dump=CreateFileW(path,GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(dump!=INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION info={GetCurrentThreadId(),exception,FALSE};
        if(writeDump) dumped=writeDump(GetCurrentProcess(),GetCurrentProcessId(),dump,MiniDumpWithThreadInfo,&info,nullptr,nullptr)!=FALSE;
        CloseHandle(dump);
    }
    HMODULE module=nullptr; wchar_t moduleName[1024]=L"unknown";
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                      reinterpret_cast<LPCWSTR>(exception->ExceptionRecord->ExceptionAddress),&module);
    if(module) GetModuleFileNameW(module,moduleName,1024);
    swprintf_s(path,L"%s.txt",stem); FILE* report=nullptr;
    if(!_wfopen_s(&report,path,L"w") && report) {
        fwprintf(report,L"Amnesia Online Co-op crash\nexception=0x%08lx\naddress=%p\nmodule=%s\noffset=0x%llx\nthread=%lu\ndump_written=%u\n",
            exception->ExceptionRecord->ExceptionCode,exception->ExceptionRecord->ExceptionAddress,moduleName,
            static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(exception->ExceptionRecord->ExceptionAddress)-reinterpret_cast<uintptr_t>(module)),GetCurrentThreadId(),dumped?1:0);
        fclose(report);
    }
    // Preserve Windows reporting and any filter the engine installed earlier.
    return previous?previous(exception):EXCEPTION_CONTINUE_SEARCH;
}
static void Install(const wchar_t* path) {
    if(!path || wcslen(path)+2>=1024) return;
    wcscpy_s(directory,path);
    const size_t n=wcslen(directory);
    if(n && directory[n-1]!=L'/' && directory[n-1]!=L'\\') wcscat_s(directory,L"\\");
    if(HMODULE library=LoadLibraryExW(L"dbghelp.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32))
        writeDump=reinterpret_cast<decltype(writeDump)>(GetProcAddress(library,"MiniDumpWriteDump"));
    const auto filter=SetUnhandledExceptionFilter(Handle);
    if(filter!=Handle) previous=filter;
}
}}
#else
namespace online { namespace crash { inline void Install(const wchar_t*) {} }}
#endif
