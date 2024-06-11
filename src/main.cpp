#include <iostream>
#include <fstream>
#include <unordered_map>
#include <vector>
#include <string>
#include <PcapLiveDeviceList.h>
#include <clipp.h>

#if defined(__APPLE__)

#include <SystemConfiguration/SystemConfiguration.h>

#endif

#include "exploit.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <mmsystem.h>
#endif

std::vector<uint8_t> readBinary(const std::string &filename) {
    std::ifstream file(filename, std::ios::binary | std::ios::ate);
    if (!file) {
        std::cout << "[-] Cannot open: " << filename << std::endl;
        return {};
    }

    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);

    std::vector<uint8_t> buffer(size);
    if (!file.read(reinterpret_cast<char *>(buffer.data()), size)) {
        std::cout << "[-] Cannot read: " << filename << std::endl;
        return {};
    }

    return buffer;
}

void listInterfaces() {
    std::cout << "[+] interfaces: " << std::endl;
#if defined(__APPLE__)
    CFArrayRef interfaces = SCNetworkInterfaceCopyAll();
    if (!interfaces) {
        std::cerr << "[-] Failed to get interfaces" << std::endl;
        exit(1);
    }
    CFIndex serviceCount = CFArrayGetCount(interfaces);
    char buffer[1024];
    for (CFIndex i = 0; i < serviceCount; ++i) {
        auto interface = (SCNetworkInterfaceRef) CFArrayGetValueAtIndex(interfaces, i);
        auto serviceName = SCNetworkInterfaceGetLocalizedDisplayName(interface);
        auto bsdName = SCNetworkInterfaceGetBSDName(interface);
        if (bsdName) {
            CFStringGetCString(bsdName, buffer, sizeof(buffer), kCFStringEncodingUTF8);
            printf("\t%s ", buffer);
            if (serviceName) {
                CFStringGetCString(serviceName, buffer, sizeof(buffer), kCFStringEncodingUTF8);
                printf("%s", buffer);
            }
            printf("\n");
        }
    }
    CFRelease(interfaces);
#else
    std::vector<pcpp::PcapLiveDevice *> devList = pcpp::PcapLiveDeviceList::getInstance().getPcapLiveDevicesList();
    for (pcpp::PcapLiveDevice *dev: devList) {
        if (dev->getLoopback()) continue;
        std::cout << "\t" << dev->getName() << " " << dev->getDesc() << std::endl;
    }
#endif
    exit(0);
}

enum FirmwareVersion getFirmwareOffset(int fw) {
    std::unordered_map<int, enum FirmwareVersion> fw_choices = {
            {700,  FIRMWARE_700_702},
            {701,  FIRMWARE_700_702},
            {702,  FIRMWARE_700_702},
            {750,  FIRMWARE_750_755},
            {750,  FIRMWARE_750_755},
            {751,  FIRMWARE_750_755},
            {755,  FIRMWARE_750_755},
            {800,  FIRMWARE_800_803},
            {801,  FIRMWARE_800_803},
            {803,  FIRMWARE_800_803},
            {850,  FIRMWARE_850_852},
            {852,  FIRMWARE_850_852},
            {900,  FIRMWARE_900},
            {903,  FIRMWARE_903_904},
            {904,  FIRMWARE_903_904},
            {950,  FIRMWARE_950_960},
            {951,  FIRMWARE_950_960},
            {960,  FIRMWARE_950_960},
            {1000, FIRMWARE_1000_1001},
            {1001, FIRMWARE_1000_1001},
            {1050, FIRMWARE_1050_1071},
            {1070, FIRMWARE_1050_1071},
            {1071, FIRMWARE_1050_1071},
            {1100, FIRMWARE_1100}
    };
    if (fw_choices.count(fw) == 0) return FIRMWARE_UNKNOWN;
    return fw_choices[fw];
}

#define SUPPORTED_FIRMWARE "{700,701,702,750,751,755,800,801,803,850,852,900,903,904,950,951,960,1000,1001,1050,1070,1071,1100} (default: 1100)"

int main(int argc, char *argv[]) {
    using namespace clipp;
    std::cout << "[+] PPPwn++ - PlayStation 4 PPPoE RCE by theflow" << std::endl;
    std::string interface, stage1 = "stage1.bin", stage2 = "stage2.bin";
    int fw = 1100;
    int timeout = 0;
    bool retry = true;
    bool no_wait_padi = true;

    auto cli = (
            ("network interface" % required("--interface") & value("interface", interface), \
            SUPPORTED_FIRMWARE % option("--fw") & integer("fw", fw), \
            "stage1 binary (default: stage1/stage1.bin)" % option("-s1", "--stage1") & value("STAGE1", stage1), \
            "stage2 binary (default: stage2/stage2.bin)" % option("-s2", "--stage2") & value("STAGE2", stage2), \
            "timeout in seconds for ps4 response, 0 means always wait (default: 0)" %
            option("-t", "--timeout") & integer("seconds", timeout), \
            "automatically retry when fails or timeout" % option("-a", "--auto-retry").set(retry), \
            "don't wait one more PADI before starting" % option("-nw", "--no-wait-padi").set(no_wait_padi)
            ) | \
            "list interfaces" % command("list").call(listInterfaces)
    );

    auto result = parse(argc, argv, cli);
    if (!result) {
        std::cout << make_man_page(cli, "pppwn");
        return 1;
    }

    auto offset = getFirmwareOffset(fw);
    if (offset == FIRMWARE_UNKNOWN) {
        std::cerr << "[-] Invalid firmware version" << std::endl;
        std::cout << make_man_page(cli, "pppwn");
        return 1;
    }

    std::cout << "[+] args: interface=" << interface << " fw=" << fw << " stage1=" << stage1 << " stage2=" << stage2
              << " timeout=" << timeout
              << " auto-retry=" << (retry ? "on" : "off") << " no-wait-padi=" << (no_wait_padi ? "on" : "off")
              << std::endl;

#ifdef _WIN32

    timeBeginPeriod(1);
    std::atexit([](){
        timeEndPeriod(1);
    });
#endif

    Exploit exploit;
    if (exploit.setFirmwareVersion((FirmwareVersion) fw)) return 1;
    if (exploit.setInterface(interface)) return 1;
    auto stage1_data = readBinary(stage1);
    if (stage1_data.empty()) return 1;
    auto stage2_data = readBinary(stage2);
    if (stage2_data.empty()) return 1;
    exploit.setStage1(std::move(stage1_data));
    exploit.setStage2(std::move(stage2_data));
    exploit.setTimeout(timeout);
    exploit.setWaitPADI(!no_wait_padi);

    if (!retry) return exploit.run();

    while (exploit.run() != 0) {
        exploit.ppp_byebye();
        std::cerr << "[*] Retry after 5s..." << std::endl;
        std::this_thread::sleep_for(std::chrono::seconds(5));
    }
    return 0;
}
