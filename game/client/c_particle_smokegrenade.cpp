//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose: 
//
// $NoKeywords: $
//===========================================================================//

#include "cbase.h"
#include "c_smoke_trail.h"
#include "smoke_fog_overlay.h"
#include "engine/IEngineTrace.h"
#include "view.h"
#include "dlight.h"
#include "iefx.h"
#include "tier1/KeyValues.h"
#include "toolframework_client.h"
#include "engine/ivdebugoverlay.h"

#if CSTRIKE_DLL
#include "c_cs_player.h"
#endif

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

// ------------------------------------------------------------------------- //
// Definitions
// ------------------------------------------------------------------------- //

static Vector s_FadePlaneDirections[] =
{
	Vector( 1,0,0),
	Vector(-1,0,0),
	Vector(0, 1,0),
	Vector(0,-1,0),
	Vector(0,0, 1),
	Vector(0,0,-1)
};
#define NUM_FADE_PLANES	(sizeof(s_FadePlaneDirections)/sizeof(s_FadePlaneDirections[0]))

// This is used to randomize the direction it chooses to move a particle in.
int g_OffsetLookup[3] = {-1,0,1};

// Bullet holes stay small: the cone edge is not widened past the hole itself,
// and only half the puff's half-size pads the test — enough that the card
// covering the hole centre stays suppressed, without clearing two neighbours
// each side and blowing the hole across the whole cloud.
#define SMOKE_HOLE_FATTEN	1.0f
#define SMOKE_HOLE_PUFF_SCALE	0.5f


// CS2-style reactive smoke (see docs/smoke-reactive.md): bullets punch
// short-lived tunnels, HE blasts clear a sphere that refills in place.
// Client-only visuals — the server sim and bot radius are untouched.
static ConVar smoke_reactive_enable( "smoke_reactive_enable", "1", FCVAR_ARCHIVE, "CS2-style smoke: bullets carve holes, HE blasts clear smoke that refills." );
static ConVar smoke_bullet_radius( "smoke_bullet_radius", "20", FCVAR_ARCHIVE, "Radius around a bullet path that thins smoke.", true, 4.0f, true, 160.0f );
static ConVar smoke_bullet_strength( "smoke_bullet_strength", "1.0", FCVAR_ARCHIVE, "How much smoke one bullet clears (0-1).", true, 0.0f, true, 1.0f );
static ConVar smoke_bullet_recover( "smoke_bullet_recover", "1.4", FCVAR_ARCHIVE, "Seconds for a bullet hole to refill.", true, 0.2f, true, 10.0f );
static ConVar smoke_he_radius( "smoke_he_radius", "160", FCVAR_ARCHIVE, "Radius of the HE smoke clear.", true, 50.0f, true, 800.0f );
static ConVar smoke_he_strength( "smoke_he_strength", "1.0", FCVAR_ARCHIVE, "How much smoke an HE blast clears (0-1).", true, 0.0f, true, 1.0f );
static ConVar smoke_he_recover( "smoke_he_recover", "3.0", FCVAR_ARCHIVE, "Seconds for HE-cleared smoke to refill.", true, 0.5f, true, 15.0f );

// Volumetric CS2-style smoke (see docs/smoke-volumetric.md): the cloud molds
// to the ground and walls, blooms fast with a soft overshoot, and dissolves
// patchily instead of shrinking as a ball. Client-only visuals.
static ConVar smoke_mold_enable( "smoke_mold_enable", "1", FCVAR_ARCHIVE, "Smoke molds to the ground/walls instead of clipping through them." );
static ConVar smoke_bloom_time( "smoke_bloom_time", "1.4", FCVAR_ARCHIVE, "Seconds for the smoke cloud to bloom to full size.", true, 0.5f, true, 3.0f );
static ConVar smoke_core( "smoke_core", "0.42", FCVAR_ARCHIVE, "Fraction of the cloud that stays fully dense (soft edge outside it).", true, 0.2f, true, 0.8f );
static ConVar smoke_brightness( "smoke_brightness", "1.0", FCVAR_ARCHIVE, "Smoke puff brightness multiplier.", true, 0.4f, true, 1.6f );
static ConVar smoke_scale( "smoke_scale", "1.0", FCVAR_ARCHIVE, "Smoke cloud size multiplier at detonation (0.6-1.4).", true, 0.6f, true, 1.4f );
static ConVar smoke_debug( "smoke_debug", "0", FCVAR_NONE, "Print smoke carve diagnostics to the console." );

// Same pattern as c_func_smokevolume/c_smokestack: low-end lever that halves
// the cloud to a checkerboard when the user opts out of dense particles.
static ConVar mat_reduceparticles( "mat_reduceparticles", "0" );


// ------------------------------------------------------------------------- //
// Classes
// ------------------------------------------------------------------------- //
class C_ParticleSmokeGrenade : public C_BaseParticleEntity, public IPrototypeAppEffect
{
public:
	DECLARE_CLASS( C_ParticleSmokeGrenade, C_BaseParticleEntity );
	DECLARE_CLIENTCLASS();

					C_ParticleSmokeGrenade();
					~C_ParticleSmokeGrenade();

private:
	
	class SmokeGrenadeParticle : public Particle
	{
	public:
		float				m_RotationSpeed;
		float				m_CurRotation;
		float				m_FadeAlpha;		// Set as it moves around.
		float				m_flSize;			// Base puff size (bloom + fade scale it).
		float				m_flDensity;		// Per-puff opacity jitter (breaks the shell).
		float				m_flSuppress;		// Smoothed carve suppression (0..1).
		unsigned char		m_ColorInterp;		// Amount between min and max colors.
		unsigned char		m_Color[4];
	};


public:

	// Optional call. It will use defaults if you don't call this.
	void			SetParams(
		);

	// Call this to move the source..
	void			SetPos(const Vector &pos);


// C_BaseEntity.
public:
	virtual void	OnDataChanged( DataUpdateType_t updateType );

	virtual void	CleanupToolRecordingState( KeyValues *msg );

// IPrototypeAppEffect.
public:
	virtual void	Start(CParticleMgr *pParticleMgr, IPrototypeArgAccess *pArgs);


// IParticleEffect.
public:
	virtual void	Update(float fTimeDelta);
	virtual void	RenderParticles( CParticleRenderIterator *pIterator );
	virtual void	SimulateParticles( CParticleSimulateIterator *pIterator );
	virtual void	NotifyRemove();
	virtual void	GetParticlePosition( Particle *pParticle, Vector& worldpos );
	virtual void	ClientThink();


// Proxies.
public:

	static void		RecvProxy_CurrentStage(  const CRecvProxyData *pData, void *pStruct, void *pOut );


private:

	// The SmokeEmitter represents a grid in 3D space.
	class SmokeParticleInfo
	{
	public:
		SmokeGrenadeParticle	*m_pParticle;
		int						m_TradeIndex;		// -1 if not exchanging yet.
		float					m_TradeClock;		// How long since they started trading.
		float					m_TradeDuration;	// How long the trade will take to finish.
		float					m_FadeAlpha;		// Calculated from nearby world geometry.
		Vector					m_MoldPos;			// Molded local offset (wall-pulled, ground-clamped).
		bool					m_bMolded;			// False for culled (inside-solid) cells.
		unsigned char			m_Color[4];
	};

	void ApplyDynamicLight( const Vector &vParticlePos, Vector &color );
	void UpdateDynamicLightList( const Vector &vMins, const Vector &vMaxs );

	// 0 = full smoke, 1 = fully cleared by a carve hole, at this world position.
	float HoleSuppressAt( const Vector &vWorldPos, float flPuffRadius = 0.0f );
	// Punch a round hole into the cloud (ring of MAX_CARVE_HOLES, oldest recycled).
	void AddCarveHole( const Vector &vCenter, float flRadius, float flStrength, float flLife, bool bExplosion = false );

	void UpdateSmokeTrail( float fTimeDelta );
	
	void UpdateParticleAndFindTrade( int iParticle, float fTimeDelta );
	void UpdateParticleDuringTrade( int iParticle, float flTimeDelta );

	inline int					GetSmokeParticleIndex(int x, int y, int z)	{return z*m_xCount*m_yCount+y*m_yCount+x;}
	inline SmokeParticleInfo*	GetSmokeParticleInfo(int x, int y, int z)	{return &m_SmokeParticleInfos[GetSmokeParticleIndex(x,y,z)];}
	inline void					GetParticleInfoXYZ(int index, int &x, int &y, int &z)
	{
		z = index / (m_xCount*m_yCount);
		int zIndex = z*m_xCount*m_yCount;
		y = (index - zIndex) / m_yCount;
		int yIndex = y*m_yCount;
		x = index - zIndex - yIndex;
	}

	inline bool					IsValidXYZCoords(int x, int y, int z)
	{
		return x >= 0 && y >= 0 && z >= 0 && x < m_xCount && y < m_yCount && z < m_zCount;
	}

	inline Vector				GetSmokeParticlePos(int x, int y, int z)	
	{
		// Molded cells keep their wall-pulled, ground-clamped offset so the
		// churn trade animation shuffles puffs inside the fitted volume.
		if ( m_bVolumeFilled )
		{
			SmokeParticleInfo *pMold = &m_SmokeParticleInfos[GetSmokeParticleIndex(x,y,z)];
			if ( pMold->m_bMolded )
				return m_SmokeBasePos + pMold->m_MoldPos;
		}
		return m_SmokeBasePos + 
			Vector( ((float)x / (m_xCount-1)) * m_SpacingRadius * 2 - m_SpacingRadius,
				((float)y / (m_yCount-1)) * m_SpacingRadius * 2 - m_SpacingRadius,
				((float)z / (m_zCount-1)) * m_SpacingRadius * m_flHeightScale * 2 - m_SpacingRadius * m_flHeightScale);
	}

