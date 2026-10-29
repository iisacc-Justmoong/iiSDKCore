#include "Model.h"
#include <iiSDKCore/Core/Property/Observable.h>
#include <vector>
struct Model final {ii::core::Observable<int> count{3};};
extern "C" void* observable_model_create(){return new Model;}
extern "C" int observable_model_value(void* handle){return static_cast<Model*>(handle)->count.get()*2;}
extern "C" void observable_model_set(void* handle,int value){static_cast<Model*>(handle)->count.set(value);}
extern "C" void observable_model_destroy(void* handle){delete static_cast<Model*>(handle);}
extern "C" bool observable_model_scope_active(){return ii::core::ObservableReadScope::active();}
extern "C" int observable_model_collect(void* model,int(*operation)(void*),void* context,int* count){
    std::vector<ii::core::ObservableRead> reads;
    const auto value=ii::core::ObservableReadScope::collect([&](ii::core::ObservableRead read){reads.push_back(std::move(read));},[&]{
        return observable_model_value(model)+(operation?operation(context):0);
    });
    *count=static_cast<int>(reads.size());return value;
}
