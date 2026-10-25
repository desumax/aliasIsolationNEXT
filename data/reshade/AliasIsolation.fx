// Alias Isolation NEXT: puts the mod's switch and settings in ReShade's Home tab.
//
// This effect draws nothing. The mod (aliasIsolation.asi) reads the technique's checkbox and the values below every
// frame, and turns its anti-aliasing on or off to match. Without the mod loaded, the technique does nothing at all.

uniform bool SharpeningEnabled <
    ui_label = "Sharpening";
> = true;

uniform float Sharpening <
    ui_type = "slider";
    ui_label = "Sharpening amount";
    ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
> = 0.3;

uniform bool ChromaticAberrationEnabled <
    ui_label = "Chromatic aberration";
    ui_tooltip = "The mod's own chromatic aberration. Keep the game's turned off in its video settings.";
> = true;

uniform float ChromaticAberration <
    ui_type = "slider";
    ui_label = "Chromatic aberration amount";
    ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
> = 0.5;

uniform bool ShowLogo <
    ui_label = "Show logo when a level loads";
> = true;

uniform bool StartDlss5AtLoading <
    ui_label = "Start DLSS 5 at the first loading screen";
    ui_tooltip = "Keeps DLSS 5 Feed off at launch and switches it on when the first loading screen appears, so its\nstart-up stall is hidden there. It starts anyway after 30 seconds in 3D. Needs DLSS5-Feeder.";
> = true;

// All three vertices land on the same point, so the triangle covers no pixels and the pixel shader never runs.
void AliasIsolation_VS(in uint id : SV_VertexID, out float4 position : SV_Position)
{
    position = float4(0.0, 0.0, 0.0, 1.0);
}

float4 AliasIsolation_PS(float4 position : SV_Position) : SV_Target
{
    return 0.0;
}

technique AliasIsolation <
    ui_label = "Alias Isolation";
    ui_tooltip = "Temporal anti-aliasing for Alien: Isolation.\nNeeds Anti-Aliasing SMAA T1x, Motion Blur on and the game's Chromatic Aberration off.";
>
{
    pass
    {
        VertexShader = AliasIsolation_VS;
        PixelShader = AliasIsolation_PS;
    }
}

// Its own entry in the Home tab: the mod moves the intro videos when it is switched, so it takes effect from the next
// launch.
technique AliasIsolation_SkipIntroVideos <
    ui_label = "Skip intro videos (from the next launch)";
    ui_tooltip = "Moves the AMD, Creative Assembly and Fox logo videos to _intro-videos-backup next to AI.exe.\nNeeds Alias Isolation installed; it does nothing else.";
>
{
    pass
    {
        VertexShader = AliasIsolation_VS;
        PixelShader = AliasIsolation_PS;
    }
}
