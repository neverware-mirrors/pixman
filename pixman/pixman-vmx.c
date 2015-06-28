/*
 * Copyright © 2007 Luca Barbato
 *
 * Permission to use, copy, modify, distribute, and sell this software and its
 * documentation for any purpose is hereby granted without fee, provided that
 * the above copyright notice appear in all copies and that both that
 * copyright notice and this permission notice appear in supporting
 * documentation, and that the name of Luca Barbato not be used in advertising or
 * publicity pertaining to distribution of the software without specific,
 * written prior permission.  Luca Barbato makes no representations about the
 * suitability of this software for any purpose.  It is provided "as is"
 * without express or implied warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS
 * SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS, IN NO EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN
 * AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
 * OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS
 * SOFTWARE.
 *
 * Author:  Luca Barbato (lu_zero@gentoo.org)
 *
 * Based on fbmmx.c by Owen Taylor, Søren Sandmann and Nicholas Miell
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif
#include "pixman-private.h"
#include "pixman-combine32.h"
#include "pixman-inlines.h"
#include <altivec.h>

#define AVV(x...) {x}

static vector unsigned int mask_00ff;
static vector unsigned int mask_ff000000;
static vector unsigned int mask_red;
static vector unsigned int mask_green;
static vector unsigned int mask_blue;
static vector unsigned int mask_565_fix_rb;
static vector unsigned int mask_565_fix_g;

static force_inline vector unsigned int
splat_alpha (vector unsigned int pix)
{
#ifdef WORDS_BIGENDIAN
    return vec_perm (pix, pix,
		     (vector unsigned char)AVV (
			 0x00, 0x00, 0x00, 0x00, 0x04, 0x04, 0x04, 0x04,
			 0x08, 0x08, 0x08, 0x08, 0x0C, 0x0C, 0x0C, 0x0C));
#else
    return vec_perm (pix, pix,
		     (vector unsigned char)AVV (
			 0x03, 0x03, 0x03, 0x03, 0x07, 0x07, 0x07, 0x07,
			 0x0B, 0x0B, 0x0B, 0x0B, 0x0F, 0x0F, 0x0F, 0x0F));
#endif
}

static force_inline vector unsigned int
pix_multiply (vector unsigned int p, vector unsigned int a)
{
    vector unsigned short hi, lo, mod;

    /* unpack to short */
    hi = (vector unsigned short)
#ifdef WORDS_BIGENDIAN
	vec_mergeh ((vector unsigned char)AVV (0),
		    (vector unsigned char)p);
#else
	vec_mergeh ((vector unsigned char) p,
		    (vector unsigned char) AVV (0));
#endif

    mod = (vector unsigned short)
#ifdef WORDS_BIGENDIAN
	vec_mergeh ((vector unsigned char)AVV (0),
		    (vector unsigned char)a);
#else
	vec_mergeh ((vector unsigned char) a,
		    (vector unsigned char) AVV (0));
#endif

    hi = vec_mladd (hi, mod, (vector unsigned short)
                    AVV (0x0080, 0x0080, 0x0080, 0x0080,
                         0x0080, 0x0080, 0x0080, 0x0080));

    hi = vec_adds (hi, vec_sr (hi, vec_splat_u16 (8)));

    hi = vec_sr (hi, vec_splat_u16 (8));

    /* unpack to short */
    lo = (vector unsigned short)
#ifdef WORDS_BIGENDIAN
	vec_mergel ((vector unsigned char)AVV (0),
		    (vector unsigned char)p);
#else
	vec_mergel ((vector unsigned char) p,
		    (vector unsigned char) AVV (0));
#endif

    mod = (vector unsigned short)
#ifdef WORDS_BIGENDIAN
	vec_mergel ((vector unsigned char)AVV (0),
		    (vector unsigned char)a);
#else
	vec_mergel ((vector unsigned char) a,
		    (vector unsigned char) AVV (0));
#endif

    lo = vec_mladd (lo, mod, (vector unsigned short)
                    AVV (0x0080, 0x0080, 0x0080, 0x0080,
                         0x0080, 0x0080, 0x0080, 0x0080));

    lo = vec_adds (lo, vec_sr (lo, vec_splat_u16 (8)));

    lo = vec_sr (lo, vec_splat_u16 (8));

    return (vector unsigned int)vec_packsu (hi, lo);
}

static force_inline vector unsigned int
pix_add (vector unsigned int a, vector unsigned int b)
{
    return (vector unsigned int)vec_adds ((vector unsigned char)a,
                                          (vector unsigned char)b);
}

static force_inline vector unsigned int
pix_add_mul (vector unsigned int x,
             vector unsigned int a,
             vector unsigned int y,
             vector unsigned int b)
{
    vector unsigned int t1, t2;

    t1 = pix_multiply (x, a);
    t2 = pix_multiply (y, b);

    return pix_add (t1, t2);
}

static force_inline vector unsigned int
negate (vector unsigned int src)
{
    return vec_nor (src, src);
}

/* dest*~srca + src */
static force_inline vector unsigned int
over (vector unsigned int src,
      vector unsigned int srca,
      vector unsigned int dest)
{
    vector unsigned char tmp = (vector unsigned char)
	pix_multiply (dest, negate (srca));

    tmp = vec_adds ((vector unsigned char)src, tmp);
    return (vector unsigned int)tmp;
}

/* in == pix_multiply */
#define in_over(src, srca, mask, dest)					\
    over (pix_multiply (src, mask),					\
          pix_multiply (srca, mask), dest)

#ifdef WORDS_BIGENDIAN

#define COMPUTE_SHIFT_MASK(source)					\
    source ## _mask = vec_lvsl (0, source);

#define COMPUTE_SHIFT_MASKS(dest, source)				\
    source ## _mask = vec_lvsl (0, source);

#define COMPUTE_SHIFT_MASKC(dest, source, mask)				\
    mask ## _mask = vec_lvsl (0, mask);					\
    source ## _mask = vec_lvsl (0, source);

