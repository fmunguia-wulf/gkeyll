#include <acutest.h>
#include <gkyl_fem_proj.h>
#include <gkyl_fem_poisson.h>
#include <gkyl_array_ops.h>
#include <gkyl_alloc.h>
#include <math.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

// Four-point quadrature integrates all weighted p2 products exactly.
static const double quad_x[] = {
  -0.8611363115940526, -0.3399810435848563, 0.3399810435848563, 0.8611363115940526
};
static const double quad_w[] = {
  0.3478548451374539, 0.6521451548625461, 0.6521451548625461, 0.3478548451374539
};

static struct gkyl_array *
array_new(bool gpu, int nb, long size)
{
  return gpu ? gkyl_array_cu_dev_new(GKYL_DOUBLE, nb, size) : gkyl_array_new(GKYL_DOUBLE, nb, size);
}

// Independent small dense solve with partial pivoting.
static void
dense_solve(int count, double *matrix, double *load)
{
  for (int col = 0; col < count; col++) {
    int pivot = col;
    for (int row = col + 1; row < count; row++) {
      if (fabs(matrix[row * count + col]) > fabs(matrix[pivot * count + col])) {
        pivot = row;
      }
    }
    TEST_ASSERT(fabs(matrix[pivot * count + col]) > 1e-14);
    for (int j = col; j < count; j++) {
      double tmp = matrix[col * count + j];
      matrix[col * count + j] = matrix[pivot * count + j];
      matrix[pivot * count + j] = tmp;
    }
    double tmp = load[col];
    load[col] = load[pivot];
    load[pivot] = tmp;
    for (int row = col + 1; row < count; row++) {
      double factor = matrix[row * count + col] / matrix[col * count + col];
      for (int j = col; j < count; j++) {
        matrix[row * count + j] -= factor * matrix[col * count + j];
      }
      load[row] -= factor * load[col];
    }
  }
  for (int row = count - 1; row >= 0; row--) {
    for (int col = row + 1; col < count; col++) {
      load[row] -= matrix[row * count + col] * load[col];
    }
    load[row] /= matrix[row * count + row];
  }
}

static double
boundary_function(int scenario, double x, double y)
{
  return scenario == 2 || scenario == 5 ? 1.25 : 0.7 + 0.2 * x + 0.1 * y * y;
}

