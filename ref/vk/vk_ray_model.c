#include "vk_ray_internal.h"

#include "vk_rtx.h"
#include "vk_textures.h"
#include "vk_materials.h"
#include "vk_geometry.h"
#include "vk_render.h"
#include "vk_staging.h"
#include "vk_light.h"
#include "vk_math.h"
#include "vk_combuf.h"
#include "vk_logs.h"
#include "profiler.h"

#include "eiface.h"
#include "xash3d_mathlib.h"

#include <string.h>

xvk_ray_model_state_t g_ray_model_state;

typedef struct rt_model_s {
	struct rt_blas_s *blas;
	VkDeviceAddress blas_addr;
	rt_kusochki_t kusochki;
} rt_model_t;

static void applyMaterialToKusok(vk_kusok_data_t* kusok, const vk_render_geometry_t *geom, int override_texture_id, const vec4_t override_color) {
	const int tex_id = override_texture_id > 0 ? override_texture_id : geom->texture;
	const xvk_material_t *const mat = XVK_GetMaterialForTextureIndex( tex_id );
	ASSERT(mat);

	// TODO split kusochki into static geometry data and potentially dynamic material data
	// This data is static, should never change
	kusok->vertex_offset = geom->vertex_offset;
	kusok->index_offset = geom->index_offset;

	// Material data itself is mostly static. Except for animated textures, which just get a new material slot for each frame.
	kusok->material = (struct Material){
		.tex_base_color = mat->tex_base_color,
		.tex_roughness = mat->tex_roughness,
		.tex_metalness = mat->tex_metalness,
		.tex_normalmap = mat->tex_normalmap,

		.roughness = mat->roughness,
		.metalness = mat->metalness,
		.normal_scale = mat->normal_scale,
	};

	// TODO emissive is potentially "dynamic", not tied to the material directly, as it is specified per-surface in rad files
	VectorCopy(geom->emissive, kusok->emissive);
	Vector4Copy(mat->base_color, kusok->material.base_color);

	if (override_color) {
		kusok->material.base_color[0] *= override_color[0];
		kusok->material.base_color[1] *= override_color[1];
		kusok->material.base_color[2] *= override_color[2];
		kusok->material.base_color[3] *= override_color[3];
	}

	// TODO should be patched by the Chrome material source itself to generate a static chrome material
	const qboolean HACK_chrome = geom->material == kXVkMaterialChrome;
	if (!mat->set && HACK_chrome)
		kusok->material.tex_roughness = tglob.grayTexture;

	// Purely static. Once a sky forever a sky.
	if (geom->material == kXVkMaterialSky)
		kusok->material.tex_base_color = TEX_BASE_SKYBOX;
}

// TODO utilize uploadKusochki([1]) to avoid 2 copies of staging code
#if 0
static qboolean uploadKusochkiSubset(const vk_ray_model_t *const model, const vk_render_model_t *const render_model,  const int *geom_indexes, int geom_indexes_count) {
	// TODO can we sort all animated geometries (in brush) to have only a single range here?
	for (int i = 0; i < geom_indexes_count; ++i) {
		const int index = geom_indexes[i];

		const vk_staging_buffer_args_t staging_args = {
			.buffer = g_ray_model_state.kusochki_buffer.buffer,
			.offset = (model->kusochki_offset + index) * sizeof(vk_kusok_data_t),
			.size = sizeof(vk_kusok_data_t),
			.alignment = 16,
		};
		const vk_staging_region_t kusok_staging = R_VkStagingLockForBuffer(staging_args);

		if (!kusok_staging.ptr) {
			gEngine.Con_Printf(S_ERROR "Couldn't allocate staging for %d kusochek for model %s\n", 1, render_model->debug_name);
			return false;
		}

		vk_kusok_data_t *const kusochki = kusok_staging.ptr;

		vk_render_geometry_t *geom = render_model->geometries + index;
		applyMaterialToKusok(kusochki + 0, geom, -1, NULL);

		/* gEngine.Con_Reportf("model %s: geom=%d kuoffs=%d kustoff=%d kustsz=%d sthndl=%d\n", */
		/* 		render_model->debug_name, */
		/* 		render_model->num_geometries, */
		/* 		model->kusochki_offset, */
		/* 		staging_args.offset, staging_args.size, */
		/* 		kusok_staging.handle */
		/* 		); */

		R_VkStagingUnlock(kusok_staging.handle);
	}
	return true;
}
#endif

