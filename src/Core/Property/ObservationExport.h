#pragma once

#if defined(IISDKCORE_OBSERVATION_STATIC)
#define IISDKCORE_OBSERVATION_API
#elif defined(_WIN32)
#if defined(IISDKCORE_OBSERVATION_BUILD)
#define IISDKCORE_OBSERVATION_API __declspec(dllexport)
#else
#define IISDKCORE_OBSERVATION_API __declspec(dllimport)
#endif
#else
#define IISDKCORE_OBSERVATION_API __attribute__((visibility("default")))
#endif
