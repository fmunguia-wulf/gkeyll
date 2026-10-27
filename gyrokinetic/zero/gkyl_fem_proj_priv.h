#pragma once

#include <gkyl_fem_proj.h>
#include <gkyl_fem_proj_kernels.h>
#include <gkyl_superlu_ops.h>
#ifdef GKYL_HAVE_CUDA
#include <gkyl_culinsolver_ops.h>
#endif

// Largest serendipity basis planned here: 3D p2 (20 nodes).
#define FEM_PROJ_MAX_NODES 20

typedef void (*fem_proj_mass_t)(const double *, double *);
typedef void (*fem_proj_src_t)(const double *, const double *, double *);
typedef void (*fem_proj_transform_t)(const double *, double *);
struct fem_proj_kernels {
  fem_proj_mass_t mass;
  fem_proj_src_t src;
  fem_proj_transform_t eval, sol;
};

// Choose once at construction, including unity-weight specializations.
GKYL_CU_DH static inline struct fem_proj_kernels
fem_proj_choose_kernels(int dim, int order, bool weighted_left, bool weighted_right)
{
  struct fem_proj_kernels k = {0};
#define FEM_PROJ_SELECT(D, P)                                           \
  if (dim == D && order == P) {                                         \
    k.mass = weighted_left ? fem_proj_##D##x_ser_p##P##_mass_weighted : \
                             fem_proj_##D##x_ser_p##P##_mass;           \
    k.src = weighted_right ? fem_proj_##D##x_ser_p##P##_src_weighted :  \
                             fem_proj_##D##x_ser_p##P##_src_noweight;   \
    k.eval = fem_proj_##D##x_ser_p##P##_eval;                           \
    k.sol = fem_proj_##D##x_ser_p##P##_sol;                             \
  }
  FEM_PROJ_SELECT(1, 1)
  FEM_PROJ_SELECT(1, 2)
  FEM_PROJ_SELECT(2, 1)
  FEM_PROJ_SELECT(2, 2)
#undef FEM_PROJ_SELECT
  return k;
}

struct fem_proj_boundary {
  long global; // Global FEM node index.
  long cell; // Owning cell's index in the parent array.
  int node; // Local node index within the owning cell.
  bool varying; // Evaluate Dirichlet data from a DG field.
  double value; // Constant Dirichlet value when not varying.
};

struct gkyl_fem_proj {
  struct gkyl_rect_grid grid; // Projection grid.
  struct gkyl_range range; // Solve range with parent-array indexing.
  struct gkyl_basis basis; // Modal DG basis.
  struct gkyl_fem_proj_bc bcs; // Per-direction boundary conditions.
  bool use_gpu; // Execute on the GPU.
  bool varying; // Any boundary requires a Dirichlet DG field.
  bool rhs_ready; // RHS assembled and available for the next solve.
  long num_nodes; // Number of global FEM nodes.
  long num_boundary; // Number of constrained global nodes.
  size_t expect_arr_sz; // Minimum array size required by the solve range.
  struct gkyl_array *map; // Cell-local to global FEM node indices.
  struct gkyl_array *weight_left; // Fixed mass-matrix weight; NULL means unity.
  struct gkyl_array *weight_right; // Fixed RHS weight; NULL means unity.
  struct fem_proj_boundary *boundary; // One Dirichlet record per constrained node.
  struct fem_proj_kernels kernels; // Host kernel function pointers.
  double *rhs; // Assembled global RHS buffer for CPU solves.
  struct gkyl_superlu_prob *prob; // CPU sparse linear solver.
#ifdef GKYL_HAVE_CUDA
  struct fem_proj_kernels *kernels_cu; // Device kernel function pointers.
  struct gkyl_culinsolver_prob *prob_cu; // GPU sparse linear solver.
#endif
};

#ifdef GKYL_HAVE_CUDA
// Allocate device kernel pointers during construction.
void fem_proj_init_cu(struct gkyl_fem_proj *up);
// Assemble the RHS and enforce boundary values on device.
void fem_proj_set_rhs_cu(
  struct gkyl_fem_proj *up, const struct gkyl_array *rhs, const struct gkyl_array *boundary_values
);
// Solve and reconstruct without host transfers.
void fem_proj_solve_cu(struct gkyl_fem_proj *up, struct gkyl_array *out);
#endif
