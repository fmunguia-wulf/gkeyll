#include <acutest.h>
#include <gkyl_fem_proj.h>
#include <gkyl_fem_proj_priv.h>
#include <gkyl_fem_poisson.h>
#include <gkyl_gauss_quad_data.h>
#include <gkyl_array_ops.h>
#include <math.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static struct gkyl_array *
mkarr(bool gpu, int nb, long size)
{
  return gpu ? gkyl_array_cu_dev_new(GKYL_DOUBLE, nb, size) : gkyl_array_new(GKYL_DOUBLE, nb, size);
}

// A continuous nodal field compatible with every constant Dirichlet face.
// Distinct-values cases vary in one direction and are periodic in the other.
static double
fproj_boundary_function(
  const struct gkyl_rect_grid *grid, const struct gkyl_fem_proj_bc *bcs, const double *point,
  int distinct
)
{
  if (distinct) {
    return 0.5 + 0.3 * point[(distinct - 1) / 3];
  }

  double variation = 0.2;
  for (int d = 0; d < grid->ndim; d++) {
    if (bcs->lo_type[d] == GKYL_FEM_PROJ_DIRICHLET) {
      variation *= point[d] - grid->lower[d];
    }
    if (bcs->up_type[d] == GKYL_FEM_PROJ_DIRICHLET) {
      variation *= point[d] - grid->upper[d];
    }
    variation *= 0.7 + 0.1 * point[d];
  }
  return 1.25 + variation;
}

// Enumerate the nine ordered nonperiodic pairs and the paired-periodic case.
static struct gkyl_fem_proj_bc
fproj_boundary_conditions(int ndim, int code)
{
  struct gkyl_fem_proj_bc bcs = {0};
  const enum gkyl_fem_proj_bc_type types[] = {
    GKYL_FEM_PROJ_NONE, GKYL_FEM_PROJ_DIRICHLET, GKYL_FEM_PROJ_DIRICHLET_VARYING
  };

  for (int d = 0; d < ndim; d++) {
    int pair = code % 10;
    code /= 10;
    bcs.lo_type[d] = pair == 9 ? GKYL_FEM_PROJ_PERIODIC : types[pair % 3];
    bcs.up_type[d] = pair == 9 ? GKYL_FEM_PROJ_PERIODIC : types[pair / 3];
    bcs.lo_value[d] = bcs.up_value[d] = 1.25;
  }

  return bcs;
}

