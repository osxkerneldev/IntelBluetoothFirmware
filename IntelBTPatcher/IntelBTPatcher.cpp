//
//  IntelBTPatcher.cpp
//  IntelBTPatcher
//
//  Created by zxystd <zxystd@foxmail.com> on 2021/2/8.
//

#include <Headers/kern_api.hpp>
#include <Headers/kern_util.hpp>
#include <Headers/plugin_start.hpp>

#include "IntelBTPatcher.hpp"

static CIntelBTPatcher ibtPatcher;
static CIntelBTPatcher *callbackIBTPatcher = nullptr;

static const char *bootargOff[] {
    "-ibtcompatoff"
};

static const char *bootargDebug[] {
    "-ibtcompatdbg"
};

static const char *bootargBeta[] {
    "-ibtcompatbeta"
};

PluginConfiguration ADDPR(config) {
    xStringify(PRODUCT_NAME),
    parseModuleVersion(xStringify(MODULE_VERSION)),
    LiluAPI::AllowNormal | LiluAPI::AllowInstallerRecovery | LiluAPI::AllowSafeMode,
    bootargOff,
    arrsize(bootargOff),
    bootargDebug,
    arrsize(bootargDebug),
    bootargBeta,
    arrsize(bootargBeta),
    KernelVersion::MountainLion,
    KernelVersion::Tahoe,
    []() {
        ibtPatcher.init();
    }
};

static const char *IntelBTPatcher_IOBluetoothFamily[] { "/System/Library/Extensions/IOBluetoothFamily.kext/Contents/MacOS/IOBluetoothFamily" };

static KernelPatcher::KextInfo IntelBTPatcher_IOBluetoothInfo {
    "com.apple.iokit.IOBluetoothFamily",
    IntelBTPatcher_IOBluetoothFamily,
    1,
    {true, true},
    {},
    KernelPatcher::KextInfo::Unloaded
};

static const char *IntelBTPatcher_IOUSBHostFamily[] {
    "/System/Library/Extensions/IOUSBHostFamily.kext/Contents/MacOS/IOUSBHostFamily" };

static KernelPatcher::KextInfo IntelBTPatcher_IOUsbHostInfo {
    "com.apple.iokit.IOUSBHostFamily",
    IntelBTPatcher_IOUSBHostFamily,
    1,
    {true, true},
    {},
    KernelPatcher::KextInfo::Unloaded
};

void *CIntelBTPatcher::_hookPipeInstance = nullptr;
IOSimpleLock *CIntelBTPatcher::_stateLock = nullptr;
uint16_t CIntelBTPatcher::_pendingFeatureHandles[CIntelBTPatcher::kMaxPendingFeatureHandles] = {};
uint32_t CIntelBTPatcher::_pendingFeatureHandleCount = 0;
bool CIntelBTPatcher::_randomAddressInit = false;

bool CIntelBTPatcher::init()
{
    DBGLOG(DRV_NAME, "%s", __PRETTY_FUNCTION__);
    callbackIBTPatcher = this;
    // Guards the pending-handle table, which is touched from USB completions.
    // If it cannot be allocated the event rewrite simply stays disabled.
    if (_stateLock == nullptr)
        _stateLock = IOSimpleLockAlloc();
    if (_stateLock == nullptr)
        SYSLOG(DRV_NAME, "failed to allocate state lock, LE PHY workaround disabled");
    if (getKernelVersion() < KernelVersion::Monterey) {
        lilu.onKextLoadForce(&IntelBTPatcher_IOBluetoothInfo, 1,
        [](void *user, KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size) {
            callbackIBTPatcher->processKext(patcher, index, address, size);
        }, this);
    } else {
        lilu.onKextLoadForce(&IntelBTPatcher_IOUsbHostInfo, 1,
        [](void *user, KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size) {
            callbackIBTPatcher->processKext(patcher, index, address, size);
        }, this);
    }
    return true;
}

void CIntelBTPatcher::free()
{
    DBGLOG(DRV_NAME, "%s", __PRETTY_FUNCTION__);
    _hookPipeInstance = nullptr;
    if (_stateLock != nullptr) {
        IOSimpleLockFree(_stateLock);
        _stateLock = nullptr;
    }
    _pendingFeatureHandleCount = 0;
}

