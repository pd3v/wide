//
//  wide - Live coding DSLish API + MIDI sequencer
//
//  Created by @pd3v_
//

#pragma once

#include <vector>
#include <functional>
#include <math.h>
#include "notes.hpp"
#include "generator.hpp"

extern const function<Notes()> SILENCE;

using namespace std;

class Instrument {
public:
  Instrument(int id) {
    this->id = id;
    _ch = id;
    ccs = make_shared<vector<function<CC()>>>((vector<function<CC()>>){[&]()->CC{return (CC){127,0};}});
  }
  
  void play(function<Notes(void)> fn) {
    if (!Generator::parseDurPattern(fn).empty())
      *f = fn;
  }
  
  vector<int> outNotes() {
    return out.notes;
  }

  int outNotes(int notePos) {
    return out.notes.at(notePos);
  }
  
  double outAmp() {
    return out.amp;
  }
  
  vector<int> outDur() {
    return out.dur;
  }
  
  int outDur(int durPos) {
    return out.dur.at(durPos);
  }
  
  int outOct() {
    return out.oct;
  }
  
  void ctrl(){
    *ccs = _ccs;
    recurCCStart = true;
  }
  
  template <typename T,typename... U>
  void ctrl(T fn1,U... fn2) {
    if (recurCCStart) {
      _ccs.clear();
      recurCCStart = false;
    }
    
    _ccs.push_back(fn1);
    ctrl(fn2...);
  }
  
  void noctrl() {
    ccs->clear();
    _ccs.clear();
  }
  
  void mute() {
    _mute = true;
  }
  
  void unmute() {
    _mute = false;
  }
  
  bool isMuted() {
    return _mute;
  }
  
  int id;
  uint32_t step = 0, ccStep = 0;
  shared_ptr<function<Notes(void)>> const f = make_shared<function<Notes(void)>>(SILENCE); // 1/4 note silence
  shared_ptr<vector<function<CC()>>> ccs;
  Notes out = {{0},0.,{1},1};
  
private:
  int _ch;
  bool _mute = false;
  bool recurCCStart = true;
  vector<function<CC()>> _ccs{};
};


// Metro, an instrument wrapper struct with
// the specific job of being a metronome

struct Metro {
public:
  const static unsigned long metroPrecision = 64;
  static Instrument* inst;
  
  static void setInst(Instrument& _inst) {
    inst = &_inst;
    
    // Instrument working as a metronome and ticks 64 times per bar
    function<Notes()> beatFunc = [=]()->Notes {return {(vector<int>{0}),0,{metroPrecision},1};};
    inst->play(beatFunc);
  }
  
  //FIXME: this range conversion sometimes returns the same value for two contiguous steps
  static uint32_t sync(int timeSignature) {
    return floor(static_cast<float>(inst->step)*(timeSignature/64.0));
  }
  
  static uint32_t playhead() {
    return sync(4);
  }
};

Instrument* Metro::inst = nullptr;

