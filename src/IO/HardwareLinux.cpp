#include "HardwareInternal.h"
#include <algorithm>
#include <alsa/asoundlib.h>
#include <charconv>
#include <cstring>
#include <dbus/dbus.h>
#include <fcntl.h>
#include <libudev.h>
#include <linux/hidraw.h>
#include <linux/usbdevice_fs.h>
#include <map>
#include <poll.h>
#include <set>
#include <sys/ioctl.h>
#include <unistd.h>

namespace ii::io {
HardwareCapabilities hardware_capabilities() noexcept {
  return {true, true, true, true, true, true, true};
}
namespace detail {
namespace {
auto failure(ErrorCode code) { return std::unexpected(Error{code, {}}); }
auto native_error(int number = errno) {
  auto code = number == EACCES || number == EPERM ? ErrorCode::permission_denied
              : number == ENOENT                  ? ErrorCode::not_found
              : number == EAGAIN                  ? ErrorCode::would_block
              : number == ENODEV || number == EBADF ? ErrorCode::closed
              : number == ETIMEDOUT                 ? ErrorCode::timed_out
                                                    : ErrorCode::system_error;
  return std::unexpected(Error{code, {number, std::generic_category()}});
}
std::string text(const char *value) { return value ? value : ""; }
std::uint16_t hex(const char *value) {
  if (!value)
    return 0;
  unsigned result = 0;
  std::from_chars(value, value + std::strlen(value), result, 16);
  return result;
}
struct Message final {
  DBusMessage *value = nullptr;
  ~Message() {
    if (value)
      dbus_message_unref(value);
  }
  Message(const Message &) = delete;
  Message &operator=(const Message &) = delete;
  explicit Message(DBusMessage *message) : value(message) {};
};
struct Property final {
  std::string text;
  bool flag = false;
  std::vector<std::byte> bytes;
};
using Properties = std::map<std::string, Property>;
using ObjectTree = std::map<std::string, std::map<std::string, Properties>>;
Property property(DBusMessageIter value) {
  Property result;
  if (dbus_message_iter_get_arg_type(&value) == DBUS_TYPE_VARIANT) {
    DBusMessageIter inner;
    dbus_message_iter_recurse(&value, &inner);
    value = inner;
  }
  const auto type = dbus_message_iter_get_arg_type(&value);
  if (type == DBUS_TYPE_STRING || type == DBUS_TYPE_OBJECT_PATH) {
    const char *str = nullptr;
    dbus_message_iter_get_basic(&value, &str);
    result.text = text(str);
  } else if (type == DBUS_TYPE_BOOLEAN) {
    dbus_bool_t flag = 0;
    dbus_message_iter_get_basic(&value, &flag);
    result.flag = flag;
  } else if (type == DBUS_TYPE_ARRAY &&
             dbus_message_iter_get_element_type(&value) == DBUS_TYPE_BYTE) {
    DBusMessageIter array;
    dbus_message_iter_recurse(&value, &array);
    unsigned char *data = nullptr;
    int length = 0;
    dbus_message_iter_get_fixed_array(&array, &data, &length);
    if (length > 0)
      result.bytes.assign(reinterpret_cast<std::byte *>(data),
                          reinterpret_cast<std::byte *>(data) + length);
  }
  return result;
}
Properties properties(DBusMessageIter value) {
  Properties result;
  DBusMessageIter entries;
  dbus_message_iter_recurse(&value, &entries);
  while (dbus_message_iter_get_arg_type(&entries) == DBUS_TYPE_DICT_ENTRY) {
    DBusMessageIter entry;
    dbus_message_iter_recurse(&entries, &entry);
    const char *key = nullptr;
    dbus_message_iter_get_basic(&entry, &key);
    dbus_message_iter_next(&entry);
    result[text(key)] = property(entry);
    dbus_message_iter_next(&entries);
  }
  return result;
}
ObjectTree tree(DBusMessage *message) {
  ObjectTree result;
  DBusMessageIter root;
  if (!dbus_message_iter_init(message, &root))
    return result;
  DBusMessageIter objects;
  dbus_message_iter_recurse(&root, &objects);
  while (dbus_message_iter_get_arg_type(&objects) == DBUS_TYPE_DICT_ENTRY) {
    DBusMessageIter object;
    dbus_message_iter_recurse(&objects, &object);
    const char *path = nullptr;
    dbus_message_iter_get_basic(&object, &path);
    dbus_message_iter_next(&object);
    DBusMessageIter interfaces;
    dbus_message_iter_recurse(&object, &interfaces);
    while (dbus_message_iter_get_arg_type(&interfaces) ==
           DBUS_TYPE_DICT_ENTRY) {
      DBusMessageIter interface;
      dbus_message_iter_recurse(&interfaces, &interface);
      const char *name = nullptr;
      dbus_message_iter_get_basic(&interface, &name);
      dbus_message_iter_next(&interface);
      result[text(path)][text(name)] = properties(interface);
      dbus_message_iter_next(&interfaces);
    }
    dbus_message_iter_next(&objects);
  }
  return result;
}
struct FileSession final {
  int fd = -1;
  DeviceKind kind;
  bool failed = false;
  ~FileSession() {
    if (fd >= 0)
      ::close(fd);
  }
};
struct PCM final {
  snd_pcm_t *handle = nullptr;
  AudioConfig config;
  DeviceKind kind;
  ~PCM() {
    if (handle) {
      snd_pcm_drop(handle);
      snd_pcm_close(handle);
    }
  }
};
struct MidiPort final {
  int port = -1;
  bool source = false, owned = false;
};
struct Reply final {
  std::weak_ptr<HardwareState> state;
  Endpoint endpoint;
  HardwareEventKind kind;
  std::string service, characteristic;
  bool value = false;
  static void complete(DBusPendingCall *pending, void *context) {
    auto &self = *static_cast<Reply *>(context);
    Message message(dbus_pending_call_steal_reply(pending));
    auto state = self.state.lock();
    if (!state || !message.value)
      return;
    HardwareEvent event;
    event.endpoint = self.endpoint;
    event.kind = self.kind;
    event.device.kind = DeviceKind::bluetooth;
    event.service = self.service;
    event.characteristic = self.characteristic;
    if (dbus_message_get_type(message.value) == DBUS_MESSAGE_TYPE_ERROR) {
      event.kind = HardwareEventKind::failure;
      event.characteristic = text(dbus_message_get_error_name(message.value));
      event.error = Error{ErrorCode::system_error, {}};
    } else if (self.value) {
      DBusMessageIter args;
      if (dbus_message_iter_init(message.value, &args))
        event.bytes = property(args).bytes;
    }
    state->publish(std::move(event));
  }
  static void destroy(void *context) { delete static_cast<Reply *>(context); }
};
} // namespace
struct NativeHardware::Impl {
  std::shared_ptr<HardwareState> state;
  std::uint64_t next = 0;
  udev *context = udev_new();
  udev_monitor *monitor = nullptr;
  DBusConnection *bus = nullptr;
  std::string adapter;
  ObjectTree objects;
  bool dirty = false, scanning = false;
  snd_seq_t *sequencer = nullptr;
  int announce = -1;
  std::map<std::uint64_t, DeviceKind> kinds;
  std::map<std::uint64_t, std::unique_ptr<FileSession>> files;
  std::map<std::uint64_t, std::unique_ptr<PCM>> pcm;
  std::map<std::uint64_t, MidiPort> midi;
  std::map<std::uint64_t, std::string> bluetooth;
  std::vector<std::pair<Endpoint, DBusPendingCall *>> pending_calls;
  std::set<DeviceKind> watched;
  std::map<DeviceKind, std::vector<DeviceInfo>> snapshots;
  explicit Impl(std::shared_ptr<HardwareState> value)
      : state(std::move(value)) {}
  ~Impl() {
    cancel_pending({});
    if (bus) {
      for (const auto &[_, path] : bluetooth) {
        Message request(dbus_message_new_method_call(
            "org.bluez", path.c_str(), "org.bluez.Device1", "Disconnect"));
        if (request.value)
          dbus_connection_send(bus, request.value, nullptr);
      }
      if (scanning) {
        Message request(dbus_message_new_method_call(
            "org.bluez", adapter.c_str(), "org.bluez.Adapter1",
            "StopDiscovery"));
        if (request.value)
          dbus_connection_send(bus, request.value, nullptr);
      }
      dbus_connection_flush(bus);
      dbus_connection_remove_filter(bus, signal, this);
      dbus_connection_close(bus);
      dbus_connection_unref(bus);
    }
    if (sequencer)
      snd_seq_close(sequencer);
    if (monitor)
      udev_monitor_unref(monitor);
    if (context)
      udev_unref(context);
  }
  void cancel_pending(Endpoint ep) {
    std::erase_if(pending_calls, [&](const auto &entry) {
      if (ep.value && entry.first != ep)
        return false;
      dbus_pending_call_cancel(entry.second);
      dbus_pending_call_unref(entry.second);
      return true;
    });
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
  static DBusHandlerResult signal(DBusConnection *, DBusMessage *message,
                                  void *context) {
    auto &self = *static_cast<Impl *>(context);
    if (dbus_message_get_type(message) != DBUS_MESSAGE_TYPE_SIGNAL)
      return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    if (dbus_message_is_signal(message, "org.freedesktop.DBus.ObjectManager",
                               "InterfacesAdded") ||
        dbus_message_is_signal(message, "org.freedesktop.DBus.ObjectManager",
                               "InterfacesRemoved") ||
        dbus_message_is_signal(message, "org.freedesktop.DBus.Properties",
                               "PropertiesChanged"))
      self.dirty = true;
    if (dbus_message_is_signal(message, "org.freedesktop.DBus.Properties",
                               "PropertiesChanged")) {
      DBusMessageIter args;
      if (!dbus_message_iter_init(message, &args))
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
      const char *interface = nullptr;
      dbus_message_iter_get_basic(&args, &interface);
      dbus_message_iter_next(&args);
      if (text(interface) != "org.bluez.GattCharacteristic1")
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
      auto changed = properties(args);
      auto value = changed.find("Value");
      if (value == changed.end())
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
      const auto path = text(dbus_message_get_path(message));
      for (const auto &[ep, device] : self.bluetooth)
        if (path.starts_with(device + "/")) {
          HardwareEvent event;
          event.endpoint = {self.state->owner, ep};
          event.device.kind = DeviceKind::bluetooth;
          event.bytes = std::move(value->second.bytes);
          event.characteristic =
              self.objects[path]["org.bluez.GattCharacteristic1"]["UUID"].text;
          const auto service =
              self.objects[path]["org.bluez.GattCharacteristic1"]["Service"]
                  .text;
          event.service =
              self.objects[service]["org.bluez.GattService1"]["UUID"].text;
          self.state->publish(std::move(event));
          break;
        }
    }
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
  }
  Result<void> connect_bus() {
    if (bus)
      return {};
    DBusError error;
    dbus_error_init(&error);
    bus = dbus_bus_get_private(DBUS_BUS_SYSTEM, &error);
    if (!bus) {
      dbus_error_free(&error);
      return failure(ErrorCode::not_found);
    }
    dbus_connection_set_exit_on_disconnect(bus, false);
    dbus_bus_add_match(bus, "type='signal',sender='org.bluez'", &error);
    if (dbus_error_is_set(&error)) {
      dbus_error_free(&error);
      dbus_connection_close(bus);
      dbus_connection_unref(bus);
      bus = nullptr;
      return failure(ErrorCode::permission_denied);
    }
    if (!dbus_connection_add_filter(bus, signal, this, nullptr)) {
      dbus_connection_close(bus);
      dbus_connection_unref(bus);
      bus = nullptr;
      return failure(ErrorCode::system_error);
    }
    return refresh();
  }
  Result<void> refresh() {
    Message request(dbus_message_new_method_call(
        "org.bluez", "/", "org.freedesktop.DBus.ObjectManager",
        "GetManagedObjects"));
    DBusError error;
    dbus_error_init(&error);
    Message reply(dbus_connection_send_with_reply_and_block(bus, request.value,
                                                            1000, &error));
    if (!reply.value) {
      dbus_error_free(&error);
      return failure(ErrorCode::system_error);
    }
    objects = tree(reply.value);
    adapter.clear();
    for (const auto &[path, interfaces] : objects)
      if (interfaces.contains("org.bluez.Adapter1")) {
        adapter = path;
        break;
      }
    return {};
  }
  Result<void> asynchronous(Message &message, Endpoint endpoint,
                            HardwareEventKind kind, bool value = false,
                            std::string service = {},
                            std::string characteristic = {}) {
    if (pending_calls.size() >= state->options.event_capacity)
      return failure(ErrorCode::busy);
    DBusPendingCall *pending = nullptr;
    if (!dbus_connection_send_with_reply(bus, message.value, &pending, 10000) ||
        !pending)
      return failure(ErrorCode::system_error);
    auto context =
        std::make_unique<Reply>(Reply{state, endpoint, kind, std::move(service),
                                      std::move(characteristic), value});
    if (!dbus_pending_call_set_notify(pending, Reply::complete, context.get(),
                                      Reply::destroy)) {
      dbus_pending_call_cancel(pending);
      dbus_pending_call_unref(pending);
      return failure(ErrorCode::system_error);
    }
    context.release();
    pending_calls.emplace_back(endpoint, pending);
    return {};
  }
  Result<void> seq() {
    if (sequencer)
      return {};
    int error = snd_seq_open(&sequencer, "default", SND_SEQ_OPEN_DUPLEX,
                             SND_SEQ_NONBLOCK);
    if (error < 0)
      return native_error(-error);
    snd_seq_set_client_name(sequencer, "iiSDKCore");
    error =
        snd_seq_set_client_midi_version(sequencer, SND_SEQ_CLIENT_UMP_MIDI_2_0);
    if (error < 0) {
      snd_seq_close(sequencer);
      sequencer = nullptr;
      return failure(ErrorCode::unsupported);
    }
    return {};
  }
};
NativeHardware::NativeHardware(std::shared_ptr<HardwareState> state)
    : impl_(std::make_unique<Impl>(std::move(state))) {}
NativeHardware::~NativeHardware() = default;
Result<std::vector<DeviceInfo>> NativeHardware::enumerate(DeviceKind kind) {
  std::vector<DeviceInfo> result;
  if (kind == DeviceKind::usb || kind == DeviceKind::hid) {
    if (!impl_->context)
      return failure(ErrorCode::system_error);
    auto enumeration = udev_enumerate_new(impl_->context);
    if (!enumeration)
      return failure(ErrorCode::system_error);
    udev_enumerate_add_match_subsystem(
        enumeration, kind == DeviceKind::usb ? "usb" : "hidraw");
    udev_enumerate_scan_devices(enumeration);
    for (auto *entry = udev_enumerate_get_list_entry(enumeration); entry;
         entry = udev_list_entry_get_next(entry)) {
      auto device = udev_device_new_from_syspath(
          impl_->context, udev_list_entry_get_name(entry));
      if (!device)
        continue;
      const auto node = udev_device_get_devnode(device);
      if (!node || (kind == DeviceKind::usb &&
                    text(udev_device_get_devtype(device)) != "usb_device")) {
        udev_device_unref(device);
        continue;
      }
      DeviceInfo info;
      info.kind = kind;
      info.id = text(udev_device_get_syspath(device));
      auto usb = kind == DeviceKind::usb
                     ? device
                     : udev_device_get_parent_with_subsystem_devtype(
                           device, "usb", "usb_device");
      if (usb) {
        info.vendor = hex(udev_device_get_sysattr_value(usb, "idVendor"));
        info.product = hex(udev_device_get_sysattr_value(usb, "idProduct"));
        info.name = text(udev_device_get_sysattr_value(usb, "product"));
        info.manufacturer =
            text(udev_device_get_sysattr_value(usb, "manufacturer"));
        info.serial = text(udev_device_get_sysattr_value(usb, "serial"));
      }
      result.push_back(std::move(info));
      udev_device_unref(device);
    }
    udev_enumerate_unref(enumeration);
    return result;
  }
  if (kind == DeviceKind::bluetooth) {
    auto connected = impl_->connect_bus();
    if (!connected)
      return std::unexpected(connected.error());
    auto refreshed = impl_->refresh();
    if (!refreshed)
      return std::unexpected(refreshed.error());
    for (auto &[path, interfaces] : impl_->objects) {
      auto device = interfaces.find("org.bluez.Device1");
      if (device == interfaces.end())
        continue;
      DeviceInfo info;
      info.kind = kind;
      info.id = path;
      info.name = device->second["Name"].text;
      result.push_back(std::move(info));
    }
    return result;
  }
  if (kind == DeviceKind::audio_input || kind == DeviceKind::audio_output) {
    void **hints = nullptr;
    const auto status = snd_device_name_hint(-1, "pcm", &hints);
    if (status < 0)
      return native_error(-status);
    for (auto hint = hints; hint && *hint; ++hint) {
      char *name = snd_device_name_get_hint(*hint, "NAME"),
           *description = snd_device_name_get_hint(*hint, "DESC"),
           *direction = snd_device_name_get_hint(*hint, "IOID");
      if (name &&
          (!direction ||
           text(direction) ==
               (kind == DeviceKind::audio_input ? "Input" : "Output"))) {
        DeviceInfo info;
        info.kind = kind;
        info.id = name;
        info.name = text(description);
        result.push_back(std::move(info));
      }
      free(name);
      free(description);
      free(direction);
    }
    snd_device_name_free_hint(hints);
    return result;
  }
  auto ready = impl_->seq();
  if (!ready)
    return std::unexpected(ready.error());
  snd_seq_client_info_t *client;
  snd_seq_client_info_alloca(&client);
  snd_seq_port_info_t *port;
  snd_seq_port_info_alloca(&port);
  snd_seq_client_info_set_client(client, -1);
  while (snd_seq_query_next_client(impl_->sequencer, client) >= 0) {
    int number = snd_seq_client_info_get_client(client);
    if (number == SND_SEQ_CLIENT_SYSTEM)
      continue;
    snd_seq_port_info_set_client(port, number);
    snd_seq_port_info_set_port(port, -1);
    while (snd_seq_query_next_port(impl_->sequencer, port) >= 0) {
      const auto capability = snd_seq_port_info_get_capability(port);
      const auto required =
          kind == DeviceKind::midi_input
              ? SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ
              : SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE;
      if ((capability & required) != unsigned(required) ||
          (capability & SND_SEQ_PORT_CAP_NO_EXPORT))
        continue;
      DeviceInfo info;
      info.kind = kind;
      info.id = std::string(kind == DeviceKind::midi_input ? "midi:in:"
                                                           : "midi:out:") +
                std::to_string(number) + ":" +
                std::to_string(snd_seq_port_info_get_port(port));
      info.name = text(snd_seq_client_info_get_name(client)) + " / " +
                  text(snd_seq_port_info_get_name(port));
      result.push_back(std::move(info));
    }
  }
  return result;
}
Result<void> NativeHardware::watch(DeviceKind kind) {
  if (impl_->watched.contains(kind))
    return {};
  if (!impl_->monitor) {
    impl_->monitor = udev_monitor_new_from_netlink(impl_->context, "udev");
    if (!impl_->monitor)
      return failure(ErrorCode::system_error);
    for (const char *subsystem : {"usb", "hidraw", "sound"})
      udev_monitor_filter_add_match_subsystem_devtype(impl_->monitor, subsystem,
                                                      nullptr);
    if (udev_monitor_enable_receiving(impl_->monitor) < 0)
      return native_error();
  }
  if (kind == DeviceKind::midi_input || kind == DeviceKind::midi_output) {
    auto ready = impl_->seq();
    if (!ready)
      return ready;
    if (impl_->announce < 0) {
      impl_->announce = snd_seq_create_simple_port(
          impl_->sequencer, "Topology",
          SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE |
              SND_SEQ_PORT_CAP_NO_EXPORT,
          SND_SEQ_PORT_TYPE_APPLICATION);
      if (impl_->announce < 0)
        return native_error(-impl_->announce);
      const auto result = snd_seq_connect_from(
          impl_->sequencer, impl_->announce, SND_SEQ_CLIENT_SYSTEM,
          SND_SEQ_PORT_SYSTEM_ANNOUNCE);
      if (result < 0)
        return native_error(-result);
    }
  }
  if (kind == DeviceKind::bluetooth)
    return failure(ErrorCode::invalid_argument);
  impl_->watched.insert(kind);
  impl_->dirty = true;
  return {};
}
Result<Endpoint> NativeHardware::open(DeviceKind kind, std::string_view id,
                                      AudioConfig config) {
  const std::string identifier(id);
  if (kind == DeviceKind::usb || kind == DeviceKind::hid) {
    auto device =
        udev_device_new_from_syspath(impl_->context, identifier.c_str());
    if (!device)
      return failure(ErrorCode::not_found);
    const auto subsystem = text(udev_device_get_subsystem(device));
    const auto node = text(udev_device_get_devnode(device));
    if (node.empty() ||
        subsystem != (kind == DeviceKind::usb ? "usb" : "hidraw")) {
      udev_device_unref(device);
      return failure(ErrorCode::invalid_argument);
    }
    auto session = std::make_unique<FileSession>();
    session->fd = ::open(node.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    session->kind = kind;
    udev_device_unref(device);
    if (session->fd < 0)
      return native_error();
    const auto ep = impl_->endpoint(kind);
    impl_->files[ep.value] = std::move(session);
    return ep;
  }
  if (kind == DeviceKind::bluetooth) {
    auto connected = impl_->connect_bus();
    if (!connected)
      return std::unexpected(connected.error());
    if (std::any_of(
            impl_->bluetooth.begin(), impl_->bluetooth.end(),
            [&](const auto &entry) { return entry.second == identifier; }))
      return failure(ErrorCode::busy);
    if (!impl_->objects.contains(identifier) ||
        !impl_->objects[identifier].contains("org.bluez.Device1"))
      return failure(ErrorCode::not_found);
    const auto ep = impl_->endpoint(kind);
    Message request(dbus_message_new_method_call(
        "org.bluez", identifier.c_str(), "org.bluez.Device1", "Connect"));
    auto sent = impl_->asynchronous(request, ep, HardwareEventKind::connected);
    if (!sent) {
      impl_->kinds.erase(ep.value);
      impl_->state->deactivate(ep);
      return std::unexpected(sent.error());
    }
    impl_->bluetooth[ep.value] = identifier;
    return ep;
  }
  if (kind == DeviceKind::audio_input || kind == DeviceKind::audio_output) {
    auto session = std::make_unique<PCM>();
    session->kind = kind;
    session->config = config;
    int status = snd_pcm_open(
        &session->handle, id.empty() ? "default" : identifier.c_str(),
        kind == DeviceKind::audio_input ? SND_PCM_STREAM_CAPTURE
                                        : SND_PCM_STREAM_PLAYBACK,
        SND_PCM_NONBLOCK);
    if (status < 0)
      return native_error(-status);
    status = snd_pcm_set_params(
        session->handle, SND_PCM_FORMAT_FLOAT, SND_PCM_ACCESS_RW_INTERLEAVED,
        config.channels, static_cast<unsigned>(config.sample_rate), 1,
        static_cast<unsigned>(1000000.0 * config.queued_frames /
                              config.sample_rate));
    if (status < 0)
      return native_error(-status);
    if (kind == DeviceKind::audio_input) {
      status = snd_pcm_start(session->handle);
      if (status < 0)
        return native_error(-status);
    }
    const auto ep = impl_->endpoint(kind);
    impl_->pcm[ep.value] = std::move(session);
    return ep;
  }
  if (kind == DeviceKind::midi_input) {
    bool source = id.starts_with("midi:in:");
    if (!source && !id.starts_with("midi:out:"))
      return failure(ErrorCode::invalid_argument);
    id.remove_prefix(source ? 8 : 9);
    const auto separator = id.find(':');
    if (separator == std::string_view::npos)
      return failure(ErrorCode::invalid_argument);
    int client = -1, port = -1;
    auto a = std::from_chars(id.data(), id.data() + separator, client),
         b = std::from_chars(id.data() + separator + 1, id.data() + id.size(),
                             port);
    if (a.ec != std::errc{} || a.ptr != id.data() + separator ||
        b.ec != std::errc{} || b.ptr != id.data() + id.size() || client < 0 ||
        client > 255 || port < 0 || port > 255)
      return failure(ErrorCode::invalid_argument);
    auto ready = impl_->seq();
    if (!ready)
      return std::unexpected(ready.error());
    int local = snd_seq_create_simple_port(
        impl_->sequencer, "I/O",
        (source ? SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE
                : SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ) |
            SND_SEQ_PORT_CAP_NO_EXPORT,
        SND_SEQ_PORT_TYPE_MIDI_UMP | SND_SEQ_PORT_TYPE_APPLICATION);
    if (local < 0)
      return native_error(-local);
    int status =
        source ? snd_seq_connect_from(impl_->sequencer, local, client, port)
               : snd_seq_connect_to(impl_->sequencer, local, client, port);
    if (status < 0) {
      snd_seq_delete_simple_port(impl_->sequencer, local);
      return native_error(-status);
    }
    auto ep = impl_->endpoint(source ? DeviceKind::midi_input
                                     : DeviceKind::midi_output);
    impl_->midi[ep.value] = {local, source, false};
    return ep;
  }
  return failure(ErrorCode::unsupported);
}
Result<std::vector<std::byte>>
NativeHardware::usb_control(Endpoint ep, protocol::USBSetup setup,
                            std::span<const std::byte> output,
                            std::chrono::milliseconds timeout) {
  if (!impl_->valid(ep))
    return failure(ErrorCode::closed);
  auto found = impl_->files.find(ep.value);
  if (found == impl_->files.end() || found->second->kind != DeviceKind::usb)
    return failure(ErrorCode::invalid_argument);
  std::vector<std::byte> data(setup.length);
  std::copy(output.begin(), output.end(), data.begin());
  usbdevfs_ctrltransfer transfer{
      setup.request_type, setup.request, setup.value,
      setup.index,        setup.length,  static_cast<unsigned>(timeout.count()),
      data.data()};
  const auto count = ioctl(found->second->fd, USBDEVFS_CONTROL, &transfer);
  if (count < 0)
    return native_error();
  if (static_cast<unsigned>(count) > setup.length)
    return failure(ErrorCode::system_error);
  if (!(setup.request_type & 0x80))
    return std::vector<std::byte>{};
  data.resize(count);
  return data;
}
Result<std::vector<std::byte>> NativeHardware::hid_descriptor(Endpoint ep) {
  if (!impl_->valid(ep))
    return failure(ErrorCode::closed);
  auto found = impl_->files.find(ep.value);
  if (found == impl_->files.end() || found->second->kind != DeviceKind::hid)
    return failure(ErrorCode::invalid_argument);
  int size = 0;
  if (ioctl(found->second->fd, HIDIOCGRDESCSIZE, &size) < 0)
    return native_error();
  if (size <= 0 || size > HID_MAX_DESCRIPTOR_SIZE)
    return failure(ErrorCode::system_error);
  hidraw_report_descriptor descriptor{};
  descriptor.size = size;
  if (ioctl(found->second->fd, HIDIOCGRDESC, &descriptor) < 0)
    return native_error();
  if (descriptor.size > HID_MAX_DESCRIPTOR_SIZE)
    return failure(ErrorCode::system_error);
  return std::vector<std::byte>(
      reinterpret_cast<std::byte *>(descriptor.value),
      reinterpret_cast<std::byte *>(descriptor.value) + descriptor.size);
}
Result<std::vector<std::byte>>
NativeHardware::hid_get(Endpoint ep, protocol::HIDReportType type,
                        std::uint8_t id, std::size_t length) {
  if (!impl_->valid(ep))
    return failure(ErrorCode::closed);
  auto found = impl_->files.find(ep.value);
  if (found == impl_->files.end() || found->second->kind != DeviceKind::hid)
    return failure(ErrorCode::invalid_argument);
  if (length + (id ? 0 : 1) > 16383)
    return failure(ErrorCode::invalid_argument);
  std::vector<std::byte> bytes(length + (id ? 0 : 1));
  bytes[0] = std::byte(id);
  auto request =
      type == protocol::HIDReportType::feature ? HIDIOCGFEATURE(bytes.size())
      : type == protocol::HIDReportType::input ? HIDIOCGINPUT(bytes.size())
                                               : HIDIOCGOUTPUT(bytes.size());
  int count = ioctl(found->second->fd, request, bytes.data());
  if (count < 0)
    return native_error();
  if (count > int(bytes.size()))
    return failure(ErrorCode::system_error);
  bytes.resize(count);
  if (!id && !bytes.empty())
    bytes.erase(bytes.begin());
  return bytes;
}
Result<void> NativeHardware::hid_set(Endpoint ep, protocol::HIDReportType type,
                                     std::uint8_t id,
                                     std::span<const std::byte> input) {
  if (!impl_->valid(ep))
    return failure(ErrorCode::closed);
  auto found = impl_->files.find(ep.value);
  if (found == impl_->files.end() || found->second->kind != DeviceKind::hid)
    return failure(ErrorCode::invalid_argument);
  if (input.size() + (id ? 0 : 1) > 16383)
    return failure(ErrorCode::invalid_argument);
  std::vector<std::byte> bytes(input.begin(), input.end());
  if (!id)
    bytes.insert(bytes.begin(), std::byte{});
  else if (bytes.empty() || bytes[0] != std::byte(id))
    return failure(ErrorCode::invalid_argument);
  const auto request =
      type == protocol::HIDReportType::feature ? HIDIOCSFEATURE(bytes.size())
      : type == protocol::HIDReportType::input ? HIDIOCSINPUT(bytes.size())
                                               : HIDIOCSOUTPUT(bytes.size());
  if (ioctl(found->second->fd, request, bytes.data()) < 0)
    return native_error();
  return {};
}
Result<void>
NativeHardware::scan(std::span<const protocol::BluetoothUUID> services) {
  auto connected = impl_->connect_bus();
  if (!connected)
    return connected;
  if (impl_->adapter.empty())
    return failure(ErrorCode::not_found);
  Message filter(
      dbus_message_new_method_call("org.bluez", impl_->adapter.c_str(),
                                   "org.bluez.Adapter1", "SetDiscoveryFilter"));
  DBusMessageIter args, array, entry, variant, strings;
  dbus_message_iter_init_append(filter.value, &args);
  dbus_message_iter_open_container(&args, DBUS_TYPE_ARRAY, "{sv}", &array);
  dbus_message_iter_open_container(&array, DBUS_TYPE_DICT_ENTRY, nullptr,
                                   &entry);
  const char *key = "UUIDs";
  dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
  dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "as", &variant);
  dbus_message_iter_open_container(&variant, DBUS_TYPE_ARRAY, "s", &strings);
  for (const auto &uuid : services) {
    auto value = uuid.string();
    const auto *data = value.c_str();
    dbus_message_iter_append_basic(&strings, DBUS_TYPE_STRING, &data);
  }
  dbus_message_iter_close_container(&variant, &strings);
  dbus_message_iter_close_container(&entry, &variant);
  dbus_message_iter_close_container(&array, &entry);
  dbus_message_iter_close_container(&args, &array);
  DBusError error;
  dbus_error_init(&error);
  Message filtered(dbus_connection_send_with_reply_and_block(
      impl_->bus, filter.value, 1000, &error));
  if (!filtered.value) {
    dbus_error_free(&error);
    return failure(ErrorCode::system_error);
  }
  Message request(
      dbus_message_new_method_call("org.bluez", impl_->adapter.c_str(),
                                   "org.bluez.Adapter1", "StartDiscovery"));
  auto sent = impl_->asynchronous(request, {}, HardwareEventKind::connected);
  if (sent) {
    impl_->scanning = true;
    impl_->watched.insert(DeviceKind::bluetooth);
    impl_->dirty = true;
  }
  return sent;
}
Result<void> NativeHardware::stop_scan() {
  if (!impl_->scanning)
    return {};
  Message request(
      dbus_message_new_method_call("org.bluez", impl_->adapter.c_str(),
                                   "org.bluez.Adapter1", "StopDiscovery"));
  impl_->scanning = false;
  return impl_->asynchronous(request, {}, HardwareEventKind::disconnected);
}
Result<void> NativeHardware::gatt(Endpoint ep, protocol::BluetoothUUID service,
                                  protocol::BluetoothUUID characteristic,
                                  int operation,
                                  std::span<const std::byte> data, bool flag) {
  if (!impl_->valid(ep))
    return failure(ErrorCode::closed);
  auto device = impl_->bluetooth.find(ep.value);
  if (device == impl_->bluetooth.end())
    return failure(ErrorCode::invalid_argument);
  auto refreshed = impl_->refresh();
  if (!refreshed)
    return refreshed;
  std::string service_path, characteristic_path;
  for (auto &[path, interfaces] : impl_->objects) {
    auto found = interfaces.find("org.bluez.GattService1");
    if (found != interfaces.end() &&
        found->second["Device"].text == device->second &&
        found->second["UUID"].text == service.string()) {
      service_path = path;
      break;
    }
  }
  for (auto &[path, interfaces] : impl_->objects) {
    auto found = interfaces.find("org.bluez.GattCharacteristic1");
    if (found != interfaces.end() &&
        found->second["Service"].text == service_path &&
        found->second["UUID"].text == characteristic.string()) {
      characteristic_path = path;
      break;
    }
  }
  if (characteristic_path.empty())
    return failure(ErrorCode::not_found);
  const char *method = operation == 0   ? "ReadValue"
                       : operation == 1 ? "WriteValue"
                       : flag           ? "StartNotify"
                                        : "StopNotify";
  Message request(
      dbus_message_new_method_call("org.bluez", characteristic_path.c_str(),
                                   "org.bluez.GattCharacteristic1", method));
  DBusMessageIter args;
  dbus_message_iter_init_append(request.value, &args);
  if (operation == 1) {
    DBusMessageIter bytes;
    dbus_message_iter_open_container(&args, DBUS_TYPE_ARRAY, "y", &bytes);
    const void *pointer = data.data();
    dbus_message_iter_append_fixed_array(&bytes, DBUS_TYPE_BYTE, &pointer,
                                         data.size());
    dbus_message_iter_close_container(&args, &bytes);
  }
  if (operation < 2) {
    DBusMessageIter options;
    dbus_message_iter_open_container(&args, DBUS_TYPE_ARRAY, "{sv}", &options);
    if (operation == 1) {
      DBusMessageIter entry, value;
      dbus_message_iter_open_container(&options, DBUS_TYPE_DICT_ENTRY, nullptr,
                                       &entry);
      const char *key = "type", *type = flag ? "request" : "command";
      dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
      dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "s", &value);
      dbus_message_iter_append_basic(&value, DBUS_TYPE_STRING, &type);
      dbus_message_iter_close_container(&entry, &value);
      dbus_message_iter_close_container(&options, &entry);
    }
    dbus_message_iter_close_container(&args, &options);
  }
  return impl_->asynchronous(request, ep,
                             operation == 0 ? HardwareEventKind::input
                                            : HardwareEventKind::characteristic,
                             operation == 0, service.string(),
                             characteristic.string());
}
Result<std::size_t> NativeHardware::audio_write(Endpoint ep,
                                                std::span<const float> data) {
  if (!impl_->valid(ep))
    return failure(ErrorCode::closed);
  auto found = impl_->pcm.find(ep.value);
  if (found == impl_->pcm.end() ||
      found->second->kind != DeviceKind::audio_output ||
      data.size() % found->second->config.channels)
    return failure(ErrorCode::invalid_argument);
  const auto count =
      snd_pcm_writei(found->second->handle, data.data(),
                     data.size() / found->second->config.channels);
  if (count < 0) {
    if (count == -EPIPE || count == -ESTRPIPE)
      snd_pcm_recover(found->second->handle, count, 1);
    return native_error(-count);
  }
  return std::size_t(count) * found->second->config.channels;
}
Result<Endpoint> NativeHardware::virtual_midi(std::string_view name,
                                              bool source) {
  auto ready = impl_->seq();
  if (!ready)
    return std::unexpected(ready.error());
  const std::string label(name);
  const int port = snd_seq_create_simple_port(
      impl_->sequencer, label.c_str(),
      source ? SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ
             : SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
      SND_SEQ_PORT_TYPE_MIDI_UMP | SND_SEQ_PORT_TYPE_APPLICATION);
  if (port < 0)
    return native_error(-port);
  const auto ep = impl_->endpoint(source ? DeviceKind::midi_output
                                         : DeviceKind::midi_input);
  impl_->midi[ep.value] = {port, !source, true};
  return ep;
}
Result<void> NativeHardware::midi_send(Endpoint ep,
                                       std::span<const protocol::UMP> packets) {
  if (!impl_->valid(ep))
    return failure(ErrorCode::closed);
  auto found = impl_->midi.find(ep.value);
  if (found == impl_->midi.end() || found->second.source)
    return failure(ErrorCode::permission_denied);
  for (const auto &packet : packets) {
    snd_seq_ump_event_t event{};
    event.flags = SND_SEQ_EVENT_UMP;
    event.queue = SND_SEQ_QUEUE_DIRECT;
    event.source.port = found->second.port;
    event.dest.client = SND_SEQ_ADDRESS_SUBSCRIBERS;
    event.dest.port = SND_SEQ_ADDRESS_UNKNOWN;
    std::copy(packet.words.begin(), packet.words.end(), event.ump);
    const auto status =
        snd_seq_ump_event_output_direct(impl_->sequencer, &event);
    if (status < 0)
      return native_error(-status);
  }
  return {};
}
Result<void> NativeHardware::close(Endpoint ep) {
  if (!impl_->valid(ep))
    return failure(ErrorCode::closed);
  impl_->cancel_pending(ep);
  auto bt = impl_->bluetooth.find(ep.value);
  if (bt != impl_->bluetooth.end()) {
    Message request(dbus_message_new_method_call(
        "org.bluez", bt->second.c_str(), "org.bluez.Device1", "Disconnect"));
    dbus_connection_send(impl_->bus, request.value, nullptr);
    dbus_connection_flush(impl_->bus);
    impl_->bluetooth.erase(bt);
  }
  auto midi = impl_->midi.find(ep.value);
  if (midi != impl_->midi.end()) {
    snd_seq_delete_simple_port(impl_->sequencer, midi->second.port);
    impl_->midi.erase(midi);
  }
  impl_->files.erase(ep.value);
  impl_->pcm.erase(ep.value);
  impl_->kinds.erase(ep.value);
  return {};
}
void NativeHardware::collect() {
  if (impl_->monitor)
    for (std::size_t i = 0; i < 32; ++i) {
      auto device = udev_monitor_receive_device(impl_->monitor);
      if (!device)
        break;
      impl_->dirty = true;
      udev_device_unref(device);
    }
  if (impl_->bus) {
    dbus_connection_read_write(impl_->bus, 0);
    for (std::size_t i = 0;
         i < 32 && dbus_connection_get_dispatch_status(impl_->bus) ==
                       DBUS_DISPATCH_DATA_REMAINS;
         ++i)
      dbus_connection_dispatch(impl_->bus);
    std::erase_if(impl_->pending_calls, [](const auto &entry) {
      if (!dbus_pending_call_get_completed(entry.second))
        return false;
      dbus_pending_call_unref(entry.second);
      return true;
    });
  }
  if (impl_->sequencer)
    for (std::size_t i = 0; i < 32; ++i) {
      snd_seq_ump_event_t *event = nullptr;
      const auto status = snd_seq_ump_event_input(impl_->sequencer, &event);
      if (status < 0 || !event)
        break;
      if (!(event->flags & SND_SEQ_EVENT_UMP)) {
        impl_->dirty = true;
        continue;
      }
      for (const auto &[ep, port] : impl_->midi)
        if (port.source && event->dest.port == port.port) {
          HardwareEvent input;
          input.endpoint = {impl_->state->owner, ep};
          input.device.kind = DeviceKind::midi_input;
          const unsigned sizes[]{1, 1, 1, 2, 2, 4, 1, 1,
                                 2, 2, 2, 3, 3, 4, 4, 4};
          auto parsed =
              protocol::ump_packets({event->ump, sizes[event->ump[0] >> 28]});
          if (parsed) {
            input.midi = std::move(*parsed);
            impl_->state->publish(std::move(input));
          }
          break;
        }
    }
  for (const auto &[ep, session] : impl_->files)
    if (session->kind == DeviceKind::hid && !session->failed) {
      HardwareEvent event;
      event.endpoint = {impl_->state->owner, ep};
      event.device.kind = DeviceKind::hid;
      event.bytes.resize(impl_->state->options.max_payload_bytes);
      const auto count =
          ::read(session->fd, event.bytes.data(), event.bytes.size());
      if (count > 0) {
        event.bytes.resize(count);
        impl_->state->publish(std::move(event));
      } else if (count < 0 && errno != EAGAIN && errno != EINTR) {
        session->failed = true;
        event.bytes.clear();
        event.kind = HardwareEventKind::failure;
        event.error = native_error().error();
        impl_->state->publish(std::move(event));
      }
    }
  for (auto &[ep, session] : impl_->pcm)
    if (session->kind == DeviceKind::audio_input) {
      HardwareEvent event;
      event.endpoint = {impl_->state->owner, ep};
      event.device.kind = DeviceKind::audio_input;
      const auto frames =
          std::min<std::size_t>(session->config.frames_per_buffer,
                                impl_->state->options.max_payload_bytes /
                                    sizeof(float) / session->config.channels);
      event.samples.resize(frames * session->config.channels);
      const auto count =
          snd_pcm_readi(session->handle, event.samples.data(), frames);
      if (count > 0) {
        event.samples.resize(count * session->config.channels);
        impl_->state->publish(std::move(event));
      } else if (count < 0 && count != -EAGAIN) {
        snd_pcm_recover(session->handle, count, 1);
        event.samples.clear();
        event.kind = HardwareEventKind::failure;
        event.error = native_error(-count).error();
        impl_->state->publish(std::move(event));
      }
    }
  if (impl_->dirty) {
    impl_->dirty = false;
    for (auto kind : impl_->watched) {
      auto snapshot = enumerate(kind);
      if (!snapshot)
        continue;
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
  }
}
} // namespace detail
} // namespace ii::io
