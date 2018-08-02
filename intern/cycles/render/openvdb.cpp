
#include <openvdb/openvdb.h>
#include <openvdb/tools/GridTransformer.h>

#include "render/attribute.h"
#include "render/openvdb.h"

#include "util/util_logging.h"
#include "util/util_path.h"
#include "util/util_sparse_grid.h"

/* Functions that directly use the OpenVDB library throughout render. */

struct OpenVDBReader;

CCL_NAMESPACE_BEGIN

namespace {

/* Misc internal helper functions. */

template <typename T> bool gte_any(const T &a, const float &b) { return float(a) > b; }
template <> bool gte_any(const openvdb::math::Vec3d &a, const float &b)
{
	return float(a.x()) >= b || float(a.y()) >= b || float(a.z()) >= b;
}
template <> bool gte_any(const openvdb::math::Vec3i &a, const float &b)
{
	return float(a.x()) >= b || float(a.y()) >= b || float(a.z()) >= b;
}
template <> bool gte_any(const openvdb::math::Vec3s &a, const float &b)
{
	return float(a.x()) >= b || float(a.y()) >= b || float(a.z()) >= b;
}

template <typename T> void copy(float *des, const T *src) { *des = float(*src); }
template <> void copy(float *des, const openvdb::math::Vec3d *src)
{
	*(des + 0) = float(src->x());
	*(des + 1) = float(src->y());
	*(des + 2) = float(src->z());
	*(des + 3) = 1.0f;
}
template <> void copy(float *des, const openvdb::math::Vec3i *src)
{
	*(des + 0) = float(src->x());
	*(des + 1) = float(src->y());
	*(des + 2) = float(src->z());
	*(des + 3) = 1.0f;
}
template <> void copy(float *des, const openvdb::math::Vec3s *src)
{
	*(des + 0) = float(src->x());
	*(des + 1) = float(src->y());
	*(des + 2) = float(src->z());
	*(des + 3) = 1.0f;
}

const int get_tile_index(const openvdb::math::Coord &start,
                         const openvdb::math::Coord &tiled_res)
{
	return compute_index(start.x() / TILE_SIZE,
	                     start.y() / TILE_SIZE,
	                     start.z() / TILE_SIZE,
	                     tiled_res.x(),
	                     tiled_res.y());
}

const int coord_product(const openvdb::math::Coord &c)
{
	return c.x() * c.y() * c.z();
}

const openvdb::math::Coord get_tile_dim(const openvdb::math::Coord &tile_min_bound,
                                        const openvdb::math::Coord &image_res,
                                        const openvdb::math::Coord &remainder)
{
	openvdb::math::Coord tile_dim;
	for(int i = 0; i < 3; ++i) {
		tile_dim[i] = (tile_min_bound[i] + TILE_SIZE > image_res[i]) ? remainder[i] : TILE_SIZE;
	}
	return tile_dim;
}

void expand_bbox(openvdb::io::File *vdb_file,
                 openvdb::math::CoordBBox *bbox,
                 AttributeStandard std)
{
	const char *grid_name = Attribute::standard_name(std);
	if(vdb_file->hasGrid(grid_name)) {
		bbox->expand(vdb_file->readGrid(grid_name)->evalActiveVoxelBoundingBox());
	}
}

void get_bounds(openvdb::io::File *vdb_file,
                openvdb::math::Coord &resolution,
                openvdb::math::Coord &min_bound)
{
	openvdb::math::CoordBBox bbox(openvdb::math::Coord(0, 0, 0),
	                              openvdb::math::Coord(0, 0, 0));

	/* Get the combined bounding box of all possible smoke grids in the file. */
	expand_bbox(vdb_file, &bbox, ATTR_STD_VOLUME_DENSITY);
	expand_bbox(vdb_file, &bbox, ATTR_STD_VOLUME_COLOR);
	expand_bbox(vdb_file, &bbox, ATTR_STD_VOLUME_FLAME);
	expand_bbox(vdb_file, &bbox, ATTR_STD_VOLUME_HEAT);
	expand_bbox(vdb_file, &bbox, ATTR_STD_VOLUME_TEMPERATURE);
	expand_bbox(vdb_file, &bbox, ATTR_STD_VOLUME_VELOCITY);

	resolution = bbox.dim();
	min_bound = bbox.getStart();
}

/* File and Grid IO */

void cleanup_file(openvdb::io::File *vdb_file)
{
	if(vdb_file) {
		vdb_file->close();
		delete vdb_file;
		vdb_file = NULL;
	}
}

openvdb::io::File *load_file(const string &filepath)
{
	if(!path_exists(filepath) || path_is_directory(filepath)) {
		return NULL;
	}

	openvdb::io::File *vdb_file = NULL;
	try {
		vdb_file = new openvdb::io::File(filepath);
		vdb_file->setCopyMaxBytes(0);
		vdb_file->open();
	}
	/* Mostly to catch exceptions related to Blosc not being supported. */
	catch (const openvdb::IoError &e) {
		std::cerr << e.what() << '\n';
		cleanup_file(vdb_file);
	}

	return vdb_file;
}

bool get_grid(const string &filepath,
              const string &grid_name,
              openvdb::GridBase::Ptr &grid,
              OpenVDBGridType &grid_type,
              openvdb::math::Coord &resolution,
              openvdb::math::Coord &min_bound)
{
	using namespace openvdb;

	io::File *vdb_file = load_file(filepath);

	if(!vdb_file) {
		return false;
	}
	if (!vdb_file->hasGrid(grid_name)) {
		cleanup_file(vdb_file);
		return false;
	}

	grid = vdb_file->readGrid(grid_name);

	if (grid->isType<BoolGrid>()) {
		grid_type = OPENVDB_GRID_BOOL;
	}
	else if (grid->isType<DoubleGrid>()) {
		grid_type = OPENVDB_GRID_DOUBLE;
	}
	else if (grid->isType<FloatGrid>()) {
		grid_type = OPENVDB_GRID_FLOAT;
	}
	else if (grid->isType<Int32Grid>()) {
		grid_type = OPENVDB_GRID_INT32;
	}
	else if (grid->isType<Int64Grid>()) {
		grid_type = OPENVDB_GRID_INT64;
	}
	else if (grid->isType<Vec3DGrid>()) {
		grid_type = OPENVDB_GRID_VEC_DOUBLE;
	}
	else if (grid->isType<Vec3IGrid>()) {
		grid_type = OPENVDB_GRID_VEC_UINT32;
	}
	else if (grid->isType<Vec3SGrid>()) {
		grid_type = OPENVDB_GRID_VEC_FLOAT;
	}
	else {
		grid_type = OPENVDB_GRID_MISC;
	}

	/* Retrieve bound data. */
	get_bounds(vdb_file, resolution, min_bound);

	cleanup_file(vdb_file);
	return true;
}

template <typename GridType, typename T>
bool validate_and_process_grid(typename GridType::Ptr grid)
{
	using namespace openvdb;

	/* Verify that leaf dimensions match internal tile dimensions. */
	typename GridType::TreeType::LeafCIter iter = grid->tree().cbeginLeaf();
	if(iter) {
		const math::Coord dim = iter.getLeaf()->getNodeBoundingBox().dim();

		if(dim[0] != TILE_SIZE || dim[1] != TILE_SIZE || dim[2] != TILE_SIZE) {
			VLOG(1) << "Cannot load grid " << grid->getName()
			        << ", leaf dimensions are "
			        << dim[0] << "x" << dim[1] << "x" << dim[2];
			return false;
		}
	}

	/* Need to account for external grids with a non-zero background value.
	 * May have strange results depending on the grid. */
	const T background_value = grid->background();

	if(background_value != T(0)) {
		for (typename GridType::ValueOnIter iter = grid->beginValueOn(); iter; ++iter) {
		    iter.setValue(iter.getValue() - background_value);
		}
		tools::changeBackground(grid->tree(), T(0));
	}

	return true;
}

/* Load OpenVDB grid to texture. */

template<typename GridType, typename T>
void image_load_preprocess(openvdb::GridBase::Ptr grid_base,
                           const openvdb::math::Coord resolution,
                           const openvdb::math::Coord min_bound,
                           const int channels,
                           const float threshold,
                           vector<int> *sparse_indexes,
                           int &sparse_size)
{
	using namespace openvdb;

	typename GridType::Ptr grid = gridPtrCast<GridType>(grid_base);
	if(!validate_and_process_grid<GridType, T>(grid)) {
		return;
	}

	math::Coord tiled_res, remainder;
	for(int i = 0; i < 3; ++i) {
		tiled_res[i] = get_tile_res(resolution[i]);
		remainder[i] = resolution[i] % TILE_SIZE;
	}

	const int tile_count = coord_product(tiled_res);
	const int tile_pix_count = TILE_SIZE * TILE_SIZE * TILE_SIZE * channels;

	sparse_indexes->resize(tile_count, -1); /* 0 if active, -1 if inactive. */
	int voxel_count = 0;

	for (typename GridType::TreeType::LeafCIter iter = grid->tree().cbeginLeaf(); iter; ++iter) {
		const typename GridType::TreeType::LeafNodeType *leaf = iter.getLeaf();
		const T *data = leaf->buffer().data();

		for(int i = 0; i < tile_pix_count; ++i) {
			if(gte_any(data[i], threshold)) {
				const math::Coord tile_start = leaf->getNodeBoundingBox().getStart() - min_bound;
				sparse_indexes->at(get_tile_index(tile_start, tiled_res)) = 0;
				/* Calculate how many voxels are in this tile. */
				voxel_count += coord_product(get_tile_dim(tile_start, resolution, remainder));
				break;
			}
		}
	}

	/* Check memory savings. */
	const int sparse_mem_use = tile_count * sizeof(int) + voxel_count * channels * sizeof(float);
	const int dense_mem_use = coord_product(resolution) * channels * sizeof(float);

	VLOG(1) << grid->getName() << " memory usage: \n"
	        << "Dense: " << string_human_readable_size(dense_mem_use) << "\n"
	        << "Sparse: " << string_human_readable_size(sparse_mem_use) << "\n"
	        << "VDB Grid: " << string_human_readable_size(grid->memUsage());

	if(sparse_mem_use < dense_mem_use) {
		sparse_size = voxel_count * channels;
	}
	else {
		sparse_size = -1;
		sparse_indexes->resize(0);
	}
}

template<typename GridType, typename T>
void image_load_dense(openvdb::GridBase::Ptr grid_base,
                      const openvdb::math::Coord resolution,
                      const openvdb::math::Coord min_bound,
                      const int channels,
                      float *data)
{
	using namespace openvdb;

	typename GridType::Ptr grid = gridPtrCast<GridType>(grid_base);
	if(!validate_and_process_grid<GridType, T>(grid)) {
		return;
	}

	math::Coord tiled_res, remainder;
	for(int i = 0; i < 3; ++i) {
		tiled_res[i] = get_tile_res(resolution[i]);
		remainder[i] = resolution[i] % TILE_SIZE;
	}

	memset(data, 0, coord_product(resolution) * channels * sizeof(float));

	for (typename GridType::TreeType::LeafCIter iter = grid->tree().cbeginLeaf(); iter; ++iter) {
		const typename GridType::TreeType::LeafNodeType *leaf = iter.getLeaf();
		const T *leaf_data = leaf->buffer().data();
		const math::Coord tile_start = leaf->getNodeBoundingBox().getStart() - min_bound;
		const math::Coord tile_dim = get_tile_dim(tile_start, resolution, remainder);

		for (int k = 0; k < tile_dim.z(); ++k) {
			for (int j = 0; j < tile_dim.y(); ++j) {
				for (int i = 0; i < tile_dim.x(); ++i) {
					int data_index = compute_index(tile_start.x() + i,
												   tile_start.y() + j,
												   tile_start.z() + k,
												   resolution.x(),
					                               resolution.y());
					/* Index computation by coordinates is reversed in VDB grids. */
					int leaf_index = compute_index(k, j, i, tile_dim.z(), tile_dim.y());
					copy(data + data_index, leaf_data + leaf_index);
				}
			}
		}
	}
}

template<typename GridType, typename T>
void image_load_sparse(openvdb::GridBase::Ptr grid_base,
                       const openvdb::math::Coord resolution,
                       const openvdb::math::Coord min_bound,
                       const int channels,
                       float *data,
                       vector<int> *sparse_indexes)
{
	using namespace openvdb;

	typename GridType::Ptr grid = gridPtrCast<GridType>(grid_base);
	if(!validate_and_process_grid<GridType, T>(grid)) {
		return;
	}

	math::Coord tiled_res, remainder;
	for(int i = 0; i < 3; ++i) {
		tiled_res[i] = get_tile_res(resolution[i]);
		remainder[i] = resolution[i] % TILE_SIZE;
	}

	int voxel_count = 0;

	for (typename GridType::TreeType::LeafCIter iter = grid->tree().cbeginLeaf(); iter; ++iter) {
		const typename GridType::TreeType::LeafNodeType *leaf = iter.getLeaf();

		const math::Coord tile_start = leaf->getNodeBoundingBox().getStart() - min_bound;
		int tile_index = get_tile_index(tile_start, tiled_res);
		if(sparse_indexes->at(tile_index) == -1) {
			continue;
		}

		sparse_indexes->at(tile_index) = voxel_count / channels;
		const math::Coord tile_dim = get_tile_dim(tile_start, resolution, remainder);
		const T *leaf_tile = leaf->buffer().data();
		float *data_tile = data + voxel_count;

		for(int k = 0; k < tile_dim.z(); ++k) {
			for(int j = 0; j < tile_dim.y(); ++j) {
				for(int i = 0; i < tile_dim.x(); ++i, ++voxel_count) {
					int data_index = compute_index(i, j, k, tile_dim.x(), tile_dim.y());
					/* Index computation by coordinates is reversed in VDB grids. */
					int leaf_index = compute_index(k, j, i, TILE_SIZE, TILE_SIZE);
					copy(data_tile + data_index, leaf_tile + leaf_index);
				}
			}
		}
	}
}

} /* namespace */

