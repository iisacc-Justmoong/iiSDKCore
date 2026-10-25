#include "LoadRouting.h"
#include <algorithm>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>

namespace ii::runtime {
namespace {
char lower(char c) { return c>='A' && c<='Z' ? static_cast<char>(c+('a'-'A')) : c; }
bool equal_fold(std::string_view a,std::string_view b) {
    return a.size()==b.size() && std::equal(a.begin(),a.end(),b.begin(),[](char x,char y){return lower(x)==lower(y);});
}
bool host_matches(std::string_view pattern,std::string_view host) {
    std::size_t p=0,h=0,star=std::string_view::npos,retry=0;
    while(h<host.size()) {
        if(p<pattern.size() && (pattern[p]=='?' || lower(pattern[p])==lower(host[h]))) { ++p; ++h; }
        else if(p<pattern.size() && pattern[p]=='*') { star=p++; retry=h; }
        else if(star!=std::string_view::npos) { p=star+1; h=++retry; }
        else return false;
    }
    while(p<pattern.size() && pattern[p]=='*') ++p;
    return p==pattern.size();
}
bool text(std::string_view value) { return value.find_first_of(std::string_view("\0\r\n",3))==value.npos; }
bool field_matches(const LoadCondition& condition,std::span<const LoadField> fields,bool fold) {
    return std::ranges::any_of(fields,[&](const auto& field) {
        return (fold ? equal_fold(condition.name,field.name) : condition.name==field.name) && condition.value==field.value;
    });
}
bool route_matches(const ApplicationRoute& route,const ApplicationRequest& request) {
    if((!route.host.empty() && !host_matches(route.host,request.host)) ||
       (!route.path_prefix.empty() && !request.path.starts_with(route.path_prefix)) ||
       (!route.method.empty() && route.method!=request.method)) return false;
    return std::ranges::all_of(route.headers,[&](const auto& field){return field_matches(field,request.headers,true);}) &&
        std::ranges::all_of(route.query,[&](const auto& field){return field_matches(field,request.query,false);});
}
std::optional<BalancePolicy> classic_policy(ClassicProtocol protocol) {
    switch(protocol) {
        case ClassicProtocol::tcp: case ClassicProtocol::tls: return BalancePolicy::round_robin;
        case ClassicProtocol::http: case ClassicProtocol::https: return BalancePolicy::least_outstanding;
    }
    return {};
}
}
struct ApplicationLoadBalancer::Impl final {
    struct Rules final { std::vector<ApplicationRoute> routes; LoadGroup fallback=0; };
    const ApplicationLoadBalancerOptions options;
    LoadBalancer targets;
    mutable std::shared_mutex rules_mutex;
    std::shared_ptr<const Rules> rules;
    explicit Impl(ApplicationLoadBalancerOptions settings):options(settings),targets(settings.targets),rules(std::make_shared<Rules>()) {
        if(!options.max_rules || !options.max_conditions || !options.max_metadata_bytes || !options.max_fields ||
           options.targets.policy==BalancePolicy::flow_hash)
            throw std::invalid_argument("Application load routing requires positive bounds and a request routing policy");
    }
    bool consume(std::string_view value,std::size_t& used) const {
        if(!text(value) || value.size()>options.max_metadata_bytes-used) return false;
        used+=value.size(); return true;
    }
    std::expected<LoadGroup,SubmitError> resolve(const ApplicationRequest& request) const {
        std::size_t used=0;
        if(request.host.size()>253 || request.path.empty() || request.path.front()!='/' ||
            request.headers.size()>options.max_fields || request.query.size()>options.max_fields-request.headers.size() ||
            !consume(request.host,used) || !consume(request.path,used) || !consume(request.method,used))
            return std::unexpected(SubmitError::invalid);
        for(const auto fields:{request.headers,request.query}) for(const auto& field:fields)
            if(field.name.empty() || !consume(field.name,used) || !consume(field.value,used))
                return std::unexpected(SubmitError::invalid);
        std::shared_ptr<const Rules> snapshot;
        { std::shared_lock lock(rules_mutex); snapshot=rules; }
        for(const auto& route:snapshot->routes) if(route_matches(route,request)) return route.group;
        return snapshot->fallback;
    }
};
ApplicationLoadBalancer::ApplicationLoadBalancer(ApplicationLoadBalancerOptions options):impl_(std::make_unique<Impl>(options)) {}
ApplicationLoadBalancer::~ApplicationLoadBalancer()=default;
LoadBalancer& ApplicationLoadBalancer::targets() noexcept { return impl_->targets; }
std::expected<void,SubmitError> ApplicationLoadBalancer::configure(std::vector<ApplicationRoute> routes,LoadGroup fallback) {
    if(routes.size()>impl_->options.max_rules) return std::unexpected(SubmitError::full);
    for(const auto& route:routes) {
        const auto conditions=(!route.host.empty() ? 1U : 0U)+(!route.path_prefix.empty() ? 1U : 0U)+(!route.method.empty() ? 1U : 0U);
        auto remaining=impl_->options.max_conditions;
        for(const auto count:{static_cast<std::size_t>(conditions),route.headers.size(),route.query.size()}) {
            if(count>remaining) return std::unexpected(SubmitError::invalid);
            remaining-=count;
        }
        std::size_t used=0;
        if(!route.priority || route.host.size()>253 || (!route.path_prefix.empty() && route.path_prefix.front()!='/') ||
           !impl_->consume(route.host,used) || !impl_->consume(route.path_prefix,used) || !impl_->consume(route.method,used))
            return std::unexpected(SubmitError::invalid);
        for(const auto* fields:{&route.headers,&route.query}) for(const auto& field:*fields)
            if(field.name.empty() || !impl_->consume(field.name,used) || !impl_->consume(field.value,used))
                return std::unexpected(SubmitError::invalid);
    }
    std::ranges::sort(routes,{},&ApplicationRoute::priority);
    if(std::adjacent_find(routes.begin(),routes.end(),[](const auto& a,const auto& b){return a.priority==b.priority;})!=routes.end())
        return std::unexpected(SubmitError::invalid);
    auto snapshot=std::make_shared<Impl::Rules>();
    snapshot->routes=std::move(routes); snapshot->fallback=fallback;
    { std::unique_lock lock(impl_->rules_mutex); impl_->rules=std::move(snapshot); }
    return {};
}
std::expected<LoadLease,SubmitError> ApplicationLoadBalancer::acquire(const ApplicationRequest& request,LoadRequest resources) {
    const auto group=impl_->resolve(request);
    if(!group) return std::unexpected(group.error());
    resources.group=*group;
    if(resources.policy==BalancePolicy::flow_hash) return std::unexpected(SubmitError::invalid);
    return impl_->targets.acquire(resources);
}
Submission ApplicationLoadBalancer::submit(const ApplicationRequest& request,Task task,LoadRequest resources) {
    const auto group=impl_->resolve(request);
    if(!group) return {{},group.error()};
    resources.group=*group;
    if(resources.policy==BalancePolicy::flow_hash) return {{},SubmitError::invalid};
    return impl_->targets.submit(std::move(task),resources);
}
std::expected<LoadLease,SubmitError> ClassicLoadBalancer::acquire(ClassicProtocol protocol,LoadRequest resources) {
    const auto policy=classic_policy(protocol);
    if(!policy) return std::unexpected(SubmitError::invalid);
    resources.policy=*policy;
    return targets_.acquire(resources);
}
Submission ClassicLoadBalancer::submit(ClassicProtocol protocol,Task task,LoadRequest resources) {
    const auto policy=classic_policy(protocol);
    if(!policy) return {{},SubmitError::invalid};
    resources.policy=*policy;
    return targets_.submit(std::move(task),resources);
}
} // namespace ii::runtime
