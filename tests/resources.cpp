#include <iiSDKCore/Resource/Pack.h>
#include <iiSDKCore/Runtime/Operation.h>
#include "file_fixture.h"
#include <array>
#include <algorithm>
#include <chrono>
#include <iostream>
#include <thread>

using namespace ii;
using namespace std::chrono_literals;
namespace {
int checks=0;
void check(bool value,const char* what) { ++checks; if (!value) throw std::runtime_error(what); }
template<class T> void good(const T& value,const char* what) { check(bool(value),what); }
std::string text(const resource::View& view) {
    auto bytes=view.bytes(); return {reinterpret_cast<const char*>(bytes.data()),bytes.size()};
}
core::Bytes sample() {
    const std::array sources{
        resource::Source{"text/한글.txt",core::byte_view("Hello, native resources!")},
        resource::Source{"aliases/hello.txt",core::byte_view("Hello, native resources!")},
        resource::Source{"empty",{}},
        resource::Source{"binary",core::byte_view(std::string_view("a\0b",3))}
    };
    auto built=resource::build(sources);good(built,"build deterministic pack");return *built;
}
void patch(core::Bytes& bytes,std::size_t at,std::uint64_t value) {
    for (unsigned i=0;i<8;++i) bytes[at+i]=std::byte((value>>(i*8))&255);
}
void checksum(core::Bytes& bytes,std::size_t end) {
    std::uint64_t value=14695981039346656037ull;
    for (std::size_t i=0;i<end;++i) {
        if (i>=48 && i<56) continue;
        value^=std::to_integer<unsigned char>(bytes[i]);value*=1099511628211ull;
    }patch(bytes,48,value);
}
void contracts() {
    for (const auto* invalid:{"","/root","a/../b","a/./b","a//b","a\\b","a/","C:/asset"}) {
        check(!resource::valid_name(invalid),"reject noncanonical virtual names");
        const std::array source{resource::Source{invalid,{}}};
        check(!resource::build(source),"builder rejects invalid virtual name");
    }
    check(!resource::valid_name(std::string_view("nul\0name",8)) &&
        !resource::valid_name("\xc0\x80"),"strict UTF-8 and NUL boundary");
    check(resource::valid_name("icons/한글.svg"),"UTF-8 logical resource name");
    auto bytes=sample(); auto copied=resource::Pack::copy(bytes);good(copied,"own aligned data");
    check(copied->stats().entries==4 && copied->stats().unique_payloads==3 &&
        copied->stats().storage==resource::StorageKind::owned,"pack metadata and dedup counts");
    auto first=copied->find("text/한글.txt"); auto alias=copied->find("aliases/hello.txt");
    good(first,"find Unicode resource");good(alias,"find alias");
    check(first->bytes().data()==alias->bytes().data() && first->verify(),"dedup views share exact native payload");
    check(reinterpret_cast<std::uintptr_t>(first->bytes().data())%resource::PackAlignment==0,"SIMD-friendly payload alignment");
    check(text(*first)=="Hello, native resources!","byte fidelity");
    auto before=*first;auto after=std::move(before);
    check(before.bytes().empty() && before.name().empty() && !before.verify() &&
        after.bytes().data()==first->bytes().data(),"moved-from views cannot expose unowned storage");
    auto binary=copied->find("binary");good(binary,"binary entry");check(binary->bytes().size()==3 && binary->bytes()[1]==std::byte{},"preserve binary NUL");
    auto empty=copied->find("empty");good(empty,"empty resource");check(empty->bytes().empty() && empty->verify(),"zero-byte resource");
    check(!copied->find("missing") && !copied->find("../bad") && !copied->at(4),"missing and bounds errors");
    check(copied->at(0)->name()=="aliases/hello.txt","canonical binary-search order");
    const auto payload_offset=copied->stats().metadata_bytes;
    auto retained=*first;copied=core::fail(core::ErrorCode::closed,"removed");
    check(text(retained)=="Hello, native resources!" && retained.verify(),"view retains removed pack ownership");
    alignas(resource::PackAlignment) static std::array<std::byte,2048> embedded{};
    std::ranges::copy(bytes,embedded.begin());
    auto static_pack=resource::Pack::open_static(std::span(embedded).first(bytes.size()));good(static_pack,"native static pack");
    check(static_pack->stats().storage==resource::StorageKind::embedded &&
        static_pack->find("binary")->bytes().data()>=embedded.data() &&
        static_pack->find("binary")->bytes().data()<embedded.data()+embedded.size(),"embedded storage is not copied");
    check(!resource::Pack::open_static(std::span(embedded).subspan(1,bytes.size())),"reject misaligned borrowed storage");
    const std::array duplicate{resource::Source{"same",{}},resource::Source{"same",{}}};
    check(!resource::build(duplicate),"duplicate logical names rejected");
    check(!resource::build({}, {.maximum_bytes=1}),"build byte budget");
    check(!resource::Pack::copy(bytes,{.maximum_entries=1}),"parser entry budget");
    check(!resource::Pack::copy(bytes,{.maximum_metadata_bytes=64}),"parser metadata budget");
    auto damaged=bytes;damaged[payload_offset]^=std::byte{1};
    auto corrupted=resource::Pack::copy(damaged);good(corrupted,"payload verification is explicit, preserving demand paging");
    auto changed=corrupted->find("text/한글.txt");good(changed,"locate damaged payload");
    check(!changed->verify(),"detect payload corruption without claiming authenticity");
    for (std::size_t cut=0;cut<bytes.size();++cut) check(!resource::Pack::copy(std::span(bytes).first(cut)),"reject truncated pack");
    for (std::size_t index=0;index<64;++index) {
        auto bad=bytes;bad[index]^=std::byte{0x80};
        check(!resource::Pack::copy(bad),"validate every header field and metadata checksum");
    }
    // Recompute the non-security fingerprint so structural validation itself
    // must reject malformed metadata rather than stopping at the checksum.
    for (const auto [field,value]:std::array<std::pair<std::size_t,std::uint64_t>,5>{{
        {64,UINT64_MAX},{72,UINT64_MAX},{80,UINT64_MAX},{88,UINT64_MAX},{104,1}}}) {
        auto bad=bytes;patch(bad,field,value);checksum(bad,payload_offset);
        check(!resource::Pack::copy(bad),"structural checks reject correctly fingerprinted malicious metadata");
    }
    auto overlapping=bytes;patch(overlapping,64+48+16,payload_offset+1);checksum(overlapping,payload_offset);
    check(!resource::Pack::copy(overlapping),"reject misaligned/overlapping payload ranges");
    auto invalid_text=bytes;
    const auto names=64+4*48;
    invalid_text[names]=std::byte{0xff};checksum(invalid_text,payload_offset);
    check(!resource::Pack::copy(invalid_text),"validate name encoding even with correct metadata fingerprint");
    auto empty_pack=resource::build({});good(empty_pack,"empty pack encodes");
    check(empty_pack->size()==64 && resource::Pack::copy(*empty_pack)->stats().entries==0,"empty pack round-trip");
    resource::Pack absent;check(!absent.find("x") && !absent.at(0),"empty handle rejects access");
}
void mapping() {
    auto bytes=sample();
    const auto path=std::filesystem::current_path()/"resource-map-test.bin";
    auto file=testing::open_file(path,{io::Access::read_write,io::Creation::truncate});good(file,"provision fixture");
    good(file->write(bytes),"write pack fixture");
    auto map=io::MappedRegion::map(*file);
    if (!io::MappedRegion::supported()) {
        check(!map && map.error().code==io::ErrorCode::unsupported,"unsupported mapping is explicit");
        (void)file->close();std::filesystem::remove(path);return;
    }
    good(map,"OS read-only mapping");
    auto eof=io::MappedRegion::map(*file,bytes.size(),0);good(eof,"empty end-of-file range");
    check(eof->bytes().empty() && eof->backend()==map->backend(),"empty native view has no fabricated bytes");
    check(map->bytes().size()==bytes.size() && std::ranges::equal(map->bytes(),bytes),"mapped bytes");
    auto slice=map->slice(3,19);good(slice,"unaligned logical subrange");
    check(slice->bytes().data()==map->bytes().data()+3,"slice never copies");
    const auto hinted=slice->prefetch();
    check(hinted || (map->backend()==io::MappingBackend::windows && hinted.error().code==io::ErrorCode::unsupported),
        "native subrange prefetch or explicit optional Windows capability");
    if (map->backend()==io::MappingBackend::posix) good(slice->advise(io::AccessPattern::random),"native access hint");
    check(!map->slice(bytes.size()+1) && !map->slice(0,bytes.size()+1),"slice bounds");
    check(!slice->advise(static_cast<io::AccessPattern>(99)),"invalid paging hint rejected");
    check(!io::MappedRegion::map(*file,bytes.size()+1) && !io::MappedRegion::map(*file,1,bytes.size()),"map file bounds");
    auto unaligned=io::MappedRegion::map(*file,1,7);good(unaligned,"OS granularity handled privately");
    check(unaligned->bytes().data()!=nullptr && std::ranges::equal(unaligned->bytes(),std::span(bytes).subspan(1,7)),"unaligned mapping byte fidelity");
    auto pack=resource::Pack::open(*map);good(pack,"map-backed pack");
    auto held=pack->find("binary");good(held,"mapped view");
    check(pack->stats().storage==resource::StorageKind::mapped,"native backing observable");
    good(file->close(),"close original descriptor");
    check(!io::MappedRegion::map(*file),"closed source cannot map");
    pack=core::fail(core::ErrorCode::closed,"removed");map=io::Result<io::MappedRegion>{io::MappedRegion{}};
    check(held->bytes()[2]==std::byte{'b'} && held->verify(),"mapped view survives channel and pack removal");
    const auto resource_hint=held->prefetch();
    check(resource_hint || (io::platform_capabilities().backend==io::Backend::windows &&
        resource_hint.error().code==io::ErrorCode::unsupported),"resource-specific prefetch capability");
    held=core::fail(core::ErrorCode::closed,"removed");slice=io::MappedRegion{};unaligned=io::MappedRegion{};
    check(std::filesystem::remove(path),"release final mapping before fixture cleanup");
}
void optimization_and_async() {
    core::Bytes large(4*1024*1024,std::byte{0x5a});
    const std::array sources{resource::Source{"a",large},resource::Source{"b",large}};
    auto encoded=resource::build(sources);good(encoded,"large immutable assets");
    auto pack=resource::Pack::copy(*encoded);good(pack,"large pack");
    auto stats=pack->stats();
    check(stats.unique_payloads==1 && stats.logical_bytes==2*large.size() && stats.stored_bytes<large.size()+1024,"measured duplicate payload elimination");
    auto reference=pack->find("a");good(reference,"reference view");
    const auto started=runtime::Clock::now();
    for (int i=0;i<100000;++i) {
        auto view=pack->find(i%2?"a":"b");
        check(view && view->bytes().data()==reference->bytes().data(),"repeated lookups reuse native payload storage");
    }
    std::cout<<"RESOURCE_OPTIMIZATION logical_bytes="<<stats.logical_bytes<<" stored_bytes="<<stats.stored_bytes
        <<" unique_payloads="<<stats.unique_payloads<<" lookups=100000 lookup_us="
        <<std::chrono::duration_cast<std::chrono::microseconds>(runtime::Clock::now()-started).count()<<'\n';
    if (!runtime::Thread::supported()) return;
    std::atomic<bool> shared_ok=true;
    std::array<std::thread,4> readers;
    for (auto& reader:readers) reader=std::thread([shared=*pack,&shared_ok] {
        for (int i=0;i<1000;++i) {
            auto a=shared.find("a"),b=shared.find("b");
            if (!a || !b || a->bytes().data()!=b->bytes().data()) shared_ok=false;
        }
    });
    for (auto& reader:readers) reader.join();
    check(shared_ok,"concurrent pack copies query immutable shared storage");
    runtime::Runtime workers({.workers=1});runtime::OperationOwner jobs;runtime::TaskScope view;
    runtime::EventLoop events;bool delivered=false;
    auto task=jobs.submit<resource::View>(workers,[owner=*pack](auto& context)->core::Result<resource::View> {
        if (auto ready=context.checkpoint();!ready) return std::unexpected(ready.error());
        return owner.find("a");
    });
    good(task.then_on(events.sender(),view.sender(),[&](auto result) {
        check(result->value && result->value->bytes().size()==large.size(),"resource result retains immutable storage");
        delivered=true;
    }),"owned native resource completion");
    good(workers.join(),"join resource worker");events.poll();
    check(delivered,"resource completion reaches host loop");
}
}
int main() {
    try { contracts();mapping();optimization_and_async();std::cout<<"Resources: "<<checks<<" checks passed\n";return 0; }
    catch(const std::exception& error) { std::cerr<<error.what()<<'\n';return 1; }
}