#define LOAD_VECTOR(source)				  \
do							  \
{							  \
    vector unsigned char tmp1, tmp2;			  \
    tmp1 = (typeof(tmp1))vec_ld (0, source);		  \
    tmp2 = (typeof(tmp2))vec_ld (15, source);		  \
    v ## source = (typeof(v ## source)) 		  \
	vec_perm (tmp1, tmp2, source ## _mask);		  \
} while (0)

#define LOAD_VECTORS(dest, source)			  \
do							  \
{							  \
    LOAD_VECTOR(source);				  \
    v ## dest = (typeof(v ## dest))vec_ld (0, dest);	  \
} while (0)

#define LOAD_VECTORSC(dest, source, mask)		  \
do							  \
{							  \
    LOAD_VECTORS(dest, source); 			  \
    LOAD_VECTOR(mask);					  \
} while (0)

#define DECLARE_SRC_MASK_VAR vector unsigned char src_mask
#define DECLARE_MASK_MASK_VAR vector unsigned char mask_mask

#else

/* Now the COMPUTE_SHIFT_{MASK, MASKS, MASKC} below are just no-op.
 * They are defined that way because little endian altivec can do unaligned
 * reads natively and have no need for constructing the permutation pattern
 * variables.
 */
#define COMPUTE_SHIFT_MASK(source)

#define COMPUTE_SHIFT_MASKS(dest, source)

#define COMPUTE_SHIFT_MASKC(dest, source, mask)

# define LOAD_VECTOR(source)				\
    v ## source = *((typeof(v ## source)*)source);

# define LOAD_VECTORS(dest, source)			\
    LOAD_VECTOR(source);				\
    LOAD_VECTOR(dest);					\

# define LOAD_VECTORSC(dest, source, mask)		\
    LOAD_VECTORS(dest, source); 			\
    LOAD_VECTOR(mask);					\

#define DECLARE_SRC_MASK_VAR
#define DECLARE_MASK_MASK_VAR

#endif /* WORDS_BIGENDIAN */

#define LOAD_VECTORSM(dest, source, mask)				\
    LOAD_VECTORSC (dest, source, mask); 				\
    v ## source = pix_multiply (v ## source,				\
                                splat_alpha (v ## mask));

#define STORE_VECTOR(dest)						\
    vec_st ((vector unsigned int) v ## dest, 0, dest);

/* load 4 pixels from a 16-byte boundary aligned address */
static force_inline vector unsigned int
load_128_aligned (const uint32_t* src)
{
    return *((vector unsigned int *) src);
}

/* load 4 pixels from a unaligned address */
static force_inline vector unsigned int
load_128_unaligned (const uint32_t* src)
{
    vector unsigned int vsrc;
    DECLARE_SRC_MASK_VAR;

    COMPUTE_SHIFT_MASK (src);
    LOAD_VECTOR (src);

    return vsrc;
}

/* save 4 pixels on a 16-byte boundary aligned address */
static force_inline void
save_128_aligned (uint32_t* data,
		  vector unsigned int vdata)
{
    STORE_VECTOR(data)
}

static force_inline vector unsigned int
create_mask_16_128 (uint16_t mask)
{
    uint16_t* src;
    vector unsigned short vsrc;
    DECLARE_SRC_MASK_VAR;

    src = &mask;

    COMPUTE_SHIFT_MASK (src);
    LOAD_VECTOR (src);
    return (vector unsigned int) vec_splat(vsrc, 0);
}

static force_inline vector unsigned int
create_mask_1x32_128 (const uint32_t *src)
{
    vector unsigned int vsrc;
    DECLARE_SRC_MASK_VAR;

    COMPUTE_SHIFT_MASK (src);
    LOAD_VECTOR (src);
    return vec_splat(vsrc, 0);
}

static force_inline vector unsigned int
create_mask_32_128 (uint32_t mask)
{
    return create_mask_1x32_128(&mask);
}

static force_inline vector unsigned int
unpack_32_1x128 (uint32_t data)
{
    vector unsigned int vdata = {0, 0, 0, data};
    vector unsigned short lo;

    lo = (vector unsigned short)
#ifdef WORDS_BIGENDIAN
	vec_mergel ((vector unsigned char) AVV(0),
		    (vector unsigned char) vdata);
#else
	vec_mergel ((vector unsigned char) vdata,
		    (vector unsigned char) AVV(0));
#endif

    return (vector unsigned int) lo;
}

static force_inline vector unsigned int
unpacklo_128_16x8 (vector unsigned int data1, vector unsigned int data2)
{
    vector unsigned char lo;

    /* unpack to short */
    lo = (vector unsigned char)
#ifdef WORDS_BIGENDIAN
	vec_mergel ((vector unsigned char) data2,
		    (vector unsigned char) data1);
#else
	vec_mergel ((vector unsigned char) data1,
		    (vector unsigned char) data2);
#endif

    return (vector unsigned int) lo;
}

static force_inline vector unsigned int
unpackhi_128_16x8 (vector unsigned int data1, vector unsigned int data2)
{
    vector unsigned char hi;

    /* unpack to short */
    hi = (vector unsigned char)
#ifdef WORDS_BIGENDIAN
	vec_mergeh ((vector unsigned char) data2,
		    (vector unsigned char) data1);
#else
	vec_mergeh ((vector unsigned char) data1,
		    (vector unsigned char) data2);
#endif

    return (vector unsigned int) hi;
}

static force_inline vector unsigned int
unpacklo_128_8x16 (vector unsigned int data1, vector unsigned int data2)
{
    vector unsigned short lo;

    /* unpack to char */
    lo = (vector unsigned short)
#ifdef WORDS_BIGENDIAN
	vec_mergel ((vector unsigned short) data2,
		    (vector unsigned short) data1);
#else
	vec_mergel ((vector unsigned short) data1,
		    (vector unsigned short) data2);
#endif

    return (vector unsigned int) lo;
}

static force_inline vector unsigned int
unpackhi_128_8x16 (vector unsigned int data1, vector unsigned int data2)
{
    vector unsigned short hi;

    /* unpack to char */
    hi = (vector unsigned short)
#ifdef WORDS_BIGENDIAN
	vec_mergeh ((vector unsigned short) data2,
		    (vector unsigned short) data1);
#else
	vec_mergeh ((vector unsigned short) data1,
		    (vector unsigned short) data2);
#endif

    return (vector unsigned int) hi;
}

static force_inline void
unpack_128_2x128 (vector unsigned int data1, vector unsigned int data2,
		    vector unsigned int* data_lo, vector unsigned int* data_hi)
{
    *data_lo = unpacklo_128_16x8(data1, data2);
    *data_hi = unpackhi_128_16x8(data1, data2);
}

static force_inline void
unpack_128_2x128_16 (vector unsigned int data1, vector unsigned int data2,
		    vector unsigned int* data_lo, vector unsigned int* data_hi)
{
    *data_lo = unpacklo_128_8x16(data1, data2);
    *data_hi = unpackhi_128_8x16(data1, data2);
}

static force_inline vector unsigned int
unpack_565_to_8888 (vector unsigned int lo)
{
    vector unsigned int r, g, b, rb, t;

    r = vec_and (vec_sl(lo, create_mask_32_128(8)), mask_red);
    g = vec_and (vec_sl(lo, create_mask_32_128(5)), mask_green);
    b = vec_and (vec_sl(lo, create_mask_32_128(3)), mask_blue);

    rb = vec_or (r, b);
    t  = vec_and (rb, mask_565_fix_rb);
    t  = vec_sr (t, create_mask_32_128(5));
    rb = vec_or (rb, t);

    t  = vec_and (g, mask_565_fix_g);
    t  = vec_sr (t, create_mask_32_128(6));
    g  = vec_or (g, t);

    return vec_or (rb, g);
}

static force_inline uint32_t
pack_1x128_32 (vector unsigned int data)
{
    vector unsigned char vpack;

    vpack = vec_packsu((vector unsigned short) data,
			(vector unsigned short) AVV(0));

    return vec_extract((vector unsigned int) vpack, 1);
}

static force_inline vector unsigned int
pack_2x128_128 (vector unsigned int lo, vector unsigned int hi)
{
    vector unsigned char vpack;

    vpack = vec_packsu((vector unsigned short) hi,
			(vector unsigned short) lo);

    return (vector unsigned int) vpack;
}

static force_inline void
negate_2x128 (vector unsigned int  data_lo,
	      vector unsigned int  data_hi,
	      vector unsigned int* neg_lo,
	      vector unsigned int* neg_hi)
{
    *neg_lo = vec_xor (data_lo, mask_00ff);
    *neg_hi = vec_xor (data_hi, mask_00ff);
}

static force_inline int
is_opaque (vector unsigned int x)
{
    uint32_t cmp_result;
    vector bool int ffs = vec_cmpeq(x, x);

    cmp_result = vec_all_eq(x, ffs);

    return (cmp_result & 0x8888) == 0x8888;
}

static force_inline int
is_zero (vector unsigned int x)
{
    uint32_t cmp_result;

    cmp_result = vec_all_eq(x, (vector unsigned int) AVV(0));

    return cmp_result == 0xffff;
}

static force_inline int
is_transparent (vector unsigned int x)
{
    uint32_t cmp_result;

    cmp_result = vec_all_eq(x, (vector unsigned int) AVV(0));
    return (cmp_result & 0x8888) == 0x8888;
}

static force_inline vector unsigned int
expand_pixel_8_1x128 (uint8_t data)
{
    vector unsigned int vdata;

    vdata = unpack_32_1x128 ((uint32_t) data);

#ifdef WORDS_BIGENDIAN
    return vec_perm (vdata, vdata,
		     (vector unsigned char)AVV (
			 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
			 0x0E, 0x0F, 0x0E, 0x0F, 0x0E, 0x0F, 0x0E, 0x0F));
#else
    return vec_perm (vdata, vdata,
		     (vector unsigned char)AVV (
			 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
			 0x08, 0x09, 0x08, 0x09, 0x08, 0x09, 0x08, 0x09));
#endif
}

static force_inline vector unsigned int
expand_alpha_1x128 (vector unsigned int data)
{
#ifdef WORDS_BIGENDIAN
    return vec_perm (data, data,
		     (vector unsigned char)AVV (
			 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01,
			 0x08, 0x09, 0x08, 0x09, 0x08, 0x09, 0x08, 0x09));
#else
    return vec_perm (data, data,
		     (vector unsigned char)AVV (
			 0x06, 0x07, 0x06, 0x07, 0x06, 0x07, 0x06, 0x07,
			 0x0E, 0x0F, 0x0E, 0x0F, 0x0E, 0x0F, 0x0E, 0x0F));
#endif
}

static force_inline void
expand_alpha_2x128 (vector unsigned int  data_lo,
		    vector unsigned int  data_hi,
		    vector unsigned int* alpha_lo,
		    vector unsigned int* alpha_hi)
{

    *alpha_lo = expand_alpha_1x128(data_lo);
    *alpha_hi = expand_alpha_1x128(data_hi);
}

static force_inline void
expand_alpha_rev_2x128 (vector unsigned int  data_lo,
			vector unsigned int  data_hi,
			vector unsigned int* alpha_lo,
			vector unsigned int* alpha_hi)
{
#ifdef WORDS_BIGENDIAN
    *alpha_lo = vec_perm (data_lo, data_lo,
		     (vector unsigned char)AVV (
			 0x06, 0x07, 0x06, 0x07, 0x06, 0x07, 0x06, 0x07,
			 0x0E, 0x0F, 0x0E, 0x0F, 0x0E, 0x0F, 0x0E, 0x0F));

    *alpha_hi = vec_perm (data_hi, data_hi,
		     (vector unsigned char)AVV (
			 0x06, 0x07, 0x06, 0x07, 0x06, 0x07, 0x06, 0x07,
			 0x0E, 0x0F, 0x0E, 0x0F, 0x0E, 0x0F, 0x0E, 0x0F));
#else
    *alpha_lo = vec_perm (data_lo, data_lo,
		     (vector unsigned char)AVV (
			 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01,
			 0x08, 0x09, 0x08, 0x09, 0x08, 0x09, 0x08, 0x09));

    *alpha_hi = vec_perm (data_hi, data_hi,
		     (vector unsigned char)AVV (
			 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01,
			 0x08, 0x09, 0x08, 0x09, 0x08, 0x09, 0x08, 0x09));
#endif
}

static force_inline void
pix_multiply_2x128 (vector unsigned int* data_lo,
		    vector unsigned int* data_hi,
		    vector unsigned int* alpha_lo,
		    vector unsigned int* alpha_hi,
		    vector unsigned int* ret_lo,
		    vector unsigned int* ret_hi)
{
    *ret_lo = pix_multiply(*data_lo, *alpha_lo);
    *ret_hi = pix_multiply(*data_hi, *alpha_hi);
}

static force_inline void
over_2x128 (vector unsigned int* src_lo,
	    vector unsigned int* src_hi,
	    vector unsigned int* alpha_lo,
	    vector unsigned int* alpha_hi,
	    vector unsigned int* dst_lo,
	    vector unsigned int* dst_hi)
{
    vector unsigned int t1, t2;

    negate_2x128 (*alpha_lo, *alpha_hi, &t1, &t2);

    pix_multiply_2x128 (dst_lo, dst_hi, &t1, &t2, dst_lo, dst_hi);

    *dst_lo = (vector unsigned int)
		    vec_adds ((vector unsigned char) *src_lo,
			      (vector unsigned char) *dst_lo);

    *dst_hi = (vector unsigned int)
		    vec_adds ((vector unsigned char) *src_hi,
			      (vector unsigned char) *dst_hi);
}

static force_inline void
in_over_2x128 (vector unsigned int* src_lo,
	       vector unsigned int* src_hi,
	       vector unsigned int* alpha_lo,
	       vector unsigned int* alpha_hi,
	       vector unsigned int* mask_lo,
	       vector unsigned int* mask_hi,
	       vector unsigned int* dst_lo,
	       vector unsigned int* dst_hi)
{
    vector unsigned int s_lo, s_hi;
    vector unsigned int a_lo, a_hi;

    pix_multiply_2x128 (src_lo,   src_hi, mask_lo, mask_hi, &s_lo, &s_hi);
    pix_multiply_2x128 (alpha_lo, alpha_hi, mask_lo, mask_hi, &a_lo, &a_hi);

    over_2x128 (&s_lo, &s_hi, &a_lo, &a_hi, dst_lo, dst_hi);
}

static force_inline uint32_t
core_combine_over_u_pixel_vmx (uint32_t src, uint32_t dst)
{
    uint8_t a;
    vector unsigned int vmxs;

    a = src >> 24;

    if (a == 0xff)
    {
	return src;
    }
    else if (src)
    {
	vmxs = unpack_32_1x128 (src);
	return pack_1x128_32(
		over(vmxs, expand_alpha_1x128 (vmxs), unpack_32_1x128 (dst)));
    }

    return dst;
}

static force_inline uint32_t
combine1 (const uint32_t *ps, const uint32_t *pm)
{
    uint32_t s = *ps;

    if (pm)
    {
	vector unsigned int ms, mm;

	mm = unpack_32_1x128 (*pm);
	mm = expand_alpha_1x128 (mm);

	ms = unpack_32_1x128 (s);
	ms = pix_multiply (ms, mm);

	s = pack_1x128_32 (ms);
    }

    return s;
}

static force_inline vector unsigned int
combine4 (const uint32_t* ps, const uint32_t* pm)
{
    vector unsigned int vmx_src_lo, vmx_src_hi;
    vector unsigned int vmx_msk_lo, vmx_msk_hi;
    vector unsigned int s;

    if (pm)
    {
	vmx_msk_lo = load_128_unaligned(pm);

	if (is_transparent(vmx_msk_lo))
	    return (vector unsigned int) AVV(0);
    }

    s = load_128_unaligned(ps);

    if (pm)
    {
	unpack_128_2x128(s, (vector unsigned int) AVV(0),
			    &vmx_src_lo, &vmx_src_hi);

	unpack_128_2x128(vmx_msk_lo, (vector unsigned int) AVV(0),
			    &vmx_msk_lo, &vmx_msk_hi);

	expand_alpha_2x128(vmx_msk_lo, vmx_msk_hi, &vmx_msk_lo, &vmx_msk_hi);

	pix_multiply_2x128(&vmx_src_lo, &vmx_src_hi,
			   &vmx_msk_lo, &vmx_msk_hi,
			   &vmx_src_lo, &vmx_src_hi);

	s = pack_2x128_128(vmx_src_lo, vmx_src_hi);
    }

    return s;
}

static void
vmx_combine_over_u_no_mask (uint32_t *      dest,
                            const uint32_t *src,
                            int             width)
{
    int i;
    vector unsigned int vdest, vsrc;
    DECLARE_SRC_MASK_VAR;

    while (width && ((uintptr_t)dest & 15))
    {
	uint32_t s = *src++;
	uint32_t d = *dest;
	uint32_t ia = ALPHA_8 (~s);

	UN8x4_MUL_UN8_ADD_UN8x4 (d, ia, s);

	*dest++ = d;
	width--;
    }

    COMPUTE_SHIFT_MASKS (dest, src);

    /* printf ("%s\n",__PRETTY_FUNCTION__); */
    for (i = width / 4; i > 0; i--)
    {

	LOAD_VECTORS (dest, src);

	vdest = over (vsrc, splat_alpha (vsrc), vdest);

	STORE_VECTOR (dest);

	src += 4;
	dest += 4;
    }

    for (i = width % 4; --i >= 0;)
    {
	uint32_t s = src[i];
	uint32_t d = dest[i];
	uint32_t ia = ALPHA_8 (~s);

	UN8x4_MUL_UN8_ADD_UN8x4 (d, ia, s);

	dest[i] = d;
    }
}

static void
vmx_combine_over_u_mask (uint32_t *      dest,
                         const uint32_t *src,
                         const uint32_t *mask,
                         int             width)
{
    int i;
    vector unsigned int vdest, vsrc, vmask;
    DECLARE_SRC_MASK_VAR;
    DECLARE_MASK_MASK_VAR;

    while (width && ((uintptr_t)dest & 15))
    {
	uint32_t m = ALPHA_8 (*mask++);
	uint32_t s = *src++;
	uint32_t d = *dest;
	uint32_t ia;

	UN8x4_MUL_UN8 (s, m);

	ia = ALPHA_8 (~s);

	UN8x4_MUL_UN8_ADD_UN8x4 (d, ia, s);
	*dest++ = d;
	width--;
    }

    COMPUTE_SHIFT_MASKC (dest, src, mask);

    /* printf ("%s\n",__PRETTY_FUNCTION__); */
    for (i = width / 4; i > 0; i--)
    {
	LOAD_VECTORSM (dest, src, mask);

	vdest = over (vsrc, splat_alpha (vsrc), vdest);

	STORE_VECTOR (dest);

	src += 4;
	dest += 4;
	mask += 4;
    }

    for (i = width % 4; --i >= 0;)
    {
	uint32_t m = ALPHA_8 (mask[i]);
	uint32_t s = src[i];
	uint32_t d = dest[i];
	uint32_t ia;

	UN8x4_MUL_UN8 (s, m);

	ia = ALPHA_8 (~s);

	UN8x4_MUL_UN8_ADD_UN8x4 (d, ia, s);
	dest[i] = d;
    }
}

static void
vmx_combine_over_u (pixman_implementation_t *imp,
                    pixman_op_t              op,
                    uint32_t *               dest,
                    const uint32_t *         src,
                    const uint32_t *         mask,
                    int                      width)
{
    if (mask)
	vmx_combine_over_u_mask (dest, src, mask, width);
    else
	vmx_combine_over_u_no_mask (dest, src, width);
}

static void
vmx_combine_over_reverse_u_no_mask (uint32_t *      dest,
                                    const uint32_t *src,
                                    int             width)
{
    int i;
    vector unsigned int vdest, vsrc;
    DECLARE_SRC_MASK_VAR;

    while (width && ((uintptr_t)dest & 15))
    {
	uint32_t s = *src++;
	uint32_t d = *dest;
	uint32_t ia = ALPHA_8 (~d);

	UN8x4_MUL_UN8_ADD_UN8x4 (s, ia, d);
	*dest++ = s;
	width--;
    }

    COMPUTE_SHIFT_MASKS (dest, src);

    /* printf ("%s\n",__PRETTY_FUNCTION__); */
    for (i = width / 4; i > 0; i--)
    {

	LOAD_VECTORS (dest, src);

	vdest = over (vdest, splat_alpha (vdest), vsrc);

	STORE_VECTOR (dest);

	src += 4;
	dest += 4;
    }

    for (i = width % 4; --i >= 0;)
    {
	uint32_t s = src[i];
	uint32_t d = dest[i];
	uint32_t ia = ALPHA_8 (~dest[i]);

	UN8x4_MUL_UN8_ADD_UN8x4 (s, ia, d);
	dest[i] = s;
    }
}

static void
vmx_combine_over_reverse_u_mask (uint32_t *      dest,
                                 const uint32_t *src,
                                 const uint32_t *mask,
                                 int             width)
{
    int i;
    vector unsigned int vdest, vsrc, vmask;
    DECLARE_SRC_MASK_VAR;
    DECLARE_MASK_MASK_VAR;

    while (width && ((uintptr_t)dest & 15))
    {
	uint32_t m = ALPHA_8 (*mask++);
	uint32_t s = *src++;
	uint32_t d = *dest;
	uint32_t ia = ALPHA_8 (~d);

	UN8x4_MUL_UN8 (s, m);

	UN8x4_MUL_UN8_ADD_UN8x4 (s, ia, d);
	*dest++ = s;
	width--;
    }

    COMPUTE_SHIFT_MASKC (dest, src, mask);

    /* printf ("%s\n",__PRETTY_FUNCTION__); */
    for (i = width / 4; i > 0; i--)
    {

	LOAD_VECTORSM (dest, src, mask);

	vdest = over (vdest, splat_alpha (vdest), vsrc);

	STORE_VECTOR (dest);

	src += 4;
	dest += 4;
	mask += 4;
    }

    for (i = width % 4; --i >= 0;)
    {
	uint32_t m = ALPHA_8 (mask[i]);
	uint32_t s = src[i];
	uint32_t d = dest[i];
	uint32_t ia = ALPHA_8 (~dest[i]);

	UN8x4_MUL_UN8 (s, m);

	UN8x4_MUL_UN8_ADD_UN8x4 (s, ia, d);
	dest[i] = s;
    }
}

static void
vmx_combine_over_reverse_u (pixman_implementation_t *imp,
                            pixman_op_t              op,
                            uint32_t *               dest,
                            const uint32_t *         src,
                            const uint32_t *         mask,
                            int                      width)
{
    if (mask)
	vmx_combine_over_reverse_u_mask (dest, src, mask, width);
    else
	vmx_combine_over_reverse_u_no_mask (dest, src, width);
}

static void
vmx_combine_in_u_no_mask (uint32_t *      dest,
                          const uint32_t *src,
                          int             width)
{
    int i;
    vector unsigned int vdest, vsrc;
    DECLARE_SRC_MASK_VAR;

    while (width && ((uintptr_t)dest & 15))
    {
	uint32_t s = *src++;
	uint32_t a = ALPHA_8 (*dest);

	UN8x4_MUL_UN8 (s, a);
	*dest++ = s;
	width--;
    }

    COMPUTE_SHIFT_MASKS (dest, src);

    /* printf ("%s\n",__PRETTY_FUNCTION__); */
    for (i = width / 4; i > 0; i--)
    {
	LOAD_VECTORS (dest, src);

	vdest = pix_multiply (vsrc, splat_alpha (vdest));

	STORE_VECTOR (dest);

	src += 4;
	dest += 4;
    }

    for (i = width % 4; --i >= 0;)
    {
	uint32_t s = src[i];
	uint32_t a = ALPHA_8 (dest[i]);

	UN8x4_MUL_UN8 (s, a);
	dest[i] = s;
    }
}

static void
vmx_combine_in_u_mask (uint32_t *      dest,
                       const uint32_t *src,
                       const uint32_t *mask,
                       int             width)
{
    int i;
    vector unsigned int vdest, vsrc, vmask;
    DECLARE_SRC_MASK_VAR;
    DECLARE_MASK_MASK_VAR;

    while (width && ((uintptr_t)dest & 15))
    {
	uint32_t m = ALPHA_8 (*mask++);
	uint32_t s = *src++;
	uint32_t a = ALPHA_8 (*dest);

	UN8x4_MUL_UN8 (s, m);
	UN8x4_MUL_UN8 (s, a);

	*dest++ = s;
	width--;
    }

    COMPUTE_SHIFT_MASKC (dest, src, mask);

    /* printf ("%s\n",__PRETTY_FUNCTION__); */
    for (i = width / 4; i > 0; i--)
    {
	LOAD_VECTORSM (dest, src, mask);

	vdest = pix_multiply (vsrc, splat_alpha (vdest));

	STORE_VECTOR (dest);

	src += 4;
	dest += 4;
	mask += 4;
    }

    for (i = width % 4; --i >= 0;)
    {
	uint32_t m = ALPHA_8 (mask[i]);
	uint32_t s = src[i];
	uint32_t a = ALPHA_8 (dest[i]);

	UN8x4_MUL_UN8 (s, m);
	UN8x4_MUL_UN8 (s, a);

	dest[i] = s;
    }
}

static void
vmx_combine_in_u (pixman_implementation_t *imp,
                  pixman_op_t              op,
                  uint32_t *               dest,
                  const uint32_t *         src,
                  const uint32_t *         mask,
                  int                      width)
{
    if (mask)
	vmx_combine_in_u_mask (dest, src, mask, width);
    else
	vmx_combine_in_u_no_mask (dest, src, width);
}

static void
vmx_combine_in_reverse_u_no_mask (uint32_t *      dest,
                                  const uint32_t *src,
                                  int             width)
{
    int i;
    vector unsigned int vdest, vsrc;
    DECLARE_SRC_MASK_VAR;

    while (width && ((uintptr_t)dest & 15))
    {
	uint32_t d = *dest;
	uint32_t a = ALPHA_8 (*src++);

	UN8x4_MUL_UN8 (d, a);

	*dest++ = d;
	width--;
    }

    COMPUTE_SHIFT_MASKS (dest, src);

    /* printf ("%s\n",__PRETTY_FUNCTION__); */
    for (i = width / 4; i > 0; i--)
    {
	LOAD_VECTORS (dest, src);

	vdest = pix_multiply (vdest, splat_alpha (vsrc));

	STORE_VECTOR (dest);

	src += 4;
	dest += 4;
    }

    for (i = width % 4; --i >= 0;)
    {
	uint32_t d = dest[i];
	uint32_t a = ALPHA_8 (src[i]);

	UN8x4_MUL_UN8 (d, a);

	dest[i] = d;
    }
}

static void
vmx_combine_in_reverse_u_mask (uint32_t *      dest,
                               const uint32_t *src,
                               const uint32_t *mask,
                               int             width)
{
    int i;
    vector unsigned int vdest, vsrc, vmask;
    DECLARE_SRC_MASK_VAR;
    DECLARE_MASK_MASK_VAR;

    while (width && ((uintptr_t)dest & 15))
    {
	uint32_t m = ALPHA_8 (*mask++);
	uint32_t d = *dest;
	uint32_t a = *src++;

	UN8x4_MUL_UN8 (a, m);
	a = ALPHA_8 (a);
	UN8x4_MUL_UN8 (d, a);

	*dest++ = d;
	width--;
    }

    COMPUTE_SHIFT_MASKC (dest, src, mask);

    /* printf ("%s\n",__PRETTY_FUNCTION__); */
    for (i = width / 4; i > 0; i--)
    {
	LOAD_VECTORSM (dest, src, mask);

	vdest = pix_multiply (vdest, splat_alpha (vsrc));

	STORE_VECTOR (dest);

	src += 4;
	dest += 4;
	mask += 4;
    }

    for (i = width % 4; --i >= 0;)
    {
	uint32_t m = ALPHA_8 (mask[i]);
	uint32_t d = dest[i];
	uint32_t a = src[i];

	UN8x4_MUL_UN8 (a, m);
	a = ALPHA_8 (a);
	UN8x4_MUL_UN8 (d, a);

	dest[i] = d;
    }
}

static void
vmx_combine_in_reverse_u (pixman_implementation_t *imp,
                          pixman_op_t              op,
                          uint32_t *               dest,
                          const uint32_t *         src,
                          const uint32_t *         mask,
                          int                      width)
{
    if (mask)
	vmx_combine_in_reverse_u_mask (dest, src, mask, width);
    else
	vmx_combine_in_reverse_u_no_mask (dest, src, width);
}

static void
vmx_combine_out_u_no_mask (uint32_t *      dest,
                           const uint32_t *src,
                           int             width)
{
    int i;
    vector unsigned int vdest, vsrc;
    DECLARE_SRC_MASK_VAR;

    while (width && ((uintptr_t)dest & 15))
    {
	uint32_t s = *src++;
	uint32_t a = ALPHA_8 (~(*dest));

	UN8x4_MUL_UN8 (s, a);

	*dest++ = s;
	width--;
    }

    COMPUTE_SHIFT_MASKS (dest, src);

    /* printf ("%s\n",__PRETTY_FUNCTION__); */
    for (i = width / 4; i > 0; i--)
    {
	LOAD_VECTORS (dest, src);

	vdest = pix_multiply (vsrc, splat_alpha (negate (vdest)));

	STORE_VECTOR (dest);

	src += 4;
	dest += 4;
    }

    for (i = width % 4; --i >= 0;)
    {
	uint32_t s = src[i];
	uint32_t a = ALPHA_8 (~dest[i]);

	UN8x4_MUL_UN8 (s, a);

	dest[i] = s;
    }
}

static void
vmx_combine_out_u_mask (uint32_t *      dest,
                        const uint32_t *src,
                        const uint32_t *mask,
                        int             width)
{
    int i;
    vector unsigned int vdest, vsrc, vmask;
    DECLARE_SRC_MASK_VAR;
    DECLARE_MASK_MASK_VAR;

    while (width && ((uintptr_t)dest & 15))
    {
	uint32_t m = ALPHA_8 (*mask++);
	uint32_t s = *src++;
	uint32_t a = ALPHA_8 (~(*dest));

	UN8x4_MUL_UN8 (s, m);
	UN8x4_MUL_UN8 (s, a);

	*dest++ = s;
	width--;
    }

    COMPUTE_SHIFT_MASKC (dest, src, mask);

    /* printf ("%s\n",__PRETTY_FUNCTION__); */
    for (i = width / 4; i > 0; i--)
    {
	LOAD_VECTORSM (dest, src, mask);

	vdest = pix_multiply (vsrc, splat_alpha (negate (vdest)));

	STORE_VECTOR (dest);

	src += 4;
	dest += 4;
	mask += 4;
    }

    for (i = width % 4; --i >= 0;)
    {
	uint32_t m = ALPHA_8 (mask[i]);
	uint32_t s = src[i];
	uint32_t a = ALPHA_8 (~dest[i]);

	UN8x4_MUL_UN8 (s, m);
	UN8x4_MUL_UN8 (s, a);

	dest[i] = s;
    }
}

static void
vmx_combine_out_u (pixman_implementation_t *imp,
                   pixman_op_t              op,
                   uint32_t *               dest,
                   const uint32_t *         src,
                   const uint32_t *         mask,
                   int                      width)
{
    if (mask)
	vmx_combine_out_u_mask (dest, src, mask, width);
    else
	vmx_combine_out_u_no_mask (dest, src, width);
}

static void
vmx_combine_out_reverse_u_no_mask (uint32_t *      dest,
                                   const uint32_t *src,
                                   int             width)
{
    int i;
    vector unsigned int vdest, vsrc;
    DECLARE_SRC_MASK_VAR;

    while (width && ((uintptr_t)dest & 15))
    {
	uint32_t d = *dest;
	uint32_t a = ALPHA_8 (~(*src++));

	UN8x4_MUL_UN8 (d, a);

	*dest++ = d;
	width--;
    }

    COMPUTE_SHIFT_MASKS (dest, src);

    /* printf ("%s\n",__PRETTY_FUNCTION__); */
    for (i = width / 4; i > 0; i--)
    {

	LOAD_VECTORS (dest, src);

	vdest = pix_multiply (vdest, splat_alpha (negate (vsrc)));

	STORE_VECTOR (dest);

	src += 4;
	dest += 4;
    }

    for (i = width % 4; --i >= 0;)
    {
	uint32_t d = dest[i];
	uint32_t a = ALPHA_8 (~src[i]);

	UN8x4_MUL_UN8 (d, a);

	dest[i] = d;
    }
}

static void
vmx_combine_out_reverse_u_mask (uint32_t *      dest,
                                const uint32_t *src,
                                const uint32_t *mask,
                                int             width)
{
    int i;
    vector unsigned int vdest, vsrc, vmask;
    DECLARE_SRC_MASK_VAR;
    DECLARE_MASK_MASK_VAR;

    while (width && ((uintptr_t)dest & 15))
    {
	uint32_t m = ALPHA_8 (*mask++);
	uint32_t d = *dest;
	uint32_t a = *src++;

	UN8x4_MUL_UN8 (a, m);
	a = ALPHA_8 (~a);
	UN8x4_MUL_UN8 (d, a);

	*dest++ = d;
	width--;
    }

    COMPUTE_SHIFT_MASKC (dest, src, mask);

    /* printf ("%s\n",__PRETTY_FUNCTION__); */
    for (i = width / 4; i > 0; i--)
    {
	LOAD_VECTORSM (dest, src, mask);

	vdest = pix_multiply (vdest, splat_alpha (negate (vsrc)));

	STORE_VECTOR (dest);

	src += 4;
	dest += 4;
	mask += 4;
    }

    for (i = width % 4; --i >= 0;)
    {
	uint32_t m = ALPHA_8 (mask[i]);
	uint32_t d = dest[i];
	uint32_t a = src[i];

	UN8x4_MUL_UN8 (a, m);
	a = ALPHA_8 (~a);
	UN8x4_MUL_UN8 (d, a);

	dest[i] = d;
    }
}

static void
vmx_combine_out_reverse_u (pixman_implementation_t *imp,
                           pixman_op_t              op,
                           uint32_t *               dest,
                           const uint32_t *         src,
                           const uint32_t *         mask,
                           int                      width)
{
    if (mask)
	vmx_combine_out_reverse_u_mask (dest, src, mask, width);
    else
	vmx_combine_out_reverse_u_no_mask (dest, src, width);
}

static void
vmx_combine_atop_u_no_mask (uint32_t *      dest,
                            const uint32_t *src,
                            int             width)
{
    int i;
    vector unsigned int vdest, vsrc;
    DECLARE_SRC_MASK_VAR;

    while (width && ((uintptr_t)dest & 15))
    {
	uint32_t s = *src++;
	uint32_t d = *dest;
	uint32_t dest_a = ALPHA_8 (d);
	uint32_t src_ia = ALPHA_8 (~s);

	UN8x4_MUL_UN8_ADD_UN8x4_MUL_UN8 (s, dest_a, d, src_ia);

	*dest++ = s;
	width--;
    }

    COMPUTE_SHIFT_MASKS (dest, src);

    /* printf ("%s\n",__PRETTY_FUNCTION__); */
    for (i = width / 4; i > 0; i--)
    {
	LOAD_VECTORS (dest, src);

	vdest = pix_add_mul (vsrc, splat_alpha (vdest),
			     vdest, splat_alpha (negate (vsrc)));

	STORE_VECTOR (dest);

	src += 4;
	dest += 4;
    }

    for (i = width % 4; --i >= 0;)
    {
	uint32_t s = src[i];
	uint32_t d = dest[i];
	uint32_t dest_a = ALPHA_8 (d);
	uint32_t src_ia = ALPHA_8 (~s);

	UN8x4_MUL_UN8_ADD_UN8x4_MUL_UN8 (s, dest_a, d, src_ia);

	dest[i] = s;
    }
}

static void
vmx_combine_atop_u_mask (uint32_t *      dest,
                         const uint32_t *src,
                         const uint32_t *mask,
                         int             width)
{
    int i;
    vector unsigned int vdest, vsrc, vmask;
    DECLARE_SRC_MASK_VAR;
    DECLARE_MASK_MASK_VAR;

    while (width && ((uintptr_t)dest & 15))
    {
	uint32_t m = ALPHA_8 (*mask++);
	uint32_t s = *src++;
	uint32_t d = *dest;
	uint32_t dest_a = ALPHA_8 (d);
	uint32_t src_ia;

	UN8x4_MUL_UN8 (s, m);

	src_ia = ALPHA_8 (~s);

	UN8x4_MUL_UN8_ADD_UN8x4_MUL_UN8 (s, dest_a, d, src_ia);

	*dest++ = s;
	width--;
    }

    COMPUTE_SHIFT_MASKC (dest, src, mask);

    /* printf ("%s\n",__PRETTY_FUNCTION__); */
    for (i = width / 4; i > 0; i--)
    {
	LOAD_VECTORSM (dest, src, mask);

	vdest = pix_add_mul (vsrc, splat_alpha (vdest),
			     vdest, splat_alpha (negate (vsrc)));

	STORE_VECTOR (dest);

	src += 4;
	dest += 4;
	mask += 4;
    }

    for (i = width % 4; --i >= 0;)
    {
	uint32_t m = ALPHA_8 (mask[i]);
	uint32_t s = src[i];
	uint32_t d = dest[i];
	uint32_t dest_a = ALPHA_8 (d);
	uint32_t src_ia;

	UN8x4_MUL_UN8 (s, m);

	src_ia = ALPHA_8 (~s);

	UN8x4_MUL_UN8_ADD_UN8x4_MUL_UN8 (s, dest_a, d, src_ia);

	dest[i] = s;
    }
}

static void
vmx_combine_atop_u (pixman_implementation_t *imp,
                    pixman_op_t              op,
                    uint32_t *               dest,
                    const uint32_t *         src,
                    const uint32_t *         mask,
                    int                      width)
{
    if (mask)
	vmx_combine_atop_u_mask (dest, src, mask, width);
    else
	vmx_combine_atop_u_no_mask (dest, src, width);
}

static void
vmx_combine_atop_reverse_u_no_mask (uint32_t *      dest,
                                    const uint32_t *src,
                                    int             width)
{
    int i;
    vector unsigned int vdest, vsrc;
    DECLARE_SRC_MASK_VAR;

    while (width && ((uintptr_t)dest & 15))
    {
	uint32_t s = *src++;
	uint32_t d = *dest;
	uint32_t src_a = ALPHA_8 (s);
	uint32_t dest_ia = ALPHA_8 (~d);

	UN8x4_MUL_UN8_ADD_UN8x4_MUL_UN8 (s, dest_ia, d, src_a);

	*dest++ = s;
	width--;
    }

    COMPUTE_SHIFT_MASKS (dest, src);

    /* printf ("%s\n",__PRETTY_FUNCTION__); */
    for (i = width / 4; i > 0; i--)
    {
	LOAD_VECTORS (dest, src);

	vdest = pix_add_mul (vdest, splat_alpha (vsrc),
			     vsrc, splat_alpha (negate (vdest)));

	STORE_VECTOR (dest);

	src += 4;
	dest += 4;
    }

    for (i = width % 4; --i >= 0;)
    {
	uint32_t s = src[i];
	uint32_t d = dest[i];
	uint32_t src_a = ALPHA_8 (s);
	uint32_t dest_ia = ALPHA_8 (~d);

	UN8x4_MUL_UN8_ADD_UN8x4_MUL_UN8 (s, dest_ia, d, src_a);

	dest[i] = s;
    }
}

static void
vmx_combine_atop_reverse_u_mask (uint32_t *      dest,
                                 const uint32_t *src,
                                 const uint32_t *mask,
                                 int             width)
{
    int i;
    vector unsigned int vdest, vsrc, vmask;
    DECLARE_SRC_MASK_VAR;
    DECLARE_MASK_MASK_VAR;

    while (width && ((uintptr_t)dest & 15))
    {
	uint32_t m = ALPHA_8 (*mask++);
	uint32_t s = *src++;
	uint32_t d = *dest;
	uint32_t src_a;
	uint32_t dest_ia = ALPHA_8 (~d);

	UN8x4_MUL_UN8 (s, m);

	src_a = ALPHA_8 (s);

	UN8x4_MUL_UN8_ADD_UN8x4_MUL_UN8 (s, dest_ia, d, src_a);

	*dest++ = s;
	width--;
    }

    COMPUTE_SHIFT_MASKC (dest, src, mask);

    /* printf ("%s\n",__PRETTY_FUNCTION__); */
    for (i = width / 4; i > 0; i--)
    {
	LOAD_VECTORSM (dest, src, mask);

	vdest = pix_add_mul (vdest, splat_alpha (vsrc),
			     vsrc, splat_alpha (negate (vdest)));

	STORE_VECTOR (dest);

	src += 4;
	dest += 4;
	mask += 4;
    }

    for (i = width % 4; --i >= 0;)
    {
	uint32_t m = ALPHA_8 (mask[i]);
	uint32_t s = src[i];
	uint32_t d = dest[i];
	uint32_t src_a;
	uint32_t dest_ia = ALPHA_8 (~d);

	UN8x4_MUL_UN8 (s, m);

	src_a = ALPHA_8 (s);

	UN8x4_MUL_UN8_ADD_UN8x4_MUL_UN8 (s, dest_ia, d, src_a);

	dest[i] = s;
    }
}

static void
vmx_combine_atop_reverse_u (pixman_implementation_t *imp,
                            pixman_op_t              op,
                            uint32_t *               dest,
                            const uint32_t *         src,
                            const uint32_t *         mask,
                            int                      width)
{
    if (mask)
	vmx_combine_atop_reverse_u_mask (dest, src, mask, width);
    else
	vmx_combine_atop_reverse_u_no_mask (dest, src, width);
}

static void
vmx_combine_xor_u_no_mask (uint32_t *      dest,
                           const uint32_t *src,
                           int             width)
{
    int i;
    vector unsigned int vdest, vsrc;
    DECLARE_SRC_MASK_VAR;

    while (width && ((uintptr_t)dest & 15))
    {
	uint32_t s = *src++;
	uint32_t d = *dest;
	uint32_t src_ia = ALPHA_8 (~s);
	uint32_t dest_ia = ALPHA_8 (~d);

	UN8x4_MUL_UN8_ADD_UN8x4_MUL_UN8 (s, dest_ia, d, src_ia);

	*dest++ = s;
	width--;
    }

    COMPUTE_SHIFT_MASKS (dest, src);

    /* printf ("%s\n",__PRETTY_FUNCTION__); */
    for (i = width / 4; i > 0; i--)
    {
	LOAD_VECTORS (dest, src);

	vdest = pix_add_mul (vsrc, splat_alpha (negate (vdest)),
			     vdest, splat_alpha (negate (vsrc)));

	STORE_VECTOR (dest);

	src += 4;
	dest += 4;
    }

    for (i = width % 4; --i >= 0;)
    {
	uint32_t s = src[i];
	uint32_t d = dest[i];
	uint32_t src_ia = ALPHA_8 (~s);
	uint32_t dest_ia = ALPHA_8 (~d);

	UN8x4_MUL_UN8_ADD_UN8x4_MUL_UN8 (s, dest_ia, d, src_ia);

	dest[i] = s;
    }
}

static void
vmx_combine_xor_u_mask (uint32_t *      dest,
                        const uint32_t *src,
                        const uint32_t *mask,
                        int             width)
{
    int i;
    vector unsigned int vdest, vsrc, vmask;
    DECLARE_SRC_MASK_VAR;
    DECLARE_MASK_MASK_VAR;

    while (width && ((uintptr_t)dest & 15))
    {
	uint32_t m = ALPHA_8 (*mask++);
	uint32_t s = *src++;
	uint32_t d = *dest;
	uint32_t src_ia;
	uint32_t dest_ia = ALPHA_8 (~d);

	UN8x4_MUL_UN8 (s, m);

	src_ia = ALPHA_8 (~s);

	UN8x4_MUL_UN8_ADD_UN8x4_MUL_UN8 (s, dest_ia, d, src_ia);

	*dest++ = s;
	width--;
    }

    COMPUTE_SHIFT_MASKC (dest, src, mask);

    /* printf ("%s\n",__PRETTY_FUNCTION__); */
    for (i = width / 4; i > 0; i--)
    {
	LOAD_VECTORSM (dest, src, mask);

	vdest = pix_add_mul (vsrc, splat_alpha (negate (vdest)),
			     vdest, splat_alpha (negate (vsrc)));

	STORE_VECTOR (dest);

	src += 4;
	dest += 4;
	mask += 4;
    }

    for (i = width % 4; --i >= 0;)
    {
	uint32_t m = ALPHA_8 (mask[i]);
	uint32_t s = src[i];
	uint32_t d = dest[i];
	uint32_t src_ia;
	uint32_t dest_ia = ALPHA_8 (~d);

	UN8x4_MUL_UN8 (s, m);

	src_ia = ALPHA_8 (~s);

	UN8x4_MUL_UN8_ADD_UN8x4_MUL_UN8 (s, dest_ia, d, src_ia);

	dest[i] = s;
    }
}

static void
vmx_combine_xor_u (pixman_implementation_t *imp,
                   pixman_op_t              op,
                   uint32_t *               dest,
                   const uint32_t *         src,
                   const uint32_t *         mask,
                   int                      width)
{
    if (mask)
	vmx_combine_xor_u_mask (dest, src, mask, width);
    else
	vmx_combine_xor_u_no_mask (dest, src, width);
}

static void
vmx_combine_add_u_no_mask (uint32_t *      dest,
                           const uint32_t *src,
                           int             width)
{
    int i;
    vector unsigned int vdest, vsrc;
    DECLARE_SRC_MASK_VAR;

    while (width && ((uintptr_t)dest & 15))
    {
	uint32_t s = *src++;
	uint32_t d = *dest;

	UN8x4_ADD_UN8x4 (d, s);

	*dest++ = d;
	width--;
    }

    COMPUTE_SHIFT_MASKS (dest, src);
    /* printf ("%s\n",__PRETTY_FUNCTION__); */
    for (i = width / 4; i > 0; i--)
    {
	LOAD_VECTORS (dest, src);

	vdest = pix_add (vsrc, vdest);

	STORE_VECTOR (dest);

	src += 4;
	dest += 4;
    }

    for (i = width % 4; --i >= 0;)
    {
	uint32_t s = src[i];
	uint32_t d = dest[i];

	UN8x4_ADD_UN8x4 (d, s);

	dest[i] = d;
    }
}

static void
vmx_combine_add_u_mask (uint32_t *      dest,
                        const uint32_t *src,
                        const uint32_t *mask,
                        int             width)
{
    int i;
    vector unsigned int vdest, vsrc, vmask;
    DECLARE_SRC_MASK_VAR;
    DECLARE_MASK_MASK_VAR;

    while (width && ((uintptr_t)dest & 15))
    {
	uint32_t m = ALPHA_8 (*mask++);
	uint32_t s = *src++;
	uint32_t d = *dest;

	UN8x4_MUL_UN8 (s, m);
	UN8x4_ADD_UN8x4 (d, s);

	*dest++ = d;
	width--;
    }

    COMPUTE_SHIFT_MASKC (dest, src, mask);

    /* printf ("%s\n",__PRETTY_FUNCTION__); */
    for (i = width / 4; i > 0; i--)
    {
	LOAD_VECTORSM (dest, src, mask);

	vdest = pix_add (vsrc, vdest);

	STORE_VECTOR (dest);

	src += 4;
	dest += 4;
	mask += 4;
    }

    for (i = width % 4; --i >= 0;)
    {
	uint32_t m = ALPHA_8 (mask[i]);
	uint32_t s = src[i];
	uint32_t d = dest[i];

	UN8x4_MUL_UN8 (s, m);
	UN8x4_ADD_UN8x4 (d, s);

	dest[i] = d;
    }
}

static void
vmx_combine_add_u (pixman_implementation_t *imp,
                   pixman_op_t              op,
                   uint32_t *               dest,
                   const uint32_t *         src,
                   const uint32_t *         mask,
                   int                      width)
{
    if (mask)
	vmx_combine_add_u_mask (dest, src, mask, width);
    else
	vmx_combine_add_u_no_mask (dest, src, width);
}

static void
vmx_combine_src_ca (pixman_implementation_t *imp,
                    pixman_op_t              op,
                    uint32_t *               dest,
                    const uint32_t *         src,
                    const uint32_t *         mask,
                    int                      width)
{
    int i;
    vector unsigned int vdest, vsrc, vmask;
    DECLARE_SRC_MASK_VAR;
    DECLARE_MASK_MASK_VAR;

    while (width && ((uintptr_t)dest & 15))
    {
	uint32_t a = *mask++;
	uint32_t s = *src++;

	UN8x4_MUL_UN8x4 (s, a);

	*dest++ = s;
	width--;
    }

    COMPUTE_SHIFT_MASKC (dest, src, mask);

    /* printf ("%s\n",__PRETTY_FUNCTION__); */
    for (i = width / 4; i > 0; i--)
    {
	LOAD_VECTORSC (dest, src, mask);

	vdest = pix_multiply (vsrc, vmask);

	STORE_VECTOR (dest);

	mask += 4;
	src += 4;
	dest += 4;
    }

    for (i = width % 4; --i >= 0;)
    {
	uint32_t a = mask[i];
	uint32_t s = src[i];

	UN8x4_MUL_UN8x4 (s, a);

	dest[i] = s;
    }
}

static void
vmx_combine_over_ca (pixman_implementation_t *imp,
                     pixman_op_t              op,
                     uint32_t *               dest,
                     const uint32_t *         src,
                     const uint32_t *         mask,
                     int                      width)
{
    int i;
    vector unsigned int vdest, vsrc, vmask;
    DECLARE_SRC_MASK_VAR;
    DECLARE_MASK_MASK_VAR;

    while (width && ((uintptr_t)dest & 15))
    {
	uint32_t a = *mask++;
	uint32_t s = *src++;
	uint32_t d = *dest;
	uint32_t sa = ALPHA_8 (s);

	UN8x4_MUL_UN8x4 (s, a);
	UN8x4_MUL_UN8 (a, sa);
	UN8x4_MUL_UN8x4_ADD_UN8x4 (d, ~a, s);

	*dest++ = d;
	width--;
    }

    COMPUTE_SHIFT_MASKC (dest, src, mask);

    /* printf ("%s\n",__PRETTY_FUNCTION__); */
    for (i = width / 4; i > 0; i--)
    {
	LOAD_VECTORSC (dest, src, mask);

	vdest = in_over (vsrc, splat_alpha (vsrc), vmask, vdest);

	STORE_VECTOR (dest);

	mask += 4;
	src += 4;
	dest += 4;
    }

    for (i = width % 4; --i >= 0;)
    {
	uint32_t a = mask[i];
	uint32_t s = src[i];
	uint32_t d = dest[i];
	uint32_t sa = ALPHA_8 (s);

	UN8x4_MUL_UN8x4 (s, a);
	UN8x4_MUL_UN8 (a, sa);
	UN8x4_MUL_UN8x4_ADD_UN8x4 (d, ~a, s);

	dest[i] = d;
    }
}

static void
vmx_combine_over_reverse_ca (pixman_implementation_t *imp,
                             pixman_op_t              op,
                             uint32_t *               dest,
                             const uint32_t *         src,
                             const uint32_t *         mask,
                             int                      width)
{
    int i;
    vector unsigned int vdest, vsrc, vmask;
    DECLARE_SRC_MASK_VAR;
    DECLARE_MASK_MASK_VAR;

    while (width && ((uintptr_t)dest & 15))
    {
	uint32_t a = *mask++;
	uint32_t s = *src++;
	uint32_t d = *dest;
	uint32_t ida = ALPHA_8 (~d);

	UN8x4_MUL_UN8x4 (s, a);
	UN8x4_MUL_UN8_ADD_UN8x4 (s, ida, d);

	*dest++ = s;
	width--;
    }

    COMPUTE_SHIFT_MASKC (dest, src, mask);

    /* printf("%s\n",__PRETTY_FUNCTION__); */
    for (i = width / 4; i > 0; i--)
    {
	LOAD_VECTORSC (dest, src, mask);

	vdest = over (vdest, splat_alpha (vdest), pix_multiply (vsrc, vmask));

	STORE_VECTOR (dest);

	mask += 4;
	src += 4;
	dest += 4;
    }

    for (i = width % 4; --i >= 0;)
    {
	uint32_t a = mask[i];
	uint32_t s = src[i];
	uint32_t d = dest[i];
	uint32_t ida = ALPHA_8 (~d);

	UN8x4_MUL_UN8x4 (s, a);
	UN8x4_MUL_UN8_ADD_UN8x4 (s, ida, d);

	dest[i] = s;
    }
}

static void
vmx_combine_in_ca (pixman_implementation_t *imp,
                   pixman_op_t              op,
                   uint32_t *               dest,
                   const uint32_t *         src,
                   const uint32_t *         mask,
                   int                      width)
{
    int i;
    vector unsigned int vdest, vsrc, vmask;
    DECLARE_SRC_MASK_VAR;
    DECLARE_MASK_MASK_VAR;

    while (width && ((uintptr_t)dest & 15))
    {
	uint32_t a = *mask++;
	uint32_t s = *src++;
	uint32_t da = ALPHA_8 (*dest);

	UN8x4_MUL_UN8x4 (s, a);
	UN8x4_MUL_UN8 (s, da);

	*dest++ = s;
	width--;
    }

    COMPUTE_SHIFT_MASKC (dest, src, mask);

    /* printf ("%s\n",__PRETTY_FUNCTION__); */
    for (i = width / 4; i > 0; i--)
    {
	LOAD_VECTORSC (dest, src, mask);

	vdest = pix_multiply (pix_multiply (vsrc, vmask), splat_alpha (vdest));

	STORE_VECTOR (dest);

	src += 4;
	dest += 4;
	mask += 4;
    }

    for (i = width % 4; --i >= 0;)
    {
	uint32_t a = mask[i];
	uint32_t s = src[i];
	uint32_t da = ALPHA_8 (dest[i]);

	UN8x4_MUL_UN8x4 (s, a);
	UN8x4_MUL_UN8 (s, da);

	dest[i] = s;
    }
}

static void
vmx_combine_in_reverse_ca (pixman_implementation_t *imp,
                           pixman_op_t              op,
                           uint32_t *               dest,
                           const uint32_t *         src,
                           const uint32_t *         mask,
                           int                      width)
{
    int i;
    vector unsigned int vdest, vsrc, vmask;
    DECLARE_SRC_MASK_VAR;
    DECLARE_MASK_MASK_VAR;

    while (width && ((uintptr_t)dest & 15))
    {
	uint32_t a = *mask++;
	uint32_t d = *dest;
	uint32_t sa = ALPHA_8 (*src++);

	UN8x4_MUL_UN8 (a, sa);
	UN8x4_MUL_UN8x4 (d, a);

	*dest++ = d;
	width--;
    }

    COMPUTE_SHIFT_MASKC (dest, src, mask);

    /* printf ("%s\n",__PRETTY_FUNCTION__); */
    for (i = width / 4; i > 0; i--)
    {

	LOAD_VECTORSC (dest, src, mask);

	vdest = pix_multiply (vdest, pix_multiply (vmask, splat_alpha (vsrc)));

	STORE_VECTOR (dest);

	src += 4;
	dest += 4;
	mask += 4;
    }

    for (i = width % 4; --i >= 0;)
    {
	uint32_t a = mask[i];
	uint32_t d = dest[i];
	uint32_t sa = ALPHA_8 (src[i]);

	UN8x4_MUL_UN8 (a, sa);
	UN8x4_MUL_UN8x4 (d, a);

	dest[i] = d;
    }
}

static void
vmx_combine_out_ca (pixman_implementation_t *imp,
                    pixman_op_t              op,
                    uint32_t *               dest,
                    const uint32_t *         src,
                    const uint32_t *         mask,
                    int                      width)
{
    int i;
    vector unsigned int vdest, vsrc, vmask;
    DECLARE_SRC_MASK_VAR;
    DECLARE_MASK_MASK_VAR;

    while (width && ((uintptr_t)dest & 15))
    {
	uint32_t a = *mask++;
	uint32_t s = *src++;
	uint32_t d = *dest;
	uint32_t da = ALPHA_8 (~d);

	UN8x4_MUL_UN8x4 (s, a);
	UN8x4_MUL_UN8 (s, da);

	*dest++ = s;
	width--;
    }

    COMPUTE_SHIFT_MASKC (dest, src, mask);

    /* printf ("%s\n",__PRETTY_FUNCTION__); */
    for (i = width / 4; i > 0; i--)
    {
	LOAD_VECTORSC (dest, src, mask);

	vdest = pix_multiply (
	    pix_multiply (vsrc, vmask), splat_alpha (negate (vdest)));

	STORE_VECTOR (dest);

	src += 4;
	dest += 4;
	mask += 4;
    }

    for (i = width % 4; --i >= 0;)
    {
	uint32_t a = mask[i];
	uint32_t s = src[i];
	uint32_t d = dest[i];
	uint32_t da = ALPHA_8 (~d);

	UN8x4_MUL_UN8x4 (s, a);
	UN8x4_MUL_UN8 (s, da);

	dest[i] = s;
    }
}

static void
vmx_combine_out_reverse_ca (pixman_implementation_t *imp,
                            pixman_op_t              op,
                            uint32_t *               dest,
                            const uint32_t *         src,
                            const uint32_t *         mask,
                            int                      width)
{
    int i;
    vector unsigned int vdest, vsrc, vmask;
    DECLARE_SRC_MASK_VAR;
    DECLARE_MASK_MASK_VAR;

    while (width && ((uintptr_t)dest & 15))
    {
	uint32_t a = *mask++;
	uint32_t s = *src++;
	uint32_t d = *dest;
	uint32_t sa = ALPHA_8 (s);

	UN8x4_MUL_UN8 (a, sa);
	UN8x4_MUL_UN8x4 (d, ~a);

	*dest++ = d;
	width--;
    }

    COMPUTE_SHIFT_MASKC (dest, src, mask);

    /* printf ("%s\n",__PRETTY_FUNCTION__); */
    for (i = width / 4; i > 0; i--)
    {
	LOAD_VECTORSC (dest, src, mask);

	vdest = pix_multiply (
	    vdest, negate (pix_multiply (vmask, splat_alpha (vsrc))));

	STORE_VECTOR (dest);

	src += 4;
	dest += 4;
	mask += 4;
    }

    for (i = width % 4; --i >= 0;)
    {
	uint32_t a = mask[i];
	uint32_t s = src[i];
	uint32_t d = dest[i];
	uint32_t sa = ALPHA_8 (s);

	UN8x4_MUL_UN8 (a, sa);
	UN8x4_MUL_UN8x4 (d, ~a);

	dest[i] = d;
    }
}

static void
vmx_combine_atop_ca (pixman_implementation_t *imp,
                     pixman_op_t              op,
                     uint32_t *               dest,
                     const uint32_t *         src,
                     const uint32_t *         mask,
                     int                      width)
{
    int i;
    vector unsigned int vdest, vsrc, vmask, vsrca;
    DECLARE_SRC_MASK_VAR;
    DECLARE_MASK_MASK_VAR;

    while (width && ((uintptr_t)dest & 15))
    {
	uint32_t a = *mask++;
	uint32_t s = *src++;
	uint32_t d = *dest;
	uint32_t sa = ALPHA_8 (s);
	uint32_t da = ALPHA_8 (d);

	UN8x4_MUL_UN8x4 (s, a);
	UN8x4_MUL_UN8 (a, sa);
	UN8x4_MUL_UN8x4_ADD_UN8x4_MUL_UN8 (d, ~a, s, da);

	*dest++ = d;
	width--;
    }

    COMPUTE_SHIFT_MASKC (dest, src, mask);

    /* printf ("%s\n",__PRETTY_FUNCTION__); */
    for (i = width / 4; i > 0; i--)
    {
	LOAD_VECTORSC (dest, src, mask);

	vsrca = splat_alpha (vsrc);

	vsrc = pix_multiply (vsrc, vmask);
	vmask = pix_multiply (vmask, vsrca);

	vdest = pix_add_mul (vsrc, splat_alpha (vdest),
			     negate (vmask), vdest);

	STORE_VECTOR (dest);

	src += 4;
	dest += 4;
	mask += 4;
    }

    for (i = width % 4; --i >= 0;)
    {
	uint32_t a = mask[i];
	uint32_t s = src[i];
	uint32_t d = dest[i];
	uint32_t sa = ALPHA_8 (s);
	uint32_t da = ALPHA_8 (d);

	UN8x4_MUL_UN8x4 (s, a);
	UN8x4_MUL_UN8 (a, sa);
	UN8x4_MUL_UN8x4_ADD_UN8x4_MUL_UN8 (d, ~a, s, da);

	dest[i] = d;
    }
}

static void
vmx_combine_atop_reverse_ca (pixman_implementation_t *imp,
                             pixman_op_t              op,
                             uint32_t *               dest,
                             const uint32_t *         src,
                             const uint32_t *         mask,
                             int                      width)
{
    int i;
    vector unsigned int vdest, vsrc, vmask;
    DECLARE_SRC_MASK_VAR;
    DECLARE_MASK_MASK_VAR;

    while (width && ((uintptr_t)dest & 15))
    {
	uint32_t a = *mask++;
	uint32_t s = *src++;
	uint32_t d = *dest;
	uint32_t sa = ALPHA_8 (s);
	uint32_t da = ALPHA_8 (~d);

	UN8x4_MUL_UN8x4 (s, a);
	UN8x4_MUL_UN8 (a, sa);
	UN8x4_MUL_UN8x4_ADD_UN8x4_MUL_UN8 (d, a, s, da);

	*dest++ = d;
	width--;
    }

    COMPUTE_SHIFT_MASKC (dest, src, mask);

    /* printf ("%s\n",__PRETTY_FUNCTION__); */
    for (i = width / 4; i > 0; i--)
    {
	LOAD_VECTORSC (dest, src, mask);

	vdest = pix_add_mul (vdest,
			     pix_multiply (vmask, splat_alpha (vsrc)),
			     pix_multiply (vsrc, vmask),
			     negate (splat_alpha (vdest)));

	STORE_VECTOR (dest);

	src += 4;
	dest += 4;
	mask += 4;
    }

    for (i = width % 4; --i >= 0;)
    {
	uint32_t a = mask[i];
	uint32_t s = src[i];
	uint32_t d = dest[i];
	uint32_t sa = ALPHA_8 (s);
	uint32_t da = ALPHA_8 (~d);

	UN8x4_MUL_UN8x4 (s, a);
	UN8x4_MUL_UN8 (a, sa);
	UN8x4_MUL_UN8x4_ADD_UN8x4_MUL_UN8 (d, a, s, da);

	dest[i] = d;
    }
}

static void
vmx_combine_xor_ca (pixman_implementation_t *imp,
                    pixman_op_t              op,
                    uint32_t *               dest,
                    const uint32_t *         src,
                    const uint32_t *         mask,
                    int                      width)
{
    int i;
    vector unsigned int vdest, vsrc, vmask;
    DECLARE_SRC_MASK_VAR;
    DECLARE_MASK_MASK_VAR;

    while (width && ((uintptr_t)dest & 15))
    {
	uint32_t a = *mask++;
	uint32_t s = *src++;
	uint32_t d = *dest;
	uint32_t sa = ALPHA_8 (s);
	uint32_t da = ALPHA_8 (~d);

	UN8x4_MUL_UN8x4 (s, a);
	UN8x4_MUL_UN8 (a, sa);
	UN8x4_MUL_UN8x4_ADD_UN8x4_MUL_UN8 (d, ~a, s, da);

	*dest++ = d;
	width--;
    }

    COMPUTE_SHIFT_MASKC (dest, src, mask);

    /* printf ("%s\n",__PRETTY_FUNCTION__); */
    for (i = width / 4; i > 0; i--)
    {
	LOAD_VECTORSC (dest, src, mask);

	vdest = pix_add_mul (vdest,
			     negate (pix_multiply (vmask, splat_alpha (vsrc))),
			     pix_multiply (vsrc, vmask),
			     negate (splat_alpha (vdest)));

	STORE_VECTOR (dest);

	src += 4;
	dest += 4;
	mask += 4;
    }

    for (i = width % 4; --i >= 0;)
    {
	uint32_t a = mask[i];
	uint32_t s = src[i];
	uint32_t d = dest[i];
	uint32_t sa = ALPHA_8 (s);
	uint32_t da = ALPHA_8 (~d);

	UN8x4_MUL_UN8x4 (s, a);
	UN8x4_MUL_UN8 (a, sa);
	UN8x4_MUL_UN8x4_ADD_UN8x4_MUL_UN8 (d, ~a, s, da);

	dest[i] = d;
    }
}

static void
vmx_combine_add_ca (pixman_implementation_t *imp,
                    pixman_op_t              op,
                    uint32_t *               dest,
                    const uint32_t *         src,
                    const uint32_t *         mask,
                    int                      width)
{
    int i;
    vector unsigned int vdest, vsrc, vmask;
    DECLARE_SRC_MASK_VAR;
    DECLARE_MASK_MASK_VAR;

    while (width && ((uintptr_t)dest & 15))
    {
	uint32_t a = *mask++;
	uint32_t s = *src++;
	uint32_t d = *dest;

	UN8x4_MUL_UN8x4 (s, a);
	UN8x4_ADD_UN8x4 (s, d);

	*dest++ = s;
	width--;
    }

    COMPUTE_SHIFT_MASKC (dest, src, mask);

    /* printf ("%s\n",__PRETTY_FUNCTION__); */
    for (i = width / 4; i > 0; i--)
    {
	LOAD_VECTORSC (dest, src, mask);

	vdest = pix_add (pix_multiply (vsrc, vmask), vdest);

	STORE_VECTOR (dest);

	src += 4;
	dest += 4;
	mask += 4;
    }

    for (i = width % 4; --i >= 0;)
    {
	uint32_t a = mask[i];
	uint32_t s = src[i];
	uint32_t d = dest[i];

	UN8x4_MUL_UN8x4 (s, a);
	UN8x4_ADD_UN8x4 (s, d);

	dest[i] = s;
    }
}

static pixman_bool_t
vmx_fill (pixman_implementation_t *imp,
           uint32_t *               bits,
           int                      stride,
           int                      bpp,
           int                      x,
           int                      y,
           int                      width,
           int                      height,
           uint32_t		    filler)
{
    uint32_t byte_width;
    uint8_t *byte_line;

    vector unsigned int vfiller;

    if (bpp == 8)
    {
	uint8_t b;
	uint16_t w;

	stride = stride * (int) sizeof (uint32_t) / 1;
	byte_line = (uint8_t *)(((uint8_t *)bits) + stride * y + x);
	byte_width = width;
	stride *= 1;

	b = filler & 0xff;
	w = (b << 8) | b;
	filler = (w << 16) | w;
    }
    else if (bpp == 16)
    {
	stride = stride * (int) sizeof (uint32_t) / 2;
	byte_line = (uint8_t *)(((uint16_t *)bits) + stride * y + x);
	byte_width = 2 * width;
	stride *= 2;

        filler = (filler & 0xffff) * 0x00010001;
    }
    else if (bpp == 32)
    {
	stride = stride * (int) sizeof (uint32_t) / 4;
	byte_line = (uint8_t *)(((uint32_t *)bits) + stride * y + x);
	byte_width = 4 * width;
	stride *= 4;
    }
    else
    {
	return FALSE;
    }

    vfiller = create_mask_1x32_128(&filler);

    while (height--)
    {
	int w;
	uint8_t *d = byte_line;
	byte_line += stride;
	w = byte_width;

	if (w >= 1 && ((uintptr_t)d & 1))
	{
	    *(uint8_t *)d = filler;
	    w -= 1;
	    d += 1;
	}

	while (w >= 2 && ((uintptr_t)d & 3))
	{
	    *(uint16_t *)d = filler;
	    w -= 2;
	    d += 2;
	}

	while (w >= 4 && ((uintptr_t)d & 15))
	{
	    *(uint32_t *)d = filler;

	    w -= 4;
	    d += 4;
	}

	while (w >= 128)
	{
	    vec_st(vfiller, 0, (uint32_t *) d);
	    vec_st(vfiller, 0, (uint32_t *) d + 4);
	    vec_st(vfiller, 0, (uint32_t *) d + 8);
	    vec_st(vfiller, 0, (uint32_t *) d + 12);
	    vec_st(vfiller, 0, (uint32_t *) d + 16);
	    vec_st(vfiller, 0, (uint32_t *) d + 20);
	    vec_st(vfiller, 0, (uint32_t *) d + 24);
	    vec_st(vfiller, 0, (uint32_t *) d + 28);

	    d += 128;
	    w -= 128;
	}

	if (w >= 64)
	{
	    vec_st(vfiller, 0, (uint32_t *) d);
	    vec_st(vfiller, 0, (uint32_t *) d + 4);
	    vec_st(vfiller, 0, (uint32_t *) d + 8);
	    vec_st(vfiller, 0, (uint32_t *) d + 12);

	    d += 64;
	    w -= 64;
	}

	if (w >= 32)
	{
	    vec_st(vfiller, 0, (uint32_t *) d);
	    vec_st(vfiller, 0, (uint32_t *) d + 4);

	    d += 32;
	    w -= 32;
	}

	if (w >= 16)
	{
	    vec_st(vfiller, 0, (uint32_t *) d);

	    d += 16;
	    w -= 16;
	}

	while (w >= 4)
	{
	    *(uint32_t *)d = filler;

	    w -= 4;
	    d += 4;
	}

	if (w >= 2)
	{
	    *(uint16_t *)d = filler;
	    w -= 2;
	    d += 2;
	}

	if (w >= 1)
	{
	    *(uint8_t *)d = filler;
	    w -= 1;
	    d += 1;
	}
    }

    return TRUE;
}

static const pixman_fast_path_t vmx_fast_paths[] =
{
    {   PIXMAN_OP_NONE	},
};

pixman_implementation_t *
_pixman_implementation_create_vmx (pixman_implementation_t *fallback)
{
    pixman_implementation_t *imp = _pixman_implementation_create (fallback, vmx_fast_paths);

    /* VMX constants */
    mask_00ff = create_mask_16_128 (0x00ff);
    mask_ff000000 = create_mask_32_128 (0xff000000);
    mask_red   = create_mask_32_128 (0x00f80000);
    mask_green = create_mask_32_128 (0x0000fc00);
    mask_blue  = create_mask_32_128 (0x000000f8);
    mask_565_fix_rb = create_mask_32_128 (0x00e000e0);
    mask_565_fix_g = create_mask_32_128  (0x0000c000);

    /* Set up function pointers */

    imp->combine_32[PIXMAN_OP_OVER] = vmx_combine_over_u;
    imp->combine_32[PIXMAN_OP_OVER_REVERSE] = vmx_combine_over_reverse_u;
    imp->combine_32[PIXMAN_OP_IN] = vmx_combine_in_u;
    imp->combine_32[PIXMAN_OP_IN_REVERSE] = vmx_combine_in_reverse_u;
    imp->combine_32[PIXMAN_OP_OUT] = vmx_combine_out_u;
    imp->combine_32[PIXMAN_OP_OUT_REVERSE] = vmx_combine_out_reverse_u;
    imp->combine_32[PIXMAN_OP_ATOP] = vmx_combine_atop_u;
    imp->combine_32[PIXMAN_OP_ATOP_REVERSE] = vmx_combine_atop_reverse_u;
    imp->combine_32[PIXMAN_OP_XOR] = vmx_combine_xor_u;

    imp->combine_32[PIXMAN_OP_ADD] = vmx_combine_add_u;

    imp->combine_32_ca[PIXMAN_OP_SRC] = vmx_combine_src_ca;
    imp->combine_32_ca[PIXMAN_OP_OVER] = vmx_combine_over_ca;
    imp->combine_32_ca[PIXMAN_OP_OVER_REVERSE] = vmx_combine_over_reverse_ca;
    imp->combine_32_ca[PIXMAN_OP_IN] = vmx_combine_in_ca;
    imp->combine_32_ca[PIXMAN_OP_IN_REVERSE] = vmx_combine_in_reverse_ca;
    imp->combine_32_ca[PIXMAN_OP_OUT] = vmx_combine_out_ca;
    imp->combine_32_ca[PIXMAN_OP_OUT_REVERSE] = vmx_combine_out_reverse_ca;
    imp->combine_32_ca[PIXMAN_OP_ATOP] = vmx_combine_atop_ca;
    imp->combine_32_ca[PIXMAN_OP_ATOP_REVERSE] = vmx_combine_atop_reverse_ca;
    imp->combine_32_ca[PIXMAN_OP_XOR] = vmx_combine_xor_ca;
    imp->combine_32_ca[PIXMAN_OP_ADD] = vmx_combine_add_ca;

    imp->fill = vmx_fill;

    return imp;
}