// Reference node matching uses physical coordinates, independently of the generated indexing formulas.
static void
fproj_projection_case(
  int ndim, int poly_order, int scenario, int weights, int single_mask, int distinct, bool use_gpu
)
{
  // Four-point quadrature integrates all weighted p2 products exactly.
  const double *quad_x = gkyl_gauss_ordinates[4], *quad_w = gkyl_gauss_weights[4];
  TEST_CASE_(
    "dim=%d p=%d bc=%d weights=%d single_mask=%d distinct=%d use_gpu=%d", ndim, poly_order,
    scenario, weights, single_mask, distinct, use_gpu
  );

  int cells[] = {(single_mask & 1) ? 1 : 3, (single_mask & 2) ? 1 : 3};

  struct gkyl_rect_grid grid;
  gkyl_rect_grid_init(&grid, ndim, (double[]){-0.2, -0.5}, (double[]){1.3, 0.5}, cells);

  // Non-unit lower indices and an embedded range catch incorrect flat indexing.
  int lower[] = {-1, 5}, upper[] = {cells[0] - 2, cells[1] + 4};
  int extlo[] = {-2, 4}, extup[] = {cells[0] - 1, cells[1] + 5};

  struct gkyl_range ext, local;
  gkyl_range_init(&ext, ndim, extlo, extup);
  gkyl_sub_range_init(&local, &ext, lower, upper);

  struct gkyl_basis basis;
  gkyl_cart_modal_serendip(&basis, ndim, poly_order);
  int nb = basis.num_basis;

  double nodes[16];
  basis.node_list(nodes);

  double shape_coeff[8][8] = {{0}};
  for (int n = 0; n < nb; n++) {
    double values[8] = {0};
    values[n] = 1;
    basis.nodal_to_modal(values, shape_coeff[n]);
  }

  struct gkyl_fem_proj_inp inp = {
    .grid = &grid,
    .basis = &basis,
    .solve_range = &local,
    .use_gpu = use_gpu,
    .bcs = fproj_boundary_conditions(ndim, scenario),
  };
  if (distinct) {
    for (int d = 0; d < ndim; d++) {
      inp.bcs.lo_type[d] = inp.bcs.up_type[d] = GKYL_FEM_PROJ_PERIODIC;
    }
    int direction = (distinct - 1) / 3, pair = (distinct - 1) % 3;
    inp.bcs.lo_type[direction] = pair == 1 ? GKYL_FEM_PROJ_DIRICHLET_VARYING :
                                             GKYL_FEM_PROJ_DIRICHLET;
    inp.bcs.up_type[direction] = pair == 0 ? GKYL_FEM_PROJ_DIRICHLET_VARYING :
                                             GKYL_FEM_PROJ_DIRICHLET;
    inp.bcs.lo_value[direction] = 0.5 + 0.3 * grid.lower[direction];
    inp.bcs.up_value[direction] = 0.5 + 0.3 * grid.upper[direction];
  }

  struct gkyl_array *input = mkarr(false, nb, ext.volume), *left = mkarr(false, nb, ext.volume);
  struct gkyl_array *right = mkarr(false, nb, ext.volume), *bc = mkarr(false, nb, ext.volume);
  struct gkyl_array *result = mkarr(false, nb, ext.volume), *first = mkarr(false, nb, ext.volume);
  struct gkyl_array *rhs = mkarr(use_gpu, nb, ext.volume), *out = mkarr(use_gpu, nb, ext.volume);
  struct gkyl_array *wl = mkarr(use_gpu, nb, ext.volume), *wr = mkarr(use_gpu, nb, ext.volume),
                    *bcd = mkarr(use_gpu, nb, ext.volume);

  int count = 0, ids[9][8];
  bool has_dirichlet = false;
  double coords[64][2] = {{0}}, prescribed[64] = {0};
  bool constrained[64] = {0};
  struct gkyl_range_iter iter;
  int cell = 0;

  gkyl_range_iter_init(&iter, &local);
  while (gkyl_range_iter_next(&iter)) {
    long loc = gkyl_range_idx(&local, iter.idx);

    double *f = gkyl_array_fetch(input, loc), *l = gkyl_array_fetch(left, loc),
           *r = gkyl_array_fetch(right, loc);

    l[0] = (weights == 1 ? 1.0 : 2.0) * pow(sqrt(2.0), ndim);
    if (weights >= 2) {
      for (int k = 1; k < nb; k++) {
        l[k] = 0.015 * cos(k + cell);
      }
    }
    for (int k = 0; k < nb; k++) {
      f[k] = sin(0.7 * (cell + 1) * (k + 1));
      r[k] = weights == 3 ? 0.6 * l[k] : l[k];
    }

    double values[8];
    for (int n = 0; n < nb; n++) {
      double xy[2] = {0, 0};
      for (int d = 0; d < ndim; d++) {
        xy[d] = grid.lower[d] +
                (iter.idx[d] - local.lower[d] + 0.5 * (nodes[n * ndim + d] + 1)) * grid.dx[d];
        if (inp.bcs.lo_type[d] == GKYL_FEM_PROJ_PERIODIC && fabs(xy[d] - grid.upper[d]) < 1e-13) {
          xy[d] = grid.lower[d];
        }
      }

      int global = 0;
      while (global < count &&
             (fabs(coords[global][0] - xy[0]) > 1e-12 || fabs(coords[global][1] - xy[1]) > 1e-12)) {
        global++;
      }

      if (global == count) {
        coords[count][0] = xy[0];
        coords[count++][1] = xy[1];
      }

      ids[cell][n] = global;
      values[n] = fproj_boundary_function(&grid, &inp.bcs, xy, distinct);
      for (int d = 0; d < ndim; d++) {
        for (int side = 0; side < 2; side++) {
          int type = side ? inp.bcs.up_type[d] : inp.bcs.lo_type[d];
          bool at = iter.idx[d] == (side ? local.upper[d] : local.lower[d]) &&
                    nodes[n * ndim + d] == (side ? 1 : -1);
          if (at && (type == GKYL_FEM_PROJ_DIRICHLET || type == GKYL_FEM_PROJ_DIRICHLET_VARYING)) {
            has_dirichlet = true;
            constrained[global] = true;
            prescribed[global] = values[n];
          }
        }
      }
    }

    basis.nodal_to_modal(values, gkyl_array_fetch(bc, loc));
    cell++;
  }

  gkyl_array_copy(wl, left);
  gkyl_array_copy(wr, right);
  gkyl_array_copy(bcd, bc);

  bool has_left = weights && weights != 4, has_right = weights && weights != 5;
  inp.weight_left = has_left ? wl : NULL;
  inp.weight_right = has_right ? wr : NULL;

  // Create projection operator.
  struct gkyl_fem_proj *up = gkyl_fem_proj_inew(&inp);

  // Random DG input, a compatible continuous field, and a constant field.
  for (int trial = 0; trial < (distinct ? 2 : 3); trial++) {
    if (trial == 1) {
      gkyl_array_copy(input, bc);
    }
    if (trial == 2) {
      gkyl_array_clear(input, 0);
      gkyl_array_shiftc(input, 1.25 * pow(sqrt(2.0), ndim), 0);
      gkyl_array_clear(bc, 0);
      gkyl_array_shiftc(bc, 1.25 * pow(sqrt(2.0), ndim), 0);
      gkyl_array_copy(bcd, bc);
      for (int n = 0; n < count; n++) {
        if (constrained[n]) {
          prescribed[n] = 1.25;
        }
      }
    }

    gkyl_array_copy(rhs, input);
    gkyl_array_clear(out, 12345.0);

    // Perform FEM projection.
    gkyl_fem_proj_set_rhs(up, rhs, bcd);
    gkyl_fem_proj_solve(up, out);

    // Check results.
    gkyl_array_copy(result, out);
    cell = 0;
    double seen[64], residual[64] = {0}, residual_scale[64] = {0};
    bool visited[64] = {0};
    double source_integral = 0, output_integral = 0;

    gkyl_range_iter_init(&iter, &local);
    while (gkyl_range_iter_next(&iter)) {
      long loc = gkyl_range_idx(&local, iter.idx);

      const double *got = gkyl_array_cfetch(result, loc);
      for (int n = 0; n < nb; n++) {
        double at_node = basis.eval_expand(&nodes[n * ndim], got);
        int global = ids[cell][n];
        if (constrained[global]) {
          TEST_CHECK(fabs(at_node - prescribed[global]) < 1e-11 * (1 + fabs(prescribed[global])));
        }
        if (visited[global]) {
          TEST_CHECK(fabs(at_node - seen[global]) < 1e-11);
        }
        seen[global] = at_node;
        visited[global] = true;
      }

      if (trial > 0 && weights <= 2) {
        const double *f = gkyl_array_cfetch(input, loc);
        for (int n = 0; n < nb; n++) {
          TEST_CHECK(fabs(got[n] - f[n]) < 1e-11);
        }
      }

      for (int qx = 0; qx < 4; qx++) {
        for (int qy = 0; qy < (ndim == 2 ? 4 : 1); qy++) {
          double point[] = {quad_x[qx], quad_x[qy]};

          double l = has_left ? basis.eval_expand(point, gkyl_array_cfetch(left, loc)) : 1;
          double r = has_right ? basis.eval_expand(point, gkyl_array_cfetch(right, loc)) : 1;

          double f = basis.eval_expand(point, gkyl_array_cfetch(input, loc));
          double projected = basis.eval_expand(point, got);

          double measure = quad_w[qx] * (ndim == 2 ? quad_w[qy] : 1);
          source_integral += measure * r * f;
          output_integral += measure * l * projected;

          // Assemble the weak residual using coordinate-based node matching.
          // The uniform cell Jacobian is common to every contribution and cancels.
          for (int n = 0; n < nb; n++) {
            double shape = basis.eval_expand(point, shape_coeff[n]);
            int global = ids[cell][n];
            residual[global] += measure * shape * (l * projected - r * f);
            residual_scale[global] += measure * fabs(shape) * (fabs(l * projected) + fabs(r * f));
          }
        }
      }
      cell++;
    }

    // Dirichlet nodes satisfy prescribed values; all other nodes satisfy the projection equation.
    for (int n = 0; n < count; n++) {
      if (!constrained[n]) {
        TEST_CHECK(fabs(residual[n]) < 1e-11 * (1 + residual_scale[n]));
        TEST_MSG("node=%d residual=%.17g scale=%.17g", n, residual[n], residual_scale[n]);
      }
    }
    if (!has_dirichlet) {
      TEST_CHECK(fabs(output_integral - source_integral) < 1e-10);
    }

    gkyl_range_iter_init(&iter, &ext);
    while (gkyl_range_iter_next(&iter)) {
      if (!gkyl_range_contains_idx(&local, iter.idx)) {
        const double *v = gkyl_array_cfetch(result, gkyl_range_idx(&ext, iter.idx));
        for (int n = 0; n < nb; n++) {
          TEST_CHECK(v[n] == 12345.0);
        }
      }
    }

    if (weights <= 2) {
      gkyl_array_copy(first, result);
      // Here we check that in-place projection is supported. Also checks that
      // this is indeed a projection operator (idempotence): that projecting a
      // second time leaves the answer unchanged.
      gkyl_fem_proj_set_rhs(up, out, bcd);
      gkyl_fem_proj_solve(up, out);

      gkyl_array_copy(result, out);
      gkyl_range_iter_init(&iter, &local);
      while (gkyl_range_iter_next(&iter)) {
        long loc = gkyl_range_idx(&local, iter.idx);
        const double *a = gkyl_array_cfetch(first, loc), *b = gkyl_array_cfetch(result, loc);
        for (int n = 0; n < nb; n++) {
          TEST_CHECK(fabs(a[n] - b[n]) < 1e-11);
        }
      }
    }
  }

  gkyl_fem_proj_release(up);
  gkyl_array_release(input);
  gkyl_array_release(left);
  gkyl_array_release(right);
  gkyl_array_release(bc);
  gkyl_array_release(result);
  gkyl_array_release(first);
  gkyl_array_release(rhs);
  gkyl_array_release(out);
  gkyl_array_release(wl);
  gkyl_array_release(wr);
  gkyl_array_release(bcd);
}

