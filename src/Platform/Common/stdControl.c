#include "Platform/stdControl.h"

#include "Devices/sithControl.h"
#include "Win95/Window.h"
#include "stdPlatform.h"
#include "General/stdMath.h"
#include "Main/sithCvar.h"

#include <math.h>

#include "jk.h"

const char *stdControl_aAxisNames[JK_NUM_AXES+1] =
{
    "AXIS_JOY1_X",
    "AXIS_JOY1_Y",
    "AXIS_JOY1_Z",
    "AXIS_JOY1_R",
    "AXIS_JOY1_U",
    "AXIS_JOY1_V",
    "AXIS_JOY2_X",
    "AXIS_JOY2_Y",
    "AXIS_JOY2_Z",
    "AXIS_JOY2_R",
    "AXIS_JOY2_U",
    "AXIS_JOY2_V",
    "AXIS_MOUSE_X",
    "AXIS_MOUSE_Y",
    "AXIS_MOUSE_Z",
    " "
};

const stdControlDikStrToNum stdControl_aDikNumToStr[JK_TOTAL_NUM_KEYS] =
{
  { DIK_ESCAPE,         "DIK_ESCAPE" },
  { DIK_1,              "DIK_1" },
  { DIK_2,              "DIK_2" },
  { DIK_3,              "DIK_3" },
  { DIK_4,              "DIK_4" },
  { DIK_5,              "DIK_5" },
  { DIK_6,              "DIK_6" },
  { DIK_7,              "DIK_7" },
  { DIK_8,              "DIK_8" },
  { DIK_9,              "DIK_9" },
  { DIK_0,              "DIK_0" },
  { DIK_MINUS,          "DIK_MINUS" },
  { DIK_EQUALS,         "DIK_EQUALS" },
  { DIK_BACK,           "DIK_BACK" },
  { DIK_TAB,            "DIK_TAB" },
  { DIK_Q,              "DIK_Q" },
  { DIK_W,              "DIK_W" },
  { DIK_E,              "DIK_E" },
  { DIK_R,              "DIK_R" },
  { DIK_T,              "DIK_T" },
  { DIK_Y,              "DIK_Y" },
  { DIK_U,              "DIK_U" },
  { DIK_I,              "DIK_I" },
  { DIK_O,              "DIK_O" },
  { DIK_P,              "DIK_P" },
  { DIK_LBRACKET,       "DIK_LBRACKET" },
  { DIK_RBRACKET,       "DIK_RBRACKET" },
  { DIK_RETURN,         "DIK_RETURN" },
  { DIK_LCONTROL,       "DIK_LCONTROL" },
  { DIK_A,              "DIK_A" },
  { DIK_S,              "DIK_S" },
  { DIK_D,              "DIK_D" },
  { DIK_F,              "DIK_F" },
  { DIK_G,              "DIK_G" },
  { DIK_H,              "DIK_H" },
  { DIK_J,              "DIK_J" },
  { DIK_K,              "DIK_K" },
  { DIK_L,              "DIK_L" },
  { DIK_SEMICOLON,      "DIK_SEMICOLON" },
  { DIK_APOSTROPHE,     "DIK_APOSTROPHE" },
  { DIK_GRAVE,          "DIK_GRAVE" },
  { DIK_LSHIFT,         "DIK_LSHIFT" },
  { DIK_BACKSLASH,      "DIK_BACKSLASH" },
  { DIK_Z,              "DIK_Z" },
  { DIK_X,              "DIK_X" },
  { DIK_C,              "DIK_C" },
  { DIK_V,              "DIK_V" },
  { DIK_B,              "DIK_B" },
  { DIK_N,              "DIK_N" },
  { DIK_M,              "DIK_M" },
  { DIK_COMMA,          "DIK_COMMA" },
  { DIK_PERIOD,         "DIK_PERIOD" },
  { DIK_SLASH,          "DIK_SLASH" },
  { DIK_RSHIFT,         "DIK_RSHIFT" },
  { DIK_MULTIPLY,       "DIK_MULTIPLY" },
  { DIK_LMENU,          "DIK_LMENU" },
  { DIK_SPACE,          "DIK_SPACE" },
  { DIK_CAPITAL,        "DIK_CAPITAL" },
  { DIK_F1,             "DIK_F1" },
  { DIK_F2,             "DIK_F2" },
  { DIK_F3,             "DIK_F3" },
  { DIK_F4,             "DIK_F4" },
  { DIK_F5,             "DIK_F5" },
  { DIK_F6,             "DIK_F6" },
  { DIK_F7,             "DIK_F7" },
  { DIK_F8,             "DIK_F8" },
  { DIK_F9,             "DIK_F9" },
  { DIK_F10,            "DIK_F10" },
  { DIK_NUMLOCK,        "DIK_NUMLOCK" },
  { DIK_SCROLL,         "DIK_SCROLL" },
  { DIK_NUMPAD7,        "DIK_NUMPAD7" },
  { DIK_NUMPAD8,        "DIK_NUMPAD8" },
  { DIK_NUMPAD9,        "DIK_NUMPAD9" },
  { DIK_SUBTRACT,       "DIK_SUBTRACT" },
  { DIK_NUMPAD4,        "DIK_NUMPAD4" },
  { DIK_NUMPAD5,        "DIK_NUMPAD5" },
  { DIK_NUMPAD6,        "DIK_NUMPAD6" },
  { DIK_ADD,            "DIK_ADD" },
  { DIK_NUMPAD1,        "DIK_NUMPAD1" },
  { DIK_NUMPAD2,        "DIK_NUMPAD2" },
  { DIK_NUMPAD3,        "DIK_NUMPAD3" },
  { DIK_NUMPAD0,        "DIK_NUMPAD0" },
  { DIK_DECIMAL,        "DIK_DECIMAL" },
  { DIK_F11,            "DIK_F11" },
  { DIK_F12,            "DIK_F12" },
  { DIK_F13,            "DIK_F13" },
  { DIK_F14,            "DIK_F14" },
  { DIK_F15,            "DIK_F15" },
  { DIK_KANA,           "DIK_KANA" },
  { DIK_CONVERT,        "DIK_CONVERT" },
  { DIK_NOCONVERT,      "DIK_NOCONVERT" },
  { DIK_YEN,            "DIK_YEN" },
  { DIK_NUMPADEQUALS,   "DIK_NUMPADEQUALS" },
  { DIK_CIRCUMFLEX,     "DIK_CIRCUMFLEX" },
  { DIK_AT,             "DIK_AT" },
  { DIK_COLON,          "DIK_COLON" },
  { DIK_UNDERLINE,      "DIK_UNDERLINE" },
  { DIK_KANJI,          "DIK_KANJI" },
  { DIK_STOP,           "DIK_STOP" },
  { DIK_AX,             "DIK_AX" },
  { DIK_UNLABELED,      "DIK_UNLABELED" },
  { DIK_NUMPADENTER,    "DIK_NUMPADENTER" },
  { DIK_RCONTROL,       "DIK_RCONTROL" },
  { DIK_NUMPADCOMMA,    "DIK_NUMPADCOMMA" },
  { DIK_DIVIDE,         "DIK_DIVIDE" },
  { DIK_SYSRQ,          "DIK_SYSRQ" },
  { DIK_RMENU,          "DIK_RMENU" },
  { DIK_HOME,           "DIK_HOME" },
  { DIK_UP,             "DIK_UP" },
  { DIK_PRIOR,          "DIK_PRIOR" },
  { DIK_LEFT,           "DIK_LEFT" },
  { DIK_RIGHT,          "DIK_RIGHT" },
  { DIK_END,            "DIK_END" },
  { DIK_DOWN,           "DIK_DOWN" },
  { DIK_NEXT,           "DIK_NEXT" },
  { DIK_INSERT,         "DIK_INSERT" },
  { DIK_DELETE,         "DIK_DELETE" },
  { DIK_LWIN,           "DIK_LWIN" },
  { DIK_RWIN,           "DIK_RWIN" },
  { DIK_APPS,           "DIK_APPS" },


  { KEY_JOY1_B1,        "KEY_JOY1_B1" },
  { KEY_JOY1_B2,        "KEY_JOY1_B2" },
  { KEY_JOY1_B3,        "KEY_JOY1_B3" },
  { KEY_JOY1_B4,        "KEY_JOY1_B4" },
  { KEY_JOY1_B5,        "KEY_JOY1_B5" },
  { KEY_JOY1_B6,        "KEY_JOY1_B6" },
  { KEY_JOY1_B7,        "KEY_JOY1_B7" },
  { KEY_JOY1_B8,        "KEY_JOY1_B8" },
#ifdef SDL2_RENDER
  { KEY_JOY1_B9,        "KEY_JOY1_B9" },

  { KEY_JOY1_B10,       "KEY_JOY1_B10" },
  { KEY_JOY1_B11,        "KEY_JOY1_B11" },
  { KEY_JOY1_B12,        "KEY_JOY1_B12" },
  { KEY_JOY1_B13,        "KEY_JOY1_B13" },
  { KEY_JOY1_B14,        "KEY_JOY1_B14" },
  { KEY_JOY1_B15,        "KEY_JOY1_B15" },
  { KEY_JOY1_B16,        "KEY_JOY1_B16" },
  { KEY_JOY1_B17,        "KEY_JOY1_B17" },
  { KEY_JOY1_B18,        "KEY_JOY1_B18" },
  { KEY_JOY1_B19,        "KEY_JOY1_B19" },
  { KEY_JOY1_B20,        "KEY_JOY1_B20" },
  { KEY_JOY1_B21,        "KEY_JOY1_B21" },
  { KEY_JOY1_B22,        "KEY_JOY1_B22" },
  { KEY_JOY1_B23,        "KEY_JOY1_B23" },
  { KEY_JOY1_B24,        "KEY_JOY1_B24" },
  { KEY_JOY1_B25,        "KEY_JOY1_B25" },
  { KEY_JOY1_B26,        "KEY_JOY1_B26" },
  { KEY_JOY1_B27,        "KEY_JOY1_B27" },
  { KEY_JOY1_B28,        "KEY_JOY1_B28" },
  { KEY_JOY1_B29,        "KEY_JOY1_B29" },
  { KEY_JOY1_B30,        "KEY_JOY1_B30" },
  { KEY_JOY1_B31,        "KEY_JOY1_B31" },
  { KEY_JOY1_B32,        "KEY_JOY1_B32" },
#endif
  { KEY_JOY1_HLEFT,     "KEY_JOY1_HLEFT" },
  { KEY_JOY1_HUP,       "KEY_JOY1_HUP" },
  { KEY_JOY1_HRIGHT,    "KEY_JOY1_HRIGHT" },
  { KEY_JOY1_HDOWN,     "KEY_JOY1_HDOWN" },
  { KEY_JOY2_B1,        "KEY_JOY2_B1" },
  { KEY_JOY2_B2,        "KEY_JOY2_B2" },
  { KEY_JOY2_B3,        "KEY_JOY2_B3" },
  { KEY_JOY2_B4,        "KEY_JOY2_B4" },
  { KEY_JOY2_B5,        "KEY_JOY2_B5" },
  { KEY_JOY2_B6,        "KEY_JOY2_B6" },
  { KEY_JOY2_B7,        "KEY_JOY2_B7" },
  { KEY_JOY2_B8,        "KEY_JOY2_B8" },
#ifdef SDL2_RENDER
  { KEY_JOY2_B9,        "KEY_JOY2_B9" },

  { KEY_JOY2_B10,       "KEY_JOY2_B10" },
  { KEY_JOY2_B11,        "KEY_JOY2_B11" },
  { KEY_JOY2_B12,        "KEY_JOY2_B12" },
  { KEY_JOY2_B13,        "KEY_JOY2_B13" },
  { KEY_JOY2_B14,        "KEY_JOY2_B14" },
  { KEY_JOY2_B15,        "KEY_JOY2_B15" },
  { KEY_JOY2_B16,        "KEY_JOY2_B16" },
  { KEY_JOY2_B17,        "KEY_JOY2_B17" },
  { KEY_JOY2_B18,        "KEY_JOY2_B18" },
  { KEY_JOY2_B19,        "KEY_JOY2_B19" },
  { KEY_JOY2_B20,        "KEY_JOY2_B20" },
  { KEY_JOY2_B21,        "KEY_JOY2_B21" },
  { KEY_JOY2_B22,        "KEY_JOY2_B22" },
  { KEY_JOY2_B23,        "KEY_JOY2_B23" },
  { KEY_JOY2_B24,        "KEY_JOY2_B24" },
  { KEY_JOY2_B25,        "KEY_JOY2_B25" },
  { KEY_JOY2_B26,        "KEY_JOY2_B26" },
  { KEY_JOY2_B27,        "KEY_JOY2_B27" },
  { KEY_JOY2_B28,        "KEY_JOY2_B28" },
  { KEY_JOY2_B29,        "KEY_JOY2_B29" },
  { KEY_JOY2_B30,        "KEY_JOY2_B30" },
  { KEY_JOY2_B31,        "KEY_JOY2_B31" },
  { KEY_JOY2_B32,        "KEY_JOY2_B32" },
#endif
  { KEY_JOY2_HLEFT,     "KEY_JOY2_HLEFT" },
  { KEY_JOY2_HUP,       "KEY_JOY2_HUP" },
  { KEY_JOY2_HRIGHT,    "KEY_JOY2_HRIGHT" },
  { KEY_JOY2_HDOWN,     "KEY_JOY2_HDOWN" },
  { KEY_MOUSE_B1,       "KEY_MOUSE_B1" },
  { KEY_MOUSE_B2,       "KEY_MOUSE_B2" },
  { KEY_MOUSE_B3,       "KEY_MOUSE_B3" },
  { KEY_MOUSE_B4,       "KEY_MOUSE_B4" },
#ifdef SDL2_RENDER
  { KEY_MOUSE_B5,       "KEY_MOUSE_B5" }
#endif
};

