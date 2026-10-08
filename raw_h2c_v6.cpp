#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <setupapi.h>
#include <malloc.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#pragma comment(lib, "setupapi.lib")

static const GUID QDMA_IF_GUID =
{ 0x80f90fa8, 0x0cea, 0x43c5, { 0x81, 0xce, 0x4b, 0xf9, 0x4a, 0xa1, 0x4c, 0x72 } };

static std::wstring winerr(DWORD e) {
    wchar_t* msg = nullptr;
    DWORD n = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER |
        FORMAT_MESSAGE_FROM_SYSTEM |
        FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, e, 0, reinterpret_cast<wchar_t*>(&msg), 0, nullptr);

    std::wstring s = (n && msg) ? msg : L"(unknown error)";
    if (msg) LocalFree(msg);
    while (!s.empty() && (s.back() == L'\r' || s.back() == L'\n'))
        s.pop_back();
    return s;
}

static std::wstring lower_copy(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(),
        [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    return s;
}

static std::vector<std::wstring> enumerate_qdma_interfaces() {
    std::vector<std::wstring> paths;

    HDEVINFO devs = SetupDiGetClassDevsW(
        &QDMA_IF_GUID, nullptr, nullptr,
        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);

    if (devs == INVALID_HANDLE_VALUE)
        return paths;

    for (DWORD index = 0;; ++index) {
        SP_DEVICE_INTERFACE_DATA ifdata{};
        ifdata.cbSize = sizeof(ifdata);

        if (!SetupDiEnumDeviceInterfaces(
                devs, nullptr, &QDMA_IF_GUID, index, &ifdata))
            break;

        DWORD required = 0;
        SetupDiGetDeviceInterfaceDetailW(
            devs, &ifdata, nullptr, 0, &required, nullptr);

        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || required == 0)
            continue;

        std::vector<std::uint8_t> buf(required);
        auto* detail =
            reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buf.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);

        if (SetupDiGetDeviceInterfaceDetailW(
                devs, &ifdata, detail, required, nullptr, nullptr)) {
            paths.emplace_back(detail->DevicePath);
        }
    }

    SetupDiDestroyDeviceInfoList(devs);
    return paths;
}

static std::wstring select_b03f_interface(
    const std::vector<std::wstring>& interfaces) {

    if (interfaces.empty())
        return L"";

    for (const auto& p : interfaces) {
        const auto lp = lower_copy(p);
        if (lp.find(L"ven_10ee") != std::wstring::npos &&
            lp.find(L"dev_b03f") != std::wstring::npos) {
            return p;
        }
    }

    return interfaces.front();
}

static void dump_counter_prefix(const void* ptr, size_t bytes) {
    const auto* p = static_cast<const std::uint16_t*>(ptr);
    const size_t n = (std::min)(bytes / sizeof(std::uint16_t), size_t(32));

    std::cout << "Golden counter first " << n << " uint16 values:\n";
    for (size_t i = 0; i < n; ++i) {
        if ((i % 8) == 0)
            std::cout << "  ";
        std::cout << std::hex << std::setfill('0')
                  << std::setw(4) << p[i] << ' ';
        if ((i % 8) == 7 || i + 1 == n)
            std::cout << '\n';
    }
    std::cout << std::dec;
}

static bool write_user_u32(
    const std::wstring& base,
    std::uint64_t offset,
    std::uint32_t value) {

    const std::wstring path = base + L"\\user";

    std::wcout << L"Opening USER BAR node:\n  " << path << L"\n";

    HANDLE h = CreateFileW(
        path.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        0,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);

    if (h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        std::wcerr << L"ERROR: CreateFileW(user) failed, code="
                   << e << L" (" << winerr(e) << L")\n";
        return false;
    }

    LARGE_INTEGER li{};
    li.QuadPart = static_cast<LONGLONG>(offset);

    if (!SetFilePointerEx(h, li, nullptr, FILE_BEGIN)) {
        DWORD e = GetLastError();
        std::wcerr << L"ERROR: SetFilePointerEx(user) failed, code="
                   << e << L" (" << winerr(e) << L")\n";
        CloseHandle(h);
        return false;
    }

    DWORD done = 0;
    BOOL ok = WriteFile(
        h,
        &value,
        static_cast<DWORD>(sizeof(value)),
        &done,
        nullptr);

    if (!ok || done != sizeof(value)) {
        DWORD e = ok ? ERROR_WRITE_FAULT : GetLastError();
        std::wcerr << L"ERROR: USER BAR write failed, code="
                   << e << L" (" << winerr(e) << L"), bytes="
                   << done << L"\n";
        CloseHandle(h);
        return false;
    }

    std::wcout << L"USER BAR write PASS: offset=0x"
               << std::hex << offset
               << L", value=0x" << value
               << std::dec << L", bytes=" << done << L"\n";

    FlushFileBuffers(h); // harmless for BAR node; keep ordering explicit
    CloseHandle(h);
    return true;
}