// Test projection for a fixed dimension, polynomial order, and CPU/GPU backend.
// In each direction, test all nine lower/upper pairs of NONE, constant Dirichlet,
// and varying Dirichlet conditions, plus paired periodic boundaries. Combine
// these with all six weight settings (implicit/explicit unity, equal/unequal
// varying weights, and either side weighted alone) and one or three cells per
// direction. Each configuration checks an irregular DG input, a compatible
// continuous field, and a constant field using fproj_projection_case.
static void
fproj_solver_combos(int ndim, int poly_order, bool use_gpu)
{
  for (int bc = 0; bc < (ndim == 1 ? 10 : 100); bc++) {
    for (int weights = 0; weights < 6; weights++) {
      for (int single_mask = 0; single_mask < (1 << ndim); single_mask++) {
        fproj_projection_case(ndim, poly_order, bc, weights, single_mask, 0, use_gpu);
      }
    }
  }
}

// Check that lower and upper Dirichlet values are applied to the correct faces
// for a fixed dimension, polynomial order, and CPU/GPU backend. In turn, impose
// different values in each direction using constant/varying, varying/constant,
// and constant/constant boundary pairs; the other direction, if present, is
// periodic. Test one or three cells per direction, including cells touching both
// boundaries, with implicit unity weights and unequal varying weights. Each
// configuration checks an irregular DG input and a compatible continuous field;
// a constant field cannot satisfy the distinct prescribed values.
static void
fproj_distinct_boundary_combos(int ndim, int poly_order, bool use_gpu)
{
  for (int single_mask = 0; single_mask < (1 << ndim); single_mask++) {
    for (int distinct = 1; distinct <= 3 * ndim; distinct++) {
      fproj_projection_case(ndim, poly_order, 0, 0, single_mask, distinct, use_gpu);
      fproj_projection_case(ndim, poly_order, 0, 3, single_mask, distinct, use_gpu);
    }
  }
}

