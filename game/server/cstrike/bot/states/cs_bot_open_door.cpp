//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose: 
//
// $NoKeywords: $
//=============================================================================//

// Author: Michael S. Booth (mike@turtlerockstudios.com), April 2005

#include "cbase.h"
#include "cs_bot.h"
#include "BasePropDoor.h"
#include "doors.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"


//-------------------------------------------------------------------------------------------------
/**
 * Face the door and open it.
 * NOTE: This state assumes we are standing in range of the door to be opened, with no obstructions.
 */
void OpenDoorState::OnEnter( CCSBot *me )
{
	m_isDone = false;
	m_timeout.Start( 1.0f );
}


//-------------------------------------------------------------------------------------------------
void OpenDoorState::SetDoor( CBaseEntity *door )
{
	// clear both handles first - otherwise a bot that has ever opened a
	// func_door keeps taking the m_funcDoor branch and never looks at the
	// prop_door_rotating in front of it (the handles are CHandles, so they
	// only null on entity deletion, and func_doors persist)
	m_funcDoor = NULL;
	m_propDoor = NULL;

	CBaseDoor *funcDoor = dynamic_cast< CBaseDoor * >(door);
	if ( funcDoor )
	{
		m_funcDoor = funcDoor;
		return;
	}

	CBasePropDoor *propDoor = dynamic_cast< CBasePropDoor * >(door);
	if ( propDoor )
	{
		m_propDoor = propDoor;
		return;
	}
}


//-------------------------------------------------------------------------------------------------
void OpenDoorState::OnUpdate( CCSBot *me )
{
	me->ResetStuckMonitor();

	// wait for door to swing open before leaving state
	if (m_timeout.IsElapsed())
	{
		m_isDone = true;
		return;
	}

	// look at the door
	Vector pos;
	bool isDoorMoving = false;
	if ( m_funcDoor )
	{
		pos = m_funcDoor->WorldSpaceCenter();
		isDoorMoving = m_funcDoor->m_toggle_state == TS_GOING_UP || m_funcDoor->m_toggle_state == TS_GOING_DOWN;
	}
	else if ( m_propDoor )
	{
		pos = m_propDoor->WorldSpaceCenter();
		isDoorMoving = m_propDoor->IsDoorOpening() || m_propDoor->IsDoorClosing();
	}
	else
	{
		// door vanished between SetDoor and now
		m_isDone = true;
		return;
	}

	me->SetLookAt( "Open door", pos, PRIORITY_HIGH );

	// if we are looking at the door, "use" it and exit
	if (me->IsLookingAtPosition( pos ))
	{
		me->UseEnvironment();
	}
}


//-------------------------------------------------------------------------------------------------
void OpenDoorState::OnExit( CCSBot *me )
{
	me->ClearLookAt();
	me->ResetStuckMonitor();
}