// Added: mouse tuning, the m_* cvars (game folder's openjkdf2_cvars.json, so per machine,
// on top of each profile's per-axis sensitivity slider). Defaults leave the mouse as it was.
int stdControl_mouseSmooth = 0;              // 0 off, 1 JK.EXE's, 2 averaged over m_smoothMs
int stdControl_mouseSmoothMs = 20;
flex_t stdControl_mouseSensitivity = 1.0;
flex_t stdControl_mouseYawScale = 1.0;
flex_t stdControl_mousePitchScale = 1.0;
flex_t stdControl_mouseAccel = 0.0;          // extra gain per count/ms of mouse speed
flex_t stdControl_mouseAccelOffset = 0.0;    // speed (counts/ms) where acceleration starts
flex_t stdControl_mouseAccelMax = 0.0;       // largest gain, 0 = no limit
int stdControl_bMouseSystemAccel = 0;        // let the OS apply its pointer speed/acceleration
int stdControl_bMouseDebug = 0;

// Added: this read's processed mouse movement, in counts, fractions kept
static flex_d_t stdControl_aMouseAxisF[2];
static int stdControl_bMouseAxisF = 0;

void stdControl_StartupCvars()
{
    sithCvar_RegisterInt("m_smooth",            0,   &stdControl_mouseSmooth,        CVARFLAG_GLOBAL);
    sithCvar_RegisterInt("m_smoothMs",          20,  &stdControl_mouseSmoothMs,      CVARFLAG_GLOBAL);
    sithCvar_RegisterFlex("m_sensitivity",      1.0, &stdControl_mouseSensitivity,   CVARFLAG_GLOBAL);
    sithCvar_RegisterFlex("m_yawScale",         1.0, &stdControl_mouseYawScale,      CVARFLAG_GLOBAL);
    sithCvar_RegisterFlex("m_pitchScale",       1.0, &stdControl_mousePitchScale,    CVARFLAG_GLOBAL);
    sithCvar_RegisterFlex("m_accel",            0.0, &stdControl_mouseAccel,         CVARFLAG_GLOBAL);
    sithCvar_RegisterFlex("m_accelOffset",      0.0, &stdControl_mouseAccelOffset,   CVARFLAG_GLOBAL);
    sithCvar_RegisterFlex("m_accelMax",         0.0, &stdControl_mouseAccelMax,      CVARFLAG_GLOBAL);
    sithCvar_RegisterBool("m_systemAccel",      0,   &stdControl_bMouseSystemAccel,  CVARFLAG_GLOBAL);
    sithCvar_RegisterBool("m_debug",            0,   &stdControl_bMouseDebug,        CVARFLAG_GLOBAL);
}

