#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#define IISDKCORE_PROCESS_FINAL final
#else
#define IISDKCORE_PROCESS_FINAL
#endif
typedef struct iisdkcore_process_owner iisdkcore_process_owner;
enum iisdkcore_process_error {
    IISDKCORE_PROCESS_OK=0, IISDKCORE_PROCESS_INVALID=1,
    IISDKCORE_PROCESS_OUT_OF_MEMORY=3, IISDKCORE_PROCESS_CLOSED=4,
    IISDKCORE_PROCESS_BUSY=5, IISDKCORE_PROCESS_FAILED=7,
    IISDKCORE_PROCESS_UNSUPPORTED=8
};
typedef struct iisdkcore_process_exit IISDKCORE_PROCESS_FINAL {
    int ready,reason;
    uint32_t code;
    int signal;
} iisdkcore_process_exit;
/* Child stdin is closed and both outputs are discarded. Concurrent control/wait
 * is allowed; destroy only after all calls using this owner have returned. */
int iisdkcore_process_start(size_t,const char* const*,iisdkcore_process_owner**);
void iisdkcore_process_destroy(iisdkcore_process_owner*);
int iisdkcore_process_id(iisdkcore_process_owner*,uint64_t*);
int iisdkcore_process_signal(iisdkcore_process_owner*,int,int);
int iisdkcore_process_wait(iisdkcore_process_owner*,uint64_t,iisdkcore_process_exit*);
int iisdkcore_process_cancel(iisdkcore_process_owner*,iisdkcore_process_exit*);
#undef IISDKCORE_PROCESS_FINAL
#ifdef __cplusplus
}
#endif