	inline Vector				GetSmokeParticlePosIndex(int index)
	{
		int x, y, z;
		GetParticleInfoXYZ(index, x, y, z);
		return GetSmokeParticlePos(x, y, z);
	}

	inline const Vector&		GetPos()	{ return GetAbsOrigin(); }

	// Start filling the smoke volume (and stop the smoke trail).
	void						FillVolume();

public:
	// CS2-style reactive smoke (see docs/smoke-reactive.md).
	void						ApplyBulletSegment( const Vector &vecStart, const Vector &vecEnd );
	void						ApplyExplosion( const Vector &vecCenter );


// State variables from server.
public:
	
	unsigned char		m_CurrentStage;
	Vector				m_SmokeBasePos;

	// What time the effect was initially created
	float				m_flSpawnTime;

	// It will fade out during this time.
	float				m_FadeStartTime;
	float				m_FadeEndTime;
	float				m_FadeAlpha;	// Calculated from the fade start/end times each frame.

	// Used during rendering.. active dlights.
	class CActiveLight
	{
	public:
		Vector m_vColor;
		Vector m_vOrigin;
		float m_flRadiusSqr;
	};
	CActiveLight		m_ActiveLights[MAX_DLIGHTS];
	int					m_nActiveLights;


private:
						C_ParticleSmokeGrenade( const C_ParticleSmokeGrenade & );

	bool				m_bStarted;
	bool				m_bVolumeFilled;
	PMaterialHandle		m_MaterialHandles[NUM_MATERIAL_HANDLES];

	SmokeParticleInfo	m_SmokeParticleInfos[NUM_PARTICLES_PER_DIMENSION*NUM_PARTICLES_PER_DIMENSION*NUM_PARTICLES_PER_DIMENSION];
	int					m_xCount, m_yCount, m_zCount;
	float				m_SpacingRadius;

	Vector				m_MinColor;
	Vector				m_MaxColor;

	float				m_ExpandTimeCounter;	// How long since we started expanding.	
	float				m_ExpandRadius;			// How large is our radius.
	float				m_flHeightScale;		// Z squish of the cloud (volumetric: wider than tall).
	float				m_flBloomEase;			// 0-1 cubic-out bloom progress (drives puff inflation).
	float				m_flFadeT;				// 0-1 global fade progress (drives patchy dissolve).
	Vector				m_vecBaseLift;			// Ground-snap lift applied to the base every frame.

	// CS2-style reactive carve (see docs/smoke-reactive.md). A shot punches a
	// round hole you can see through: we store each hole's world-space centre
	// and radius, then at render time suppress every puff whose sight line from
	// the eye falls inside the cone that hole subtends. That is a circular hole
	// through the *whole* cloud depth — not just the cells on the bullet's
	// axis, which neighbours overdraw back. Holes open fast, hold, then refill.
	struct SmokeHole_t
	{
		Vector	vCenter;
		bool	bExplosion;
		float	flRadius;
		float	flStrength;		// 0..1 how opaque the hole stays at full open
		float	flBirth;		// when it opened — drives the pop-in ramp
		float	flEndTime;		// when it has fully refilled (pushed back on a spray)
		float	flRecover;		// refill duration, for the feather shape
	};
	enum { MAX_CARVE_HOLES = 12 };
	SmokeHole_t			m_CarveHoles[MAX_CARVE_HOLES];
	int					m_nCarveHoles;

	C_SmokeTrail		m_SmokeTrail;
};


// Expose to the particle app.
EXPOSE_PROTOTYPE_EFFECT(SmokeGrenade, C_ParticleSmokeGrenade);


// Datatable..
IMPLEMENT_CLIENTCLASS_DT(C_ParticleSmokeGrenade, DT_ParticleSmokeGrenade, ParticleSmokeGrenade)
	RecvPropTime(RECVINFO(m_flSpawnTime)),
	RecvPropFloat(RECVINFO(m_FadeStartTime)),
	RecvPropFloat(RECVINFO(m_FadeEndTime)),
	RecvPropInt(RECVINFO(m_CurrentStage), 0, &C_ParticleSmokeGrenade::RecvProxy_CurrentStage),
END_RECV_TABLE()


// ------------------------------------------------------------------------- //
// Helpers.
// ------------------------------------------------------------------------- //

static inline void InterpColor(unsigned char dest[4], unsigned char src1[4], unsigned char src2[4], float percent)
{
	dest[0] = (unsigned char)(src1[0] + (src2[0] - src1[0]) * percent);
	dest[1] = (unsigned char)(src1[1] + (src2[1] - src1[1]) * percent);
	dest[2] = (unsigned char)(src1[2] + (src2[2] - src1[2]) * percent);
}


static inline int GetWorldPointContents(const Vector &vPos)
{
	#if defined(PARTICLEPROTOTYPE_APP)
		return 0;
	#else
		return enginetrace->GetPointContents( vPos );
	#endif
}

// Shortest distance from a point to a segment. Used to carve bullet tunnels.
static inline float ReactiveSmokeDistPointToSegment( const Vector &vPoint, const Vector &vStart, const Vector &vEnd )
{
	Vector vSeg = vEnd - vStart;
	float flLenSqr = vSeg.LengthSqr();
	if ( flLenSqr < 1e-6f )
		return ( vPoint - vStart ).Length();

	float flT = DotProduct( vPoint - vStart, vSeg ) / flLenSqr;
	flT = clamp( flT, 0.0f, 1.0f );
	return ( vPoint - ( vStart + vSeg * flT ) ).Length();
}

// Closest point on a segment to a point. Used to place a carve hole where the
// bullet passed nearest the middle of the cloud (its midpoint may be far
// outside, e.g. on a long shot at a wall well behind the smoke).
static inline Vector ReactiveSmokeClosestPointOnSegment( const Vector &vPoint, const Vector &vStart, const Vector &vEnd )
{
	Vector vSeg = vEnd - vStart;
	float flLenSqr = vSeg.LengthSqr();
	if ( flLenSqr < 1e-6f )
		return vStart;
	float flT = clamp( DotProduct( vPoint - vStart, vSeg ) / flLenSqr, 0.0f, 1.0f );
	return vStart + vSeg * flT;
}

static inline void WorldTraceLine( const Vector &start, const Vector &end, int contentsMask, trace_t *trace )
{
	#if defined(PARTICLEPROTOTYPE_APP)
		trace->fraction = 1;
	#else
		UTIL_TraceLine(start, end, contentsMask, NULL, COLLISION_GROUP_NONE, trace);
	#endif
}

static inline Vector EngineGetLightForPoint(const Vector &vPos)
{
	#if defined(PARTICLEPROTOTYPE_APP)
		return Vector(1,1,1);
	#else
		return engine->GetLightForPoint(vPos, true);
	#endif
}

static inline const Vector& EngineGetVecRenderOrigin()
{
	#if defined(PARTICLEPROTOTYPE_APP)
		static Vector dummy(0,0,0);
		return dummy;
	#else
		return CurrentViewOrigin();
	#endif
}

static inline float& EngineGetSmokeFogOverlayAlpha()
{
	#if defined(PARTICLEPROTOTYPE_APP)
		static float dummy;
		return dummy;
	#else
		return g_SmokeFogOverlayAlpha;
	#endif
}

static inline C_BaseEntity* ParticleGetEntity(int index)
{
	#if defined(PARTICLEPROTOTYPE_APP)
		return NULL;
	#else
		return cl_entitylist->GetEnt(index);
	#endif
}



// ------------------------------------------------------------------------- //
// ParticleMovieExplosion
// ------------------------------------------------------------------------- //
C_ParticleSmokeGrenade::C_ParticleSmokeGrenade()
{
	memset(m_MaterialHandles, 0, sizeof(m_MaterialHandles));

	// Update() builds the bbox from this even at stage 0, before FillVolume()
	// assigns it — Vector() does not zero, so one frame of garbage otherwise.
	m_SmokeBasePos.Init();

	// Volumetric look (see docs/smoke-volumetric.md): bright CS2-style grey
	// with a faint cool lift. World lighting tints it per-puff at spawn.
	m_MinColor.Init(0.86, 0.86, 0.86);
	m_MaxColor.Init(0.96, 0.96, 0.96);

	m_nActiveLights = 0;
	m_ExpandRadius = 0;
	m_ExpandTimeCounter = 0;
	m_xCount = m_yCount = m_zCount = 0;
	m_SpacingRadius = 0;
	m_flHeightScale = SMOKE_CLOUD_HEIGHT_SCALE;
	m_flBloomEase = 0;
	m_flFadeT = 0;
	m_vecBaseLift.Init();
	m_FadeStartTime = 0;
	m_FadeEndTime = 0;
	m_flSpawnTime = 0;
	m_bVolumeFilled = false;
	m_CurrentStage = 0;
	m_nCarveHoles = 0;

	m_bStarted = false;
}


C_ParticleSmokeGrenade::~C_ParticleSmokeGrenade()
{
	ParticleMgr()->RemoveEffect( &m_ParticleEffect );
}


void C_ParticleSmokeGrenade::SetParams(
	)
{
}


void C_ParticleSmokeGrenade::OnDataChanged( DataUpdateType_t updateType )
{
	C_BaseEntity::OnDataChanged(updateType);

	if(updateType == DATA_UPDATE_CREATED )
	{
		Start(ParticleMgr(), NULL);
	}
}


