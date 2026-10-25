#pragma once
#include "LoadBalancer.h"
#include <span>
#include <string_view>
#include <vector>

namespace ii::runtime {
// Borrowed normalized metadata. Parsing, URL decoding, sockets and TLS belong
// to the network adapter; no request body or transport owner enters Core.
struct LoadField final { std::string_view name,value; };
struct LoadCondition final { std::string name,value; };
struct ApplicationRequest final {
    std::string_view host;
    std::string_view path="/";
    std::string_view method;
    std::span<const LoadField> headers;
    std::span<const LoadField> query;
};
struct ApplicationRoute final {
    std::uint32_t priority=1;
    LoadGroup group=0;
    std::string host; // ASCII case-insensitive glob: '*' and '?'
    std::string path_prefix; // case-sensitive prefix, after adapter normalization
    std::string method;
    std::vector<LoadCondition> headers,query;
};
struct ApplicationLoadBalancerOptions final {
    LoadBalancerOptions targets{.policy=BalancePolicy::least_outstanding};
    std::size_t max_rules=64,max_conditions=16,max_metadata_bytes=64*1024;
    std::size_t max_fields=128;
};
class ApplicationLoadBalancer final {
public:
    explicit ApplicationLoadBalancer(ApplicationLoadBalancerOptions={});
    ~ApplicationLoadBalancer();
    ApplicationLoadBalancer(const ApplicationLoadBalancer&)=delete;
    ApplicationLoadBalancer& operator=(const ApplicationLoadBalancer&)=delete;
    LoadBalancer& targets() noexcept;
    // Transactional, priority-sorted immutable snapshot; readers keep using the
    // previous complete configuration until this replacement is published.
    std::expected<void,SubmitError> configure(std::vector<ApplicationRoute>,LoadGroup default_group=0);
    std::expected<LoadLease,SubmitError> acquire(const ApplicationRequest&,LoadRequest={});
    Submission submit(const ApplicationRequest&,Task,LoadRequest={});
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
enum class ClassicProtocol { tcp,tls,http,https };
class ClassicLoadBalancer final {
public:
    explicit ClassicLoadBalancer(LoadBalancerOptions options={}):targets_(options) {}
    LoadBalancer& targets() noexcept { return targets_; }
    std::expected<LoadLease,SubmitError> acquire(ClassicProtocol,LoadRequest={});
    Submission submit(ClassicProtocol,Task,LoadRequest={});
private:
    LoadBalancer targets_;
};
} // namespace ii::runtime