// Added: turns one read's mouse movement (counts, may have fractions) into the mouse axes:
// acceleration, smoothing, then m_sensitivity and the yaw/pitch scales.
void stdControl_ProcessMouse(flex_d_t dx, flex_d_t dy, uint32_t dtMs)
{
    static flex_d_t remX = 0.0, remY = 0.0;
    static flex_d_t jkRemX = 0.0, jkRemY = 0.0;
    static int jkLastX = 0, jkLastY = 0;
    static flex_d_t histX[64], histY[64];
    static uint32_t histMs[64];
    static int histNext = 0, histCount = 0;

    flex_d_t rawX = dx, rawY = dy;
    flex_d_t dt = dtMs ? (flex_d_t)dtMs : 1.0;
    flex_d_t gain = 1.0;

    if (stdControl_mouseAccel > 0.0 && (dx != 0.0 || dy != 0.0))
    {
        flex_d_t speed = sqrt(dx*dx + dy*dy) / dt - stdControl_mouseAccelOffset;
        if (speed > 0.0)
        {
            gain = 1.0 + stdControl_mouseAccel * speed;
            if (stdControl_mouseAccelMax > 0.0 && gain > stdControl_mouseAccelMax)
                gain = stdControl_mouseAccelMax;
            dx *= gain;
            dy *= gain;
        }
    }

    if (dtMs > 250)
    {
        // a menu or a hitch, don't drag old movement along
        histCount = 0;
        jkLastX = jkLastY = 0;
    }

    if (stdControl_mouseSmooth == 1)
    {
        // JK.EXE (1.01 0x42f1d8): DirectInput's whole counts; under 25 ms between reads this read
        // and the last are averaged with an arithmetic shift, so a lone +1 count is lost and a
        // lone -1 counts twice. Kept as is.
        int ix, iy;
        jkRemX += dx;
        jkRemY += dy;
        ix = (int)jkRemX;
        iy = (int)jkRemY;
        jkRemX -= ix;
        jkRemY -= iy;
        int outX = ix, outY = iy;
        if (dtMs < 25)
        {
            outX = (int)floor((ix + jkLastX) / 2.0);
            outY = (int)floor((iy + jkLastY) / 2.0);
        }
        jkLastX = ix;
        jkLastY = iy;
        dx = outX;
        dy = outY;
    }
    else if (stdControl_mouseSmooth == 2 && stdControl_mouseSmoothMs > 0)
    {
        // average speed over the last m_smoothMs, this read included
        histX[histNext] = dx;
        histY[histNext] = dy;
        histMs[histNext] = dtMs;
        histNext = (histNext + 1) % 64;
        if (histCount < 64)
            histCount++;

        flex_d_t sumX = 0.0, sumY = 0.0;
        uint32_t sumMs = 0;
        for (int i = 0; i < histCount && sumMs < (uint32_t)stdControl_mouseSmoothMs; i++)
        {
            int idx = (histNext - 1 - i + 64) % 64;
            sumX += histX[idx];
            sumY += histY[idx];
            sumMs += histMs[idx];
        }
        if (sumMs)
        {
            dx = sumX * (flex_d_t)dtMs / (flex_d_t)sumMs;
            dy = sumY * (flex_d_t)dtMs / (flex_d_t)sumMs;
        }
    }
    else
    {
        histCount = 0;
    }

    dx *= stdControl_mouseSensitivity * stdControl_mouseYawScale;
    dy *= stdControl_mouseSensitivity * stdControl_mousePitchScale;

    stdControl_aMouseAxisF[0] = dx;
    stdControl_aMouseAxisF[1] = dy;
    stdControl_bMouseAxisF = 1;

    // whole counts for the non-raw axis reads, the remainder carried over
    remX += dx;
    remY += dy;
    stdControl_aAxisStates[AXIS_MOUSE_X] = (int)remX;
    stdControl_aAxisStates[AXIS_MOUSE_Y] = (int)remY;
    remX -= (int)remX;
    remY -= (int)remY;

    if (stdControl_bMouseDebug && (rawX != 0.0 || rawY != 0.0))
        stdPlatform_Printf("mouse: %3u ms  raw %7.2f %7.2f  gain %5.2f  out %7.2f %7.2f\n", dtMs, rawX, rawY, gain, dx, dy);
}