void C_ParticleSmokeGrenade::Start(CParticleMgr *pParticleMgr, IPrototypeArgAccess *pArgs)
{
	if(!pParticleMgr->AddEffect( &m_ParticleEffect, this ))
		return;
	
	m_SmokeTrail.Start(pParticleMgr, pArgs);

	m_SmokeTrail.m_ParticleLifetime = 0.5;
	m_SmokeTrail.SetSpawnRate(40);
	m_SmokeTrail.m_MinSpeed = 0;
	m_SmokeTrail.m_MaxSpeed = 0;
	m_SmokeTrail.m_StartSize = 3;
	m_SmokeTrail.m_EndSize = 10;
	m_SmokeTrail.m_SpawnRadius = 0;

	m_SmokeTrail.SetLocalOrigin( GetAbsOrigin() );

	// CS:GO smoke sprites (particle/smokesprites_0001..0016) — soft, wispy,
	// irregular puffs the stock quad emitter can feed (plain UnlitGeneric
	// with $vertexcolor/$vertexalpha, no TEXCOORD1-4 like SpriteCard). Using
	// all 16 at random makes the cloud read as billowed smoke instead of a
	// union of identical CS:S blobs with a hard circular outline. They ship
	// in the stock cstrike VPK, so this works on any install.
	for ( int i = 0; i < NUM_MATERIAL_HANDLES; i++ )
	{
		char str[64];
		Q_snprintf( str, sizeof( str ), "particle/smokesprites_%04d", i + 1 );
		m_MaterialHandles[i] = m_ParticleEffect.FindOrAddMaterial( str );
	}

	if( m_CurrentStage == 2 )
	{
		FillVolume();
	}

	// Go straight into "fill volume" mode if they want. Guarded on
	// m_bVolumeFilled so the particle-editor path (stage 2 AND -FillVolume)
	// can't fill twice and leak a second, untracked 216-particle grid.
	if(pArgs)
	{
		if(pArgs->FindArg("-FillVolume") && !m_bVolumeFilled)
		{
			FillVolume();
		}
	}

	m_bStarted = true;
	SetNextClientThink( CLIENT_THINK_ALWAYS );

#if CSTRIKE_DLL
	C_CSPlayer *pPlayer = C_CSPlayer::GetLocalCSPlayer();

	if ( pPlayer )
	{
		 pPlayer->m_SmokeGrenades.AddToTail( this );
	}
#endif
		 
}

void C_ParticleSmokeGrenade::ClientThink()
{
	if ( m_CurrentStage != 1 )
		return;

	// "Am I in the cloud" is measured against the same squashed ellipsoid
	// RenderParticles shades, normalised so flNorm == 1 at the shell. The old
	// test was a sphere of radius m_ExpandRadius — but that is the cloud's
	// *diameter* (2 x m_SpacingRadius), so the fog reached a full half-width
	// past the smoke and started before you were anywhere near it.
	Vector vLocal = MainViewOrigin() - m_SmokeBasePos;
	vLocal.z /= MAX( 0.3f, m_flHeightScale );
	float flNorm = vLocal.Length() / MAX( 1.0f, m_SpacingRadius );

	const float flCore = 0.55f;		// fully fogged once buried past this
	const float flShell = 1.15f;	// a little padding, so stepping in fogs
	if ( flNorm >= flShell )
		return;

	float flIn = clamp( ( flShell - flNorm ) / ( flShell - flCore ), 0.0f, 1.0f );
	flIn = flIn * flIn * ( 3.0f - 2.0f * flIn );	// smoothstep, no hard rim

	// Median-patch fade: the overlay follows what a mid-dissolve patch shows,
	// not the unstaggered global — otherwise the insider stays fogged while
	// staring through a clear hole.
	float flMedT = clamp( m_flFadeT * 1.35f - 0.175f, 0.0f, 1.0f );
	float flFogAlpha = 1.0f - flMedT * flMedT * ( 3.0f - 2.0f * flMedT );

	// Ramp with the bloom, so the fog can't appear before the cloud does.
	if ( m_SpacingRadius > 0.0f )
		flFogAlpha *= clamp( m_ExpandRadius / ( m_SpacingRadius * 2.0f ), 0.0f, 1.0f );

	// A carve hole you're looking through clears the screen fog too (two-way
	// carve). The bullet hole is a view cone anchored at the eye, so sample a
	// few points along the *view ray*: testing only the cloud centre cleared
	// the fog just when the hole happened to line up behind it, not when you
	// shoot a hole in the smoke directly in front of you.
	float flHole = 0.0f;
	{
		Vector vEye = MainViewOrigin();
		Vector vFwd = MainViewForward();
		float flReach = MAX( 1.0f, ( m_SmokeBasePos - vEye ).Length() );
		for ( int i = 1; i <= 3; i++ )
		{
			Vector vSample = vEye + vFwd * ( flReach * ( i / 3.0f ) );
			flHole = MAX( flHole, HoleSuppressAt( vSample ) );
		}
	}
	flFogAlpha *= 1.0f - flHole;

	EngineGetSmokeFogOverlayAlpha() += flFogAlpha * flIn;
}


void C_ParticleSmokeGrenade::UpdateSmokeTrail( float fTimeDelta )
{
	C_BaseEntity *pAimEnt = GetFollowedEntity();
	if ( pAimEnt )
	{
		Vector forward, right, up;

		// Update the smoke particle color.
		if(m_CurrentStage == 0)
		{
			m_SmokeTrail.m_StartColor = EngineGetLightForPoint(GetAbsOrigin()) * 0.5f;
			m_SmokeTrail.m_EndColor = m_SmokeTrail.m_StartColor;
		}

		// Spin the smoke trail.
		AngleVectors(pAimEnt->GetAbsAngles(), &forward, &right, &up);
		m_SmokeTrail.m_VelocityOffset = forward * 30 + GetAbsVelocity();

		m_SmokeTrail.SetLocalOrigin( GetAbsOrigin() );
		m_SmokeTrail.Update(fTimeDelta);
	}	
}


inline void C_ParticleSmokeGrenade::UpdateParticleDuringTrade( int iParticle, float fTimeDelta )
{
	SmokeParticleInfo *pInfo = &m_SmokeParticleInfos[iParticle];
	SmokeParticleInfo *pOther = &m_SmokeParticleInfos[pInfo->m_TradeIndex];
	Assert(pOther->m_TradeIndex == iParticle);
	
	// This makes sure the trade only gets updated once per frame.
	if(pInfo < pOther)
	{
		// Increment the trade clock..
		pInfo->m_TradeClock = (pOther->m_TradeClock += fTimeDelta);
		int x, y, z;
		GetParticleInfoXYZ(iParticle, x, y, z);
		Vector myPos = GetSmokeParticlePos(x, y, z) - m_SmokeBasePos;
		
		int otherX, otherY, otherZ;
		GetParticleInfoXYZ(pInfo->m_TradeIndex, otherX, otherY, otherZ);
		Vector otherPos = GetSmokeParticlePos(otherX, otherY, otherZ) - m_SmokeBasePos;

		// Is the trade finished?
		if(pInfo->m_TradeClock >= pInfo->m_TradeDuration)
		{
			pInfo->m_TradeIndex = pOther->m_TradeIndex = -1;
			
			pInfo->m_pParticle->m_Pos = otherPos;
			pOther->m_pParticle->m_Pos = myPos;

			SmokeGrenadeParticle *temp = pInfo->m_pParticle;
			pInfo->m_pParticle = pOther->m_pParticle;
			pOther->m_pParticle = temp;
		}
		else
		{			
			// Ok, move them closer.
			float percent = (float)cos(pInfo->m_TradeClock * 2 * 1.57079632f / pInfo->m_TradeDuration);
			percent = percent * 0.5 + 0.5;
			
			pInfo->m_pParticle->m_FadeAlpha  = pInfo->m_FadeAlpha + (pOther->m_FadeAlpha - pInfo->m_FadeAlpha) * (1 - percent);
			pOther->m_pParticle->m_FadeAlpha = pInfo->m_FadeAlpha + (pOther->m_FadeAlpha - pInfo->m_FadeAlpha) * percent;

			InterpColor(pInfo->m_pParticle->m_Color,  pInfo->m_Color, pOther->m_Color, 1-percent);
			InterpColor(pOther->m_pParticle->m_Color, pInfo->m_Color, pOther->m_Color, percent);

			pInfo->m_pParticle->m_Pos  = myPos + (otherPos - myPos) * (1 - percent);
			pOther->m_pParticle->m_Pos = myPos + (otherPos - myPos) * percent;
		}
	}
}


void C_ParticleSmokeGrenade::UpdateParticleAndFindTrade( int iParticle, float fTimeDelta )
{
	SmokeParticleInfo *pInfo = &m_SmokeParticleInfos[iParticle];

	pInfo->m_pParticle->m_FadeAlpha = pInfo->m_FadeAlpha;
	pInfo->m_pParticle->m_Color[0] = pInfo->m_Color[0];
	pInfo->m_pParticle->m_Color[1] = pInfo->m_Color[1];
	pInfo->m_pParticle->m_Color[2] = pInfo->m_Color[2];

	// Is there an adjacent one that's not trading?
	int x, y, z;
	GetParticleInfoXYZ(iParticle, x, y, z);

	int xCountOffset = rand();
	int yCountOffset = rand();
	int zCountOffset = rand();

	bool bFound = false;
	for(int xCount=0; xCount < 3 && !bFound; xCount++)
	{
		for(int yCount=0; yCount < 3 && !bFound; yCount++)
		{
			for(int zCount=0; zCount < 3; zCount++)
			{
				int testX = x + g_OffsetLookup[(xCount+xCountOffset) % 3];
				int testY = y + g_OffsetLookup[(yCount+yCountOffset) % 3];
				int testZ = z + g_OffsetLookup[(zCount+zCountOffset) % 3];

				if(testX == x && testY == y && testZ == z)
					continue;

				if(IsValidXYZCoords(testX, testY, testZ))
				{
					SmokeParticleInfo *pOther = GetSmokeParticleInfo(testX, testY, testZ);
					if(pOther->m_pParticle && pOther->m_TradeIndex == -1)
					{
						// The straight trade path must not saw through a
						// wall — otherwise churn visibly clips cards through
						// doorframes around corners. Blocked? Keep looking.
						Vector vMine = m_SmokeBasePos + pInfo->m_pParticle->m_Pos;
						Vector vTheirs = m_SmokeBasePos + pOther->m_pParticle->m_Pos;
						trace_t trTrade;
						WorldTraceLine( vMine, vTheirs, MASK_SOLID_BRUSHONLY, &trTrade );
						if ( trTrade.fraction < 1.0f && !trTrade.startsolid )
							continue;

						// Ok, this one is looking to trade also.
						pInfo->m_TradeIndex = GetSmokeParticleIndex(testX, testY, testZ);
						pOther->m_TradeIndex = iParticle;
						pInfo->m_TradeClock = pOther->m_TradeClock = 0;
						pInfo->m_TradeDuration = FRand(TRADE_DURATION_MIN, TRADE_DURATION_MAX);
						
						bFound = true;
						break;
					}
				}
			}
		}
	}
}


