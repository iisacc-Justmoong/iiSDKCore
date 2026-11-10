#include "MigratingWork.h"
#include "HostLoop.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <optional>
#include <utility>

namespace ii::runtime {
namespace {
constexpr std::array<std::byte,8> magic{std::byte{'I'},std::byte{'I'},std::byte{'W'},std::byte{'O'},std::byte{'R'},std::byte{'K'},std::byte{1},std::byte{0}};
bool valid_kind(const std::string& value){
    return !value.empty()&&value.size()<=128&&std::ranges::all_of(value,[](unsigned char c){
        return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='.'||c=='_'||c=='-';
    });
}
bool valid_program(const MigrationProgram& value){return valid_kind(value.kind)&&value.version&&bool(value.advance);}
void write_number(std::vector<std::byte>& out,std::uint64_t value,unsigned width){
    for(unsigned i=width;i>0;--i)out.push_back(std::byte((value>>((i-1)*8))&0xff));
}
bool read_number(std::span<const std::byte> data,std::size_t& cursor,unsigned width,std::uint64_t& value){
    if(cursor>data.size()||width>data.size()-cursor)return false;
    value=0;for(unsigned i=0;i<width;++i)value=(value<<8)|std::to_integer<unsigned>(data[cursor++]);return true;
}
MigrationError rejected(SubmitError error){
    return error==SubmitError::full||error==SubmitError::overloaded?MigrationError::full:
           error==SubmitError::stopped?MigrationError::stopped:MigrationError::invalid;
}
bool settled(MigrationState state){return state!=MigrationState::queued&&state!=MigrationState::running;}
}
WorkCheckpoint::WorkCheckpoint(WorkCheckpoint&& other) noexcept
    :kind_(std::move(other.kind_)),version_(other.version_),steps_(other.steps_),data_(std::move(other.data_)),valid_(std::exchange(other.valid_,false)){}
WorkCheckpoint& WorkCheckpoint::operator=(WorkCheckpoint&& other) noexcept {
    if(this==&other)return *this;
    kind_=std::move(other.kind_);version_=other.version_;steps_=other.steps_;data_=std::move(other.data_);
    valid_=std::exchange(other.valid_,false);return *this;
}
void WorkCheckpoint::reset() noexcept {
    std::string{}.swap(kind_);std::vector<std::byte>{}.swap(data_);version_=steps_=0;valid_=false;
}
std::vector<std::byte> WorkCheckpoint::encode() const {
    if(!valid_)return {};
    std::vector<std::byte> out;out.reserve(36+kind_.size()+data_.size());
    out.insert(out.end(),magic.begin(),magic.end());write_number(out,kind_.size(),4);
    write_number(out,version_,8);write_number(out,steps_,8);write_number(out,data_.size(),8);
    for(unsigned char value:kind_)out.push_back(std::byte(value));
    out.insert(out.end(),data_.begin(),data_.end());return out;
}
std::expected<WorkCheckpoint,MigrationError> WorkCheckpoint::decode(std::span<const std::byte> input,std::size_t maximum_bytes){
    if(input.size()<36||!std::equal(magic.begin(),magic.end(),input.begin()))return std::unexpected(MigrationError::invalid);
    std::size_t cursor=magic.size();std::uint64_t kind_size=0,version=0,steps=0,data_size=0;
    if(!read_number(input,cursor,4,kind_size)||!read_number(input,cursor,8,version)||
       !read_number(input,cursor,8,steps)||!read_number(input,cursor,8,data_size))return std::unexpected(MigrationError::invalid);
    if(data_size>maximum_bytes)return std::unexpected(MigrationError::too_large);
    if(!kind_size||kind_size>128||kind_size>input.size()-cursor)return std::unexpected(MigrationError::invalid);
    const auto remaining=input.size()-cursor-static_cast<std::size_t>(kind_size);
    if(data_size!=remaining||!version)return std::unexpected(MigrationError::invalid);
    WorkCheckpoint out;out.kind_.reserve(static_cast<std::size_t>(kind_size));
    for(std::uint64_t i=0;i<kind_size;++i)out.kind_.push_back(char(std::to_integer<unsigned>(input[cursor++])));
    if(!valid_kind(out.kind_))return std::unexpected(MigrationError::invalid);
    out.version_=version;out.steps_=steps;out.data_.assign(input.begin()+static_cast<std::ptrdiff_t>(cursor),input.end());out.valid_=true;
    return out;
}

struct MigratingWork::State final {
    enum class Transition { none,paused,completed,failed,cancelled,migrate };
    struct Ticket final {
        std::weak_ptr<State> owner;
        std::uint64_t generation=0;
        std::atomic<bool> started{false},armed{true};
        ~Ticket();
    };
    mutable std::mutex mutex;
    std::condition_variable changed;
    std::shared_ptr<const MigrationProgram> program,pending_program;
    Runtime* executor=nullptr;
    Runtime* pending_executor=nullptr;
    std::vector<std::byte> data;
    std::size_t maximum_bytes=0;
    std::uint64_t generation=0;
    MigrationStatus current;
    Transition transition=Transition::none;
    MigrationError transition_error=MigrationError::none;
    bool pause_requested=false,cancel_requested=false,handing_off=false;