void CIntelBTPatcher::processKext(KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size)
{
    DBGLOG(DRV_NAME, "%s", __PRETTY_FUNCTION__);
    if (getKernelVersion() < KernelVersion::Monterey) {
        if (IntelBTPatcher_IOBluetoothInfo.loadIndex == index) {
            DBGLOG(DRV_NAME, "%s", IntelBTPatcher_IOBluetoothInfo.id);
            
            KernelPatcher::RouteRequest findQueueRequestRequest {
                "__ZN25IOBluetoothHostController17FindQueuedRequestEtP22BluetoothDeviceAddresstbPP21IOBluetoothHCIRequest",
                newFindQueueRequest,
                oldFindQueueRequest
            };
            patcher.routeMultiple(index, &findQueueRequestRequest, 1, address, size);
            if (patcher.getError() == KernelPatcher::Error::NoError) {
                DBGLOG(DRV_NAME, "routed %s", findQueueRequestRequest.symbol);
            } else {
                SYSLOG(DRV_NAME, "failed to resolve %s, error = %d", findQueueRequestRequest.symbol, patcher.getError());
                patcher.clearError();
            }
            
        }
    } else {
        if (IntelBTPatcher_IOUsbHostInfo.loadIndex == index) {
            SYSLOG(DRV_NAME, "%s", IntelBTPatcher_IOUsbHostInfo.id);
            
            KernelPatcher::RouteRequest hostDeviceRequest {
            "__ZN15IOUSBHostDevice13deviceRequestEP9IOServiceRN11StandardUSB13DeviceRequestEPvP18IOMemoryDescriptorRjP19IOUSBHostCompletionj",
                newHostDeviceRequest,
                oldHostDeviceRequest
            };
            patcher.routeMultiple(index, &hostDeviceRequest, 1, address, size);
            if (patcher.getError() == KernelPatcher::Error::NoError) {
                SYSLOG(DRV_NAME, "routed %s", hostDeviceRequest.symbol);
            } else {
                SYSLOG(DRV_NAME, "failed to resolve %s, error = %d", hostDeviceRequest.symbol, patcher.getError());
                patcher.clearError();
            }

            KernelPatcher::RouteRequest asyncIORequest {
            "__ZN13IOUSBHostPipe2ioEP18IOMemoryDescriptorjP19IOUSBHostCompletionj",
                newAsyncIO,
                oldAsyncIO
            };
            patcher.routeMultiple(index, &asyncIORequest, 1, address, size);
            if (patcher.getError() == KernelPatcher::Error::NoError) {
                SYSLOG(DRV_NAME, "routed %s", asyncIORequest.symbol);
            } else {
                SYSLOG(DRV_NAME, "failed to resolve %s, error = %d", asyncIORequest.symbol, patcher.getError());
                patcher.clearError();
            }

            KernelPatcher::RouteRequest initPipeRequest {
            "__ZN13IOUSBHostPipe28initWithDescriptorsAndOwnersEPKN11StandardUSB18EndpointDescriptorEPKNS0_37SuperSpeedEndpointCompanionDescriptorEP22AppleUSBHostControllerP15IOUSBHostDeviceP18IOUSBHostInterfaceht",
                newInitPipe,
                oldInitPipe
            };
            patcher.routeMultiple(index, &initPipeRequest, 1, address, size);
            if (patcher.getError() == KernelPatcher::Error::NoError) {
                SYSLOG(DRV_NAME, "routed %s", initPipeRequest.symbol);
            } else {
                SYSLOG(DRV_NAME, "failed to resolve %s, error = %d", initPipeRequest.symbol, patcher.getError());
                patcher.clearError();
            }
        }
    }
}

#pragma mark - For Bigsur-, patch unhandled 0x2019 opcode

#define HCI_OP_LE_START_ENCRYPTION 0x2019

