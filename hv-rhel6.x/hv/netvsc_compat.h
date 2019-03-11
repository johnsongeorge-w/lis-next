/*
 * Compatiability macros to adapt to older kernel versions
 */

static inline bool compat_napi_complete_done(struct napi_struct *n, int work_done)
{
	napi_complete(n);
	return true;
}

#define napi_complete_done  compat_napi_complete_done
