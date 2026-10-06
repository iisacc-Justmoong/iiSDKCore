#include "HardwareAppleIds.h"
#include "HardwareInternal.h"
#import <AudioToolbox/AudioToolbox.h>
#import <CoreBluetooth/CoreBluetooth.h>
#import <CoreMIDI/CoreMIDI.h>
#import <Foundation/Foundation.h>
#include <TargetConditionals.h>
#include <algorithm>
#include <charconv>
#include <future>
#include <map>
#include <set>
#if TARGET_OS_IPHONE
#import <AVFAudio/AVFAudio.h>
#endif
#if !TARGET_OS_IPHONE
#import <CoreAudio/CoreAudio.h>
#import <IOKit/IOKitLib.h>
#import <IOKit/hid/IOHIDManager.h>
#import <IOUSBHost/IOUSBHost.h>
#endif

namespace ii::io::detail {
struct BluetoothState;
}
@interface IIHardwareBluetoothDelegate
    : NSObject <CBCentralManagerDelegate, CBPeripheralDelegate> {
@public
  ii::io::detail::BluetoothState *context;
}
@end
namespace ii::io {
HardwareCapabilities hardware_capabilities() noexcept {
#if TARGET_OS_IPHONE
  return {false, false, true, true, true, true, true};
#else
  return {true, true, true, true, true, true, true};
#endif
}
namespace detail {
namespace {
auto failure(ErrorCode code) { return std::unexpected(Error{code, {}}); }
auto status_error(std::int64_t value) {
  auto code = ErrorCode::system_error;
#if !TARGET_OS_IPHONE
  if (value == kIOReturnNotPermitted || value == kIOReturnNotPrivileged)
    code = ErrorCode::permission_denied;
  else if (value == kIOReturnExclusiveAccess || value == kIOReturnBusy)
    code = ErrorCode::busy;
  else if (value == kIOReturnNoDevice || value == kIOReturnNotOpen)
    code = ErrorCode::closed;
  else if (value == kIOReturnTimeout)
    code = ErrorCode::timed_out;
#endif
  return std::unexpected(
      Error{code, {static_cast<int>(value), std::system_category()}});
}
std::string string(NSString *value) {
  return value.UTF8String ? value.UTF8String : "";
}
NSString *string(std::string_view value) {
  return [[NSString alloc] initWithBytes:value.data()
                                  length:value.size()
                                encoding:NSUTF8StringEncoding];
}
std::string cf_string(CFStringRef value) {
  if (!value)
    return {};
  std::vector<char> bytes(CFStringGetMaximumSizeForEncoding(
                              CFStringGetLength(value), kCFStringEncodingUTF8) +
                          1);
  return CFStringGetCString(value, bytes.data(), bytes.size(),
                            kCFStringEncodingUTF8)
             ? std::string(bytes.data())
             : std::string{};
}
std::optional<std::uint64_t> numeric(std::string_view text,
                                     std::string_view prefix) {
  if (!text.starts_with(prefix))
    return {};
  text.remove_prefix(prefix.size());
  std::uint64_t value = 0;
  const auto result =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (result.ec != std::errc{} || result.ptr != text.data() + text.size() ||
      value == 0)
    return {};
  return value;
}
bool usage_key(NSString *key) {
  return [NSBundle.mainBundle objectForInfoDictionaryKey:key] != nil;
}
#if !TARGET_OS_IPHONE
std::string property_string(io_service_t service, CFStringRef key) {
  auto value =
      IORegistryEntryCreateCFProperty(service, key, kCFAllocatorDefault, 0);
  if (!value)
    return {};
  auto result = CFGetTypeID(value) == CFStringGetTypeID()
                    ? cf_string(static_cast<CFStringRef>(value))
                    : std::string{};
  CFRelease(value);
  return result;
}
std::uint32_t property_number(io_service_t service, CFStringRef key) {
  auto value =
      IORegistryEntryCreateCFProperty(service, key, kCFAllocatorDefault, 0);
  std::int64_t result = 0;
  if (value) {
    if (CFGetTypeID(value) == CFNumberGetTypeID())
      CFNumberGetValue(static_cast<CFNumberRef>(value), kCFNumberSInt64Type,
                       &result);
    CFRelease(value);
  }
  return static_cast<std::uint32_t>(result);
}
DeviceInfo registry_info(io_service_t service, DeviceKind kind) {
  std::uint64_t id = 0;
  IORegistryEntryGetRegistryEntryID(service, &id);
  DeviceInfo info;
  info.kind = kind;
  info.id = (kind == DeviceKind::usb ? "usb:" : "hid:") + std::to_string(id);
  info.vendor = property_number(service, CFSTR("idVendor"));
  info.product = property_number(service, CFSTR("idProduct"));
  info.name = property_string(service, CFSTR("USB Product Name"));
  info.manufacturer = property_string(service, CFSTR("USB Vendor Name"));
  info.serial = property_string(service, CFSTR("USB Serial Number"));
  if (kind == DeviceKind::hid) {
    info.vendor = property_number(service, CFSTR(kIOHIDVendorIDKey));
    info.product = property_number(service, CFSTR(kIOHIDProductIDKey));
    info.name = property_string(service, CFSTR(kIOHIDProductKey));
    info.manufacturer = property_string(service, CFSTR(kIOHIDManufacturerKey));
    info.serial = property_string(service, CFSTR(kIOHIDSerialNumberKey));
    info.usage_page =
        property_number(service, CFSTR(kIOHIDPrimaryUsagePageKey));
    info.usage = property_number(service, CFSTR(kIOHIDPrimaryUsageKey));
  }
  return info;
}
IOHIDReportType report_type(protocol::HIDReportType type) {
  return type == protocol::HIDReportType::input    ? kIOHIDReportTypeInput
         : type == protocol::HIDReportType::output ? kIOHIDReportTypeOutput
                                                   : kIOHIDReportTypeFeature;
}
#endif
} // namespace
struct BluetoothState final {
  std::shared_ptr<HardwareState> state;
  dispatch_queue_t queue =
      dispatch_queue_create("iiSDKCore.bluetooth", DISPATCH_QUEUE_SERIAL);
  IIHardwareBluetoothDelegate *delegate;
  CBCentralManager *manager;
  NSMutableDictionary<NSString *, CBPeripheral *> *peripherals =
      [NSMutableDictionary dictionary];
  std::map<std::string, DeviceInfo> devices;
  std::map<std::string, Endpoint> endpoints;
  NSArray<CBUUID *> *services;
  bool scanning = false;
  explicit BluetoothState(std::shared_ptr<HardwareState> value)
      : state(std::move(value)) {
    delegate = [IIHardwareBluetoothDelegate new];
    delegate->context = this;
    manager = [[CBCentralManager alloc]
        initWithDelegate:delegate
                   queue:queue
                 options:@{
                   CBCentralManagerOptionShowPowerAlertKey : @NO
                 }];
  }
  ~BluetoothState() {
    dispatch_sync(queue, ^{
      [manager stopScan];
      for (CBPeripheral *p in peripherals.allValues) {
        p.delegate = nil;
        if (p.state != CBPeripheralStateDisconnected)
          [manager cancelPeripheralConnection:p];
      }
      manager.delegate = nil;
      delegate->context = nullptr;
      [peripherals removeAllObjects];
    });
  }
  void error(NSError *error, Endpoint endpoint = {}) {
    HardwareEvent event;
    event.kind = HardwareEventKind::failure;
    event.endpoint = endpoint;
    event.error = Error{ErrorCode::system_error,
                        {static_cast<int>(error.code), std::system_category()}};
    state->publish(std::move(event));
  }
  void updated() {
    if (manager.state == CBManagerStatePoweredOn) {
      if (scanning)
        [manager scanForPeripheralsWithServices:services options:nil];
    } else if (manager.state == CBManagerStateUnauthorized ||
               manager.state == CBManagerStateUnsupported) {
      HardwareEvent event;
      event.kind = HardwareEventKind::failure;
      event.error = Error{manager.state == CBManagerStateUnauthorized
                              ? ErrorCode::permission_denied
                              : ErrorCode::unsupported,
                          {}};
      state->publish(std::move(event));
    }
  }
  void discovered(CBPeripheral *p, NSDictionary *data) {
    const auto id = string(p.identifier.UUIDString);
    if (!devices.contains(id) &&
        devices.size() >= state->options.event_capacity)
      return;
    DeviceInfo info;
    info.kind = DeviceKind::bluetooth;
    info.id = id;
    info.name = string(p.name ? p.name : data[CBAdvertisementDataLocalNameKey]);
    const bool fresh = !devices.contains(id);
    devices[id] = info;
    peripherals[p.identifier.UUIDString] = p;
    if (fresh) {
      HardwareEvent event;
      event.kind = HardwareEventKind::added;
      event.device = info;
      state->publish(std::move(event));
    }
  }
  void connection(CBPeripheral *p, bool connected, NSError *error = nil) {
    const auto id = string(p.identifier.UUIDString);
    const auto found = endpoints.find(id);
    if (found == endpoints.end())
      return;
    if (error) {
      this->error(error, found->second);
      return;
    }
    HardwareEvent event;
    event.endpoint = found->second;
    event.device = devices[id];
    event.kind = connected ? HardwareEventKind::connected
                           : HardwareEventKind::disconnected;
    state->publish(std::move(event));
    if (connected) {
      p.delegate = delegate;
      [p discoverServices:nil];
    }
  }
  void characteristics(CBPeripheral *p, CBService *service, NSError *error) {
    if (error) {
      this->error(error);
      return;
    }
    for (CBCharacteristic *c in service.characteristics) {
      HardwareEvent event;
      event.kind = HardwareEventKind::characteristic;
      event.endpoint = endpoints[string(p.identifier.UUIDString)];
      event.service =
          protocol::BluetoothUUID::parse(string(service.UUID.UUIDString))
              .value()
              .string();
      event.characteristic =
          protocol::BluetoothUUID::parse(string(c.UUID.UUIDString))
              .value()
              .string();
      event.bytes.push_back(std::byte(c.properties & 255));
      state->publish(std::move(event));
    }
  }
  void value(CBPeripheral *p, CBCharacteristic *c, NSError *error) {
    const auto found = endpoints.find(string(p.identifier.UUIDString));
    if (found == endpoints.end())
      return;
    if (error) {
      this->error(error, found->second);
      return;
    }
    if (c.value.length > state->options.max_payload_bytes)
      return;
    HardwareEvent event;
    event.endpoint = found->second;
    event.kind = HardwareEventKind::input;
    event.device.kind = DeviceKind::bluetooth;
    event.service =
        protocol::BluetoothUUID::parse(string(c.service.UUID.UUIDString))
            .value()
            .string();
    event.characteristic =
        protocol::BluetoothUUID::parse(string(c.UUID.UUIDString))
            .value()
            .string();
    const auto *begin = static_cast<const std::byte *>(c.value.bytes);
    if (c.value.length)
      event.bytes.assign(begin, begin + c.value.length);
    state->publish(std::move(event));
  }
};
struct AudioSession final {
  Endpoint endpoint;
  AudioConfig config;
  DeviceKind kind;
  std::shared_ptr<HardwareState> state;
  AudioRing ring;
  AudioQueueRef queue = nullptr;
  std::atomic<std::uint64_t> overruns{0};
  AudioSession(Endpoint ep, AudioConfig c, DeviceKind k,
               std::shared_ptr<HardwareState> s)
      : endpoint(ep), config(c), kind(k), state(std::move(s)),
        ring(std::size_t(c.queued_frames) * c.channels) {}
  ~AudioSession() {
    if (queue) {
      AudioQueueStop(queue, true);
      AudioQueueDispose(queue, true);
    }
  }
  static void input(void *context, AudioQueueRef queue,
                    AudioQueueBufferRef buffer, const AudioTimeStamp *, UInt32,
                    const AudioStreamPacketDescription *) {
    auto &self = *static_cast<AudioSession *>(context);
    const auto count = buffer->mAudioDataByteSize / sizeof(float);
    if (self.ring.write(
            {static_cast<const float *>(buffer->mAudioData), count}) != count)
      self.overruns.fetch_add(1, std::memory_order_relaxed);
    AudioQueueEnqueueBuffer(queue, buffer, 0, nullptr);
  }
  static void output(void *context, AudioQueueRef queue,
                     AudioQueueBufferRef buffer) {
    auto &self = *static_cast<AudioSession *>(context);
    const auto count =
        std::size_t(self.config.frames_per_buffer) * self.config.channels;
    auto *data = static_cast<float *>(buffer->mAudioData);
    const auto read = self.ring.read({data, count});
    std::fill(data + read, data + count, 0.0f);
    buffer->mAudioDataByteSize = static_cast<UInt32>(count * sizeof(float));
    AudioQueueEnqueueBuffer(queue, buffer, 0, nullptr);
  }
  Result<void> start(std::string_view id) {
    AudioStreamBasicDescription format{};
    format.mSampleRate = config.sample_rate;
    format.mFormatID = kAudioFormatLinearPCM;
    format.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    format.mBitsPerChannel = 32;
    format.mChannelsPerFrame = config.channels;
    format.mFramesPerPacket = 1;
    format.mBytesPerFrame = 4 * config.channels;
    format.mBytesPerPacket = format.mBytesPerFrame;
    auto status = kind == DeviceKind::audio_input
                      ? AudioQueueNewInput(&format, input, this, nullptr,
                                           nullptr, 0, &queue)
                      : AudioQueueNewOutput(&format, output, this, nullptr,
                                            nullptr, 0, &queue);
    if (status)
      return status_error(status);
#if !TARGET_OS_IPHONE
    if (!id.empty()) {
      CFStringRef uid = (__bridge CFStringRef)string(id);
      status = AudioQueueSetProperty(queue, kAudioQueueProperty_CurrentDevice,
                                     &uid, sizeof(uid));
      if (status)
        return status_error(status);
    }
#else
    if (!id.empty())
      return failure(ErrorCode::unsupported);
#endif
    for (int i = 0; i < 3; ++i) {
      AudioQueueBufferRef buffer = nullptr;
      status = AudioQueueAllocateBuffer(
          queue, config.frames_per_buffer * format.mBytesPerFrame, &buffer);
      if (status)
        return status_error(status);
      if (kind == DeviceKind::audio_output)
        output(this, queue, buffer);
      else {
        status = AudioQueueEnqueueBuffer(queue, buffer, 0, nullptr);
        if (status)
          return status_error(status);
      }
    }
    status = AudioQueueStart(queue, nullptr);
    if (status)
      return status_error(status);
    return {};
  }
};
struct MidiSession final {
  Endpoint endpoint;
  MIDIClientRef client = 0;
  MIDIPortRef port = 0;
  MIDIEndpointRef native = 0;
  bool source = false, owned = false;
  ~MidiSession() {
    if (owned && native)
      MIDIEndpointDispose(native);
    if (port)
      MIDIPortDispose(port);
    if (client)
      MIDIClientDispose(client);
  }
};
// CoreMIDI copies blocks into an opaque native transport. Serialize publication
// and the initial capture read across that boundary; unlock before parsing.
std::mutex &midi_publication() {
  static std::mutex value;
  return value;
}
// CoreMIDI's process notification transport needs a live CFRunLoop even when
// notify blocks choose another delivery thread. Share one owned pump among
// active hubs, so closing the first hub cannot strand another hub's monitor.
struct MidiPump;
struct MidiRegistry final {
  std::mutex mutex;
  std::condition_variable changed;
  std::weak_ptr<MidiPump> shared;
  MidiPump *current = nullptr;
};
MidiRegistry &midi_registry() {
  static MidiRegistry value;
  return value;
}
struct MidiPump final {
  runtime::Thread thread;
  std::shared_ptr<std::atomic<std::uint64_t>> epoch =
      std::make_shared<std::atomic<std::uint64_t>>(0);
  std::atomic<CFRunLoopRef> loop{nullptr};
  bool registered = false;
  Result<void> start() {
    auto ready = std::make_shared<std::promise<OSStatus>>();
    auto future = ready->get_future();
    const auto changed = epoch;
    const auto started =
        thread.start([this, ready, changed](runtime::ThreadContext &context) {
          @autoreleasepool {
            auto current = CFRunLoopGetCurrent();
            CFRetain(current);
            loop = current;
            CFRunLoopSourceContext source_context{};
            auto source =
                CFRunLoopSourceCreate(kCFAllocatorDefault, 0, &source_context);
            if (!source) {
              ready->set_value(-1);
              return;
            }
            CFRunLoopAddSource(current, source, kCFRunLoopDefaultMode);
            MIDIClientRef client = 0;
            OSStatus status = 0;
            {
              std::lock_guard publication(midi_publication());
              status = MIDIClientCreateWithBlock(
                  CFSTR("iiSDKCore MIDI notification pump"), &client,
                  ^(const MIDINotification *) {
                    std::shared_ptr<std::atomic<std::uint64_t>> target;
                    {
                      std::lock_guard publication(midi_publication());
                      target = changed;
                    }
                    target->fetch_add(1);
                  });
            }
            ready->set_value(status);
            if (!status)
              while (!context.stop_requested())
                @autoreleasepool {
                  CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.025, false);
                }
            if (client)
              MIDIClientDispose(client);
            CFRunLoopRemoveSource(current, source, kCFRunLoopDefaultMode);
            CFRelease(source);
          }
        });
    if (!started)
      return failure(ErrorCode::system_error);
    const auto status = future.get();
    if (status)
      return status_error(status);
    return {};
  }
  ~MidiPump() {
    auto &registry = midi_registry();
    std::unique_lock<std::mutex> lock(registry.mutex, std::defer_lock);
    if (registered)
      lock.lock();
    (void)thread.request_stop();
    auto current = loop.load();
    if (current)
      CFRunLoopWakeUp(current);
    (void)thread.join();
    if (current)
      CFRelease(current);
    if (registered) {
      registry.shared.reset();
      registry.current = nullptr;
      registry.changed.notify_all();
    }
  }
};
Result<std::shared_ptr<MidiPump>> midi_pump() {
  auto &registry = midi_registry();
  std::unique_lock lock(registry.mutex);
  for (;;) {
    if (auto existing = registry.shared.lock())
      return existing;
    if (!registry.current)
      break;
    registry.changed.wait(lock);
  }
  auto created = std::make_shared<MidiPump>();
  auto started = created->start();
  if (!started)
    return std::unexpected(started.error());
  registry.shared = created;
  registry.current = created.get();
  created->registered = true;
  return created;
}
#if !TARGET_OS_IPHONE
struct HidSession final {
  IOHIDDeviceRef device = nullptr;
  dispatch_queue_t queue =
      dispatch_queue_create("iiSDKCore.hid", DISPATCH_QUEUE_SERIAL);
  dispatch_semaphore_t cancelled = dispatch_semaphore_create(0);
  std::weak_ptr<HardwareState> state;
  Endpoint endpoint;
  std::vector<std::uint8_t> report;
  bool active = false;
  ~HidSession() {
    if (device) {
      if (active) {
        IOHIDDeviceCancel(device);
        dispatch_semaphore_wait(cancelled, DISPATCH_TIME_FOREVER);
      }
      IOHIDDeviceClose(device, kIOHIDOptionsTypeNone);
      CFRelease(device);
    }
  }
  static void input(void *context, IOReturn result, void *, IOHIDReportType,
                    std::uint32_t id, std::uint8_t *bytes, CFIndex length) {
    auto &self = *static_cast<HidSession *>(context);
    auto state = self.state.lock();
    if (!state)
      return;
    HardwareEvent event;
    event.endpoint = self.endpoint;
    event.device.kind = DeviceKind::hid;
    if (result) {
      event.kind = HardwareEventKind::failure;
      event.error = status_error(result).error();
    } else {
      event.kind = HardwareEventKind::input;
      if (length < 0 || std::size_t(length) > state->options.max_payload_bytes)
        return;
      // IOHID input bytes already contain the report ID when the descriptor
      // uses one.
      event.bytes.assign(reinterpret_cast<std::byte *>(bytes),
                         reinterpret_cast<std::byte *>(bytes) + length);
      event.characteristic = std::to_string(id);
    }
    state->publish(std::move(event));
  }
};
struct UsbSession final {
  IOUSBHostDevice *device;
  ~UsbSession() { [device destroy]; }
};
#endif
struct NativeHardware::Impl {
  std::shared_ptr<MidiPump> midi_notifications;
  std::uint64_t midi_epoch = 0;
  runtime::Clock::time_point midi_reconcile{};
  std::shared_ptr<HardwareState> state;
  std::uint64_t next = 0;
  std::map<std::uint64_t, DeviceKind> kinds;
  std::map<std::uint64_t, std::unique_ptr<AudioSession>> audio;
  std::map<std::uint64_t, std::unique_ptr<MidiSession>> midi;
  std::unique_ptr<BluetoothState> bluetooth;
  std::set<DeviceKind> watched;
  std::map<DeviceKind, std::vector<DeviceInfo>> snapshots;
  std::shared_ptr<std::atomic<bool>> changed =
      std::make_shared<std::atomic<bool>>(false);
#if TARGET_OS_IPHONE
  id route_observer = nil;
#endif
#if !TARGET_OS_IPHONE
  std::map<std::uint64_t, std::unique_ptr<HidSession>> hid;
  std::map<std::uint64_t, std::unique_ptr<UsbSession>> usb;
  IONotificationPortRef notifications = nullptr;
  dispatch_queue_t notify_queue =
      dispatch_queue_create("iiSDKCore.devices", DISPATCH_QUEUE_SERIAL);
  std::vector<io_iterator_t> iterators;
  AudioObjectPropertyListenerBlock audio_listener = nil;
  static void notification(void *context, io_iterator_t iterator) {
    while (auto service = IOIteratorNext(iterator))
      IOObjectRelease(service);
    *static_cast<Impl *>(context)->changed = true;
  }
#endif
  explicit Impl(std::shared_ptr<HardwareState> value)
      : state(std::move(value)) {}
  ~Impl() {
#if TARGET_OS_IPHONE
    if (route_observer)
      [NSNotificationCenter.defaultCenter removeObserver:route_observer];
#endif
#if !TARGET_OS_IPHONE
    if (audio_listener) {
      AudioObjectPropertyAddress address{kAudioHardwarePropertyDevices,
                                         kAudioObjectPropertyScopeGlobal,
                                         kAudioObjectPropertyElementMain};
      AudioObjectRemovePropertyListenerBlock(kAudioObjectSystemObject, &address,
                                             notify_queue, audio_listener);
    }
    if (notifications) {
      IONotificationPortSetDispatchQueue(notifications, nullptr);
      IONotificationPortDestroy(notifications);
    }
    dispatch_sync(notify_queue, ^{
                  });
    for (auto iterator : iterators)
      IOObjectRelease(iterator);
#endif
  }
  Result<void> ensure_midi() {
    if (midi_notifications)
      return {};
    auto result = midi_pump();
    if (!result)
      return std::unexpected(result.error());
    midi_notifications = std::move(*result);
    return {};
  }
  Endpoint endpoint(DeviceKind kind) {
    Endpoint ep{state->owner, ++next};
    kinds[ep.value] = kind;
    state->activate(ep);
    return ep;
  }
  bool valid(Endpoint ep) const {
    return ep.owner == state->owner && kinds.contains(ep.value);
  }
  MIDIReceiveBlock receive(Endpoint endpoint) {
    std::weak_ptr<HardwareState> weak = state;
    return ^(const MIDIEventList *list, void *) {
      std::shared_ptr<HardwareState> state;
      Endpoint destination;
      {
        std::lock_guard publication(midi_publication());
        state = weak.lock();
        destination = endpoint;
      }
      if (!state)
        return;
      HardwareEvent event;
      event.endpoint = destination;
      event.device.kind = DeviceKind::midi_input;
      auto *packet = &list->packet[0];
      for (UInt32 i = 0; i < list->numPackets; ++i) {
        if (packet->wordCount >
            state->options.max_payload_bytes / sizeof(std::uint32_t))
          return;
        auto parsed = protocol::ump_packets({packet->words, packet->wordCount});
        if (!parsed)
          return;
        if (event.midi.size() + parsed->size() >
            state->options.max_payload_bytes / sizeof(protocol::UMP))
          return;
        event.midi.insert(event.midi.end(), parsed->begin(), parsed->end());
        packet = MIDIEventPacketNext(packet);
      }
      state->publish(std::move(event));
    };
  }
};
NativeHardware::NativeHardware(std::shared_ptr<HardwareState> state)
    : impl_(std::make_unique<Impl>(std::move(state))) {}
NativeHardware::~NativeHardware() {
  @autoreleasepool {
    impl_.reset();
  }
}
Result<std::vector<DeviceInfo>> NativeHardware::enumerate(DeviceKind kind) {
  @autoreleasepool {
    std::vector<DeviceInfo> result;
#if !TARGET_OS_IPHONE
    if (kind == DeviceKind::usb || kind == DeviceKind::hid) {
      io_iterator_t iterator = 0;
      const auto status = IOServiceGetMatchingServices(
          kIOMainPortDefault,
          IOServiceMatching(kind == DeviceKind::usb ? "IOUSBHostDevice"
                                                    : "IOHIDDevice"),
          &iterator);
      if (status)
        return status_error(status);
      while (auto service = IOIteratorNext(iterator)) {
        result.push_back(registry_info(service, kind));
        IOObjectRelease(service);
      }
      IOObjectRelease(iterator);
      return result;
    }
    if (kind == DeviceKind::audio_input || kind == DeviceKind::audio_output) {
      AudioObjectPropertyAddress address{kAudioHardwarePropertyDevices,
                                         kAudioObjectPropertyScopeGlobal,
                                         kAudioObjectPropertyElementMain};
      UInt32 size = 0;
      auto status = AudioObjectGetPropertyDataSize(kAudioObjectSystemObject,
                                                   &address, 0, nullptr, &size);
      if (status)
        return status_error(status);
      std::vector<AudioDeviceID> devices(size / sizeof(AudioDeviceID));
      status = AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0,
                                          nullptr, &size, devices.data());
      if (status)
        return status_error(status);
      for (auto device : devices) {
        address = {kAudioDevicePropertyStreams,
                   kind == DeviceKind::audio_input
                       ? kAudioDevicePropertyScopeInput
                       : kAudioDevicePropertyScopeOutput,
                   kAudioObjectPropertyElementMain};
        UInt32 streams = 0;
        if (AudioObjectGetPropertyDataSize(device, &address, 0, nullptr,
                                           &streams) != noErr ||
            !streams)
          continue;
        DeviceInfo info;
        info.kind = kind;
        CFStringRef value = nullptr;
        size = sizeof(value);
        address = {kAudioDevicePropertyDeviceUID,
                   kAudioObjectPropertyScopeGlobal,
                   kAudioObjectPropertyElementMain};
        if (AudioObjectGetPropertyData(device, &address, 0, nullptr, &size,
                                       &value) == noErr) {
          info.id = cf_string(value);
          if (value)
            CFRelease(value);
        }
        value = nullptr;
        size = sizeof(value);
        address.mSelector = kAudioObjectPropertyName;
        if (AudioObjectGetPropertyData(device, &address, 0, nullptr, &size,
                                       &value) == noErr) {
          info.name = cf_string(value);
          if (value)
            CFRelease(value);
        }
        result.push_back(std::move(info));
      }
      return result;
    }
#else
    if (kind == DeviceKind::usb || kind == DeviceKind::hid)
      return failure(ErrorCode::unsupported);
    if (kind == DeviceKind::audio_input || kind == DeviceKind::audio_output) {
      DeviceInfo info;
      info.kind = kind;
      info.name = "System audio route";
      result.push_back(std::move(info));
      return result;
    }
#endif
    if (kind == DeviceKind::midi_input || kind == DeviceKind::midi_output) {
      auto ready = impl_->ensure_midi();
      if (!ready)
        return std::unexpected(ready.error());
      const auto count = kind == DeviceKind::midi_input
                             ? MIDIGetNumberOfSources()
                             : MIDIGetNumberOfDestinations();
      for (ItemCount i = 0; i < count; ++i) {
        const auto endpoint = kind == DeviceKind::midi_input
                                  ? MIDIGetSource(i)
                                  : MIDIGetDestination(i);
        SInt32 unique = 0;
        CFStringRef name = nullptr;
        if (MIDIObjectGetIntegerProperty(endpoint, kMIDIPropertyUniqueID,
                                         &unique) != noErr)
          continue;
        DeviceInfo info;
        info.kind = kind;
        info.id = "midi:" + std::to_string(unique);
        if (MIDIObjectGetStringProperty(endpoint, kMIDIPropertyDisplayName,
                                        &name) == noErr) {
          info.name = cf_string(name);
          if (name)
            CFRelease(name);
        }
        result.push_back(std::move(info));
      }
      return result;
    }
    if (kind == DeviceKind::bluetooth) {
      auto *output = &result;
      if (impl_->bluetooth)
        dispatch_sync(impl_->bluetooth->queue, ^{
          for (const auto &[_, info] : impl_->bluetooth->devices)
            output->push_back(info);
        });
      return result;
    }
    return failure(ErrorCode::unsupported);
  }
}
Result<void> NativeHardware::watch(DeviceKind kind) {
  @autoreleasepool {
    if (impl_->watched.contains(kind))
      return {};
#if !TARGET_OS_IPHONE
    if (kind == DeviceKind::usb || kind == DeviceKind::hid) {
      if (!impl_->notifications) {
        impl_->notifications = IONotificationPortCreate(kIOMainPortDefault);
        if (!impl_->notifications)
          return failure(ErrorCode::system_error);
        IONotificationPortSetDispatchQueue(impl_->notifications,
                                           impl_->notify_queue);
      }
      for (const auto *type :
           {kIOFirstMatchNotification, kIOTerminatedNotification}) {
        io_iterator_t iterator = 0;
        auto status = IOServiceAddMatchingNotification(
            impl_->notifications, type,
            IOServiceMatching(kind == DeviceKind::usb ? "IOUSBHostDevice"
                                                      : "IOHIDDevice"),
            Impl::notification, impl_.get(), &iterator);
        if (status)
          return status_error(status);
        impl_->iterators.push_back(iterator);
        Impl::notification(impl_.get(), iterator);
      }
    }
    if ((kind == DeviceKind::audio_input || kind == DeviceKind::audio_output) &&
        !impl_->audio_listener) {
      AudioObjectPropertyAddress address{kAudioHardwarePropertyDevices,
                                         kAudioObjectPropertyScopeGlobal,
                                         kAudioObjectPropertyElementMain};
      const std::weak_ptr<std::atomic<bool>> weak = impl_->changed;
      impl_->audio_listener = ^(UInt32, const AudioObjectPropertyAddress *) {
        if (auto flag = weak.lock())
          *flag = true;
      };
      const auto status = AudioObjectAddPropertyListenerBlock(
          kAudioObjectSystemObject, &address, impl_->notify_queue,
          impl_->audio_listener);
      if (status) {
        impl_->audio_listener = nil;
        return status_error(status);
      }
    }
#else
    if (kind == DeviceKind::usb || kind == DeviceKind::hid)
      return failure(ErrorCode::unsupported);
    if ((kind == DeviceKind::audio_input || kind == DeviceKind::audio_output) &&
        !impl_->route_observer) {
      const std::weak_ptr<std::atomic<bool>> weak = impl_->changed;
      impl_->route_observer = [NSNotificationCenter.defaultCenter
          addObserverForName:AVAudioSessionRouteChangeNotification
                      object:nil
                       queue:nil
                  usingBlock:^(NSNotification *) {
                    if (auto flag = weak.lock())
                      *flag = true;
                  }];
    }
#endif
    if (kind == DeviceKind::midi_input || kind == DeviceKind::midi_output) {
      auto ready = impl_->ensure_midi();
      if (!ready)
        return ready;
    }
    if (kind == DeviceKind::bluetooth)
      return failure(
          ErrorCode::invalid_argument); // scan requires service filters
    impl_->watched.insert(kind);
    *impl_->changed = true;
    return {};
  }
}
Result<Endpoint> NativeHardware::open(DeviceKind kind, std::string_view id,
                                      AudioConfig config) {
  @autoreleasepool {
#if !TARGET_OS_IPHONE
    if (kind == DeviceKind::usb || kind == DeviceKind::hid) {
      const auto number =
          numeric(id, kind == DeviceKind::usb ? "usb:" : "hid:");
      if (!number)
        return failure(ErrorCode::invalid_argument);
      const auto service = IOServiceGetMatchingService(
          kIOMainPortDefault, IORegistryEntryIDMatching(*number));
      if (!service)
        return failure(ErrorCode::not_found);
      if (!IOObjectConformsTo(service, kind == DeviceKind::usb
                                           ? "IOUSBHostDevice"
                                           : "IOHIDDevice")) {
        IOObjectRelease(service);
        return failure(ErrorCode::invalid_argument);
      }
      if (kind == DeviceKind::usb) {
        NSError *error = nil;
        auto session = std::make_unique<UsbSession>();
        session->device = [[IOUSBHostDevice alloc] initWithIOService:service
                                                             options:0
                                                               queue:nil
                                                               error:&error
                                                     interestHandler:nil];
        IOObjectRelease(service);
        if (!session->device)
          return status_error(error.code);
        const auto ep = impl_->endpoint(kind);
        impl_->usb[ep.value] = std::move(session);
        return ep;
      }
      auto session = std::make_unique<HidSession>();
      session->device = IOHIDDeviceCreate(kCFAllocatorDefault, service);
      IOObjectRelease(service);
      if (!session->device)
        return failure(ErrorCode::not_found);
      const auto status =
          IOHIDDeviceOpen(session->device, kIOHIDOptionsTypeNone);
      if (status)
        return status_error(status);
      auto size = IOHIDDeviceGetProperty(session->device,
                                         CFSTR(kIOHIDMaxInputReportSizeKey));
      CFIndex length = 0;
      if (size && CFGetTypeID(size) == CFNumberGetTypeID())
        CFNumberGetValue(static_cast<CFNumberRef>(size), kCFNumberCFIndexType,
                         &length);
      if (length <= 0 ||
          std::size_t(length) > impl_->state->options.max_payload_bytes)
        return failure(ErrorCode::unsupported);
      session->report.resize(length);
      session->state = impl_->state;
      session->endpoint = impl_->endpoint(kind);
      IOHIDDeviceRegisterInputReportCallback(session->device,
                                             session->report.data(), length,
                                             HidSession::input, session.get());
      IOHIDDeviceSetDispatchQueue(session->device, session->queue);
      const auto cancelled = session->cancelled;
      IOHIDDeviceSetCancelHandler(session->device, ^{
        dispatch_semaphore_signal(cancelled);
      });
      IOHIDDeviceActivate(session->device);
      session->active = true;
      const auto ep = session->endpoint;
      impl_->hid[ep.value] = std::move(session);
      return ep;
    }
#else
    if (kind == DeviceKind::usb || kind == DeviceKind::hid)
      return failure(ErrorCode::unsupported);
#endif
    if (kind == DeviceKind::audio_input || kind == DeviceKind::audio_output) {
      if (kind == DeviceKind::audio_input &&
          !usage_key(@"NSMicrophoneUsageDescription"))
        return failure(ErrorCode::permission_denied);
      const auto ep = impl_->endpoint(kind);
      auto session =
          std::make_unique<AudioSession>(ep, config, kind, impl_->state);
      auto started = session->start(id);
      if (!started) {
        impl_->kinds.erase(ep.value);
        impl_->state->deactivate(ep);
        return std::unexpected(started.error());
      }
      impl_->audio[ep.value] = std::move(session);
      return ep;
    }
    if (kind == DeviceKind::midi_input) {
      auto ready = impl_->ensure_midi();
      if (!ready)
        return std::unexpected(ready.error());
      const auto number = core_midi_identity(id);
      if (!number)
        return failure(ErrorCode::invalid_argument);
      MIDIObjectRef object = 0;
      MIDIObjectType type;
      auto status = MIDIObjectFindByUniqueID(static_cast<SInt32>(*number),
                                             &object, &type);
      if (status)
        return status_error(status);
      if (type != kMIDIObjectType_Source && type != kMIDIObjectType_Destination)
        return failure(ErrorCode::invalid_argument);
      auto session = std::make_unique<MidiSession>();
      session->native = object;
      session->source = type == kMIDIObjectType_Source;
      session->endpoint = impl_->endpoint(
          session->source ? DeviceKind::midi_input : DeviceKind::midi_output);
      status = MIDIClientCreateWithBlock(
          (__bridge CFStringRef)string(
              "iiSDKCore MIDI " + std::to_string(session->endpoint.owner) +
              ":" + std::to_string(session->endpoint.value)),
          &session->client, nil);
      if (!status) {
        std::lock_guard publication(midi_publication());
        status = session->source
                     ? MIDIInputPortCreateWithProtocol(
                           session->client, CFSTR("Input"), kMIDIProtocol_2_0,
                           &session->port, impl_->receive(session->endpoint))
                     : MIDIOutputPortCreate(session->client, CFSTR("Output"),
                                            &session->port);
      }
      if (!status && session->source)
        status = MIDIPortConnectSource(session->port, session->native, nullptr);
      if (status) {
        impl_->kinds.erase(session->endpoint.value);
        impl_->state->deactivate(session->endpoint);
        return status_error(status);
      }
      const auto ep = session->endpoint;
      impl_->midi[ep.value] = std::move(session);
      return ep;
    }
    if (kind == DeviceKind::bluetooth) {
      if (!impl_->bluetooth)
        return failure(ErrorCode::not_found);
      __block CBPeripheral *peripheral = nil;
      __block bool duplicate = false;
      dispatch_sync(impl_->bluetooth->queue, ^{
        peripheral = impl_->bluetooth->peripherals[string(id)];
        duplicate = impl_->bluetooth->endpoints.contains(std::string(id));
      });
      if (duplicate)
        return failure(ErrorCode::busy);
      if (!peripheral)
        return failure(ErrorCode::not_found);
      const auto ep = impl_->endpoint(kind);
      dispatch_sync(impl_->bluetooth->queue, ^{
        impl_->bluetooth->endpoints[std::string(id)] = ep;
        [impl_->bluetooth->manager connectPeripheral:peripheral options:nil];
      });
      return ep;
    }
    return failure(ErrorCode::unsupported);
  }
}
Result<std::vector<std::byte>>
NativeHardware::usb_control(Endpoint ep, protocol::USBSetup setup,
                            std::span<const std::byte> output,
                            std::chrono::milliseconds timeout) {
  @autoreleasepool {
    if (!impl_->valid(ep))
      return failure(ErrorCode::closed);
#if !TARGET_OS_IPHONE
    auto found = impl_->usb.find(ep.value);
    if (found == impl_->usb.end())
      return failure(ErrorCode::invalid_argument);
    NSMutableData *data = [NSMutableData dataWithLength:setup.length];
    if (!output.empty())
      memcpy(data.mutableBytes, output.data(), output.size());
    IOUSBDeviceRequest request{setup.request_type, setup.request, setup.value,
                               setup.index, setup.length};
    NSUInteger count = 0;
    NSError *error = nil;
    if (![found->second->device sendDeviceRequest:request
                                             data:data
                                 bytesTransferred:&count
                                completionTimeout:timeout.count() / 1000.0
                                            error:&error])
      return status_error(error.code);
    if (count > data.length)
      return failure(ErrorCode::system_error);
    if (!(setup.request_type & 0x80))
      return std::vector<std::byte>{};
    const auto *begin = static_cast<const std::byte *>(data.bytes);
    return count ? std::vector<std::byte>(begin, begin + count)
                 : std::vector<std::byte>{};
#else
    (void)setup;
    (void)output;
    (void)timeout;
    return failure(ErrorCode::unsupported);
#endif
  }
}
Result<std::vector<std::byte>> NativeHardware::hid_descriptor(Endpoint ep) {
  @autoreleasepool {
    if (!impl_->valid(ep))
      return failure(ErrorCode::closed);
#if !TARGET_OS_IPHONE
    auto found = impl_->hid.find(ep.value);
    if (found == impl_->hid.end())
      return failure(ErrorCode::invalid_argument);
    auto data = IOHIDDeviceGetProperty(found->second->device,
                                       CFSTR(kIOHIDReportDescriptorKey));
    if (!data || CFGetTypeID(data) != CFDataGetTypeID())
      return failure(ErrorCode::unsupported);
    auto bytes = static_cast<CFDataRef>(data);
    const auto *begin =
        reinterpret_cast<const std::byte *>(CFDataGetBytePtr(bytes));
    return std::vector<std::byte>(begin, begin + CFDataGetLength(bytes));
#else
    return failure(ErrorCode::unsupported);
#endif
  }
}
Result<std::vector<std::byte>>
NativeHardware::hid_get(Endpoint ep, protocol::HIDReportType type,
                        std::uint8_t id, std::size_t length) {
  @autoreleasepool {
    if (!impl_->valid(ep))
      return failure(ErrorCode::closed);
#if !TARGET_OS_IPHONE
    auto found = impl_->hid.find(ep.value);
    if (found == impl_->hid.end())
      return failure(ErrorCode::invalid_argument);
    std::vector<std::byte> result(length);
    CFIndex count = length;
    if (id)
      result[0] = std::byte(id);
    const auto status = IOHIDDeviceGetReport(
        found->second->device, report_type(type), id,
        reinterpret_cast<std::uint8_t *>(result.data()), &count);
    if (status)
      return status_error(status);
    if (count < 0 || std::size_t(count) > length)
      return failure(ErrorCode::system_error);
    result.resize(count);
    return result;
#else
    (void)type;
    (void)id;
    (void)length;
    return failure(ErrorCode::unsupported);
#endif
  }
}
Result<void> NativeHardware::hid_set(Endpoint ep, protocol::HIDReportType type,
                                     std::uint8_t id,
                                     std::span<const std::byte> bytes) {
  @autoreleasepool {
    if (!impl_->valid(ep))
      return failure(ErrorCode::closed);
#if !TARGET_OS_IPHONE
    auto found = impl_->hid.find(ep.value);
    if (found == impl_->hid.end())
      return failure(ErrorCode::invalid_argument);
    const auto status = IOHIDDeviceSetReport(
        found->second->device, report_type(type), id,
        reinterpret_cast<const std::uint8_t *>(bytes.data()), bytes.size());
    if (status)
      return status_error(status);
    return {};
#else
    (void)type;
    (void)id;
    (void)bytes;
    return failure(ErrorCode::unsupported);
#endif
  }
}
Result<void>
NativeHardware::scan(std::span<const protocol::BluetoothUUID> services) {
  @autoreleasepool {
    if (!usage_key(@"NSBluetoothAlwaysUsageDescription"))
      return failure(ErrorCode::permission_denied);
    if (!impl_->bluetooth)
      impl_->bluetooth = std::make_unique<BluetoothState>(impl_->state);
    NSMutableArray<CBUUID *> *uuids = [NSMutableArray array];
    for (const auto &uuid : services)
      [uuids addObject:[CBUUID UUIDWithString:string(uuid.string())]];
    dispatch_sync(impl_->bluetooth->queue, ^{
      impl_->bluetooth->services = uuids;
      impl_->bluetooth->scanning = true;
      impl_->bluetooth->updated();
    });
    return {};
  }
}
Result<void> NativeHardware::stop_scan() {
  @autoreleasepool {
    if (impl_->bluetooth)
      dispatch_sync(impl_->bluetooth->queue, ^{
        impl_->bluetooth->scanning = false;
        [impl_->bluetooth->manager stopScan];
      });
    return {};
  }
}
Result<void> NativeHardware::gatt(Endpoint ep, protocol::BluetoothUUID service,
                                  protocol::BluetoothUUID characteristic,
                                  int operation,
                                  std::span<const std::byte> data, bool flag) {
  @autoreleasepool {
    if (!impl_->valid(ep))
      return failure(ErrorCode::closed);
    if (!impl_->bluetooth || impl_->kinds[ep.value] != DeviceKind::bluetooth)
      return failure(ErrorCode::invalid_argument);
    __block std::optional<ErrorCode> error;
    dispatch_sync(impl_->bluetooth->queue, ^{
      CBPeripheral *peripheral = nil;
      for (const auto &[id, endpoint] : impl_->bluetooth->endpoints)
        if (endpoint == ep) {
          peripheral = impl_->bluetooth->peripherals[string(id)];
          break;
        }
      if (!peripheral || peripheral.state != CBPeripheralStateConnected) {
        error = ErrorCode::would_block;
        return;
      }
      CBCharacteristic *target = nil;
      for (CBService *s in peripheral.services)
        if ([s.UUID isEqual:[CBUUID UUIDWithString:string(service.string())]])
          for (CBCharacteristic *c in s.characteristics)
            if ([c.UUID
                    isEqual:[CBUUID
                                UUIDWithString:string(
                                                   characteristic.string())]]) {
              target = c;
              break;
            }
      if (!target) {
        error = ErrorCode::not_found;
        return;
      }
      if (operation == 0) {
        if (!(target.properties & CBCharacteristicPropertyRead)) {
          error = ErrorCode::permission_denied;
          return;
        }
        [peripheral readValueForCharacteristic:target];
      } else if (operation == 2) {
        if (!(target.properties & (CBCharacteristicPropertyNotify |
                                   CBCharacteristicPropertyIndicate))) {
          error = ErrorCode::unsupported;
          return;
        }
        [peripheral setNotifyValue:flag forCharacteristic:target];
      } else {
        const auto mode = flag ? CBCharacteristicWriteWithResponse
                               : CBCharacteristicWriteWithoutResponse;
        if (!(target.properties &
              (flag ? CBCharacteristicPropertyWrite
                    : CBCharacteristicPropertyWriteWithoutResponse))) {
          error = ErrorCode::permission_denied;
          return;
        }
        if (data.size() > [peripheral maximumWriteValueLengthForType:mode]) {
          error = ErrorCode::invalid_argument;
          return;
        }
        if (!flag && !peripheral.canSendWriteWithoutResponse) {
          error = ErrorCode::would_block;
          return;
        }
        [peripheral writeValue:[NSData dataWithBytes:data.data()
                                              length:data.size()]
             forCharacteristic:target
                          type:mode];
      }
    });
    if (error)
      return failure(*error);
    return {};
  }
}
Result<std::size_t> NativeHardware::audio_write(Endpoint ep,
                                                std::span<const float> data) {
  @autoreleasepool {
    if (!impl_->valid(ep))
      return failure(ErrorCode::closed);
    auto found = impl_->audio.find(ep.value);
    if (found == impl_->audio.end() ||
        found->second->kind != DeviceKind::audio_output ||
        data.size() % found->second->config.channels)
      return failure(ErrorCode::invalid_argument);
    return found->second->ring.write(data);
  }
}
Result<Endpoint> NativeHardware::virtual_midi(std::string_view name,
                                              bool source) {
  @autoreleasepool {
    auto ready = impl_->ensure_midi();
    if (!ready)
      return std::unexpected(ready.error());
    auto session = std::make_unique<MidiSession>();
    session->endpoint = impl_->endpoint(source ? DeviceKind::midi_output
                                               : DeviceKind::midi_input);
    session->owned = true;
    session->source = source;
    auto status = MIDIClientCreateWithBlock(
        (__bridge CFStringRef)string("iiSDKCore virtual MIDI " +
                                     std::to_string(session->endpoint.owner) +
                                     ":" +
                                     std::to_string(session->endpoint.value)),
        &session->client, nil);
    if (!status) {
      std::lock_guard publication(midi_publication());
      status = source ? MIDISourceCreateWithProtocol(
                            session->client, (__bridge CFStringRef)string(name),
                            kMIDIProtocol_2_0, &session->native)
                      : MIDIDestinationCreateWithProtocol(
                            session->client, (__bridge CFStringRef)string(name),
                            kMIDIProtocol_2_0, &session->native,
                            impl_->receive(session->endpoint));
    }
    if (status) {
      impl_->kinds.erase(session->endpoint.value);
      impl_->state->deactivate(session->endpoint);
      return status_error(status);
    }
    const auto ep = session->endpoint;
    impl_->midi[ep.value] = std::move(session);
    return ep;
  }
}
Result<void> NativeHardware::midi_send(Endpoint ep,
                                       std::span<const protocol::UMP> packets) {
  @autoreleasepool {
    if (!impl_->valid(ep))
      return failure(ErrorCode::closed);
    auto found = impl_->midi.find(ep.value);
    if (found == impl_->midi.end())
      return failure(ErrorCode::invalid_argument);
    auto &session = *found->second;
    if (session.source && !session.owned)
      return failure(ErrorCode::permission_denied);
    if (!session.source && session.owned)
      return failure(ErrorCode::permission_denied);
    // Each UMP keeps its protocol. CoreMIDI converts MIDI 1.0 UMP as needed.
    for (const auto &packet : packets) {
      const auto protocol =
          packet.type() == 2 || packet.type() == 1 || packet.type() == 3
              ? kMIDIProtocol_1_0
              : kMIDIProtocol_2_0;
      MIDIEventList list{};
      auto *current = MIDIEventListInit(&list, protocol);
      current = MIDIEventListAdd(&list, sizeof(list), current, 0, packet.size,
                                 packet.words.data());
      if (!current)
        return failure(ErrorCode::invalid_argument);
      const auto status =
          session.owned
              ? MIDIReceivedEventList(session.native, &list)
              : MIDISendEventList(session.port, session.native, &list);
      if (status)
        return status_error(status);
    }
    return {};
  }
}
Result<void> NativeHardware::close(Endpoint ep) {
  @autoreleasepool {
    if (!impl_->valid(ep))
      return failure(ErrorCode::closed);
    if (impl_->kinds[ep.value] == DeviceKind::bluetooth && impl_->bluetooth)
      dispatch_sync(impl_->bluetooth->queue, ^{
        for (auto it = impl_->bluetooth->endpoints.begin();
             it != impl_->bluetooth->endpoints.end(); ++it)
          if (it->second == ep) {
            CBPeripheral *p = impl_->bluetooth->peripherals[string(it->first)];
            p.delegate = nil;
            [impl_->bluetooth->manager cancelPeripheralConnection:p];
            impl_->bluetooth->endpoints.erase(it);
            break;
          }
      });
    impl_->audio.erase(ep.value);
    impl_->midi.erase(ep.value);
#if !TARGET_OS_IPHONE
    impl_->hid.erase(ep.value);
    impl_->usb.erase(ep.value);
#endif
    impl_->kinds.erase(ep.value);
    return {};
  }
}
void NativeHardware::collect() {
  @autoreleasepool {
    if (impl_->midi_notifications) {
      const auto epoch = impl_->midi_notifications->epoch->load();
      if (epoch != impl_->midi_epoch) {
        impl_->midi_epoch = epoch;
        *impl_->changed = true;
      }
    }
    const bool changed = impl_->changed->exchange(false);
    const auto now = runtime::Clock::now();
    const bool reconcile = now >= impl_->midi_reconcile;
    if (reconcile)
      impl_->midi_reconcile = now + std::chrono::milliseconds(100);
    if (changed || reconcile)
      for (auto kind : impl_->watched) {
        // CoreMIDI's original process notification run loop may stay fixed
        // after the last client closes. Reconcile native topology without
        // keeping an SDK thread or client alive solely to preserve that process
        // context.
        if (!changed && kind != DeviceKind::midi_input &&
            kind != DeviceKind::midi_output)
          continue;
        auto snapshot = enumerate(kind);
        if (!snapshot) {
          HardwareEvent event;
          event.kind = HardwareEventKind::failure;
          event.error = snapshot.error();
          impl_->state->publish(std::move(event));
          continue;
        }
        auto &previous = impl_->snapshots[kind];
        for (const auto &info : *snapshot)
          if (std::none_of(previous.begin(), previous.end(),
                           [&](const auto &p) { return p.id == info.id; })) {
            HardwareEvent event;
            event.kind = HardwareEventKind::added;
            event.device = info;
            impl_->state->publish(std::move(event));
          }
        for (const auto &info : previous)
          if (std::none_of(snapshot->begin(), snapshot->end(),
                           [&](const auto &p) { return p.id == info.id; })) {
            HardwareEvent event;
            event.kind = HardwareEventKind::removed;
            event.device = info;
            impl_->state->publish(std::move(event));
          }
        previous = std::move(*snapshot);
      }
    for (auto &[_, session] : impl_->audio) {
      if (session->kind != DeviceKind::audio_input)
        continue;
      HardwareEvent event;
      event.endpoint = session->endpoint;
      event.device.kind = DeviceKind::audio_input;
      event.samples.resize(
          std::min<std::size_t>(impl_->state->options.max_payload_bytes /
                                    sizeof(float) / session->config.channels,
                                session->config.frames_per_buffer) *
          session->config.channels);
      const auto count = session->ring.read(event.samples);
      if (count) {
        event.samples.resize(count);
        impl_->state->publish(std::move(event));
      }
      if (session->overruns.exchange(0)) {
        HardwareEvent error;
        error.kind = HardwareEventKind::failure;
        error.endpoint = session->endpoint;
        error.error = Error{ErrorCode::would_block, {}};
        impl_->state->publish(std::move(error));
      }
    }
  }
}
} // namespace detail
} // namespace ii::io
@implementation IIHardwareBluetoothDelegate
- (void)centralManagerDidUpdateState:(CBCentralManager *)central {
  (void)central;
  if (context)
    context->updated();
}
- (void)centralManager:(CBCentralManager *)central
    didDiscoverPeripheral:(CBPeripheral *)peripheral
        advertisementData:(NSDictionary *)data
                     RSSI:(NSNumber *)rssi {
  (void)central;
  (void)rssi;
  if (context)
    context->discovered(peripheral, data);
}
- (void)centralManager:(CBCentralManager *)central
    didConnectPeripheral:(CBPeripheral *)peripheral {
  (void)central;
  if (context)
    context->connection(peripheral, true);
}
- (void)centralManager:(CBCentralManager *)central
    didDisconnectPeripheral:(CBPeripheral *)peripheral
                      error:(NSError *)error {
  (void)central;
  if (context)
    context->connection(peripheral, false, error);
}
- (void)centralManager:(CBCentralManager *)central
    didFailToConnectPeripheral:(CBPeripheral *)peripheral
                         error:(NSError *)error {
  (void)central;
  if (context)
    context->connection(peripheral, false, error);
}
- (void)peripheral:(CBPeripheral *)peripheral
    didDiscoverServices:(NSError *)error {
  if (!context)
    return;
  if (error) {
    context->error(error);
    return;
  }
  for (CBService *service in peripheral.services)
    [peripheral discoverCharacteristics:nil forService:service];
}
- (void)peripheral:(CBPeripheral *)peripheral
    didDiscoverCharacteristicsForService:(CBService *)service
                                   error:(NSError *)error {
  if (context)
    context->characteristics(peripheral, service, error);
}
- (void)peripheral:(CBPeripheral *)peripheral
    didUpdateValueForCharacteristic:(CBCharacteristic *)characteristic
                              error:(NSError *)error {
  if (context)
    context->value(peripheral, characteristic, error);
}
- (void)peripheral:(CBPeripheral *)peripheral
    didWriteValueForCharacteristic:(CBCharacteristic *)characteristic
                             error:(NSError *)error {
  (void)peripheral;
  (void)characteristic;
  if (context && error)
    context->error(error);
}
- (void)peripheral:(CBPeripheral *)peripheral
    didUpdateNotificationStateForCharacteristic:
        (CBCharacteristic *)characteristic
                                          error:(NSError *)error {
  (void)peripheral;
  (void)characteristic;
  if (context && error)
    context->error(error);
}
@end
