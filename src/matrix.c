/*
 * Copyright © 2011 Intel Corporation
 * Copyright © 2012 Collabora, Ltd.
 *
 * Permission to use, copy, modify, distribute, and sell this software and
 * its documentation for any purpose is hereby granted without fee, provided
 * that the above copyright notice appear in all copies and that both that
 * copyright notice and this permission notice appear in supporting
 * documentation, and that the name of the copyright holders not be used in
 * advertising or publicity pertaining to distribution of the software
 * without specific, written prior permission.  The copyright holders make
 * no representations about the suitability of this software for any
 * purpose.  It is provided "as is" without express or implied warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS
 * SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS, IN NO EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER
 * RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF
 * CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <GLES2/gl2.h>
#include <wayland-server.h>

#include "matrix.h"

/*
 * Matrices are stored in column-major order, that is the array indices are:
 *  0  4  8 12
 *  1  5  9 13
 *  2  6 10 14
 *  3  7 11 15
 */

WL_EXPORT void
weston_matrix_init(struct weston_matrix *matrix)
{
	static const struct weston_matrix identity = {
		{ 1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,  0, 0, 0, 1 }
	};

	memcpy(matrix, &identity, sizeof identity);
}

/* m <- n * m, that is, m is multiplied on the LEFT. */
WL_EXPORT void
weston_matrix_multiply(struct weston_matrix *m, const struct weston_matrix *n)
{
	struct weston_matrix tmp;
	const GLfloat *row, *column;
	div_t d;
	int i, j;

	for (i = 0; i < 16; i++) {
		tmp.d[i] = 0;
		d = div(i, 4);
		row = m->d + d.quot * 4;
		column = n->d + d.rem;
		for (j = 0; j < 4; j++)
			tmp.d[i] += row[j] * column[j * 4];
	}
	memcpy(m, &tmp, sizeof tmp);
}

WL_EXPORT void
weston_matrix_translate(struct weston_matrix *matrix, GLfloat x, GLfloat y, GLfloat z)
{
	struct weston_matrix translate = {
		{ 1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,  x, y, z, 1 }
	};

	weston_matrix_multiply(matrix, &translate);
}

WL_EXPORT void
weston_matrix_scale(struct weston_matrix *matrix, GLfloat x, GLfloat y, GLfloat z)
{
	struct weston_matrix scale = {
		{ x, 0, 0, 0,  0, y, 0, 0,  0, 0, z, 0,  0, 0, 0, 1 }
	};

	weston_matrix_multiply(matrix, &scale);
}

/* v <- m * v */
WL_EXPORT void
weston_matrix_transform(struct weston_matrix *matrix, struct weston_vector *v)
{
	int i, j;
	struct weston_vector t;

	for (i = 0; i < 4; i++) {
		t.f[i] = 0;
		for (j = 0; j < 4; j++)
			t.f[i] += v->f[j] * matrix->d[i + j * 4];
	}

	*v = t;
}

static inline void
swap_rows(double *a, double *b)
{
	unsigned k;
	double tmp;

	for (k = 0; k < 13; k += 4) {
		tmp = a[k];
		a[k] = b[k];
		b[k] = tmp;
	}
}

static inline unsigned
find_pivot(double *column, unsigned k)
{
	unsigned p = k;
	for (++k; k < 4; ++k)
		if (fabs(column[p]) < fabs(column[k]))
			p = k;

	return p;
}

/*
 * reference: Gene H. Golub and Charles F. van Loan. Matrix computations.
 * 3rd ed. The Johns Hopkins University Press. 1996.
 * LU decomposition, forward and back substitution: Chapter 3.
 */

WL_EXPORT int
weston_matrix_invert(struct weston_inverse_matrix *inverse,
		     const struct weston_matrix *matrix)
{
	double A[16];
	unsigned p[4] = { 0, 1, 2, 3 };
	unsigned i, j, k;
	unsigned pivot;
	double pv;

	for (i = 16; i--; )
		A[i] = matrix->d[i];

	/* LU decomposition with partial pivoting */
	for (k = 0; k < 4; ++k) {
		pivot = find_pivot(&A[k * 4], k);
		if (pivot != k) {
			unsigned tmp = p[k];
			p[k] = p[pivot];
			p[pivot] = tmp;
			swap_rows(&A[k], &A[pivot]);
		}

		pv = A[k * 4 + k];
		if (fabs(pv) < 1e-9)
			return -1; /* zero pivot, not invertible */

		for (i = k + 1; i < 4; ++i) {
			A[i + k * 4] /= pv;

			for (j = k + 1; j < 4; ++j)
				A[i + j * 4] -= A[i + k * 4] * A[k + j * 4];
		}
	}

	memcpy(inverse->LU, A, sizeof(A));
	memcpy(inverse->p, p, sizeof(p));
	return 0;
}

WL_EXPORT void
weston_matrix_inverse_transform(struct weston_inverse_matrix *inverse,
				struct weston_vector *v)
{
	/* Solve A * x = v, when we have P * A = L * U.
	 * P * A * x = P * v  =>  L * U * x = P * v
	 * Let U * x = b, then L * b = P * v.
	 */
	unsigned *p = inverse->p;
	double *LU = inverse->LU;
	double b[4];
	unsigned k, j;

	/* Forward substitution, column version, solves L * b = P * v */
	/* The diagonal of L is all ones, and not explicitly stored. */
	b[0] = v->f[p[0]];
	b[1] = (double)v->f[p[1]] - b[0] * LU[1 + 0 * 4];
	b[2] = (double)v->f[p[2]] - b[0] * LU[2 + 0 * 4];
	b[3] = (double)v->f[p[3]] - b[0] * LU[3 + 0 * 4];
	b[2] -= b[1] * LU[2 + 1 * 4];
	b[3] -= b[1] * LU[3 + 1 * 4];
	b[3] -= b[2] * LU[3 + 2 * 4];

	/* backward substitution, column version, solves U * y = b */
#if 1
	/* hand-unrolled, 25% faster for whole function */
	b[3] /= LU[3 + 3 * 4];
	b[0] -= b[3] * LU[0 + 3 * 4];
	b[1] -= b[3] * LU[1 + 3 * 4];
	b[2] -= b[3] * LU[2 + 3 * 4];

	b[2] /= LU[2 + 2 * 4];
	b[0] -= b[2] * LU[0 + 2 * 4];
	b[1] -= b[2] * LU[1 + 2 * 4];

	b[1] /= LU[1 + 1 * 4];
	b[0] -= b[1] * LU[0 + 1 * 4];

	b[0] /= LU[0 + 0 * 4];
#else
	for (j = 3; j > 0; --j) {
		b[j] /= LU[j + j * 4];
		for (k = 0; k < j; ++k)
			b[k] -= b[j] * LU[k + j * 4];
	}

	b[0] /= LU[0 + 0 * 4];
#endif

	/* the result */
	for (j = 0; j < 4; ++j)
		v->f[j] = b[j];
}
