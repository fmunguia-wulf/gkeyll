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

// Compact grid node indices into shared FEM node numbers; retain one BC owner per node.
static void
fem_proj_init_nodes(struct gkyl_fem_proj *up)
{
  int ndim = up->grid.ndim, order = up->basis.poly_order, nb = up->basis.num_basis;
  int cells[GKYL_MAX_CDIM], shape[GKYL_MAX_CDIM];
  double nodes[FEM_PROJ_MAX_NODES * GKYL_MAX_CDIM];

  up->basis.node_list(nodes);

  bool periodic[GKYL_MAX_CDIM];
  long grid_size = 1;
  for (int d = 0; d < ndim; d++) {
    cells[d] = up->range.upper[d] - up->range.lower[d] + 1;
    periodic[d] = up->bcs.lo_type[d] == GKYL_FEM_PROJ_PERIODIC;
    shape[d] = order * cells[d] + !periodic[d];
    if (shape[d] <= 0 || grid_size > LONG_MAX / shape[d]) {
      gkyl_exit("fem_proj: node grid is too large.");
    }
    grid_size *= shape[d];
  }
  void (*local_to_global)(const int *, const int *, long *) = NULL;

#define FEM_PROJ_NODES(D, P)                                               \
  if (ndim == D && order == P) {                                           \
    up->num_nodes = fem_proj_##D##x_ser_p##P##_num_nodes(cells, periodic); \
    local_to_global = fem_proj_##D##x_ser_p##P##_local_to_global;          \
  }
  FEM_PROJ_NODES(1, 1)
  FEM_PROJ_NODES(1, 2)
  FEM_PROJ_NODES(2, 1)
  FEM_PROJ_NODES(2, 2)
#undef FEM_PROJ_NODES
  if (up->num_nodes <= 0 || up->num_nodes > INT_MAX) {
    gkyl_exit("fem_proj: unsupported matrix size.");
  }
  long *number = gkyl_malloc(grid_size * sizeof(long));
  for (long i = 0; i < grid_size; i++) {
    number[i] = -1;
  }
  long *owner = gkyl_malloc(up->num_nodes * sizeof(long));
  for (long i = 0; i < up->num_nodes; i++) {
    owner[i] = -1;
  }
  struct fem_proj_boundary *boundary = gkyl_malloc(up->num_nodes * sizeof(*boundary));
  struct gkyl_array *map = gkyl_array_new(GKYL_LONG, nb, up->expect_arr_sz);
  long next = 0;
  struct gkyl_range_iter iter;
  gkyl_range_iter_init(&iter, &up->range);
  while (gkyl_range_iter_next(&iter)) {
    int idx[GKYL_MAX_CDIM];
    for (int d = 0; d < ndim; d++) {
      idx[d] = iter.idx[d] - up->range.lower[d];
    }
    long global_idx[FEM_PROJ_MAX_NODES];
    local_to_global(idx, shape, global_idx);
    long loc = gkyl_range_idx(&up->range, iter.idx);
    long *ids = gkyl_array_fetch(map, loc);
    for (int n = 0; n < nb; n++) {
      if (number[global_idx[n]] == -1) {
        number[global_idx[n]] = next++;
      }
      long global = ids[n] = number[global_idx[n]];
      for (int d = 0; d < ndim; d++) {
        // A boundary node lies on the corresponding face of a boundary cell.
        bool lower = idx[d] == 0 && nodes[n * ndim + d] == -1.0;
        bool upper = idx[d] == cells[d] - 1 && nodes[n * ndim + d] == 1.0;
        if (!lower && !upper) {
          continue;
        }
        enum gkyl_fem_proj_bc_type type = lower ? up->bcs.lo_type[d] : up->bcs.up_type[d];
        if ((type == GKYL_FEM_PROJ_DIRICHLET || type == GKYL_FEM_PROJ_DIRICHLET_VARYING) &&
            owner[global] == -1) {
          owner[global] = up->num_boundary;
          boundary[up->num_boundary++] = (struct fem_proj_boundary){
            .global = global,
            .cell = loc,
            .node = n,
            .varying = type == GKYL_FEM_PROJ_DIRICHLET_VARYING,
            .value = lower ? up->bcs.lo_value[d] : up->bcs.up_value[d],
          };
        }
      }
    }
  }
  assert(next == up->num_nodes);
  up->map = map;
  up->boundary = boundary;
  gkyl_free(owner);
  gkyl_free(number);
}