// TODO this material mapping is context dependent. I.e. different entity types might need different ray tracing behaviours for
// same render_mode/type and even texture.
static uint32_t materialModeFromRenderType(vk_render_type_e render_type) {
	switch (render_type) {
		case kVkRenderTypeSolid:
			return MATERIAL_MODE_OPAQUE;
			break;
		case kVkRenderType_A_1mA_RW: // blend: scr*a + dst*(1-a), depth: RW
		case kVkRenderType_A_1mA_R:  // blend: scr*a + dst*(1-a), depth test
			// FIXME where is MATERIAL_MODE_TRANSLUCENT??1
			return MATERIAL_MODE_BLEND_MIX;
			break;
		case kVkRenderType_A_1:   // blend: scr*a + dst, no depth test or write; sprite:kRenderGlow only
			return MATERIAL_MODE_BLEND_GLOW;
			break;
		case kVkRenderType_A_1_R: // blend: scr*a + dst, depth test
		case kVkRenderType_1_1_R: // blend: scr + dst, depth test
			return MATERIAL_MODE_BLEND_ADD;
			break;
		case kVkRenderType_AT: // no blend, depth RW, alpha test
			return MATERIAL_MODE_OPAQUE_ALPHA_TEST;
			break;

		default:
			gEngine.Host_Error("Unexpected render type %d\n", render_type);
	}

	return MATERIAL_MODE_OPAQUE;
}

void RT_RayModel_Clear(void) {
	R_DEBuffer_Init(&g_ray_model_state.kusochki_alloc, MAX_KUSOCHKI / 2, MAX_KUSOCHKI / 2);
}

void XVK_RayModel_ClearForNextFrame( void ) {
	g_ray_model_state.frame.instances_count = 0;
	R_DEBuffer_Flip(&g_ray_model_state.kusochki_alloc);
}

rt_kusochki_t RT_KusochkiAllocLong(int count) {
	// TODO Proper block allocator, not just double-ended buffer
	uint32_t kusochki_offset = R_DEBuffer_Alloc(&g_ray_model_state.kusochki_alloc, LifetimeStatic, count, 1);

	if (kusochki_offset == ALO_ALLOC_FAILED) {
		gEngine.Con_Printf(S_ERROR "Maximum number of kusochki exceeded\n");
		return (rt_kusochki_t){0,0,-1};
	}

	return (rt_kusochki_t){
		.offset = kusochki_offset,
		.count = count,
		.internal_index__ = 0, // ???
	};
}

uint32_t RT_KusochkiAllocOnce(int count) {
	// TODO Proper block allocator
	uint32_t kusochki_offset = R_DEBuffer_Alloc(&g_ray_model_state.kusochki_alloc, LifetimeDynamic, count, 1);

	if (kusochki_offset == ALO_ALLOC_FAILED) {
		gEngine.Con_Printf(S_ERROR "Maximum number of kusochki exceeded\n");
		return ALO_ALLOC_FAILED;
	}

	return kusochki_offset;
}

void RT_KusochkiFree(const rt_kusochki_t *kusochki) {
	// TODO block alloc
	PRINT_NOT_IMPLEMENTED();
}

// TODO this function can't really fail. It'd mean that staging is completely broken.
qboolean RT_KusochkiUpload(uint32_t kusochki_offset, const struct vk_render_geometry_s *geoms, int geoms_count, int override_texture_id, const vec4_t *override_colors) {
	const vk_staging_buffer_args_t staging_args = {
		.buffer = g_ray_model_state.kusochki_buffer.buffer,
		.offset = kusochki_offset * sizeof(vk_kusok_data_t),
		.size = geoms_count * sizeof(vk_kusok_data_t),
		.alignment = 16,
	};
	const vk_staging_region_t kusok_staging = R_VkStagingLockForBuffer(staging_args);

	if (!kusok_staging.ptr) {
		gEngine.Con_Printf(S_ERROR "Couldn't allocate staging for %d kusochkov\n", geoms_count);
		return false;
	}

	vk_kusok_data_t *const p = kusok_staging.ptr;
	for (int i = 0; i < geoms_count; ++i) {
		const vk_render_geometry_t *geom = geoms + i;
		applyMaterialToKusok(p + i, geom, override_texture_id, override_colors ? override_colors[i] : NULL);
	}

	R_VkStagingUnlock(kusok_staging.handle);
	return true;
}

struct rt_model_s *RT_ModelCreate(rt_model_create_t args) {
	const rt_kusochki_t kusochki = RT_KusochkiAllocLong(args.geometries_count);
	if (kusochki.count == 0) {
		gEngine.Con_Printf(S_ERROR "Cannot allocate kusochki for %s\n", args.debug_name);
		return NULL;
	}

	struct rt_blas_s* blas = RT_BlasCreate(args.debug_name, args.usage);
	if (!blas) {
		gEngine.Con_Printf(S_ERROR "Cannot create BLAS for %s\n", args.debug_name);
		goto fail;
	}