void C_ParticleSmokeGrenade::Update(float fTimeDelta)
{
	float flLifetime = gpGlobals->curtime - m_flSpawnTime;

	// Turning the reactive system off must drop any live holes too, or an
	// existing tunnel keeps suppressing puffs for its full 1.4-3s lifetime
	// while the cvar reads 0 (docs/smoke-reactive.md promises "never punch").
	if ( !smoke_reactive_enable.GetBool() )
		m_nCarveHoles = 0;

	// Update the smoke trail.
	UpdateSmokeTrail( fTimeDelta );

	if(m_CurrentStage == 1)
	{
		// Bloom: fast cubic-out fill with a faint pressure overshoot, then
		// settle — the CS2 "whoomph" instead of the old slow sine swell.
		float flBloomTime = MAX( 0.5f, smoke_bloom_time.GetFloat() );
		m_ExpandTimeCounter = flLifetime;
		if(m_ExpandTimeCounter > flBloomTime)
			m_ExpandTimeCounter = flBloomTime;

		float flT = clamp( m_ExpandTimeCounter / flBloomTime, 0.0f, 1.0f );
		m_flBloomEase = 1.0f - (1.0f - flT) * (1.0f - flT) * (1.0f - flT);
		float flOvershoot = 1.0f + 0.06f * sin( M_PI * m_flBloomEase ) * ( 1.0f - m_flBloomEase );

		m_ExpandRadius = (m_SpacingRadius*2) * m_flBloomEase * flOvershoot;

//		debugoverlay->AddBoxOverlay( GetPos(), Vector( -m_ExpandRadius, -m_ExpandRadius, -m_ExpandRadius), Vector( m_ExpandRadius, m_ExpandRadius, m_ExpandRadius), vec3_angle, 0, 255, 0, 1, 1.0f );
	}

	// Update our fade alpha. Smoothstep hold-then-dissolve: dense until the
	// fade window opens, then a steady thin-out. Per-puff stagger (in
	// RenderParticles) breaks the cloud up patchily on top of this.
	if(flLifetime < m_FadeStartTime)
	{
		m_flFadeT = 0.0f;
		m_FadeAlpha = 1;
	}
	else if(flLifetime < m_FadeEndTime)
	{
		float flFadeSpan = MAX( 0.001f, m_FadeEndTime - m_FadeStartTime );
		m_flFadeT = (flLifetime - m_FadeStartTime) / flFadeSpan;
		float flS = m_flFadeT * m_flFadeT * (3.0f - 2.0f * m_flFadeT);
		m_FadeAlpha = 1.0f - flS;
	}
	else
	{
		m_flFadeT = 1.0f;
		m_FadeAlpha = 0;
	}

	// Scale by the amount the sphere has grown, so the cloud blooms in
	// instead of popping to full density on the first frame.
	if ( m_SpacingRadius > 0.0f )
		m_FadeAlpha *= m_ExpandRadius / (m_SpacingRadius*2);

	
	// Update our bbox.

	Vector vMins = m_SmokeBasePos - Vector( m_SpacingRadius + SMOKEGRENADE_PARTICLERADIUS, m_SpacingRadius + SMOKEGRENADE_PARTICLERADIUS, m_SpacingRadius + SMOKEGRENADE_PARTICLERADIUS );
	Vector vMaxs = m_SmokeBasePos + Vector( m_SpacingRadius + SMOKEGRENADE_PARTICLERADIUS, m_SpacingRadius + SMOKEGRENADE_PARTICLERADIUS, m_SpacingRadius + SMOKEGRENADE_PARTICLERADIUS );
	m_ParticleEffect.SetBBox( vMins, vMaxs );


	// Update the current light list.
	UpdateDynamicLightList( vMins, vMaxs );


	if(m_CurrentStage == 1)
	{
		// Retire spent carve holes (swap-remove; order doesn't matter).
		for ( int i = 0; i < m_nCarveHoles; )
		{
			if ( gpGlobals->curtime >= m_CarveHoles[i].flEndTime )
				m_CarveHoles[i] = m_CarveHoles[--m_nCarveHoles];
			else
				i++;
		}

		// Update all the moving traders and establish new ones.
		int nTotal = m_xCount * m_yCount * m_zCount;
		for(int i=0; i < nTotal; i++)
		{
			SmokeParticleInfo *pInfo = &m_SmokeParticleInfos[i];

			if(!pInfo->m_pParticle)
				continue;
		
			if(pInfo->m_TradeIndex == -1)
			{
				UpdateParticleAndFindTrade( i, fTimeDelta );
			}
			else
			{
				UpdateParticleDuringTrade( i, fTimeDelta );
			}
		}
	}

	m_SmokeBasePos = GetPos() + m_vecBaseLift;
}


void C_ParticleSmokeGrenade::UpdateDynamicLightList( const Vector &vMins, const Vector &vMaxs )
{
	dlight_t *lights[MAX_DLIGHTS];
	int nLights = effects->CL_GetActiveDLights( lights );
	m_nActiveLights = 0;
	for ( int i=0; i < nLights; i++ )
	{
		dlight_t *pIn = lights[i];
		if ( pIn->origin.x + pIn->radius <= vMins.x || 
			 pIn->origin.y + pIn->radius <= vMins.y || 
			 pIn->origin.z + pIn->radius <= vMins.z || 
			 pIn->origin.x - pIn->radius >= vMaxs.x || 
			 pIn->origin.y - pIn->radius >= vMaxs.y || 
			 pIn->origin.z - pIn->radius >= vMaxs.z )
		{
		}
		else
		{
			CActiveLight *pOut = &m_ActiveLights[m_nActiveLights];
			if ( (pIn->color.r != 0 || pIn->color.g != 0 || pIn->color.b != 0) && pIn->color.exponent != 0 )
			{
				ColorRGBExp32ToVector( pIn->color, pOut->m_vColor );
				pOut->m_vColor /= 255.0f;
				pOut->m_flRadiusSqr = (pIn->radius + SMOKEPARTICLE_SIZE) * (pIn->radius + SMOKEPARTICLE_SIZE);
				pOut->m_vOrigin = pIn->origin;
				++m_nActiveLights;
			}
		}
	}
}


inline void C_ParticleSmokeGrenade::ApplyDynamicLight( const Vector &vParticlePos, Vector &color )
{
	if ( m_nActiveLights )
	{
		for ( int i=0; i < m_nActiveLights; i++ )
		{
			CActiveLight *pLight = &m_ActiveLights[i];

			float flDistSqr = (vParticlePos - pLight->m_vOrigin).LengthSqr();
			if ( flDistSqr < pLight->m_flRadiusSqr )
			{
				color += pLight->m_vColor * (1 - flDistSqr / pLight->m_flRadiusSqr) * 0.1f;
			}
		}
	
		// Rescale the color..
		float flMax = MAX( color.x, MAX( color.y, color.z ) );
		if ( flMax > 1 )
		{
			color /= flMax;
		}
	}
}