static void
test_fem_proj_projection_1x_p1_ho(void)
{
  fproj_solver_combos(1, 1, false);
}

static void
test_fem_proj_projection_1x_p2_ho(void)
{
  fproj_solver_combos(1, 2, false);
}

static void
test_fem_proj_projection_2x_p1_ho(void)
{
  fproj_solver_combos(2, 1, false);
}

static void
test_fem_proj_projection_2x_p2_ho(void)
{
  fproj_solver_combos(2, 2, false);
}

static void
test_fem_proj_distinct_boundary_1x_p1_ho(void)
{
  fproj_distinct_boundary_combos(1, 1, false);
}

static void
test_fem_proj_distinct_boundary_1x_p2_ho(void)
{
  fproj_distinct_boundary_combos(1, 2, false);
}

static void
test_fem_proj_distinct_boundary_2x_p1_ho(void)
{
  fproj_distinct_boundary_combos(2, 1, false);
}

static void
test_fem_proj_distinct_boundary_2x_p2_ho(void)
{
  fproj_distinct_boundary_combos(2, 2, false);
}

#ifdef GKYL_HAVE_CUDA
static void
test_fem_proj_projection_1x_p1_dev(void)
{
  fproj_solver_combos(1, 1, true);
}

static void
test_fem_proj_projection_1x_p2_dev(void)
{
  fproj_solver_combos(1, 2, true);
}

