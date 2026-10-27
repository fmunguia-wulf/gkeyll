#include <gkyl_deflate_zsurf.h>
#include <gkyl_fem_poisson.h>
#include <gkyl_gyrokinetic_priv.h>
#include <gkyl_gk_field_priv.h>
#include <gkyl_translate_dim.h>

struct gk_field_ts_fem_perpproj {
  struct gkyl_rect_grid grid;
  struct gkyl_basis basis;
  struct gkyl_range local, local_ext;
  struct gkyl_array *buff_dg, *buff_fem, *epsilon, *ksq;
  struct gkyl_deflate_zsurf *deflate;
  struct gkyl_translate_dim *inflate;
  struct gkyl_fem_poisson *projector;
};

// Apply the default projection only when the field uses twist-shift boundaries.
static bool
gk_field_ts_fem_perpproj_requested(const gkyl_gyrokinetic_app *app, const struct gk_field *field)
{
  return !app->core_parallel_bcs.ts_field_dg && (field->bc_par_phi == GKYL_BC_GK_FIELD_TWISTSHIFT ||
                                                 (app->cdim == 3 && app->gk_geom->has_LCFS));
}

static void
gk_field_ts_fem_perpproj_disabled(
  const gkyl_gyrokinetic_app *app, struct gk_field *field, struct gkyl_array *potential
)
{
}

void
gk_field_ts_fem_perpproj_check_input(const gkyl_gyrokinetic_app *app, const struct gk_field *field)
{
  if (!gk_field_ts_fem_perpproj_requested(app, field)) {
    return;
  }
  if (app->cdim != 3 || app->poly_order != 1 || app->basis.b_type != GKYL_BASIS_MODAL_SERENDIPITY) {
    gkyl_exit("ts_field_dg=false requires a 3D p1 serendipity configuration basis.");
  }
  if (field->gkfield_id != GKYL_GK_FIELD_ES) {
    gkyl_exit("ts_field_dg=false requires an electrostatic field.");
  }
}

static void
gk_field_ts_fem_perpproj_validate(const gkyl_gyrokinetic_app *app, const struct gk_field *field)
{
  if (app->gk_geom->has_LCFS || field->bc_par_phi != GKYL_BC_GK_FIELD_TWISTSHIFT) {
    gkyl_exit("ts_field_dg=false requires an electrostatic twist-shift flux tube without an LCFS.");
  }
  if (field->use_flr || (field->info.bias_line_list && field->info.bias_line_list->num_bias_line)) {
    gkyl_exit("ts_field_dg=false does not support FLR or internal bias lines.");
  }
  const struct gkyl_poisson_bc *bcs = &field->poisson_bcs;
  if (bcs->lo_type[0] != GKYL_POISSON_DIRICHLET || bcs->up_type[0] != GKYL_POISSON_DIRICHLET ||
      bcs->lo_type[1] != GKYL_POISSON_PERIODIC || bcs->up_type[1] != GKYL_POISSON_PERIODIC) {
    gkyl_exit("ts_field_dg=false requires constant Dirichlet x and periodic y boundary conditions."
    );
  }
  for (int d = 0; d < 2; d++) {
    if (app->local.lower[d] != app->global.lower[d] ||
        app->local.upper[d] != app->global.upper[d]) {
      gkyl_exit("ts_field_dg=false supports decomposition only along z.");
    }
  }
}

static void
gk_field_ts_fem_perpproj_enabled(
  const gkyl_gyrokinetic_app *app, struct gk_field *field, struct gkyl_array *potential
)
{
  struct gk_field_ts_fem_perpproj *proj = field->ts_fem_perpproj;
  // The physical lower boundary is the UPPER face of the lower ghost cell.
  gkyl_deflate_zsurf_advance(
    proj->deflate, app->global.lower[2] - 1, &app->global_ext, &proj->local, potential,
    proj->buff_dg, 1
  );
  gkyl_fem_poisson_set_rhs(proj->projector, proj->buff_dg, NULL);
  gkyl_fem_poisson_solve(proj->projector, proj->buff_fem);
  // Constant-in-z inflation supplies the corrected boundary value without touching the RHS.
  gkyl_translate_dim_advance(
    proj->inflate, &proj->local, &app->global_lower_ghost[2], proj->buff_fem, 1, potential
  );
}

void
gk_field_ts_fem_perpproj_new(const gkyl_gyrokinetic_app *app, struct gk_field *field)
{
  field->ts_fem_perpproj = NULL;
  field->ts_fem_perpproj_func = gk_field_ts_fem_perpproj_disabled;
  if (!gk_field_ts_fem_perpproj_requested(app, field)) {
    return;
  }

  // Abort if it's for a simulation not yet supported.
  gk_field_ts_fem_perpproj_validate(app, field);

  struct gk_field_ts_fem_perpproj *proj = gkyl_malloc(sizeof(*proj));

  // Perpendicular grid, basis and range.
  gkyl_rect_grid_init(&proj->grid, 2, app->grid.lower, app->grid.upper, app->grid.cells);
  gkyl_cart_modal_serendip(&proj->basis, 2, 1);
  gkyl_create_grid_ranges(&proj->grid, (int[]){1, 1}, &proj->local_ext, &proj->local);

  // DG and FEM buffers.
  proj->buff_dg = mkarr(app->use_gpu, proj->basis.num_basis, proj->local_ext.volume);
  proj->buff_fem = mkarr(app->use_gpu, proj->basis.num_basis, proj->local_ext.volume);

  // epsilon=0 and ksq=-1 inputs to fem_poisson.
  proj->epsilon = mkarr(app->use_gpu, proj->basis.num_basis, proj->local_ext.volume);
  proj->ksq = mkarr(app->use_gpu, proj->basis.num_basis, proj->local_ext.volume);
  gkyl_array_clear(proj->epsilon, 0.0);
  gkyl_array_clear(proj->ksq, 0.0);
  gkyl_array_shiftc(proj->ksq, -2.0, 0);

  // Initialize operator to project onto FEM.
  struct gkyl_poisson_bc bcs = field->poisson_bcs;
  proj->projector = gkyl_fem_poisson_new(
    &proj->local, &proj->grid, proj->basis, &bcs, NULL, proj->epsilon, proj->ksq, true, app->use_gpu
  );

  // Inflation and deflation operators.
  proj->deflate = gkyl_deflate_zsurf_new(&app->basis, &proj->basis, 1, app->use_gpu);
  proj->inflate =
    gkyl_translate_dim_new(2, proj->basis, 3, app->basis, 2, GKYL_UPPER_EDGE, app->use_gpu);

  // Set function pointers.
  field->ts_fem_perpproj = proj;
  field->ts_fem_perpproj_func = gk_field_ts_fem_perpproj_enabled;
}

void
gk_field_ts_fem_perpproj_advance(
  const gkyl_gyrokinetic_app *app, struct gk_field *field, struct gkyl_array *potential
)
{
  field->ts_fem_perpproj_func(app, field, potential);
}

void
gk_field_ts_fem_perpproj_release(struct gk_field *field)
{
  struct gk_field_ts_fem_perpproj *proj = field->ts_fem_perpproj;
  if (!proj) {
    return;
  }
  gkyl_fem_poisson_release(proj->projector);
  gkyl_deflate_zsurf_release(proj->deflate);
  gkyl_translate_dim_release(proj->inflate);
  gkyl_array_release(proj->buff_dg);
  gkyl_array_release(proj->buff_fem);
  gkyl_array_release(proj->epsilon);
  gkyl_array_release(proj->ksq);
  gkyl_free(proj);
}
