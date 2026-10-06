#include <iiSDKCore/Core/Serialization/Serialization.h>
#include <iiSDKCore/Runtime/Query.h>
#include <iostream>
#include <map>
#include <string>

int main() {
  namespace wire = ii::serialization;
  using Payload = std::map<std::string, std::vector<double>>;
  const Payload input{{"samples", {0.25, 0.5, 0.75}}};
  auto encode = [](const Payload &value) { return wire::serialize(value); };
  wire::Result<wire::Bytes> result = std::unexpected(
      wire::Error{wire::ErrorCode::codec_failed, 0, "No result", {}});
  if (ii::runtime::Thread::supported()) {
    ii::runtime::Runtime runtime;
    ii::runtime::SerialQueue queue(runtime);
    ii::runtime::QueryPort<Payload, wire::Result<wire::Bytes>> port(queue,
                                                                    encode);
    if (!port.post(input, [&](auto reply) {
          if (reply)
            result = std::move(*reply);
        }))
      return 1;
    if (!runtime.wait_idle() || !port.close() || !queue.close() ||
        !runtime.join())
      return 2;
  } else {
    result = encode(input);
  }
  if (!result || wire::deserialize<Payload>(*result) != input)
    return 3;
  std::cout << "serialization query complete: bytes=" << result->size()
            << " samples=" << input.at("samples").size() << '\n';
}