    static std::expected<void,MigrationError> schedule(const std::shared_ptr<State>& self,Runtime& target,
        std::shared_ptr<const MigrationProgram> program,std::shared_ptr<Ticket>& release_after_unlock){
        // The caller holds self->mutex. Runtime invokes accepted work asynchronously.
        if(self->generation==std::numeric_limits<std::uint64_t>::max())return std::unexpected(MigrationError::invalid);
        const auto previous_program=self->program;auto* previous_executor=self->executor;
        self->program=std::move(program);self->executor=&target;self->current.state=MigrationState::queued;
        self->current.error=MigrationError::none;self->transition=Transition::none;const auto generation=++self->generation;
        std::shared_ptr<Ticket> ticket;
        Submission submitted;
        try {
            ticket=std::make_shared<Ticket>();ticket->owner=self;ticket->generation=generation;
            submitted=target.post([self,ticket,generation]{ticket->started.store(true,std::memory_order_release);run(self,generation);});
        } catch(...) {
            if(ticket)ticket->armed.store(false,std::memory_order_release);
            self->program=previous_program;self->executor=previous_executor;self->current.state=MigrationState::paused;
            self->current.error=MigrationError::callback_failed;self->changed.notify_all();return std::unexpected(self->current.error);
        }
        if(!submitted){
            ticket->armed.store(false,std::memory_order_release);
            self->program=previous_program;self->executor=previous_executor;self->current.state=MigrationState::paused;
            self->current.error=rejected(submitted.error);self->changed.notify_all();return std::unexpected(self->current.error);
        }
        release_after_unlock=std::move(ticket);self->changed.notify_all();return {};
    }
    static void run(const std::shared_ptr<State>& self,std::uint64_t generation){
        {
            std::unique_lock lock(self->mutex);
            if(self->generation!=generation||self->current.state!=MigrationState::queued)return;
            self->current.state=MigrationState::running;self->changed.notify_all();
            if(self->cancel_requested){self->transition=Transition::cancelled;return;}
            if(self->pause_requested){self->pause_requested=false;self->transition=Transition::paused;return;}
        }
        for(;;){
            std::vector<std::byte> input;std::shared_ptr<const MigrationProgram> program;
            {
                std::lock_guard lock(self->mutex);
                if(self->cancel_requested){self->transition=Transition::cancelled;return;}
                input=std::move(self->data);program=self->program;
            }
            std::optional<MigrationStep> output;
            try{output=program->advance(input);}catch(...){}
            std::unique_lock lock(self->mutex);
            if(!output){self->data=std::move(input);self->transition=Transition::failed;self->transition_error=MigrationError::callback_failed;return;}
            if(output->checkpoint.size()>self->maximum_bytes||self->current.steps==std::numeric_limits<std::uint64_t>::max()){
                self->data=std::move(input);self->transition=Transition::failed;self->transition_error=MigrationError::too_large;return;
            }
            self->data=std::move(output->checkpoint);++self->current.steps;
            if(self->cancel_requested){self->transition=Transition::cancelled;return;}
            if(output->complete){self->transition=Transition::completed;return;}
            if(self->pause_requested){self->pause_requested=false;self->transition=Transition::paused;return;}
            if(self->pending_executor){
                if(self->pending_executor==self->executor){
                    self->pending_executor=nullptr;self->program=std::move(self->pending_program);lock.unlock();continue;
                }
                self->transition=Transition::migrate;return;
            }
        }
    }
};
MigratingWork::State::Ticket::~Ticket(){
    if(!armed.load(std::memory_order_acquire))return;
    auto state=owner.lock();if(!state)return;
    std::shared_ptr<const MigrationProgram> retired;
    std::shared_ptr<Ticket> release_after_unlock;
    std::unique_lock lock(state->mutex);
    if(state->generation!=generation)return;
    if(!started.load(std::memory_order_acquire)){
        if(state->current.state==MigrationState::queued){
            if(state->cancel_requested){
                retired=std::move(state->program);state->handing_off=true;lock.unlock();retired.reset();lock.lock();state->handing_off=false;
                state->current.state=MigrationState::cancelled;
            }else state->current.state=MigrationState::paused;
            state->current.error=MigrationError::stopped;state->changed.notify_all();
        }
        return;
    }
    if(state->current.state!=MigrationState::running)return;
    if(state->cancel_requested)state->transition=Transition::cancelled;
    if(state->transition==Transition::migrate){
        auto* target=std::exchange(state->pending_executor,nullptr);
        auto next=std::move(state->pending_program);
        retired=std::move(state->program);state->program=std::move(next);state->handing_off=true;
        lock.unlock();retired.reset();lock.lock();
        state->handing_off=false;
        if(state->cancel_requested){state->current.state=MigrationState::cancelled;state->changed.notify_all();return;}
        if(state->pause_requested){state->pause_requested=false;state->current.state=MigrationState::paused;state->changed.notify_all();return;}
        (void)State::schedule(state,*target,state->program,release_after_unlock);lock.unlock();return;
    }
    if(state->transition==Transition::paused){state->current.state=MigrationState::paused;state->transition=Transition::none;state->changed.notify_all();return;}
    const auto outcome=state->transition;
    retired=std::move(state->program);auto unused=std::move(state->pending_program);
    state->pending_executor=nullptr;state->handing_off=true;
    lock.unlock();retired.reset();unused.reset();lock.lock();state->handing_off=false;
    switch(state->cancel_requested?Transition::cancelled:outcome){
    case Transition::completed:state->current.state=MigrationState::completed;break;
    case Transition::failed:state->current.state=MigrationState::failed;state->current.error=state->transition_error;break;
    case Transition::cancelled:state->current.state=MigrationState::cancelled;break;
    default:state->current.state=MigrationState::failed;state->current.error=MigrationError::callback_failed;break;
    }
    state->transition=Transition::none;state->changed.notify_all();
}
MigratingWork::~MigratingWork(){cancel();}
MigratingWork::MigratingWork(MigratingWork&& other) noexcept:state_(std::move(other.state_)){}
MigratingWork& MigratingWork::operator=(MigratingWork&& other) noexcept {
    if(this!=&other){cancel();state_=std::move(other.state_);}return *this;
}
std::expected<MigratingWork,MigrationError> MigratingWork::start(Runtime& target,MigrationProgram program,std::vector<std::byte> initial,std::size_t maximum){
    if(!valid_program(program)||!maximum)return std::unexpected(MigrationError::invalid);
    if(initial.size()>maximum)return std::unexpected(MigrationError::too_large);
    auto state=std::make_shared<State>();state->program=std::make_shared<MigrationProgram>(std::move(program));state->data=std::move(initial);state->maximum_bytes=maximum;
    std::shared_ptr<State::Ticket> release_after_unlock;
    std::lock_guard lock(state->mutex);auto result=State::schedule(state,target,state->program,release_after_unlock);
    if(!result)return std::unexpected(result.error());return MigratingWork(std::move(state));
}
std::expected<MigratingWork,MigrationError> MigratingWork::resume(Runtime& target,MigrationProgram program,WorkCheckpoint& checkpoint,std::size_t maximum){
    if(!checkpoint.valid_||!valid_program(program)||!maximum||program.kind!=checkpoint.kind_||program.version!=checkpoint.version_)
        return std::unexpected(MigrationError::invalid);
    if(checkpoint.data_.size()>maximum)return std::unexpected(MigrationError::too_large);
    auto state=std::make_shared<State>();state->program=std::make_shared<MigrationProgram>(std::move(program));
    state->data=std::move(checkpoint.data_);state->maximum_bytes=maximum;state->current.steps=checkpoint.steps_;
    std::shared_ptr<State::Ticket> release_after_unlock;
    std::lock_guard lock(state->mutex);auto result=State::schedule(state,target,state->program,release_after_unlock);
    if(!result){checkpoint.data_=std::move(state->data);return std::unexpected(result.error());}
    checkpoint.valid_=false;return MigratingWork(std::move(state));
}
std::expected<void,MigrationError> MigratingWork::migrate(Runtime& target,MigrationProgram program){
    if(!state_||!valid_program(program))return std::unexpected(MigrationError::invalid);
    auto next=std::make_shared<MigrationProgram>(std::move(program));
    std::shared_ptr<const MigrationProgram> retained;std::shared_ptr<State::Ticket> release_after_unlock;std::unique_lock lock(state_->mutex);
    if(!state_->program||next->kind!=state_->program->kind||next->version!=state_->program->version)return std::unexpected(MigrationError::invalid);
    if(state_->handing_off)return std::unexpected(MigrationError::busy);
    if(state_->current.state==MigrationState::paused){
        retained=state_->program;auto result=State::schedule(state_,target,std::move(next),release_after_unlock);lock.unlock();return result;
    }
    if(state_->current.state!=MigrationState::queued&&state_->current.state!=MigrationState::running)return std::unexpected(MigrationError::stopped);
    if(state_->pause_requested||state_->pending_executor||state_->handing_off)return std::unexpected(MigrationError::busy);
    state_->pending_executor=&target;state_->pending_program=std::move(next);return {};
}
std::expected<void,MigrationError> MigratingWork::pause(){
    if(!state_)return std::unexpected(MigrationError::invalid);std::lock_guard lock(state_->mutex);
    if(state_->current.state==MigrationState::paused)return {};
    if(state_->current.state!=MigrationState::queued&&state_->current.state!=MigrationState::running)return std::unexpected(MigrationError::stopped);
    if(state_->pending_executor||state_->handing_off)return std::unexpected(MigrationError::busy);state_->pause_requested=true;return {};
}
std::expected<void,MigrationError> MigratingWork::resume(Runtime& target,MigrationProgram program){
    if(!state_||!valid_program(program))return std::unexpected(MigrationError::invalid);
    auto next=std::make_shared<MigrationProgram>(std::move(program));
    std::shared_ptr<const MigrationProgram> retained;std::shared_ptr<State::Ticket> release_after_unlock;std::unique_lock lock(state_->mutex);
    if(state_->current.state!=MigrationState::paused||state_->handing_off)return std::unexpected(MigrationError::busy);
    if(!state_->program||next->kind!=state_->program->kind||next->version!=state_->program->version)return std::unexpected(MigrationError::invalid);
    retained=state_->program;auto result=State::schedule(state_,target,std::move(next),release_after_unlock);lock.unlock();return result;
}
void MigratingWork::cancel() noexcept {
    if(!state_)return;std::shared_ptr<const MigrationProgram> retired;std::unique_lock lock(state_->mutex);
    if(state_->current.state==MigrationState::paused&&!state_->handing_off){
        state_->handing_off=true;retired=std::move(state_->program);lock.unlock();retired.reset();lock.lock();
        state_->handing_off=false;state_->current.state=MigrationState::cancelled;state_->changed.notify_all();
    }
    else if(state_->current.state==MigrationState::running||state_->current.state==MigrationState::queued)state_->cancel_requested=true;
}
MigrationStatus MigratingWork::status() const {
    if(!state_)return {MigrationState::cancelled,0,MigrationError::stopped};std::lock_guard lock(state_->mutex);return state_->current;
}
std::expected<MigrationStatus,MigrationError> MigratingWork::wait(std::chrono::nanoseconds timeout) const {
    if(!state_||timeout.count()<0)return std::unexpected(MigrationError::invalid);
    if(Runtime::is_worker_thread()||HostLoop::is_current_thread())return std::unexpected(MigrationError::wrong_executor);
    std::unique_lock lock(state_->mutex);const auto done=[&]{return settled(state_->current.state);};
    if(timeout==std::chrono::nanoseconds::max())state_->changed.wait(lock,done);
    else if(!state_->changed.wait_for(lock,timeout,done))return std::unexpected(MigrationError::timed_out);
    return state_->current;
}
std::expected<std::vector<std::byte>,MigrationError> MigratingWork::result() const {
    if(!state_)return std::unexpected(MigrationError::invalid);std::lock_guard lock(state_->mutex);
    if(state_->current.state!=MigrationState::completed)return std::unexpected(MigrationError::busy);return state_->data;
}
std::expected<WorkCheckpoint,MigrationError> MigratingWork::take_checkpoint(){
    if(!state_)return std::unexpected(MigrationError::invalid);
    std::shared_ptr<const MigrationProgram> retired;std::unique_lock lock(state_->mutex);
    if(state_->current.state!=MigrationState::paused||state_->handing_off||!state_->program)return std::unexpected(MigrationError::busy);
    WorkCheckpoint out;out.kind_=state_->program->kind;out.version_=state_->program->version;out.steps_=state_->current.steps;
    out.data_=std::move(state_->data);out.valid_=true;retired=std::move(state_->program);
    state_->current.state=MigrationState::transferred;state_->changed.notify_all();lock.unlock();retired.reset();return out;
}
} // namespace ii::runtime
