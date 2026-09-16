//
//  IntelBTPatcher.h
//  IntelBTPatcher
//
//  Created by zxystd <zxystd@foxmail.com> on 2021/2/8.
//

#ifndef IntelBTPatcher_h
#define IntelBTPatcher_h

#include <Headers/kern_patcher.hpp>

#include <IOKit/IOLocks.h>
#include <IOKit/usb/IOUSBHostDevice.h>

#define DRV_NAME "ibtp"

class BluetoothDeviceAddress;

typedef struct {
    void *owner;
    IOMemoryDescriptor *dataBuffer;
    IOUSBHostCompletionAction action;
    // True while this transfer holds an extra prepare() on dataBuffer, which is
    // what makes the buffer safe to read from the completion.
    bool prepared;
} AsyncOwnerData;

typedef struct __attribute__((packed))
{
    uint16_t    opcode;    /* OCF & OGF */
    uint8_t     len;
    uint8_t     data[];
} HciCommandHdr;

typedef struct __attribute__((packed))
{
    uint8_t     evt;
    uint8_t     len;
    uint8_t     data[];
} HciEventHdr;

const char *requestDirectionNames[] = {
    "OUT",
    "IN"
};

const char *requestTypeNames[] = {
    "Standard",
    "Class",
    "Vendor"
};

const char *requestRecipientNames[] = {
    "Device",
    "Interface",
    "Endpoint",
    "Other"
};

const char* _hexDumpHCIData(uint8_t *buf, size_t len)
{
    ssize_t str_len = len * 3 + 1;
    char *str = (char*)IOMalloc(str_len);
    if (!str)
        return nullptr;
    for (size_t i = 0; i < len; i++)
    snprintf(str + 3 * i, (len - i) * 3, "%02x ", buf[i]);
    str[MAX(str_len - 2, 0)] = 0;
    return str;
}

class CIntelBTPatcher {
public:
    bool init();
    void free();
    
    void processKext(KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size);
    static IOReturn newFindQueueRequest(void *that, unsigned short arg1, void *addr, unsigned short arg2, bool arg3, void **hciRequestPtr);
    
    static IOReturn newHostDeviceRequest(void *that, IOService *provider, StandardUSB::DeviceRequest &request, void *data, IOMemoryDescriptor *descriptor, unsigned int &length,IOUSBHostCompletion *completion, unsigned int timeout);
    static IOReturn newAsyncIO(void *that, IOMemoryDescriptor* dataBuffer, uint32_t dataBufferLength, IOUSBHostCompletion* completion, uint32_t completionTimeoutMs);
    // The second parameter is a SuperSpeedEndpointCompanionDescriptor up to
    // macOS 15 and a ConfigurationDescriptor from macOS 26 on, which changes the
    // mangled name. It is never dereferenced here, so keep it type-agnostic and
    // let one routine serve both signatures.
    static int newInitPipe(void *that, StandardUSB::EndpointDescriptor const *descriptor, const void *companionOrConfigDescriptor, AppleUSBHostController *controller, IOUSBHostDevice *device, IOUSBHostInterface *interface, unsigned char, unsigned short);

    static void asyncIOCompletion(void *owner, void *parameter, IOReturn status, uint32_t bytesTransferred);
    // Returns true when this is the second Read Remote Features Complete seen
    // for the handle, i.e. the duplicate that may be rewritten.
    static bool consumeFeatureCompleteForHandle(uint16_t handle);
    // Called once the duplicate command really went out, so a completion is
    // only ever rewritten when a second one is genuinely expected.
    static bool armFeatureHandle(uint16_t handle);
    static void resetFeatureHandles();

    
    mach_vm_address_t oldFindQueueRequest {};
    mach_vm_address_t oldHostDeviceRequest {};
    mach_vm_address_t oldAsyncIO {};
    mach_vm_address_t oldInitPipe {};
    
private:
    // Bounded: concurrent LE connections are few, and a full table just means
    // events are passed through untouched.
    static constexpr uint32_t kMaxPendingFeatureHandles = 8;

    static void *_hookPipeInstance;
    static IOSimpleLock *_stateLock;
    static uint16_t _pendingFeatureHandles[kMaxPendingFeatureHandles];
    static uint32_t _pendingFeatureHandleCount;
    static bool _randomAddressInit;
};

#endif /* IntelBTPatcher_h */