// Added: ReadAxisRaw for the mouse with the fractions ProcessMouse keeps
flex_t stdControl_ReadAxisRawF(int axisNum)
{
    if ((axisNum != AXIS_MOUSE_X && axisNum != AXIS_MOUSE_Y) || !stdControl_bMouseAxisF)
        return (flex_t)stdControl_ReadAxisRaw(axisNum);

    if ( !stdControl_bControlsActive )
        return 0.0;
    if ( (stdControl_aAxes[axisNum].flags & 2) == 0 )
        return 0.0;
    flex_d_t result = stdControl_aMouseAxisF[axisNum - AXIS_MOUSE_X];
    if ( result == 0.0 )
        return 0.0;
#ifdef QOL_IMPROVEMENTS
    sithControl_msIdle = 0;
#endif
    stdControl_bControlsIdle = 0;
    return (flex_t)result;
}

void stdControl_Reset()
{
    stdControlJoystickEntry *v0; // eax

    stdControl_bReadMouse = 0;
#ifndef SDL2_RENDER
    stdControl_bReadJoysticks = 0;
#endif

    for (int i = 0; i < JK_NUM_JOYSTICKS; i++) {
        stdControl_aAxisEnabled[i] = 0;
        stdControl_aAxisConnected[i] = 0x680;
    }

    v0 = stdControl_aAxes;
    for (int i = 0; i < JK_NUM_AXES; i++)
    {
        v0->flags &= ~2;
        ++v0;
    }
}