// CS2-style carve, see docs/smoke-reactive.md. Every shot leaves a round hole
// in the cloud. The hole is stored as a world-space centre + radius; at render
// (and fog) time we test whether the sight line from the eye to a point falls
// inside the cone that hole subtends from the camera. Puffs inside the cone get
// cleared, so the hole goes through the whole cloud depth and always reads as a
// circle you can see the map behind. The eye is the cone's apex, so the hole
// looks right from any angle, and the per-hole pop/hold/refill curve deforms it.
float C_ParticleSmokeGrenade::HoleSuppressAt( const Vector &vWorldPos, float flPuffRadius )
{
	if ( m_nCarveHoles <= 0 )
		return 0.0f;

	CParticleMgr *pMgr = ParticleMgr();
	if ( !pMgr )
		return 0.0f;
	const VMatrix &mv = pMgr->GetModelView();
	Vector vP;
	TransformParticle( mv, vWorldPos, vP );

	float flNow = gpGlobals->curtime;
	float flBest = 0.0f;

	for ( int i = 0; i < m_nCarveHoles; i++ )
	{
		const SmokeHole_t &h = m_CarveHoles[i];

		float flAge = flNow - h.flBirth;
		float flLeft = h.flEndTime - flNow;
		if ( flLeft <= 0.0f )
			continue;

		// Open fast (~0.12s), hold, then refill over the tail of the recover
		// window. The pop uses birth (so a spray that keeps pushing the expiry
		// back never re-ramps it), and the feather uses the fixed recover time
		// (so a long-held hole doesn't dim as its age grows).
		float flPop = clamp( flAge / 0.12f, 0.0f, 1.0f );
		float flFade = clamp( flLeft / MAX( 0.01f, h.flRecover * 0.6f ), 0.0f, 1.0f );

		if ( h.bExplosion )
		{
			// HE: a world-space blast sphere — clears from every angle,
			// including inside the cloud (CS2 grenades reveal with it).
			float flDist = ( vWorldPos - h.vCenter ).Length();
			float flClear = ( h.flRadius + flPuffRadius ) - flDist;
			if ( flClear <= 0.0f )
				continue;
			// Feather over the outer half of the blast (was 30%): with the
			// clear radius near the cloud's own size the old narrow shell was
			// fully saturated across the whole cloud, so an HE wiped it out
			// instead of biting a visible round hole into it.
			float flS = clamp( flClear / MAX( 1.0f, h.flRadius * 0.5f ), 0.0f, 1.0f );
			flBest = MAX( flBest, flS * h.flStrength * flPop * flFade );
			continue;
		}

		// Bullet: view-cone hole. Transforms to view space (eye at the
		// origin) so the hole is always a circle from any angle.
		Vector vC;
		TransformParticle( mv, h.vCenter, vC );
		float flCLen = vC.Length();
		if ( flCLen < 1.0f )
			continue;

		Vector vDir = vC / flCLen;
		float flAlong = DotProduct( vP, vDir );
		if ( flAlong <= 0.0f )
			continue;			// behind the hole, not through it

		Vector vPerp = vP - vDir * flAlong;
		float flLateral = vPerp.Length();
		float flHoleLat = ( h.flRadius / flCLen ) * flAlong;	// cone half-width here
		// Cap the cone (~35 deg half-angle): standing right at the hole must
		// not blank the whole screen.
		flHoleLat = MIN( flHoleLat, flAlong * 0.7f );
		// Pad with half the puff's half-size so the card covering the hole
		// centre stays suppressed without clearing neighbours two-wide.
		// Full padding was what ballooned single shots across the cloud.
		flHoleLat += flPuffRadius * SMOKE_HOLE_PUFF_SCALE;
		float flEdge = flHoleLat * SMOKE_HOLE_FATTEN;
		if ( flLateral >= flEdge )
			continue;

		// Hard core, short feather to the edge.
		float flCore = flHoleLat * 0.8f;
		float flS = ( flLateral <= flCore )
			? 1.0f
			: 1.0f - ( flLateral - flCore ) / MAX( 1.0f, flEdge - flCore );

		flBest = MAX( flBest, clamp( flS * h.flStrength * flPop * flFade, 0.0f, 1.0f ) );
	}

	return flBest;
}


void C_ParticleSmokeGrenade::RenderParticles( CParticleRenderIterator *pIterator )
{
	const SmokeGrenadeParticle *pParticle = (const SmokeGrenadeParticle*)pIterator->GetFirst();

	// Hoisted: ConVar reads don't belong in the per-puff loop.
	float flCutoff = clamp( smoke_core.GetFloat(), 0.2f, 0.8f );
	float flBright = smoke_brightness.GetFloat();
	float flCloudRadius = MAX( 1.0f, m_SpacingRadius * 2.0f );

	// The cloud is lifted to sit on the ground, so the bloom has to radiate
	// from the detonation point itself, not from that lifted centre —
	// otherwise the smoke materialises in mid-air as a ball and swells,
	// instead of growing up and out of the ground where it landed.
	Vector vBloomOrigin = m_SmokeBasePos - m_vecBaseLift;
	float flRevealBand = MAX( 1.0f, m_SpacingRadius * 0.5f );

	while ( pParticle )
	{
		Vector vWorldSpacePos = m_SmokeBasePos + pParticle->m_Pos;

		float sortKey;

		// Draw. Ellipsoidal metric so the squashed cloud feathers evenly
		// on all axes instead of ending in a dense flat top. The falloff
		// is anchored below center (~55% of the half-height) so the dense
		// core sits at head height over the dirt — a centered ball reads
		// as hovering and leaves the ground wispy. This describes the final
		// cloud *shape*, so it's measured against the full radius, not the
		// growing bloom radius.
		Vector vEll = pParticle->m_Pos;
		vEll.z /= MAX( 0.3f, m_flHeightScale );
		vEll.z += m_SpacingRadius * m_flHeightScale * 0.55f;
		float len = vEll.Length();

		// Bloom reveal: a wavefront radiating from the detonation point. The
		// puffs keep their true molded positions and only their alpha ramps
		// as the front passes, so the volume fills outward from the ground
		// instead of popping into a ball around the lifted centre. The short
		// band is the soft edge of the front — puffs don't snap on as it
		// crosses them.
		float flRevealDist = ( vWorldSpacePos - vBloomOrigin ).Length();
		float flRevealReach = ( flCloudRadius * 1.15f ) * m_flBloomEase;
		float flReveal = clamp( ( flRevealReach - flRevealDist ) / flRevealBand, 0.0f, 1.0f );
		flReveal = flReveal * flReveal * ( 3.0f - 2.0f * flReveal );

		if ( flReveal <= 0.0f )
		{
			Vector vTemp;
			TransformParticle(ParticleMgr()->GetModelView(), vWorldSpacePos, vTemp);
			sortKey = vTemp.z;		
		}
		else
		{
			// Puffs stay where they molded — no pull toward a growing ball.
			Vector renderPos = vWorldSpacePos;

			// Figure out the alpha based on where it is in the cloud.
			float alpha = MAX( 0.0f, 1 - len / flCloudRadius );
			
			// Dense volumetric core that feathers out softly. The core
			// fraction is tunable live (smoke_core).
			if(alpha > flCutoff)
			{
				alpha = 1;
			}
			else
			{
				// at flCutoff it's 1, at 0, it's 0 — smoothstepped so the
				// silhouette melts instead of banding.
				alpha = alpha / flCutoff;
				alpha = alpha * alpha * (3.0f - 2.0f * alpha);
			}

			// Fade out globally.
			alpha *= m_FadeAlpha;

			// Patchy dissolve: each puff leads or lags the global fade by
			// up to ~35% of the window (seeded by its color variation), so
			// the cloud breaks apart instead of shrinking as a ball.
			float flSeed = pParticle->m_ColorInterp / 255.0f;
			float flLocalT = clamp( m_flFadeT * 1.35f - flSeed * 0.35f, 0.0f, 1.0f );
			alpha *= 1.0f - flLocalT * flLocalT * (3.0f - 2.0f * flLocalT);

			// Apply the precalculated fade alpha from world geometry.
			alpha *= pParticle->m_FadeAlpha;

			// Bloom front edge: a puff eases in as the wavefront reaches it.
			alpha *= flReveal;

			// Per-puff density variation so the outer shell isn't uniform.
			alpha *= pParticle->m_flDensity;

			// Puff size drives the carve test too: a card's drawn extent
			// covers the hole even when its centre is outside it, so the
			// suppressor gets the rendered half-size (bloom + fade included).
			float flSize = pParticle->m_flSize * (0.55f + 0.45f * m_flBloomEase) * (1.0f + 0.25f * m_flFadeT);

			// CS2-style reactive carve: punch a round hole you can see through.
			// Test the card's drawn position, not its local-space grid slot.
			// Holes are event-driven, so a full-auto burst used to pulse the
			// cloud as each shot opened a fresh hole. Smooth the per-puff
			// suppression instead: quick to open (a shot still reads
			// instantly), slower to close, so a spray doesn't flicker.
			{
				SmokeGrenadeParticle *pMutable = const_cast<SmokeGrenadeParticle*>( pParticle );
				float flHoleSuppress = ( m_nCarveHoles > 0 ) ? HoleSuppressAt( renderPos, flSize ) : 0.0f;
				float flRate = ( flHoleSuppress > pMutable->m_flSuppress ) ? 16.0f : 5.0f;
				pMutable->m_flSuppress = Approach( flHoleSuppress, pMutable->m_flSuppress, gpGlobals->frametime * flRate );
				if ( pMutable->m_flSuppress > 0.0f )
					alpha *= ( 1.0f - pMutable->m_flSuppress );
			}

			// TODO: optimize this whole routine!
			Vector color = (m_MinColor + (m_MaxColor - m_MinColor) * (pParticle->m_ColorInterp / 255.1f)) * flBright;
			color.x *= pParticle->m_Color[0] / 255.0f;
			color.y *= pParticle->m_Color[1] / 255.0f;
			color.z *= pParticle->m_Color[2] / 255.0f;

			// Lighting.
			ApplyDynamicLight( renderPos, color );

			// Grey floor: CS2 smoke stays grey even under a bridge —
			// slightly cool, never soot. Warm-tinted maps shouldn't paint
			// the cloud orange; this pins the base channel at grey. Kept
			// low enough that map lighting still reads: at 0.62 the floor
			// swallowed nearly every indoor light value and the cloud came
			// out a flat single grey with no top/bottom/edge shading.
			color.x = MAX( color.x, 0.45f );
			color.y = MAX( color.y, 0.45f );
			color.z = MAX( color.z, 0.45f );

			// Kill residual hue: CS2 smoke is neutral grey. Lighting tints
			// the puffs, so unify toward luminance — but only gently, or it
			// flattens the shading the floor above just let through.
			float flLum = color.x * 0.3f + color.y * 0.59f + color.z * 0.11f;
			color += (Vector( flLum, flLum, flLum ) - color) * 0.6f;
			color.x = clamp( color.x, 0.0f, 1.0f );
			color.y = clamp( color.y, 0.0f, 1.0f );
			color.z = clamp( color.z, 0.0f, 1.0f );
			
			Vector tRenderPos;
			TransformParticle(ParticleMgr()->GetModelView(), renderPos, tRenderPos);
			sortKey = tRenderPos.z;

			//debugoverlay->AddBoxOverlay( renderPos, Vector( -2, -2, -2), Vector( 2, 2, 2), vec3_angle, 255, 255, 255, 255, 1.0f );

			// Draw if there is any alpha left; a carve hole can clear a puff.
			if ( alpha > 0.001f )
			{
				// Fade a card that is right on top of the eye, or standing in
				// the cloud draws the nearest 92u billboard as a full-screen
				// opaque quad (defeats the see-through hole). Stock did this.
				alpha *= GetAlphaDistanceFade( tRenderPos, 0, 10 );

				// The per-puff density jitter reaches 1.05 (see FillVolume), so
				// a fully-dense core puff can land above 1.0. RenderParticle_*
				// casts alpha * 254.9 straight to an unsigned char, so anything
				// over 1 wraps to a near-zero alpha and the puff drops out —
				// sprinkling permanent holes through the dense core. Clamp.
				alpha = clamp( alpha, 0.0f, 1.0f );

				if ( alpha > 0.001f )
				{
					RenderParticle_ColorSizeAngle(
						pIterator->GetParticleDraw(),
						tRenderPos,
						color,
						alpha,
						flSize,
						pParticle->m_CurRotation
						);
				}
			}
		}

		pParticle = (SmokeGrenadeParticle*)pIterator->GetNext( sortKey );
	}
}


