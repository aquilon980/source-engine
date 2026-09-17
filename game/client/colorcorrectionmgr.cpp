//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose : Singleton manager for color correction on the client
//
// $NoKeywords: $
//===========================================================================//

#include "cbase.h"
#include "tier0/vprof.h"
#include "colorcorrectionmgr.h"
#include "filesystem.h"


//------------------------------------------------------------------------------
// Custom filmic tonemap
//
// macOS can't compile a new post-process shader (no HLSL compiler here, and waf
// does not rebuild the fxctmp9 combo indices), but Source's colour-correction
// pass is a per-pixel transform whose input is a data file: a 3D lookup table.
// So the "tonemapper" is a 32x32x32 LUT, baked by scripts/bake-tonemap.py and
// shipped in the seb_defaults pack. See docs/tonemap.md.
//------------------------------------------------------------------------------
static ConVar cl_tonemap( "cl_tonemap", "1", FCVAR_ARCHIVE,
	"Custom filmic tonemap / colour grade (0 = off, stock colours)." );
static ConVar cl_tonemap_strength( "cl_tonemap_strength", "1.0", FCVAR_ARCHIVE,
	"Blend strength of the custom tonemap (0..1).", true, 0.0f, true, 1.0f );

#define SEB_TONEMAP_FILE "materials/correction/seb_tonemap.raw"

static ClientCCHandle_t s_TonemapHandle = INVALID_CLIENT_CCHANDLE;
static bool s_bTonemapMissingWarned = false;

// Creates the lookup on first use, then keeps its weight in sync with the cvars.
// Runs right after the per-frame weight reset, so a map's own colour-correction
// entities still blend on top of ours.
static void SebTonemap_Update()
{
	// Off: never create the lookup. Creating it while off would both cost the
	// pass and flash the grade for one frame, because SetLookupWeight() only
	// ever *raises* a weight -- it is the per-frame ResetLookupWeights() (this
	// lookup is resetable) that lets a weight drop again, and that lands on the
	// next frame.
	if ( !cl_tonemap.GetBool() )
	{
		return;
	}

	if ( s_TonemapHandle == INVALID_CLIENT_CCHANDLE )
	{
		if ( !g_pFullFileSystem->FileExists( SEB_TONEMAP_FILE, "GAME" ) )
		{
			if ( !s_bTonemapMissingWarned )
			{
				Warning( "cl_tonemap: lookup '%s' not found; tonemap disabled.\n", SEB_TONEMAP_FILE );
				s_bTonemapMissingWarned = true;
			}
			return;
		}

		s_TonemapHandle = g_pColorCorrectionMgr->AddColorCorrection( "seb_tonemap", SEB_TONEMAP_FILE );
		if ( s_TonemapHandle == INVALID_CLIENT_CCHANDLE )
		{
			Warning( "cl_tonemap: could not create colour-correction lookup.\n" );
			return;
		}
	}

	// Re-asserted every frame. The reset just zeroed this lookup (it is
	// resetable), so this is always a raise from 0 and always takes effect --
	// which is what makes cl_tonemap_strength go both up and down, and the
	// cvar go back to stock when set to 0.
	g_pColorCorrectionMgr->SetColorCorrectionWeight( s_TonemapHandle, cl_tonemap_strength.GetFloat() );
}


//------------------------------------------------------------------------------
// Singleton access
//------------------------------------------------------------------------------
static CColorCorrectionMgr s_ColorCorrectionMgr;
CColorCorrectionMgr *g_pColorCorrectionMgr = &s_ColorCorrectionMgr;


//------------------------------------------------------------------------------
// Constructor
//------------------------------------------------------------------------------
CColorCorrectionMgr::CColorCorrectionMgr()
{
	m_nActiveWeightCount = 0;
}


//------------------------------------------------------------------------------
// Creates, destroys color corrections
//------------------------------------------------------------------------------
ClientCCHandle_t CColorCorrectionMgr::AddColorCorrection( const char *pName, const char *pFileName )
{
	if ( !pFileName )
	{
		pFileName = pName;
	}

	CMatRenderContextPtr pRenderContext( g_pMaterialSystem );
	ColorCorrectionHandle_t ccHandle = pRenderContext->AddLookup( pName );
	if ( ccHandle )
	{
		pRenderContext->LockLookup( ccHandle );
		pRenderContext->LoadLookup( ccHandle, pFileName );
		pRenderContext->UnlockLookup( ccHandle );
	}
	else
	{
		Warning("Cannot find color correction lookup file: '%s'\n", pFileName );
	}

	return (ClientCCHandle_t)ccHandle;
}

void CColorCorrectionMgr::RemoveColorCorrection( ClientCCHandle_t h )
{
	if ( h != INVALID_CLIENT_CCHANDLE )
	{
		CMatRenderContextPtr pRenderContext( g_pMaterialSystem );
		ColorCorrectionHandle_t ccHandle = (ColorCorrectionHandle_t)h;
		pRenderContext->RemoveLookup( ccHandle );
	}
}


//------------------------------------------------------------------------------
// Modify color correction weights
//------------------------------------------------------------------------------
void CColorCorrectionMgr::SetColorCorrectionWeight( ClientCCHandle_t h, float flWeight )
{
	if ( h != INVALID_CLIENT_CCHANDLE )
	{
		CMatRenderContextPtr pRenderContext( g_pMaterialSystem );
		ColorCorrectionHandle_t ccHandle = (ColorCorrectionHandle_t)h;
		pRenderContext->SetLookupWeight( ccHandle, flWeight );

		// FIXME: NOTE! This doesn't work if the same handle has
		// its weight set twice with no intervening calls to ResetColorCorrectionWeights
		// which, at the moment, is true
		if ( flWeight != 0.0f )
		{
			++m_nActiveWeightCount;
		}
	}
}

void CColorCorrectionMgr::ResetColorCorrectionWeights()
{
	VPROF_("ResetColorCorrectionWeights", 2, VPROF_BUDGETGROUP_OTHER_UNACCOUNTED, false, 0);
	// FIXME: Where should I put this? It needs to happen prior to SimulateEntities()
	// which is where the client thinks for c_colorcorrection + c_colorcorrectionvolumes
	// update the color correction weights.
	CMatRenderContextPtr pRenderContext( g_pMaterialSystem );
	pRenderContext->ResetLookupWeights();
	m_nActiveWeightCount = 0;

	// Re-assert our tonemap after the reset, before the CC entities get to
	// blend theirs in during SimulateEntities.
	SebTonemap_Update();
}

void CColorCorrectionMgr::SetResetable( ClientCCHandle_t h, bool bResetable )
{
	// NOTE: Setting stuff to be not resettable doesn't work when in queued mode
	// because the logic that sets m_nActiveWeightCount to 0 in ResetColorCorrectionWeights
	// is no longer valid when stuff is not resettable.
	Assert( bResetable || !g_pMaterialSystem->GetThreadMode() == MATERIAL_SINGLE_THREADED );
	if ( h != INVALID_CLIENT_CCHANDLE )
	{
		CMatRenderContextPtr pRenderContext( g_pMaterialSystem );
		ColorCorrectionHandle_t ccHandle = (ColorCorrectionHandle_t)h;
		pRenderContext->SetResetable( ccHandle, bResetable );
	}
}


//------------------------------------------------------------------------------
// Is color correction active?
//------------------------------------------------------------------------------
bool CColorCorrectionMgr::HasNonZeroColorCorrectionWeights() const
{
	return ( m_nActiveWeightCount != 0 );
}
