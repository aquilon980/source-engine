//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose: 
//
// $NoKeywords: $
//=============================================================================//

// Author: Michael S. Booth (mike@turtlerockstudios.com), 2003

#include "cbase.h"
#include "cs_bot.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

//--------------------------------------------------------------------------------------------------------------
/**
 * Begin defusing the bomb
 */
void DefuseBombState::OnEnter( CCSBot *me )
{
	me->SetDisposition( CCSBot::SELF_DEFENSE );
	me->GetChatter()->Say( "DefusingBomb" );

	// stop any residual path movement - we drive ourselves from here
	me->StandUp();
	me->ClearMovement();

	m_reachTimestamp = -1.0f;
	m_startTimestamp = -1.0f;
}


//--------------------------------------------------------------------------------------------------------------
/**
 * Defuse the bomb
 *
 * The engine only lets a CT +use the C4 from within 96 units of its origin
 * (measured eye->bomb) and while facing it (see CCSPlayer::FindUseEntity).
 * The old code checked a looser 100-unit range, traced LOS to the bomb's
 * floor-level origin, and gave up after one second if the defuse hadn't
 * started - so a bot that stopped at the edge of the range pressed +use at
 * nothing and walked away. Now we close the last few units ourselves, aim
 * with a tight tolerance, and only give up when someone else has the defuse,
 * the bomb is gone, or we truly can't reach it.
 */
void DefuseBombState::OnUpdate( CCSBot *me )
{
	const Vector *bombPos = me->GetGameState()->GetBombPosition();

	if (bombPos == NULL)
	{
		me->PrintIfWatched( "In Defuse state, but don't know where the bomb is!\n" );
		me->Idle();
		return;
	}

	// if bomb has been defused, give up
	if (!TheCSBots()->IsBombPlanted())
	{
		me->Idle();
		return;
	}

	// if someone else got the defuse, give up - don't stand on their head
	CCSPlayer *defuser = TheCSBots()->GetBombDefuser();
	if (defuser && defuser != me)
	{
		me->PrintIfWatched( "Someone else started defusing, giving up\n" );
		me->Idle();
		return;
	}

	// aim at a point just above the bomb (its origin sits on the floor, and a
	// trace straight to it can clip the ground)
	const Vector bombAim = *bombPos + Vector( 0, 0, 16.0f );

	// the engine's range test is eye -> bomb ORIGIN (CCSPlayer::FindUseEntity),
	// so measure the same thing here or we can think we're in range when the
	// engine disagrees
	const float defuseRange = 88.0f;		// engine needs < 96 from the eye
	float range = (*bombPos - me->EyePosition()).Length();

	me->SetLookAt( "Defuse bomb", bombAim, PRIORITY_HIGH, -1.0f, false, 2.0f );

	if (range > defuseRange)
	{
		// the path ended short of the bomb (nav endpoint vs exact position) -
		// close the last stretch instead of bouncing Idle <-> MoveTo forever
		me->StandUp();
		me->MoveForward();

		// window only counts while we're actually out of range, so a fight
		// interruption doesn't eat it
		if (m_reachTimestamp < 0.0f)
			m_reachTimestamp = gpGlobals->curtime;

		const float reachTimeout = 3.0f;
		if (gpGlobals->curtime - m_reachTimestamp > reachTimeout)
		{
			me->PrintIfWatched( "Can't reach the bomb, giving up\n" );
			me->Idle();
		}
		return;
	}

	// in range - reset the reach window and crouch down to defuse.  The engine
	// sets m_bIsDefusing once +use takes; use that rather than a state
	// timestamp, which does not reset when we resume after a firefight.
	m_reachTimestamp = -1.0f;
	me->Crouch();
	me->UseEnvironment();

	if (me->m_bIsDefusing)
	{
		// defuse is running - keep the view locked on and let it finish
		m_startTimestamp = -1.0f;
		return;
	}

	// +use hasn't taken yet.  The view may still be slewing onto the bomb, so
	// give it a real window before deciding we can't get it.
	if (m_startTimestamp < 0.0f)
		m_startTimestamp = gpGlobals->curtime;

	const float startTimeout = 2.5f;
	if (gpGlobals->curtime - m_startTimestamp > startTimeout)
	{
		me->PrintIfWatched( "Defuse didn't take, repositioning\n" );
		me->Idle();
	}
}

//--------------------------------------------------------------------------------------------------------------
void DefuseBombState::OnExit( CCSBot *me )
{
	me->StandUp();
	me->ResetStuckMonitor();
	me->SetTask( CCSBot::SEEK_AND_DESTROY );
	me->SetDisposition( CCSBot::ENGAGE_AND_INVESTIGATE );
	me->ClearLookAt();
}
