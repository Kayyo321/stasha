#ifndef CommonH
#define CommonH

enum {
    True = 1,
    False = 0,

    Ok = 0,
    Err = 1,
};

#define uns unsigned

typedef char byte_t;
typedef uns char boolean_t; 
typedef uns char result_t;
typedef uns long usize_t;

typedef struct {
    void *pointer;
    usize_t size;
    void *priv;
} heap_t;

extern void *const Null;
extern const heap_t NullHeap;

void quit(result_t res);

result_t open_logger(void);
result_t close_logger(void);

void log_msg(const char *fmt, ...);
void log_warn(const char *fmt, ...);
void log_err(const char *fmt, ...);

usize_t get_error_count(void);
usize_t get_warn_count(void);

#ifdef Testing
void restore_diagnostic_counts(usize warn_count, usize error_count);
#endif // Testing

heap_t allocate(usize_t count, usize_t bytes);
heap_t reallocate(heap_t heap, usize_t new_size);
void deallocate(heap_t heap);

result_t scan_and_deallocate(void); // returns Err if any leaked bytes are reclaimed, otherwise Ok

#if !defined(CommonAllowStdlibAllocators)
#define malloc(...) UseAllocateInsteadOfMalloc
#define realloc(...) UseReallocateInsteadOfRealloc
#define free(...) UseDeallocateInsteadOfFree
#endif

#endif