IOReturn CIntelBTPatcher::newFindQueueRequest(void *that, unsigned short arg1, void *addr, unsigned short arg2, bool arg3, void **hciRequestPtr)
{
    IOReturn ret = FunctionCast(newFindQueueRequest, callbackIBTPatcher->oldFindQueueRequest)(that, arg1, addr, arg2, arg3, hciRequestPtr);
    if (ret != 0 && arg1 == HCI_OP_LE_START_ENCRYPTION) {
        ret = FunctionCast(newFindQueueRequest, callbackIBTPatcher->oldFindQueueRequest)(that, arg1, addr, 0xffff, arg3, hciRequestPtr);
        DBGLOG(DRV_NAME, "%s ret: %d arg1: 0x%04x arg2: 0x%04x arg3: %d ptr: %p", __FUNCTION__, ret, arg1, arg2, arg3, *hciRequestPtr);
    }
    return ret;
}

#pragma mark - For Monterey+ patch for intercept HCI REQ and RESP

StandardUSB::DeviceRequest randomAddressRequest;
// Hardcoded Random Address HCI Command
const uint8_t randomAddressHci[9] = {0x05, 0x20, 0x06, 0x94, 0x50, 0x64, 0xD0, 0x78, 0x6B}; 
IOBufferMemoryDescriptor *writeHCIDescriptor = nullptr;

#define MAX_HCI_BUF_LEN                 255
#define HCI_OP_RESET                    0x0c03
#define HCI_OP_LE_SET_SCAN_PARAM        0x200B
#define HCI_OP_LE_SET_SCAN_ENABLE       0x200C
#define HCI_OP_LE_READ_REMOTE_FEATURES  0x2016

