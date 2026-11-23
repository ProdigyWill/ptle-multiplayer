#pragma once

#include "Multiplayer.h"

#include "ptle/ERAnim.h"
#include "ptle/EAnimController.h"


// The puppets : the other players' Evil Harrys, spawned, animated and placed where their player is.


static const int MAX_LAYER_SCAN = 256;

static inline uint32_t GetLayerClipCRC( AnimLayer* layer ) { return layer->m_anim ? layer->m_anim->m_crc : 0; }

Matrix4f* GetPose( EAnimController* animCtrl, int* bones );
AnimLayer* GetAnimLayer( EICharacter* c, int key );
uint32_t GetPlayingClip( EICharacter* c, int* outLayer, float* outWeight );
int GetHeldItem( EIHarry* harry );

void ForgetPuppets();
void NoteLevelLeaving( uint32_t newLevel );
void CheckLevelLeaving( ERLevel* myWorld );
void ReplaceStalledPuppets();
void DropLostPuppets();

static const float MP_PI = 3.14159265F;
extern bool  g_autoLocomotion;   // F6 switches to hand-forced states.

// Harry's normal run measured about 0.36/tick (peaks near 0.44 on slopes).
static const float SPRINT_START_SPEED = 0.50F;

static const int MIRROR_LAYERS[2] = { 200, 201 };

// EInstance flags (offset 0x2C).
static const uint32_t INSTANCE_VISIBLE = 0x1;

void KeepUndrawnItem( EIHarry* harry );
void InstallItemDrawHooks();
void InstallPitHooks();
void InstallFadeHooks();
void InstallPuppetTickHook();


#ifdef PTLE_MP_EMBEDTESTS   // For the tests (tests.cpp).

extern bool g_allowPuppetFreeze;
extern volatile LONG g_puppetFreezesStopped;   // Puppets kept out of the ice cube (ThrownFromWater_Hook).
extern ItemStruct* ITEM_TABLE;

EItemBase* GetItemObject( int index );
int FindStateIndex( EIBeast* beast, const char* shortName );

extern int g_puppetsSpawned;   // For the autotest.
extern float g_forwardLocalAngle;

void SetIdleAnimation( EIEvilHarryP* p, const char* stateName, Peer* peer );
void SetAirAnimation( EIEvilHarryP* p, const char* stateName, Peer* peer );

extern bool g_puppetSolid;   // Autotest flips it to compare.
extern int g_puppetPoseCopies;   // For the autotest.
extern int g_puppetItemDraws;   // For the autotest.
extern int g_aimItemDraws;
extern int g_copyDrawsAlone;   // For the autotest : copies drawn while Harry draws no item.
extern volatile LONG g_fadesStopped;
extern float g_lastPuppetFade;   // What the game wanted for a puppet (tests).
extern volatile LONG g_puppetFadeCalls;
extern bool g_allowPuppetFade;

#endif
