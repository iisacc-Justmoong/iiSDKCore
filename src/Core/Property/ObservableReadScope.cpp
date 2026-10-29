#include "ObservableReadScope.h"

namespace ii::core {
namespace {
// One TLS slot per thread in the linked Observation execution component.
// A private backend DLL must use this same component rather than its own slot.
thread_local ObservableReadScope* current_scope=nullptr;
thread_local std::size_t scope_depth=0;
}
ObservableReadScope::ObservableReadScope(void* receiver,Delivery deliver)
    :receiver_(receiver),deliver_(deliver),previous_(current_scope){
    if(scope_depth==maximum_depth)throw std::length_error("Observable read scope depth exhausted");
    ++scope_depth;current_scope=this;
}
ObservableReadScope::~ObservableReadScope(){current_scope=previous_;--scope_depth;}
bool ObservableReadScope::active()noexcept{return current_scope&&current_scope->deliver_;}
void ObservableReadScope::record(ObservableRead read){
    if(!active())return;
    auto* scope=current_scope;current_scope=nullptr;
    struct Restore final {ObservableReadScope* previous;~Restore(){current_scope=previous;}}restore{scope};
    scope->deliver_(scope->receiver_,std::move(read));
}
}
