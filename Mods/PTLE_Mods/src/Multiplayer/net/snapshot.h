#pragma once

// One player's state at one game tick : what the puppet of that player replays.

#include <stdint.h>

#include "ptle/math/Matrix4f.h"
#include "ptle/math/Vector3f.h"


static const int MAX_CHAR_BONES = 80;   // Harry's skeleton has 76 bones.
static const int MAX_ITEM_BONES = 64;

struct PlayerSnapshot
{
	uint32_t time;                               // Sender's clock (ms) when it was recorded.
	uint32_t level;                              // CRC of the level the player is in.
	Matrix4f transform;
	Vector3f position;
	Vector3f rotationEuler;
	bool onGround;                               // EICharacter::m_onGround (CheatLab writes it too).
	float lift;                                  // Height above where he last stood (jumps, climbs), never below 0.
	uint32_t clipCRC;                            // Animation clip the player is playing (0 = unknown).
	float clipWeight;
	int heldItem;                                // Item table index of the item in Harry's hand, or -1.
	int bones;                                   // Harry's pose : his bones relative to his transform (0 : unknown).
	Matrix4f pose[MAX_CHAR_BONES];
	Matrix4f handR, handL;                       // His hand matrices, relative to his transform.
	int drawnSlot;                               // DRAWN_ITEMS index of the item Harry drew this frame, or -1.
	int itemState;                               // Its state (+0x70) when it drew : some draw only in some states.
	Matrix4f itemLocal;                          // Its matrix, relative to his transform.
	int itemBones;                               // Animated items : their bones, relative to his transform (0 : none).
	Matrix4f itemPose[MAX_ITEM_BONES];
};