int stdControl_EnableAxis(unsigned int idx)
{
    if ( idx >= JK_NUM_AXES )
        return 0;

    if ( (stdControl_aAxes[idx].flags & 1) == 0 )
        return 0;
    stdControl_aAxes[idx].flags |= 2;
    if ( idx < AXIS_MOUSE_X )
    {
        int controller_idx = idx / JK_JOYSTICK_AXIS_STRIDE;
        stdControl_bReadJoysticks = 1;
        stdControl_aAxisEnabled[controller_idx] = 1;
        switch (idx % JK_JOYSTICK_AXIS_STRIDE)
        {
            case 0u:
                stdControl_aAxisConnected[controller_idx] |= 1u;
                break;
            case 1u:
                stdControl_aAxisConnected[controller_idx] |= 2u;
                break;
            case 2u:
                stdControl_aAxisConnected[controller_idx] |= 4u;
                break;
            case 3u:
                stdControl_aAxisConnected[controller_idx] |= 8u;
                break;
            case 4u:
                stdControl_aAxisConnected[controller_idx] |= 0x10u;
                break;
            case 5u:
                stdControl_aAxisConnected[controller_idx] |= 0x20u;
                break;
            default:
                return 1;
        }
    }
    else
    {
        stdControl_bReadMouse = 1;
    }
    return 1;
}