static void
test_fem_proj_projection_2x_p1_dev(void)
{
  fproj_solver_combos(2, 1, true);
}

static void
test_fem_proj_projection_2x_p2_dev(void)
{
  fproj_solver_combos(2, 2, true);
}

static void
test_fem_proj_distinct_boundary_1x_p1_dev(void)
{
  fproj_distinct_boundary_combos(1, 1, true);
}

static void
test_fem_proj_distinct_boundary_1x_p2_dev(void)
{
  fproj_distinct_boundary_combos(1, 2, true);
}

static void
test_fem_proj_distinct_boundary_2x_p1_dev(void)
{
  fproj_distinct_boundary_combos(2, 1, true);
}

static void
test_fem_proj_distinct_boundary_2x_p2_dev(void)
{
  fproj_distinct_boundary_combos(2, 2, true);
}

#endif

// Verify compatibility with the previous zero-epsilon Poisson projection.
static void
fproj_poisson_comparison(bool gpu)
{
  struct gkyl_rect_grid grid;
  gkyl_rect_grid_init(&grid, 2, (double[]){0, -1}, (double[]){1, 1}, (int[]){3, 4});

  struct gkyl_range ext, local;
  gkyl_create_grid_ranges(&grid, (int[]){1, 1}, &ext, &local);

  struct gkyl_basis basis;
  gkyl_cart_modal_serendip(&basis, 2, 1);

  struct gkyl_array *host = mkarr(false, 4, ext.volume), *a = mkarr(false, 4, ext.volume),
                    *b = mkarr(false, 4, ext.volume);

  for (long i = 0; i < ext.volume; i++) {
    double *v = gkyl_array_fetch(host, i);
    for (int k = 0; k < 4; k++) {
      v[k] = sin(0.3 * (i + 1) * (k + 1));
    }
  }

  struct gkyl_array *rhs = mkarr(gpu, 4, ext.volume), *out = mkarr(gpu, 4, ext.volume);
  struct gkyl_array *epsilon = mkarr(gpu, 4, ext.volume), *ksq = mkarr(gpu, 4, ext.volume);

  gkyl_array_copy(rhs, host);
  gkyl_array_clear(epsilon, 0);
  gkyl_array_clear(ksq, 0);
  gkyl_array_shiftc(ksq, -2, 0);

  struct gkyl_poisson_bc oldbc = {
    .lo_type = {GKYL_POISSON_DIRICHLET, GKYL_POISSON_PERIODIC},
    .up_type = {GKYL_POISSON_DIRICHLET, GKYL_POISSON_PERIODIC},
  };
  oldbc.lo_value[0].v[0] = 1.25;
  oldbc.up_value[0].v[0] = -0.75;

  struct gkyl_fem_poisson *old =
    gkyl_fem_poisson_new(&local, &grid, basis, &oldbc, NULL, epsilon, ksq, true, gpu);

  // Solve Poisson projection.
  gkyl_fem_poisson_set_rhs(old, rhs, NULL);
  gkyl_fem_poisson_solve(old, out);
  gkyl_array_copy(a, out);

  // Project with fem_proj.
  struct gkyl_fem_proj_inp inp = {
    .grid = &grid,
    .basis = &basis,
    .solve_range = &local,
    .use_gpu = gpu,
    .bcs =
      {
        .lo_type = {GKYL_FEM_PROJ_DIRICHLET, GKYL_FEM_PROJ_PERIODIC},
        .up_type = {GKYL_FEM_PROJ_DIRICHLET, GKYL_FEM_PROJ_PERIODIC},
        .lo_value = {1.25},
        .up_value = {-0.75},
      },
  };

  struct gkyl_fem_proj *up = gkyl_fem_proj_inew(&inp);
  gkyl_fem_proj_set_rhs(up, rhs, NULL);
  gkyl_fem_proj_solve(up, out);
  gkyl_array_copy(b, out);

  // Check for equivalence.
  struct gkyl_range_iter iter;
  gkyl_range_iter_init(&iter, &local);
  while (gkyl_range_iter_next(&iter)) {
    long loc = gkyl_range_idx(&local, iter.idx);
    const double *av = gkyl_array_cfetch(a, loc), *bv = gkyl_array_cfetch(b, loc);
    for (int k = 0; k < 4; k++) {
      TEST_CHECK(fabs(av[k] - bv[k]) < 1e-11);
    }
  }

  gkyl_fem_proj_release(up);
  gkyl_fem_poisson_release(old);
  gkyl_array_release(host);
  gkyl_array_release(a);
  gkyl_array_release(b);
  gkyl_array_release(rhs);
  gkyl_array_release(out);
  gkyl_array_release(epsilon);
  gkyl_array_release(ksq);
}
static void
test_fem_proj_poisson_ho(void)
{
  fproj_poisson_comparison(false);
}
#ifdef GKYL_HAVE_CUDA
static void
test_fem_proj_poisson_dev(void)
{
  fproj_poisson_comparison(true);
}
#endif

