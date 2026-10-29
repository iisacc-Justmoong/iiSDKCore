#pragma once
#if defined(_WIN32)
#if defined(IISDKCORE_TEST_OBSERVABLE_MODULE)
#define OBSERVABLE_MODULE_API __declspec(dllexport)
#else
#define OBSERVABLE_MODULE_API __declspec(dllimport)
#endif
#else
#define OBSERVABLE_MODULE_API __attribute__((visibility("default")))
#endif
extern "C" {
OBSERVABLE_MODULE_API void* observable_model_create();
OBSERVABLE_MODULE_API int observable_model_value(void*);
OBSERVABLE_MODULE_API void observable_model_set(void*,int);
OBSERVABLE_MODULE_API void observable_model_destroy(void*);
OBSERVABLE_MODULE_API bool observable_model_scope_active();
OBSERVABLE_MODULE_API int observable_model_collect(void*,int(*)(void*),void*,int*);
}
