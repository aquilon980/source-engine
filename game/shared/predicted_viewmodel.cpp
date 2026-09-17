//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose:
//
//=============================================================================//
#include "cbase.h"
#include "predicted_viewmodel.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

LINK_ENTITY_TO_CLASS( predicted_viewmodel, CPredictedViewModel );

IMPLEMENT_NETWORKCLASS_ALIASED( PredictedViewModel, DT_PredictedViewModel )

BEGIN_NETWORK_TABLE( CPredictedViewModel, DT_PredictedViewModel )
END_NETWORK_TABLE()

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
#ifdef CLIENT_DLL
CPredictedViewModel::CPredictedViewModel() : m_LagAnglesHistory("CPredictedViewModel::m_LagAnglesHistory")
{
	m_vLagAngles.Init();
	m_LagAnglesHistory.Setup( &m_vLagAngles, 0 );
	m_vPredictedOffset.Init();
	m_vLoweredWeaponOffset.Init();
}
#else
CPredictedViewModel::CPredictedViewModel()
{
}
#endif


//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
CPredictedViewModel::~CPredictedViewModel()
{
}

#ifdef CLIENT_DLL
ConVar cl_wpn_sway_interp( "cl_wpn_sway_interp", "0.1", FCVAR_CLIENTDLL );
ConVar cl_wpn_sway_scale( "cl_wpn_sway_scale", "0.6", FCVAR_CLIENTDLL|FCVAR_CHEAT );
#endif

void CPredictedViewModel::CalcViewModelLag( Vector& origin, QAngle& angles, QAngle& original_angles )
{
	#ifdef CLIENT_DLL
		// Calculate our drift
		Vector	forward, right, up;
		AngleVectors( angles, &forward, &right, &up );

		// Add an entry to the history.
		m_vLagAngles = angles;
		m_LagAnglesHistory.NoteChanged( gpGlobals->curtime, cl_wpn_sway_interp.GetFloat(), false );

		// Interpolate back 100ms.
		m_LagAnglesHistory.Interpolate( gpGlobals->curtime, cl_wpn_sway_interp.GetFloat() );

		// Now take the 100ms angle difference and figure out how far the forward vector moved in local space.
		Vector vLaggedForward;
		QAngle angleDiff = m_vLagAngles - angles;
		AngleVectors( -angleDiff, &vLaggedForward, 0, 0 );
		Vector vForwardDiff = Vector(1,0,0) - vLaggedForward;

#ifdef WIN32 // ShouldFlipViewModel comes up unresolved on osx (defined inline in c_baseviewmodel.cpp) — same guard as baseviewmodel_shared.cpp
		if ( ShouldFlipViewModel() )
			right = -right;
#endif

		// Now offset the origin using that.
		vForwardDiff *= cl_wpn_sway_scale.GetFloat();
		m_vPredictedOffset = forward*vForwardDiff.x + right*-vForwardDiff.y + up*vForwardDiff.z;

		// Reduce the offset as the viewmodel approaches vertical (avoids wild swings looking straight up/down).
		float flMult = clamp( abs(DotProduct(up, Vector(0,0,1))) - 0.02f, 0, 1 );

		origin += (m_vPredictedOffset * flMult);
	#endif
}

#ifdef CLIENT_DLL
ConVar cl_gunlowerangle( "cl_gunlowerangle", "2", FCVAR_CLIENTDLL );
ConVar cl_gunlowerspeed( "cl_gunlowerspeed", "0.1", FCVAR_CLIENTDLL );
#endif //CLIENT_DLL

void CPredictedViewModel::ApplyViewModelPitchAndDip( CBasePlayer *owner, Vector& vecNewOrigin, QAngle& vecNewAngles )
{
	// Dips the weapon while airborne so jumps read in the gun, not just the camera.
	// (Upstream also lands a dip on touchdown, but that needs player fields this
	// tree doesn't have — m_bInLanding / m_flLandingTime — so it's left out.)
#ifdef CLIENT_DLL
	if ( !owner )
		return;

	bool bJumping = !(owner->GetFlags() & FL_ONGROUND);

	QAngle vecLoweredAngles( 0, 0, 0 );

	m_vLoweredWeaponOffset.x = Approach( bJumping ? cl_gunlowerangle.GetFloat() : 0, m_vLoweredWeaponOffset.x, cl_gunlowerspeed.GetFloat() );
	vecLoweredAngles.x += m_vLoweredWeaponOffset.x;
	vecNewAngles -= vecLoweredAngles * 0.2f;
	vecNewOrigin.z -= vecLoweredAngles.x * 0.4f; // translation offset looks more natural than rotation
#endif
}

void CPredictedViewModel::CalcViewModelView( CBasePlayer *owner, const Vector& eyePosition, const QAngle& eyeAngles )
{
#if defined( CLIENT_DLL )
	Vector vecNewOrigin = eyePosition;
	QAngle vecNewAngles = eyeAngles;

	ApplyViewModelPitchAndDip( owner, vecNewOrigin, vecNewAngles );

	BaseClass::CalcViewModelView( owner, vecNewOrigin, vecNewAngles );
#endif //CLIENT_DLL
}
