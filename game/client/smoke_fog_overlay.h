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

#define SMOKESPHERE_EXPAND_TIME		1		// Take N seconds to expand to SMOKESPHERE_MAX_RADIUS.

#define NUM_PARTICLES_PER_DIMENSION 6

// SMOKEGRENADE_PARTICLERADIUS / SMOKEPARTICLE_OVERLAP / SMOKEPARTICLE_SIZE
// come from smoke_fog_overlay_shared.h (one definition, no re-#define).
#define NUM_MATERIAL_HANDLES		16

// Volumetric smoke (see docs/smoke-volumetric.md): the cloud is wider than
// it is tall and sits on the ground, CS2-style, instead of a floating cube.
// CS2/CS:GO smokes are rounder than CS:S's squat column, so keep the dome
// close to the width instead of flattening it.
#define SMOKE_CLOUD_HEIGHT_SCALE	0.85


void InitSmokeFogOverlay();
void TermSmokeFogOverlay();
void DrawSmokeFogOverlay();


// Set these before calling DrawSmokeFogOverlay.
extern float	g_SmokeFogOverlayAlpha;
extern Vector	g_SmokeFogOverlayColor;


#endif


