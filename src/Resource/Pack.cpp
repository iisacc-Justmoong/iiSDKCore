#include "Pack.h"
#include "../Core/Text/Utf8.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <new>
#include <optional>
#include <stdexcept>
#include <tuple>
#include <unordered_map>
#include <variant>

namespace ii::resource {
namespace {
constexpr std::size_t Header=64,Entry=48;
constexpr std::array Magic{std::byte{'I'},std::byte{'I'},std::byte{'R'},std::byte{'P'},
    std::byte{'A'},std::byte{'C'},std::byte{'K'},std::byte{0}};
constexpr std::uint64_t Basis=14695981039346656037ull,Prime=1099511628211ull;
std::uint64_t hash(core::ByteView bytes,std::uint64_t value=Basis) noexcept {
    for (auto b:bytes) { value^=std::to_integer<unsigned char>(b);value*=Prime; }return value;
}
std::uint64_t number(core::ByteView data,std::size_t at) noexcept {
    std::uint64_t value=0;for (unsigned i=0;i<8;++i) value|=std::uint64_t(std::to_integer<unsigned char>(data[at+i]))<<(i*8);return value;
}
void put(core::Bytes& data,std::size_t at,std::uint64_t value) noexcept {
    for (unsigned i=0;i<8;++i) data[at+i]=std::byte((value>>(i*8))&255);
}
std::size_t aligned(std::size_t value) {
    if (value>std::numeric_limits<std::size_t>::max()-(PackAlignment-1)) throw std::invalid_argument("Resource alignment overflow");
    return (value+PackAlignment-1)&~(PackAlignment-1);
}
std::string_view name_at(core::ByteView data,std::size_t i) noexcept {
    const auto entry=Header+i*Entry;
    return {reinterpret_cast<const char*>(data.data()+number(data,entry)),static_cast<std::size_t>(number(data,entry+8))};
}
std::uint64_t metadata_hash(core::ByteView bytes,std::size_t end) noexcept {
    return hash(bytes.subspan(56,end-56),hash(bytes.first(48)));
}
PackStats inspect(core::ByteView data,Limits limits) {
    auto demand=[](bool value,const char* message) { if (!value) throw std::invalid_argument(message); };
    demand(data.size()>=Header,"Resource header is truncated");
    demand(std::ranges::equal(data.first(8),Magic),"Resource pack magic mismatch");
    demand(number(data,8)==1 && number(data,56)==0,"Unsupported resource version/flags/reserved field");
    demand(number(data,16)==data.size(),"Resource pack extent mismatch");
    const auto count=number(data,24),names=number(data,32),payload=number(data,40);
    demand(count<=limits.maximum_entries && count<=(data.size()-Header)/Entry,"Resource entry count exceeds extent/budget");
    demand(names==Header+count*Entry && names<=payload && payload<=data.size() &&
        payload%PackAlignment==0 && payload<=limits.maximum_metadata_bytes,"Resource metadata extent/alignment/budget invalid");
    demand(number(data,48)==metadata_hash(data,static_cast<std::size_t>(payload)),"Resource metadata fingerprint mismatch");
    struct Range final { std::uint64_t offset,size,fingerprint; };
    std::vector<Range> ranges;ranges.reserve(static_cast<std::size_t>(count));
    PackStats stats{.entries=static_cast<std::size_t>(count),.stored_bytes=data.size(),.metadata_bytes=static_cast<std::size_t>(payload)};
    std::uint64_t next_name=names;
    std::string_view previous;
    for (std::size_t i=0;i<count;++i) {
        const auto entry=Header+i*Entry;
        const auto name_offset=number(data,entry),name_size=number(data,entry+8);
        demand(name_offset==next_name && name_size<=payload-next_name &&
            name_size<=limits.maximum_name_bytes,"Resource name extent/budget invalid");
        const auto name=name_at(data,i);
        demand(valid_name(name) && (i==0 || previous<name),"Resource names must be canonical, sorted and unique");
        previous=name;next_name+=name_size;
        const auto offset=number(data,entry+16),size=number(data,entry+24),fingerprint=number(data,entry+32);
        demand(number(data,entry+40)==0 && offset>=payload && offset<=data.size() &&
            offset%PackAlignment==0 && size<=data.size()-offset,"Resource payload extent/alignment invalid");
        demand(size<=std::numeric_limits<std::uint64_t>::max()-stats.logical_bytes,"Resource logical extent overflow");
        stats.logical_bytes+=size;ranges.push_back({offset,size,fingerprint});
    }
    demand(aligned(static_cast<std::size_t>(next_name))==payload,"Resource name padding is noncanonical");
    std::ranges::sort(ranges,[](auto a,auto b) { return std::tie(a.offset,a.size)<std::tie(b.offset,b.size); });
    std::uint64_t end=payload;std::optional<Range> previous_range;
    for (const auto& range:ranges) {
        if (previous_range && range.offset==previous_range->offset && range.size==previous_range->size) {
            demand(range.fingerprint==previous_range->fingerprint,"Aliased resource fingerprints disagree");
            continue;
        }
        demand(range.offset==aligned(static_cast<std::size_t>(end)),"Resource ranges overlap or contain unindexed gaps");
        end=range.offset+range.size;previous_range=range;++stats.unique_payloads;
    }
    demand(end==data.size(),"Resource trailing extent is unindexed");
    return stats;
}
auto failed(const std::exception& error) {
    return core::fail(core::ErrorCode::invalid_argument,std::string("Resource pack: ")+error.what(),std::current_exception());
}
}
namespace detail {
struct PackData final {
    core::ByteView bytes;
    PackStats stats;
    std::variant<std::monostate,std::shared_ptr<const void>,io::MappedRegion> storage;
};
}
bool valid_name(std::string_view name) noexcept {
    if (name.empty() || !core::valid_utf8(name) || name.front()=='/' || name.back()=='/' ||
        name.find_first_of("\\:\0",0,3)!=name.npos) return false;
    while (!name.empty()) {
        const auto slash=name.find('/');const auto part=name.substr(0,slash);
        if (part.empty() || part=="." || part=="..") return false;
        if (slash==name.npos) break;
        name.remove_prefix(slash+1);
    }
    return true;
}
core::Result<core::Bytes> build(std::span<const Source> input,BuildOptions options) {
    try {
        if (input.size()>options.limits.maximum_entries || options.maximum_bytes<Header ||
            input.size()>(options.maximum_bytes-Header)/Entry)
            return core::fail(core::ErrorCode::full,"Resource build entry/byte budget exceeded");
        std::vector<Source> sources(input.begin(),input.end());
        std::ranges::sort(sources,{},&Source::name);
        std::size_t names_end=Header+sources.size()*Entry;
        std::string_view previous;bool first=true;
        for (const auto& source:sources) {
            if (source.name.size()>options.limits.maximum_name_bytes || !valid_name(source.name) ||
                (!first && source.name==previous))
                return core::fail(core::ErrorCode::invalid_argument,"Resource name invalid/duplicate: "+std::string(source.name));
            if (source.name.size()>options.maximum_bytes-names_end)
                return core::fail(core::ErrorCode::full,"Resource build name budget exceeded");
            names_end+=source.name.size();previous=source.name;first=false;
        }
        const auto payload=aligned(names_end);
        if (payload>options.maximum_bytes || payload>options.limits.maximum_metadata_bytes)
            return core::fail(core::ErrorCode::full,"Resource metadata budget exceeded");
        core::Bytes result(payload);std::ranges::copy(Magic,result.begin());
        put(result,8,1);put(result,24,sources.size());put(result,32,Header+sources.size()*Entry);put(result,40,payload);
        struct Payload final { core::ByteView bytes;std::size_t offset; };
        std::unordered_multimap<std::uint64_t,Payload> unique;unique.reserve(sources.size());
        auto name_offset=Header+sources.size()*Entry;
        for (std::size_t i=0;i<sources.size();++i) {
            const auto& source=sources[i];const auto fingerprint=hash(source.bytes);
            std::optional<std::size_t> offset;
            auto [begin,end]=unique.equal_range(fingerprint);
            for (;begin!=end;++begin) if (std::ranges::equal(begin->second.bytes,source.bytes)) { offset=begin->second.offset;break; }
            if (!offset) {
                const auto start=aligned(result.size());
                if (start>options.maximum_bytes || source.bytes.size()>options.maximum_bytes-start)
                    return core::fail(core::ErrorCode::full,"Resource payload byte budget exceeded");
                result.resize(start+source.bytes.size());
                std::ranges::copy(source.bytes,result.begin()+static_cast<std::ptrdiff_t>(start));
                offset=start;unique.emplace(fingerprint,Payload{source.bytes,start});
            }
            const auto entry=Header+i*Entry;
            put(result,entry,name_offset);put(result,entry+8,source.name.size());
            put(result,entry+16,*offset);put(result,entry+24,source.bytes.size());put(result,entry+32,fingerprint);
            std::ranges::copy(core::byte_view(source.name),result.begin()+static_cast<std::ptrdiff_t>(name_offset));
            name_offset+=source.name.size();
        }
        put(result,16,result.size());put(result,48,metadata_hash(result,payload));
        return result;
    } catch (const std::bad_alloc&) { return core::fail(core::ErrorCode::failed,"Resource build allocation failed",std::current_exception()); }
      catch (const std::exception& error) { return failed(error); }
}
core::Result<Pack> Pack::open_static(core::ByteView bytes,Limits limits) {
    try {
        auto stats=inspect(bytes,limits);stats.storage=StorageKind::embedded;
        if (reinterpret_cast<std::uintptr_t>(bytes.data())%PackAlignment)
            return core::fail(core::ErrorCode::invalid_argument,"Static resource storage is not 64-byte aligned");
        auto data=std::make_shared<detail::PackData>();data->bytes=bytes;data->stats=stats;
        Pack result;result.state_=std::move(data);return result;
    } catch (const std::bad_alloc&) { return core::fail(core::ErrorCode::failed,"Resource storage allocation failed",std::current_exception()); }
      catch (const std::exception& error) { return failed(error); }
}
core::Result<Pack> Pack::copy(core::ByteView bytes,Limits limits) {
    try {
        auto stats=inspect(bytes,limits);stats.storage=StorageKind::owned;
        auto* allocation=static_cast<std::byte*>(::operator new(bytes.size(),std::align_val_t{PackAlignment}));
        std::shared_ptr<const void> owner(allocation,[](const void* memory) { ::operator delete(const_cast<void*>(memory),std::align_val_t{PackAlignment}); });
        std::memcpy(allocation,bytes.data(),bytes.size());
        auto data=std::make_shared<detail::PackData>();data->bytes={allocation,bytes.size()};data->stats=stats;data->storage=std::move(owner);
        Pack result;result.state_=std::move(data);return result;
    } catch (const std::bad_alloc&) { return core::fail(core::ErrorCode::failed,"Resource storage allocation failed",std::current_exception()); }
      catch (const std::exception& error) { return failed(error); }
}
core::Result<Pack> Pack::open(io::MappedRegion mapping,Limits limits) {
    try {
        auto bytes=mapping.bytes();auto stats=inspect(bytes,limits);stats.storage=StorageKind::mapped;
        if (reinterpret_cast<std::uintptr_t>(bytes.data())%PackAlignment)
            return core::fail(core::ErrorCode::invalid_argument,"Mapped resource storage is not 64-byte aligned");
        auto data=std::make_shared<detail::PackData>();data->bytes=bytes;data->stats=stats;data->storage=std::move(mapping);
        Pack result;result.state_=std::move(data);return result;
    } catch (const std::bad_alloc&) { return core::fail(core::ErrorCode::failed,"Resource storage allocation failed",std::current_exception()); }
      catch (const std::exception& error) { return failed(error); }
}
core::Result<View> Pack::at(std::size_t index) const {
    if (!state_) return core::fail(core::ErrorCode::closed,"Resource pack is empty");
    if (index>=state_->stats.entries) return core::fail(core::ErrorCode::out_of_range,"Resource entry index out of range");
    const auto entry=Header+index*Entry;const auto bytes=state_->bytes;
    View result;result.owner_=state_;result.name_=name_at(bytes,index);
    result.bytes_=bytes.subspan(static_cast<std::size_t>(number(bytes,entry+16)),static_cast<std::size_t>(number(bytes,entry+24)));
    result.fingerprint_=number(bytes,entry+32);return result;
}
core::Result<View> Pack::find(std::string_view name) const {
    if (!state_) return core::fail(core::ErrorCode::closed,"Resource pack is empty");
    if (!valid_name(name)) return core::fail(core::ErrorCode::invalid_argument,"Resource lookup name is noncanonical");
    std::size_t first=0,last=state_->stats.entries;
    while (first<last) {
        const auto middle=first+(last-first)/2;
        if (name_at(state_->bytes,middle)<name) first=middle+1;else last=middle;
    }
    if (first==state_->stats.entries || name_at(state_->bytes,first)!=name)
        return core::fail(core::ErrorCode::not_found,"Resource not found: "+std::string(name));
    return at(first);
}
PackStats Pack::stats() const noexcept { return state_?state_->stats:PackStats{}; }
bool View::verify() const noexcept { return owner_ && hash(bytes_)==fingerprint_; }
io::Result<void> View::prefetch() const {
    if (!owner_) return std::unexpected(io::Error{io::ErrorCode::closed,{}});
    const auto mapping=std::get_if<io::MappedRegion>(&owner_->storage);
    if (!mapping) return std::unexpected(io::Error{io::ErrorCode::unsupported,{}});
    const auto slice=mapping->slice(static_cast<std::size_t>(bytes_.data()-owner_->bytes.data()),bytes_.size());
    if (!slice) return std::unexpected(slice.error());
    return slice->prefetch();
}
} // namespace ii::resource
