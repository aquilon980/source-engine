//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose: Joystick and gamepad support has been REMOVED entirely.
//
// This file used to hold every analog-stick ConVar, the whole stick response
// curve, autoaim dampening, advanced-axis mapping and the gamepad config exec
// machinery. All of it is gone: no cvars, no polling, no config exec.
//
// What remains are no-op bodies for the handful of methods IInput still
// declares pure virtual, so the client keeps compiling and linking.
//
//=============================================================================//

#include "cbase.h"
#include "input.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

// A joystick or gamepad is never present.
bool CInput::EnableJoystickMode()
{
	return false;
}

// Called once for advanced setup; nothing to set up.
void CInput::Joystick_Advanced(void)
{
}

// Joystick button events were never pumped here (windprocs); still nothing.
void CInput::ControllerCommands( void )
{
}

void CInput::Joystick_SetSampleTime(float frametime)
{
}

float CInput::Joystick_GetForward( void )
{
	return 0.0f;
}

float CInput::Joystick_GetSide( void )
{
	return 0.0f;
}

float CInput::Joystick_GetPitch( void )
{
	return 0.0f;
}

float CInput::Joystick_GetYaw( void )
{
	return 0.0f;
}
