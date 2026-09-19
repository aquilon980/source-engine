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

#define NUM_PARTICLES_PER_DIMENSION 9

// SMOKEGRENADE_PARTICLERADIUS / SMOKEPARTICLE_OVERLAP / SMOKEPARTICLE_SIZE
// come from smoke_fog_overlay_shared.h (one definition, no re-#define).
// NUM_MATERIAL_HANDLES is the number of CS:GO smoke sprites to draw from.
#define NUM_MATERIAL_HANDLES		16

// Volumetric smoke (see docs/smoke-volumetric.md): the cloud is a sphere that
// sits on the ground, CS2-style, instead of a floating cube. The grid is
// 9x9x9 = 729 candidates but the fill culls everything outside the ellipsoid,
// leaving ~257 puffs on a tighter lattice than the old 6x6x6 cube (216) — a
// denser, rounder ball. CS2/CS:GO smokes are near as tall as they are wide;
// 0.85 keeps the dome slightly squashed without going columnar.
#define SMOKE_CLOUD_HEIGHT_SCALE	0.85


void InitSmokeFogOverlay();
void TermSmokeFogOverlay();
void DrawSmokeFogOverlay();


// Set these before calling DrawSmokeFogOverlay.
extern float	g_SmokeFogOverlayAlpha;
extern Vector	g_SmokeFogOverlayColor;
// Per-channel tint multiplier accumulated by the smoke puffs (team colour,
// CS2-style). Reset to (1,1,1) at the top of each render frame beside
// g_SmokeFogOverlayAlpha. See docs/smoke-team-color.md.
extern Vector	g_SmokeFogOverlayTint;


#endif