/* Initializer, must be called if OpenVDB will be used. */
void openvdb_initialize()
{
	openvdb::initialize();
}

bool openvdb_has_grid(const string& filepath, const string& grid_name)
{
	if(grid_name.empty()) {
		return false;
	}
	openvdb::io::File *vdb_file = load_file(filepath);
	if(!vdb_file) {
		return false;
	}
	bool has_grid = vdb_file->hasGrid(grid_name);
	cleanup_file(vdb_file);
	return has_grid;
}

int3 openvdb_get_resolution(const string& filepath)
{
	openvdb::io::File *vdb_file = load_file(filepath);
	if(!vdb_file) {
		return make_int3(-1, -1, -1);
	}
	openvdb::math::Coord resolution, min_bound;
	get_bounds(vdb_file, resolution, min_bound);
	cleanup_file(vdb_file);
	return make_int3(resolution.x(), resolution.y(), resolution.z());
}

void openvdb_load_preprocess(const string& filepath,
                             const string& grid_name,
                             const float threshold,
                             vector<int> *sparse_indexes,
                             int &sparse_size)
{
	using namespace openvdb;

	GridBase::Ptr grid;
    OpenVDBGridType grid_type;
    math::Coord resolution, min_bound;

	if(!get_grid(filepath, grid_name, grid, grid_type, resolution, min_bound)) {
		return;
	}

	switch(grid_type) {
		case OPENVDB_GRID_BOOL:
			return image_load_preprocess<BoolGrid, unsigned long int>(
			            grid, resolution, min_bound, 1, threshold,
			            sparse_indexes, sparse_size);
		case OPENVDB_GRID_DOUBLE:
			return image_load_preprocess<DoubleGrid, double>(
			            grid, resolution, min_bound, 1, threshold,
			            sparse_indexes, sparse_size);
		case OPENVDB_GRID_FLOAT:
			return image_load_preprocess<FloatGrid, float>(
			            grid, resolution, min_bound, 1, threshold,
			            sparse_indexes, sparse_size);
		case OPENVDB_GRID_INT32:
			return image_load_preprocess<Int32Grid, int32_t>(
			            grid, resolution, min_bound, 1, threshold,
			            sparse_indexes, sparse_size);
		case OPENVDB_GRID_INT64:
			return image_load_preprocess<Int64Grid, int64_t>(
			            grid, resolution, min_bound, 1, threshold,
			            sparse_indexes, sparse_size);
		case OPENVDB_GRID_VEC_DOUBLE:
			return image_load_preprocess<Vec3DGrid, math::Vec3d>(
			            grid, resolution, min_bound, 4, threshold,
			            sparse_indexes, sparse_size);
		case OPENVDB_GRID_VEC_UINT32:
			return image_load_preprocess<Vec3IGrid, math::Vec3i>(
			            grid, resolution, min_bound, 4, threshold,
			            sparse_indexes, sparse_size);
		case OPENVDB_GRID_VEC_FLOAT:
			return image_load_preprocess<Vec3SGrid, math::Vec3s>(
			            grid, resolution, min_bound, 4, threshold,
			            sparse_indexes, sparse_size);
		case OPENVDB_GRID_MISC:
		default:
			return;
	}
}

