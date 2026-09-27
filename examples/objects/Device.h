#pragma once

#include <iiSDKCore/Runtime/Query.h>
#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>

namespace ii::examples {

struct SetLevel { int value; };
struct ReadOutput { bool level = false, revision = false; };
struct ReadAudio { ReadOutput output; };
struct ReadDevice { ReadAudio audio; bool name = false, object = false; };
struct OutputFields { std::optional<int> level; std::optional<std::size_t> revision; };
struct AudioFields { std::optional<OutputFields> output; };
class Device;
struct DeviceFields {
    std::optional<AudioFields> audio;
    std::optional<std::string> name;
    std::optional<std::reference_wrapper<const Device>> object;
};

// C++ composition is the storage tree; C++ access rules protect every edge.
// There is no parallel string-key registry that can bypass private ancestors.
class Device final {
    struct Internal {
        struct Identity {
            std::string name = "device A";
            std::string read_name() const { return name; }
        } identity;
    } internal_;

public:
    class Audio final {
    public:
        class Output final {
        public:
            struct Limits final {
                int minimum = 0, maximum = 100;
                bool contains(int value) const noexcept { return minimum <= value && value <= maximum; }
            };
        private:
            struct State {
                struct History {
                    std::size_t revision = 0;
                    void record() noexcept { ++revision; }
                } history;
                int level = 0;
                void set_level(int value) {
                    if (!Limits{}.contains(value)) throw std::out_of_range("Output level must be in [0, 100]");
                    level = value;
                    history.record();
                }
            } state_;

            void apply(const SetLevel& query) { state_.set_level(query.value); }

        public:
            const Limits limits{};
            const int& level = state_.level;
            const std::size_t& revision = state_.history.revision;
            runtime::QueryPort<SetLevel> set_level{[this](const SetLevel& query) { apply(query); }};

            void reset() { state_.set_level(0); }
        };

        Output output;
    };

    Audio audio;
    runtime::QueryPort<ReadDevice, DeviceFields> read{[this](const ReadDevice& query) {
        DeviceFields response;
        const auto& selected = query.audio.output;
        if (selected.level || selected.revision) {
            OutputFields output;
            if (selected.level) output.level = audio.output.level;
            if (selected.revision) output.revision = audio.output.revision;
            response.audio = AudioFields{std::move(output)};
        }
        if (query.name) response.name = internal_.identity.read_name();
        if (query.object) response.object = std::cref(*this);
        return response;
    }};
};

} // namespace ii::examples
