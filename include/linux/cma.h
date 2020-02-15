#ifndef __CMA_H__
#define __CMA_H__

/*
 * There is always at least global CMA area and a few optional
 * areas configured in kernel .config.
 */
#ifdef CONFIG_CMA_AREAS
#define MAX_CMA_AREAS	(1 + CONFIG_CMA_AREAS)

#else
#define MAX_CMA_AREAS	(0)

#endif

struct cma;

extern unsigned long totalcma_pages;
extern unsigned long totalrbin_pages;
extern atomic_t rbin_allocated_pages;
extern atomic_t rbin_pool_pages;
extern phys_addr_t cma_get_base(const struct cma *cma);
extern unsigned long cma_get_size(const struct cma *cma);

extern int __init cma_declare_contiguous(phys_addr_t base,
			phys_addr_t size, phys_addr_t limit,
			phys_addr_t alignment, unsigned int order_per_bit,
			bool fixed, const char *name, struct cma **res_cma);
#ifdef CONFIG_RBIN
extern void cma_set_rbin(struct cma *cma);
#else
static inline void cma_set_rbin(struct cma *cma) {}
#endif

extern int cma_init_reserved_mem_with_name(phys_addr_t base, phys_addr_t size,
					unsigned int order_per_bit,
					struct cma **res_cma, const char *name);
extern int __init gcma_declare_contiguous(phys_addr_t base,
			phys_addr_t size, phys_addr_t limit,
			phys_addr_t alignment, unsigned int order_per_bit,
			bool fixed, const char *name, struct cma **res_cma);
static inline int cma_init_reserved_mem(phys_addr_t base, phys_addr_t size,
					unsigned int order_per_bit,
					struct cma **res_cma)
{
	return cma_init_reserved_mem_with_name(base, size, order_per_bit,
					       res_cma, NULL);
}
extern struct page *cma_alloc(struct cma *cma, size_t count, unsigned int align);
extern bool cma_release(struct cma *cma, const struct page *pages, unsigned int count);
#endif
