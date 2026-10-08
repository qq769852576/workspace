#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <setupapi.h>
#include <malloc.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
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

static void dump_prefix(const void* ptr, size_t size, const char* title) {
    const auto* data = static_cast<const std::uint8_t*>(ptr);
    const size_t m = (std::min)(size, static_cast<size_t>(64));

    std::cout << title << " first " << m << " byte(s):\n";
    for (size_t i = 0; i < m; ++i) {
        if ((i % 16) == 0) {
            std::cout << std::hex << std::setfill('0')
                      << std::setw(4) << i << ": ";
        }
        std::cout << std::setw(2)
                  << static_cast<unsigned>(data[i]) << ' ';
        if ((i % 16) == 15 || i + 1 == m)
            std::cout << '\n';
    }
    std::cout << std::dec;
}

static void usage() {
    std::wcout
        << L"Usage:\n"
        << L"  raw_h2c_v5.exe <qid> <input.bin> [timeout_ms]\n\n"
        << L"Example:\n"
        << L"  raw_h2c_v5.exe 0 a5_1KB.bin 10000\n\n"
        << L"v5 mirrors dma-arw mode-0 completion handling more closely:\n"
        << L"  CreateFile flags 0x60000080\n"
        << L"  4096-byte aligned buffer\n"
        << L"  CreateIoCompletionPort\n"
        << L"  WriteFile(..., NULL, &OVERLAPPED)\n"
        << L"  GetQueuedCompletionStatusEx in 100-ms polling windows\n"
        << L"  completion determined from accumulated transferred byte count\n";
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 3 || argc > 4) {
        usage();
        return 1;
    }

    unsigned long qid = 0;
    DWORD timeout_ms = 10000;

    try {
        qid = std::stoul(argv[1]);
        if (argc == 4)
            timeout_ms = static_cast<DWORD>(std::stoul(argv[3]));
    } catch (...) {
        std::wcerr << L"ERROR: invalid qid/timeout\n";
        return 1;
    }

    if (qid > 511) {
        std::wcerr << L"ERROR: qid must be 0..511\n";
        return 1;
    }

    const std::filesystem::path input = argv[2];
    if (!std::filesystem::exists(input)) {
        std::wcerr << L"ERROR: input file does not exist: "
                   << input.wstring() << L"\n";
        return 1;
    }

    std::ifstream ifs(input, std::ios::binary | std::ios::ate);
    if (!ifs) {
        std::wcerr << L"ERROR: cannot open input file\n";
        return 1;
    }

    const std::streamoff end = ifs.tellg();
    if (end <= 0 || static_cast<unsigned long long>(end) > 0xffffffffULL) {
        std::wcerr << L"ERROR: invalid input size\n";
        return 1;
    }

    const DWORD length = static_cast<DWORD>(end);
    std::vector<std::uint8_t> file_data(length);

    ifs.seekg(0, std::ios::beg);
    if (!ifs.read(reinterpret_cast<char*>(file_data.data()), length)) {
        std::wcerr << L"ERROR: failed to read input file\n";
        return 1;
    }

    dump_prefix(file_data.data(), file_data.size(), "Input file");

    auto interfaces = enumerate_qdma_interfaces();
    if (interfaces.empty()) {
        std::wcerr << L"ERROR: no enabled QDMA interface found\n";
        return 2;
    }

    std::wstring base = interfaces.front();
    for (const auto& p : interfaces) {
        const auto lp = lower_copy(p);
        if (lp.find(L"ven_10ee") != std::wstring::npos &&
            lp.find(L"dev_b03f") != std::wstring::npos) {
            base = p;
            break;
        }
    }

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
        const DWORD e = GetLastError();
        std::wcerr << L"ERROR: CreateFileW failed, code="
                   << e << L" (" << winerr(e) << L")\n";
        return 3;
    }

    std::wcout << L"CreateFileW(queue) PASS, flags=0x"
               << std::hex << open_flags << std::dec << L"\n";

    SIZE_T alloc_size = static_cast<SIZE_T>(length) * 2;
    if (alloc_size < 4096)
        alloc_size = 4096;

    void* aligned = _aligned_malloc(alloc_size, 4096);
    if (!aligned) {
        std::wcerr << L"ERROR: _aligned_malloc failed\n";
        CloseHandle(h);
        return 4;
    }

    std::memset(aligned, 0, alloc_size);
    std::memcpy(aligned, file_data.data(), length);

    dump_prefix(aligned, length, "H2C user buffer BEFORE WriteFile");

    const auto addr = reinterpret_cast<std::uintptr_t>(aligned);
    std::wcout << L"Buffer address: 0x"
               << std::hex << addr << std::dec
               << L" (mod 4096 = " << (addr & 0xfff) << L")\n";

    HANDLE iocp = CreateIoCompletionPort(
        h, nullptr, static_cast<ULONG_PTR>(1), 0);

    if (!iocp) {
        const DWORD e = GetLastError();
        std::wcerr << L"ERROR: CreateIoCompletionPort failed, code="
                   << e << L" (" << winerr(e) << L")\n";
        _aligned_free(aligned);
        CloseHandle(h);
        return 5;
    }

    std::wcout << L"CreateIoCompletionPort PASS\n";

    OVERLAPPED ov{};
    ov.Offset = 0;
    ov.OffsetHigh = 0;
    ov.hEvent = nullptr;

    BOOL write_ok = WriteFile(
        h,
        aligned,
        length,
        nullptr,
        &ov);

    if (!write_ok) {
        const DWORD e = GetLastError();
        if (e != ERROR_IO_PENDING) {
            std::wcerr << L"ERROR: WriteFile failed immediately, code="
                       << e << L" (" << winerr(e) << L")\n";
            CloseHandle(iocp);
            _aligned_free(aligned);
            CloseHandle(h);
            return 6;
        }
        std::wcout << L"WriteFile -> ERROR_IO_PENDING (expected)\n";
    } else {
        std::wcout << L"WriteFile returned TRUE immediately\n";
    }

    // dma-arw's completion thread allocates room for 50 OVERLAPPED_ENTRY
    // objects and polls GetQueuedCompletionStatusEx with a 100-ms timeout.
    OVERLAPPED_ENTRY entries[50]{};

    DWORD total_bytes = 0;
    unsigned completion_count = 0;
    bool saw_nonzero_internal = false;

    const auto t0 = std::chrono::steady_clock::now();

    while (total_bytes < length) {
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0).count();

        if (elapsed >= timeout_ms) {
            std::wcerr << L"ERROR: timed out after "
                       << elapsed << L" ms; accumulated "
                       << total_bytes << L" / " << length << L" bytes\n";
            CancelIoEx(h, &ov);
            CloseHandle(iocp);
            _aligned_free(aligned);
            CloseHandle(h);
            return 7;
        }

        ULONG removed = 0;
        std::memset(entries, 0, sizeof(entries));
        SetLastError(ERROR_SUCCESS);

        const BOOL got = GetQueuedCompletionStatusEx(
            iocp,
            entries,
            50,
            &removed,
            100,
            FALSE);

        if (!got) {
            const DWORD e = GetLastError();

            // A 100-ms poll timeout is normal; dma-arw simply polls again.
            if (e == WAIT_TIMEOUT) {
                continue;
            }

            std::wcerr << L"ERROR: GetQueuedCompletionStatusEx failed, code="
                       << e << L" (" << winerr(e) << L")\n";
            CancelIoEx(h, &ov);
            CloseHandle(iocp);
            _aligned_free(aligned);
            CloseHandle(h);
            return 8;
        }

        std::wcout << L"Completion batch: " << removed << L" entr"
                   << (removed == 1 ? L"y" : L"ies") << L"\n";

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

            if (e.Internal != 0)
                saw_nonzero_internal = true;

            // This is exactly the important behavior seen in dma-arw:
            // completion is driven by accumulated byte counts.
            total_bytes += e.dwNumberOfBytesTransferred;
        }

        std::wcout << L"Accumulated bytes: "
                   << total_bytes << L" / " << length << L"\n";
    }

    std::wcout << L"RAW H2C COMPLETION PASS by dma-arw byte-count rule: "
               << total_bytes << L" / " << length << L" bytes\n";

    std::wcout << L"Original OVERLAPPED after completion:"
               << L" Internal=0x" << std::hex
               << static_cast<unsigned long long>(ov.Internal)
               << L" InternalHigh=0x"
               << static_cast<unsigned long long>(ov.InternalHigh)
               << std::dec << L"\n";

    if (saw_nonzero_internal || ov.Internal != 0) {
        std::wcout
            << L"WARNING: completion carried a non-zero Internal status.\n"
            << L"dma-arw's mode-0 completion thread does not use that field\n"
            << L"to decide transfer completion; it accumulates transferred bytes.\n";
    }

    dump_prefix(aligned, length, "H2C user buffer AFTER completion");

    // Keep behavior conservative: give the driver a short settling interval
    // before closing the queue handle.
    Sleep(200);

    CloseHandle(iocp);
    _aligned_free(aligned);
    CloseHandle(h);

    return 0;
}
