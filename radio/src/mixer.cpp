/*
 * Authors (alphabetical order)
 * - Andre Bernet <bernet.andre@gmail.com>
 * - Andreas Weitl
 * - Bertrand Songis <bsongis@gmail.com>
 * - Bryan J. Rentoul (Gruvin) <gruvin@gmail.com>
 * - Cameron Weeks <th9xer@gmail.com>
 * - Erez Raviv
 * - Gabriel Birkus
 * - Jean-Pierre Parisy
 * - Karl Szmutny
 * - Michael Blandford
 * - Michal Hlavinka
 * - Pat Mackenzie
 * - Philip Moss
 * - Rob Thomson
 * - Romolo Manfredini <romolo.manfredini@gmail.com>
 * - Thomas Husterer
 *
 * opentx is based on code named
 * gruvin9x by Bryan J. Rentoul: http://code.google.com/p/gruvin9x/,
 * er9x by Erez Raviv: http://code.google.com/p/er9x/,
 * and the original (and ongoing) project by
 * Thomas Husterer, th9x: http://code.google.com/p/th9x/
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include "opentx.h"

#if defined(PCBTARANIS)
  int8_t  virtualInputsTrims[NUM_INPUTS];
#else
  int16_t rawAnas[NUM_INPUTS] = {0};
#endif

int16_t  anas [NUM_INPUTS] = {0};
int16_t  trims[NUM_STICKS] = {0};
int32_t  chans[NUM_CHNOUT] = {0};
BeepANACenter bpanaCenter = 0;

int24_t act   [MAX_MIXERS] = {0};
SwOn    swOn  [MAX_MIXERS]; // TODO better name later...

uint8_t mixWarning;

#if defined(MODULE_ALWAYS_SEND_PULSES)
  uint8_t startupWarningState;
#endif

int16_t calibratedStick[NUM_STICKS+NUM_POTS];
int16_t channelOutputs[NUM_CHNOUT] = {0};
int16_t ex_chans[NUM_CHNOUT] = {0}; // Outputs (before LIMITS) of the last perMain;

#if defined(HELI)
  int16_t cyc_anas[3] = {0};
  #if defined(PCBTARANIS)
    int16_t heliAnas[4] = {0};
  #endif
#endif

void applyExpos(int16_t *anas, uint8_t mode APPLY_EXPOS_EXTRA_PARAMS)
{
#if defined(PCBTARANIS)
#if defined(HELI)
  int16_t heliAnasCopy[4];
  memcpy(heliAnasCopy, heliAnas, sizeof(heliAnasCopy));
#endif
#else
  int16_t anas2[NUM_INPUTS]; // values before expo, to ensure same expo base when multiple expo lines are used
  memcpy(anas2, anas, sizeof(anas2));
#endif

  int8_t cur_chn = -1;

  for (uint8_t i=0; i<MAX_EXPOS; i++) {
#if defined(BOLD_FONT)
    if (mode==e_perout_mode_normal) swOn[i].activeExpo = false;
#endif
    ExpoData * ed = expoAddress(i);
    if (!EXPO_VALID(ed)) break; // end of list
    if (ed->chn == cur_chn)
      continue;
    if (ed->phases & (1<<s_current_mixer_flight_mode))
      continue;
    if (getSwitch(ed->swtch)) {
#if defined(PCBTARANIS)
      int v;
      if (ed->srcRaw == ovwrIdx)
        v = ovwrValue;
#if defined(HELI)
      else if (ed->srcRaw == MIXSRC_Ele)
        v = heliAnasCopy[ELE_STICK];
      else if (ed->srcRaw == MIXSRC_Ail)
        v = heliAnasCopy[AIL_STICK];
#endif
      else {
        v = getValue(ed->srcRaw);
        if (ed->srcRaw >= MIXSRC_FIRST_TELEM && ed->scale > 0) {
          v = limit(-1024, int((v * 1024) / convertTelemValue(ed->srcRaw-MIXSRC_FIRST_TELEM+1, ed->scale)), 1024);
        }
      }
#else
      int16_t v = anas2[ed->chn];
#endif
      if (EXPO_MODE_ENABLE(ed, v)) {
#if defined(BOLD_FONT)
        if (mode==e_perout_mode_normal) swOn[i].activeExpo = true;
#endif
        cur_chn = ed->chn;

        //========== CURVE=================
#if defined(PCBTARANIS)
        if (ed->curve.value) {
          v = applyCurve(v, ed->curve);
        }
#else
        int8_t curveParam = ed->curveParam;
        if (curveParam) {
          if (ed->curveMode == MODE_CURVE)
            v = applyCurve(v, curveParam);
          else
            v = expo(v, GET_GVAR(curveParam, -100, 100, s_current_mixer_flight_mode));
        }
#endif

        //========== WEIGHT ===============
        int16_t weight = GET_GVAR(ed->weight, MIN_EXPO_WEIGHT, 100, s_current_mixer_flight_mode);
        weight = calc100to256(weight);
        v = ((int32_t)v * weight) >> 8;

#if defined(PCBTARANIS)
        //========== OFFSET ===============
        int16_t offset = GET_GVAR(ed->offset, -100, 100, s_current_mixer_flight_mode);
        if (offset) v += calc100toRESX(offset);

        //========== TRIMS ================
        if (ed->carryTrim < TRIM_ON)
          virtualInputsTrims[cur_chn] = -ed->carryTrim - 1;
        else if (ed->carryTrim == TRIM_ON && ed->srcRaw >= MIXSRC_Rud && ed->srcRaw <= MIXSRC_Ail)
          virtualInputsTrims[cur_chn] = ed->srcRaw - MIXSRC_Rud;
        else
          virtualInputsTrims[cur_chn] = -1;

#if defined(HELI)
        if (ed->srcRaw == MIXSRC_Ele)
          heliAnas[ELE_STICK] = v;
        else if (ed->srcRaw == MIXSRC_Ail)
          heliAnas[AIL_STICK] = v;
#endif
#endif

        anas[cur_chn] = v;
      }
    }
  }
}

// #define PREVENT_ARITHMETIC_OVERFLOW
// because of optimizations the reserves before overruns occurs is only the half
// this defines enables some checks the greatly improves this situation
// It should nearly prevent all overruns (is still a chance for it, but quite low)
// negative side is code cost 96 bytes flash

// we do it now half way, only in applyLimits, which costs currently 50bytes
// according opinion poll this topic is currently not very important
// the change below improves already the situation
// the check inside mixer would slow down mix a little bit and costs additionally flash
// also the check inside mixer still is not bulletproof, there may be still situations a overflow could occur
// a bulletproof implementation would take about additional 100bytes flash
// therefore with go with this compromize, interested people could activate this define

// @@@2 open.20.fsguruh ;
// channel = channelnumber -1;
// value = outputvalue with 100 mulitplied usual range -102400 to 102400; output -1024 to 1024
// changed rescaling from *100 to *256 to optimize performance
// rescaled from -262144 to 262144
int16_t applyLimits(uint8_t channel, int32_t value)
{
  LimitData * lim = limitAddress(channel);

#if defined(PCBTARANIS)
  if (lim->curve) {
    // TODO we loose precision here, applyCustomCurve could work with int32_t on ARM boards...
    if (lim->curve > 0)
      value = 256 * applyCustomCurve(value/256, lim->curve-1);
    else
      value = 256 * applyCustomCurve(-value/256, -lim->curve-1);
  }
#endif

  int16_t ofs   = LIMIT_OFS_RESX(lim);
  int16_t lim_p = LIMIT_MAX_RESX(lim);
  int16_t lim_n = LIMIT_MIN_RESX(lim);

  if (ofs > lim_p) ofs = lim_p;
  if (ofs < lim_n) ofs = lim_n;

  // because the rescaling optimization would reduce the calculation reserve we activate this for all builds
  // it increases the calculation reserve from factor 20,25x to 32x, which it slightly better as original
  // without it we would only have 16x which is slightly worse as original, we should not do this

  // thanks to gbirkus, he motivated this change, which greatly reduces overruns
  // unfortunately the constants and 32bit compares generates about 50 bytes codes; didn't find a way to get it down.
  value = limit(int32_t(-RESXl*256), value, int32_t(RESXl*256));  // saves 2 bytes compared to other solutions up to now

#if defined(PPM_LIMITS_SYMETRICAL)
  if (value) {
    int16_t tmp;
    if (lim->symetrical)
      tmp = (value > 0) ? (lim_p) : (-lim_n);
    else
      tmp = (value > 0) ? (lim_p - ofs) : (-lim_n + ofs);
    value = (int32_t) value * tmp;   //  div by 1024*256 -> output = -1024..1024
#else
  if (value) {
    int16_t tmp = (value > 0) ? (lim_p - ofs) : (-lim_n + ofs);
    value = (int32_t) value * tmp;   //  div by 1024*256 -> output = -1024..1024
#endif

#ifdef CORRECT_NEGATIVE_SHIFTS
    int8_t sign = (value<0?1:0);
    value -= sign;
    tmp = value>>16;   // that's quite tricky: the shiftright 16 operation is assmbled just with addressmove; just forget the two least significant bytes;
    tmp >>= 2;   // now one simple shift right for two bytes does the rest
    tmp += sign;
#else
    tmp = value>>16;   // that's quite tricky: the shiftright 16 operation is assmbled just with addressmove; just forget the two least significant bytes;
    tmp >>= 2;   // now one simple shift right for two bytes does the rest
#endif

    ofs += tmp;  // ofs can to added directly because already recalculated,
  }

  if (ofs > lim_p) ofs = lim_p;
  if (ofs < lim_n) ofs = lim_n;

  if (lim->revert) ofs = -ofs; // finally do the reverse.

  if (safetyCh[channel] != -128) // if safety channel available for channel check
    ofs = calc100toRESX(safetyCh[channel]);

  return ofs;
}

// TODO same naming convention than the putsMixerSource

getvalue_t getValue(uint8_t i)
{
  if (i==MIXSRC_NONE) return 0;

#if defined(PCBTARANIS)
  else if (i <= MIXSRC_LAST_INPUT) {
    return anas[i-MIXSRC_FIRST_INPUT];
  }
#endif

#if defined(PCBTARANIS)
  else if (i<MIXSRC_LAST_LUA) {
#if defined(LUA_MODEL_SCRIPTS)
    div_t qr = div(i-MIXSRC_FIRST_LUA, MAX_SCRIPT_OUTPUTS);
    return scriptInternalData[qr.quot].outputs[qr.rem].value;
#else
    return 0;
#endif
  }
#endif

  else if (i<=MIXSRC_LAST_POT) return calibratedStick[i-MIXSRC_Rud];

#if defined(PCBGRUVIN9X) || defined(PCBMEGA2560) || defined(ROTARY_ENCODERS)
  else if (i<=MIXSRC_LAST_ROTARY_ENCODER) return getRotaryEncoder(i-MIXSRC_REa);
#endif

  else if (i==MIXSRC_MAX) return 1024;

  else if (i<=MIXSRC_CYC3)
#if defined(HELI)
    return cyc_anas[i-MIXSRC_CYC1];
#else
    return 0;
#endif

  else if (i<=MIXSRC_TrimAil) return calc1000toRESX((int16_t)8 * getTrimValue(s_current_mixer_flight_mode, i-MIXSRC_TrimRud));

#if defined(PCBTARANIS)
  else if (i==MIXSRC_SA) return (switchState(SW_SA0) ? -1024 : (switchState(SW_SA1) ? 0 : 1024));
  else if (i==MIXSRC_SB) return (switchState(SW_SB0) ? -1024 : (switchState(SW_SB1) ? 0 : 1024));
  else if (i==MIXSRC_SC) return (switchState(SW_SC0) ? -1024 : (switchState(SW_SC1) ? 0 : 1024));
  else if (i==MIXSRC_SD) return (switchState(SW_SD0) ? -1024 : (switchState(SW_SD1) ? 0 : 1024));
  else if (i==MIXSRC_SE) return (switchState(SW_SE0) ? -1024 : (switchState(SW_SE1) ? 0 : 1024));
  else if (i==MIXSRC_SF) return (switchState(SW_SF0) ? -1024 : 1024);
  else if (i==MIXSRC_SG) return (switchState(SW_SG0) ? -1024 : (switchState(SW_SG1) ? 0 : 1024));
  else if (i==MIXSRC_SH) return (switchState(SW_SH0) ? -1024 : 1024);
  else if (i<=MIXSRC_LAST_LOGICAL_SWITCH) return getSwitch(SWSRC_FIRST_LOGICAL_SWITCH+i-MIXSRC_FIRST_LOGICAL_SWITCH) ? 1024 : -1024;
#else
  else if (i==MIXSRC_3POS) return (getSwitch(SW_ID0-SW_BASE+1) ? -1024 : (getSwitch(SW_ID1-SW_BASE+1) ? 0 : 1024));
  // don't use switchState directly to give getSwitch possibility to hack values if needed for switch warning
#if defined(EXTRA_3POS)
  else if (i==MIXSRC_3POS2) return (getSwitch(SW_ID3-SW_BASE+1) ? -1024 : (getSwitch(SW_ID4-SW_BASE+1) ? 0 : 1024));
  // don't use switchState directly to give getSwitch possibility to hack values if needed for switch warning
#endif
  else if (i<=MIXSRC_LAST_LOGICAL_SWITCH) return getSwitch(SWSRC_THR+i-MIXSRC_THR) ? 1024 : -1024;
#endif

  else if (i<=MIXSRC_LAST_TRAINER) { int16_t x = g_ppmIns[i-MIXSRC_FIRST_TRAINER]; if (i<MIXSRC_FIRST_TRAINER+NUM_CAL_PPM) { x-= g_eeGeneral.trainer.calib[i-MIXSRC_FIRST_TRAINER]; } return x*2; }
  else if (i<=MIXSRC_LAST_CH) return ex_chans[i-MIXSRC_CH1];

#if defined(GVARS)
  else if (i<=MIXSRC_LAST_GVAR) return GVAR_VALUE(i-MIXSRC_GVAR1, getGVarFlightPhase(s_current_mixer_flight_mode, i-MIXSRC_GVAR1));
#endif

  else if (i==MIXSRC_FIRST_TELEM-1+TELEM_TX_VOLTAGE) return g_vbat100mV;
#if defined(CPUARM) && defined(RTCLOCK)
  else if (i==MIXSRC_FIRST_TELEM-1+TELEM_TX_TIME) {
    struct gtm t;
    gettime(&t);
    return t.tm_hour*60 + t.tm_min;
  }
#endif
  else if (i<=MIXSRC_FIRST_TELEM-1+TELEM_TM2) return timersStates[i-MIXSRC_FIRST_TELEM+1-TELEM_TM1].val;
#if defined(FRSKY)
#if defined(CPUARM)
  else if (i==MIXSRC_FIRST_TELEM-1+TELEM_SWR) return frskyData.swr.value;
#endif
  else if (i==MIXSRC_FIRST_TELEM-1+TELEM_RSSI_TX) return frskyData.rssi[1].value;
  else if (i==MIXSRC_FIRST_TELEM-1+TELEM_RSSI_RX) return frskyData.rssi[0].value;
  else if (i<=MIXSRC_FIRST_TELEM-1+TELEM_A2) return frskyData.analog[i-MIXSRC_FIRST_TELEM+1-TELEM_A1].value;
#if defined(FRSKY_SPORT)
  else if (i==MIXSRC_FIRST_TELEM-1+TELEM_ALT) return frskyData.hub.baroAltitude;
#elif defined(FRSKY_HUB) || defined(WS_HOW_HIGH)
  else if (i==MIXSRC_FIRST_TELEM-1+TELEM_ALT) return TELEMETRY_RELATIVE_BARO_ALT_BP;
#endif
#if defined(FRSKY_HUB)
  else if (i==MIXSRC_FIRST_TELEM-1+TELEM_RPM) return frskyData.hub.rpm;
  else if (i==MIXSRC_FIRST_TELEM-1+TELEM_FUEL) return frskyData.hub.fuelLevel;
  else if (i==MIXSRC_FIRST_TELEM-1+TELEM_T1) return frskyData.hub.temperature1;
  else if (i==MIXSRC_FIRST_TELEM-1+TELEM_T2) return frskyData.hub.temperature2;
  else if (i==MIXSRC_FIRST_TELEM-1+TELEM_SPEED) return TELEMETRY_GPS_SPEED_BP;
  else if (i==MIXSRC_FIRST_TELEM-1+TELEM_DIST) return frskyData.hub.gpsDistance;
  else if (i==MIXSRC_FIRST_TELEM-1+TELEM_GPSALT) return TELEMETRY_RELATIVE_GPS_ALT_BP;
  else if (i==MIXSRC_FIRST_TELEM-1+TELEM_CELL) return (int16_t)TELEMETRY_MIN_CELL_VOLTAGE;
  else if (i==MIXSRC_FIRST_TELEM-1+TELEM_CELLS_SUM) return (int16_t)frskyData.hub.cellsSum;
  else if (i==MIXSRC_FIRST_TELEM-1+TELEM_VFAS) return (int16_t)frskyData.hub.vfas;
  else if (i==MIXSRC_FIRST_TELEM-1+TELEM_CURRENT) return (int16_t)frskyData.hub.current;
  else if (i==MIXSRC_FIRST_TELEM-1+TELEM_CONSUMPTION) return frskyData.hub.currentConsumption;
  else if (i==MIXSRC_FIRST_TELEM-1+TELEM_POWER) return frskyData.hub.power;
  else if (i==MIXSRC_FIRST_TELEM-1+TELEM_ACCx) return frskyData.hub.accelX;
  else if (i==MIXSRC_FIRST_TELEM-1+TELEM_ACCy) return frskyData.hub.accelY;
  else if (i==MIXSRC_FIRST_TELEM-1+TELEM_ACCz) return frskyData.hub.accelZ;
  else if (i==MIXSRC_FIRST_TELEM-1+TELEM_HDG) return frskyData.hub.gpsCourse_bp;
  else if (i==MIXSRC_FIRST_TELEM-1+TELEM_VSPEED) return frskyData.hub.varioSpeed;
  else if (i==MIXSRC_FIRST_TELEM-1+TELEM_ASPEED) return frskyData.hub.airSpeed;
  else if (i==MIXSRC_FIRST_TELEM-1+TELEM_DTE) return frskyData.hub.dTE;
  else if (i==MIXSRC_FIRST_TELEM-1+TELEM_MIN_A1) return frskyData.analog[0].min;
  else if (i==MIXSRC_FIRST_TELEM-1+TELEM_MIN_A2) return frskyData.analog[1].min;
  else if (i<=MIXSRC_FIRST_TELEM-1+TELEM_CSW_MAX) return *(((int16_t*)(&frskyData.hub.minAltitude))+i-(MIXSRC_FIRST_TELEM-1+TELEM_MIN_ALT));
#endif
#endif
  else return 0;
}

void evalInputs(uint8_t mode)
{
  BeepANACenter anaCenter = 0;

#if defined(HELI)
  uint16_t d = 0;
  if (g_model.swashR.value) {
    uint32_t v = (int32_t(calibratedStick[ELE_STICK])*calibratedStick[ELE_STICK] + int32_t(calibratedStick[AIL_STICK])*calibratedStick[AIL_STICK]);
    uint32_t q = calc100toRESX(g_model.swashR.value);
    q *= q;
    if (v > q) {
      d = isqrt32(v);
    }
  }
#endif

  for (uint8_t i=0; i<NUM_STICKS+NUM_POTS+NUM_ROTARY_ENCODERS; i++) {

    // normalization [0..2048] -> [-1024..1024]
    uint8_t ch = (i < NUM_STICKS ? CONVERT_MODE(i) : i);

#if defined(ROTARY_ENCODERS)
    int16_t v = ((i < NUM_STICKS+NUM_POTS) ? anaIn(i) : getRotaryEncoder(i-(NUM_STICKS+NUM_POTS)));
#else
    int16_t v = anaIn(i);
#endif

#if !defined(SIMU)
    if (i < NUM_STICKS+NUM_POTS) {
      if (IS_POT_MULTIPOS(i)) {
        v -= RESX;
      }
      else {
        CalibData * calib = &g_eeGeneral.calib[i];
        v -= calib->mid;
        v = v * (int32_t)RESX / (max((int16_t)100, (v>0 ? calib->spanPos : calib->spanNeg)));
      }
    }
#endif

    if (v < -RESX) v = -RESX;
    if (v >  RESX) v =  RESX;

#if defined(PCBTARANIS)
    if (i==POT1 || i==SLIDER1) {
      v = -v;
    }
#endif

    if (g_model.throttleReversed && ch==THR_STICK) {
      v = -v;
    }

#if defined(EXTRA_3POS)
    if (i == POT1+EXTRA_3POS-1) {
      if (v < -RESX/2)
        v = -RESX;
      else if (v > +RESX/2)
        v = +RESX;
      else
        v = 0;
    }
#endif

    BeepANACenter mask = (BeepANACenter)1 << ch;

    if (i < NUM_STICKS+NUM_POTS) {

      calibratedStick[ch] = v; // for show in expo

      // filtering for center beep
      uint8_t tmp = (uint16_t)abs(v) / 16;
#if defined(CPUARM)
      if (mode == e_perout_mode_normal) {
        if (tmp==0 || (tmp==1 && (bpanaCenter & mask))) {
          anaCenter |= mask;
          if ((g_model.beepANACenter & mask) && !(bpanaCenter & mask)) {
            AUDIO_POT_MIDDLE(i);
          }
        }
      }
#else
      if (tmp <= 1) anaCenter |= (tmp==0 ? mask : (bpanaCenter & mask));
#endif
    }
    else {
      // rotary encoders
      if (v == 0) anaCenter |= mask;
    }

    if (ch < NUM_STICKS) { //only do this for sticks
#if defined(PCBTARANIS)
      if (mode & e_perout_mode_nosticks) {
        calibratedStick[ch] = 0;
      }
#endif

      if (mode <= e_perout_mode_inactive_phase && isFunctionActive(FUNCTION_TRAINER+ch) && ppmInValid) {
        // trainer mode
        TrainerMix* td = &g_eeGeneral.trainer.mix[ch];
        if (td->mode) {
          uint8_t chStud = td->srcChn;
          int32_t vStud  = (g_ppmIns[chStud]- g_eeGeneral.trainer.calib[chStud]);
          vStud *= td->studWeight;
          vStud /= 50;
          switch (td->mode) {
            case 1: v += vStud;   break; // add-mode
            case 2: v  = vStud;   break; // subst-mode
          }
#if defined(PCBTARANIS)
          calibratedStick[ch] = v;
#endif
        }
      }

#if defined(HELI)
      if (d && (ch==ELE_STICK || ch==AIL_STICK)) {
        v = (int32_t(v) * calc100toRESX(g_model.swashR.value)) / int32_t(d);
      }
#if defined(PCBTARANIS)
      heliAnas[ch] = v;
#endif
#endif

#if !defined(PCBTARANIS)
      rawAnas[ch] = v;
      anas[ch] = v; //set values for mixer
#endif
    }
  }

  /* TRIMs */
  evalTrims();

  /* EXPOs */
  applyExpos(anas, mode);

  if (mode == e_perout_mode_normal) {
#if !defined(CPUARM)
    anaCenter &= g_model.beepANACenter;
    if(((bpanaCenter ^ anaCenter) & anaCenter)) AUDIO_POT_MIDDLE();
#endif
    bpanaCenter = anaCenter;
  }
}