void C_ParticleSmokeGrenade::SimulateParticles( CParticleSimulateIterator *pIterator )
{
	SmokeGrenadeParticle *pParticle = (SmokeGrenadeParticle*)pIterator->GetFirst();
	while ( pParticle )
	{
		pParticle->m_CurRotation += pParticle->m_RotationSpeed * pIterator->GetTimeDelta();
		pParticle = (SmokeGrenadeParticle*)pIterator->GetNext();
	}
}


void C_ParticleSmokeGrenade::NotifyRemove()
{
	m_xCount = m_yCount = m_zCount = 0;

#if CSTRIKE_DLL
	C_CSPlayer *pPlayer = C_CSPlayer::GetLocalCSPlayer();

	if ( pPlayer )
	{
		 pPlayer->m_SmokeGrenades.FindAndRemove( this );
	}
#endif

}


void C_ParticleSmokeGrenade::GetParticlePosition( Particle *pParticle, Vector& worldpos )
{
	worldpos = pParticle->m_Pos + m_SmokeBasePos;
}


void C_ParticleSmokeGrenade::RecvProxy_CurrentStage(  const CRecvProxyData *pData, void *pStruct, void *pOut )
{
	C_ParticleSmokeGrenade *pGrenade = (C_ParticleSmokeGrenade*)pStruct;
	Assert( pOut == &pGrenade->m_CurrentStage );

	if ( pGrenade && pGrenade->m_CurrentStage == 0 && pData->m_Value.m_Int == 1 )
	{
		if( pGrenade->m_bStarted )
			pGrenade->FillVolume();
		else
			pGrenade->m_CurrentStage = 2;
	}
}

void C_ParticleSmokeGrenade::FillVolume()
{
	m_CurrentStage = 1;
	m_SmokeBasePos = GetPos();
	m_SmokeTrail.SetEmit(false);
	m_ExpandTimeCounter = m_ExpandRadius = 0;
	m_flBloomEase = 0;
	m_flFadeT = 0;
	m_vecBaseLift.Init();
	m_bVolumeFilled = true;
	m_nCarveHoles = 0;

	// Spawn all of our particles. Denser than stock (6x6x6 = 216 puffs) at
	// roughly the same world size, so the cloud reads as one volume. The
	// whole cloud (spread + puff size) scales with smoke_scale, so density
	// is preserved when the size is dialed.
	float overlap = SMOKEPARTICLE_OVERLAP;
	float flScale = smoke_scale.GetFloat();

	m_SpacingRadius = (SMOKEGRENADE_PARTICLERADIUS - overlap) * NUM_PARTICLES_PER_DIMENSION * 0.5f * flScale;
	m_xCount = m_yCount = m_zCount = NUM_PARTICLES_PER_DIMENSION;
	m_flHeightScale = smoke_mold_enable.GetBool() ? SMOKE_CLOUD_HEIGHT_SCALE : 1.0f;

	bool bMold = smoke_mold_enable.GetBool();
	float flHz = m_SpacingRadius * m_flHeightScale;

	// CS2-style molding: snap the cloud onto the ground and squash it under
	// low ceilings, so smoke hugs floors and fills rooms instead of burying
	// a third of its puffs underground or poking through the floor above.
	float flGroundZ = m_SmokeBasePos.z;
	bool bHaveGround = false;
	if ( bMold )
	{
		trace_t trGround;
		WorldTraceLine( m_SmokeBasePos + Vector( 0, 0, 32 ), m_SmokeBasePos - Vector( 0, 0, 220 ), MASK_SOLID_BRUSHONLY, &trGround );
		if ( trGround.fraction < 1.0f && !trGround.startsolid )
		{
			flGroundZ = trGround.endpos.z;
			bHaveGround = true;
		}

		trace_t trCeil;
		WorldTraceLine( m_SmokeBasePos, m_SmokeBasePos + Vector( 0, 0, 340 ), MASK_SOLID_BRUSHONLY, &trCeil );
		if ( bHaveGround && trCeil.fraction < 1.0f && !trCeil.startsolid )
		{
			float flAvail = trCeil.endpos.z - flGroundZ;
			float flHzFit = MAX( 30.0f, ( flAvail - 24.0f ) * 0.5f );
			if ( flHzFit < flHz )
			{
				// Clamp the scale where it is stored: RenderParticles derives its
				// alpha metric from MAX( 0.3, m_flHeightScale ), so a raw 0.28
				// under a very low ceiling would draw geometry squashed more
				// than the falloff that shapes it. flHz follows the clamped
				// scale so the ground lift stays consistent.
				m_flHeightScale = MAX( 0.3f, flHzFit / m_SpacingRadius );
				flHz = m_flHeightScale * m_SpacingRadius;
			}
		}

		if ( bHaveGround )
		{
			// Bottom row rests just above the dirt; Update() re-applies
			// this lift every frame via m_SmokeBasePos.
			m_vecBaseLift.Init( 0, 0, ( flGroundZ + flHz + 10.0f ) - m_SmokeBasePos.z );
			m_SmokeBasePos += m_vecBaseLift;
		}
	}

	// Per-column ground heights so the sheet rides slopes, stairs and crate
	// tops instead of clipping through them.
	float flColGround[NUM_PARTICLES_PER_DIMENSION][NUM_PARTICLES_PER_DIMENSION];
	for ( int gx = 0; gx < NUM_PARTICLES_PER_DIMENSION; gx++ )
	{
		for ( int gy = 0; gy < NUM_PARTICLES_PER_DIMENSION; gy++ )
		{
			flColGround[gx][gy] = m_SmokeBasePos.z - flHz - 160.0f;
			if ( !bMold )
				continue;

			// Start at the cloud top, which the ceiling squash guarantees
			// is inside the room — any higher and the probe begins inside
			// the floor above and collapses the whole column to it.
			Vector vColTop(
				m_SmokeBasePos.x + ((float)gx / (NUM_PARTICLES_PER_DIMENSION-1)) * m_SpacingRadius * 2 - m_SpacingRadius,
				m_SmokeBasePos.y + ((float)gy / (NUM_PARTICLES_PER_DIMENSION-1)) * m_SpacingRadius * 2 - m_SpacingRadius,
				m_SmokeBasePos.z + flHz );
			trace_t trCol;
			WorldTraceLine( vColTop, vColTop - Vector( 0, 0, flHz + 220.0f ), MASK_SOLID_BRUSHONLY, &trCol );
			if ( trCol.fraction < 1.0f && !trCol.startsolid )
				flColGround[gx][gy] = trCol.endpos.z;
		}
	}

	float invNumPerDimX = 1.0f / (m_xCount-1);
	float invNumPerDimY = 1.0f / (m_yCount-1);
	float invNumPerDimZ = 1.0f / (m_zCount-1);

	Vector vPos;
	for(int x=0; x < m_xCount; x++)
	{
		vPos.x = m_SmokeBasePos.x + ((float)x * invNumPerDimX) * m_SpacingRadius * 2 - m_SpacingRadius;

		for(int y=0; y < m_yCount; y++)
		{
			vPos.y = m_SmokeBasePos.y + ((float)y * invNumPerDimY) * m_SpacingRadius * 2 - m_SpacingRadius;
							  
			for(int z=0; z < m_zCount; z++)
			{
				vPos.z = m_SmokeBasePos.z + ((float)z * invNumPerDimZ) * m_SpacingRadius * m_flHeightScale * 2 - m_SpacingRadius * m_flHeightScale;

				if(SmokeParticleInfo *pInfo = GetSmokeParticleInfo(x,y,z))
				{
					pInfo->m_pParticle = NULL;
					pInfo->m_bMolded = false;
					pInfo->m_TradeIndex = -1;
					pInfo->m_FadeAlpha = 1.0f;

					// Low-end lever: checkerboard the grid (108 puffs).
					// After init so skipped cells stay clean NULLs.
					if ( mat_reduceparticles.GetBool() && ( ( x + y + z ) & 1 ) )
						continue;

					Vector vMolded = vPos;

					if ( bMold )
					{
						// Inside a wall? Slide back to a fixed kiss inset off
						// the surface, so smoke hugs cover at any distance
						// instead of retreating a fraction of the ray (which
						// left a visible gap on far walls). Still stuck
						// (fully buried cells) means no puff at all.
						// Brush-only: players and props move, the mold must
						// not keep their shape after they leave.
						if ( GetWorldPointContents( vMolded ) & CONTENTS_SOLID )
						{
							trace_t trPull;
							WorldTraceLine( m_SmokeBasePos, vMolded, MASK_SOLID_BRUSHONLY, &trPull );
							if ( trPull.fraction < 1.0f && !trPull.startsolid )
							{
								Vector vDir = vMolded - m_SmokeBasePos;
								float flLen = vDir.Length();
								if ( flLen > 1.0f )
									vDir /= flLen;
								else
									vDir.Init( 0, 0, 1 );
								vMolded = trPull.endpos - vDir * 12.0f;
							}
							if ( GetWorldPointContents( vMolded ) & CONTENTS_SOLID )
								continue;
						}

						// Ride the terrain: hang the sheet slightly below
						// its column's ground so the bottom row kisses the
						// dirt (and low cover pockets stay filled) instead
						// of floating a sprite above it.
						float flMinZ = flColGround[x][y] - 12.0f;
						if ( vMolded.z < flMinZ )
							vMolded.z = flMinZ;
					}

					// Billow, don't voxel: nudge each puff a little so the
					// union of wispy cards reads as cloud, not a regular
					// lattice with a clean geometric silhouette. The mold
					// still wins — jitter never pushes a card through the
					// floor or into a wall.
					{
						Vector vClean = vMolded;
						vMolded.x += FRand( -14.0f, 14.0f ) * flScale;
						vMolded.y += FRand( -14.0f, 14.0f ) * flScale;
						vMolded.z += FRand( -10.0f, 10.0f ) * flScale;
						if ( bMold )
						{
							float flMinZJit = flColGround[x][y] - 12.0f;
							if ( vMolded.z < flMinZJit )
								vMolded.z = flMinZJit;
							if ( GetWorldPointContents( vMolded ) & CONTENTS_SOLID )
								vMolded = vClean;
						}
					}

					{
						SmokeGrenadeParticle *pParticle = 
							(SmokeGrenadeParticle*)m_ParticleEffect.AddParticle(sizeof(SmokeGrenadeParticle), m_MaterialHandles[rand() % NUM_MATERIAL_HANDLES]);

						if(pParticle)
						{
							pParticle->m_Pos = vMolded - m_SmokeBasePos; // store its position in local space
							pParticle->m_ColorInterp = (unsigned char)( rand() % 256 );
							pParticle->m_RotationSpeed = FRand(-ROTATION_SPEED, ROTATION_SPEED); // Rotation speed.
							pParticle->m_CurRotation = FRand(-6, 6);
							pParticle->m_flSize = SMOKEPARTICLE_SIZE * flScale * FRand( 0.7f, 1.3f );
							pParticle->m_flDensity = FRand( 0.65f, 1.05f );
							pParticle->m_flSuppress = 0.0f;

							//debugoverlay->AddBoxOverlay( vMolded, Vector( -2, -2, -2), Vector( 2, 2, 2), vec3_angle, 255, 0, 0, 255, 5.0f );
						}

						

						#ifdef _DEBUG
							int testX, testY, testZ;
							int index = GetSmokeParticleIndex(x,y,z);
							GetParticleInfoXYZ(index, testX, testY, testZ);
							assert(testX == x && testY == y && testZ == z);
						#endif

						// Sample lighting just above the dirt, never inside it:
						// the sheet hangs slightly under the surface and a
						// buried sample comes back black, painting the whole
						// ground layer as soot instead of smoke.
						Vector vLightPos = vMolded;
						if ( bMold )
							vLightPos.z = MAX( vLightPos.z, flColGround[x][y] + 4.0f );
						Vector vColor = EngineGetLightForPoint(vLightPos);
						pInfo->m_Color[0] = (unsigned char)(vColor.x * 255.9f);
						pInfo->m_Color[1] = (unsigned char)(vColor.y * 255.9f);
						pInfo->m_Color[2] = (unsigned char)(vColor.z * 255.9f);

						// Soften puffs kissing geometry: short axial probes
						// fade cells whose centers sit almost inside a
						// surface, so cards don't visibly slice through
						// doorframes and crates at grazing angles.
						pInfo->m_FadeAlpha = 1;
						if ( bMold )
						{
							for(int i=0; i < (int)NUM_FADE_PLANES; i++)
							{
								trace_t trace;
								WorldTraceLine(vMolded, vMolded + s_FadePlaneDirections[i] * 32, MASK_SOLID_BRUSHONLY, &trace);
								if(trace.fraction < 1.0f)
								{
									float flDist = trace.fraction * 32.0f;
									if(flDist < 1.0f)
									{
										pInfo->m_FadeAlpha = 0;
										break;
									}
									else if(flDist < 24.0f)
									{
										float flEdge = flDist / 24.0f;
										flEdge = flEdge * flEdge * (3.0f - 2.0f * flEdge);
										pInfo->m_FadeAlpha *= flEdge;
									}
								}
							}
						}

						pInfo->m_MoldPos = vMolded - m_SmokeBasePos;
						pInfo->m_bMolded = true;
						pInfo->m_pParticle = pParticle;
						pInfo->m_TradeIndex = -1;
					}
				}
			}
		}
	}
}