// Reference node matching uses physical coordinates, independently of updater lattice keys.
static void
projection_case(int ndim, int order, int scenario, int weights, bool single, bool gpu)
{
  TEST_CASE_(
    "dim=%d p=%d bc=%d weights=%d single=%d gpu=%d", ndim, order, scenario, weights, single, gpu
  );
  int cells[] = {single ? 1 : 3, single ? 1 : 2};
  struct gkyl_rect_grid grid;
  gkyl_rect_grid_init(&grid, ndim, (double[]){-0.2, -0.5}, (double[]){1.3, 0.5}, cells);
  // Non-unit lower indices and an embedded range catch incorrect flat indexing.
  int lower[] = {-1, 5}, upper[] = {cells[0] - 2, cells[1] + 4};
  int extlo[] = {-2, 4}, extup[] = {cells[0] - 1, cells[1] + 5};
  struct gkyl_range ext, local;
  gkyl_range_init(&ext, ndim, extlo, extup);
  gkyl_sub_range_init(&local, &ext, lower, upper);
  struct gkyl_basis basis;
  gkyl_cart_modal_serendip(&basis, ndim, order);
  int nb = basis.num_basis;
  double nodes[16], shape_coeff[8][8] = {{0}};
  basis.node_list(nodes);
  for (int n = 0; n < nb; n++) {
    double values[8] = {0};
    values[n] = 1;
    basis.nodal_to_modal(values, shape_coeff[n]);
  }
  struct gkyl_fem_proj_inp inp = {
    .grid = &grid,
    .basis = &basis,
    .solve_range = &local,
    .use_gpu = gpu,
  };
  if (scenario == 1) {
    for (int d = 0; d < ndim; d++) {
      inp.bcs.lo_type[d] = inp.bcs.up_type[d] = GKYL_FEM_PROJ_PERIODIC;
    }
  } else if (scenario >= 2) {
    inp.bcs.lo_type[0] = inp.bcs.up_type[0] =
      scenario == 2 || scenario == 5 ? GKYL_FEM_PROJ_DIRICHLET : GKYL_FEM_PROJ_DIRICHLET_VARYING;
    inp.bcs.lo_value[0] = inp.bcs.up_value[0] = 1.25;
    if (ndim == 2) {
      inp.bcs.lo_type[1] = inp.bcs.up_type[1] = scenario == 4 ? GKYL_FEM_PROJ_DIRICHLET_VARYING :
                                                                GKYL_FEM_PROJ_PERIODIC;
    }
    if (scenario == 5) {
      inp.bcs.up_type[0] = GKYL_FEM_PROJ_NONE;
    }
  }
  struct gkyl_array *input = array_new(false, nb, ext.volume),
                    *left = array_new(false, nb, ext.volume);
  struct gkyl_array *right = array_new(false, nb, ext.volume),
                    *bc = array_new(false, nb, ext.volume);
  struct gkyl_array *result = array_new(false, nb, ext.volume),
                    *first = array_new(false, nb, ext.volume);
  struct gkyl_array *rhs = array_new(gpu, nb, ext.volume), *out = array_new(gpu, nb, ext.volume);
  struct gkyl_array *wl = array_new(gpu, nb, ext.volume), *wr = array_new(gpu, nb, ext.volume),
                    *bcd = array_new(gpu, nb, ext.volume);
  int count = 0, ids[6][8];
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
      values[n] = boundary_function(scenario, xy[0], xy[1]);
      for (int d = 0; d < ndim; d++) {
        for (int side = 0; side < 2; side++) {
          int type = side ? inp.bcs.up_type[d] : inp.bcs.lo_type[d];
          bool at = iter.idx[d] == (side ? local.upper[d] : local.lower[d]) &&
                    nodes[n * ndim + d] == (side ? 1 : -1);
          if (at && (type == GKYL_FEM_PROJ_DIRICHLET || type == GKYL_FEM_PROJ_DIRICHLET_VARYING)) {
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
  struct gkyl_fem_proj *up = gkyl_fem_proj_inew(&inp);
  // Random DG input, a compatible continuous field, and a constant field.
  for (int trial = 0; trial < 3; trial++) {
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
    double *matrix = gkyl_malloc(count * count * sizeof(double)),
           *load = gkyl_malloc(count * sizeof(double));
    memset(matrix, 0, count * count * sizeof(double));
    memset(load, 0, count * sizeof(double));
    double source_integral = 0;
    cell = 0;
    gkyl_range_iter_init(&iter, &local);
    while (gkyl_range_iter_next(&iter)) {
      long loc = gkyl_range_idx(&local, iter.idx);
      for (int qx = 0; qx < 4; qx++) {
        for (int qy = 0; qy < (ndim == 2 ? 4 : 1); qy++) {
          double point[] = {quad_x[qx], quad_x[qy]};
          double measure = quad_w[qx] * (ndim == 2 ? quad_w[qy] : 1), shape[8];
          double l = has_left ? basis.eval_expand(point, gkyl_array_cfetch(left, loc)) : 1;
          double r = has_right ? basis.eval_expand(point, gkyl_array_cfetch(right, loc)) : 1;
          double f = basis.eval_expand(point, gkyl_array_cfetch(input, loc));
          source_integral += measure * r * f;
          for (int n = 0; n < nb; n++) {
            shape[n] = basis.eval_expand(point, shape_coeff[n]);
          }
          for (int n = 0; n < nb; n++) {
            load[ids[cell][n]] += measure * r * f * shape[n];
            for (int m = 0; m < nb; m++) {
              matrix[ids[cell][n] * count + ids[cell][m]] += measure * l * shape[n] * shape[m];
            }
          }
        }
      }
      cell++;
    }
    for (int n = 0; n < count; n++) {
      if (constrained[n]) {
        for (int m = 0; m < count; m++) {
          matrix[n * count + m] = n == m;
        }
        load[n] = prescribed[n];
      }
    }
    dense_solve(count, matrix, load);
    gkyl_array_copy(rhs, input);
    gkyl_array_clear(out, 12345.0);
    gkyl_fem_proj_set_rhs(up, rhs, bcd);
    gkyl_fem_proj_solve(up, out);
    gkyl_array_copy(result, out);
    cell = 0;
    double seen[64];
    bool visited[64] = {0};
    gkyl_range_iter_init(&iter, &local);
    double output_integral = 0;
    while (gkyl_range_iter_next(&iter)) {
      long loc = gkyl_range_idx(&local, iter.idx);
      const double *got = gkyl_array_cfetch(result, loc);
      double values[8], coeff[8];
      for (int n = 0; n < nb; n++) {
        values[n] = load[ids[cell][n]];
        double at_node = basis.eval_expand(&nodes[n * ndim], got);
        int global = ids[cell][n];
        if (visited[global]) {
          TEST_CHECK(fabs(at_node - seen[global]) < 1e-11);
        }
        seen[global] = at_node;
        visited[global] = true;
      }
      basis.nodal_to_modal(values, coeff);
      for (int n = 0; n < nb; n++) {
        TEST_CHECK(fabs(got[n] - coeff[n]) < 1e-11 * (1 + fabs(coeff[n])));
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
          output_integral +=
            quad_w[qx] * (ndim == 2 ? quad_w[qy] : 1) * l * basis.eval_expand(point, got);
        }
      }
      cell++;
    }
    if (scenario < 2) {
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
      // Alias the previously computed solution as the next input/output.
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
    gkyl_free(matrix);
    gkyl_free(load);
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

static void
all_cases(bool gpu)
{
  for (int ndim = 1; ndim <= 2; ndim++) {
    for (int order = 1; order <= 2; order++) {
      for (int bc = 0; bc < 6; bc++) {
        for (int weights = 0; weights < 6; weights++) {
          for (int single = 0; single < 2; single++) {
            projection_case(ndim, order, bc, weights, single, gpu);
          }
        }
      }
    }
  }
}

static void
test_projection_host(void)
{
  all_cases(false);
}
#ifdef GKYL_HAVE_CUDA
static void
test_projection_device(void)
{
  all_cases(true);
}
#endif

// Verify compatibility with the previous zero-epsilon Poisson projection.
static void
poisson_comparison(bool gpu)
{
  struct gkyl_rect_grid grid;
  gkyl_rect_grid_init(&grid, 2, (double[]){0, -1}, (double[]){1, 1}, (int[]){3, 4});
  struct gkyl_range ext, local;
  gkyl_create_grid_ranges(&grid, (int[]){1, 1}, &ext, &local);
  struct gkyl_basis basis;
  gkyl_cart_modal_serendip(&basis, 2, 1);
  struct gkyl_array *host = array_new(false, 4, ext.volume), *a = array_new(false, 4, ext.volume),
                    *b = array_new(false, 4, ext.volume);
  for (long i = 0; i < ext.volume; i++) {
    double *v = gkyl_array_fetch(host, i);
    for (int k = 0; k < 4; k++) {
      v[k] = sin(0.3 * (i + 1) * (k + 1));
    }
  }
  struct gkyl_array *rhs = array_new(gpu, 4, ext.volume), *out = array_new(gpu, 4, ext.volume);
  struct gkyl_array *epsilon = array_new(gpu, 4, ext.volume), *ksq = array_new(gpu, 4, ext.volume);
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
  gkyl_fem_poisson_set_rhs(old, rhs, NULL);
  gkyl_fem_poisson_solve(old, out);
  gkyl_array_copy(a, out);
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
test_poisson_host(void)
{
  poisson_comparison(false);
}
#ifdef GKYL_HAVE_CUDA
static void
test_poisson_device(void)
{
  poisson_comparison(true);
}
#endif

static void
test_invalid_input(void)
{
  for (int scenario = 0; scenario < 2; scenario++) {
    pid_t child = fork();
    TEST_ASSERT(child >= 0);
    if (child == 0) {
      freopen("/dev/null", "w", stderr);
      int ndim = scenario == 0 ? 3 : 2;
      struct gkyl_rect_grid grid;
      struct gkyl_range range;
      struct gkyl_basis basis;
      gkyl_rect_grid_init(&grid, ndim, (double[]){0, 0, 0}, (double[]){1, 1, 1}, (int[]){2, 2, 2});
      gkyl_range_init(&range, ndim, (int[]){1, 1, 1}, (int[]){2, 2, 2});
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

TEST_LIST = {
  {"projection_host", test_projection_host},
  {"poisson_comparison_host", test_poisson_host},
  {"invalid_input", test_invalid_input},
#ifdef GKYL_HAVE_CUDA
  {"projection_device", test_projection_device},
  {"poisson_comparison_device", test_poisson_device},
#endif
  {NULL, NULL}
};