#if defined(PCBTARANIS)
  #define HELI_ANAS_ARRAY heliAnas
#else
  #define HELI_ANAS_ARRAY anas
#endif

uint8_t s_current_mixer_flight_mode;
void evalFlightModeMixes(uint8_t mode, uint8_t tick10ms)
{
  evalInputs(mode);

  evalLogicalSwitches(mode==e_perout_mode_normal);

#if defined(MODULE_ALWAYS_SEND_PULSES)
  checkStartupWarnings();
#endif

#if defined(HELI)
  if (g_model.swashR.value) {
    uint32_t v = ((int32_t)HELI_ANAS_ARRAY[ELE_STICK]*HELI_ANAS_ARRAY[ELE_STICK] + (int32_t)HELI_ANAS_ARRAY[AIL_STICK]*HELI_ANAS_ARRAY[AIL_STICK]);
    uint32_t q = calc100toRESX(g_model.swashR.value);
    q *= q;
    if (v>q) {
      uint16_t d = isqrt32(v);
      int16_t tmp = calc100toRESX(g_model.swashR.value);
      HELI_ANAS_ARRAY[ELE_STICK] = (int32_t) HELI_ANAS_ARRAY[ELE_STICK]*tmp/d;
      HELI_ANAS_ARRAY[AIL_STICK] = (int32_t) HELI_ANAS_ARRAY[AIL_STICK]*tmp/d;
    }
  }

#define REZ_SWASH_X(x)  ((x) - (x)/8 - (x)/128 - (x)/512)   //  1024*sin(60) ~= 886
#define REZ_SWASH_Y(x)  ((x))   //  1024 => 1024

  if (g_model.swashR.type) {
    getvalue_t vp = HELI_ANAS_ARRAY[ELE_STICK]+trims[ELE_STICK];
    getvalue_t vr = HELI_ANAS_ARRAY[AIL_STICK]+trims[AIL_STICK];
    getvalue_t vc = 0;
    if (g_model.swashR.collectiveSource)
      vc = getValue(g_model.swashR.collectiveSource);

    if (g_model.swashR.invertELE) vp = -vp;
    if (g_model.swashR.invertAIL) vr = -vr;
    if (g_model.swashR.invertCOL) vc = -vc;

    switch (g_model.swashR.type) {
      case SWASH_TYPE_120:
        vp = REZ_SWASH_Y(vp);
        vr = REZ_SWASH_X(vr);
        cyc_anas[0] = vc - vp;
        cyc_anas[1] = vc + vp/2 + vr;
        cyc_anas[2] = vc + vp/2 - vr;
        break;
      case SWASH_TYPE_120X:
        vp = REZ_SWASH_X(vp);
        vr = REZ_SWASH_Y(vr);
        cyc_anas[0] = vc - vr;
        cyc_anas[1] = vc + vr/2 + vp;
        cyc_anas[2] = vc + vr/2 - vp;
        break;
      case SWASH_TYPE_140:
        vp = REZ_SWASH_Y(vp);
        vr = REZ_SWASH_Y(vr);
        cyc_anas[0] = vc - vp;
        cyc_anas[1] = vc + vp + vr;
        cyc_anas[2] = vc + vp - vr;
        break;
      case SWASH_TYPE_90:
        vp = REZ_SWASH_Y(vp);
        vr = REZ_SWASH_Y(vr);
        cyc_anas[0] = vc - vp;
        cyc_anas[1] = vc + vr;
        cyc_anas[2] = vc - vr;
        break;
      default:
        break;
    }
  }
#endif

  memclear(chans, sizeof(chans));        // All outputs to 0

  //========== MIXER LOOP ===============
  uint8_t lv_mixWarning = 0;

  uint8_t pass = 0;

  bitfield_channels_t dirtyChannels = (bitfield_channels_t)-1; // all dirty when mixer starts

  do {

    bitfield_channels_t passDirtyChannels = 0;

    for (uint8_t i=0; i<MAX_MIXERS; i++) {

#if defined(BOLD_FONT)
      if (mode==e_perout_mode_normal && pass==0) swOn[i].activeMix = 0;
#endif

      MixData *md = mixAddress(i);

      if (md->srcRaw == 0) break;

      uint8_t stickIndex = md->srcRaw - MIXSRC_Rud;

      if (!(dirtyChannels & ((bitfield_channels_t)1 << md->destCh))) continue;

      // if this is the first calculation for the destination channel, initialize it with 0 (otherwise would be random)
      if (i == 0 || md->destCh != (md-1)->destCh) {
        chans[md->destCh] = 0;
      }

      //========== PHASE && SWITCH =====
      bool mixCondition = (md->phases != 0 || md->swtch);
      delayval_t mixEnabled = !(md->phases & (1 << s_current_mixer_flight_mode)) && getSwitch(md->swtch);

      if (mixEnabled && md->srcRaw >= MIXSRC_FIRST_TRAINER && md->srcRaw <= MIXSRC_LAST_TRAINER && !ppmInValid) {
        mixEnabled = 0;
      }

      //========== VALUE ===============
      getvalue_t v = 0;
      if (mode > e_perout_mode_inactive_phase) {
#if defined(PCBTARANIS)
        if (!mixEnabled) {
          continue;
        }
        else {
          v = getValue(md->srcRaw);
        }
#else
        if (!mixEnabled || stickIndex >= NUM_STICKS || (stickIndex == THR_STICK && g_model.thrTrim)) {
          continue;
        }
        else {
          if (!(mode & e_perout_mode_nosticks)) v = anas[stickIndex];
        }
#endif
      }
      else {
#if !defined(PCBTARANIS)
        if (stickIndex < NUM_STICKS) {
          v = md->noExpo ? rawAnas[stickIndex] : anas[stickIndex];
        }
        else
#endif
        {
          int8_t srcRaw = MIXSRC_Rud + stickIndex;
          v = getValue(srcRaw);
          srcRaw -= MIXSRC_CH1;
          if (srcRaw>=0 && srcRaw<=MIXSRC_LAST_CH-MIXSRC_CH1 && md->destCh != srcRaw) {
            if (dirtyChannels & ((bitfield_channels_t)1 << srcRaw) & (passDirtyChannels|~(((bitfield_channels_t) 1 << md->destCh)-1)))
              passDirtyChannels |= (bitfield_channels_t) 1 << md->destCh;
            if (srcRaw < md->destCh || pass > 0)
              v = chans[srcRaw] >> 8;
          }
        }
        if (!mixCondition) {
          mixEnabled = v >> DELAY_POS_SHIFT;
        }
      }

      bool apply_offset_and_curve = true;

      //========== DELAYS ===============
      delayval_t _swOn = swOn[i].now;
      delayval_t _swPrev = swOn[i].prev;
      bool swTog = (mixEnabled != _swOn);
      if (mode==e_perout_mode_normal && swTog) {
        if (!swOn[i].delay) _swPrev = _swOn;
        swOn[i].delay = (mixEnabled > _swOn ? md->delayUp : md->delayDown) * (100/DELAY_STEP);
        swOn[i].now = mixEnabled;
        swOn[i].prev = _swPrev;
      }
      if (mode==e_perout_mode_normal && swOn[i].delay > 0) {
        swOn[i].delay = max<int16_t>(0, (int16_t)swOn[i].delay - tick10ms);
        if (!mixCondition)
          v = _swPrev << DELAY_POS_SHIFT;
        else if (mixEnabled)
          continue;
      }
      else if (!mixEnabled) {
        if ((md->speedDown || md->speedUp) && md->mltpx!=MLTPX_REP) {
          if (mixCondition) {
            v = (md->mltpx == MLTPX_ADD ? 0 : RESX);
            apply_offset_and_curve = false;
          }
        }
        else if (mixCondition) {
          continue;
        }
      }

      if (mode==e_perout_mode_normal && (!mixCondition || mixEnabled || swOn[i].delay)) {
        if (md->mixWarn) lv_mixWarning |= 1 << (md->mixWarn - 1);
#if defined(BOLD_FONT)
        swOn[i].activeMix = true;
#endif
      }

      if (apply_offset_and_curve) {

        //========== TRIMS ================
        if (!(mode & e_perout_mode_notrims)) {
#if defined(PCBTARANIS)
          if (md->carryTrim == 0) {
            int8_t mix_trim;
            if (stickIndex < NUM_STICKS)
              mix_trim = stickIndex;
            else if (md->srcRaw <= MIXSRC_LAST_INPUT)
              mix_trim = virtualInputsTrims[md->srcRaw-1];
            else
              mix_trim = -1;
            if (mix_trim >= 0) {
              int16_t trim = trims[mix_trim];
              if (mix_trim == THR_STICK && g_model.throttleReversed)
                v -= trim;
              else
                v += trim;
            }
          }
#else
          int8_t mix_trim = md->carryTrim;
          if (mix_trim < TRIM_ON)
            mix_trim = -mix_trim - 1;
          else if (mix_trim == TRIM_ON && stickIndex < NUM_STICKS)
            mix_trim = stickIndex;
          else
            mix_trim = -1;
          if (mix_trim >= 0) {
            int16_t trim = trims[mix_trim];
            if (mix_trim == THR_STICK && g_model.throttleReversed)
              v -= trim;
            else
              v += trim;
          }
#endif
        }
      }

      // saves 12 bytes code if done here and not together with weight; unknown reason
      int16_t weight = GET_GVAR(MD_WEIGHT(md), GV_RANGELARGE_NEG, GV_RANGELARGE, s_current_mixer_flight_mode);
      weight = calc100to256_16Bits(weight);

      //========== SPEED ===============
      // now its on input side, but without weight compensation. More like other remote controls
      // lower weight causes slower movement

      if (mode <= e_perout_mode_inactive_phase && (md->speedUp || md->speedDown)) { // there are delay values
#define DEL_MULT_SHIFT 8
        // we recale to a mult 256 higher value for calculation
        int32_t tact = act[i];
        int16_t diff = v - (tact>>DEL_MULT_SHIFT);
        if (diff) {
          // open.20.fsguruh: speed is defined in % movement per second; In menu we specify the full movement (-100% to 100%) = 200% in total
          // the unit of the stored value is the value from md->speedUp or md->speedDown divide SLOW_STEP seconds; e.g. value 4 means 4/SLOW_STEP = 2 seconds for CPU64
          // because we get a tick each 10msec, we need 100 ticks for one second
          // the value in md->speedXXX gives the time it should take to do a full movement from -100 to 100 therefore 200%. This equals 2048 in recalculated internal range
          if (tick10ms || !s_mixer_first_run_done) {
            // only if already time is passed add or substract a value according the speed configured
            int32_t rate = (int32_t) tick10ms << (DEL_MULT_SHIFT+11);  // = DEL_MULT*2048*tick10ms
            // rate equals a full range for one second; if less time is passed rate is accordingly smaller
            // if one second passed, rate would be 2048 (full motion)*256(recalculated weight)*100(100 ticks needed for one second)
            int32_t currentValue = ((int32_t) v<<DEL_MULT_SHIFT);
            if (diff > 0) {
              if (s_mixer_first_run_done && md->speedUp > 0) {
                // if a speed upwards is defined recalculate the new value according configured speed; the higher the speed the smaller the add value is
                int32_t newValue = tact+rate/((int16_t)(100/SLOW_STEP)*md->speedUp);
                if (newValue<currentValue) currentValue = newValue; // Endposition; prevent toggling around the destination
              }
            }
            else {  // if is <0 because ==0 is not possible
              if (s_mixer_first_run_done && md->speedDown > 0) {
                // see explanation in speedUp
                int32_t newValue = tact-rate/((int16_t)(100/SLOW_STEP)*md->speedDown);
                if (newValue>currentValue) currentValue = newValue; // Endposition; prevent toggling around the destination
              }
            }
            act[i] = tact = currentValue;
            // open.20.fsguruh: this implementation would save about 50 bytes code
          } // endif tick10ms ; in case no time passed assign the old value, not the current value from source
          v = (tact >> DEL_MULT_SHIFT);
        }
      }

      //========== CURVES ===============
#if defined(PCBTARANIS)
      if (apply_offset_and_curve && md->curve.type != CURVE_REF_DIFF && md->curve.value) {
        v = applyCurve(v, md->curve);
      }
#else
      if (apply_offset_and_curve && md->curveParam && md->curveMode == MODE_CURVE) {
        v = applyCurve(v, md->curveParam);
      }
#endif

      //========== WEIGHT ===============
      int32_t dv = (int32_t) v * weight;

      //========== OFFSET / AFTER ===============
      if (apply_offset_and_curve) {
        int16_t offset = GET_GVAR(MD_OFFSET(md), GV_RANGELARGE_NEG, GV_RANGELARGE, s_current_mixer_flight_mode);
        if (offset) dv += calc100toRESX_16Bits(offset) << 8;
      }

      //========== DIFFERENTIAL =========
#if defined(PCBTARANIS)
      if (md->curve.type == CURVE_REF_DIFF && md->curve.value) {
        dv = applyCurve(dv, md->curve);
      }
#else
      if (md->curveMode == MODE_DIFFERENTIAL) {
        // @@@2 also recalculate curveParam to a 256 basis which ease the calculation later a lot
        int16_t curveParam = calc100to256(GET_GVAR(md->curveParam, -100, 100, s_current_mixer_flight_mode));
        if (curveParam > 0 && dv < 0)
          dv = (dv * (256 - curveParam)) >> 8;
        else if (curveParam < 0 && dv > 0)
          dv = (dv * (256 + curveParam)) >> 8;
      }
#endif

      int32_t *ptr = &chans[md->destCh]; // Save calculating address several times

      switch (md->mltpx) {
        case MLTPX_REP:
          *ptr = dv;
#if defined(BOLD_FONT)
          if (mode==e_perout_mode_normal) {
            for (uint8_t m=i-1; m<MAX_MIXERS && mixAddress(m)->destCh==md->destCh; m--)
              swOn[m].activeMix = false;
          }
#endif
          break;
        case MLTPX_MUL:
          // @@@2 we have to remove the weight factor of 256 in case of 100%; now we use the new base of 256
          dv >>= 8;
          dv *= *ptr;
          dv >>= RESX_SHIFT;   // same as dv /= RESXl;
          *ptr = dv;
          break;
        default: // MLTPX_ADD
          *ptr += dv; //Mixer output add up to the line (dv + (dv>0 ? 100/2 : -100/2))/(100);
          break;
      } //endswitch md->mltpx
#ifdef PREVENT_ARITHMETIC_OVERFLOW
/*
      // a lot of assumptions must be true, for this kind of check; not really worth for only 4 bytes flash savings
      // this solution would save again 4 bytes flash
      int8_t testVar=(*ptr<<1)>>24;
      if ( (testVar!=-1) && (testVar!=0 ) ) {
        // this devices by 64 which should give a good balance between still over 100% but lower then 32x100%; should be OK
        *ptr >>= 6;  // this is quite tricky, reduces the value a lot but should be still over 100% and reduces flash need
      } */


      PACK( union u_int16int32_t {
        struct {
          int16_t lo;
          int16_t hi;
        } words_t;
        int32_t dword;
      });

      u_int16int32_t tmp;
      tmp.dword=*ptr;

      if (tmp.dword<0) {
        if ((tmp.words_t.hi&0xFF80)!=0xFF80) tmp.words_t.hi=0xFF86; // set to min nearly
      }
      else {
        if ((tmp.words_t.hi|0x007F)!=0x007F) tmp.words_t.hi=0x0079; // set to max nearly
      }
      *ptr = tmp.dword;
      // this implementation saves 18bytes flash

/*      dv=*ptr>>8;
      if (dv>(32767-RESXl)) {
        *ptr=(32767-RESXl)<<8;
      } else if (dv<(-32767+RESXl)) {
        *ptr=(-32767+RESXl)<<8;
      }*/
      // *ptr=limit( int32_t(int32_t(-1)<<23), *ptr, int32_t(int32_t(1)<<23));  // limit code cost 72 bytes
      // *ptr=limit( int32_t((-32767+RESXl)<<8), *ptr, int32_t((32767-RESXl)<<8));  // limit code cost 80 bytes
#endif

    } //endfor mixers

    tick10ms = 0;
    dirtyChannels &= passDirtyChannels;

  } while (++pass < 5 && dirtyChannels);

  mixWarning = lv_mixWarning;
}

