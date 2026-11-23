#pragma once

#include <stdint.h>

#include <Windows.h>


class EIHarry;
class EIEvilHarryP;
class EItemBase;
class Vector3f;

struct DIJOYSTATE2;
struct MenuInput;
struct Peer;
struct PlayerSnapshot;


// The autotests (Debug builds only, PTLE_MP_EMBEDTESTS : see Multiplayer.h). Their
// scripts are in tests.cpp. The mod calls the hooks below where the tests watch or
// steer the game; in Release builds they do nothing (and g_autotest is false).


#ifdef PTLE_MP_EMBEDTESTS

extern bool g_autotest;   // PTLE_MP_AUTOTEST=1 : a test is running.

void EnableTests();

// Harry's tick : before the game's (the crash test), after ours (the test script).
void TestBeforeHarryTick();
void TestAfterHarryTick( EIHarry* harry );

// The puppets.
bool TestSpawnsPuppet( const Peer* peer );          // The test spawns this one itself (not when its player arrives).
void TestMovePuppetTarget( Vector3f* position );    // Pinned where the test wants it, or beside Harry.
int  TestPuppetOutfit();                            // Every puppet wears this one (-1 : their own).
bool TestStallsPuppet( EIEvilHarryP* puppet );      // Its tick is skipped (as if a pitfall caught it).
bool TestSkipsPuppetAI();                           // No AI, no animation : the puppet is only placed.
bool TestHidesHarryItem( EItemBase* item );         // Harry's item isn't drawn (as with the camera in his head).
bool TestAnimSurveyOn();                            // The animation survey : the puppet plays its clip, no overlay.

// Network.
void TestCheckPacking( const PlayerSnapshot& sent );   // How much packing a snapshot loses.

// Input : in background tests, the keys and the controller come from the test.
bool TestInBackground();
bool TestPollKeyboard( void (*poll)(), uint8_t* keysNow, uint8_t* keysBefore );   // True : polled (the test's keys).
void TestSawKeys( const uint8_t* keys );
DIJOYSTATE2* TestPollGamepad( int index, DIJOYSTATE2* state );
void TestSawGamepad( const DIJOYSTATE2* state );
bool TestMenuInput( MenuInput* in, char* pasted, size_t size );   // True : the test has the mouse.

// Hide and seek.
void TestHsTick( EIHarry* harry );
void TestHsHeardCatch( int hider, int seeker );

#else

#	define g_autotest false

static inline void EnableTests() { }

static inline void TestBeforeHarryTick() { }
static inline void TestAfterHarryTick( EIHarry* ) { }

static inline bool TestSpawnsPuppet( const Peer* ) { return false; }
static inline void TestMovePuppetTarget( Vector3f* ) { }
static inline int  TestPuppetOutfit() { return -1; }
static inline bool TestStallsPuppet( EIEvilHarryP* ) { return false; }
static inline bool TestSkipsPuppetAI() { return false; }
static inline bool TestHidesHarryItem( EItemBase* ) { return false; }
static inline bool TestAnimSurveyOn() { return false; }

static inline void TestCheckPacking( const PlayerSnapshot& ) { }

static inline bool TestInBackground() { return false; }
static inline bool TestPollKeyboard( void (*)(), uint8_t*, uint8_t* ) { return false; }
static inline void TestSawKeys( const uint8_t* ) { }
static inline DIJOYSTATE2* TestPollGamepad( int, DIJOYSTATE2* state ) { return state; }
static inline void TestSawGamepad( const DIJOYSTATE2* ) { }
static inline bool TestMenuInput( MenuInput*, char*, size_t ) { return false; }

static inline void TestHsTick( EIHarry* ) { }
static inline void TestHsHeardCatch( int, int ) { }

#endif