IOReturn CIntelBTPatcher::newHostDeviceRequest(void *that, IOService *provider, StandardUSB::DeviceRequest &request, void *data, IOMemoryDescriptor *descriptor, unsigned int &length, IOUSBHostCompletion *completion, unsigned int timeout)
{
    HciCommandHdr *hdr = nullptr;
    uint32_t hdrLen = 0;
    char hciBuf[MAX_HCI_BUF_LEN] = {0};
    
    if (data == nullptr) {
        if (descriptor != nullptr &&
            (getKernelVersion() < KernelVersion::Sequoia || !descriptor->prepare(kIODirectionOut))) {
            if (descriptor->getLength() > 0) {
                descriptor->readBytes(0, hciBuf, min(descriptor->getLength(), MAX_HCI_BUF_LEN));
                hdrLen = (uint32_t)min(descriptor->getLength(), MAX_HCI_BUF_LEN);
            }
            if (getKernelVersion() >= KernelVersion::Sequoia)
                descriptor->complete(kIODirectionOut);
        }
        hdr = (HciCommandHdr *)hciBuf;
        if (hdr->opcode == HCI_OP_LE_SET_SCAN_PARAM) {
            if (!_randomAddressInit) {
                randomAddressRequest.bmRequestType = makeDeviceRequestbmRequestType(kRequestDirectionOut, kRequestTypeClass, kRequestRecipientInterface);
                randomAddressRequest.bRequest = 0xE0;
                randomAddressRequest.wIndex = 0;
                randomAddressRequest.wValue = 0;
                randomAddressRequest.wLength = 9;
                length = 9;
                if (writeHCIDescriptor == nullptr)
                    writeHCIDescriptor = IOBufferMemoryDescriptor::withBytes(randomAddressHci, 9, kIODirectionOut);
                if (writeHCIDescriptor != nullptr) {
                    writeHCIDescriptor->prepare(kIODirectionOut);
                    IOReturn ret = FunctionCast(newHostDeviceRequest, callbackIBTPatcher->oldHostDeviceRequest)(that, provider, randomAddressRequest, nullptr, writeHCIDescriptor, length, nullptr, timeout);
                    writeHCIDescriptor->complete();
                    const char *randAddressDump = _hexDumpHCIData((uint8_t *)randomAddressHci, 9);
                    if (randAddressDump) {
                        SYSLOG(DRV_NAME, "[PATCH] Sending Random Address HCI %d %s", ret, randAddressDump);
                        IOFree((void *)randAddressDump, 9 * 3 + 1);
                    }
                    _randomAddressInit = true;
                    SYSLOG(DRV_NAME, "[PATCH] Resend LE SCAN PARAM HCI %d", ret);
                } else {
                    SYSLOG(DRV_NAME, "[PATCH] Failed to allocate Random Address HCI descriptor");
                }
            }
        } else if (hdr->opcode == HCI_OP_LE_READ_REMOTE_FEATURES &&
                   hdrLen >= sizeof(HciCommandHdr) + 2) {
            // Issue a duplicate so a second Read Remote Features Complete comes
            // back for this handle; the completion path turns that spare event
            // into the LE PHY Update Complete that Ventura+ waits for. The
            // request and length are copied so the caller's own call is
            // untouched by anything the inner call writes back.
            uint16_t connectionHandle = (uint16_t)(hdr->data[0] | (hdr->data[1] << 8));
            StandardUSB::DeviceRequest extraRequest = request;
            unsigned int extraLength = length;
            IOReturn ret = FunctionCast(newHostDeviceRequest, callbackIBTPatcher->oldHostDeviceRequest)(that, provider, extraRequest, nullptr, descriptor, extraLength, nullptr, timeout);
            if (ret == kIOReturnSuccess)
                armFeatureHandle(connectionHandle);
            SYSLOG(DRV_NAME, "[PATCH] Sending extra LE Read Remote Features command %d handle 0x%04x", ret, connectionHandle);
        }
    } else {
        hdr = (HciCommandHdr *)data;
        // wLength counts the whole command; anything shorter than the header
        // would underflow the unsigned subtraction below.
        hdrLen = request.wLength >= sizeof(HciCommandHdr) ? request.wLength - (uint32_t)sizeof(HciCommandHdr) : 0;
    }
    if (hdr) {
        // HCI reset, we need to send Random address again
        if (hdr->opcode == HCI_OP_RESET) {
            _randomAddressInit = false;
            // Connection handles do not survive a controller reset.
            resetFeatureHandles();
        }
#if DEBUG
        DBGLOG(DRV_NAME, "[%s] bRequest: 0x%x direction: %s type: %s recipient: %s wValue: 0x%02x wIndex: 0x%02x opcode: 0x%04x len: %d length: %d async: %d", provider->getName(), request.bRequest, requestDirectionNames[(request.bmRequestType & kDeviceRequestDirectionMask) >> kDeviceRequestDirectionPhase], requestRecipientNames[(request.bmRequestType & kDeviceRequestRecipientMask) >> kDeviceRequestRecipientPhase], requestTypeNames[(request.bmRequestType & kDeviceRequestTypeMask) >> kDeviceRequestTypePhase], request.wValue, request.wIndex, hdr->opcode, hdr->len, request.wLength, completion != nullptr);
        if (hdrLen) {
            const char *dump = _hexDumpHCIData((uint8_t *)hdr, hdrLen);
            if (dump) {
                DBGLOG(DRV_NAME, "[Request]: %s", dump);
                IOFree((void *)dump, hdrLen * 3 + 1);
            }
        }
#endif
    }
    return FunctionCast(newHostDeviceRequest, callbackIBTPatcher->oldHostDeviceRequest)(that, provider, request, data, descriptor, length, completion, timeout);
}

#define HCI_EVT_LE_META                               0x3E
#define HCI_EVT_LE_META_READ_REMOTE_FEATURES_COMPLETE 0x04
#define HCI_EVT_LE_META_PHY_UPDATE_COMPLETE           0x0C

// evt(1) plen(1) subevent(1) status(1) handle(2)
#define HCI_EVT_LE_META_MIN_LEN                       6
#define HCI_EVT_PHY_UPDATE_COMPLETE_LEN               8

// High bit of a stored handle marks "first completion already seen". Real
// connection handles are 12 bits, so the bit is always free.
#define FEATURE_HANDLE_SEEN_FLAG                      0x8000

bool CIntelBTPatcher::armFeatureHandle(uint16_t handle)
{
    if (_stateLock == nullptr)
        return false;

    bool armed = false;
    IOInterruptState state = IOSimpleLockLockDisableInterrupt(_stateLock);
    for (uint32_t i = 0; i < _pendingFeatureHandleCount; i++) {
        if ((_pendingFeatureHandles[i] & ~FEATURE_HANDLE_SEEN_FLAG) == handle) {
            // Already waiting on this handle; re-arm rather than duplicate it.
            _pendingFeatureHandles[i] = handle;
            armed = true;
            break;
        }
    }
    if (!armed && _pendingFeatureHandleCount < kMaxPendingFeatureHandles) {
        _pendingFeatureHandles[_pendingFeatureHandleCount++] = handle;
        armed = true;
    }
    IOSimpleLockUnlockEnableInterrupt(_stateLock, state);
    return armed;
}

