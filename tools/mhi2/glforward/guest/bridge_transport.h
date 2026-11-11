/* Experimental QNX GLES bridge, adapted from the workspace GL-forward prototype. */
extern long write(int,const void *,unsigned long);

/* GL-forward guest shim. Records: little-endian opcode, payload length, payload.
 * Transport: optional QEMU MMIO service at 0x5f000000. */
typedef unsigned int   u32;
typedef int            i32;
void glUniform2iv(i32,i32,const i32 *);
void glUniform3iv(i32,i32,const i32 *);
void glUniform4iv(i32,i32,const i32 *);

/* ---- libc / libsocket (resolved by ldqnx at load) ---- */
extern char *getenv(const char *);
extern int   atoi(const char *);
extern unsigned long strlen(const char *);
extern void *mmap_device_memory(void *, unsigned, int, int, unsigned long long);
extern int usleep(unsigned);
extern int getpid(void);
extern int *__get_errno_ptr(void);
extern int shm_open(const char *, int, unsigned);
extern void *mmap64(void *, unsigned long, int, int, int, long long);
extern int munmap(void *, unsigned long);
extern int close(int);
static volatile unsigned char *bridge;
static unsigned char *bulk;
static unsigned bulk_size;
extern void *memcpy(void *,const void *,unsigned long);
static int g_fd = -1;
static u32 g_id;
extern int pthread_key_create(unsigned *, void (*)(void *));
extern void *pthread_getspecific(unsigned);
extern int pthread_setspecific(unsigned, const void *);
extern void *malloc(unsigned long);
extern void *realloc(void *, unsigned long);
extern void free(void *);
extern void abort(void);
static unsigned record_key;
static volatile unsigned record_key_state;
struct ClientAttrib { u32 size, type, norm, stride, buffer, enabled; const void *pointer; };
struct Record { unsigned char *data; unsigned size, used, capacity, reply;
    u32 array_buffer, element_buffer; struct ClientAttrib attribs[16];
#ifdef GLBRIDGE_MH2P_SCREEN
    u32 egl_thread;
    i32 egl_error;
    void *egl_context, *egl_draw, *egl_read;
#endif
};
#ifdef GLBRIDGE_MH2P_SCREEN
static void release_screen_record(struct Record *r);
static struct Record *screen_graphics_record(void);
#endif
static void release_record(void *p)
{ struct Record *r=p; if(r){
#ifdef GLBRIDGE_MH2P_SCREEN
    release_screen_record(r);
#endif
    free(r->data);free(r);} }