static void usage() {
    std::wcout
        << L"Usage:\n"
        << L"  raw_h2c_v6.exe <qid> [timeout_ms]\n\n"
        << L"Example:\n"
        << L"  raw_h2c_v6.exe 0 10000\n\n"
        << L"Purpose:\n"
        << L"  Golden-replica experiment. Do NOT send arbitrary file data yet.\n"
        << L"  This version intentionally mirrors the known-good dma-arw ST H2C path:\n"
        << L"    1) write USER BAR offset 0x0C = 1\n"
        << L"    2) generate 4096-byte uint16 increment pattern 0000,0001,...\n"
        << L"    3) open queue_st_<qid> with flags 0x60000080\n"
        << L"    4) associate queue with IOCP\n"
        << L"    5) asynchronous WriteFile\n"
        << L"    6) GetQueuedCompletionStatusEx polling\n";
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 2 || argc > 3) {
        usage();
        return 1;
    }

    unsigned long qid = 0;
    DWORD timeout_ms = 10000;

    try {
        qid = std::stoul(argv[1]);
        if (argc == 3)
            timeout_ms = static_cast<DWORD>(std::stoul(argv[2]));
    } catch (...) {
        std::wcerr << L"ERROR: invalid qid/timeout\n";
        return 1;
    }

    if (qid > 511) {
        std::wcerr << L"ERROR: qid must be 0..511\n";
        return 1;
    }

    constexpr DWORD kLength = 4096;
    constexpr size_t kAlignment = 4096;

    auto interfaces = enumerate_qdma_interfaces();
    if (interfaces.empty()) {
        std::wcerr << L"ERROR: no enabled QDMA interface found\n";
        return 2;
    }

    const std::wstring base = select_b03f_interface(interfaces);
    std::wcout << L"Selected QDMA interface:\n  " << base << L"\n";

    // Golden-reference pre-step observed in dma-arw:
    // write H2C control register in USER BAR.
    if (!write_user_u32(base, 0x0C, 0x00000001)) {
        std::wcerr << L"ERROR: golden USER BAR pre-write failed\n";
        return 3;
    }

    // Keep the pre-write close to DMA submission.
    Sleep(1);

    void* aligned = _aligned_malloc(kLength, kAlignment);
    if (!aligned) {
        std::wcerr << L"ERROR: _aligned_malloc failed\n";
        return 4;
    }

    auto* words = static_cast<std::uint16_t*>(aligned);
    constexpr size_t word_count = kLength / sizeof(std::uint16_t);

    for (size_t i = 0; i < word_count; ++i)
        words[i] = static_cast<std::uint16_t>(i);

    dump_counter_prefix(aligned, kLength);

    const auto addr = reinterpret_cast<std::uintptr_t>(aligned);
    std::wcout << L"Buffer address: 0x"
               << std::hex << addr << std::dec
               << L" (mod 4096 = " << (addr & 0xfff) << L")\n";

    std::wostringstream qnode;
    qnode << base << L"\\queue_st_" << qid;
    const std::wstring queue_path = qnode.str();

    std::wcout << L"Opening H2C queue node:\n  " << queue_path << L"\n";

    const DWORD open_flags =
        FILE_FLAG_OVERLAPPED |
        FILE_FLAG_NO_BUFFERING |
        FILE_ATTRIBUTE_NORMAL; // 0x60000080

    HANDLE h = CreateFileW(
        queue_path.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        0,
        nullptr,
        OPEN_EXISTING,
        open_flags,
        nullptr);

    if (h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        std::wcerr << L"ERROR: CreateFileW(queue) failed, code="
                   << e << L" (" << winerr(e) << L")\n";
        _aligned_free(aligned);
        return 5;
    }

    std::wcout << L"CreateFileW(queue) PASS, flags=0x"
               << std::hex << open_flags << std::dec << L"\n";

    HANDLE iocp = CreateIoCompletionPort(
        h,
        nullptr,
        static_cast<ULONG_PTR>(1),
        0);

    if (!iocp) {
        DWORD e = GetLastError();
        std::wcerr << L"ERROR: CreateIoCompletionPort failed, code="
                   << e << L" (" << winerr(e) << L")\n";
        CloseHandle(h);
        _aligned_free(aligned);
        return 6;
    }

    std::wcout << L"CreateIoCompletionPort PASS\n";

    OVERLAPPED ov{};
    ov.Offset = 0;
    ov.OffsetHigh = 0;
    ov.hEvent = nullptr;

    SetLastError(ERROR_SUCCESS);

    BOOL write_ok = WriteFile(
        h,
        aligned,
        kLength,
        nullptr,
        &ov);

    if (!write_ok) {
        DWORD e = GetLastError();
        if (e != ERROR_IO_PENDING) {
            std::wcerr << L"ERROR: WriteFile failed immediately, code="
                       << e << L" (" << winerr(e) << L")\n";
            CloseHandle(iocp);
            CloseHandle(h);
            _aligned_free(aligned);
            return 7;
        }
        std::wcout << L"WriteFile -> ERROR_IO_PENDING (expected)\n";
    } else {
        std::wcout << L"WriteFile returned TRUE immediately\n";
    }

    OVERLAPPED_ENTRY entries[50]{};
    DWORD total_bytes = 0;
    unsigned completion_count = 0;

    const auto t0 = std::chrono::steady_clock::now();

    while (total_bytes < kLength) {
        auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0).count();

        if (elapsed >= timeout_ms) {
            std::wcerr << L"ERROR: timeout after "
                       << elapsed << L" ms; accumulated "
                       << total_bytes << L" / " << kLength << L" bytes\n";
            CancelIoEx(h, &ov);
            CloseHandle(iocp);
            CloseHandle(h);
            _aligned_free(aligned);
            return 8;
        }

        ULONG removed = 0;
        std::memset(entries, 0, sizeof(entries));
        SetLastError(ERROR_SUCCESS);

        BOOL got = GetQueuedCompletionStatusEx(
            iocp,
            entries,
            50,
            &removed,
            100,
            FALSE);

        if (!got) {
            DWORD e = GetLastError();
            if (e == WAIT_TIMEOUT)
                continue;

            std::wcerr << L"ERROR: GetQueuedCompletionStatusEx failed, code="
                       << e << L" (" << winerr(e) << L")\n";
            CancelIoEx(h, &ov);
            CloseHandle(iocp);
            CloseHandle(h);
            _aligned_free(aligned);
            return 9;
        }

        std::wcout << L"Completion batch: " << removed << L"\n";

        for (ULONG i = 0; i < removed; ++i) {
            const auto& e = entries[i];

            std::wcout
                << L"  [" << completion_count++ << L"]"
                << L" key=" << e.lpCompletionKey
                << L" ov=" << e.lpOverlapped
                << L" Internal=0x" << std::hex
                << static_cast<unsigned long long>(e.Internal)
                << std::dec
                << L" bytes=" << e.dwNumberOfBytesTransferred
                << L"\n";

            total_bytes += e.dwNumberOfBytesTransferred;
        }

        std::wcout << L"Accumulated bytes: "
                   << total_bytes << L" / " << kLength << L"\n";
    }

    std::wcout << L"GOLDEN H2C PASS: "
               << total_bytes << L" / " << kLength << L" bytes\n";

    std::wcout << L"Final OVERLAPPED Internal=0x"
               << std::hex
               << static_cast<unsigned long long>(ov.Internal)
               << L" InternalHigh=0x"
               << static_cast<unsigned long long>(ov.InternalHigh)
               << std::dec << L"\n";

    Sleep(200);

    CloseHandle(iocp);
    CloseHandle(h);
    _aligned_free(aligned);

    return 0;
}