bool CIntelBTPatcher::consumeFeatureCompleteForHandle(uint16_t handle)
{
    if (_stateLock == nullptr)
        return false;

    bool rewrite = false;
    IOInterruptState state = IOSimpleLockLockDisableInterrupt(_stateLock);
    for (uint32_t i = 0; i < _pendingFeatureHandleCount; i++) {
        if ((_pendingFeatureHandles[i] & ~FEATURE_HANDLE_SEEN_FLAG) != handle)
            continue;
        if (_pendingFeatureHandles[i] & FEATURE_HANDLE_SEEN_FLAG) {
            // Second completion for this handle: this is the spare one.
            _pendingFeatureHandles[i] = _pendingFeatureHandles[_pendingFeatureHandleCount - 1];
            _pendingFeatureHandleCount--;
            rewrite = true;
        } else {
            // First completion carries the real remote features; pass it on.
            _pendingFeatureHandles[i] |= FEATURE_HANDLE_SEEN_FLAG;
        }
        break;
    }
    IOSimpleLockUnlockEnableInterrupt(_stateLock, state);
    return rewrite;
}

void CIntelBTPatcher::resetFeatureHandles()
{
    if (_stateLock == nullptr)
        return;
    IOInterruptState state = IOSimpleLockLockDisableInterrupt(_stateLock);
    _pendingFeatureHandleCount = 0;
    IOSimpleLockUnlockEnableInterrupt(_stateLock, state);
}

void CIntelBTPatcher::asyncIOCompletion(void *owner, void *parameter, IOReturn status, uint32_t bytesTransferred)
{
    AsyncOwnerData *asyncOwner = (AsyncOwnerData *)owner;
    if (asyncOwner == nullptr)
        return;

    // Take a copy and release the per-transfer context straight away, so the
    // wrapper cannot leak no matter which path we take below.
    IOUSBHostCompletionAction action = asyncOwner->action;
    void *realOwner = asyncOwner->owner;
    IOMemoryDescriptor *dataBuffer = asyncOwner->dataBuffer;
    IOFree(asyncOwner, sizeof(AsyncOwnerData));

    if (dataBuffer != nullptr &&
        bytesTransferred >= HCI_EVT_LE_META_MIN_LEN &&
        dataBuffer->getLength() >= HCI_EVT_PHY_UPDATE_COMPLETE_LEN) {
        uint8_t evtBuf[HCI_EVT_LE_META_MIN_LEN] = {0};
        if (dataBuffer->readBytes(0, evtBuf, sizeof(evtBuf)) == sizeof(evtBuf)) {
            const HciEventHdr *hdr = (const HciEventHdr *)evtBuf;
            // Only touch a complete LE meta event that actually fits in what
            // the controller sent; anything shorter is left alone.
            if (hdr->evt == HCI_EVT_LE_META &&
                (uint32_t)hdr->len + 2 <= bytesTransferred &&
                hdr->data[0] == HCI_EVT_LE_META_READ_REMOTE_FEATURES_COMPLETE) {
                uint16_t handle = (uint16_t)(evtBuf[4] | (evtBuf[5] << 8));
                if (consumeFeatureCompleteForHandle(handle)) {
                    uint8_t phyUpdateComplete[HCI_EVT_PHY_UPDATE_COMPLETE_LEN] = {
                        HCI_EVT_LE_META, 0x06, HCI_EVT_LE_META_PHY_UPDATE_COMPLETE,
                        0x00, evtBuf[4], evtBuf[5], 0x02, 0x02
                    };
                    if (dataBuffer->writeBytes(0, phyUpdateComplete, sizeof(phyUpdateComplete)) == sizeof(phyUpdateComplete)) {
                        // Report the rewritten event's real size rather than the
                        // longer one the controller sent.
                        bytesTransferred = HCI_EVT_PHY_UPDATE_COMPLETE_LEN;
                    }
                }
            }
        }
    }

    if (action != nullptr)
        action(realOwner, parameter, status, bytesTransferred);
}

