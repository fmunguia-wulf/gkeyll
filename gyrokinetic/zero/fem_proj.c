#include <gkyl_fem_proj_priv.h>
#include <gkyl_alloc.h>
#include <gkyl_array_ops.h>
#include <limits.h>
#include <string.h>

// Validate parent-array addressing as well as host/device placement.
static void
fem_proj_check_array(const struct gkyl_fem_proj *up, const struct gkyl_array *arr)
{
  if (!arr || arr->type != GKYL_DOUBLE || arr->ncomp != up->basis.num_basis ||
      arr->size < up->expect_arr_sz || gkyl_array_is_cu_dev(arr) != up->use_gpu) {
    gkyl_exit("fem_proj: incompatible DG array layout or memory location.");
  }
}

// Count global FEM nodes and check size limits before constructing the matrix.
static void
fem_proj_init_num_nodes(struct gkyl_fem_proj *up)
{
  int ndim = up->grid.ndim;
  bool periodic[GKYL_MAX_CDIM];
  // Bound the node count before evaluating the generated count formula.
  long max_nodes = 1;
  for (int d = 0; d < ndim; d++) {
    periodic[d] = (up->periodic_mask & (1 << d)) != 0;
    long extent = (long)up->basis.poly_order * up->num_cells[d] + 1;
    if (max_nodes > LONG_MAX / extent) {
      gkyl_exit("fem_proj: node grid is too large.");
    }
    max_nodes *= extent;
  }

  up->num_nodes = up->kernels.num_nodes(up->num_cells, periodic);

  if (up->num_nodes <= 0 || up->num_nodes > INT_MAX) {
    gkyl_exit("fem_proj: unsupported matrix size.");
  }
}

// Matrix construction is on the host, including for a GPU solve.
static void
fem_proj_init_matrix(struct gkyl_fem_proj *up)
{
  struct gkyl_array *weight = NULL;
  if (up->weight_left) {
    weight = gkyl_array_new(GKYL_DOUBLE, up->basis.num_basis, up->weight_left->size);
    gkyl_array_copy(weight, up->weight_left);
  }

  struct gkyl_mat_triples *matrix = gkyl_mat_triples_new(up->num_nodes, up->num_nodes);
  if (up->use_gpu) {
    gkyl_mat_triples_set_rowmaj_order(matrix);
  }

  struct gkyl_range_iter iter;
  gkyl_range_iter_init(&iter, &up->range);
  while (gkyl_range_iter_next(&iter)) {
    long loc = gkyl_range_idx(&up->range, iter.idx);

    long global_idx[FEM_PROJ_MAX_NODES];
    fem_proj_global_indices(&up->kernels, &up->range, up->num_cells, iter.idx, global_idx);

    // Uniform cell volume factor cancels from the unconstrained equations.
    int location = fem_proj_cell_location(&up->range, iter.idx);
    up->kernels.mass[location](weight ? gkyl_array_cfetch(weight, loc) : NULL, global_idx, matrix);
  }

#ifdef GKYL_HAVE_CUDA
  if (up->use_gpu) {
    up->prob_cu = gkyl_culinsolver_prob_new(1, up->num_nodes, up->num_nodes, 1);
    gkyl_culinsolver_amat_from_triples(up->prob_cu, &matrix);
  } else
#endif
  {
    up->prob = gkyl_superlu_prob_new(1, up->num_nodes, up->num_nodes, 1);
    gkyl_superlu_amat_from_triples(up->prob, &matrix);
    gkyl_superlu_ludecomp(up->prob);
    up->rhs = gkyl_malloc(up->num_nodes * sizeof(double));
  }

  gkyl_mat_triples_release(matrix);
  if (weight) {
    gkyl_array_release(weight);
  }
}

