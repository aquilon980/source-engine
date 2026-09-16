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


// CS2-style reactive smoke (see docs/smoke-reactive.md): bullets punch
// short-lived tunnels, HE blasts clear a sphere that refills in place.
// Client-only visuals — the server sim and bot radius are untouched.
static ConVar smoke_reactive_enable( "smoke_reactive_enable", "1", FCVAR_ARCHIVE, "CS2-style smoke: bullets carve holes, HE blasts clear smoke that refills." );
static ConVar smoke_bullet_radius( "smoke_bullet_radius", "60", FCVAR_ARCHIVE, "Radius around a bullet path that thins smoke.", true, 4.0f, true, 160.0f );
static ConVar smoke_bullet_strength( "smoke_bullet_strength", "0.85", FCVAR_ARCHIVE, "How much smoke one bullet clears (0-1).", true, 0.0f, true, 1.0f );
static ConVar smoke_bullet_recover( "smoke_bullet_recover", "1.4", FCVAR_ARCHIVE, "Seconds for a bullet hole to refill.", true, 0.2f, true, 10.0f );
static ConVar smoke_he_radius( "smoke_he_radius", "290", FCVAR_ARCHIVE, "Radius of the HE smoke clear.", true, 50.0f, true, 800.0f );
static ConVar smoke_he_strength( "smoke_he_strength", "1.0", FCVAR_ARCHIVE, "How much smoke an HE blast clears (0-1).", true, 0.0f, true, 1.0f );
static ConVar smoke_he_recover( "smoke_he_recover", "3.0", FCVAR_ARCHIVE, "Seconds for HE-cleared smoke to refill.", true, 0.5f, true, 15.0f );

