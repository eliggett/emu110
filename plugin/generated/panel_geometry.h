// GENERATED FILE -- do not edit.
// Produced by plugin/tools/panel_export.py from resources/graphics/overall_panel_inkscape.svg
//
// Every panel coordinate lives in the Inkscape artwork.  To move a control,
// move it in Inkscape and re-run the exporter; nothing here is authored by hand.

#pragma once

namespace voltaire {
namespace panel {

// Design-space units are the SVG user units (millimetres).  The UI scales
// the whole panel by one factor; no code should assume a pixel size.
inline constexpr float kDesignWidth  = 779.48499f;
inline constexpr float kDesignHeight = 676.16971f;

struct Rect { float x, y, w, h; };
struct Knob { float cx, cy, r; float zero_deg; };

enum ButtonId : int {
    BUT_PART_JUMP,
    BUT_LEFT,
    BUT_RIGHT,
    BUT_FILTER,
    BUT_EDIT_EXIT,
    BUT_DEC,
    BUT_INC_EXIT,
    BUT_RESET,
    BUT_PATCH_MENU,
    BUT_TONE,
    BUT_DIVE,
    BUT_CART,
    BUTTONID_COUNT
};

inline constexpr Rect kButton[BUTTONID_COUNT] = {
    {  480.1989f,   29.7837f,  59.4823f,  22.2454f },  // BUT_part_jump
    {  573.9124f,   30.0560f,  59.4823f,  22.2454f },  // BUT_left
    {  667.6259f,   30.0560f,  59.4823f,  22.2454f },  // BUT_right
    {   38.1489f,   69.1950f,  21.8850f,  38.2988f },  // BUT_filter
    {  481.7130f,   97.3993f,  59.4823f,  22.2454f },  // BUT_edit_exit
    {  573.9124f,   97.3993f,  59.4823f,  22.2454f },  // BUT_dec
    {  669.0034f,   97.3993f,  59.4823f,  22.2454f },  // BUT_inc_exit
    {  300.7933f,  131.3654f,  59.4823f,  11.6583f },  // BUT_reset
    {  189.6255f,  176.0089f,  59.4823f,  22.2454f },  // BUT_Patch_Menu
    {  281.8250f,  176.0089f,  59.4823f,  22.2454f },  // BUT_tone
    {  376.9160f,  176.0089f,  59.4823f,  22.2454f },  // BUT_dive
    {  472.0070f,  176.0089f, 256.5959f,  22.2454f },  // BUT_Cart
};

inline constexpr const char *kButtonName[BUTTONID_COUNT] = {
    "BUT_part_jump",
    "BUT_left",
    "BUT_right",
    "BUT_filter",
    "BUT_edit_exit",
    "BUT_dec",
    "BUT_inc_exit",
    "BUT_reset",
    "BUT_Patch_Menu",
    "BUT_tone",
    "BUT_dive",
    "BUT_Cart",
};

// SVG element ids, for looking shapes up in the parsed artwork.
inline constexpr const char *kButtonSvgId[BUTTONID_COUNT] = {
    "rect3",
    "rect4",
    "rect5",
    "rect10",
    "rect6",
    "rect7",
    "rect8",
    "rect36",
    "rect14",
    "rect15",
    "rect17",
    "rect38",
};

enum LedId : int {
    LED_PART_JUMP,
    LED_FILT,
    LED_CLIP,
    LED_EDIT_EXIT,
    LED_MIDI,
    LEDID_COUNT
};

inline constexpr Rect kLed[LEDID_COUNT] = {
    {  495.3332f,   29.7837f,  29.2137f,   6.1505f },  // LED_part_jump
    {   35.3672f,   57.1660f,  29.2137f,   6.1505f },  // LED_filt
    {  115.0075f,   57.1660f,  29.2137f,   6.1505f },  // LED_clip
    {  496.8473f,   97.3993f,  29.2137f,   6.1505f },  // LED_edit_exit
    {  185.8856f,  134.1193f,  29.2137f,   6.1505f },  // LED_midi
};

inline constexpr const char *kLedName[LEDID_COUNT] = {
    "LED_part_jump",
    "LED_filt",
    "LED_clip",
    "LED_edit_exit",
    "LED_midi",
};

// SVG element ids, for looking shapes up in the parsed artwork.
inline constexpr const char *kLedSvgId[LEDID_COUNT] = {
    "rect30",
    "rect34",
    "rect33",
    "rect32",
    "rect35",
};

enum MeterId : int {
    VU_HORIZ_STEREO,
    METERID_COUNT
};

inline constexpr Rect kMeter[METERID_COUNT] = {
    {   24.8509f,  176.0089f, 131.8599f,  22.2454f },  // VU_Horiz_Stereo
};

inline constexpr const char *kMeterName[METERID_COUNT] = {
    "VU_Horiz_Stereo",
};

// SVG element ids, for looking shapes up in the parsed artwork.
inline constexpr const char *kMeterSvgId[METERID_COUNT] = {
    "rect39",
};

inline constexpr Rect kLcdOuter = { 184.4372f, 57.0556f, 257.1494f, 62.5776f };
inline constexpr Rect kLcdInner = { 196.5165f, 62.7724f, 232.9907f, 51.1440f };

// The pointer shape "path39" is drawn by the SVG; rotate it about
// (cx, cy).  zero_deg is where the artwork already points, so a value of
// 0.0 needs no rotation at all.
inline constexpr Knob kVolumeKnob = { 129.6144f, 88.3444f, 18.2945f, -43.2132f };
inline constexpr const char *kVolumeKnobPointerId = "path39";

// ------------------------------------------------------------------ DIVE
//
// The drawer lives below the panel in the same artwork, so every coordinate
// here is in the same design space as the panel above it.  It has three
// heights: shut, open on a page with one tab row, and open on a page with
// two.  A page that shows no second row is drawn shifted up by exactly that
// row's height, and the geometry below already has the shift applied.

inline constexpr float kPanelShutHeight = 213.02323f;
inline constexpr float kDiveRow2Height  = 50.61816f;
inline constexpr float kDiveOpenHeight  = 676.16971f;
inline constexpr float kDiveOpenHeight1Row = 625.55155f;

// The drawer body and its content box are the two shapes whose size depends
// on which page is open, so the UI draws them itself and skips the artwork's
// copies.  Same pattern as the knob pointer.
inline constexpr const char *kDiveBodySvgId    = "rect389";
inline constexpr const char *kDiveContentSvgId = "rect11";
inline constexpr Rect kDiveBody    = { 2.0000f, 211.0232f, 775.4852f, 463.1465f };
inline constexpr Rect kDiveContent = { 2.0000f, 312.2595f, 775.4852f, 361.9102f };

enum DiveTabId : int {
    TAB_SET,
    TAB_COMMON,
    TAB_P1,
    TAB_P2,
    TAB_P3,
    TAB_P4,
    TAB_P5,
    TAB_P6,
    TAB_BASIC,
    TAB_LEVEL,
    TAB_PITCH,
    TAB_LFO,
    DIVETABID_COUNT
};

inline constexpr Rect kDiveTab[DIVETABID_COUNT] = {
    {    2.0000f,  211.0232f, 121.4311f,  50.6182f },  // BUT_set
    {  123.4311f,  211.0232f, 208.2385f,  50.6182f },  // BUT_common
    {  331.6696f,  211.0232f,  74.3026f,  50.6182f },  // BUT_P1
    {  405.9722f,  211.0232f,  74.3026f,  50.6182f },  // BUT_P2
    {  480.2748f,  211.0232f,  74.3026f,  50.6182f },  // BUT_P3
    {  554.5773f,  211.0232f,  74.3026f,  50.6182f },  // BUT_P4
    {  628.8799f,  211.0232f,  74.3026f,  50.6182f },  // BUT_P5
    {  703.1826f,  211.0232f,  74.3026f,  50.6182f },  // BUT_P6
    {  100.6682f,  261.6414f, 144.5373f,  50.6182f },  // BUT_basic
    {  245.2054f,  261.6414f, 144.5373f,  50.6182f },  // BUT_level
    {  389.7427f,  261.6414f, 144.5373f,  50.6182f },  // BUT_pitch
    {  534.2800f,  261.6414f, 144.5373f,  50.6182f },  // BUT_LFO
};

// 0 is the always-visible row; 1 only appears once a part tab is chosen.
inline constexpr int kDiveTabRow[DIVETABID_COUNT] = {
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    1,
    1,
    1,
    1,
};

inline constexpr const char *kDiveTabSvgId[DIVETABID_COUNT] = {
    "rect60",
    "rect59",
    "rect49",
    "rect50",
    "rect51",
    "rect52",
    "rect53",
    "rect54",
    "rect65",
    "rect62",
    "rect63",
    "rect64",
};

inline constexpr const char *kDiveTabTextSvgId[DIVETABID_COUNT] = {
    "text60",
    "text40",
    "text37",
    "text35",
    "text49",
    "text34",
    "text47",
    "text48",
    "text65",
    "text62",
    "text63",
    "text64",
};

inline constexpr const char *kDiveTabName[DIVETABID_COUNT] = {
    "set",
    "common",
    "P1",
    "P2",
    "P3",
    "P4",
    "P5",
    "P6",
    "basic",
    "level",
    "pitch",
    "LFO",
};

enum DivePage : int {
    DIVE_SET,
    DIVE_COMMON,
    DIVE_BASIC,
    DIVE_LEVEL,
    DIVE_PITCH,
    DIVE_LFO,
    DIVEPAGE_COUNT
};

inline constexpr const char *kDivePageName[DIVEPAGE_COUNT] = {
    "set",
    "common",
    "basic",
    "level",
    "pitch",
    "lfo",
};

// The pages with no per-part sub-tabs: their second row is hidden and their
// content is already shifted up by kDiveRow2Height.
inline constexpr bool kDivePageHidesRow2[DIVEPAGE_COUNT] = {
    true,
    true,
    false,
    false,
    false,
    false,
};

enum DiveKind : int { DK_BUTTON, DK_MENU, DK_SLIDER, DK_LCD };

// One row per control.  `travel` is where a slider tap's CENTRE may go --
// the artwork draws ten graticules and the tap stays between the first and
// the last -- and `tap` is the tap's own size, taken from where it was
// parked in Inkscape.  Both are zero for anything that is not a slider.
struct DiveControl {
    const char *label;
    const char *id;
    const char *tap_id;
    const char *text_id;
    int page;
    int kind;
    Rect box;
    Rect travel;
    Rect tap;
};

inline constexpr DiveControl kDiveControl[] = {
    { "SB_Master_Tune", "g63", "rect63", "text227-6", 0, DK_SLIDER,
      {   86.0240f,  311.0812f,  28.6869f, 206.7601f },
      {   86.0240f,  325.5870f,  28.6869f, 178.8750f },
      {   85.8675f,  412.1123f,  29.0000f,   5.7180f } },
    { "M_Control_Channel", "rect11-3", nullptr, "text30", 0, DK_MENU,
      {  647.3747f,  355.9293f,  57.4400f,  22.2454f },
      {    0.0000f,    0.0000f,   0.0000f,   0.0000f },
      {    0.0000f,    0.0000f,   0.0000f,   0.0000f } },
    { "BUT_Control_Change", "rect12", nullptr, "text31", 0, DK_BUTTON,
      {  647.3747f,  382.1746f,  57.4400f,  22.2454f },
      {    0.0000f,    0.0000f,   0.0000f,   0.0000f },
      {    0.0000f,    0.0000f,   0.0000f,   0.0000f } },
    { "BUT_Program_Change", "rect13", nullptr, "text33", 0, DK_BUTTON,
      {  647.3747f,  408.4201f,  57.4400f,  22.2454f },
      {    0.0000f,    0.0000f,   0.0000f,   0.0000f },
      {    0.0000f,    0.0000f,   0.0000f,   0.0000f } },
    { "BUT_Channel_Pressure", "rect18", nullptr, "text32", 0, DK_BUTTON,
      {  647.3749f,  434.6654f,  57.4400f,  22.2454f },
      {    0.0000f,    0.0000f,   0.0000f,   0.0000f },
      {    0.0000f,    0.0000f,   0.0000f,   0.0000f } },
    { "BUT_Polyphonic_Pressure", "rect45", nullptr, "text42", 0, DK_BUTTON,
      {  647.3749f,  460.9109f,  57.4400f,  22.2454f },
      {    0.0000f,    0.0000f,   0.0000f,   0.0000f },
      {    0.0000f,    0.0000f,   0.0000f,   0.0000f } },
    { "BUT_Pitch_Bender", "rect46", nullptr, "text44", 0, DK_BUTTON,
      {  647.3749f,  487.1563f,  57.4400f,  22.2454f },
      {    0.0000f,    0.0000f,   0.0000f,   0.0000f },
      {    0.0000f,    0.0000f,   0.0000f,   0.0000f } },
    { "M_Map_Edit", "rect49", nullptr, "text49", 0, DK_MENU,
      {  199.4624f,  494.5958f,  57.4400f,  22.2454f },
      {    0.0000f,    0.0000f,   0.0000f,   0.0000f },
      {    0.0000f,    0.0000f,   0.0000f,   0.0000f } },
    { "BUT_Exclusive", "rect47", nullptr, "text45", 0, DK_BUTTON,
      {  647.3749f,  513.4017f,  57.4400f,  22.2454f },
      {    0.0000f,    0.0000f,   0.0000f,   0.0000f },
      {    0.0000f,    0.0000f,   0.0000f,   0.0000f } },
    { "LCD_Patch_Name", "rect369", nullptr, "text369", 1, DK_LCD,
      {  123.9298f,  300.8000f, 160.2894f,  26.2142f },
      {    0.0000f,    0.0000f,   0.0000f,   0.0000f },
      {    0.0000f,    0.0000f,   0.0000f,   0.0000f } },
    { "BUT_write", "rect386", nullptr, "text386", 1, DK_BUTTON,
      {  526.8887f,  302.7827f, 134.8885f,  22.2454f },
      {    0.0000f,    0.0000f,   0.0000f,   0.0000f },
      {    0.0000f,    0.0000f,   0.0000f,   0.0000f } },
    { "SB_Chorus_Rate", "g19", "rect291", "text309", 1, DK_SLIDER,
      {  397.1855f,  339.9170f,  28.6869f, 206.7601f },
      {  397.1855f,  354.4228f,  28.6869f, 178.8750f },
      {  397.0290f,  351.5638f,  29.0000f,   5.7180f } },
    { "SB_Chorus_Depth", "g54", "rect55", "text321", 1, DK_SLIDER,
      {  497.3010f,  339.9170f,  28.6869f, 206.7601f },
      {  497.3010f,  354.4228f,  28.6869f, 178.8750f },
      {  497.1444f,  351.5638f,  29.0000f,   5.7180f } },
    { "SB_Tremolo_Rate", "g65", "rect65", "text345", 1, DK_SLIDER,
      {  587.0636f,  339.9170f,  28.6869f, 206.7601f },
      {  587.0636f,  354.4228f,  28.6869f, 178.8750f },
      {  586.9070f,  351.5638f,  29.0000f,   5.7180f } },
    { "SB_Tremolo_Depth", "g76", "rect76", "text357", 1, DK_SLIDER,
      {  671.2669f,  339.9170f,  28.6869f, 206.7601f },
      {  671.2669f,  354.4228f,  28.6869f, 178.8750f },
      {  671.1103f,  351.5638f,  29.0000f,   5.7180f } },
    { "M_output_mode", "rect370", nullptr, "text373", 1, DK_MENU,
      {  267.4636f,  366.0332f,  57.4400f,  26.2140f },
      {    0.0000f,    0.0000f,   0.0000f,   0.0000f },
      {    0.0000f,    0.0000f,   0.0000f,   0.0000f } },
    { "BUT_preset_left", "rect376", nullptr, "text376", 1, DK_BUTTON,
      {  221.0304f,  438.7782f,  59.4823f,  22.2454f },
      {    0.0000f,    0.0000f,   0.0000f,   0.0000f },
      {    0.0000f,    0.0000f,   0.0000f,   0.0000f } },
    { "BUT_preset_right", "rect379", nullptr, "text377", 1, DK_BUTTON,
      {  221.0304f,  472.7234f,  59.4823f,  22.2454f },
      {    0.0000f,    0.0000f,   0.0000f,   0.0000f },
      {    0.0000f,    0.0000f,   0.0000f,   0.0000f } },
    { "BUT_preset_LR_dry", "rect381", nullptr, "text378", 1, DK_BUTTON,
      {  221.0304f,  506.6686f,  59.4823f,  22.2454f },
      {    0.0000f,    0.0000f,   0.0000f,   0.0000f },
      {    0.0000f,    0.0000f,   0.0000f,   0.0000f } },
    { "BUT_preset_LR_efx", "rect382", nullptr, "text379", 1, DK_BUTTON,
      {  221.0304f,  540.6138f,  59.4823f,  22.2454f },
      {    0.0000f,    0.0000f,   0.0000f,   0.0000f },
      {    0.0000f,    0.0000f,   0.0000f,   0.0000f } },
    { "LCD_tone", "rect17-7", nullptr, "text293", 2, DK_LCD,
      {  561.2153f,  425.0448f, 156.3942f,  26.2142f },
      {    0.0000f,    0.0000f,   0.0000f,   0.0000f },
      {    0.0000f,    0.0000f,   0.0000f,   0.0000f } },
    { "M_output_assign", "rect295", nullptr, "text295", 2, DK_MENU,
      {  298.2194f,  429.0155f,  57.4400f,  22.2454f },
      {    0.0000f,    0.0000f,   0.0000f,   0.0000f },
      {    0.0000f,    0.0000f,   0.0000f,   0.0000f } },
    { "M_midi_rx_channel", "rect296", nullptr, "text296", 2, DK_MENU,
      {  298.2194f,  463.0656f,  57.4400f,  22.2454f },
      {    0.0000f,    0.0000f,   0.0000f,   0.0000f },
      {    0.0000f,    0.0000f,   0.0000f,   0.0000f } },
    { "M_key_low", "rect299", nullptr, "text302", 2, DK_MENU,
      {  561.2153f,  466.0571f,  95.0109f,  22.2454f },
      {    0.0000f,    0.0000f,   0.0000f,   0.0000f },
      {    0.0000f,    0.0000f,   0.0000f,   0.0000f } },
    { "M_map", "rect305", nullptr, "text301", 2, DK_MENU,
      {  298.2194f,  501.1656f,  57.4400f,  22.2454f },
      {    0.0000f,    0.0000f,   0.0000f,   0.0000f },
      {    0.0000f,    0.0000f,   0.0000f,   0.0000f } },
    { "M_key_high", "rect300", nullptr, "text303", 2, DK_MENU,
      {  561.2153f,  504.3405f,  95.0109f,  22.2454f },
      {    0.0000f,    0.0000f,   0.0000f,   0.0000f },
      {    0.0000f,    0.0000f,   0.0000f,   0.0000f } },
    { "BUT_program_change", "rect306", nullptr, "text297", 2, DK_BUTTON,
      {  298.2194f,  538.5236f,  57.4400f,  22.2454f },
      {    0.0000f,    0.0000f,   0.0000f,   0.0000f },
      {    0.0000f,    0.0000f,   0.0000f,   0.0000f } },
    { "BUT_defaults", "rect307", nullptr, "text307", 2, DK_BUTTON,
      {  562.1011f,  539.1391f,  59.4823f,  22.2454f },
      {    0.0000f,    0.0000f,   0.0000f,   0.0000f },
      {    0.0000f,    0.0000f,   0.0000f,   0.0000f } },
    { "SB_Part_Level", "g1", "rect53", "text43", 3, DK_SLIDER,
      {  204.2905f,  375.7930f,  28.6869f, 206.7601f },
      {  204.2905f,  390.2988f,  28.6869f, 178.8750f },
      {  204.1340f,  388.4293f,  29.0000f,   5.7180f } },
    { "SB_Velocity_Sensitivity", "g2", "rect65", "text55", 3, DK_SLIDER,
      {  287.6450f,  375.7930f,  28.6869f, 206.7601f },
      {  287.6450f,  390.2988f,  28.6869f, 178.8750f },
      {  287.4884f,  388.4293f,  29.0000f,   5.7180f } },
    { "SB_ENV_Attack_Rate", "g3", "rect78", "text68", 3, DK_SLIDER,
      {  370.9994f,  375.7930f,  28.6869f, 206.7601f },
      {  370.9994f,  390.2988f,  28.6869f, 178.8750f },
      {  370.8429f,  388.4293f,  29.0000f,   5.7180f } },
    { "SB_ENV_Release_Rate", "g4", "rect93", "text83", 3, DK_SLIDER,
      {  454.3538f,  375.7930f,  28.6869f, 206.7601f },
      {  454.3538f,  390.2988f,  28.6869f, 178.8750f },
      {  454.1973f,  388.4293f,  29.0000f,   5.7180f } },
    { "SB_Channel_Pressure_Sensitivity", "g5", "rect106", "text96", 3, DK_SLIDER,
      {  537.7083f,  375.7930f,  28.6869f, 206.7601f },
      {  537.7083f,  390.2988f,  28.6869f, 178.8750f },
      {  537.5518f,  388.4293f,  29.0000f,   5.7180f } },
    { "SB_Shift_Course", "g9", "rect119", "text109", 4, DK_SLIDER,
      {  215.6284f,  375.8798f,  28.6869f, 206.7601f },
      {  215.6284f,  390.3856f,  28.6869f, 178.8750f },
      {  215.4719f,  387.3689f,  29.0000f,   5.7180f } },
    { "SB_Shift_Fine", "g8", "rect130", "text120", 4, DK_SLIDER,
      {  291.3914f,  375.8798f,  28.6869f, 206.7601f },
      {  291.3914f,  390.3856f,  28.6869f, 178.8750f },
      {  291.2348f,  387.3689f,  29.0000f,   5.7180f } },
    { "SB_Bend_Range", "g7", "rect142", "text132", 4, DK_SLIDER,
      {  373.3244f,  375.8798f,  28.6869f, 206.7601f },
      {  373.3244f,  390.3856f,  28.6869f, 178.8750f },
      {  373.1679f,  387.3689f,  29.0000f,   5.7180f } },
    { "SB_Detune_Depth", "g6", "rect153", "text143", 4, DK_SLIDER,
      {  458.2617f,  375.8798f,  28.6869f, 206.7601f },
      {  458.2617f,  390.3856f,  28.6869f, 178.8750f },
      {  458.1052f,  387.3689f,  29.0000f,   5.7180f } },
    { "SB_Polyphonic_Pressure_Sensitivity", "g5", "rect164", "text154", 4, DK_SLIDER,
      {  542.9643f,  375.8798f,  28.6869f, 206.7601f },
      {  542.9643f,  390.3856f,  28.6869f, 178.8750f },
      {  542.8078f,  387.3689f,  29.0000f,   5.7180f } },
    { "SB_LFO_Rate", "g119", "rect119", "text183", 5, DK_SLIDER,
      {  102.0682f,  370.8544f,  28.6869f, 206.7601f },
      {  102.0682f,  385.3603f,  28.6869f, 178.8750f },
      {  101.9116f,  382.5013f,  29.0000f,   5.7180f } },
    { "SB_Auto_Depth", "g108", "rect109", "text194", 5, DK_SLIDER,
      {  179.8349f,  370.8544f,  28.6869f, 206.7601f },
      {  179.8349f,  385.3603f,  28.6869f, 178.8750f },
      {  179.6784f,  382.5013f,  29.0000f,   5.7180f } },
    { "SB_Auto_Delay_Time", "g97", "rect97", "text205", 5, DK_SLIDER,
      {  262.8416f,  370.8544f,  28.6869f, 206.7601f },
      {  262.8416f,  385.3603f,  28.6869f, 178.8750f },
      {  262.6851f,  382.5013f,  29.0000f,   5.7180f } },
    { "SB_Auto_Rise_Time", "g86", "rect86", "text216", 5, DK_SLIDER,
      {  340.6773f,  370.8544f,  28.6869f, 206.7601f },
      {  340.6773f,  385.3603f,  28.6869f, 178.8750f },
      {  340.5208f,  382.5013f,  29.0000f,   5.7180f } },
    { "SB_Manual_Depth", "g63", "rect63", "text227", 5, DK_SLIDER,
      {  415.7818f,  370.8544f,  28.6869f, 206.7601f },
      {  415.7818f,  385.3603f,  28.6869f, 178.8750f },
      {  415.6252f,  382.5013f,  29.0000f,   5.7180f } },
    { "SB_Manual_Rise_Time", "g52", "rect52", "text256", 5, DK_SLIDER,
      {  490.8641f,  370.8544f,  28.6869f, 206.7601f },
      {  490.8641f,  385.3603f,  28.6869f, 178.8750f },
      {  490.7076f,  382.5013f,  29.0000f,   5.7180f } },
    { "SB_Channel_Pressure_Sensitivity", "g41", "rect41", "text268", 5, DK_SLIDER,
      {  572.8725f,  370.8544f,  28.6869f, 206.7601f },
      {  572.8725f,  385.3603f,  28.6869f, 178.8750f },
      {  572.7160f,  382.5013f,  29.0000f,   5.7180f } },
    { "SB_Polyphonic_Pressure_Sensitivity", "g19", "rect291", "text281", 5, DK_SLIDER,
      {  651.3504f,  370.8544f,  28.6869f, 206.7601f },
      {  651.3504f,  385.3603f,  28.6869f, 178.8750f },
      {  651.1938f,  382.5013f,  29.0000f,   5.7180f } },
};
inline constexpr int kDiveControlCount = 46;

// Half-open range of kDiveControl belonging to each page.
inline constexpr int kDivePageFirst[DIVEPAGE_COUNT + 1] = {
    0,
    9,
    20,
    28,
    33,
    38,
    46,
};

} // namespace panel
} // namespace voltaire