gkyl_fem_proj *
gkyl_fem_proj_inew(const struct gkyl_fem_proj_inp *inp)
{
  int ndim = inp->grid->ndim;

  // Consistency checks.
  if (!inp || !inp->grid || !inp->basis || !inp->solve_range) {
    gkyl_exit("fem_proj: grid, basis and solve range are required.");
  }
  if (ndim == 3) {
    gkyl_exit("fem_proj: 3D kernels have not been generated yet.");
  }
  if (ndim < 1 || ndim > 2 || inp->basis->ndim != ndim || inp->solve_range->ndim != ndim ||
      inp->basis->b_type != GKYL_BASIS_MODAL_SERENDIPITY ||
      (inp->basis->poly_order != 1 && inp->basis->poly_order != 2)) {
    gkyl_exit("fem_proj: requires 1D/2D serendipity p1/p2.");
  }
  for (int d = 0; d < ndim; d++) {
    if (inp->solve_range->upper[d] < inp->solve_range->lower[d]) {
      gkyl_exit("fem_proj: empty solve range.");
    }
    int lo = inp->bcs.lo_type[d], hi = inp->bcs.up_type[d];
    if (lo < GKYL_FEM_PROJ_NONE || lo > GKYL_FEM_PROJ_DIRICHLET_VARYING ||
        hi < GKYL_FEM_PROJ_NONE || hi > GKYL_FEM_PROJ_DIRICHLET_VARYING ||
        ((lo == GKYL_FEM_PROJ_PERIODIC) != (hi == GKYL_FEM_PROJ_PERIODIC))) {
      gkyl_exit("fem_proj: invalid or unpaired periodic boundary condition.");
    }
    long count = (long)inp->solve_range->upper[d] - inp->solve_range->lower[d] + 1;
    if (count > INT_MAX) {
      gkyl_exit("fem_proj: too many cells in a direction.");
    }
  }
#ifndef GKYL_HAVE_CUDA
  if (inp->use_gpu) {
    gkyl_exit("fem_proj: CUDA support is not compiled.");
  }
#endif

  // Allocate updater and copy some inputs.
  struct gkyl_fem_proj *up = gkyl_malloc(sizeof(*up));

  *up = (struct gkyl_fem_proj){
    .grid = *inp->grid,
    .range = *inp->solve_range,
    .basis = *inp->basis,
    .bcs = inp->bcs,
    .use_gpu = inp->use_gpu,
  };

  for (int d = 0; d < ndim; d++) {
    int lo = inp->bcs.lo_type[d], hi = inp->bcs.up_type[d];
    up->varying |= lo == GKYL_FEM_PROJ_DIRICHLET_VARYING || hi == GKYL_FEM_PROJ_DIRICHLET_VARYING;
    
    long count = (long)up->range.upper[d] - up->range.lower[d] + 1;
    up->num_cells[d] = count;
    up->bc_values[2 * d] = inp->bcs.lo_value[d];
    up->bc_values[2 * d + 1] = inp->bcs.up_value[d];
    up->periodic_mask |= (lo == GKYL_FEM_PROJ_PERIODIC) << d;
  }

  // Expected size of arrays (e.g. weights, RHS source array).
  up->expect_arr_sz = gkyl_range_idx(&up->range, up->range.upper) + 1;

  if (inp->weight_left) {
    fem_proj_check_array(up, inp->weight_left);
    up->weight_left = gkyl_array_acquire(inp->weight_left);
  }
  if (inp->weight_right) {
    fem_proj_check_array(up, inp->weight_right);
    up->weight_right = gkyl_array_acquire(inp->weight_right);
  }

  up->kernels = fem_proj_choose_kernels(
    ndim, up->basis.poly_order, up->weight_left != NULL, up->weight_right != NULL,
    up->periodic_mask, &up->bcs
  );

  fem_proj_init_num_nodes(up);

  fem_proj_init_matrix(up);

#ifdef GKYL_HAVE_CUDA
  if (up->use_gpu) {
    fem_proj_init_cu(up);
  }
#endif

  return up;
}

void
gkyl_fem_proj_set_rhs(
  gkyl_fem_proj *up, const struct gkyl_array *rhsin, const struct gkyl_array *boundary_values
)
{
  // Consistency check.
  fem_proj_check_array(up, rhsin);
  if (up->varying) {
    fem_proj_check_array(up, boundary_values);
  }

  up->rhs_ready = true;
#ifdef GKYL_HAVE_CUDA
  if (up->use_gpu) {
    fem_proj_set_rhs_cu(up, rhsin, boundary_values);
    return;
  }
#endif

  // Zero out rhs vector.
  memset(up->rhs, 0, up->num_nodes * sizeof(double));

  struct gkyl_range_iter iter;
  gkyl_range_iter_init(&iter, &up->range);
  while (gkyl_range_iter_next(&iter)) {
    long loc = gkyl_range_idx(&up->range, iter.idx);

    long global_idx[FEM_PROJ_MAX_NODES];
    fem_proj_global_indices(&up->kernels, &up->range, up->num_cells, iter.idx, global_idx);

    int location = fem_proj_cell_location(&up->range, iter.idx);
    up->kernels.src[location](
      up->weight_right ? gkyl_array_cfetch(up->weight_right, loc) : NULL,
      gkyl_array_cfetch(rhsin, loc), up->bc_values,
      up->varying ? gkyl_array_cfetch(boundary_values, loc) : NULL, global_idx, up->rhs
    );
  }

  gkyl_superlu_brhs_from_array(up->prob, up->rhs);
}

void
gkyl_fem_proj_solve(gkyl_fem_proj *up, struct gkyl_array *out)
{
  // Consistency check.
  fem_proj_check_array(up, out);
  if (!up->rhs_ready) {
    gkyl_exit("fem_proj: set_rhs must precede solve.");
  }

  up->rhs_ready = false;

  // Solve linear problem.
#ifdef GKYL_HAVE_CUDA
  if (up->use_gpu) {
    fem_proj_solve_cu(up, out);
    return;
  }
#endif

  gkyl_superlu_solve(up->prob);
  const double *solution = gkyl_superlu_get_rhs_ptr(up->prob, 0);

  struct gkyl_range_iter iter;
  gkyl_range_iter_init(&iter, &up->range);
  while (gkyl_range_iter_next(&iter)) {
    long loc = gkyl_range_idx(&up->range, iter.idx);

    long global_idx[FEM_PROJ_MAX_NODES];
    fem_proj_global_indices(&up->kernels, &up->range, up->num_cells, iter.idx, global_idx);

    double nodal[FEM_PROJ_MAX_NODES];
    for (int n = 0; n < up->basis.num_basis; n++) {
      nodal[n] = solution[global_idx[n]];
    }
    up->kernels.sol(nodal, gkyl_array_fetch(out, loc));
  }
}

void
gkyl_fem_proj_release(gkyl_fem_proj *up)
{
#ifdef GKYL_HAVE_CUDA
  if (up->use_gpu) {
    gkyl_culinsolver_prob_release(up->prob_cu);
    gkyl_cu_free(up->kernels_cu);
    gkyl_cu_free(up->bc_values_cu);
  } else
#endif
  {
    gkyl_superlu_prob_release(up->prob);
    gkyl_free(up->rhs);
  }
  if (up->weight_left) {
    gkyl_array_release(up->weight_left);
  }
  if (up->weight_right) {
    gkyl_array_release(up->weight_right);
  }
  gkyl_free(up);
}
