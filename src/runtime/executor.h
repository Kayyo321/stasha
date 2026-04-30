#ifndef STASHA_EXECUTOR_H
#define STASHA_EXECUTOR_H

#ifdef __cplusplus
extern "C" {
#endif

void __sts_executor_enqueue(void *coro_h);
void __sts_executor_remove(void *coro_h);
void __sts_executor_run_pending(void);
int  __sts_executor_pending(void);

#ifdef __cplusplus
}
#endif

#endif /* STASHA_EXECUTOR_H */