	if (!RT_BlasBuild(blas, args.geometries, args.geometries_count)) {
		gEngine.Con_Printf(S_ERROR "Cannot build BLAS for %s\n", args.debug_name);
		goto fail;
	}

	RT_KusochkiUpload(kusochki.offset, args.geometries, args.geometries_count, -1, NULL);

	{
		rt_model_t *const ret = Mem_Malloc(vk_core.pool, sizeof(*ret));
		ret->blas = blas;
		ret->blas_addr = RT_BlasGetDeviceAddress(ret->blas);
		ret->kusochki = kusochki;
		return ret;
	}

fail:
	if (blas)
		RT_BlasDestroy(blas);

	if (kusochki.count)
		RT_KusochkiFree(&kusochki);

	return NULL;
}

void RT_ModelDestroy(struct rt_model_s* model) {
	if (!model)
		return;

	if (model->blas)
		RT_BlasDestroy(model->blas);

	if (model->kusochki.count)
		RT_KusochkiFree(&model->kusochki);

	Mem_Free(model);
}

qboolean RT_ModelUpdate(struct rt_model_s *model, const struct vk_render_geometry_s *geometries, int geometries_count) {
	return RT_BlasBuild(model->blas, geometries, geometries_count);
}

qboolean RT_ModelUpdateMaterials(struct rt_model_s *model, const struct vk_render_geometry_s *geometries, int geometries_count, const int *geom_indices, int geom_indices_count) {
	if (!geom_indices_count)
		return true;

	APROF_SCOPE_DECLARE_BEGIN(update_materials, __FUNCTION__);

	int begin = 0;
	for (int i = 1; i < geom_indices_count; ++i) {
		const int geom_index = geom_indices[i];
		ASSERT(geom_index >= 0);
		ASSERT(geom_index < geometries_count);

		if (geom_indices[i - 1] + 1 != geom_index) {
			const int offset = geom_indices[begin];
			const int count = i - begin;
			ASSERT(offset + count <= geometries_count);
			if (!RT_KusochkiUpload(model->kusochki.offset + offset, geometries + offset, count, -1, NULL)) {
				APROF_SCOPE_END(update_materials);
				return false;
			}

			begin = i;
		}
	}

	{
		const int offset = geom_indices[begin];
		const int count = geom_indices_count - begin;
		ASSERT(offset + count <= geometries_count);
		if (!RT_KusochkiUpload(model->kusochki.offset + offset, geometries + offset, count, -1, NULL)) {

			APROF_SCOPE_END(update_materials);
			return false;
		}
	}

	APROF_SCOPE_END(update_materials);
	return true;
}

rt_draw_instance_t *getDrawInstance(void) {
	if (g_ray_model_state.frame.instances_count >= ARRAYSIZE(g_ray_model_state.frame.instances)) {
		gEngine.Con_Printf(S_ERROR "Too many RT draw instances, max = %d\n", (int)(ARRAYSIZE(g_ray_model_state.frame.instances)));
		return NULL;
	}

	return g_ray_model_state.frame.instances + (g_ray_model_state.frame.instances_count++);
}

void RT_FrameAddModel( struct rt_model_s *model, rt_frame_add_model_t args ) {
	if (!model || !model->blas)
		return;

	uint32_t kusochki_offset = model->kusochki.offset;

	if (args.override.textures > 0) {
		kusochki_offset = RT_KusochkiAllocOnce(args.override.geoms_count);
		if (kusochki_offset == ALO_ALLOC_FAILED)
			return;

		if (!RT_KusochkiUpload(kusochki_offset, args.override.geoms, args.override.geoms_count, args.override.textures, NULL)) {
			gEngine.Con_Printf(S_ERROR "Couldn't upload kusochki for instanced model\n");
			return;
		}
	}

	for (int i = 0; i < args.dynamic_polylights_count; ++i) {
		rt_light_add_polygon_t *const polylight = args.dynamic_polylights + i;
		polylight->transform_row = (const matrix3x4*)args.transform;
		polylight->dynamic = true;
		RT_LightAddPolygon(polylight);
	}

	rt_draw_instance_t *const draw_instance = getDrawInstance();
	if (!draw_instance)
		return;

	draw_instance->blas_addr = model->blas_addr;
	draw_instance->kusochki_offset = kusochki_offset;
	draw_instance->material_mode = materialModeFromRenderType(args.render_type);
	Vector4Copy(*args.color, draw_instance->color);
	Matrix3x4_Copy(draw_instance->transform_row, args.transform);
	Matrix4x4_Copy(draw_instance->prev_transform_row, args.prev_transform);
}

#define MAX_RT_DYNAMIC_GEOMETRIES 256