int32_t sum_chans512[NUM_CHNOUT] = {0};


#define MAX_ACT 0xffff
uint8_t s_last_phase = 255; // TODO reinit everything here when the model changes, no???

void evalMixes(uint8_t tick10ms)
{
#if defined(PCBGRUVIN9X) && defined(DEBUG) && !defined(VOICE)
  PORTH |= 0x40; // PORTH:6 LOW->HIGH signals start of mixer interrupt
#endif

  static uint16_t fp_act[MAX_FLIGHT_MODES] = {0};
  static uint16_t delta = 0;
  static ACTIVE_PHASES_TYPE s_fade_flight_phases = 0;

  LS_RECURSIVE_EVALUATION_RESET();
  
  uint8_t phase = getFlightPhase();

  if (s_last_phase != phase) {
    if (s_last_phase != 255) PLAY_PHASE_OFF(s_last_phase);
    PLAY_PHASE_ON(phase);

    if (s_last_phase == 255) {
      fp_act[phase] = MAX_ACT;
    }
    else {
      uint8_t fadeTime = max(g_model.phaseData[s_last_phase].fadeOut, g_model.phaseData[phase].fadeIn);
      ACTIVE_PHASES_TYPE transitionMask = ((ACTIVE_PHASES_TYPE)1 << s_last_phase) + ((ACTIVE_PHASES_TYPE)1 << phase);
      if (fadeTime) {
        s_fade_flight_phases |= transitionMask;
        delta = (MAX_ACT / (100/SLOW_STEP)) / fadeTime;
      }
      else {
        s_fade_flight_phases &= ~transitionMask;
        fp_act[s_last_phase] = 0;
        fp_act[phase] = MAX_ACT;
      }
#if defined(CPUARM)
      logicalSwitchesCopyState(s_last_phase, phase); // push last logical switches state from old to new phase
#endif
    }
    s_last_phase = phase;
  }

  int32_t weight = 0;
  if (s_fade_flight_phases) {
    memclear(sum_chans512, sizeof(sum_chans512));
    for (uint8_t p=0; p<MAX_FLIGHT_MODES; p++) {
      LS_RECURSIVE_EVALUATION_RESET();
      if (s_fade_flight_phases & ((ACTIVE_PHASES_TYPE)1 << p)) {
        s_current_mixer_flight_mode = p;
        evalFlightModeMixes(p==phase ? e_perout_mode_normal : e_perout_mode_inactive_phase, p==phase ? tick10ms : 0);
        for (uint8_t i=0; i<NUM_CHNOUT; i++)
          sum_chans512[i] += (chans[i] >> 4) * fp_act[p];
        weight += fp_act[p];
      }
      LS_RECURSIVE_EVALUATION_RESET();
    }
    assert(weight);
    s_current_mixer_flight_mode = phase;
  }
  else {
    s_current_mixer_flight_mode = phase;
    evalFlightModeMixes(e_perout_mode_normal, tick10ms);
  }

  //========== FUNCTIONS ===============
  // must be done after mixing because some functions use the inputs/channels values
  // must be done before limits because of the applyLimit function: it checks for safety switches which would be not initialized otherwise
  if (tick10ms) {
#if defined(CPUARM)
    requiredSpeakerVolume = g_eeGeneral.speakerVolume + VOLUME_LEVEL_DEF;
#endif
    evalFunctions();
  }

  //========== LIMITS ===============
  for (uint8_t i=0; i<NUM_CHNOUT; i++) {
    // chans[i] holds data from mixer.   chans[i] = v*weight => 1024*256
    // later we multiply by the limit (up to 100) and then we need to normalize
    // at the end chans[i] = chans[i]/256 =>  -1024..1024
    // interpolate value with min/max so we get smooth motion from center to stop
    // this limits based on v original values and min=-1024, max=1024  RESX=1024
    int32_t q = (s_fade_flight_phases ? (sum_chans512[i] / weight) << 4 : chans[i]);

#if defined(PCBSTD)
    ex_chans[i] = q >> 8;
#else
    ex_chans[i] = q / 256;
#endif

    int16_t value = applyLimits(i, q);  // applyLimits will remove the 256 100% basis

    cli();
    channelOutputs[i] = value;  // copy consistent word to int-level
    sei();
  }

  if (tick10ms && s_fade_flight_phases) {
    uint16_t tick_delta = delta * tick10ms;
    for (uint8_t p=0; p<MAX_FLIGHT_MODES; p++) {
      ACTIVE_PHASES_TYPE phaseMask = ((ACTIVE_PHASES_TYPE)1 << p);
      if (s_fade_flight_phases & phaseMask) {
        if (p == phase) {
          if (MAX_ACT - fp_act[p] > tick_delta)
            fp_act[p] += tick_delta;
          else {
            fp_act[p] = MAX_ACT;
            s_fade_flight_phases -= phaseMask;
          }
        }
        else {
          if (fp_act[p] > tick_delta)
            fp_act[p] -= tick_delta;
          else {
            fp_act[p] = 0;
            s_fade_flight_phases -= phaseMask;
          }
        }
      }
    }
  } //endif s_fade_fligh_phases

#if defined(PCBGRUVIN9X) && defined(DEBUG) && !defined(VOICE)
  PORTH &= ~0x40; // PORTH:6 HIGH->LOW signals end of mixer interrupt
#endif
}

#if !defined(CPUM64) && !defined(ACCURAT_THROTTLE_TIMER)
  //  code cost is about 16 bytes for higher throttle accuracy for timer
  //  would not be noticable anyway, because all version up to this change had only 16 steps;
  //  now it has already 32  steps; this define would increase to 128 steps
  #define ACCURAT_THROTTLE_TIMER
#endif