static void
test_fem_proj_invalid_input(void)
{
  for (int scenario = 0; scenario < 2; scenario++) {
    pid_t child = fork();
    TEST_ASSERT(child >= 0);
    if (child == 0) {
      freopen("/dev/null", "w", stderr);
      int ndim = scenario == 0 ? 3 : 2;
      struct gkyl_rect_grid grid;
      gkyl_rect_grid_init(&grid, ndim, (double[]){0, 0, 0}, (double[]){1, 1, 1}, (int[]){2, 2, 2});

      struct gkyl_range range;
      gkyl_range_init(&range, ndim, (int[]){1, 1, 1}, (int[]){2, 2, 2});

      struct gkyl_basis basis;
      gkyl_cart_modal_serendip(&basis, ndim, 1);

      struct gkyl_fem_proj_inp inp = {.grid = &grid, .basis = &basis, .solve_range = &range};
      if (scenario == 1) {
        inp.bcs.lo_type[0] = GKYL_FEM_PROJ_PERIODIC;
      }
      struct gkyl_fem_proj *up = gkyl_fem_proj_inew(&inp);
      gkyl_fem_proj_release(up);
      _exit(0);
    }
    int status;
    waitpid(child, &status, 0);
    TEST_CHECK(WIFEXITED(status) && WEXITSTATUS(status) != 0);
  }
}