// readcontrols

flex_t stdControl_ReadAxis(int axisNum)
{
    flex_t result; // st7
    int v2; // ecx
    int v3; // edx
    int v4; // eax
    int v5; // edx
    int v9; // [esp+8h] [ebp+4h]

    if ( !stdControl_bControlsActive )
        return 0.0;

    // Added: OOB
    if (axisNum >= JK_NUM_AXES) {
        return 0.0;
    }

    v2 = axisNum;
    v3 = stdControl_aAxes[axisNum].flags;
    if ( (v3 & 2) == 0 )
        return 0.0;
    v4 = stdControl_aAxisStates[axisNum] - stdControl_aAxes[axisNum].dwXoffs;
    v9 = v4;
    if ( !v4 )
        return 0.0;
    if ( (v3 & 8) == 0 )
    {
        v5 = stdControl_aAxes[v2].dwYoffs;
        if ( v5 )
        {
            if ( v4 < 0 )
                v4 = -v4;
            if ( v4 < v5 )
                return 0.0;
        }
    }
    result = stdMath_ClipNearZero(stdControl_aAxes[v2].fRangeConversion * (flex_d_t)v9);
#ifdef QOL_IMPROVEMENTS
    if ( result != 0.0 ) {
        sithControl_msIdle = 0;
        stdControl_bControlsIdle = 0;
    }
#else
    if ( stdControl_bControlsIdle )
    {
        if ( result != 0.0 )
            stdControl_bControlsIdle = 0;
    }
#endif

    // Added: Scale to FPS
    //result = (result * (sithTime_g_fps / 50.0));
    return result;
}

