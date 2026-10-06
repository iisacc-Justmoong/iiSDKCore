#include "HardwareInternal.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
// clang-format off
#include <windows.h>
#include <hidsdi.h>
#include <hidpi.h>
// clang-format on
#include <audioclient.h>
#include <cfgmgr32.h>
#include <cstring>
#include <propkeydef.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmdeviceapi.h>
#include <setupapi.h>
#include <initguid.h>
#include <usbiodef.h>
#include <winrt/Windows.Devices.Bluetooth.Advertisement.h>
#include <winrt/Windows.Devices.Bluetooth.GenericAttributeProfile.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Devices.Midi.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winusb.h>
#if __has_include(<winrt/Windows.Devices.Midi2.h>) && __has_include(<winrt/Windows.Devices.Midi2.Enumeration.h>)
#define II_WINDOWS_UMP 1
#include <winrt/Windows.Devices.Midi2.Enumeration.h>
#include <winrt/Windows.Devices.Midi2.h>
#endif
#include <algorithm>
#include <charconv>
#include <map>
#include <set>

namespace ii::io {
HardwareCapabilities hardware_capabilities() noexcept {
#if defined(II_WINDOWS_UMP)
  return {true, true, true, true, true, true, true};
#else
  return {true, true, true, true, true, true, false};
#endif
}
namespace detail {
namespace {
namespace enumeration = winrt::Windows::Devices::Enumeration;
namespace bt = winrt::Windows::Devices::Bluetooth;
namespace adv = bt::Advertisement;
namespace gatt = bt::GenericAttributeProfile;
namespace streams = winrt::Windows::Storage::Streams;
auto failure(ErrorCode code) { return std::unexpected(Error{code, {}}); }
auto native_error(DWORD number = GetLastError()) {
  const auto code = number == ERROR_ACCESS_DENIED ? ErrorCode::permission_denied
                    : number == ERROR_FILE_NOT_FOUND    ? ErrorCode::not_found
                    : number == ERROR_OPERATION_ABORTED ? ErrorCode::cancelled
                    : number == ERROR_SEM_TIMEOUT || number == WAIT_TIMEOUT
                        ? ErrorCode::timed_out
                        : ErrorCode::system_error;
  return std::unexpected(
      Error{code, {static_cast<int>(number), std::system_category()}});
}
auto hresult_error(HRESULT status) {
  return std::unexpected(
      Error{status == E_ACCESSDENIED       ? ErrorCode::permission_denied
            : status == RPC_E_WRONG_THREAD ? ErrorCode::busy
                                           : ErrorCode::system_error,
            {status, std::system_category()}});
}
struct Apartment final {
  APTTYPE type{};
  APTTYPEQUALIFIER qualifier{};
  HRESULT status = CoGetApartmentType(&type, &qualifier);
  bool mta() const { return SUCCEEDED(status) && type == APTTYPE_MTA; }
};
struct Handle final {
  HANDLE value = INVALID_HANDLE_VALUE;
  ~Handle() {
    if (value != INVALID_HANDLE_VALUE && value)
      CloseHandle(value);
  }
};
struct FileSession final {
  Handle file, event;
  WINUSB_INTERFACE_HANDLE usb = nullptr;
  PHIDP_PREPARSED_DATA parsed = nullptr;
  OVERLAPPED read{};
  std::vector<std::byte> input;
  bool pending = false, failed = false;
  ~FileSession() {
    if (pending) {
      CancelIoEx(file.value, &read);
      DWORD count = 0;
      GetOverlappedResult(file.value, &read, &count, true);
    }
    if (parsed)
      HidD_FreePreparsedData(parsed);
    if (usb)
      WinUsb_Free(usb);
  }
  Result<void> arm() {
    read = {};
    read.hEvent = event.value;
    ResetEvent(event.value);
    DWORD count = 0;
    if (!ReadFile(file.value, input.data(), static_cast<DWORD>(input.size()),
                  &count, &read) &&
        GetLastError() != ERROR_IO_PENDING)
      return native_error();
    pending = true;
    return {};
  }
};
struct AudioSession final {
  winrt::com_ptr<IAudioClient> client;
  winrt::com_ptr<IAudioCaptureClient> capture;
  winrt::com_ptr<IAudioRenderClient> render;
  AudioConfig config;
  DeviceKind kind;
  UINT32 frames = 0;
  ~AudioSession() {
    if (client)
      client->Stop();
  }
};
struct BluetoothState final {
  std::shared_ptr<HardwareState> state;
  runtime::TaskScope callbacks;
  std::mutex mutex;
  adv::BluetoothLEAdvertisementWatcher watcher{nullptr};
  winrt::event_token received{}, stopped{};
  std::map<std::string, DeviceInfo> devices;
  std::map<std::uint64_t, bt::BluetoothLEDevice> connections;
  struct Subscription final {
    gatt::GattCharacteristic characteristic{nullptr};
    winrt::event_token token{};
  };
  std::map<std::pair<std::uint64_t, std::string>, Subscription> subscriptions;
  explicit BluetoothState(std::shared_ptr<HardwareState> value)
      : state(std::move(value)) {}
  void shutdown() {
    if (watcher) {
      watcher.Stop();
      watcher.Received(received);
      watcher.Stopped(stopped);
    }
    (void)callbacks.close();
    for (auto &[_, entry] : subscriptions)
      entry.characteristic.ValueChanged(entry.token);
    for (auto &[_, device] : connections)
      device.Close();
    watcher = nullptr;
    subscriptions.clear();
    connections.clear();
  }
  ~BluetoothState() {
    try {
      shutdown();
    } catch (...) {
    }
  }
};
#if defined(II_WINDOWS_UMP)
namespace midi = winrt::Windows::Devices::Midi2;
struct MidiSession final {
  midi::MidiSession session{nullptr};
  midi::MidiEndpointConnection connection{nullptr};
  winrt::event_token received{};
  ~MidiSession() {
    try {
      if (connection)
        connection.MessageReceived(received);
      if (session) {
        if (connection)
          session.DisconnectEndpointConnection(connection.ConnectionId());
        session.Close();
      }
    } catch (...) {
    }
  }
};
#else
namespace midi = winrt::Windows::Devices::Midi;
struct MidiSession final {
  midi::MidiInPort input{nullptr};
  midi::IMidiOutPort output{nullptr};
  winrt::event_token received{};
  ~MidiSession() {
    try {
      if (input) {
        input.MessageReceived(received);
        input.Close();
      }
      if (output)
        output.as<winrt::Windows::Foundation::IClosable>().Close();
    } catch (...) {
    }
  }
};
#endif
Result<std::vector<DeviceInfo>> interfaces(DeviceKind kind, const GUID &guid) {
  HDEVINFO information = SetupDiGetClassDevsW(
      &guid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
  if (information == INVALID_HANDLE_VALUE)
    return native_error();
  std::vector<DeviceInfo> result;
  for (DWORD index = 0;; ++index) {
    SP_DEVICE_INTERFACE_DATA interface_data{};
    interface_data.cbSize = sizeof(interface_data);
    if (!SetupDiEnumDeviceInterfaces(information, nullptr, &guid, index,
                                     &interface_data)) {
      if (GetLastError() == ERROR_NO_MORE_ITEMS)
        break;
      auto error = native_error();
      SetupDiDestroyDeviceInfoList(information);
      return error;
    }
    DWORD size = 0;
    SetupDiGetDeviceInterfaceDetailW(information, &interface_data, nullptr, 0,
                                     &size, nullptr);
    std::vector<std::byte> bytes(size);
    auto *detail =
        reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W *>(bytes.data());
    detail->cbSize = sizeof(*detail);
    SP_DEVINFO_DATA device{};
    device.cbSize = sizeof(device);
    if (!SetupDiGetDeviceInterfaceDetailW(information, &interface_data, detail,
                                          size, nullptr, &device))
      continue;
    DeviceInfo info;
    info.kind = kind;
    info.id = winrt::to_string(detail->DevicePath);
    wchar_t name[512]{};
    if (SetupDiGetDeviceRegistryPropertyW(
            information, &device, SPDRP_FRIENDLYNAME, nullptr,
            reinterpret_cast<BYTE *>(name), sizeof(name), nullptr) ||
        SetupDiGetDeviceRegistryPropertyW(
            information, &device, SPDRP_DEVICEDESC, nullptr,
            reinterpret_cast<BYTE *>(name), sizeof(name), nullptr))
      info.name = winrt::to_string(name);
    if (kind == DeviceKind::hid) {
      Handle handle;
      handle.value =
          CreateFileW(detail->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                      nullptr, OPEN_EXISTING, 0, nullptr);
      if (handle.value != INVALID_HANDLE_VALUE) {
        HIDD_ATTRIBUTES attributes{};
        attributes.Size = sizeof(attributes);
        if (HidD_GetAttributes(handle.value, &attributes)) {
          info.vendor = attributes.VendorID;
          info.product = attributes.ProductID;
        }
        PHIDP_PREPARSED_DATA parsed = nullptr;
        if (HidD_GetPreparsedData(handle.value, &parsed)) {
          HIDP_CAPS caps{};
          if (HidP_GetCaps(parsed, &caps) == HIDP_STATUS_SUCCESS) {
            info.usage_page = caps.UsagePage;
            info.usage = caps.Usage;
          }
          HidD_FreePreparsedData(parsed);
        }
      }
    }
    result.push_back(std::move(info));
  }
  SetupDiDestroyDeviceInfoList(information);
  return result;
}
} // namespace
struct NativeHardware::Impl {
  std::shared_ptr<HardwareState> state;
  std::uint64_t next = 0;
  std::map<std::uint64_t, DeviceKind> kinds;
  std::map<std::uint64_t, std::unique_ptr<FileSession>> files;
  std::map<std::uint64_t, std::unique_ptr<AudioSession>> audio;
  std::shared_ptr<BluetoothState> bluetooth;
  std::shared_ptr<std::atomic<bool>> dirty =
      std::make_shared<std::atomic<bool>>(false);
  std::vector<HCMNOTIFICATION> notifications;
  std::map<DeviceKind, enumeration::DeviceWatcher> watchers;
  std::map<DeviceKind, std::vector<DeviceInfo>> snapshots;
  std::map<std::uint64_t, std::unique_ptr<MidiSession>> midi;
  explicit Impl(std::shared_ptr<HardwareState> value)
      : state(std::move(value)) {}
  ~Impl() {
    for (auto notification : notifications)
      CM_Unregister_Notification(notification);
    for (auto &[_, watcher] : watchers)
      try {
        if (watcher.Status() == enumeration::DeviceWatcherStatus::Started ||
            watcher.Status() ==
                enumeration::DeviceWatcherStatus::EnumerationCompleted)
          watcher.Stop();
      } catch (...) {
      }
    if (bluetooth)
      try {
        bluetooth->shutdown();
      } catch (...) {
      }
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
  static DWORD CALLBACK changed(HCMNOTIFICATION, void *context,
                                CM_NOTIFY_ACTION, PCM_NOTIFY_EVENT_DATA,
                                DWORD) {
    static_cast<std::atomic<bool> *>(context)->store(true);
    return ERROR_SUCCESS;
  }
};
NativeHardware::NativeHardware(std::shared_ptr<HardwareState> state)
    : impl_(std::make_unique<Impl>(std::move(state))) {}
NativeHardware::~NativeHardware() = default;
Result<std::vector<DeviceInfo>> NativeHardware::enumerate(DeviceKind kind) {
  try {
    Apartment apartment;
    std::vector<DeviceInfo> result;
    if (kind == DeviceKind::usb || kind == DeviceKind::hid) {
      std::vector<GUID> guids;
      GUID guid;
      if (kind == DeviceKind::usb) {
        guids.push_back(GUID_DEVINTERFACE_USB_DEVICE);
        for (const auto &id : impl_->state->options.usb_interface_guids)
          guids.push_back(winrt::guid(winrt::to_hstring(id)));
      } else {
        HidD_GetHidGuid(&guid);
        guids.push_back(guid);
      }
      for (const auto &item : guids) {
        auto found = interfaces(kind, item);
        if (!found)
          return std::unexpected(found.error());
        for (auto &info : *found)
          if (std::none_of(
                  result.begin(), result.end(),
                  [&](const auto &prior) { return prior.id == info.id; }))
            result.push_back(std::move(info));
      }
      return result;
    }
    if (kind == DeviceKind::bluetooth) {
      if (impl_->bluetooth) {
        std::lock_guard lock(impl_->bluetooth->mutex);
        for (const auto &[_, info] : impl_->bluetooth->devices)
          result.push_back(info);
      }
      return result;
    }
    if (!apartment.mta())
      return failure(ErrorCode::busy);
    if (kind == DeviceKind::audio_input || kind == DeviceKind::audio_output) {
      winrt::com_ptr<IMMDeviceEnumerator> enumerator;
      winrt::check_hresult(CoCreateInstance(
          __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
          __uuidof(IMMDeviceEnumerator), enumerator.put_void()));
      winrt::com_ptr<IMMDeviceCollection> devices;
      winrt::check_hresult(enumerator->EnumAudioEndpoints(
          kind == DeviceKind::audio_input ? eCapture : eRender,
          DEVICE_STATE_ACTIVE, devices.put()));
      UINT count = 0;
      winrt::check_hresult(devices->GetCount(&count));
      for (UINT i = 0; i < count; ++i) {
        winrt::com_ptr<IMMDevice> device;
        winrt::check_hresult(devices->Item(i, device.put()));
        LPWSTR identifier = nullptr;
        winrt::check_hresult(device->GetId(&identifier));
        DeviceInfo info;
        info.kind = kind;
        info.id = winrt::to_string(identifier);
        CoTaskMemFree(identifier);
        winrt::com_ptr<IPropertyStore> properties;
        winrt::check_hresult(
            device->OpenPropertyStore(STGM_READ, properties.put()));
        PROPVARIANT name{};
        if (SUCCEEDED(properties->GetValue(PKEY_Device_FriendlyName, &name)) &&
            name.vt == VT_LPWSTR)
          info.name = winrt::to_string(name.pwszVal);
        PropVariantClear(&name);
        result.push_back(std::move(info));
      }
      return result;
    }
#if defined(II_WINDOWS_UMP)
    if (!midi::MidiApi::EnsureServiceAvailable())
      return failure(ErrorCode::unsupported);
    auto devices = midi::Enumeration::MidiEndpointDeviceInformation::FindAll();
    for (const auto &device : devices) {
      DeviceInfo info;
      info.kind = kind;
      info.id = winrt::to_string(device.EndpointDeviceId());
      info.name = winrt::to_string(device.Name());
      result.push_back(std::move(info));
    }
    return result;
#else
    auto devices = enumeration::DeviceInformation::FindAllAsync(
                       kind == DeviceKind::midi_input
                           ? midi::MidiInPort::GetDeviceSelector()
                           : midi::MidiOutPort::GetDeviceSelector())
                       .get();
    for (const auto &device : devices) {
      DeviceInfo info;
      info.kind = kind;
      info.id = (kind == DeviceKind::midi_input ? "midi-in:" : "midi-out:") +
                winrt::to_string(device.Id());
      info.name = winrt::to_string(device.Name());
      result.push_back(std::move(info));
    }
    return result;
#endif
  } catch (const winrt::hresult_error &error) {
    return hresult_error(error.code());
  }
}
Result<void> NativeHardware::watch(DeviceKind kind) {
  try {
    if (impl_->snapshots.contains(kind))
      return {};
    Apartment apartment;
    if (kind == DeviceKind::bluetooth)
      return failure(ErrorCode::invalid_argument);
    if (kind == DeviceKind::usb || kind == DeviceKind::hid) {
      std::vector<GUID> guids;
      GUID guid;
      if (kind == DeviceKind::usb) {
        guids.push_back(GUID_DEVINTERFACE_USB_DEVICE);
        for (const auto &id : impl_->state->options.usb_interface_guids)
          guids.push_back(winrt::guid(winrt::to_hstring(id)));
      } else {
        HidD_GetHidGuid(&guid);
        guids.push_back(guid);
      }
      for (const auto &item : guids) {
        CM_NOTIFY_FILTER filter{};
        filter.cbSize = sizeof(filter);
        filter.FilterType = CM_NOTIFY_FILTER_TYPE_DEVICEINTERFACE;
        filter.u.DeviceInterface.ClassGuid = item;
        HCMNOTIFICATION notification = nullptr;
        const auto status = CM_Register_Notification(
            &filter, impl_->dirty.get(), Impl::changed, &notification);
        if (status != CR_SUCCESS)
          return native_error(status);
        impl_->notifications.push_back(notification);
      }
    } else {
      if (!apartment.mta())
        return failure(ErrorCode::busy);
      enumeration::DeviceWatcher watcher{nullptr};
      if (kind == DeviceKind::audio_input || kind == DeviceKind::audio_output)
        watcher = enumeration::DeviceInformation::CreateWatcher(
            kind == DeviceKind::audio_input
                ? enumeration::DeviceClass::AudioCapture
                : enumeration::DeviceClass::AudioRender);
#if defined(II_WINDOWS_UMP)
      else
        watcher = enumeration::DeviceInformation::CreateWatcher(
            midi::MidiEndpointConnection::GetDeviceSelector());
#else
      else
        watcher = enumeration::DeviceInformation::CreateWatcher(
            kind == DeviceKind::midi_input
                ? midi::MidiInPort::GetDeviceSelector()
                : midi::MidiOutPort::GetDeviceSelector());
#endif
      std::weak_ptr<std::atomic<bool>> weak = impl_->dirty;
      watcher.Added([weak](auto &&, auto &&) {
        if (auto value = weak.lock())
          *value = true;
      });
      watcher.Removed([weak](auto &&, auto &&) {
        if (auto value = weak.lock())
          *value = true;
      });
      watcher.Updated([weak](auto &&, auto &&) {
        if (auto value = weak.lock())
          *value = true;
      });
      watcher.Start();
      impl_->watchers.insert_or_assign(kind, watcher);
    }
    impl_->snapshots[kind] = {};
    *impl_->dirty = true;
    return {};
  } catch (const winrt::hresult_error &error) {
    return hresult_error(error.code());
  }
}
Result<Endpoint> NativeHardware::open(DeviceKind kind, std::string_view id,
                                      AudioConfig config) {
  Endpoint provisional;
  try {
    Apartment apartment;
    const auto path = winrt::to_hstring(id);
    if (kind == DeviceKind::usb || kind == DeviceKind::hid) {
      auto session = std::make_unique<FileSession>();
      session->file.value =
          CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                      FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                      OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
      if (session->file.value == INVALID_HANDLE_VALUE)
        return native_error();
      if (kind == DeviceKind::usb) {
        if (!WinUsb_Initialize(session->file.value, &session->usb))
          return native_error();
      } else {
        if (!HidD_GetPreparsedData(session->file.value, &session->parsed))
          return native_error();
        HIDP_CAPS caps{};
        if (HidP_GetCaps(session->parsed, &caps) != HIDP_STATUS_SUCCESS ||
            !caps.InputReportByteLength)
          return failure(ErrorCode::unsupported);
        if (caps.InputReportByteLength >
            impl_->state->options.max_payload_bytes)
          return failure(ErrorCode::invalid_argument);
        session->input.resize(caps.InputReportByteLength);
        session->event.value = CreateEventW(nullptr, true, false, nullptr);
        if (!session->event.value)
          return native_error();
        auto armed = session->arm();
        if (!armed)
          return std::unexpected(armed.error());
      }
      const auto ep = impl_->endpoint(kind);
      provisional = ep;
      impl_->files[ep.value] = std::move(session);
      return ep;
    }
    if (!apartment.mta())
      return failure(ErrorCode::busy);
    if (kind == DeviceKind::audio_input || kind == DeviceKind::audio_output) {
      auto session = std::make_unique<AudioSession>();
      session->kind = kind;
      session->config = config;
      winrt::com_ptr<IMMDeviceEnumerator> enumerator;
      winrt::check_hresult(CoCreateInstance(
          __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
          __uuidof(IMMDeviceEnumerator), enumerator.put_void()));
      winrt::com_ptr<IMMDevice> device;
      if (id.empty())
        winrt::check_hresult(enumerator->GetDefaultAudioEndpoint(
            kind == DeviceKind::audio_input ? eCapture : eRender, eMultimedia,
            device.put()));
      else
        winrt::check_hresult(enumerator->GetDevice(path.c_str(), device.put()));
      winrt::check_hresult(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL,
                                            nullptr,
                                            session->client.put_void()));
      WAVEFORMATEX format{};
      format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
      format.nChannels = static_cast<WORD>(config.channels);
      format.nSamplesPerSec = static_cast<DWORD>(config.sample_rate);
      format.wBitsPerSample = 32;
      format.nBlockAlign = 4 * format.nChannels;
      format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
      winrt::check_hresult(session->client->Initialize(
          AUDCLNT_SHAREMODE_SHARED,
          AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
              AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
          static_cast<REFERENCE_TIME>(10000000.0 * config.queued_frames /
                                      config.sample_rate),
          0, &format, nullptr));
      winrt::check_hresult(session->client->GetBufferSize(&session->frames));
      if (kind == DeviceKind::audio_input)
        winrt::check_hresult(session->client->GetService(
            __uuidof(IAudioCaptureClient), session->capture.put_void()));
      else
        winrt::check_hresult(session->client->GetService(
            __uuidof(IAudioRenderClient), session->render.put_void()));
      winrt::check_hresult(session->client->Start());
      const auto ep = impl_->endpoint(kind);
      provisional = ep;
      impl_->audio[ep.value] = std::move(session);
      return ep;
    }
    if (kind == DeviceKind::bluetooth) {
      if (!impl_->bluetooth)
        return failure(ErrorCode::not_found);
      if (!id.starts_with("ble:"))
        return failure(ErrorCode::invalid_argument);
      id.remove_prefix(4);
      std::uint64_t address = 0;
      const auto parsed =
          std::from_chars(id.data(), id.data() + id.size(), address);
      if (parsed.ec != std::errc{} || parsed.ptr != id.data() + id.size())
        return failure(ErrorCode::invalid_argument);
      {
        std::lock_guard lock(impl_->bluetooth->mutex);
        for (const auto &[_, prior] : impl_->bluetooth->connections)
          if (prior.BluetoothAddress() == address)
            return failure(ErrorCode::busy);
      }
      auto device =
          bt::BluetoothLEDevice::FromBluetoothAddressAsync(address).get();
      if (!device)
        return failure(ErrorCode::not_found);
      const auto ep = impl_->endpoint(kind);
      provisional = ep;
      {
        std::lock_guard lock(impl_->bluetooth->mutex);
        impl_->bluetooth->connections.insert_or_assign(ep.value, device);
      }
      HardwareEvent event;
      event.kind = HardwareEventKind::connected;
      event.endpoint = ep;
      event.device.kind = kind;
      impl_->state->publish(std::move(event));
      return ep;
    }
#if defined(II_WINDOWS_UMP)
    if (kind == DeviceKind::midi_input) {
      if (!midi::MidiApi::EnsureServiceAvailable())
        return failure(ErrorCode::unsupported);
      auto session = std::make_unique<MidiSession>();
      session->session = midi::MidiSession::Create(L"iiSDKCore");
      if (!session->session)
        return failure(ErrorCode::unsupported);
      session->connection = session->session.CreateEndpointConnection(path);
      if (!session->connection)
        return failure(ErrorCode::not_found);
      const auto ep = impl_->endpoint(DeviceKind::midi_output);
      provisional = ep;
      std::weak_ptr<HardwareState> weak = impl_->state;
      session->received = session->connection.MessageReceived(
          [weak, ep](auto &&, const midi::MidiMessageReceivedEventArgs &args) {
            auto state = weak.lock();
            if (!state)
              return;
            protocol::UMP packet;
            packet.size = args.FillWords(packet.words[0], packet.words[1],
                                         packet.words[2], packet.words[3]);
            if (packet.size < 1 || packet.size > 4)
              return;
            HardwareEvent event;
            event.endpoint = ep;
            event.device.kind = DeviceKind::midi_input;
            event.midi.push_back(packet);
            state->publish(std::move(event));
          });
      if (!session->connection.Open()) {
        impl_->kinds.erase(ep.value);
        impl_->state->deactivate(ep);
        return failure(ErrorCode::system_error);
      }
      impl_->midi[ep.value] = std::move(session);
      return ep;
    }
#else
    if (kind == DeviceKind::midi_input) {
      const bool input = id.starts_with("midi-in:");
      if (!input && !id.starts_with("midi-out:"))
        return failure(ErrorCode::invalid_argument);
      auto session = std::make_unique<MidiSession>();
      const auto native_id = winrt::to_hstring(id.substr(input ? 8 : 9));
      if (input)
        session->input = midi::MidiInPort::FromIdAsync(native_id).get();
      else
        session->output = midi::MidiOutPort::FromIdAsync(native_id).get();
      if (!session->input && !session->output)
        return failure(ErrorCode::not_found);
      const auto ep = impl_->endpoint(input ? DeviceKind::midi_input
                                            : DeviceKind::midi_output);
      provisional = ep;
      if (input) {
        std::weak_ptr<HardwareState> weak = impl_->state;
        session->received = session->input.MessageReceived(
            [weak, ep](auto &&,
                       const midi::MidiMessageReceivedEventArgs &args) {
              auto state = weak.lock();
              if (!state)
                return;
              auto buffer = args.Message().RawData();
              if (buffer.Length() > state->options.max_payload_bytes)
                return;
              std::vector<std::uint8_t> raw(buffer.Length());
              streams::DataReader::FromBuffer(buffer).ReadBytes(raw);
              protocol::MIDI1Decoder decoder(std::clamp<std::size_t>(
                  state->options.max_payload_bytes, 2, 1024 * 1024));
              auto messages = decoder.feed(std::as_bytes(std::span(raw)));
              HardwareEvent event;
              event.endpoint = ep;
              event.device.kind = DeviceKind::midi_input;
              if (!messages) {
                event.kind = HardwareEventKind::failure;
                event.error = messages.error();
              } else
                for (const auto &message : *messages) {
                  auto packet = protocol::midi1_ump(message);
                  if (!packet) {
                    event.kind = HardwareEventKind::failure;
                    event.error = Error{ErrorCode::unsupported, {}};
                    break;
                  }
                  if (event.midi.size() >=
                      state->options.max_payload_bytes / sizeof(protocol::UMP))
                    return;
                  event.midi.push_back(*packet);
                }
              state->publish(std::move(event));
            });
      }
      impl_->midi[ep.value] = std::move(session);
      return ep;
    }
#endif
    return failure(ErrorCode::unsupported);
  } catch (const winrt::hresult_error &error) {
    if (provisional.value) {
      impl_->state->deactivate(provisional);
      try {
        (void)close(provisional);
      } catch (...) {
        impl_->kinds.erase(provisional.value);
      }
    }
    return hresult_error(error.code());
  }
}
Result<std::vector<std::byte>>
NativeHardware::usb_control(Endpoint ep, protocol::USBSetup setup,
                            std::span<const std::byte> output,
                            std::chrono::milliseconds timeout) {
  if (!impl_->valid(ep))
    return failure(ErrorCode::closed);
  auto found = impl_->files.find(ep.value);
  if (found == impl_->files.end() || !found->second->usb)
    return failure(ErrorCode::invalid_argument);
  std::vector<std::byte> data(setup.length);
  std::copy(output.begin(), output.end(), data.begin());
  ULONG interval = static_cast<ULONG>(timeout.count());
  if (!WinUsb_SetPipePolicy(found->second->usb, 0, PIPE_TRANSFER_TIMEOUT,
                            sizeof(interval), &interval))
    return native_error();
  WINUSB_SETUP_PACKET request{setup.request_type, setup.request, setup.value,
                              setup.index, setup.length};
  ULONG count = 0;
  if (!WinUsb_ControlTransfer(found->second->usb, request,
                              reinterpret_cast<PUCHAR>(data.data()),
                              static_cast<ULONG>(data.size()), &count, nullptr))
    return native_error();
  if (count > data.size())
    return failure(ErrorCode::system_error);
  if (!(setup.request_type & 0x80))
    return std::vector<std::byte>{};
  data.resize(count);
  return data;
}
Result<std::vector<std::byte>> NativeHardware::hid_descriptor(Endpoint ep) {
  if (!impl_->valid(ep))
    return failure(ErrorCode::closed);
  return failure(ErrorCode::unsupported);
}
Result<std::vector<std::byte>>
NativeHardware::hid_get(Endpoint ep, protocol::HIDReportType type,
                        std::uint8_t id, std::size_t length) {
  if (!impl_->valid(ep))
    return failure(ErrorCode::closed);
  auto found = impl_->files.find(ep.value);
  if (found == impl_->files.end() || found->second->usb)
    return failure(ErrorCode::invalid_argument);
  std::vector<std::byte> bytes(length + (id ? 0 : 1));
  bytes[0] = std::byte(id);
  BOOLEAN okay = false;
  if (type == protocol::HIDReportType::input)
    okay = HidD_GetInputReport(found->second->file.value, bytes.data(),
                               static_cast<ULONG>(bytes.size()));
  else if (type == protocol::HIDReportType::feature)
    okay = HidD_GetFeature(found->second->file.value, bytes.data(),
                           static_cast<ULONG>(bytes.size()));
  else
    return failure(ErrorCode::unsupported);
  if (!okay)
    return native_error();
  if (!id)
    bytes.erase(bytes.begin());
  return bytes;
}
Result<void> NativeHardware::hid_set(Endpoint ep, protocol::HIDReportType type,
                                     std::uint8_t id,
                                     std::span<const std::byte> input) {
  if (!impl_->valid(ep))
    return failure(ErrorCode::closed);
  auto found = impl_->files.find(ep.value);
  if (found == impl_->files.end() || found->second->usb)
    return failure(ErrorCode::invalid_argument);
  std::vector<std::byte> bytes(input.begin(), input.end());
  if (!id)
    bytes.insert(bytes.begin(), std::byte{});
  else if (bytes.empty() || bytes[0] != std::byte(id))
    return failure(ErrorCode::invalid_argument);
  BOOLEAN okay = false;
  if (type == protocol::HIDReportType::feature)
    okay = HidD_SetFeature(found->second->file.value, bytes.data(),
                           static_cast<ULONG>(bytes.size()));
  else if (type == protocol::HIDReportType::output)
    okay = HidD_SetOutputReport(found->second->file.value, bytes.data(),
                                static_cast<ULONG>(bytes.size()));
  else
    return failure(ErrorCode::unsupported);
  if (!okay)
    return native_error();
  return {};
}
Result<void>
NativeHardware::scan(std::span<const protocol::BluetoothUUID> services) {
  try {
    Apartment apartment;
    if (!apartment.mta())
      return failure(ErrorCode::busy);
    if (!impl_->bluetooth)
      impl_->bluetooth = std::make_shared<BluetoothState>(impl_->state);
    auto backend = impl_->bluetooth;
    if (backend->watcher) {
      backend->watcher.Stop();
      backend->watcher.Received(backend->received);
      backend->watcher.Stopped(backend->stopped);
    }
    backend->watcher = adv::BluetoothLEAdvertisementWatcher();
    backend->watcher.ScanningMode(adv::BluetoothLEScanningMode::Active);
    for (const auto &service : services)
      backend->watcher.AdvertisementFilter()
          .Advertisement()
          .ServiceUuids()
          .Append(winrt::guid(winrt::to_hstring(service.string())));
    std::weak_ptr<BluetoothState> weak = backend;
    backend->received = backend->watcher.Received(
        [weak](auto &&,
               const adv::BluetoothLEAdvertisementReceivedEventArgs &args) {
          auto backend = weak.lock();
          if (!backend)
            return;
          (void)backend->callbacks.run([&] {
            DeviceInfo info;
            info.kind = DeviceKind::bluetooth;
            info.id = "ble:" + std::to_string(args.BluetoothAddress());
            info.name = winrt::to_string(args.Advertisement().LocalName());
            bool fresh = false;
            {
              std::lock_guard lock(backend->mutex);
              if (!backend->devices.contains(info.id) &&
                  backend->devices.size() >=
                      backend->state->options.event_capacity)
                return;
              fresh = !backend->devices.contains(info.id);
              backend->devices[info.id] = info;
            }
            if (fresh) {
              HardwareEvent event;
              event.kind = HardwareEventKind::added;
              event.device = info;
              backend->state->publish(std::move(event));
            }
          });
        });
    backend->stopped = backend->watcher.Stopped(
        [weak](
            auto &&,
            const adv::BluetoothLEAdvertisementWatcherStoppedEventArgs &args) {
          if (args.Error() == bt::BluetoothError::Success)
            return;
          if (auto backend = weak.lock()) {
            HardwareEvent event;
            event.kind = HardwareEventKind::failure;
            event.error =
                Error{ErrorCode::system_error,
                      {static_cast<int>(args.Error()), std::system_category()}};
            backend->state->publish(std::move(event));
          }
        });
    backend->watcher.Start();
    return {};
  } catch (const winrt::hresult_error &error) {
    return hresult_error(error.code());
  }
}
Result<void> NativeHardware::stop_scan() {
  try {
    if (impl_->bluetooth && impl_->bluetooth->watcher)
      impl_->bluetooth->watcher.Stop();
    return {};
  } catch (const winrt::hresult_error &error) {
    return hresult_error(error.code());
  }
}
Result<void> NativeHardware::gatt(Endpoint ep, protocol::BluetoothUUID service,
                                  protocol::BluetoothUUID characteristic,
                                  int operation,
                                  std::span<const std::byte> bytes, bool flag) {
  try {
    if (!impl_->valid(ep))
      return failure(ErrorCode::closed);
    if (!impl_->bluetooth || impl_->kinds[ep.value] != DeviceKind::bluetooth)
      return failure(ErrorCode::invalid_argument);
    Apartment apartment;
    if (!apartment.mta())
      return failure(ErrorCode::busy);
    bt::BluetoothLEDevice device{nullptr};
    {
      std::lock_guard lock(impl_->bluetooth->mutex);
      device = impl_->bluetooth->connections.at(ep.value);
    }
    auto services = device
                        .GetGattServicesForUuidAsync(
                            winrt::guid(winrt::to_hstring(service.string())),
                            bt::BluetoothCacheMode::Uncached)
                        .get();
    if (services.Status() != gatt::GattCommunicationStatus::Success)
      return failure(ErrorCode::system_error);
    gatt::GattCharacteristic target{nullptr};
    for (const auto &s : services.Services()) {
      auto characteristics =
          s.GetCharacteristicsForUuidAsync(
               winrt::guid(winrt::to_hstring(characteristic.string())))
              .get();
      if (characteristics.Status() == gatt::GattCommunicationStatus::Success &&
          characteristics.Characteristics().Size()) {
        target = characteristics.Characteristics().GetAt(0);
        break;
      }
    }
    if (!target)
      return failure(ErrorCode::not_found);
    if (operation == 0) {
      auto result =
          target.ReadValueAsync(bt::BluetoothCacheMode::Uncached).get();
      if (result.Status() != gatt::GattCommunicationStatus::Success)
        return failure(ErrorCode::system_error);
      if (result.Value().Length() > impl_->state->options.max_payload_bytes)
        return failure(ErrorCode::invalid_argument);
      HardwareEvent event;
      event.endpoint = ep;
      event.device.kind = DeviceKind::bluetooth;
      event.service = service.string();
      event.characteristic = characteristic.string();
      event.bytes.resize(result.Value().Length());
      streams::DataReader reader =
          streams::DataReader::FromBuffer(result.Value());
      reader.ReadBytes({reinterpret_cast<std::uint8_t *>(event.bytes.data()),
                        static_cast<std::uint32_t>(event.bytes.size())});
      impl_->state->publish(std::move(event));
    } else if (operation == 1) {
      streams::DataWriter writer;
      writer.WriteBytes({reinterpret_cast<const std::uint8_t *>(bytes.data()),
                         static_cast<std::uint32_t>(bytes.size())});
      const auto status =
          target
              .WriteValueAsync(
                  writer.DetachBuffer(),
                  flag ? gatt::GattWriteOption::WriteWithResponse
                       : gatt::GattWriteOption::WriteWithoutResponse)
              .get();
      if (status != gatt::GattCommunicationStatus::Success)
        return failure(ErrorCode::system_error);
    } else {
      const auto key =
          std::pair{ep.value, service.string() + "/" + characteristic.string()};
      auto found = impl_->bluetooth->subscriptions.find(key);
      if (found != impl_->bluetooth->subscriptions.end()) {
        found->second.characteristic.ValueChanged(found->second.token);
        impl_->bluetooth->subscriptions.erase(found);
      }
      const auto status =
          target
              .WriteClientCharacteristicConfigurationDescriptorAsync(
                  flag
                      ? ((target.CharacteristicProperties() &
                          gatt::GattCharacteristicProperties::Notify) !=
                                 gatt::GattCharacteristicProperties::None
                             ? gatt::
                                   GattClientCharacteristicConfigurationDescriptorValue::
                                       Notify
                             : gatt::
                                   GattClientCharacteristicConfigurationDescriptorValue::
                                       Indicate)
                      : gatt::
                            GattClientCharacteristicConfigurationDescriptorValue::
                                None)
              .get();
      if (status != gatt::GattCommunicationStatus::Success)
        return failure(ErrorCode::system_error);
      if (flag) {
        std::weak_ptr<HardwareState> weak = impl_->state;
        auto token = target.ValueChanged(
            [weak, ep, service, characteristic](
                auto &&, const gatt::GattValueChangedEventArgs &args) {
              auto state = weak.lock();
              if (!state || args.CharacteristicValue().Length() >
                                state->options.max_payload_bytes)
                return;
              HardwareEvent event;
              event.endpoint = ep;
              event.device.kind = DeviceKind::bluetooth;
              event.service = service.string();
              event.characteristic = characteristic.string();
              event.bytes.resize(args.CharacteristicValue().Length());
              auto reader =
                  streams::DataReader::FromBuffer(args.CharacteristicValue());
              reader.ReadBytes(
                  {reinterpret_cast<std::uint8_t *>(event.bytes.data()),
                   static_cast<std::uint32_t>(event.bytes.size())});
              state->publish(std::move(event));
            });
        impl_->bluetooth->subscriptions[key] = {target, token};
      }
    }
    return {};
  } catch (const winrt::hresult_error &error) {
    return hresult_error(error.code());
  }
}
Result<std::size_t> NativeHardware::audio_write(Endpoint ep,
                                                std::span<const float> data) {
  if (!impl_->valid(ep))
    return failure(ErrorCode::closed);
  auto found = impl_->audio.find(ep.value);
  if (found == impl_->audio.end() || !found->second->render ||
      data.size() % found->second->config.channels)
    return failure(ErrorCode::invalid_argument);
  auto &audio = *found->second;
  UINT32 padding = 0;
  auto status = audio.client->GetCurrentPadding(&padding);
  if (FAILED(status))
    return hresult_error(status);
  const auto frames = static_cast<UINT32>(std::min<std::size_t>(
      audio.frames - padding, data.size() / audio.config.channels));
  if (!frames)
    return std::size_t(0);
  BYTE *destination = nullptr;
  status = audio.render->GetBuffer(frames, &destination);
  if (FAILED(status))
    return hresult_error(status);
  memcpy(destination, data.data(),
         frames * audio.config.channels * sizeof(float));
  status = audio.render->ReleaseBuffer(frames, 0);
  if (FAILED(status))
    return hresult_error(status);
  return std::size_t(frames) * audio.config.channels;
}
Result<Endpoint> NativeHardware::virtual_midi(std::string_view, bool) {
  return failure(ErrorCode::unsupported);
}
Result<void> NativeHardware::midi_send(Endpoint ep,
                                       std::span<const protocol::UMP> packets) {
  if (!impl_->valid(ep))
    return failure(ErrorCode::closed);
#if defined(II_WINDOWS_UMP)
  auto found = impl_->midi.find(ep.value);
  if (found == impl_->midi.end())
    return failure(ErrorCode::invalid_argument);
  for (const auto &p : packets) {
    midi::MidiSendMessageResults result;
    switch (p.size) {
    case 1:
      result = found->second->connection.SendSingleMessageWords(0, p.words[0]);
      break;
    case 2:
      result = found->second->connection.SendSingleMessageWords(0, p.words[0],
                                                                p.words[1]);
      break;
    case 3:
      result = found->second->connection.SendSingleMessageWords(
          0, p.words[0], p.words[1], p.words[2]);
      break;
    default:
      result = found->second->connection.SendSingleMessageWords(
          0, p.words[0], p.words[1], p.words[2], p.words[3]);
    }
    if (midi::MidiEndpointConnection::SendMessageFailed(result))
      return failure(ErrorCode::system_error);
  }
  return {};
#else
  auto found = impl_->midi.find(ep.value);
  if (found == impl_->midi.end())
    return failure(ErrorCode::invalid_argument);
  if (!found->second->output)
    return failure(ErrorCode::permission_denied);
  std::vector<std::uint8_t> raw;
  // Validate the whole batch before emitting its first message.
  for (const auto &packet : packets) {
    auto message = protocol::midi1_message(packet);
    if (!message)
      return std::unexpected(message.error());
    for (auto byte : message->bytes)
      raw.push_back(std::to_integer<std::uint8_t>(byte));
  }
  try {
    Apartment apartment;
    if (!apartment.mta())
      return failure(ErrorCode::busy);
    streams::DataWriter writer;
    writer.WriteBytes(raw);
    found->second->output.SendBuffer(writer.DetachBuffer());
    return {};
  } catch (const winrt::hresult_error &error) {
    return hresult_error(error.code());
  }
#endif
}
Result<void> NativeHardware::close(Endpoint ep) {
  if (!impl_->valid(ep))
    return failure(ErrorCode::closed);
  if (impl_->bluetooth) {
    std::lock_guard lock(impl_->bluetooth->mutex);
    for (auto it = impl_->bluetooth->subscriptions.begin();
         it != impl_->bluetooth->subscriptions.end();)
      if (it->first.first == ep.value) {
        it->second.characteristic.ValueChanged(it->second.token);
        it = impl_->bluetooth->subscriptions.erase(it);
      } else
        ++it;
    auto found = impl_->bluetooth->connections.find(ep.value);
    if (found != impl_->bluetooth->connections.end()) {
      found->second.Close();
      impl_->bluetooth->connections.erase(found);
    }
  }
  impl_->files.erase(ep.value);
  impl_->audio.erase(ep.value);
  impl_->midi.erase(ep.value);
  impl_->kinds.erase(ep.value);
  return {};
}
void NativeHardware::collect() {
  for (auto &[ep, file] : impl_->files) {
    if (file->usb || !file->pending || file->failed)
      continue;
    DWORD count = 0;
    if (!GetOverlappedResult(file->file.value, &file->read, &count, false)) {
      if (GetLastError() == ERROR_IO_INCOMPLETE)
        continue;
      file->failed = true;
      HardwareEvent event;
      event.kind = HardwareEventKind::failure;
      event.endpoint = {impl_->state->owner, ep};
      event.error = native_error().error();
      impl_->state->publish(std::move(event));
      continue;
    }
    file->pending = false;
    HardwareEvent event;
    event.endpoint = {impl_->state->owner, ep};
    event.device.kind = DeviceKind::hid;
    if (count > file->input.size()) {
      file->failed = true;
      continue;
    }
    event.bytes.assign(file->input.begin(), file->input.begin() + count);
    if (!event.bytes.empty() && event.bytes[0] == std::byte{})
      event.bytes.erase(event.bytes.begin());
    impl_->state->publish(std::move(event));
    auto armed = file->arm();
    if (!armed)
      file->failed = true;
  }
  for (auto &[ep, audio] : impl_->audio) {
    if (!audio->capture)
      continue;
    UINT32 available = 0;
    if (FAILED(audio->capture->GetNextPacketSize(&available)) || !available)
      continue;
    BYTE *bytes = nullptr;
    UINT32 frames = 0;
    DWORD flags = 0;
    if (FAILED(audio->capture->GetBuffer(&bytes, &frames, &flags, nullptr,
                                         nullptr)))
      continue;
    HardwareEvent event;
    event.endpoint = {impl_->state->owner, ep};
    event.device.kind = DeviceKind::audio_input;
    const auto count = std::size_t(frames) * audio->config.channels;
    if (count <= impl_->state->options.max_payload_bytes / sizeof(float)) {
      event.samples.resize(count);
      if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT))
        memcpy(event.samples.data(), bytes, count * sizeof(float));
    }
    audio->capture->ReleaseBuffer(frames);
    if (!event.samples.empty())
      impl_->state->publish(std::move(event));
  }
  if (impl_->dirty->exchange(false))
    for (auto &[kind, previous] : impl_->snapshots) {
      auto snapshot = enumerate(kind);
      if (!snapshot)
        continue;
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
}
} // namespace detail
} // namespace ii::io