IOReturn CIntelBTPatcher::
newAsyncIO(void *that, IOMemoryDescriptor* dataBuffer, uint32_t bytesTransferred, IOUSBHostCompletion* completion, uint32_t completionTimeoutMs)
{
    // One context per transfer: the interrupt pipe keeps several reads in
    // flight, so a single shared wrapper would hand a completion the wrong
    // buffer and the wrong caller.
    AsyncOwnerData *asyncOwner = nullptr;
    if (that != nullptr && that == _hookPipeInstance &&
        completion != nullptr && completion->action != nullptr) {
        asyncOwner = (AsyncOwnerData *)IOMalloc(sizeof(AsyncOwnerData));
        if (asyncOwner != nullptr) {
            asyncOwner->owner = completion->owner;
            asyncOwner->action = completion->action;
            asyncOwner->dataBuffer = dataBuffer;
            completion->owner = asyncOwner;
            completion->action = asyncIOCompletion;
        }
    }

    IOReturn ret = FunctionCast(newAsyncIO, callbackIBTPatcher->oldAsyncIO)(that, dataBuffer, bytesTransferred, completion, completionTimeoutMs);

    if (asyncOwner != nullptr && ret != kIOReturnSuccess) {
        // The completion will never fire, so undo the swap and reclaim it here.
        completion->owner = asyncOwner->owner;
        completion->action = asyncOwner->action;
        IOFree(asyncOwner, sizeof(AsyncOwnerData));
    }
    return ret;
}

#define VENDOR_USB_INTEL                0x8087
#define USB_CLASS_WIRELESS_CONTROLLER   0xE0
#define USB_SUBCLASS_RF_CONTROLLER      0x01
#define USB_PROTOCOL_BLUETOOTH          0x01

int CIntelBTPatcher::
newInitPipe(void *that, StandardUSB::EndpointDescriptor const *descriptor, StandardUSB::SuperSpeedEndpointCompanionDescriptor const *superDescriptor, AppleUSBHostController *controller, IOUSBHostDevice *device, IOUSBHostInterface *interface, unsigned char a7, unsigned short a8)
{
    int ret = FunctionCast(newInitPipe, callbackIBTPatcher->oldInitPipe)(that, descriptor, superDescriptor, controller, device, interface, a7, a8);
    if (device != nullptr && descriptor != nullptr) {
        const StandardUSB::DeviceDescriptor *deviceDescriptor = device->getDeviceDescriptor();
        // Match the Bluetooth function specifically, not merely any Intel USB
        // device that happens to expose an interrupt endpoint.
        if (deviceDescriptor != nullptr &&
            deviceDescriptor->idVendor == VENDOR_USB_INTEL &&
            deviceDescriptor->bDeviceClass == USB_CLASS_WIRELESS_CONTROLLER &&
            deviceDescriptor->bDeviceSubClass == USB_SUBCLASS_RF_CONTROLLER &&
            deviceDescriptor->bDeviceProtocol == USB_PROTOCOL_BLUETOOTH) {
            uint8_t epType = StandardUSB::getEndpointType(descriptor);
            if (epType == kIOUSBEndpointTypeInterrupt) {
                _hookPipeInstance = that;
                _randomAddressInit = false;
                // The controller is starting over; stale handles mean nothing.
                resetFeatureHandles();
                SYSLOG(DRV_NAME, "[PATCH] Hooked Intel Bluetooth interrupt pipe %p", that);
            }
        } else if (deviceDescriptor != nullptr &&
                   deviceDescriptor->idVendor == VENDOR_USB_INTEL) {
            // Makes it obvious if a card reports its Bluetooth function
            // differently and therefore never gets hooked.
            DBGLOG(DRV_NAME, "Skipping Intel device %04x class %02x/%02x/%02x",
                   deviceDescriptor->idProduct, deviceDescriptor->bDeviceClass,
                   deviceDescriptor->bDeviceSubClass, deviceDescriptor->bDeviceProtocol);
        }
    }
    return ret;
}