//-----------------------------------------------------------------------------
// This is called after sending this entity's recording state
//-----------------------------------------------------------------------------
void C_ParticleSmokeGrenade::CleanupToolRecordingState( KeyValues *msg )
{
	if ( !ToolsEnabled() )
		return;

	BaseClass::CleanupToolRecordingState( msg );
	m_SmokeTrail.CleanupToolRecordingState( msg );

	// Generally, this is used to allow the entity to clean up
	// allocated state it put into the message, but here we're going
	// to use it to send particle system messages because we
	// know the grenade has been recorded at this point
	if ( !clienttools->IsInRecordingMode() )
		return;
	
	// NOTE: Particle system destruction message will be sent by the particle effect itself.
	if ( m_bVolumeFilled && GetToolParticleEffectId() == TOOLPARTICLESYSTEMID_INVALID )
	{
		// Needed for retriggering of the smoke grenade
		m_bVolumeFilled = false;

		int nId = AllocateToolParticleEffectId();

		KeyValues *msg = new KeyValues( "OldParticleSystem_Create" );
		msg->SetString( "name", "C_ParticleSmokeGrenade" );
		msg->SetInt( "id", nId );
		msg->SetFloat( "time", gpGlobals->curtime );

		KeyValues *pEmitter = msg->FindKey( "DmeSpriteEmitter", true );
		pEmitter->SetInt( "count", NUM_PARTICLES_PER_DIMENSION * NUM_PARTICLES_PER_DIMENSION * NUM_PARTICLES_PER_DIMENSION );
		pEmitter->SetFloat( "duration", 0 );
		pEmitter->SetString( "material", "particle/smokesprites_0001" );
		pEmitter->SetInt( "active", true );

		KeyValues *pInitializers = pEmitter->FindKey( "initializers", true );

		KeyValues *pPosition = pInitializers->FindKey( "DmeVoxelPositionInitializer", true );
		pPosition->SetFloat( "centerx", m_SmokeBasePos.x );
		pPosition->SetFloat( "centery", m_SmokeBasePos.y );
		pPosition->SetFloat( "centerz", m_SmokeBasePos.z );
		pPosition->SetFloat( "particlesPerDimension", m_xCount );
		pPosition->SetFloat( "particleSpacing", m_SpacingRadius );

		KeyValues *pLifetime = pInitializers->FindKey( "DmeRandomLifetimeInitializer", true );
		pLifetime->SetFloat( "minLifetime", m_FadeEndTime );
 		pLifetime->SetFloat( "maxLifetime", m_FadeEndTime );

		KeyValues *pVelocity = pInitializers->FindKey( "DmeAttachmentVelocityInitializer", true );
		pVelocity->SetPtr( "entindex", (void*)(intp)entindex() );
 		pVelocity->SetFloat( "minRandomSpeed", 10 );
 		pVelocity->SetFloat( "maxRandomSpeed", 20 );

		KeyValues *pRoll = pInitializers->FindKey( "DmeRandomRollInitializer", true );
		pRoll->SetFloat( "minRoll", -6.0f );
 		pRoll->SetFloat( "maxRoll", 6.0f );

		KeyValues *pRollSpeed = pInitializers->FindKey( "DmeRandomRollSpeedInitializer", true );
		pRollSpeed->SetFloat( "minRollSpeed", -ROTATION_SPEED );
 		pRollSpeed->SetFloat( "maxRollSpeed", ROTATION_SPEED );

		KeyValues *pColor = pInitializers->FindKey( "DmeRandomInterpolatedColorInitializer", true );
		Color c1( 
			FastFToC( clamp( m_MinColor.x, 0.f, 1.f ) ),
			FastFToC( clamp( m_MinColor.y, 0.f, 1.f ) ),
			FastFToC( clamp( m_MinColor.z, 0.f, 1.f ) ), 255 );
		Color c2( 
			FastFToC( clamp( m_MaxColor.x, 0.f, 1.f ) ),
			FastFToC( clamp( m_MaxColor.y, 0.f, 1.f ) ),
			FastFToC( clamp( m_MaxColor.z, 0.f, 1.f ) ), 255 );
		pColor->SetColor( "color1", c1 );
		pColor->SetColor( "color2", c2 );

		KeyValues *pAlpha = pInitializers->FindKey( "DmeRandomAlphaInitializer", true );
		pAlpha->SetInt( "minStartAlpha", 255 );
		pAlpha->SetInt( "maxStartAlpha", 255 );
		pAlpha->SetInt( "minEndAlpha", 0 );
		pAlpha->SetInt( "maxEndAlpha", 0 );

		KeyValues *pSize = pInitializers->FindKey( "DmeRandomSizeInitializer", true );
		pSize->SetFloat( "minStartSize", SMOKEPARTICLE_SIZE );
		pSize->SetFloat( "maxStartSize", SMOKEPARTICLE_SIZE );
		pSize->SetFloat( "minEndSize", SMOKEPARTICLE_SIZE );
		pSize->SetFloat( "maxEndSize", SMOKEPARTICLE_SIZE );

		pInitializers->FindKey( "DmeSolidKillInitializer", true );

		KeyValues *pUpdaters = pEmitter->FindKey( "updaters", true );

		pUpdaters->FindKey( "DmeRollUpdater", true );
		pUpdaters->FindKey( "DmeColorUpdater", true );

		KeyValues *pAlphaCosineUpdater = pUpdaters->FindKey( "DmeAlphaCosineUpdater", true );
		pAlphaCosineUpdater->SetFloat( "duration", m_FadeEndTime - m_FadeStartTime );
		
		pUpdaters->FindKey( "DmeColorDynamicLightUpdater", true );

		KeyValues *pSmokeGrenadeUpdater = pUpdaters->FindKey( "DmeSmokeGrenadeUpdater", true );
 		pSmokeGrenadeUpdater->SetFloat( "centerx", m_SmokeBasePos.x );
		pSmokeGrenadeUpdater->SetFloat( "centery", m_SmokeBasePos.y );
		pSmokeGrenadeUpdater->SetFloat( "centerz", m_SmokeBasePos.z );
		pSmokeGrenadeUpdater->SetFloat( "particlesPerDimension", m_xCount );
		pSmokeGrenadeUpdater->SetFloat( "particleSpacing", m_SpacingRadius );
		pSmokeGrenadeUpdater->SetFloat( "radiusExpandTime", SMOKESPHERE_EXPAND_TIME );
		pSmokeGrenadeUpdater->SetFloat( "cutoffFraction", 0.7f );

		ToolFramework_PostToolMessage( HTOOLHANDLE_INVALID, msg );
		msg->deleteThis();
	}
}


