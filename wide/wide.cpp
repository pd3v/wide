//
//  wide - Live coding DSLish API + MIDI sequencer
//
//  Created by @pd3v_
//

#include <iostream>
#include <vector>
#include <functional>
#include <deque>
#include <thread>
#include <future>
#include <chrono>
#include <mutex>
#include <algorithm>
#include "RtMidi.h"
#include "notes.hpp"
#include "taskpool.hpp"
#include "instrument.hpp"
#include "generator.hpp"
#include "expression.hpp"

#ifdef __linux__
  #pragma cling load("$LD_LIBRARY_PATH/librtmidi.dylib")
#elif __APPLE__
  #pragma cling load("$DYLD_LIBRARY_PATH/librtmidi.dylib")
#elif __unix__
  #pragma cling load("$LD_LIBRAsRY_PATH/librtmidi.dylib")
#endif

using namespace std;

using scaleType = const vector<int>;
using chordType = vector<int>;
using rhythmType = vector<int>;
using ampT = double;
using durT = rhythmType;
using label = int;

#define i(ch) (insts[(ch-1)])
#define isync(ch) (insts[ch-1].step)
#define ccsync(ch) (insts[ch-1].ccStep)
#define f(x) [&](){return x;}
#define n(c,a,d) [&]()->Notes{return (Notes){(vector<int> c),a,(vector<int> d),1};}     // note's absolute value setting, no octave parameter
#define no(c,a,d,o) [&]()->Notes{return (Notes){(vector<int> c),a,(vector<int> d),o};}  // note's setting with octave parameter
#define cc(ch,value) [&]()->CC{return (CC){ch,value};}
#define bar Metro::sync(Metro::metroPrecision)

const char* PROJ_NAME = "[w]AVES [i]N [d]ISTRESSED [en]TROPY";
const uint16_t NUM_TASKS = 5;
const float BAR_DUR_REF = 4000000; // microseconds
const uint8_t JOB_QUEUE_SIZE = 64;
const int CC_FREQ = 10; // Hz
constexpr uint8_t CC_RESOLUTION = 1000/CC_FREQ; //milliseconds
const float BPM_REF = 60;
const int REST_NOTE = 127;
const function<Notes()> SILENCE = []()->Notes {return {(vector<int>{}),0,{1},1};};
const vector<function<CC()>> NO_CTRL = {};

void pushSJob(vector<Instrument>& insts) {
  SJob j;
  int id = 0;
    
  while (TaskPool<SJob>::isRunning) {
    if (TaskPool<SJob>::jobs.size() < JOB_QUEUE_SIZE) {
      id = id%insts.size();
      j.id = id;
      j.job = &*insts.at(id).f;
      
      TaskPool<SJob>::jobs.push_back(j);
      
      id++;
    }
    this_thread::sleep_for(chrono::milliseconds(5));
  }
}

void pushCCJob(vector<Instrument>& insts) {
  CCJob j;
  int id = 0;
  
  while (TaskPool<CCJob>::isRunning) {
    if (TaskPool<CCJob>::jobs.size() < JOB_QUEUE_SIZE) {
      id = id%insts.size();
      j.id = id;
      j.job = &*insts.at(id).ccs;
      
      TaskPool<CCJob>::jobs.push_back(j);
      
      id++;
    }
    this_thread::sleep_for(chrono::milliseconds(5));
  }
}

// play function algorithm changes have immediate effect on every note parameter but duration
inline Notes checkPlayingFunctionChanges(function<Notes()>& newFunc, function<Notes()>& currentFunc) {
  Notes playNotes;
  
  if (newFunc) {
    if (playNotes.dur != Generator::midiNote(newFunc).dur)
      playNotes = Generator::midiNoteExcludeDur(currentFunc);
    else
      playNotes = Generator::midiNoteExcludeDur(newFunc);
  } else
      playNotes = Generator::midiNoteExcludeDur(currentFunc);
  
  return playNotes;
}

