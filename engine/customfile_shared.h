//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose: 
//
//=============================================================================//

#ifndef CUSTOMFILE_SHARED_H
#define CUSTOMFILE_SHARED_H
#ifdef _WIN32
#pragma once
#endif

#include "checksum_crc.h"

// Turns a CRC value into a filename.
class CCustomFilename
{
public:
	CCustomFilename( CRC32_t value ) 
	{
		char hex[16];
		Q_binarytohex( (byte *)&value, sizeof( value ), hex, sizeof( hex ) );
		Q_snprintf( m_Filename, sizeof( m_Filename ), "user_custom/%c%c/%s.dat", hex[0], hex[1], hex );
	}

	char m_Filename[MAX_OSPATH];
};


#endif // CUSTOMFILE_SHARED_H