//-----------------------------------------------------------------------------
// CS2-style reactive smoke (see docs/smoke-reactive.md). Client-only: carve
// the local copy of every active smoke, no server or networking changes.
//-----------------------------------------------------------------------------
// Punch a round hole into the cloud. Holes are kept in a small ring; the one
// closest to refilling is recycled when full.
void C_ParticleSmokeGrenade::AddCarveHole( const Vector &vCenter, float flRadius, float flStrength, float flLife, bool bExplosion )
{
	// A full-auto burst used to add a brand-new hole per shot, so the cloud
	// pulsed as holes popped open and the oldest slot got recycled onto a new
	// spot. Merge a bullet into a nearby bullet hole instead: the spray keeps
	// one hole open (and growing) for as long as the rounds land close, which
	// is both the CS2 behaviour and the end of the flicker.
	if ( !bExplosion )
	{
		for ( int i = 0; i < m_nCarveHoles; i++ )
		{
			SmokeHole_t &h = m_CarveHoles[i];
			if ( h.bExplosion )
				continue;

			float flMergeDist = MAX( h.flRadius, flRadius ) * 0.9f;
			if ( ( vCenter - h.vCenter ).Length() <= flMergeDist )
			{
				// Keep flBirth: re-birthing would restart the pop ramp, so a
				// held burst never let the hole open. Only push the expiry out
				// (and take the wider/stronger of the two).
				h.vCenter = vCenter;						// follow the spray
				h.flRadius = MAX( h.flRadius, flRadius );
				h.flStrength = MAX( h.flStrength, MIN( 1.0f, flStrength ) );
				h.flRecover = MAX( h.flRecover, flLife );
				h.flEndTime = gpGlobals->curtime + h.flRecover;
				return;
			}
		}
	}

	int nSlot;
	if ( m_nCarveHoles < MAX_CARVE_HOLES )
	{
		nSlot = m_nCarveHoles++;
	}
	else
	{
		nSlot = 0;
		for ( int i = 1; i < MAX_CARVE_HOLES; i++ )
			if ( m_CarveHoles[i].flEndTime < m_CarveHoles[nSlot].flEndTime )
				nSlot = i;
	}

	SmokeHole_t &h = m_CarveHoles[nSlot];
	h.vCenter = vCenter;
	h.bExplosion = bExplosion;
	h.flRadius = flRadius;
	h.flStrength = MIN( 1.0f, flStrength );
	h.flRecover = MAX( 0.05f, flLife );
	h.flBirth = gpGlobals->curtime;
	h.flEndTime = gpGlobals->curtime + h.flRecover;
}


void C_ParticleSmokeGrenade::ApplyBulletSegment( const Vector &vecStart, const Vector &vecEnd )
{
	if ( m_CurrentStage != 1 || !smoke_reactive_enable.GetBool() )
		return;

	float flRadius = smoke_bullet_radius.GetFloat();
	float flStrength = smoke_bullet_strength.GetFloat();
	if ( flRadius <= 0.0f || flStrength <= 0.0f )
		return;

	// Quick reject: the segment misses the whole cloud.
	float flCloudRadius = m_SpacingRadius * 2.0f + SMOKEPARTICLE_SIZE;
	if ( ReactiveSmokeDistPointToSegment( m_SmokeBasePos, vecStart, vecEnd ) > flCloudRadius + flRadius )
		return;

	// Hole centre: the point on the bullet's path nearest the middle of the
	// cloud, so the hole opens where the round crossed the smoke (its
	// midpoint can be far outside on a long shot into a wall behind it).
	Vector vCenter = ReactiveSmokeClosestPointOnSegment( m_SmokeBasePos, vecStart, vecEnd );

	// Keep the centre inside the cloud so a grazing shot still drills in.
	Vector vToCenter = vCenter - m_SmokeBasePos;
	float flLen = vToCenter.Length();
	float flMaxOff = MAX( 1.0f, m_SpacingRadius * 0.9f );
	if ( flLen > flMaxOff )
		vCenter = m_SmokeBasePos + vToCenter * ( flMaxOff / MAX( 1.0f, flLen ) );

	// Bullet: clear every card whose world footprint covers the opening.
	// HoleSuppressAt feeds each puff its rendered half-size so neighbours
	// can't overdraw the gap shut (see docs/smoke-reactive.md).
	AddCarveHole( vCenter, flRadius, flStrength, MAX( 0.2f, smoke_bullet_recover.GetFloat() ), false );
	if ( smoke_debug.GetBool() )
		Msg( "[smoke] bullet hole at (%.0f %.0f %.0f) r=%.0f, %d hole(s) live\n",
			vCenter.x, vCenter.y, vCenter.z, flRadius, m_nCarveHoles );
}


void C_ParticleSmokeGrenade::ApplyExplosion( const Vector &vecCenter )
{
	if ( m_CurrentStage != 1 || !smoke_reactive_enable.GetBool() )
		return;

	float flRadius = smoke_he_radius.GetFloat();
	float flStrength = smoke_he_strength.GetFloat();
	if ( flRadius <= 0.0f || flStrength <= 0.0f )
		return;

	float flCloudRadius = m_SpacingRadius * 2.0f + SMOKEPARTICLE_SIZE;
	if ( ( m_SmokeBasePos - vecCenter ).Length() > flCloudRadius + flRadius )
		return;

	// HE clears a sphere, not a view cone: same look from every angle, and
	// grenades thrown into the cloud reveal from inside too. The clear is
	// shaped in world space at the blast (see docs/smoke-reactive.md).
	if ( smoke_debug.GetBool() )
		Msg( "[smoke] HE clear at (%.0f %.0f %.0f) r=%.0f\n", vecCenter.x, vecCenter.y, vecCenter.z, flRadius );
	AddCarveHole( vecCenter, flRadius, flStrength, MAX( 0.5f, smoke_he_recover.GetFloat() ), true );
}


#if CSTRIKE_DLL
static int ReactiveSmoke_ForEachSmoke( void (*pfn)( C_ParticleSmokeGrenade*, void* ), void *pCtx )
{
	C_CSPlayer *pPlayer = C_CSPlayer::GetLocalCSPlayer();
	if ( !pPlayer )
		return 0;

	int nCount = 0;
	for ( int i = 0; i < pPlayer->m_SmokeGrenades.Count(); i++ )
	{
		C_ParticleSmokeGrenade *pSmoke = dynamic_cast< C_ParticleSmokeGrenade* >( (C_BaseParticleEntity*)pPlayer->m_SmokeGrenades.Element( i ) );
		if ( pSmoke )
		{
			pfn( pSmoke, pCtx );
			nCount++;
		}
	}
	return nCount;
}

struct ReactiveSmoke_SegmentCtx_t
{
	const Vector *pStart;
	const Vector *pEnd;
};

static void ReactiveSmoke_ApplySegment( C_ParticleSmokeGrenade *pSmoke, void *pCtx )
{
	ReactiveSmoke_SegmentCtx_t *pSegment = (ReactiveSmoke_SegmentCtx_t*)pCtx;
	pSmoke->ApplyBulletSegment( *pSegment->pStart, *pSegment->pEnd );
}

static void ReactiveSmoke_ApplyBlast( C_ParticleSmokeGrenade *pSmoke, void *pCtx )
{
	pSmoke->ApplyExplosion( *(const Vector*)pCtx );
}
#endif // CSTRIKE_DLL


// Called from CCSPlayer::FireBullet (client) for every traced bullet segment,
// hits and misses alike, so shooting through smoke leaves a tunnel.
void ReactiveSmoke_OnBulletSegment( const Vector &vecStart, const Vector &vecEnd )
{
	if ( !smoke_reactive_enable.GetBool() )
		return;

#if CSTRIKE_DLL
	ReactiveSmoke_SegmentCtx_t ctx = { &vecStart, &vecEnd };
	ReactiveSmoke_ForEachSmoke( ReactiveSmoke_ApplySegment, &ctx );
#endif
}


// Called from ClientModeCSNormal on hegrenade_detonate. The clear refills in
// place over smoke_he_recover seconds.
void ReactiveSmoke_OnExplosion( const Vector &vecCenter )
{
	if ( !smoke_reactive_enable.GetBool() )
		return;

	// A zero vector means the event carried no origin (bad key/old demo) —
	// never nuke the smoke sitting at the map origin by mistake.
	if ( vecCenter.IsZero() )
		return;

#if CSTRIKE_DLL
	ReactiveSmoke_ForEachSmoke( ReactiveSmoke_ApplyBlast, (void*)&vecCenter );
#endif
}