int taskDo(vector<Instrument>& insts) {
  RtMidiOut midiOut = RtMidiOut();
  std::unique_lock<mutex> lock(TaskPool<SJob>::mtx,std::defer_lock);
  long barStartTime, barElapsedTime, barDeltaTime, noteStartTime, noteElapsedTime, noteDeltaTime, loopIterTime = 0;
  long t = 0;
  double barDur;
  vector<unsigned char> noteMessage;
  
  SJob j;
  Notes playNotes;
  function<Notes()> playFunc;
  vector<int> durationsPattern;
  
  midiOut.openPort(0);
  noteMessage.push_back(0);
  noteMessage.push_back(0);
  noteMessage.push_back(0);

  while (TaskPool<SJob>::isRunning) {
    barStartTime = chrono::time_point_cast<chrono::microseconds>(chrono::steady_clock::now()).time_since_epoch().count();
    barDur = Generator::barDur();
    
    if (!TaskPool<SJob>::jobs.empty()) {
      TaskPool<SJob>::mtx.lock();
      j = TaskPool<SJob>::jobs.front();
      TaskPool<SJob>::jobs.pop_front();
      TaskPool<SJob>::mtx.unlock();
      
      if (j.job) {
        playFunc = *j.job;
        
        playNotes = Generator::midiNote(playFunc);
        durationsPattern = playNotes.dur;
        
        for (auto& dur : durationsPattern) {
          noteStartTime = chrono::time_point_cast<chrono::microseconds>(chrono::steady_clock::now()).time_since_epoch().count();
          for (auto& note : playNotes.notes) {
            noteMessage[0] = 144+j.id;
            noteMessage[1] = note;
            noteMessage[2] = (note != REST_NOTE && !insts[j.id].isMuted()) ? playNotes.amp : 0;
            midiOut.sendMessage(&noteMessage);
          }
          
          playNotes = Generator::midiNoteExcludeDur(playFunc);
          insts.at(j.id).step++;
          
          if (!TaskPool<SJob>::isRunning) goto finishTask;
          
          t = dur-round(dur/barDur*Metro::instsWaitingTimes.at(j.id)+loopIterTime);
          t = (t > 0 && t < barDur) ? t : dur; // prevent negative values for duration and thread overrun
          
          this_thread::sleep_for(chrono::microseconds(t));
          
          for (auto& note : playNotes.notes) {
            noteMessage[0] = 128+j.id;
            noteMessage[1] = note;
            noteMessage[2] = 0;
            midiOut.sendMessage(&noteMessage);
          }
          
          noteElapsedTime = chrono::time_point_cast<chrono::microseconds>(chrono::steady_clock::now()).time_since_epoch().count();
          noteDeltaTime = noteElapsedTime-noteStartTime;
          loopIterTime = noteDeltaTime > t ? noteDeltaTime-t : 0;
        }
      }
      Metro::syncInstTask(j.id);
    }
    
    barElapsedTime = chrono::time_point_cast<chrono::microseconds>(chrono::steady_clock::now()).time_since_epoch().count();
    barDeltaTime = barElapsedTime-barStartTime;
    
    Metro::instsWaitingTimes.at(j.id) = barDeltaTime > barDur ? barDeltaTime-barDur : 0;
  }
  
  finishTask:
  // silencing playing notes before task finishing
  for (auto& note : playNotes.notes) {
    noteMessage[0] = 128+j.id;
    noteMessage[1] = note;
    noteMessage[2] = 0;
    midiOut.sendMessage(&noteMessage);
  }
  return j.id;
}

