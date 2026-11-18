#include <winsock2.h>
#include <ws2bth.h>

#include "drivers.h"

#include <setupapi.h>
#include <initguid.h>
#include <devpkey.h>

#include <algorithm>
#include <cwchar>
#include <cwctype>

namespace {

const GUID kService = { 0xaeac4a03, 0xdff5, 0x498f, { 0x84, 0x3a, 0x34, 0x48, 0x7c, 0xf1, 0x33, 0xeb } };
const DEVPROPKEY kIsConnected = { { 0x83DA6326, 0x97A6, 0x4088, { 0x94, 0x53, 0xA1, 0x92, 0x3F, 0x57, 0x3B, 0x29 } }, 15 };

struct Ear {
    std::wstring name;
    std::wstring addressText;
    ULONGLONG address;
};

struct Battery {
    int left = -1;
    int right = -1;
    int caseLevel = -1;
    bool leftCharging = false;
    bool rightCharging = false;
};

bool IsEar2024(std::wstring name) {
    for (auto& ch : name) ch = towlower(ch);
    return name == L"nothing ear" || name == L"nothing ear (2024)" ||
           name == L"ear (2024)" || name == L"ear (b171)" ||
           name == L"nothing ear (b171)" || name == L"ear";
}

std::vector<Ear> FindConnected() {
    std::vector<Ear> devices;
    HDEVINFO di = SetupDiGetClassDevsW(nullptr, L"BTHENUM", nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (di == INVALID_HANDLE_VALUE) return devices;
    SP_DEVINFO_DATA dd{ sizeof(dd) };
    for (DWORD i = 0; SetupDiEnumDeviceInfo(di, i, &dd); ++i) {
        wchar_t instance[512];
        if (!SetupDiGetDeviceInstanceIdW(di, &dd, instance, 512, nullptr) ||
            _wcsnicmp(instance, L"BTHENUM\\DEV_", 12) != 0) continue;
        std::wstring hex(instance + 12, wcsnlen(instance + 12, 12));
        if (hex.size() != 12) continue;
        wchar_t* end = nullptr;
        ULONGLONG address = wcstoull(hex.c_str(), &end, 16);
        if (!address || *end) continue;
        DEVPROPTYPE type;
        DEVPROP_BOOLEAN connected = DEVPROP_FALSE;
        if (!SetupDiGetDevicePropertyW(di, &dd, &kIsConnected, &type, (PBYTE)&connected,
                                       sizeof(connected), nullptr, 0) ||
            type != DEVPROP_TYPE_BOOLEAN || connected == DEVPROP_FALSE) continue;
        wchar_t name[256] = {};
        if (!SetupDiGetDevicePropertyW(di, &dd, &DEVPKEY_Device_FriendlyName, &type, (PBYTE)name,
                                       sizeof(name) - sizeof(wchar_t), nullptr, 0) ||
            type != DEVPROP_TYPE_STRING || !IsEar2024(name)) continue;
        devices.push_back({ name, hex, address });
    }
    SetupDiDestroyDeviceInfoList(di);
    return devices;
}

bool StartWinsock() {
    static bool ready = [] {
        WSADATA data;
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    return ready;
}

bool WaitSocket(SOCKET socket, bool write, int milliseconds) {
    fd_set ready, errors;
    FD_ZERO(&ready);
    FD_ZERO(&errors);
    FD_SET(socket, &ready);
    FD_SET(socket, &errors);
    timeval timeout{ milliseconds / 1000, (milliseconds % 1000) * 1000 };
    int result = select(0, write ? nullptr : &ready, write ? &ready : nullptr, &errors, &timeout);
    return result > 0 && FD_ISSET(socket, &ready) && !FD_ISSET(socket, &errors);
}

unsigned short Crc16(const BYTE* data, size_t length) {
    unsigned short crc = 0xFFFF;
    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (int j = 0; j < 8; ++j)
            crc = (crc & 1) ? (unsigned short)((crc >> 1) ^ 0xA001) : (unsigned short)(crc >> 1);
    }
    return crc;
}

bool ParseFrame(std::vector<BYTE>& stream, Battery& battery) {
    while (stream.size() >= 10) {
        auto start = std::find(stream.begin(), stream.end(), (BYTE)0x55);
        stream.erase(stream.begin(), start);
        if (stream.size() < 10) return false;
        if (stream[1] != 0x60 || stream[2] != 0x01) {
            stream.erase(stream.begin());
            continue;
        }
        size_t length = 10 + stream[5];
        if (stream.size() < length) return false;
        unsigned short crc = Crc16(stream.data(), length - 2);
        bool valid = stream[length - 2] == (BYTE)crc && stream[length - 1] == (BYTE)(crc >> 8);
        unsigned command = stream[3] | (unsigned(stream[4]) << 8);
        if (valid && (command == 0x4007 || command == 0xE001)) {
            size_t payloadLength = stream[5];
            const BYTE* payload = stream.data() + 8;
            if (payloadLength >= 1) {
                Battery parsed;
                for (size_t i = 0; i < payload[0] && 2 + 2 * i < payloadLength; ++i) {
                    BYTE id = payload[1 + 2 * i];
                    BYTE level = payload[2 + 2 * i];
                    int percent = level & 0x7F;
                    if (percent > 100) continue;
                    if (id == 2) {
                        parsed.left = percent;
                        parsed.leftCharging = (level & 0x80) != 0;
                    } else if (id == 3) {
                        parsed.right = percent;
                        parsed.rightCharging = (level & 0x80) != 0;
                    } else if (id == 4) {
                        parsed.caseLevel = percent;
                    }
                }
                if (parsed.left >= 0 || parsed.right >= 0) {
                    battery = parsed;
                    return true;
                }
            }
        }
        stream.erase(stream.begin(), stream.begin() + length);
    }
    return false;
}

bool ReadBattery(ULONGLONG address, Battery& battery) {
    SOCKET socket = ::socket(AF_BTH, SOCK_STREAM, BTHPROTO_RFCOMM);
    if (socket == INVALID_SOCKET) return false;
    u_long nonblocking = 1;
    ioctlsocket(socket, FIONBIO, &nonblocking);
    SOCKADDR_BTH target{};
    target.addressFamily = AF_BTH;
    target.btAddr = address;
    target.serviceClassId = kService;
    target.port = 0;
    int status = connect(socket, (sockaddr*)&target, sizeof(target));
    bool connected = status == 0;
    if (!connected && WSAGetLastError() == WSAEWOULDBLOCK && WaitSocket(socket, true, 2500)) {
        int error = 0;
        int size = sizeof(error);
        connected = getsockopt(socket, SOL_SOCKET, SO_ERROR, (char*)&error, &size) == 0 && error == 0;
    }
    if (!connected) {
        closesocket(socket);
        return false;
    }
    BYTE request[10] = { 0x55, 0x60, 0x01, 0x07, 0xC0, 0, 0, 1, 0, 0 };
    unsigned short crc = Crc16(request, 8);
    request[8] = (BYTE)crc;
    request[9] = (BYTE)(crc >> 8);
    if (!WaitSocket(socket, true, 1000) || send(socket, (char*)request, sizeof(request), 0) != (int)sizeof(request)) {
        closesocket(socket);
        return false;
    }
    std::vector<BYTE> buffer;
    ULONGLONG deadline = GetTickCount64() + 2500;
    bool found = false;
    while (GetTickCount64() < deadline && buffer.size() < 8192) {
        int left = (int)(deadline - GetTickCount64());
        if (left <= 0 || !WaitSocket(socket, false, left)) break;
        char chunk[512];
        int bytes = recv(socket, chunk, sizeof(chunk), 0);
        if (bytes <= 0) break;
        buffer.insert(buffer.end(), chunk, chunk + bytes);
        if (ParseFrame(buffer, battery)) {
            found = true;
            break;
        }
    }
    closesocket(socket);
    return found;
}

std::wstring Percentage(int level) {
    return level >= 0 ? std::to_wstring(level) + L"%" : L"-";
}

}

void PollNothingEar(PollContext& ctx) {
    auto devices = FindConnected();
    if (devices.empty() || !StartWinsock()) return;
    for (const Ear& ear : devices) {
        Battery battery;
        if (!ReadBattery(ear.address, battery)) continue;
        DeviceStatus status;
        status.key = L"nothingear:" + ear.addressText;
        status.name = ear.name;
        status.via = L"Bluetooth";
        status.kind = DeviceKind::Headset;
        status.percent = battery.left >= 0 && battery.right >= 0
                             ? std::min(battery.left, battery.right)
                             : std::max(battery.left, battery.right);
        status.charging = battery.leftCharging || battery.rightCharging;
        status.detail = L"L " + Percentage(battery.left) + L", R " + Percentage(battery.right);
        if (battery.caseLevel >= 0) status.detail += L", case " + Percentage(battery.caseLevel);
        status.graceSec = 180;
        ctx.out.push_back(status);
    }
}