void openvdb_load_image(const string& filepath,
                        const string& grid_name,
                        float *image,
                        vector<int> *sparse_indexes)
{
	using namespace openvdb;

	GridBase::Ptr grid;
    OpenVDBGridType grid_type;
    math::Coord resolution, min_bound;

	if(!get_grid(filepath, grid_name, grid, grid_type, resolution, min_bound)) {
		return;
	}

	bool make_sparse = false;
	if(sparse_indexes) {
		if(sparse_indexes->size() > 0) {
			make_sparse = true;
		}
	}

	if(!make_sparse) {
		switch(grid_type) {
			case OPENVDB_GRID_BOOL:
				return image_load_dense<BoolGrid, unsigned long int>(
				            grid, resolution, min_bound, 1, image);
			case OPENVDB_GRID_DOUBLE:
				return image_load_dense<DoubleGrid, double>(
				            grid, resolution, min_bound, 1, image);
			case OPENVDB_GRID_FLOAT:
				return image_load_dense<FloatGrid, float>(
				            grid, resolution, min_bound, 1, image);
			case OPENVDB_GRID_INT32:
				return image_load_dense<Int32Grid, int32_t>(
				            grid, resolution, min_bound, 1, image);
			case OPENVDB_GRID_INT64:
				return image_load_dense<Int64Grid, int64_t>(
				            grid, resolution, min_bound, 1, image);
			case OPENVDB_GRID_VEC_DOUBLE:
				return image_load_dense<Vec3DGrid, math::Vec3d>(
				            grid, resolution, min_bound, 1, image);
			case OPENVDB_GRID_VEC_UINT32:
				return image_load_dense<Vec3IGrid, math::Vec3i>(
				            grid, resolution, min_bound, 1, image);
			case OPENVDB_GRID_VEC_FLOAT:
				return image_load_dense<Vec3SGrid, math::Vec3s>(
				            grid, resolution, min_bound, 1, image);
			case OPENVDB_GRID_MISC:
			default:
				return;
		}
	}
	else {
		switch(grid_type) {
			case OPENVDB_GRID_BOOL:
				return image_load_sparse<BoolGrid, unsigned long int>(
				            grid, resolution, min_bound, 1, image, sparse_indexes);
			case OPENVDB_GRID_DOUBLE:
				return image_load_sparse<DoubleGrid, double>(
				            grid, resolution, min_bound, 1, image, sparse_indexes);
			case OPENVDB_GRID_FLOAT:
				return image_load_sparse<FloatGrid, float>(
				            grid, resolution, min_bound, 1, image, sparse_indexes);
			case OPENVDB_GRID_INT32:
				return image_load_sparse<Int32Grid, int32_t>(
				            grid, resolution, min_bound, 1, image, sparse_indexes);
			case OPENVDB_GRID_INT64:
				return image_load_sparse<Int64Grid, int64_t>(
				            grid, resolution, min_bound, 1, image, sparse_indexes);
			case OPENVDB_GRID_VEC_DOUBLE:
				return image_load_sparse<Vec3DGrid, math::Vec3d>(
				            grid, resolution, min_bound, 1, image, sparse_indexes);
			case OPENVDB_GRID_VEC_UINT32:
				return image_load_sparse<Vec3IGrid, math::Vec3i>(
				            grid, resolution, min_bound, 1, image, sparse_indexes);
			case OPENVDB_GRID_VEC_FLOAT:
				return image_load_sparse<Vec3SGrid, math::Vec3s>(
				            grid, resolution, min_bound, 1, image, sparse_indexes);
			case OPENVDB_GRID_MISC:
			default:
				return;
		}
	}
}

CCL_NAMESPACE_END