// Matrix construction is on the host, including for a GPU solve.
static void
fem_proj_init_matrix(struct gkyl_fem_proj *up)
{
  bool *constrained = gkyl_malloc(up->num_nodes * sizeof(bool));
  memset(constrained, 0, up->num_nodes * sizeof(bool));
  for (long n = 0; n < up->num_boundary; n++) {
    constrained[up->boundary[n].global] = true;
  }
  struct gkyl_array *weight = NULL;
  if (up->weight_left) {
    weight = gkyl_array_new(GKYL_DOUBLE, up->basis.num_basis, up->weight_left->size);
    gkyl_array_copy(weight, up->weight_left);
  }
  struct gkyl_mat_triples *matrix = gkyl_mat_triples_new(up->num_nodes, up->num_nodes);
  if (up->use_gpu) {
    gkyl_mat_triples_set_rowmaj_order(matrix);
  }
  int nb = up->basis.num_basis;
  double mass[FEM_PROJ_MAX_NODES * FEM_PROJ_MAX_NODES];
  struct gkyl_range_iter iter;
  gkyl_range_iter_init(&iter, &up->range);
  while (gkyl_range_iter_next(&iter)) {
    long loc = gkyl_range_idx(&up->range, iter.idx);
    const long *ids = gkyl_array_cfetch(up->map, loc);
    up->kernels.mass(weight ? gkyl_array_cfetch(weight, loc) : NULL, mass);
    // Uniform Cartesian cell Jacobian cancels from the unconstrained equations.
    for (int i = 0; i < nb; i++) {
      if (!constrained[ids[i]]) {
        for (int j = 0; j < nb; j++) {
          gkyl_mat_triples_accum(matrix, ids[i], ids[j], mass[i * nb + j]);
        }
      }
    }
  }
  for (long n = 0; n < up->num_boundary; n++) {
    long row = up->boundary[n].global;
    gkyl_mat_triples_insert(matrix, row, row, 1.0);
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
  gkyl_free(constrained);
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
  }

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
    ndim, up->basis.poly_order, up->weight_left != NULL, up->weight_right != NULL
  );

  fem_proj_init_nodes(up);

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
  memset(up->rhs, 0, up->num_nodes * sizeof(double));
  struct gkyl_range_iter iter;
  gkyl_range_iter_init(&iter, &up->range);
  while (gkyl_range_iter_next(&iter)) {
    long loc = gkyl_range_idx(&up->range, iter.idx);
    const long *ids = gkyl_array_cfetch(up->map, loc);
    double load[FEM_PROJ_MAX_NODES];
    up->kernels.src(
      up->weight_right ? gkyl_array_cfetch(up->weight_right, loc) : NULL,
      gkyl_array_cfetch(rhsin, loc), load
    );
    for (int n = 0; n < up->basis.num_basis; n++) {
      up->rhs[ids[n]] += load[n];
    }
  }
  // Boundary rows are set after all additive contributions have finished.
  for (long n = 0; n < up->num_boundary; n++) {
    const struct fem_proj_boundary *bc = &up->boundary[n];
    double values[FEM_PROJ_MAX_NODES];
    if (bc->varying) {
      up->kernels.eval(gkyl_array_cfetch(boundary_values, bc->cell), values);
    }
    up->rhs[bc->global] = bc->varying ? values[bc->node] : bc->value;
  }
  gkyl_superlu_brhs_from_array(up->prob, up->rhs);
}

void
gkyl_fem_proj_solve(gkyl_fem_proj *up, struct gkyl_array *out)
{
  fem_proj_check_array(up, out);
  if (!up->rhs_ready) {
    gkyl_exit("fem_proj: set_rhs must precede solve.");
  }
  up->rhs_ready = false;
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
    const long *ids = gkyl_array_cfetch(up->map, loc);
    double nodal[FEM_PROJ_MAX_NODES];
    for (int n = 0; n < up->basis.num_basis; n++) {
      nodal[n] = solution[ids[n]];
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
    gkyl_cu_free(up->boundary);
  } else
#endif
  {
    gkyl_superlu_prob_release(up->prob);
    gkyl_free(up->rhs);
    gkyl_free(up->boundary);
  }
  gkyl_array_release(up->map);
  if (up->weight_left) {
    gkyl_array_release(up->weight_left);
  }
  if (up->weight_right) {
    gkyl_array_release(up->weight_right);
  }
  gkyl_free(up);
}