typedef struct {
	struct rt_blas_s *blas;
	VkDeviceAddress blas_addr;
	vk_render_geometry_t geometries[MAX_RT_DYNAMIC_GEOMETRIES];
	int geometries_count;
	vec4_t colors[MAX_RT_DYNAMIC_GEOMETRIES];
} rt_dynamic_t;

static const char* group_names[MATERIAL_MODE_COUNT] = {
	"MATERIAL_MODE_OPAQUE",
	"MATERIAL_MODE_OPAQUE_ALPHA_TEST",
	"MATERIAL_MODE_TRANSLUCENT",
	"MATERIAL_MODE_BLEND_ADD",
	"MATERIAL_MODE_BLEND_MIX",
	"MATERIAL_MODE_BLEND_GLOW",
};

static struct {
	rt_dynamic_t groups[MATERIAL_MODE_COUNT];
} g_dyn;

qboolean RT_DynamicModelInit(void) {
	for (int i = 0; i < MATERIAL_MODE_COUNT; ++i) {
		struct rt_blas_s *blas = RT_BlasCreate(group_names[i], kBlasBuildDynamicFast);
		if (!blas) {
			// FIXME destroy allocated
			gEngine.Con_Printf(S_ERROR "Couldn't create blas for %s\n", group_names[i]);
			return false;
		}

		if (!RT_BlasPreallocate(blas, (rt_blas_preallocate_t){
			// TODO better estimates for these constants
			.max_geometries = MAX_RT_DYNAMIC_GEOMETRIES,
			.max_prims_per_geometry = 256,
			.max_vertex_per_geometry = 256,
		})) {
			// FIXME destroy allocated
			gEngine.Con_Printf(S_ERROR "Couldn't preallocate blas for %s\n", group_names[i]);
			return false;
		}
		g_dyn.groups[i].blas = blas;
		g_dyn.groups[i].blas_addr = RT_BlasGetDeviceAddress(blas);
	}

	return true;
}

void RT_DynamicModelShutdown(void) {
	for (int i = 0; i < MATERIAL_MODE_COUNT; ++i) {
		RT_BlasDestroy(g_dyn.groups[i].blas);
	}
}

void RT_DynamicModelProcessFrame(void) {
	for (int i = 0; i < MATERIAL_MODE_COUNT; ++i) {
		rt_dynamic_t *const dyn = g_dyn.groups + i;
		if (!dyn->geometries_count)
			continue;

		rt_draw_instance_t* draw_instance;
		const uint32_t kusochki_offset = RT_KusochkiAllocOnce(dyn->geometries_count);
		if (kusochki_offset == ALO_ALLOC_FAILED) {
			gEngine.Con_Printf(S_ERROR "Couldn't allocate kusochki once for %d geoms of %s, skipping\n", dyn->geometries_count, group_names[i]);
			goto tail;
		}

		// FIXME override color
		if (!RT_KusochkiUpload(kusochki_offset, dyn->geometries, dyn->geometries_count, -1, dyn->colors)) {
			gEngine.Con_Printf(S_ERROR "Couldn't build blas for %d geoms of %s, skipping\n", dyn->geometries_count, group_names[i]);
			goto tail;
		}

		if (!RT_BlasBuild(dyn->blas, dyn->geometries, dyn->geometries_count)) {
			gEngine.Con_Printf(S_ERROR "Couldn't build blas for %d geoms of %s, skipping\n", dyn->geometries_count, group_names[i]);
			goto tail;
		}

		draw_instance = getDrawInstance();
		if (!draw_instance)
			goto tail;

		draw_instance->blas_addr = dyn->blas_addr;
		draw_instance->kusochki_offset = kusochki_offset;
		draw_instance->material_mode = i;
		Vector4Set(draw_instance->color, 1, 1, 1, 1);
		Matrix3x4_LoadIdentity(draw_instance->transform_row);
		Matrix4x4_LoadIdentity(draw_instance->prev_transform_row);

tail:
		dyn->geometries_count = 0;
	}
}

void RT_FrameAddOnce( rt_frame_add_once_t args ) {
	const int material_mode = materialModeFromRenderType(args.render_type);
	rt_dynamic_t *const dyn = g_dyn.groups + material_mode;

	for (int i = 0; i < args.geometries_count; ++i) {
		if (dyn->geometries_count == MAX_RT_DYNAMIC_GEOMETRIES) {
			ERROR_THROTTLED(1, "Too many dynamic geometries for mode %s\n", group_names[material_mode]);
			break;
		}

		Vector4Copy(*args.color, dyn->colors[dyn->geometries_count]);
		dyn->geometries[dyn->geometries_count++] = args.geometries[i];
	}
}

