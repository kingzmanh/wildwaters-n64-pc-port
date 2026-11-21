// WW_PROFILE_GFX=1 (diagnostics): a sampling profiler for the thread that runs the
// renderer's send_dl. While it is inside send_dl, a helper thread suspends it every
// millisecond, walks its stack with DbgHelp and counts functions; the hottest are printed
// every few seconds (self = top frame, total = anywhere on the stack).

#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "ww.hpp"

namespace {
    std::atomic<HANDLE> target{nullptr};
    std::atomic<bool> inside{false};
    std::mutex counts_mutex;
    std::map<DWORD64, uint32_t> self_counts, total_counts;
    uint32_t samples = 0;

    std::string name_of(DWORD64 addr) {
        alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 256];
        SYMBOL_INFO* s = reinterpret_cast<SYMBOL_INFO*>(buf);
        s->SizeOfStruct = sizeof(SYMBOL_INFO);
        s->MaxNameLen = 255;
        DWORD64 off = 0;
        if (SymFromAddr(GetCurrentProcess(), addr, &off, s)) return s->Name;
        char t[32];
        std::snprintf(t, sizeof(t), "%llx", (unsigned long long)addr);
        return t;
    }

    DWORD64 func_start(DWORD64 addr) {
        alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 256];
        SYMBOL_INFO* s = reinterpret_cast<SYMBOL_INFO*>(buf);
        s->SizeOfStruct = sizeof(SYMBOL_INFO);
        s->MaxNameLen = 255;
        DWORD64 off = 0;
        if (SymFromAddr(GetCurrentProcess(), addr, &off, s)) return addr - off;
        return addr;
    }

    void report() {
        std::vector<std::pair<uint32_t, DWORD64>> self, total;
        {
            std::lock_guard lock(counts_mutex);
            for (auto& [a, c] : self_counts) self.push_back({ c, a });
            for (auto& [a, c] : total_counts) total.push_back({ c, a });
        }
        std::sort(self.rbegin(), self.rend());
        std::sort(total.rbegin(), total.rend());
        std::printf("[profile] %u samples. self:\n", samples);
        for (size_t i = 0; i < self.size() && i < 15; i++) std::printf("[profile]   %5.1f%% %s\n", 100.0 * self[i].first / samples, name_of(self[i].second).c_str());
        std::printf("[profile] total:\n");
        for (size_t i = 0; i < total.size() && i < 25; i++) std::printf("[profile]   %5.1f%% %s\n", 100.0 * total[i].first / samples, name_of(total[i].second).c_str());
        std::fflush(stdout);
    }

    // Walks one suspended thread and counts its functions.
    void sample_thread(HANDLE process, HANDLE t) {
        if (SuspendThread(t) == (DWORD)-1) return;
        CONTEXT ctx{};
        ctx.ContextFlags = CONTEXT_FULL;
        std::vector<DWORD64> frames;
        if (GetThreadContext(t, &ctx)) {
            STACKFRAME64 f{};
            f.AddrPC.Offset = ctx.Rip; f.AddrPC.Mode = AddrModeFlat;
            f.AddrFrame.Offset = ctx.Rbp; f.AddrFrame.Mode = AddrModeFlat;
            f.AddrStack.Offset = ctx.Rsp; f.AddrStack.Mode = AddrModeFlat;
            for (int i = 0; i < 24; i++) {
                if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, t, &f, &ctx, nullptr,
                        SymFunctionTableAccess64, SymGetModuleBase64, nullptr) || f.AddrPC.Offset == 0) break;
                frames.push_back(f.AddrPC.Offset);
            }
        }
        ResumeThread(t);
        if (frames.empty()) return;
        // Skip threads that are just blocked in the kernel.
        std::string top = name_of(frames[0]);
        if (top.rfind("Nt", 0) == 0 || top.rfind("Zw", 0) == 0) return;
        std::lock_guard lock(counts_mutex);
        samples++;
        std::set<DWORD64> seen;
        for (size_t i = 0; i < frames.size(); i++) {
            DWORD64 fs = func_start(frames[i]);
            if (i == 0) self_counts[fs]++;
            if (seen.insert(fs).second) total_counts[fs]++;
        }
    }

    void sampler() {
        HANDLE process = GetCurrentProcess();
        SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
        SymInitialize(process, nullptr, TRUE);
        DWORD last_report = GetTickCount();
        const DWORD me = GetCurrentThreadId(), pid = GetCurrentProcessId();
        for (;;) {
            Sleep(2);
            if (!inside.load()) continue;
            // Every running thread of the process, not just the one in send_dl.
            HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            THREADENTRY32 te{ sizeof(te) };
            if (Thread32First(snap, &te)) {
                do {
                    if (te.th32OwnerProcessID != pid || te.th32ThreadID == me) continue;
                    HANDLE t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
                    if (t) { sample_thread(process, t); CloseHandle(t); }
                } while (Thread32Next(snap, &te));
            }
            CloseHandle(snap);
            if (GetTickCount() - last_report > 8000) {
                last_report = GetTickCount();
                report();
            }
        }
    }
}

void ww::profiler_enter() {
    static bool started = false;
    if (!started) {
        started = true;
        HANDLE h = nullptr;
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &h, 0, FALSE, DUPLICATE_SAME_ACCESS);
        target.store(h);
        std::thread(sampler).detach();
    }
    inside.store(true);
}

void ww::profiler_leave() {
    inside.store(false);
}
