//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose: Joystick and gamepad support has been REMOVED entirely.
//
// This file used to be the SDL stick / game controller backend: it opened SDL
// game controllers, posted controller button/axis events, and drove rumble
// through SDL_Haptic. All of that is gone: no cvars, no SDL gamecontroller
// init, no event watch, no devices.
//
// The CInputSystem methods below stay as no-ops only because inputsystem.h
// still declares them and inputsystem.cpp still calls the lifecycle ones.
//
//==============================================================================//

#include "inputsystem.h"

// NOTE: This has to be the last file included!
#include "tier0/memdbgon.h"

void CInputSystem::InitializeJoysticks( void )
{
	// No joystick or gamepad is ever initialized.
	m_nJoystickCount = 0;
	m_bJoystickInitialized = false;
}

void CInputSystem::ShutdownJoysticks()
{
	m_nJoystickCount = 0;
	m_bJoystickInitialized = false;
}

void CInputSystem::JoystickHotplugAdded( int joystickIndex )
{
	(void)joystickIndex;
}

void CInputSystem::JoystickHotplugRemoved( int joystickId )
{
	(void)joystickId;
}

void CInputSystem::JoystickButtonPress( int joystickId, int button )
{
	(void)joystickId;
	(void)button;
}

void CInputSystem::JoystickButtonRelease( int joystickId, int button )
{
	(void)joystickId;
	(void)button;
}

void CInputSystem::JoystickAxisMotion( int joystickId, int axis, int value )
{
	(void)joystickId;
	(void)axis;
	(void)value;
}

//-----------------------------------------------------------------------------
//	Process the event
//-----------------------------------------------------------------------------
void CInputSystem::JoystickButtonEvent( ButtonCode_t button, int sample )
{
	// Not used - we post button events from JoystickButtonPress/Release.
	(void)button;
	(void)sample;
}


//-----------------------------------------------------------------------------
// Update the joystick button state
//-----------------------------------------------------------------------------
void CInputSystem::UpdateJoystickButtonState( int nJoystick )
{
	(void)nJoystick;
}


//-----------------------------------------------------------------------------
// Update the joystick POV control
//-----------------------------------------------------------------------------
void CInputSystem::UpdateJoystickPOVControl( int nJoystick )
{
	(void)nJoystick;
}


//-----------------------------------------------------------------------------
// Purpose: Sample the joystick
//-----------------------------------------------------------------------------
void CInputSystem::PollJoystick( void )
{
}

void CInputSystem::SetXDeviceRumble( float fLeftMotor, float fRightMotor, int userId )
{
	(void)fLeftMotor;
	(void)fRightMotor;
	(void)userId;
}
