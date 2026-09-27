#include "objects/Device.h"
#include <iostream>

using namespace ii::runtime;
using namespace ii::examples;

class Controller final {
public:
    Controller(QueryPort<SetLevel>& destination, QueryPort<ReadDevice, DeviceFields>& source)
        : destination_(destination), source_(source) {}

    Submission update(EventLoop& loop) {
        return destination_.post(loop, {42}, [this](QueryReply<void> reply) {
            if (!reply) throw std::runtime_error("SetLevel query failed");
            auto fields = source_.send({.audio = {.output = {.level = true}}});
            if (!fields || !fields->audio || !fields->audio->output ||
                !fields->audio->output->level || fields->name || fields->object)
                throw std::runtime_error("ReadDevice returned an unexpected field selection");
            std::cout << "object query complete: level=" << *fields->audio->output->level
                      << " name_selected=" << fields->name.has_value()
                      << " object_selected=" << fields->object.has_value()
                      << " path=audio.output.level" << '\n';
        });
    }
private:
    QueryPort<SetLevel>& destination_;
    QueryPort<ReadDevice, DeviceFields>& source_;
};

int main() {
    Device device;
    Controller controller(device.audio.output.set_level, device.read);
    EventLoop loop;
    if (!controller.update(loop)) return 1;
    return loop.poll({8, std::chrono::seconds(1)}) == 1 ? 0 : 2;
}