// Check installed kernels and dispatch against independently enumerated cell owners.
// The reference traverses cells with the last coordinate fastest, as in fem_poisson.
static void
test_fem_proj_node_indexing(void)
{
  for (int ndim = 1; ndim <= 2; ndim++) {
    for (int order = 1; order <= 2; order++) {
      struct gkyl_basis basis;
      gkyl_cart_modal_serendip(&basis, ndim, order);

      double nodes[16];
      basis.node_list(nodes);

      for (int mask = 0; mask < (1 << ndim); mask++) {
        struct gkyl_fem_proj_bc bcs = {0};
        for (int d = 0; d < ndim; d++) {
          if (mask & (1 << d)) {
            bcs.lo_type[d] = bcs.up_type[d] = GKYL_FEM_PROJ_PERIODIC;
          }
        }
        struct fem_proj_kernels kernels =
          fem_proj_choose_kernels(ndim, order, false, false, mask, &bcs);
        bool periodic[2] = {(mask & 1) != 0, (mask & 2) != 0};

        for (int nx = 1; nx <= 3; nx++) {
          for (int ny = 1; ny <= (ndim == 2 ? 3 : 1); ny++) {
            TEST_CASE_("dim=%d p=%d periodic=%d cells=%dx%d", ndim, order, mask, nx, ny);
            int cells[2] = {nx, ny}, coordinates[64][2] = {{0}}, count = 0;

            for (int ix = 0; ix < nx; ix++) {
              for (int iy = 0; iy < ny; iy++) {
                int cell[2] = {ix, iy};
                for (int n = 0; n < basis.num_basis; n++) {
                  bool owns = true;
                  for (int d = 0; d < ndim; d++) {
                    if (nodes[n * ndim + d] == 1 && (cell[d] != cells[d] - 1 || periodic[d])) {
                      owns = false;
                    }
                  }
                  if (owns) {
                    TEST_ASSERT(count < 64);
                    for (int d = 0; d < ndim; d++) {
                      coordinates[count][d] = 2 * cell[d] + (int)nodes[n * ndim + d] + 1;
                    }
                    count++;
                  }
                }
              }
            }

            TEST_CHECK(kernels.num_nodes(cells, periodic) == count);

            int lower[] = {-2, 5}, upper[] = {nx - 3, ny + 4};
            int extlo[] = {-3, 4}, extup[] = {nx - 2, ny + 5};

            struct gkyl_range ext, range;
            gkyl_range_init(&ext, ndim, extlo, extup);
            gkyl_sub_range_init(&range, &ext, lower, upper);

            bool visited[64] = {false};
            struct gkyl_range_iter iter;
            gkyl_range_iter_init(&iter, &range);
            while (gkyl_range_iter_next(&iter)) {
              long global_idx[FEM_PROJ_MAX_NODES];
              fem_proj_global_indices(&kernels, &range, cells, iter.idx, global_idx);

              for (int n = 0; n < basis.num_basis; n++) {
                int point[2] = {0, 0};
                for (int d = 0; d < ndim; d++) {
                  point[d] = 2 * (iter.idx[d] - lower[d]) + (int)nodes[n * ndim + d] + 1;
                  if (periodic[d]) {
                    point[d] %= 2 * cells[d];
                  }
                }
                int expected = 0;
                while (expected < count && (coordinates[expected][0] != point[0] ||
                                            coordinates[expected][1] != point[1])) {
                  expected++;
                }
                TEST_ASSERT(expected < count);
                TEST_CHECK(global_idx[n] == expected);
                TEST_ASSERT(global_idx[n] >= 0 && global_idx[n] < count);
                visited[global_idx[n]] = true;
              }
            }

            for (int n = 0; n < count; n++) {
              TEST_CHECK(visited[n]);
            }
          }
        }
      }
    }
  }
}

TEST_LIST = {
  {"fem_proj_node_indexing", test_fem_proj_node_indexing},
  {"fem_proj_projection_1x_p1_ho", test_fem_proj_projection_1x_p1_ho},
  {"fem_proj_projection_1x_p2_ho", test_fem_proj_projection_1x_p2_ho},
  {"fem_proj_projection_2x_p1_ho", test_fem_proj_projection_2x_p1_ho},
  {"fem_proj_projection_2x_p2_ho", test_fem_proj_projection_2x_p2_ho},
  {"fem_proj_distinct_boundary_1x_p1_ho", test_fem_proj_distinct_boundary_1x_p1_ho},
  {"fem_proj_distinct_boundary_1x_p2_ho", test_fem_proj_distinct_boundary_1x_p2_ho},
  {"fem_proj_distinct_boundary_2x_p1_ho", test_fem_proj_distinct_boundary_2x_p1_ho},
  {"fem_proj_distinct_boundary_2x_p2_ho", test_fem_proj_distinct_boundary_2x_p2_ho},
  {"fem_proj_poisson_comparison_ho", test_fem_proj_poisson_ho},
  {"fem_proj_invalid_input", test_fem_proj_invalid_input},
#ifdef GKYL_HAVE_CUDA
  {"fem_proj_projection_1x_p1_dev", test_fem_proj_projection_1x_p1_dev},
  {"fem_proj_projection_1x_p2_dev", test_fem_proj_projection_1x_p2_dev},
  {"fem_proj_projection_2x_p1_dev", test_fem_proj_projection_2x_p1_dev},
  {"fem_proj_projection_2x_p2_dev", test_fem_proj_projection_2x_p2_dev},
  {"fem_proj_distinct_boundary_1x_p1_dev", test_fem_proj_distinct_boundary_1x_p1_dev},
  {"fem_proj_distinct_boundary_1x_p2_dev", test_fem_proj_distinct_boundary_1x_p2_dev},
  {"fem_proj_distinct_boundary_2x_p1_dev", test_fem_proj_distinct_boundary_2x_p1_dev},
  {"fem_proj_distinct_boundary_2x_p2_dev", test_fem_proj_distinct_boundary_2x_p2_dev},
  {"fem_proj_poisson_comparison_dev", test_fem_proj_poisson_dev},
#endif
  {NULL, NULL}
};
