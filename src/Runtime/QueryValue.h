#pragma once

#include "Query.h"
#include "../Core/Property/Observable.h"
#include "../Core/Result/Result.h"
#include <concepts>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <thread>
#include <utility>

namespace ii::runtime {

// Preserve the typed transport failure, including a receiver's exception.
class QueryValueFailure final : public std::runtime_error {
public:
    explicit QueryValueFailure(QueryError error)
        : std::runtime_error("Query value delivery failed"),error_(std::move(error)) {}
    [[nodiscard]] const QueryError& error() const noexcept { return error_; }
private:
    QueryError error_;
};

// A borrowed projection of one owner's read/write queries. The change marker
// is only a dependency notification; the owner remains the source of truth.
// Ports and marker must outlive this object and every observation it creates.
template<class Value,class ReadRequest,class WriteRequest> class QueryValue final {
    static_assert(std::copy_constructible<Value> && std::copy_constructible<ReadRequest>);
public:
    using ReadPort=QueryPort<ReadRequest,Value>;
    using WritePort=QueryPort<WriteRequest,core::Result<void>>;
    using WriteFactory=std::function<WriteRequest(Value)>;

    QueryValue(const ReadPort& read,ReadRequest request,const WritePort& write,
               WriteFactory make_write,const core::Observable<std::uint64_t>& changed)
        :read_(read),request_(std::move(request)),write_(write),
         make_write_(std::move(make_write)),changed_(changed) {
        if(!make_write_)throw std::invalid_argument("Query value requires a write request factory");
        (void)changed.revision(); // Verify that construction occurs in the marker's owner domain.
    }
    QueryValue(const QueryValue&)=delete;
    QueryValue& operator=(const QueryValue&)=delete;
    QueryValue(QueryValue&&)=delete;
    QueryValue& operator=(QueryValue&&)=delete;

    [[nodiscard]] Value get() const {
        if(owner_!=std::this_thread::get_id())throw QueryValueFailure({QueryErrorCode::wrong_executor,{}});
        if(core::ObservableReadScope::active())core::ObservableReadScope::record(changed_.get().read_dependency());
        return read_value(read_.get(),request_);
    }
    [[nodiscard]] std::uint64_t revision() const { return changed_.get().revision(); }
    [[nodiscard]] core::ObservableRead read_dependency() const { return changed_.get().read_dependency(); }
    [[nodiscard]] core::ObservableSubscription observe(
            std::function<void(const Value&,std::uint64_t)> callback,bool initial=false) const {
        if(!callback)throw std::invalid_argument("Query value requires an observation callback");
        return changed_.get().observe([read=read_,request=request_,callback=std::move(callback)]
                (const std::uint64_t&,std::uint64_t revision) mutable {
            auto value=read_value(read.get(),request);
            callback(value,revision);
        },initial);
    }
    [[nodiscard]] core::Result<void> set(Value value) {
        if(owner_!=std::this_thread::get_id())return core::fail(core::ErrorCode::wrong_executor,"Query value requires its owner thread");
        auto response=write_.get().send(make_write_(std::move(value)));
        if(!response){
            const auto error=response.error();
            const auto code=error.code==QueryErrorCode::receiver_expired?core::ErrorCode::closed:
                error.code==QueryErrorCode::wrong_executor?core::ErrorCode::wrong_executor:core::ErrorCode::failed;
            return core::fail(code,"Query value write delivery failed",std::make_exception_ptr(QueryValueFailure(error)));
        }
        return std::move(*response);
    }
private:
    static Value read_value(const ReadPort& port,const ReadRequest& request){
        auto response=port.send(request);
        if(!response)throw QueryValueFailure(std::move(response.error()));
        return std::move(*response);
    }
    std::reference_wrapper<const ReadPort> read_;
    ReadRequest request_;
    std::reference_wrapper<const WritePort> write_;
    WriteFactory make_write_;
    std::reference_wrapper<const core::Observable<std::uint64_t>> changed_;
    const std::thread::id owner_=std::this_thread::get_id();
};

} // namespace ii::runtime
