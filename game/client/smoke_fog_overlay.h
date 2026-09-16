//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose: 
//
// $NoKeywords: $
//
//=============================================================================//

#ifndef SMOKE_FOG_OVERLAY_H
#define SMOKE_FOG_OVERLAY_H


#include "basetypes.h"
#include "mathlib/vector.h"
#include "smoke_fog_overlay_shared.h"


#define ROTATION_SPEED				0.1
#define TRADE_DURATION_MIN			10
#define TRADE_DURATION_MAX			20
#define SMOKEGRENADE_PARTICLERADIUS	80

#define SMOKESPHERE_EXPAND_TIME		1		// Take N seconds to expand to SMOKESPHERE_MAX_RADIUS.

#define NUM_PARTICLES_PER_DIMENSION 6

#define SMOKEPARTICLE_OVERLAP		30

#define SMOKEPARTICLE_SIZE			80
#define NUM_MATERIAL_HANDLES		2

// Volumetric smoke (see docs/smoke-volumetric.md): the cloud is wider than
// it is tall and sits on the ground, CS2-style, instead of a floating cube.
#define SMOKE_CLOUD_HEIGHT_SCALE	0.72


void InitSmokeFogOverlay();
void TermSmokeFogOverlay();
void DrawSmokeFogOverlay();


// Set these before calling DrawSmokeFogOverlay.
extern float	g_SmokeFogOverlayAlpha;
extern Vector	g_SmokeFogOverlayColor;


#endif