static struct Record *current_record(void)
{
    if (__atomic_load_n(&record_key_state,__ATOMIC_ACQUIRE) != 2) {
        if (__sync_bool_compare_and_swap(&record_key_state,0,1)) {
            if(pthread_key_create(&record_key,release_record))abort();
            __atomic_store_n(&record_key_state,2,__ATOMIC_RELEASE);
        } else while(__atomic_load_n(&record_key_state,__ATOMIC_ACQUIRE)!=2)usleep(1000);
    }
    struct Record *r=pthread_getspecific(record_key);
    if(!r){r=malloc(sizeof(*r));if(!r)abort();
        for(unsigned i=0;i<sizeof(*r);i++)((unsigned char *)r)[i]=0;
#ifdef GLBRIDGE_MH2P_SCREEN
        r->egl_thread=__sync_add_and_fetch(&g_id,1);
        r->egl_error=0x3000;
#endif
        if(pthread_setspecific(record_key,r))abort();}
    return r;
}
static struct Record *current_graphics_record(void)
{
#ifdef GLBRIDGE_MH2P_SCREEN
    return screen_graphics_record();
#else
    return current_record();
#endif
}
static void unlock_record(void)
{ __asm__ volatile("dmb sy" ::: "memory"); *(volatile u32 *)(bridge+16)=0; }
static volatile u32 connection_state;
static void transport_error(const char *message, u32 detail)
{
    static const char digits[] = "0123456789abcdef";
    char value[11] = "0x00000000\n";
    for (unsigned i = 0; i < 8; i++)
        value[2+i] = digits[(detail >> (28-4*i)) & 15];
    write(2, "glbridge-transport: ", 20);
    write(2, message, strlen(message));
    write(2, value, sizeof(value));
}
static void *transport_map(unsigned size, unsigned long long physical,
                           const char *name)
{
    if (!getenv("GLBRIDGE_SHARED_TRANSPORT"))
        return mmap_device_memory(0, size, 0xb00, 0, physical);
    int fd = shm_open(name, 2, 0);
    if (fd < 0) {
        transport_error("opening shared bridge region failed, errno=",
                        *__get_errno_ptr());
        return (void *)-1;
    }
    void *mapping = mmap64(0, size, 0xb00, 1, fd, 0);
    int error = mapping == (void *)-1 ? *__get_errno_ptr() : 0;
    if (close(fd)) {
        int close_error = *__get_errno_ptr();
        transport_error("closing shared bridge region failed, errno=", close_error);
        if (mapping != (void *)-1) {
            if (munmap(mapping, size))
                transport_error("unmapping shared bridge region failed, errno=",
                                *__get_errno_ptr());
            mapping = (void *)-1;
        }
        if (!error) error = close_error;
    }
    if (error) *__get_errno_ptr() = error;
    return mapping;
}
static int gl_conn(void)
{
    /* Publish the MMIO and bulk mappings together. Other rendering threads
     * must not observe bridge before bulk_size and g_fd are initialized. */
    for (;;) {
        if (__atomic_load_n(&connection_state,__ATOMIC_ACQUIRE)==2) return g_fd;
        if (__sync_bool_compare_and_swap(&connection_state,0,1)) break;
        usleep(1000);
    }
    bridge = transport_map(12288, 0x5f000000ULL, "/mh2p-glbridge-mmio");
    if (bridge == (void *)-1) {
        transport_error("MMIO mapping failed, errno=", *__get_errno_ptr());
        bridge=0;__atomic_store_n(&connection_state,0,__ATOMIC_RELEASE);return -1;
    }
    if (*(volatile u32 *)bridge != 0x474c4252) {
        transport_error("unexpected MMIO magic=", *(volatile u32 *)bridge);
        if (munmap((void *)bridge, 12288))
            transport_error("unmapping invalid bridge failed, errno=",
                            *__get_errno_ptr());
        bridge=0;__atomic_store_n(&connection_state,0,__ATOMIC_RELEASE);return -1;
    }
    if (*(volatile u32 *)(bridge+24) == 1024*1024) {
        bulk = transport_map(2*1024*1024, 0x5e000000ULL, "/mh2p-glbridge-bulk");
        if (bulk == (void *)-1) {
            transport_error("bulk mapping unavailable; using register transport, errno=",
                            *__get_errno_ptr());
            bulk=0;
        }
        if (bulk) bulk_size=1024*1024;
    }
    g_fd=1;__atomic_store_n(&connection_state,2,__ATOMIC_RELEASE);
    return g_fd;
}
static void send_all(const void *p, unsigned long n)
{
    const unsigned char *c = p;
    if (gl_conn() < 0) return;
    while (n) {
        if (bulk) {
            unsigned chunk=n>bulk_size?bulk_size:n;
            memcpy(bulk,c,chunk);
            __asm__ volatile("dmb sy" ::: "memory");
            *(volatile u32 *)(bridge+28)=chunk;
            c+=chunk;n-=chunk;
            continue;
        }
        unsigned chunk = n > 4096 ? 4096 : n;
        unsigned i=0;
        for(;i+4<=chunk;i+=4){u32 v=c[i]|((u32)c[i+1]<<8)|((u32)c[i+2]<<16)|((u32)c[i+3]<<24);
            *(volatile u32 *)(bridge+0x1000+i)=v;}
        for(;i<chunk;i++)bridge[0x1000+i]=c[i];
        __asm__ volatile("dmb sy" ::: "memory");
        *(volatile u32 *)(bridge+4) = chunk;
        c += chunk; n -= chunk;
    }
}
static int recv_bytes(void *p, unsigned long n,int release)
{
    unsigned char *c = p;
    if (gl_conn() < 0) return -1;
    while (n) {
        unsigned tries = 0, available;
        while (!(available=*(volatile u32 *)(bridge+(bulk?32:8)))) {
            if (++tries > 10000) {
                transport_error("reply timed out, remaining bytes=", n);
                unlock_record(); return -1;
            }
            usleep(1000);
        }
        unsigned chunk=available<n?available:n, i=0;
        if (bulk) {
            memcpy(c,bulk+bulk_size,chunk);
            __asm__ volatile("dmb sy" ::: "memory");
            *(volatile u32 *)(bridge+36)=chunk;
            c+=chunk;n-=chunk;
            continue;
        }
        for(;i+4<=chunk;i+=4){u32 v=*(volatile u32 *)(bridge+0x2000+i);
            for(unsigned j=0;j<4;j++)c[i+j]=v>>(8*j);}
        for(;i<chunk;i++)c[i]=bridge[0x2000+i];
        __asm__ volatile("dmb sy" ::: "memory");
        *(volatile u32 *)(bridge+20)=chunk;c+=chunk;n-=chunk;
    }
    if(release)unlock_record();
    return 0;
}

