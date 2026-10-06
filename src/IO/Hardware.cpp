#include "HardwareInternal.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace ii::io {
namespace {
std::atomic<std::uint64_t> next_owner{1};
auto failure(ErrorCode code) { return std::unexpected(Error{code, {}}); }
bool valid_kind(DeviceKind kind) {
  return kind >= DeviceKind::usb && kind <= DeviceKind::midi_output;
}
} // namespace
namespace detail {
HardwareState::HardwareState(HardwareHandler handler, HardwareOptions config)
    : options(config), owner(next_owner.fetch_add(1)),
      events_(config.event_capacity), handler_(std::move(handler)) {}
bool HardwareState::publish(HardwareEvent event) {
  std::lock_guard lock(mutex_);
  if (closed_ ||
      (event.endpoint.value && (event.endpoint.owner != owner ||
                                !active_.contains(event.endpoint.value))))
    return false;
  const auto maximum = options.max_payload_bytes;
  std::size_t remaining = maximum;
  const auto fits = [&](std::size_t size, std::size_t unit = 1) {
    if (size > remaining / unit)
      return false;
    remaining -= size * unit;
    return true;
  };
  if (!fits(event.bytes.size()) || !fits(event.samples.size(), sizeof(float)) ||
      !fits(event.midi.size(), sizeof(protocol::UMP)) ||
      !fits(event.service.size()) || !fits(event.characteristic.size()) ||
      !fits(event.device.id.size()) || !fits(event.device.name.size()) ||
      !fits(event.device.manufacturer.size()) ||
      !fits(event.device.serial.size()) || count_ == events_.size()) {
    ++dropped_;
    return false;
  }
  events_[(begin_ + count_) % events_.size()].emplace(std::move(event));
  ++count_;
  return true;
}
void HardwareState::dispatch() {
  for (std::size_t i = 0; i < options.dispatch_batch; ++i) {
    std::optional<HardwareEvent> event;
    {
      std::lock_guard lock(mutex_);
      if (closed_ || !count_)
        return;
      event.swap(events_[begin_]);
      begin_ = (begin_ + 1) % events_.size();
      --count_;
      if (event->endpoint.value) {
        if (!active_.contains(event->endpoint.value))
          continue;
        ++executing_[event->endpoint.value];
      }
    }
    const auto endpoint = event->endpoint;
    const auto finish = [&] {
      if (!endpoint.value)
        return;
      std::lock_guard lock(mutex_);
      if (--executing_[endpoint.value] == 0) {
        executing_.erase(endpoint.value);
        closing_.erase(endpoint.value);
        changed_.notify_all();
      }
    };
    runtime::SubmitError result;
    try {
      result = callbacks.run([&] { handler_(std::move(*event)); });
    } catch (...) {
      finish();
      throw;
    }
    finish();
    if (result != runtime::SubmitError::none)
      return;
    std::lock_guard lock(mutex_);
    ++delivered_;
  }
}
void HardwareState::retire() {
  std::vector<std::optional<HardwareEvent>> retired;
  HardwareHandler handler;
  {
    std::lock_guard lock(mutex_);
    closed_ = true;
    retired.swap(events_);
    count_ = 0;
    active_.clear();
    closing_.clear();
    handler.swap(handler_);
  }
}
void HardwareState::activate(Endpoint ep) {
  std::lock_guard lock(mutex_);
  if (!closed_ && ep.owner == owner)
    active_.insert(ep.value);
}
bool HardwareState::has_capacity() const {
  std::lock_guard lock(mutex_);
  return !closed_ && active_.size() + closing_.size() < options.event_capacity;
}
void HardwareState::await_endpoint(Endpoint ep) {
  std::unique_lock lock(mutex_);
  changed_.wait(lock, [&] { return !executing_.contains(ep.value); });
}
void HardwareState::deactivate(Endpoint ep) {
  std::vector<std::optional<HardwareEvent>> retired;
  {
    std::lock_guard lock(mutex_);
    active_.erase(ep.value);
    if (executing_.contains(ep.value))
      closing_.insert(ep.value);
    if (closed_)
      return;
    std::vector<std::optional<HardwareEvent>> retained(events_.size());
    std::size_t count = 0;
    for (std::size_t i = 0; i < count_; ++i) {
      auto &event = events_[(begin_ + i) % events_.size()];
      if (event && event->endpoint != ep)
        retained[count++].swap(event);
    }
    retired.swap(events_);
    events_.swap(retained);
    begin_ = 0;
    count_ = count;
  }
}
HardwareStats HardwareState::stats() const {
  std::lock_guard lock(mutex_);
  return {count_, delivered_, dropped_};
}
AudioRing::AudioRing(std::size_t capacity) : data_(capacity) {
  if (!capacity)
    throw std::invalid_argument("audio ring capacity");
}
std::size_t AudioRing::write(std::span<const float> samples) noexcept {
  const auto write = write_.load(std::memory_order_relaxed),
             read = read_.load(std::memory_order_acquire);
  const auto count = std::min(
      samples.size(), data_.size() - static_cast<std::size_t>(write - read));
  for (std::size_t i = 0; i < count; ++i)
    data_[(write + i) % data_.size()] = samples[i];
  write_.store(write + count, std::memory_order_release);
  return count;
}
std::size_t AudioRing::read(std::span<float> samples) noexcept {
  const auto read = read_.load(std::memory_order_relaxed),
             write = write_.load(std::memory_order_acquire);
  const auto count =
      std::min(samples.size(), static_cast<std::size_t>(write - read));
  for (std::size_t i = 0; i < count; ++i)
    samples[i] = data_[(read + i) % data_.size()];
  read_.store(read + count, std::memory_order_release);
  return count;
}
} // namespace detail
struct Hardware::Impl {
  std::mutex control;
  std::shared_ptr<detail::HardwareState> state;
  std::unique_ptr<detail::NativeHardware> native;
  runtime::TaskToken token;
  bool closed = false;
  bool cleaning = false, cleaned = false;
  std::condition_variable changed;
};
Hardware::Hardware(runtime::EventLoop &loop, HardwareHandler handler,
                   HardwareOptions options)
    : impl_(std::make_unique<Impl>()) {
  if (!handler || !options.event_capacity || options.event_capacity > 65536 ||
      !options.max_payload_bytes ||
      options.max_payload_bytes > 16 * 1024 * 1024 || !options.dispatch_batch ||
      options.dispatch_batch > options.event_capacity ||
      options.dispatch_interval <= runtime::Clock::duration::zero())
    throw std::invalid_argument("invalid hardware dispatch options");
  if (options.usb_interface_guids.size() > 64 ||
      std::any_of(options.usb_interface_guids.begin(),
                  options.usb_interface_guids.end(), [](const auto &value) {
                    return value.size() != 36 ||
                           !protocol::BluetoothUUID::parse(value);
                  }))
    throw std::invalid_argument("invalid USB interface GUID");
  impl_->state =
      std::make_shared<detail::HardwareState>(std::move(handler), options);
  impl_->native = std::make_unique<detail::NativeHardware>(impl_->state);
  const std::weak_ptr<detail::HardwareState> weak = impl_->state;
  // Collection touches native state and is serialized with all commands.
  auto *owner = impl_.get();
  const auto submission = loop.every(options.dispatch_interval, [weak, owner] {
    auto state = weak.lock();
    if (!state)
      return;
    const auto result = state->callbacks.run([&] {
      {
        std::lock_guard lock(owner->control);
        if (owner->closed)
          return;
        owner->native->collect();
      }
      state->dispatch();
    });
    (void)result;
  });
  if (!submission)
    throw std::runtime_error("hardware dispatch slot unavailable");
  impl_->token = submission.token;
}
Hardware::~Hardware() {
  if (!close())
    std::terminate();
}
Result<std::vector<DeviceInfo>> Hardware::enumerate(DeviceKind kind) {
  if (!valid_kind(kind))
    return failure(ErrorCode::invalid_argument);
  std::lock_guard lock(impl_->control);
  if (impl_->closed)
    return failure(ErrorCode::closed);
  return impl_->native->enumerate(kind);
}
Result<void> Hardware::watch(DeviceKind kind) {
  if (!valid_kind(kind))
    return failure(ErrorCode::invalid_argument);
  std::lock_guard lock(impl_->control);
  if (impl_->closed)
    return failure(ErrorCode::closed);
  return impl_->native->watch(kind);
}
#define II_HW_CALL(method, ...)                                                \
  std::lock_guard lock(impl_->control);                                        \
  if (impl_->closed)                                                           \
    return failure(ErrorCode::closed);                                         \
  return impl_->native->method(__VA_ARGS__)
