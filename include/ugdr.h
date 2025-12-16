#ifndef UGDR_H_
#define UGDR_H_

#ifdef __cplusplus
extern "C"{
#endif

struct ugdr_context;
struct ugdr_pd;

struct ugdr_context* ugdr_open_device(const char* dev_name);
int ugdr_close_device(struct ugdr_context* ctx);

struct ugdr_pd* ugdr_alloc_pd(struct ugdr_context* ctx);
int ugdr_dealloc_pd(struct ugdr_context* ctx, struct ugdr_pd* pd);

#ifdef __cplusplus
}
#endif

#endif // UGDR_H_
