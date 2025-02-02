/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#include "BKE_context.h"
#include "BKE_editmesh.h"
#include "BKE_lib_id.h"
#include "BKE_mesh.h"
#include "BKE_mesh_wrapper.h"
#include "BKE_modifier.h"

#include "DNA_ID.h"
#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_space_types.h"
#include "DNA_userdef_types.h"

#include "DEG_depsgraph_query.h"

#include "ED_spreadsheet.h"

#include "NOD_geometry_nodes_eval_log.hh"

#include "bmesh.h"

#include "spreadsheet_data_source_geometry.hh"
#include "spreadsheet_intern.hh"

namespace geo_log = blender::nodes::geometry_nodes_eval_log;

namespace blender::ed::spreadsheet {

void GeometryDataSource::foreach_default_column_ids(
    FunctionRef<void(const SpreadsheetColumnID &)> fn) const
{
  component_->attribute_foreach([&](StringRefNull name, const AttributeMetaData &meta_data) {
    if (meta_data.domain != domain_) {
      return true;
    }
    SpreadsheetColumnID column_id;
    column_id.name = (char *)name.c_str();
    fn(column_id);
    return true;
  });
}

std::unique_ptr<ColumnValues> GeometryDataSource::get_column_values(
    const SpreadsheetColumnID &column_id) const
{
  std::lock_guard lock{mutex_};

  bke::ReadAttributeLookup attribute = component_->attribute_try_get_for_read(column_id.name);
  if (!attribute) {
    return {};
  }
  const fn::GVArray *varray = scope_.add(std::move(attribute.varray), __func__);
  if (attribute.domain != domain_) {
    return {};
  }
  int domain_size = varray->size();
  const CustomDataType type = bke::cpp_type_to_custom_data_type(varray->type());
  switch (type) {
    case CD_PROP_FLOAT:
      return column_values_from_function(SPREADSHEET_VALUE_TYPE_FLOAT,
                                         column_id.name,
                                         domain_size,
                                         [varray](int index, CellValue &r_cell_value) {
                                           float value;
                                           varray->get(index, &value);
                                           r_cell_value.value_float = value;
                                         });
    case CD_PROP_INT32:
      return column_values_from_function(SPREADSHEET_VALUE_TYPE_INT32,
                                         column_id.name,
                                         domain_size,
                                         [varray](int index, CellValue &r_cell_value) {
                                           int value;
                                           varray->get(index, &value);
                                           r_cell_value.value_int = value;
                                         });
    case CD_PROP_BOOL:
      return column_values_from_function(SPREADSHEET_VALUE_TYPE_BOOL,
                                         column_id.name,
                                         domain_size,
                                         [varray](int index, CellValue &r_cell_value) {
                                           bool value;
                                           varray->get(index, &value);
                                           r_cell_value.value_bool = value;
                                         });
    case CD_PROP_FLOAT2: {
      return column_values_from_function(
          SPREADSHEET_VALUE_TYPE_FLOAT2,
          column_id.name,
          domain_size,
          [varray](int index, CellValue &r_cell_value) {
            float2 value;
            varray->get(index, &value);
            r_cell_value.value_float2 = value;
          },
          default_float2_column_width);
    }
    case CD_PROP_FLOAT3: {
      return column_values_from_function(
          SPREADSHEET_VALUE_TYPE_FLOAT3,
          column_id.name,
          domain_size,
          [varray](int index, CellValue &r_cell_value) {
            float3 value;
            varray->get(index, &value);
            r_cell_value.value_float3 = value;
          },
          default_float3_column_width);
    }
    case CD_PROP_COLOR: {
      return column_values_from_function(
          SPREADSHEET_VALUE_TYPE_COLOR,
          column_id.name,
          domain_size,
          [varray](int index, CellValue &r_cell_value) {
            ColorGeometry4f value;
            varray->get(index, &value);
            r_cell_value.value_color = value;
          },
          default_color_column_width);
    }
    default:
      break;
  }
  return {};
}

int GeometryDataSource::tot_rows() const
{
  return component_->attribute_domain_size(domain_);
}

using IsVertexSelectedFn = FunctionRef<bool(int vertex_index)>;

static void get_selected_vertex_indices(const Mesh &mesh,
                                        const IsVertexSelectedFn is_vertex_selected_fn,
                                        MutableSpan<bool> selection)
{
  for (const int i : IndexRange(mesh.totvert)) {
    if (!selection[i]) {
      continue;
    }
    if (!is_vertex_selected_fn(i)) {
      selection[i] = false;
    }
  }
}

static void get_selected_corner_indices(const Mesh &mesh,
                                        const IsVertexSelectedFn is_vertex_selected_fn,
                                        MutableSpan<bool> selection)
{
  for (const int i : IndexRange(mesh.totloop)) {
    const MLoop &loop = mesh.mloop[i];
    if (!selection[i]) {
      continue;
    }
    if (!is_vertex_selected_fn(loop.v)) {
      selection[i] = false;
    }
  }
}

static void get_selected_face_indices(const Mesh &mesh,
                                      const IsVertexSelectedFn is_vertex_selected_fn,
                                      MutableSpan<bool> selection)
{
  for (const int poly_index : IndexRange(mesh.totpoly)) {
    if (!selection[poly_index]) {
      continue;
    }
    const MPoly &poly = mesh.mpoly[poly_index];
    for (const int loop_index : IndexRange(poly.loopstart, poly.totloop)) {
      const MLoop &loop = mesh.mloop[loop_index];
      if (!is_vertex_selected_fn(loop.v)) {
        selection[poly_index] = false;
        break;
      }
    }
  }
}

static void get_selected_edge_indices(const Mesh &mesh,
                                      const IsVertexSelectedFn is_vertex_selected_fn,
                                      MutableSpan<bool> selection)
{
  for (const int i : IndexRange(mesh.totedge)) {
    if (!selection[i]) {
      continue;
    }
    const MEdge &edge = mesh.medge[i];
    if (!is_vertex_selected_fn(edge.v1) || !is_vertex_selected_fn(edge.v2)) {
      selection[i] = false;
    }
  }
}

static void get_selected_indices_on_domain(const Mesh &mesh,
                                           const AttributeDomain domain,
                                           const IsVertexSelectedFn is_vertex_selected_fn,
                                           MutableSpan<bool> selection)
{
  switch (domain) {
    case ATTR_DOMAIN_POINT:
      return get_selected_vertex_indices(mesh, is_vertex_selected_fn, selection);
    case ATTR_DOMAIN_FACE:
      return get_selected_face_indices(mesh, is_vertex_selected_fn, selection);
    case ATTR_DOMAIN_CORNER:
      return get_selected_corner_indices(mesh, is_vertex_selected_fn, selection);
    case ATTR_DOMAIN_EDGE:
      return get_selected_edge_indices(mesh, is_vertex_selected_fn, selection);
    default:
      return;
  }
}

/**
 * Only data sets corresponding to mesh objects in edit mode currently support selection filtering.
 */
bool GeometryDataSource::has_selection_filter() const
{
  Object *object_orig = DEG_get_original_object(object_eval_);
  if (object_orig->type != OB_MESH) {
    return false;
  }
  if (object_orig->mode != OB_MODE_EDIT) {
    return false;
  }
  if (component_->type() != GEO_COMPONENT_TYPE_MESH) {
    return false;
  }

  return true;
}

void GeometryDataSource::apply_selection_filter(MutableSpan<bool> rows_included) const
{
  std::lock_guard lock{mutex_};

  BLI_assert(object_eval_->mode == OB_MODE_EDIT);
  BLI_assert(component_->type() == GEO_COMPONENT_TYPE_MESH);
  Object *object_orig = DEG_get_original_object(object_eval_);
  const MeshComponent *mesh_component = static_cast<const MeshComponent *>(component_);
  const Mesh *mesh_eval = mesh_component->get_for_read();
  Mesh *mesh_orig = (Mesh *)object_orig->data;
  BMesh *bm = mesh_orig->edit_mesh->bm;
  BM_mesh_elem_table_ensure(bm, BM_VERT);

  int *orig_indices = (int *)CustomData_get_layer(&mesh_eval->vdata, CD_ORIGINDEX);
  if (orig_indices != nullptr) {
    /* Use CD_ORIGINDEX layer if it exists. */
    auto is_vertex_selected = [&](int vertex_index) -> bool {
      const int i_orig = orig_indices[vertex_index];
      if (i_orig < 0) {
        return false;
      }
      if (i_orig >= bm->totvert) {
        return false;
      }
      BMVert *vert = bm->vtable[i_orig];
      return BM_elem_flag_test(vert, BM_ELEM_SELECT);
    };
    get_selected_indices_on_domain(*mesh_eval, domain_, is_vertex_selected, rows_included);
  }
  else if (mesh_eval->totvert == bm->totvert) {
    /* Use a simple heuristic to match original vertices to evaluated ones. */
    auto is_vertex_selected = [&](int vertex_index) -> bool {
      BMVert *vert = bm->vtable[vertex_index];
      return BM_elem_flag_test(vert, BM_ELEM_SELECT);
    };
    get_selected_indices_on_domain(*mesh_eval, domain_, is_vertex_selected, rows_included);
  }
}

void InstancesDataSource::foreach_default_column_ids(
    FunctionRef<void(const SpreadsheetColumnID &)> fn) const
{
  if (component_->instances_amount() == 0) {
    return;
  }

  SpreadsheetColumnID column_id;
  column_id.name = (char *)"Name";
  fn(column_id);
  for (const char *name : {"Position", "Rotation", "Scale", "ID"}) {
    column_id.name = (char *)name;
    fn(column_id);
  }
}

std::unique_ptr<ColumnValues> InstancesDataSource::get_column_values(
    const SpreadsheetColumnID &column_id) const
{
  if (component_->instances_amount() == 0) {
    return {};
  }

  const int size = this->tot_rows();
  if (STREQ(column_id.name, "Name")) {
    Span<int> reference_handles = component_->instance_reference_handles();
    Span<InstanceReference> references = component_->references();
    std::unique_ptr<ColumnValues> values = column_values_from_function(
        SPREADSHEET_VALUE_TYPE_INSTANCES,
        "Name",
        size,
        [reference_handles, references](int index, CellValue &r_cell_value) {
          const InstanceReference &reference = references[reference_handles[index]];
          switch (reference.type()) {
            case InstanceReference::Type::Object: {
              Object &object = reference.object();
              r_cell_value.value_object = ObjectCellValue{&object};
              break;
            }
            case InstanceReference::Type::Collection: {
              Collection &collection = reference.collection();
              r_cell_value.value_collection = CollectionCellValue{&collection};
              break;
            }
            case InstanceReference::Type::GeometrySet: {
              const GeometrySet &geometry_set = reference.geometry_set();
              r_cell_value.value_geometry_set = GeometrySetCellValue{&geometry_set};
              break;
            }
            case InstanceReference::Type::None: {
              break;
            }
          }
        });
    values->default_width = 8.0f;
    return values;
  }
  Span<float4x4> transforms = component_->instance_transforms();
  if (STREQ(column_id.name, "Position")) {
    return column_values_from_function(
        SPREADSHEET_VALUE_TYPE_FLOAT3,
        column_id.name,
        size,
        [transforms](int index, CellValue &r_cell_value) {
          r_cell_value.value_float3 = transforms[index].translation();
        },
        default_float3_column_width);
  }
  if (STREQ(column_id.name, "Rotation")) {
    return column_values_from_function(
        SPREADSHEET_VALUE_TYPE_FLOAT3,
        column_id.name,
        size,
        [transforms](int index, CellValue &r_cell_value) {
          r_cell_value.value_float3 = transforms[index].to_euler();
        },
        default_float3_column_width);
  }
  if (STREQ(column_id.name, "Scale")) {
    return column_values_from_function(
        SPREADSHEET_VALUE_TYPE_FLOAT3,
        column_id.name,
        size,
        [transforms](int index, CellValue &r_cell_value) {
          r_cell_value.value_float3 = transforms[index].scale();
        },
        default_float3_column_width);
  }
  Span<int> ids = component_->instance_ids();
  if (STREQ(column_id.name, "ID")) {
    /* Make the column a bit wider by default, since the IDs tend to be large numbers. */
    return column_values_from_function(
        SPREADSHEET_VALUE_TYPE_INT32,
        column_id.name,
        size,
        [ids](int index, CellValue &r_cell_value) { r_cell_value.value_int = ids[index]; },
        5.5f);
  }
  return {};
}

int InstancesDataSource::tot_rows() const
{
  return component_->instances_amount();
}

GeometrySet spreadsheet_get_display_geometry_set(const SpaceSpreadsheet *sspreadsheet,
                                                 Object *object_eval,
                                                 const GeometryComponentType used_component_type)
{
  GeometrySet geometry_set;
  if (sspreadsheet->object_eval_state == SPREADSHEET_OBJECT_EVAL_STATE_ORIGINAL) {
    Object *object_orig = DEG_get_original_object(object_eval);
    if (object_orig->type == OB_MESH) {
      MeshComponent &mesh_component = geometry_set.get_component_for_write<MeshComponent>();
      if (object_orig->mode == OB_MODE_EDIT) {
        Mesh *mesh = (Mesh *)object_orig->data;
        BMEditMesh *em = mesh->edit_mesh;
        if (em != nullptr) {
          Mesh *new_mesh = (Mesh *)BKE_id_new_nomain(ID_ME, nullptr);
          /* This is a potentially heavy operation to do on every redraw. The best solution here is
           * to display the data directly from the bmesh without a conversion, which can be
           * implemented a bit later. */
          BM_mesh_bm_to_me_for_eval(em->bm, new_mesh, nullptr);
          mesh_component.replace(new_mesh, GeometryOwnershipType::Owned);
        }
      }
      else {
        Mesh *mesh = (Mesh *)object_orig->data;
        mesh_component.replace(mesh, GeometryOwnershipType::ReadOnly);
      }
    }
    else if (object_orig->type == OB_POINTCLOUD) {
      PointCloud *pointcloud = (PointCloud *)object_orig->data;
      PointCloudComponent &pointcloud_component =
          geometry_set.get_component_for_write<PointCloudComponent>();
      pointcloud_component.replace(pointcloud, GeometryOwnershipType::ReadOnly);
    }
  }
  else {
    if (used_component_type == GEO_COMPONENT_TYPE_MESH && object_eval->mode == OB_MODE_EDIT) {
      Mesh *mesh = BKE_modifier_get_evaluated_mesh_from_evaluated_object(object_eval, false);
      if (mesh == nullptr) {
        return geometry_set;
      }
      BKE_mesh_wrapper_ensure_mdata(mesh);
      MeshComponent &mesh_component = geometry_set.get_component_for_write<MeshComponent>();
      mesh_component.replace(mesh, GeometryOwnershipType::ReadOnly);
    }
    else {
      if (BLI_listbase_count(&sspreadsheet->context_path) == 1) {
        /* Use final evaluated object. */
        if (object_eval->runtime.geometry_set_eval != nullptr) {
          geometry_set = *object_eval->runtime.geometry_set_eval;
        }
      }
      else {
        const geo_log::NodeLog *node_log =
            geo_log::ModifierLog::find_node_by_spreadsheet_editor_context(*sspreadsheet);
        if (node_log != nullptr) {
          for (const geo_log::SocketLog &input_log : node_log->input_logs()) {
            if (const geo_log::GeometryValueLog *geo_value_log =
                    dynamic_cast<const geo_log::GeometryValueLog *>(input_log.value())) {
              const GeometrySet *full_geometry = geo_value_log->full_geometry();
              if (full_geometry != nullptr) {
                geometry_set = *full_geometry;
                break;
              }
            }
          }
        }
      }
    }
  }
  return geometry_set;
}

static GeometryComponentType get_display_component_type(const bContext *C, Object *object_eval)
{
  SpaceSpreadsheet *sspreadsheet = CTX_wm_space_spreadsheet(C);
  if (sspreadsheet->object_eval_state != SPREADSHEET_OBJECT_EVAL_STATE_ORIGINAL) {
    return (GeometryComponentType)sspreadsheet->geometry_component_type;
  }
  if (object_eval->type == OB_POINTCLOUD) {
    return GEO_COMPONENT_TYPE_POINT_CLOUD;
  }
  return GEO_COMPONENT_TYPE_MESH;
}

std::unique_ptr<DataSource> data_source_from_geometry(const bContext *C, Object *object_eval)
{
  SpaceSpreadsheet *sspreadsheet = CTX_wm_space_spreadsheet(C);
  const AttributeDomain domain = (AttributeDomain)sspreadsheet->attribute_domain;
  const GeometryComponentType component_type = get_display_component_type(C, object_eval);
  GeometrySet geometry_set = spreadsheet_get_display_geometry_set(
      sspreadsheet, object_eval, component_type);

  if (!geometry_set.has(component_type)) {
    return {};
  }

  if (component_type == GEO_COMPONENT_TYPE_INSTANCES) {
    return std::make_unique<InstancesDataSource>(geometry_set);
  }
  return std::make_unique<GeometryDataSource>(object_eval, geometry_set, component_type, domain);
}

}  // namespace blender::ed::spreadsheet
