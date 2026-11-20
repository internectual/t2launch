// Retro: shared input and view helpers for the server-side gametypes (load it first in server.gametype_script).
//
// The gametypes need the player's keys as a d-pad and a view that stays put. They used to read the fly camera,
// but a camera moves at the CLIENT's own speed (stock 40 m/s, which the server cannot change), so the view slid and
// jittered with every key press. A Player is different: its speeds come from its datablock, which the client
// receives, so client and server agree exactly. Here each seated player is patched to creep at a few tenths of a
// metre per second with its pitch locked, stands on an invisible solid floor in front of its board and is hidden:
// WASD becomes a tiny velocity that is read and reset every tick, and the view moves by centimetres at most. The
// mouse can still turn the view until the server turns it back.
//
// Triggers (fire, alt-fire, jump, jet, throw) reach the games through Armor::onTrigger, with presses and releases.
// Keys come back as a mask: 1 forward, 2 back, 4 strafe left, 8 strafe right.

$Retro::Speed = 0.6;                 // the patched armors' top speed in every direction, metres/second
$Retro::Eye = 2.12;                  // eye height above the feet (measured on the light armor)
$Retro::VelMin = 0.12;              // velocity that counts as a held key
$Retro::HoldEps = 0.25;             // put the player back when it has crept this far
$Retro::YawEps = 0.02;               // radians of turn before the view is put back
$Retro::FloorSize = 8;
$Retro::Debug = 0;
// A second PlayerData datablock makes the server drop every client about twelve seconds after it joins, so the
// stock light armors are changed in place instead; player.cs recreates them from scratch on the next CreateServer,
// which undoes this.
$Retro::Armors = "LightMaleHumanArmor LightFemaleHumanArmor LightMaleBiodermArmor";

// datablocks are deleted by DestroyServer, so this runs at install and again after every CreateServer
function retroDatablocks()
{
   if (!isObject(RetroFloor))
      eval("datablock ForceFieldBareData(RetroFloor) : defaultSolidFieldBare { framesPerSec = 1; numFrames = 1; scrollSpeed = 0;"
         @ " baseTranslucency = 0.01; needsNoPower = true; };");
   if ($Retro::Patched)
      return;
   %s = $Retro::Speed;
   for (%i = 0; %i < getWordCount($Retro::Armors); %i++)
   {
      %a = getWord($Retro::Armors, %i);
      if (!isObject(%a))
         continue;
      %a.maxForwardSpeed = %s;
      %a.maxBackwardSpeed = %s;
      %a.maxSideSpeed = %s;
      %a.maxUnderwaterForwardSpeed = %s;
      %a.maxUnderwaterBackwardSpeed = %s;
      %a.maxUnderwaterSideSpeed = %s;
      %a.jumpForce = 0;
      %a.jetForce = 0;
      %a.underwaterJetForce = 0;
      %a.minJumpEnergy = 99999;
      %a.minJetEnergy = 99999;
      %a.minLookAngle = 0;
      %a.maxLookAngle = 0;
      %a.maxFreelookAngle = 0;
      %a.minImpactSpeed = 99999;
   }
   $Retro::Patched = true;
}

// Put a client's player at (%x, %y) with its eye at height %zEye, facing +Y, and give it control. The player must
// already exist (call this after the stock createPlayer). Two players must not share a spot: give them different %y.
function retroAttach(%cl, %x, %y, %zEye)
{
   %pl = %cl.player;
   if (!isObject(%pl))
      return false;
   retroDatablocks();
   %zFeet = %zEye - $Retro::Eye;
   if (isObject(%cl.retroFloor))
      %cl.retroFloor.delete();
   %half = $Retro::FloorSize / 2;
   %cl.retroFloor = new ForceFieldBare() {
      dataBlock = RetroFloor;
      position = (%x - %half) SPC (%y - %half) SPC (%zFeet - 1);
      rotation = "1 0 0 0";
      scale = $Retro::FloorSize SPC $Retro::FloorSize SPC 1;
   };
   MissionCleanup.add(%cl.retroFloor);
   %cl.retroAnchor = %x SPC %y SPC (%zFeet + 0.05);
   %pl.setTransform(%cl.retroAnchor SPC "0 0 1 0");
   %pl.setVelocity("0 0 0");
   %pl.startFade(0, 0, true);               // the body is not meant to be seen by anyone
   %cl.setControlObject(%pl);
   %cl.retroEyeLogged = false;
   return true;
}

function retroDetach(%cl)
{
   if (isObject(%cl.retroFloor))
      %cl.retroFloor.delete();
   %cl.retroFloor = "";
   %cl.retroAnchor = "";
}

// Direction keys held this tick (1 forward, 2 back, 4 left, 8 right). Also keeps the player still and facing the
// board, so call it once per tick for a seated client.
function retroPad(%cl)
{
   %pl = %cl.player;
   if (!isObject(%pl) || %cl.retroAnchor $= "")
      return 0;
   if (%cl.getControlObject() != %pl)
      %cl.setControlObject(%pl);
   %v = %pl.getVelocity();
   %vx = getWord(%v, 0);
   %vy = getWord(%v, 1);
   %mask = 0;
   if (%vy > $Retro::VelMin) %mask = %mask | 1;
   if (%vy < -$Retro::VelMin) %mask = %mask | 2;
   if (%vx < -$Retro::VelMin) %mask = %mask | 4;
   if (%vx > $Retro::VelMin) %mask = %mask | 8;
   %pl.setVelocity("0 0 0");
   // put the player back if it has crept off its spot or the mouse has turned it
   %t = %pl.getTransform();
   %dx = getWord(%t, 0) - getWord(%cl.retroAnchor, 0);
   %dy = getWord(%t, 1) - getWord(%cl.retroAnchor, 1);
   %dz = getWord(%t, 2) - getWord(%cl.retroAnchor, 2);
   if (mAbs(getWord(%t, 6)) > $Retro::YawEps || mAbs(%dx) > $Retro::HoldEps || mAbs(%dy) > $Retro::HoldEps || mAbs(%dz) > 0.6)
      %pl.setTransform(%cl.retroAnchor SPC "0 0 1 0");
   if ($Retro::Debug && !%cl.retroEyeLogged)
   {
      %cl.retroEyeLogged = true;
      echo("RETRO eye=" @ %pl.getEyeTransform() @ " feet=" @ %t @ " anchor=" @ %cl.retroAnchor);
   }
   return %mask;
}

// the keys that went down since the last call (for games that turn on a key press): remembers the last mask
function retroPressed(%cl, %mask)
{
   %new = %mask & ~%cl.retroLastMask;
   %cl.retroLastMask = %mask;
   return %new;
}

// A gametype that defines SHAPE datablocks must call this after it has defined them (and again after every CreateServer
// that redefines them). The dedicated server's anti-cheat challenge compares a client's reply with values it computed once
// at startup over the stock shapes; without a refresh, a client that has received the new shapes answers differently and is
// dropped 12 to 25 seconds after joining. Official servers never add datablocks late, so they never meet this. The hook
// recomputes the stored values with the engine's own routine (t2launch_refreshCrcChallenge, dedicated server only).
function retroRefreshCrc()
{
   t2launch_refreshCrcChallenge();
}

package T2LaunchRetroBoot
{
   function CreateServer(%mission, %missionType)
   {
      Parent::CreateServer(%mission, %missionType);
      $Retro::Patched = false;
      retroDatablocks();
   }
};

if (!$Retro::Installed)
{
   $Retro::Installed = true;
   activatePackage(T2LaunchRetroBoot);
   retroDatablocks();
   echo("Retro helpers loaded");
}
