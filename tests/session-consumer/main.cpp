#include <iiSDKCore/Runtime/Runtime.h>
#include <iiSDKCore/Runtime/Session.h>

struct Resource {
    int& destroyed;
    explicit Resource(int& value):destroyed(value) {}
    ~Resource(){++destroyed;}
};

int main() {
    int destroyed=0;
    ii::runtime::Runtime runtime({.workers=1,.capacity=4});
    auto session=runtime.create_session();
    auto value=session->objects().emplace<Resource>(destroyed);
    auto root=runtime.objects().emplace<Resource>(destroyed);
    if (!value || !root || value->with([](Resource&) {})!=ii::runtime::SubmitError::none) return 1;
    if (!session->close() || destroyed!=1 || !runtime.join() || destroyed!=2) return 2;
    return value->expired() && root->expired() ? 0 : 3;
}
