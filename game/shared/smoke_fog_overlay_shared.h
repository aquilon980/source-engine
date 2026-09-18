//========= Copyright Valve Corporation, All rights reserved. ============//

#ifndef SMOKE_FOG_OVERLAY_SHARED_H
#define SMOKE_FOG_OVERLAY_SHARED_H


// These are the volumetric values (see docs/smoke-volumetric.md). They live
// here, not in the client header, so there is exactly one definition: the
// client header used to re-#define all three, which produced macro-redefined
// diagnostics and made the effective value depend on include order.
// SMOKEGRENADE_PARTICLERADIUS stays 80 on purpose — it fixes the gameplay
// ID-block radius (see c_cs_player.cpp), which must not follow the visuals.
#define SMOKEGRENADE_PARTICLERADIUS	80
#define SMOKEPARTICLE_OVERLAP		44
#define SMOKEPARTICLE_SIZE			42


#endif