int ccTaskDo(vector<Instrument>& insts) {
  RtMidiOut midiOut = RtMidiOut();
  std::unique_lock<mutex> lock(TaskPool<CCJob>::mtx,std::defer_lock);
  long startTime, elapsedTime;
  vector<unsigned char> ccMessage;
  CCJob j;
  std::vector<std::function<CC()>> ccs;
  vector<CC> ccComputed;

  midiOut.openPort(0);
  ccMessage.push_back(0);
  ccMessage.push_back(0);
  ccMessage.push_back(0);
  
  while (TaskPool<CCJob>::isRunning) {
    if (!TaskPool<CCJob>::jobs.empty()) {
      startTime = chrono::time_point_cast<chrono::milliseconds>(chrono::steady_clock::now()).time_since_epoch().count();
      
      TaskPool<CCJob>::mtx.lock();
      j = TaskPool<CCJob>::jobs.front();
      TaskPool<CCJob>::jobs.pop_front();
      TaskPool<CCJob>::mtx.unlock();
      
      if (j.job) {
        ccs = *j.job;
        ccComputed = Generator::midiCC(ccs);
    
        for (auto &cc : ccComputed) {
          ccMessage[0] = 176+j.id;
          ccMessage[1] = cc.ch;
          ccMessage[2] = cc.value;
          midiOut.sendMessage(&ccMessage);
        }
      
        insts.at(j.id).ccStep++;
      
        elapsedTime = chrono::time_point_cast<chrono::milliseconds>(chrono::steady_clock::now()).time_since_epoch().count();
        this_thread::sleep_for(chrono::milliseconds(CC_RESOLUTION-(elapsedTime-startTime)));
      }
    }
  }
  
  return j.id;
}

vector<Instrument> insts;

void bpm(int _bpm) {
  Generator::barDur(_bpm);
}

void bpm() {
  cout << floor(Generator::bpm) << " bpm" << endl;
}

uint32_t sync(int dur) {
  return Metro::sync(dur);
}

uint32_t playhead() {
  return Metro::playhead();
}

void mute() {
  for (auto &inst : insts)
    inst.mute();
}

void mute(int inst) {
  insts[inst-1].mute();
}

void solo(int inst) {
  insts[inst-1].unmute();
  
  for (auto &_inst : insts)
    if (_inst.id != inst-1)
      _inst.mute();
}

void unmute() {
  for (auto &inst : insts)
    inst.unmute();
}

void unmute(int inst) {
  insts[inst-1].unmute();
}

void noctrl() {
  for (auto &inst : insts)
    inst.noctrl();
}

void stop() {
  mute();
  noctrl();
}

void wide() {
  if (TaskPool<SJob>::isRunning) {
    std::thread([&](){
      TaskPool<SJob>::numTasks = NUM_TASKS;
      TaskPool<CCJob>::numTasks = NUM_TASKS;
      
      // init instruments
      for (int id = 0;id < TaskPool<SJob>::numTasks;++id)
        insts.push_back(Instrument(id));
      
      Metro::start();
      
      auto futPushSJob = async(launch::async,pushSJob,ref(insts));
      auto futPushCCJob = async(launch::async,pushCCJob,ref(insts));
      
      // init sonic task pool
      for (int i = 0;i < TaskPool<SJob>::numTasks;++i)
        TaskPool<SJob>::tasks.push_back(async(launch::async,taskDo,ref(insts)));
      
      // init cc task pool
      for (int i = 0;i < TaskPool<CCJob>::numTasks;++i)
        TaskPool<CCJob>::tasks.push_back(async(launch::async,ccTaskDo,ref(insts)));
    }).detach();
    
    cout << PROJ_NAME << " on <((()))>" << endl;
  }
}

void on(){
  if (!TaskPool<SJob>::isRunning) {
    bpm(60);
    
    for (auto &inst : insts) {
      inst.play(SILENCE);
      inst.noctrl();
    }

    TaskPool<SJob>::isRunning = true;
    TaskPool<CCJob>::isRunning = true;
    
    wide();
    
  } else {
      cout << PROJ_NAME << " : already : on <((()))>" << endl;
  }
}

void off() {
  Metro::stop();
  TaskPool<SJob>::stopRunning();
  TaskPool<CCJob>::stopRunning();
  Metro::stop();
  
  insts.clear();
  
  cout << PROJ_NAME << " off <>" << endl;
}

int main() {
  return 0;
}
