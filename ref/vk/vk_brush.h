#pragma once

#include "xash3d_types.h"
#include "vk_render.h" // cl_entity_t

struct ref_viewpass_s;
struct draw_list_s;
struct model_s;
struct cl_entity_s;

qboolean VK_BrushInit( void );
void VK_BrushShutdown( void );

qboolean VK_BrushModelLoad(struct model_s *mod);
void VK_BrushModelDestroyAll( void );

void VK_BrushModelDraw( const cl_entity_t *ent, int render_mode, float blend, const matrix4x4 model );

const texture_t *R_TextureAnimation( const cl_entity_t *ent, const msurface_t *s );

void R_VkBrushModelCollectEmissiveSurfaces( const struct model_s *mod, qboolean is_worldmodel );

void VK_BrushUnloadTextures( model_t *mod );
