//========= Copyright Valve Corporation, All rights reserved. ============//

#ifndef SMOKE_FOG_OVERLAY_SHARED_H
#define SMOKE_FOG_OVERLAY_SHARED_H


// These are the volumetric values (see docs/smoke-volumetric.md). They live
// here, not in the client header, so there is exactly one definition: the
// client header used to re-#define all three, which produced macro-redefined
// diagnostics and made the effective value depend on include order.
// SMOKEGRENADE_PARTICLERADIUS stays 80 on purpose — it fixes the gameplay
// ID-block radius (see c_cs_player.cpp) and the flashbang-vs-smoke check
// (flashbang_projectile.cpp), neither of which may follow the visuals.
#define SMOKEGRENADE_PARTICLERADIUS	80

// Visual cloud half-width (grid half-extent, before smoke_scale). CS2's smoke
// is 288 u across (144 u radius) with an opaque core; the old grid gave 108 u
// (216 u across) and, because the alpha metric used 2x that as its falloff
// radius, the sides never feathered — a dense rounded *box*. 130 u of grid
// plus a radial falloff that reaches zero at the shell yields the CS2 ball.
// Gameplay radii (ID sphere 160, flash check 80) are untouched; see
// docs/smoke-cs2-audit.md.
// NOTE: not named SMOKE_CLOUD_RADIUS — dod/c_dod_smokegrenade.cpp already
// #defines that (330) after including this header, which would collide.
#define SMOKE_VISUAL_HALF_WIDTH		130.0f
#define SMOKEPARTICLE_OVERLAP		44
#define SMOKEPARTICLE_SIZE			42


#endif


