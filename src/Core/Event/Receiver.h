#pragma once
#include "../../Runtime/Query.h"
#include "Event.h"

namespace ii::core {
// Events are typed notification queries, including acceptance in the reply.
// No global bus, subscription registry or second dispatch mechanism.
template <class T> class EventReceiver final {
public:
  using Envelope = Event<T>;
  using Handler = std::function<void(const Envelope &)>;
  using Filter = std::function<bool(const Envelope &)>;
  using Completion = typename runtime::QueryPort<Envelope, bool>::Completion;
  EventReceiver(runtime::SerialQueue &executor, Handler handler,
                Filter filter = {})
      : port_(executor, [handler = checked(std::move(handler)),
                         filter = std::move(filter)](const Envelope &event) {
          if (filter && !filter(event))
            return false;
          handler(event);
          return true;
        }) {}
  [[nodiscard]] runtime::Submission
  post(Envelope event, Completion completion,
       runtime::Priority priority = runtime::Priority::normal) const {
    return port_.post(std::move(event), std::move(completion), priority);
  }
  [[nodiscard]] runtime::ThreadResult close() const { return port_.close(); }
  [[nodiscard]] const runtime::QueryPort<Envelope, bool> &
  queries() const noexcept {
    return port_;
  }

private:
  static Handler checked(Handler handler) {
    if (!handler)
      throw std::invalid_argument("Event receiver requires a handler");
    return handler;
  }
  runtime::QueryPort<Envelope, bool> port_;
};
} // namespace ii::core
