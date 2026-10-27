extern "C" {
#include <gkyl_fem_proj_priv.h>
#include <gkyl_alloc.h>
#include <gkyl_array_ops.h>
}

__global__ static void
fem_proj_set_kernels(struct fem_proj_kernels *kernels, int ndim, int order, bool left, bool right)
{
  *kernels = fem_proj_choose_kernels(ndim, order, left, right);
}

void
fem_proj_init_cu(struct gkyl_fem_proj *up)
{
  struct gkyl_array *map = gkyl_array_cu_dev_new(GKYL_LONG, up->map->ncomp, up->map->size);
  gkyl_array_copy(map, up->map);
  gkyl_array_release(up->map);
  up->map = map;
  struct fem_proj_boundary *boundary = NULL;
  if (up->num_boundary) {
    size_t size = up->num_boundary * sizeof(*boundary);
    boundary = (struct fem_proj_boundary *)gkyl_cu_malloc(size);
    gkyl_cu_memcpy(boundary, up->boundary, size, GKYL_CU_MEMCPY_H2D);
  }
  gkyl_free(up->boundary);
  up->boundary = boundary;
  up->kernels_cu = (struct fem_proj_kernels *)gkyl_cu_malloc(sizeof(*up->kernels_cu));
  fem_proj_set_kernels<<<1, 1>>>(
    up->kernels_cu, up->grid.ndim, up->basis.poly_order, up->weight_left != NULL,
    up->weight_right != NULL
  );
}

__global__ static void
fem_proj_load_kernel(
  struct gkyl_range range, const struct fem_proj_kernels *kernels, const struct gkyl_array *map,
  const struct gkyl_array *weight, const struct gkyl_array *input, double *rhs, int nb
)
{
  for (long cell = threadIdx.x + blockIdx.x * blockDim.x; cell < range.volume;
       cell += blockDim.x * gridDim.x) {
    int idx[GKYL_MAX_CDIM];
    gkyl_sub_range_inv_idx(&range, cell, idx);
    long loc = gkyl_range_idx(&range, idx);
    const long *ids = (const long *)gkyl_array_cfetch(map, loc);
    double load[FEM_PROJ_MAX_NODES];
    kernels->src(
      weight ? (const double *)gkyl_array_cfetch(weight, loc) : NULL,
      (const double *)gkyl_array_cfetch(input, loc), load
    );
    for (int n = 0; n < nb; n++) {
      atomicAdd(&rhs[ids[n]], load[n]);
    }
  }
}

__global__ static void
fem_proj_boundary_kernel(
  long count, const struct fem_proj_boundary *boundary, const struct fem_proj_kernels *kernels,
  const struct gkyl_array *values, double *rhs
)
{
  for (long n = threadIdx.x + blockIdx.x * blockDim.x; n < count; n += blockDim.x * gridDim.x) {
    const struct fem_proj_boundary bc = boundary[n];
    double nodal[FEM_PROJ_MAX_NODES];
    if (bc.varying) {
      kernels->eval((const double *)gkyl_array_cfetch(values, bc.cell), nodal);
    }
    rhs[bc.global] = bc.varying ? nodal[bc.node] : bc.value;
  }
}

__global__ static void
fem_proj_solution_kernel(
  struct gkyl_range range, const struct fem_proj_kernels *kernels, const struct gkyl_array *map,
  const double *solution, struct gkyl_array *out, int nb
)
{
  for (long cell = threadIdx.x + blockIdx.x * blockDim.x; cell < range.volume;
       cell += blockDim.x * gridDim.x) {
    int idx[GKYL_MAX_CDIM];
    gkyl_sub_range_inv_idx(&range, cell, idx);
    long loc = gkyl_range_idx(&range, idx);
    const long *ids = (const long *)gkyl_array_cfetch(map, loc);
    double nodal[FEM_PROJ_MAX_NODES];
    for (int n = 0; n < nb; n++) {
      nodal[n] = solution[ids[n]];
    }
    kernels->sol(nodal, (double *)gkyl_array_fetch(out, loc));
  }
}

void
fem_proj_set_rhs_cu(
  struct gkyl_fem_proj *up, const struct gkyl_array *rhs, const struct gkyl_array *boundary_values
)
{
  gkyl_culinsolver_clear_rhs(up->prob_cu, 0.0);
  double *load = gkyl_culinsolver_get_rhs_ptr(up->prob_cu, 0);
  fem_proj_load_kernel<<<up->range.nblocks, up->range.nthreads>>>(
    up->range, up->kernels_cu, up->map->on_dev, up->weight_right ? up->weight_right->on_dev : NULL,
    rhs->on_dev, load, up->basis.num_basis
  );
  if (up->num_boundary) {
    fem_proj_boundary_kernel<<<(up->num_boundary + 127) / 128, 128>>>(
      up->num_boundary, up->boundary, up->kernels_cu,
      boundary_values ? boundary_values->on_dev : NULL, load
    );
  }
}

void
fem_proj_solve_cu(struct gkyl_fem_proj *up, struct gkyl_array *out)
{
  gkyl_culinsolver_solve(up->prob_cu);
  fem_proj_solution_kernel<<<up->range.nblocks, up->range.nthreads>>>(
    up->range, up->kernels_cu, up->map->on_dev, gkyl_culinsolver_get_sol_ptr(up->prob_cu, 0),
    out->on_dev, up->basis.num_basis
  );
}