#define II_HW_OPEN(method, ...)                                                \
  std::lock_guard lock(impl_->control);                                        \
  if (impl_->closed)                                                           \
    return failure(ErrorCode::closed);                                         \
  if (!impl_->state->has_capacity())                                           \
    return failure(ErrorCode::busy);                                           \
  return impl_->native->method(__VA_ARGS__)
Result<Endpoint> Hardware::open_usb(std::string_view id) {
  II_HW_OPEN(open, DeviceKind::usb, id);
}
Result<Endpoint> Hardware::open_hid(std::string_view id) {
  II_HW_OPEN(open, DeviceKind::hid, id);
}
Result<std::vector<std::byte>>
Hardware::usb_control(Endpoint ep, protocol::USBSetup setup,
                      std::span<const std::byte> output,
                      std::chrono::milliseconds timeout) {
  if (setup.length > impl_->state->options.max_payload_bytes ||
      timeout.count() <= 0 || timeout.count() > 60000 ||
      ((setup.request_type & 0x80) ? !output.empty()
                                   : output.size() != setup.length))
    return failure(ErrorCode::invalid_argument);
  II_HW_CALL(usb_control, ep, setup, output, timeout);
}
Result<std::vector<std::byte>> Hardware::hid_descriptor(Endpoint ep) {
  II_HW_CALL(hid_descriptor, ep);
}
Result<std::vector<std::byte>> Hardware::hid_get(Endpoint ep,
                                                 protocol::HIDReportType type,
                                                 std::uint8_t id,
                                                 std::size_t length) {
  if (type < protocol::HIDReportType::input ||
      type > protocol::HIDReportType::feature || !length ||
      length > impl_->state->options.max_payload_bytes)
    return failure(ErrorCode::invalid_argument);
  II_HW_CALL(hid_get, ep, type, id, length);
}
Result<void> Hardware::hid_set(Endpoint ep, protocol::HIDReportType type,
                               std::uint8_t id,
                               std::span<const std::byte> bytes) {
  if (type < protocol::HIDReportType::input ||
      type > protocol::HIDReportType::feature || bytes.empty() ||
      bytes.size() > impl_->state->options.max_payload_bytes ||
      (id && bytes.front() != std::byte(id)))
    return failure(ErrorCode::invalid_argument);
  II_HW_CALL(hid_set, ep, type, id, bytes);
}
Result<void>
Hardware::scan_bluetooth(std::span<const protocol::BluetoothUUID> services) {
  if (services.empty() || services.size() > 64)
    return failure(ErrorCode::invalid_argument);
  II_HW_CALL(scan, services);
}
Result<void> Hardware::stop_bluetooth_scan() { II_HW_CALL(stop_scan); }
Result<Endpoint> Hardware::connect_bluetooth(std::string_view id) {
  II_HW_OPEN(open, DeviceKind::bluetooth, id);
}
Result<void> Hardware::gatt_read(Endpoint ep, protocol::BluetoothUUID service,
                                 protocol::BluetoothUUID characteristic) {
  II_HW_CALL(gatt, ep, service, characteristic, 0, std::span<const std::byte>{},
             true);
}
Result<void> Hardware::gatt_write(Endpoint ep, protocol::BluetoothUUID service,
                                  protocol::BluetoothUUID characteristic,
                                  std::span<const std::byte> data,
                                  bool response) {
  if (data.size() > impl_->state->options.max_payload_bytes)
    return failure(ErrorCode::invalid_argument);
  II_HW_CALL(gatt, ep, service, characteristic, 1, data, response);
}
Result<void> Hardware::gatt_notify(Endpoint ep, protocol::BluetoothUUID service,
                                   protocol::BluetoothUUID characteristic,
                                   bool enabled) {
  II_HW_CALL(gatt, ep, service, characteristic, 2, std::span<const std::byte>{},
             enabled);
}
Result<Endpoint> Hardware::open_audio(DeviceKind kind, std::string_view id,
                                      AudioConfig config) {
  if ((kind != DeviceKind::audio_input && kind != DeviceKind::audio_output) ||
      !std::isfinite(config.sample_rate) || config.sample_rate < 8000 ||
      config.sample_rate > 384000 || !config.channels || config.channels > 32 ||
      !config.frames_per_buffer || config.frames_per_buffer > 8192 ||
      config.queued_frames < config.frames_per_buffer ||
      config.queued_frames > 1024 * 1024)
    return failure(ErrorCode::invalid_argument);
  II_HW_OPEN(open, kind, id, config);
}
Result<std::size_t> Hardware::audio_write(Endpoint ep,
                                          std::span<const float> samples) {
  if (std::any_of(samples.begin(), samples.end(),
                  [](float value) { return !std::isfinite(value); }))
    return failure(ErrorCode::invalid_argument);
  II_HW_CALL(audio_write, ep, samples);
}
Result<Endpoint> Hardware::open_midi(std::string_view id) {
  II_HW_OPEN(open, DeviceKind::midi_input, id);
}
Result<Endpoint> Hardware::create_midi_source(std::string_view name) {
  if (name.empty() || name.size() > 256)
    return failure(ErrorCode::invalid_argument);
  II_HW_OPEN(virtual_midi, name, true);
}
Result<Endpoint> Hardware::create_midi_destination(std::string_view name) {
  if (name.empty() || name.size() > 256)
    return failure(ErrorCode::invalid_argument);
  II_HW_OPEN(virtual_midi, name, false);
}
Result<void> Hardware::midi_send(Endpoint ep,
                                 std::span<const protocol::UMP> packets) {
  if (packets.empty() ||
      packets.size() >
          impl_->state->options.max_payload_bytes / sizeof(protocol::UMP))
    return failure(ErrorCode::invalid_argument);
  for (const auto &packet : packets) {
    if (packet.size < 1 || packet.size > 4)
      return failure(ErrorCode::invalid_argument);
    const auto parsed =
        protocol::ump_packets(std::span(packet.words).first(packet.size));
    if (!parsed || parsed->size() != 1)
      return failure(ErrorCode::invalid_argument);
  }
  II_HW_CALL(midi_send, ep, packets);
}
Result<void> Hardware::close(Endpoint ep) {
  if (impl_->state->callbacks.is_current())
    return failure(ErrorCode::busy);
  {
    std::lock_guard lock(impl_->control);
    if (impl_->closed)
      return failure(ErrorCode::closed);
    auto result = impl_->native->close(ep);
    if (!result)
      return result;
    impl_->state->deactivate(ep);
  }
  // User completion may issue other hardware commands, so wait without control.
  impl_->state->await_endpoint(ep);
  return {};
}
#undef II_HW_CALL
#undef II_HW_OPEN
Result<void> Hardware::close() {
  if (impl_->state && impl_->state->callbacks.is_current())
    return failure(ErrorCode::busy);
  {
    std::unique_lock lock(impl_->control);
    impl_->changed.wait(lock, [&] { return !impl_->cleaning; });
    if (impl_->cleaned)
      return {};
    impl_->closed = true;
    impl_->cleaning = true;
  }
  (void)impl_->token.cancel();
  if (impl_->state) {
    const auto result = impl_->state->callbacks.close();
    if (!result) {
      std::lock_guard lock(impl_->control);
      impl_->cleaning = false;
      impl_->changed.notify_all();
      return failure(ErrorCode::busy);
    }
    impl_->native.reset();
    impl_->state->retire();
  }
  // The periodic wrapper borrows Impl. Cancellation plus callback quiescence
  // completes before Impl can be destroyed, including its final slot
  // retirement.
  while (impl_->token.pending())
    std::this_thread::yield();
  {
    std::lock_guard lock(impl_->control);
    impl_->cleaned = true;
    impl_->cleaning = false;
    impl_->changed.notify_all();
  }
  return {};
}
HardwareStats Hardware::stats() const {
  return impl_->state ? impl_->state->stats() : HardwareStats{};
}
} // namespace ii::io
