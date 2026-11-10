#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <windows.h>

// Build configurations may define NOMINMAX globally. Force the SDK macros on
// here as well so the public headers are exercised in either configuration.
#ifndef min
#define min(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef max
#define max(a, b) ((a) > (b) ? (a) : (b))
#endif

#include <iiSDKCore/Animation/Timeline.h>
#include <iiSDKCore/Core/Memory/SharedValue.h>
#include <iiSDKCore/Core/Serialization/CBOR.h>
#include <iiSDKCore/Core/Serialization/Serialization.h>
#include <iiSDKCore/Core/Task/Parallel.h>
#include <iiSDKCore/Core/Text/Number.h>
#include <iiSDKCore/Core/Time/Deadline.h>
#include <iiSDKCore/Core/Time/Time.h>
#include <iiSDKCore/Runtime/Concurrent.h>
#include <iiSDKCore/Runtime/LiveStream.h>
#include <iiSDKCore/Runtime/QueryStream.h>
#include <iiSDKCore/Runtime/QueryValue.h>
#include <iiSDKCore/Vulkan/Timeline.h>

#if !defined(min) || !defined(max)
#error "iiSDKCore public headers must preserve the consuming application's macros"
#endif
