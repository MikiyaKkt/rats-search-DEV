#pragma once
#include <QJsonObject>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#endif

inline QJsonObject processResources()
{
    QJsonObject result;
#ifdef Q_OS_WIN
    PROCESS_MEMORY_COUNTERS_EX memory{};
    memory.cb = sizeof(memory);
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory))) {
        result["working_set_bytes"] = qint64(memory.WorkingSetSize);
        result["peak_working_set_bytes"] = qint64(memory.PeakWorkingSetSize);
        result["private_bytes"] = qint64(memory.PrivateUsage);
    }
    FILETIME created, exited, kernel, user;
    if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
        const auto value = [](FILETIME t) { return (quint64(t.dwHighDateTime) << 32) | t.dwLowDateTime; };
        result["cpu_seconds"] = double(value(kernel) + value(user)) / 10000000.0;
    }
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot != INVALID_HANDLE_VALUE) {
        THREADENTRY32 entry{}; entry.dwSize = sizeof(entry); int count = 0;
        if (Thread32First(snapshot, &entry)) do { if (entry.th32OwnerProcessID == GetCurrentProcessId()) ++count; } while (Thread32Next(snapshot, &entry));
        CloseHandle(snapshot); result["threads"] = count;
    }
#endif
    result["scope"] = "probe process; excludes searchd and operating system";
    return result;
}
