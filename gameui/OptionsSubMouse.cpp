//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose: 
//
// $NoKeywords: $
//
//=============================================================================//
#include "OptionsSubMouse.h"
//#include "CommandCheckButton.h"
#include "KeyToggleCheckButton.h"
#include "CvarNegateCheckButton.h"
#include "CvarToggleCheckButton.h"
#include "cvarslider.h"

#include "EngineInterface.h"

#include <KeyValues.h>
#include <vgui/IScheme.h>
#include "tier1/convar.h"
#include <stdio.h>
#include <vgui_controls/TextEntry.h>
// memdbgon must be the last include file in a .cpp file!!!
#include <tier0/memdbgon.h>

using namespace vgui;

COptionsSubMouse::COptionsSubMouse(vgui::Panel *parent) : PropertyPage(parent, NULL)
{
	m_pReverseMouseCheckBox = new CCvarNegateCheckButton( 
		this, 
		"ReverseMouse", 
		"#GameUI_ReverseMouse", 
		"m_pitch" );
	
	m_pMouseFilterCheckBox = new CCvarToggleCheckButton( 
		this, 
		"MouseFilter", 
		"#GameUI_MouseFilter", 
		"m_filter" );

	m_pMouseRawCheckBox = new CCvarToggleCheckButton( 
		this, 
		"MouseRaw", 
		"#GameUI_MouseRaw", 
		"m_rawinput" );

	m_pQuickInfoCheckBox = new CCvarToggleCheckButton(
		this,
		"HudQuickInfo",
		"#GameUI_HudQuickInfo",
		"hud_quickinfo" );

	m_pMouseSensitivitySlider = new CCvarSlider( this, "Slider", "#GameUI_MouseSensitivity",
		0.1f, 6.0f, "sensitivity", true );

    m_pMouseSensitivityLabel = new TextEntry(this, "SensitivityLabel");
    m_pMouseSensitivityLabel->AddActionSignalTarget(this);

	LoadControlSettings("Resource\\OptionsSubMouse.res");

	UpdateSensitivityLabel();
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
COptionsSubMouse::~COptionsSubMouse()
{
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void COptionsSubMouse::OnResetData()
{
	m_pReverseMouseCheckBox->Reset();
	m_pMouseFilterCheckBox->Reset();
	m_pMouseRawCheckBox->Reset();
	m_pMouseSensitivitySlider->Reset();
	m_pQuickInfoCheckBox->Reset();
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void COptionsSubMouse::OnApplyChanges()
{
	m_pReverseMouseCheckBox->ApplyChanges();
	m_pMouseFilterCheckBox->ApplyChanges();
	m_pMouseRawCheckBox->ApplyChanges();
	m_pMouseSensitivitySlider->ApplyChanges();
	m_pQuickInfoCheckBox->ApplyChanges();
}

//-----------------------------------------------------------------------------
// Purpose: sets background color & border
//-----------------------------------------------------------------------------
void COptionsSubMouse::ApplySchemeSettings(IScheme *pScheme)
{
	BaseClass::ApplySchemeSettings(pScheme);
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void COptionsSubMouse::OnControlModified(Panel *panel)
{
	PostActionSignal(new KeyValues("ApplyButtonEnable"));

    // the HasBeenModified() check is so that if the value is outside of the range of the
    // slider, it won't use the slider to determine the display value but leave the
    // real value that we determined in the constructor
    if (panel == m_pMouseSensitivitySlider && m_pMouseSensitivitySlider->HasBeenModified())
    {
        UpdateSensitivityLabel();
    }
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void COptionsSubMouse::OnTextChanged(Panel *panel)
{
    if ( panel == m_pMouseSensitivityLabel )
    {
        char buf[64];
        m_pMouseSensitivityLabel->GetText(buf, 64);

        float fValue;
		int numParsed = sscanf(buf, "%f", &fValue);
		if ( ( numParsed == 1 ) && ( fValue >= 0.0f ) )
        {
            m_pMouseSensitivitySlider->SetSliderValue(fValue);
            PostActionSignal(new KeyValues("ApplyButtonEnable"));
        }
		return;
    }
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void COptionsSubMouse::UpdateSensitivityLabel()
{
    char buf[64];
    Q_snprintf(buf, sizeof( buf ), " %.2f", m_pMouseSensitivitySlider->GetSliderValue());
    m_pMouseSensitivityLabel->SetText(buf);
}