int stdControl_ReadAxisRaw(int axisNum)
{
    int result; // eax

    if ( !stdControl_bControlsActive )
        return 0;
    // Added: OOB
    if (axisNum >= JK_NUM_AXES) {
        return 0;
    }
    if ( (stdControl_aAxes[axisNum].flags & 2) == 0 )
        return 0;
    result = stdControl_aAxisStates[axisNum] - stdControl_aAxes[axisNum].dwXoffs;
    if ( !result )
        return 0;
#ifdef QOL_IMPROVEMENTS
    sithControl_msIdle = 0;
    stdControl_bControlsIdle = 0;

#else
    if ( stdControl_bControlsIdle )
        stdControl_bControlsIdle = 0;
#endif

    return result;
}

flex_t stdControl_ReadKeyAsAxis(int keyNum)
{
    uint32_t v1; // eax
    flex_t result; // st7

    if ( !stdControl_bControlsActive || stdControl_bDisableKeyboard )
        return 0.0;
    v1 = stdControl_aKeyIdleTimes[keyNum];
    if ( !v1 )
    {
        if ( stdControl_aKeyInfo[keyNum] )
        {
            v1 = stdControl_readDeltaTime;
        }
        else {
            return 0.0;
        }
    }

    if ( v1 >= stdControl_readDeltaTime )
        v1 = stdControl_readDeltaTime;
#ifdef QOL_IMPROVEMENTS
    result = 1.5;
#else
    result = (flex_d_t)v1 * stdControl_updateKHz;
#endif
#ifdef QOL_IMPROVEMENTS
    if ( result != 0.0 ) {
        sithControl_msIdle = 0;
        stdControl_bControlsIdle = 0;
    }
#else
    if ( stdControl_bControlsIdle )
    {
        if ( result != 0.0 )
            stdControl_bControlsIdle = 0;
    }
#endif
    return result;
}

int stdControl_ReadAxisAsKey(int axisNum)
{
    flex_d_t v1; // st7

    v1 = stdControl_ReadAxis(axisNum);
    if ( v1 < 0.0 )
        v1 = -v1;
    return v1 > 0.5;
}

int stdControl_ReadKey(int keyNum, int *pOut)
{
    int result; // eax

    if ( !stdControl_bControlsActive || stdControl_bDisableKeyboard )
    {
        if ( pOut )
            *pOut = 0;
        result = 0;
    }
    else
    {
        if ( pOut )
            *pOut += stdControl_aKeyPressed[keyNum];
        if ( stdControl_bControlsIdle )
        {
            if ( stdControl_aKeyInfo[keyNum] )
                stdControl_bControlsIdle = 0;
        }
        result = stdControl_aKeyInfo[keyNum];
    }
    return result;
}

void stdControl_FinishRead()
{
    ;
}

int stdControl_MessageHandler(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam, LRESULT* unused)
{
    if ( Msg != 0x112 )
        return 0;
    return wParam == 0xF100 || wParam == 0xF140;
}