// Volumetric CS2-style smoke (see docs/smoke-volumetric.md): the cloud molds
// to the ground and walls, blooms fast with a soft overshoot, and dissolves
// patchily instead of shrinking as a ball. Client-only visuals.
static ConVar smoke_mold_enable( "smoke_mold_enable", "1", FCVAR_ARCHIVE, "Smoke molds to the ground/walls instead of clipping through them." );
static ConVar smoke_bloom_time( "smoke_bloom_time", "1.4", FCVAR_ARCHIVE, "Seconds for the smoke cloud to bloom to full size.", true, 0.5f, true, 3.0f );
static ConVar smoke_core( "smoke_core", "0.45", FCVAR_ARCHIVE, "Fraction of the cloud that stays fully dense (soft edge outside it).", true, 0.2f, true, 0.8f );
static ConVar smoke_brightness( "smoke_brightness", "1.0", FCVAR_ARCHIVE, "Smoke puff brightness multiplier.", true, 0.4f, true, 1.6f );

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
		float				m_Suppress;			// CS2-style carve: 0 = full smoke, 1 = cleared.
		float				m_flSize;			// Base puff size (bloom + fade scale it).
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
		float					m_BulletSuppress;	// CS2-style: bullet carve, refills fast.
		float					m_BlastSuppress;	// CS2-style: HE clear, refills slow.
		Vector					m_MoldPos;			// Molded local offset (wall-pulled, ground-clamped).
		bool					m_bMolded;			// False for culled (inside-solid) cells.
		unsigned char			m_Color[4];
	};

	void ApplyDynamicLight( const Vector &vParticlePos, Vector &color );
	void UpdateDynamicLightList( const Vector &vMins, const Vector &vMaxs );

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

	// Volumetric look (see docs/smoke-volumetric.md): bright CS2-style grey
	// with a faint cool lift. World lighting tints it per-puff at spawn.
	m_MinColor.Init(0.80, 0.81, 0.84);
	m_MaxColor.Init(0.90, 0.91, 0.94);

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

	// Stock soft-smoke cards (UnlitGeneric) — the only thing the old quad
	// emitter can feed correctly. SpriteCard materials need TEXCOORD1-4
	// (radius/rotation/corner IDs) that RenderParticle_ColorSizeAngle never
	// writes, so they collapse to degenerate quads here. Wall/floor blending
	// comes from the CPU-side mold + edge fade below, not the material.
	// Two densities for a little texture variety across the cloud.
	m_MaterialHandles[0] = m_ParticleEffect.FindOrAddMaterial("particle/particle_smokegrenade1");
	m_MaterialHandles[1] = m_ParticleEffect.FindOrAddMaterial("particle/particle_smokegrenade");

	if( m_CurrentStage == 2 )
	{
		FillVolume();
	}

	// Go straight into "fill volume" mode if they want.
	if(pArgs)
	{
		if(pArgs->FindArg("-FillVolume"))
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
	if ( m_CurrentStage == 1 )
	{
		// Add our influence to the global smoke fog alpha.
		
		float testDist = (MainViewOrigin() - m_SmokeBasePos ).Length();

		float fadeEnd = m_ExpandRadius;

		// The center of the smoke cloud that always gives full fog overlay
		float flCoreDistance = fadeEnd * 0.3;
		
		if(testDist < fadeEnd)
		{
			// CS2-style: carved/cleared smoke fogs the screen less. Average the
			// per-cell clear amounts so the overlay tracks the visible puffs.
			float flSuppressSum = 0.0f;
			float flMaxSuppress = 0.0f;
			int nSuppressCount = 0;
			int nTotalFog = m_xCount * m_yCount * m_zCount;
			for ( int iFog = 0; iFog < nTotalFog; iFog++ )
			{
				SmokeParticleInfo *pFogInfo = &m_SmokeParticleInfos[iFog];
				if ( !pFogInfo->m_pParticle )
					continue;
				float flCellSuppress = MAX( pFogInfo->m_BulletSuppress, pFogInfo->m_BlastSuppress );
				flSuppressSum += flCellSuppress;
				flMaxSuppress = MAX( flMaxSuppress, flCellSuppress );
				nSuppressCount++;
			}
			// Median-patch fade: the overlay follows what a mid-dissolve
			// patch shows, not the unstaggered global — otherwise the
			// insider stays fogged while staring through a clear hole.
			float flMedT = clamp( m_flFadeT * 1.35f - 0.175f, 0.0f, 1.0f );
			float flFogAlpha = 1.0f - flMedT * flMedT * ( 3.0f - 2.0f * flMedT );
			if ( m_SpacingRadius > 0.0f )
				flFogAlpha *= m_ExpandRadius / (m_SpacingRadius*2);
			if ( nSuppressCount > 0 )
			{
				flFogAlpha *= 1.0f - ( flSuppressSum / (float)nSuppressCount );
				// Two-way carve: the clearest hole clears the insider's
				// fog too, so a tunnel works both ways like CS2.
				flFogAlpha *= 1.0f - 0.5f * flMaxSuppress;
			}

			if( testDist < flCoreDistance )
			{
				EngineGetSmokeFogOverlayAlpha() += flFogAlpha;
			}
			else
			{
				EngineGetSmokeFogOverlayAlpha() += (1 - ( testDist - flCoreDistance ) / ( fadeEnd - flCoreDistance ) ) * flFogAlpha;
			}
		}	
	}
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

			// CS2-style: the carve amount rides along with the trade.
			float flSuppressA = MAX( pInfo->m_BulletSuppress, pInfo->m_BlastSuppress );
			float flSuppressB = MAX( pOther->m_BulletSuppress, pOther->m_BlastSuppress );
			pInfo->m_pParticle->m_Suppress  = flSuppressA + (flSuppressB - flSuppressA) * (1 - percent);
			pOther->m_pParticle->m_Suppress = flSuppressA + (flSuppressB - flSuppressA) * percent;

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

	// CS2-style: carry the carve amount onto the visible particle.
	pInfo->m_pParticle->m_Suppress = MAX( pInfo->m_BulletSuppress, pInfo->m_BlastSuppress );

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
		// Update all the moving traders and establish new ones.
		int nTotal = m_xCount * m_yCount * m_zCount;
		float flBulletStep = fTimeDelta / MAX( 0.2f, smoke_bullet_recover.GetFloat() );
		float flBlastStep = fTimeDelta / MAX( 0.5f, smoke_he_recover.GetFloat() );
		for(int i=0; i < nTotal; i++)
		{
			SmokeParticleInfo *pInfo = &m_SmokeParticleInfos[i];

			if(!pInfo->m_pParticle)
				continue;

			// CS2-style refill: bullet holes close fast, HE clears linger.
			if ( pInfo->m_BulletSuppress > 0.0f )
				pInfo->m_BulletSuppress = MAX( 0.0f, pInfo->m_BulletSuppress - flBulletStep );
			if ( pInfo->m_BlastSuppress > 0.0f )
				pInfo->m_BlastSuppress = MAX( 0.0f, pInfo->m_BlastSuppress - flBlastStep );
		
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


void C_ParticleSmokeGrenade::RenderParticles( CParticleRenderIterator *pIterator )
{
	const SmokeGrenadeParticle *pParticle = (const SmokeGrenadeParticle*)pIterator->GetFirst();

	// Hoisted: ConVar reads don't belong in the per-puff loop.
	float flCutoff = clamp( smoke_core.GetFloat(), 0.2f, 0.8f );
	float flBright = smoke_brightness.GetFloat();

	while ( pParticle )
	{
		Vector vWorldSpacePos = m_SmokeBasePos + pParticle->m_Pos;

		float sortKey;

		// Draw. Ellipsoidal metric so the squashed cloud feathers evenly
		// on all axes instead of ending in a dense flat top.
		Vector vEll = pParticle->m_Pos;
		vEll.z /= MAX( 0.3f, m_flHeightScale );
		float len = vEll.Length();
		if ( len > m_ExpandRadius )
		{
			Vector vTemp;
			TransformParticle(ParticleMgr()->GetModelView(), vWorldSpacePos, vTemp);
			sortKey = vTemp.z;		
		}
		else
		{
			// This smooths out the growing sphere. Rather than having particles appear in one spot as the sphere
			// expands, they stay at the borders.
			Vector renderPos;
			if(len > m_ExpandRadius * 0.5f)
			{
				renderPos = m_SmokeBasePos + (pParticle->m_Pos * (m_ExpandRadius * 0.5f)) / len;
			}
			else
			{
				renderPos = vWorldSpacePos;
			}		

			// Figure out the alpha based on where it is in the sphere.
			float alpha = 1 - len / m_ExpandRadius;
			
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

			// CS2-style reactive smoke: carved holes thin the puff.
			alpha *= ( 1.0f - pParticle->m_Suppress );

			// TODO: optimize this whole routine!
			Vector color = (m_MinColor + (m_MaxColor - m_MinColor) * (pParticle->m_ColorInterp / 255.1f)) * flBright;
			color.x *= pParticle->m_Color[0] / 255.0f;
			color.y *= pParticle->m_Color[1] / 255.0f;
			color.z *= pParticle->m_Color[2] / 255.0f;

			// Lighting.
			ApplyDynamicLight( renderPos, color );

			// Gentle unify toward luminance keeps the smoke reading as one
			// grey volume while preserving a breath of the ambient hue —
			// the old (color + grey)/2 wash just dragged everything to mud.
			float flLum = color.x * 0.3f + color.y * 0.59f + color.z * 0.11f;
			color += (Vector( flLum, flLum, flLum ) - color) * 0.35f;
			color.x = clamp( color.x, 0.0f, 1.0f );
			color.y = clamp( color.y, 0.0f, 1.0f );
			color.z = clamp( color.z, 0.0f, 1.0f );
			
			Vector tRenderPos;
			TransformParticle(ParticleMgr()->GetModelView(), renderPos, tRenderPos);
			sortKey = tRenderPos.z;

			//debugoverlay->AddBoxOverlay( renderPos, Vector( -2, -2, -2), Vector( 2, 2, 2), vec3_angle, 255, 255, 255, 255, 1.0f );

			// Skip only puffs a carve has fully erased, so unsuppressed
			// cells always draw (no holes unless something carved one).
			if ( pParticle->m_Suppress <= 0.0f || alpha > 0.001f )
			{
			// Puffs inflate as the cloud blooms and breathe outward as it
			// dissipates, like a real volume exchanging with the air.
			float flSize = pParticle->m_flSize * (0.55f + 0.45f * m_flBloomEase) * (1.0f + 0.25f * m_flFadeT);
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

	// Spawn all of our particles. Denser than stock (6x6x6 = 216 puffs) at
	// roughly the same world size, so the cloud reads as one volume.
	float overlap = SMOKEPARTICLE_OVERLAP;

	m_SpacingRadius = (SMOKEGRENADE_PARTICLERADIUS - overlap) * NUM_PARTICLES_PER_DIMENSION * 0.5f;
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
				m_flHeightScale = flHzFit / m_SpacingRadius;
				flHz = flHzFit;
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
					pInfo->m_BulletSuppress = 0.0f;
					pInfo->m_BlastSuppress = 0.0f;
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

					{
						SmokeGrenadeParticle *pParticle = 
							(SmokeGrenadeParticle*)m_ParticleEffect.AddParticle(sizeof(SmokeGrenadeParticle), m_MaterialHandles[rand() % NUM_MATERIAL_HANDLES]);

						if(pParticle)
						{
							pParticle->m_Pos = vMolded - m_SmokeBasePos; // store its position in local space
							pParticle->m_ColorInterp = (unsigned char)((rand() * 255) / VALVE_RAND_MAX);
							pParticle->m_RotationSpeed = FRand(-ROTATION_SPEED, ROTATION_SPEED); // Rotation speed.
							pParticle->m_CurRotation = FRand(-6, 6);
							pParticle->m_Suppress = 0.0f;
							pParticle->m_flSize = SMOKEPARTICLE_SIZE * FRand( 0.85f, 1.15f );

							//debugoverlay->AddBoxOverlay( vMolded, Vector( -2, -2, -2), Vector( 2, 2, 2), vec3_angle, 255, 0, 0, 255, 5.0f );
						}

						

						#ifdef _DEBUG
							int testX, testY, testZ;
							int index = GetSmokeParticleIndex(x,y,z);
							GetParticleInfoXYZ(index, testX, testY, testZ);
							assert(testX == x && testY == y && testZ == z);
						#endif

						Vector vColor = EngineGetLightForPoint(vMolded);
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
		pEmitter->SetString( "material", "particle/particle_smokegrenade1" );
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

	int nTotal = m_xCount * m_yCount * m_zCount;
	for ( int i = 0; i < nTotal; i++ )
	{
		SmokeParticleInfo *pInfo = &m_SmokeParticleInfos[i];
		if ( !pInfo->m_pParticle )
			continue;

		Vector vWorldPos = m_SmokeBasePos + pInfo->m_pParticle->m_Pos;
		float flDist = ReactiveSmokeDistPointToSegment( vWorldPos, vecStart, vecEnd );
		if ( flDist < flRadius )
		{
			float flFall = 1.0f - flDist / flRadius;
			flFall *= flFall;
			pInfo->m_BulletSuppress = MIN( 1.0f, pInfo->m_BulletSuppress + flStrength * flFall );
			// Snap the visible copy so the hole punches this frame, not next.
			pInfo->m_pParticle->m_Suppress = MAX( pInfo->m_BulletSuppress, pInfo->m_BlastSuppress );
		}
	}
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

	int nTotal = m_xCount * m_yCount * m_zCount;
	for ( int i = 0; i < nTotal; i++ )
	{
		SmokeParticleInfo *pInfo = &m_SmokeParticleInfos[i];
		if ( !pInfo->m_pParticle )
			continue;

		Vector vWorldPos = m_SmokeBasePos + pInfo->m_pParticle->m_Pos;
		float flDist = ( vWorldPos - vecCenter ).Length();
		if ( flDist < flRadius )
		{
			float flFall = 1.0f - flDist / flRadius;
			pInfo->m_BlastSuppress = MIN( 1.0f, pInfo->m_BlastSuppress + flStrength * flFall );
			pInfo->m_pParticle->m_Suppress = MAX( pInfo->m_BulletSuppress, pInfo->m_BlastSuppress );
		}
	}
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