static int recv_all(void *p,unsigned long n){return recv_bytes(p,n,1);}

/* Build each complete command in thread-local storage before claiming MMIO.
 * Native clients can render from multiple threads and load both EGL/GLES DSOs. */
static void commit_record(struct Record *r)
{
    if(gl_conn()<0)return;
    while(*(volatile u32 *)(bridge+16))usleep(1000);
    send_all(r->data,r->size);
    if(!r->reply)unlock_record();
}
/* Write a record's framing (at most 32 bytes) and return its length. */
static unsigned record_header(struct Record *r,u32 op,u32 total,u32 *h)
{
    unsigned prefix=20;
    (void)r;
#ifdef GLBRIDGE_MH2P_SCREEN
    if(op!=1 && op!=139)prefix+=12;
#endif
    h[0]=119;h[1]=4;h[2]=getpid();
#ifdef GLBRIDGE_MH2P_SCREEN
    if(prefix==32){h[3]=140;h[4]=4;h[5]=r->egl_thread;}
#endif
    h[prefix/4-2]=op;h[prefix/4-1]=total;
    return prefix;
}
static void rec(u32 op,u32 total)
{
    struct Record *r=current_record();
    if(total>16*1024*1024)abort();
    u32 h[8];
    unsigned prefix=record_header(r,op,total,h);
    unsigned size=prefix+total;
    if(size>r->capacity){r->data=realloc(r->data,size);if(!r->data)abort();r->capacity=size;}
    memcpy(r->data,h,prefix);
    r->size=size;r->used=prefix;r->reply=(op>=100&&op<=109)||op==120||op==121||op==123||op==124||op==126||op==127||op==131||op==132||op==138||op==139||op==141||op==144||op==145||op==146||(op>=134&&op<=137);
    if(!total)commit_record(r);
}
static void record_part(const void *p,unsigned n)
{
    if(!n)return;
    struct Record *r=current_record();
    if(n>r->size-r->used)abort();
    memcpy(r->data+r->used,p,n);
    r->used+=n;
    if(r->used==r->size)commit_record(r);
}
#define PART(p, n) record_part((p), (n))

/* simple all-int record */
static void emit_iv(u32 op, const i32 *v, u32 n)
{
    rec(op, n * 4);
    PART(v, n * 4);
}