void stdControl_SetMouseSensitivity(flex_t xSensitivity, flex_t ySensitivity)
{
    stdControl_mouseXSensitivity = xSensitivity;
    stdControl_mouseYSensitivity = ySensitivity;
    if ( (stdControl_aAxes[AXIS_MOUSE_X].flags & 1) != 0 )
    {
        stdControl_aAxes[AXIS_MOUSE_X].dwYoffs = 0;
        stdControl_aAxes[AXIS_MOUSE_X].uMaxVal = (__int64)(xSensitivity * 250.0);
        stdControl_aAxes[AXIS_MOUSE_X].uMinVal = -stdControl_aAxes[AXIS_MOUSE_X].uMaxVal;
        stdControl_aAxes[AXIS_MOUSE_X].flags |= 1u;
        stdControl_aAxes[AXIS_MOUSE_X].dwXoffs = (2 * stdControl_aAxes[AXIS_MOUSE_X].uMaxVal + 1) / 2 - stdControl_aAxes[AXIS_MOUSE_X].uMaxVal;
        stdControl_aAxes[AXIS_MOUSE_X].fRangeConversion = 1.0 / (flex_d_t)(stdControl_aAxes[AXIS_MOUSE_X].uMaxVal - stdControl_aAxes[AXIS_MOUSE_X].dwXoffs);
    }
    if ( (stdControl_aAxes[AXIS_MOUSE_Y].flags & 1) != 0 )
    {
        stdControl_aAxes[AXIS_MOUSE_Y].dwYoffs = 0;
        stdControl_aAxes[AXIS_MOUSE_Y].uMaxVal = (__int64)(ySensitivity * 200.0);
        stdControl_aAxes[AXIS_MOUSE_Y].uMinVal = -stdControl_aAxes[AXIS_MOUSE_Y].uMaxVal;
        stdControl_aAxes[AXIS_MOUSE_Y].flags |= 1u;
        stdControl_aAxes[AXIS_MOUSE_Y].dwXoffs = (2 * stdControl_aAxes[AXIS_MOUSE_Y].uMaxVal + 1) / 2 - stdControl_aAxes[AXIS_MOUSE_Y].uMaxVal;
        stdControl_aAxes[AXIS_MOUSE_Y].fRangeConversion = 1.0 / (flex_d_t)(stdControl_aAxes[AXIS_MOUSE_Y].uMaxVal - stdControl_aAxes[AXIS_MOUSE_Y].dwXoffs);
    }
}

// more

void stdControl_UpdateKeyState(int keyNum, int bDown, uint32_t readTime)
{
    // Added: bounds check
    if (keyNum >= JK_NUM_KEYS || keyNum < 0)
        return;

    // Added: Prevent idle if not applicable
    if (bDown && !stdControl_aKeyInfo[keyNum]) {
        stdControl_bControlsIdle = 0;
    }
    if (!bDown && stdControl_aKeyInfo[keyNum]) {
        stdControl_bControlsIdle = 0;
    }

#ifdef TARGET_RETRO_HOMEBREW
    // TODO: I think the intent is to allow polling for inputs on a separate thread?
    // This is a perf optimization to avoid memsetting
    stdControl_aKeyPressed[keyNum] = 0;
    stdControl_aKeyIdleTimes[keyNum] = 0;
#endif

    if ( !bDown || stdControl_aKeyInfo[keyNum] )
    {
        if ( !bDown && stdControl_aKeyInfo[keyNum] )
        {
            stdControl_aKeyInfo[keyNum] = 0;
            if ( !stdControl_aKeyIdleTimes[keyNum] )
                stdControl_aKeyIdleTimes[keyNum] = stdControl_readDeltaTime;
            stdControl_aKeyIdleTimes[keyNum] += readTime - stdControl_curReadTime;
        }
    }
    else
    {
        stdControl_aKeyInfo[keyNum] = 1;
        stdControl_aKeyIdleTimes[keyNum] = stdControl_curReadTime - readTime;
        ++stdControl_aKeyPressed[keyNum];
    }
}

// readmouse

void stdControl_RegisterAxis(int index, int stickMin, int stickMax, flex_t multiplier)
{
    int v4; // eax
    int v5; // esi
    flex_d_t v6; // st7

    // Added: OOB
    if (index >= JK_NUM_AXES) {
        return;
    }

    v4 = stickMin + (stickMax - stickMin + 1) / 2;
    v5 = index;
    stdControl_aAxes[v5].flags = stdControl_aAxes[index].flags | 1;
    stdControl_aAxes[v5].uMinVal = stickMin;
    stdControl_aAxes[v5].uMaxVal = stickMax;
    stdControl_aAxes[v5].dwXoffs = v4;
    v6 = (flex_d_t)(stickMax - v4);
    stdControl_aAxes[v5].fRangeConversion = 1.0 / v6;
    if ( multiplier == 0.0 )
        stdControl_aAxes[index].dwYoffs = 0;
    else
        stdControl_aAxes[index].dwYoffs = (__int64)(multiplier * v6);
}
