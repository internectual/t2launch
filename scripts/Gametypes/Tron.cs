// Tron: server-side light-cycle ruleset for the MatrixTron mission.
// Evaluated in memory by the t2launch hook (server.gametype_script); stock clients download nothing.
// The package is active only while $CurrentMission is MatrixTron.
//
// Control model: the client's control object is its Camera, locked overhead. Fly-mode input moves the camera
// a little each tick; the server reads that offset as a compass heading, snaps the camera back, and moves a
// separate Player avatar and its trail.

$Tron::Mission = "MatrixTron";
$Tron::Half = 36;                    // arena half-size in metres
$Tron::Cell = 3;                     // trail cell size
$Tron::Speed = 10;                   // metres/second
$Tron::GroundZ = 300;
$Tron::CamZ = 390;
$Tron::CamPitch = 1.4;               // the engine clamps camera pitch to ~80 degrees, so "straight down" is 1.4 rad
$Tron::CamRot = "1 0 0 " @ $Tron::CamPitch;   // positive X angle pitches down in this engine
$Tron::CamY = -($Tron::CamZ - $Tron::GroundZ) / mTan($Tron::CamPitch);   // offset so the view centres on the arena
$Tron::InputMin = 0.3;
$Tron::TurnStrength = 2.0;            // any real camera step (a step is 2.5+) turns at once; smaller noise must repeat
$Tron::WantWindowMs = 250;           // a repeat counts only if it follows within this long
$Tron::TickMs = 32;
$Tron::Drift = 25;                    // re-anchor the camera when it has wandered this far
$Tron::WallHeight = 4;               // trail wall blocks fill one collision cell exactly
$Tron::HeadSize = 2;                 // the rider's head block (the Player model is tiny from ~90 m up)
$Tron::HeadHeight = 7;
$Tron::BorderHeight = 8;
$Tron::Palette[0] = "0.00 0.55 0.99";    // team colours, wrapping every 8 teams
$Tron::Palette[1] = "1.00 0.15 0.10";
$Tron::Palette[2] = "0.28 0.89 0.31";
$Tron::Palette[3] = "1.00 0.90 0.10";
$Tron::Palette[4] = "1.00 0.45 0.00";
$Tron::Palette[5] = "0.85 0.20 0.95";
$Tron::Palette[6] = "0.10 0.95 0.95";
$Tron::Palette[7] = "0.95 0.95 0.95";
$Tron::Debug = 0;
$Tron::nSeats = 0;
$Tron::Dirs[0] = "0 1";  $Tron::Yaw[0] = "0";
$Tron::Dirs[1] = "1 0";  $Tron::Yaw[1] = "-1.5708";
$Tron::Dirs[2] = "0 -1"; $Tron::Yaw[2] = "3.1416";
$Tron::Dirs[3] = "-1 0"; $Tron::Yaw[3] = "1.5708";

// Force-field datablocks must exist before clients connect, and DestroyServer deletes them, so they are
// defined at install and again after every CreateServer.
function tronDatablocks()
{
   if (isObject(TronField0))
      return;
   for (%i = 0; %i < 8; %i++)
      eval("datablock ForceFieldBareData(TronField" @ %i @ ") : defaultForceFieldBare {"
         @ " color = \"" @ $Tron::Palette[%i] @ "\"; baseTranslucency = 0.45; needsNoPower = true; };");
   eval("datablock ForceFieldBareData(TronBorder) : defaultForceFieldBare {"
      @ " color = \"0.9 0.9 0.9\"; baseTranslucency = 0.25; needsNoPower = true; };");
}

function tronFieldBlock(%client)
{
   return "TronField" @ ((%client.team + 7) % 8);
}

function tronField(%datablock, %x, %y, %sx, %sy, %sz)
{
   %f = new ForceFieldBare() {
      dataBlock = %datablock;
      position = %x SPC %y SPC $Tron::GroundZ;
      rotation = "1 0 0 0";
      scale = %sx SPC %sy SPC %sz;
   };
   return %f;
}

function tronDump()
{
   for (%i = 0; %i < ClientGroup.getCount(); %i++)
   {
      %c = ClientGroup.getObject(%i);
      %pl = isObject(%c.player) ? %c.player : 0;
      echo("TT> client=" @ %c @ " ctl=" @ %c.getControlObject() @ " cam=" @ %c.camera @ " pl=" @ %pl
         @ " alive=" @ %c.trAlive @ " heading=" @ %c.trHeading @ " pos=" @ %c.trX SPC %c.trY
         @ " pltr=" @ (%pl ? %pl.getTransform() : "none") @ " camtr=" @ %c.camera.getTransform()
         @ " trail=" @ (isObject($Tron::Trail) ? $Tron::Trail.getCount() : -1));
   }
}

function tronCell(%v)
{
   return mFloor(%v / $Tron::Cell);
}

function tronClearTrails()
{
   for (%i = 0; %i < $Tron::nKeys; %i++)
      $Tron::Occ[$Tron::Key[%i]] = "";
   $Tron::nKeys = 0;
   if (isObject($Tron::Trail))
      $Tron::Trail.delete();
   // each force field adds a PhysicalZone in MissionCleanup/PZones; drop them with the walls
   %zones = nameToID("MissionCleanup/PZones");
   if (%zones > 0)
      %zones.delete();
   $Tron::Trail = new SimGroup();
   if (isObject(MissionCleanup))
      MissionCleanup.add($Tron::Trail);
}

function tronSeat(%n)
{
   // four seats facing inward: N-facing at south, S-facing at north, ...
   %h = $Tron::Half - 4;
   switch (%n % 4)
   {
      case 0: return (-%h/2) SPC (-%h) SPC 0;
      case 1: return (%h/2) SPC %h SPC 2;
      case 2: return (-%h) SPC (%h/2) SPC 1;
      case 3: return %h SPC (-%h/2) SPC 3;
   }
}

function tronInitRider(%client)
{
   %seat = tronSeat(%client.trSeat);
   %client.trX = getWord(%seat, 0);
   %client.trY = getWord(%seat, 1);
   %client.trHeading = getWord(%seat, 2);
   %client.trAlive = true;
   %client.trPrevA = "";
   %client.trPrevB = "";
   %client.trCell = "";
   %client.trAnchor = "0" SPC $Tron::CamY SPC $Tron::CamZ SPC $Tron::CamRot;
   // the avatar
   %pl = %client.player;
   if (isObject(%pl))
   {
      %pl.setTransform(%client.trX SPC %client.trY SPC $Tron::GroundZ SPC "0 0 1" SPC $Tron::Yaw[%client.trHeading]);
      %pl.setVelocity("0 0 0");
      tronMakeHead(%client);
   }
   // the locked overhead camera
   %cam = %client.camera;
   %cam.setFlyMode();
   %cam.setTransform(%client.trAnchor);
   %client.setControlObject(%cam);
   %client.trLastCam = "";
   commandToClient(%client, 'setHudMode', 'Observer');
   // the stock join code hands control to the player right after spawning; take it back
   %client.schedule(100, "setControlObject", %cam);
}

function tronMakeHead(%client)
{
   if (isObject(%client.trHead))
      %client.trHead.delete();
   %half = $Tron::HeadSize / 2;
   %head = tronField(tronFieldBlock(%client), %client.trX - %half, %client.trY - %half,
      $Tron::HeadSize, $Tron::HeadSize, $Tron::HeadHeight);
   if (isObject(MissionCleanup))
      MissionCleanup.add(%head);
   %client.trHead = %head;
}

function tronMakeBorder()
{
   if (!isObject(MissionCleanup))
      return;
   if (isObject($Tron::Border) && isObject($Tron::Border.getObject(0)))
      return;
   if (isObject($Tron::Border))
      $Tron::Border.delete();
   $Tron::Border = new SimGroup();
   if (isObject(MissionCleanup))
      MissionCleanup.add($Tron::Border);
   %h = $Tron::Half;
   %len = %h * 2 + 0.5;
   $Tron::Border.add(tronField(TronBorder, -%h - 0.25, %h, %len, 0.5, $Tron::BorderHeight));
   $Tron::Border.add(tronField(TronBorder, -%h - 0.25, -%h - 0.5, %len, 0.5, $Tron::BorderHeight));
   $Tron::Border.add(tronField(TronBorder, %h, -%h - 0.25, 0.5, %len, $Tron::BorderHeight));
   $Tron::Border.add(tronField(TronBorder, -%h - 0.5, -%h - 0.25, 0.5, %len, $Tron::BorderHeight));
}

function tronKillRider(%client, %why)
{
   if (!%client.trAlive)
      return;
   echo("TRON kill " @ %client.nameBase @ " why=" @ %why @ " pos=" @ %client.trX SPC %client.trY @ " heading=" @ %client.trHeading @ " cell=" @ %client.trCell);
   %client.trAlive = false;
   if (isObject(%client.trHead))
      %client.trHead.delete();
   %pl = %client.player;
   if (isObject(%pl))
      %pl.scriptKill($DamageType::Explosion);
   messageAll('MsgTronDown', '\c1%1 derezzed.', %client.name);
   tronCheckRound();
}

function tronAliveCount()
{
   %n = 0;
   for (%i = 0; %i < ClientGroup.getCount(); %i++)
   {
      %cl = ClientGroup.getObject(%i);
      if (%cl.team > 0 && %cl.trAlive)
         %n++;
   }
   return %n;
}

function tronJoinedCount()
{
   %n = 0;
   for (%i = 0; %i < ClientGroup.getCount(); %i++)
      if (ClientGroup.getObject(%i).team > 0)
         %n++;
   return %n;
}

function tronCheckRound()
{
   if ($Tron::RoundEnding)
      return;
   %alive = tronAliveCount();
   %joined = tronJoinedCount();
   if (%joined > 1 && %alive > 1)
      return;
   if (%joined <= 1 && %alive >= 1)
      return;
   $Tron::RoundEnding = true;
   if (%alive == 1)
   {
      for (%i = 0; %i < ClientGroup.getCount(); %i++)
      {
         %cl = ClientGroup.getObject(%i);
         if (%cl.team > 0 && %cl.trAlive)
         {
            %cl.score++;
            Game.recalcScore(%cl);
            messageAll('MsgTronWin', '\c2%1 wins the round.', %cl.name);
         }
      }
   }
   schedule(4000, 0, tronStartRound);
}

function tronStartRound()
{
   $Tron::RoundEnding = false;
   tronClearTrails();
   tronMakeBorder();
   %seat = 0;
   for (%i = 0; %i < ClientGroup.getCount(); %i++)
   {
      %cl = ClientGroup.getObject(%i);
      if (%cl.team <= 0)
         continue;
      if (isObject(%cl.player))
         %cl.player.delete();
      %cl.trSeat = %seat++;
      %cl.trAlive = false;
      Game.spawnPlayer(%cl, false);   // createPlayer override calls tronInitRider
   }
}

function tronTick()
{
   cancel($Tron::TickThread);
   $Tron::TickThread = schedule($Tron::TickMs, 0, tronTick);
   if (!isObject(MissionCleanup))
      return;
   if (!isObject($Tron::Trail))
      tronClearTrails();
   tronMakeBorder();
   %now = getSimTime();
   %dt = $Tron::LastTick $= "" ? 0.032 : (%now - $Tron::LastTick) / 1000;
   $Tron::LastTick = %now;
   if (%dt > 0.2) %dt = 0.2;
   for (%i = 0; %i < ClientGroup.getCount(); %i++)
   {
      %cl = ClientGroup.getObject(%i);
      if (!%cl.trAlive || !isObject(%cl.player) || !isObject(%cl.camera))
         continue;
      %cam = %cl.camera;
      if (%cl.getControlObject() != %cam)
      {
         %cl.setControlObject(%cam);
         %cam.setTransform(%cl.trAnchor);
         continue;
      }
      // Read the key input as how far the camera moved since the last tick. Only its rotation is snapped back
      // each tick (so the mouse cannot turn it); the position is left alone, and re-anchored when it drifts.
      %t = %cam.getTransform();
      %px = getWord(%t, 0);
      %py = getWord(%t, 1);
      %pz = getWord(%t, 2);
      if (%cl.trLastCam $= "")
         %cl.trLastCam = %px SPC %py SPC %pz;
      %dx = %px - getWord(%cl.trLastCam, 0);
      %dy = %py - getWord(%cl.trLastCam, 1);
      %dz = %pz - getWord(%cl.trLastCam, 2);
      %fwd = -%dz;                 // looking down, fly-mode forward moves the camera along -Z
      %side = %dx;
      if ($Tron::Debug && (mAbs(%fwd) > 0.01 || mAbs(%side) > 0.01))
         echo("TRON input t=" @ %now @ " dt=" @ %dt @ " d=" @ %dx SPC %dy SPC %dz @ " head=" @ %cl.trHeading);
      // Turn on a full-step offset at once; a weaker, lone sample (the camera sometimes sends a stray half
      // step mid-hold) must repeat on the next input sample before it counts.
      %want = "";
      if (mAbs(%fwd) >= mAbs(%side) && mAbs(%fwd) > $Tron::InputMin)
         %want = %fwd > 0 ? 0 : 2;
      else if (mAbs(%side) > $Tron::InputMin)
         %want = %side > 0 ? 1 : 3;
      if (%want !$= "")
      {
         %strength = mAbs(%fwd) > mAbs(%side) ? mAbs(%fwd) : mAbs(%side);
         if (%now - %cl.trWantTime > $Tron::WantWindowMs)
            %cl.trWantCount = 0;
         if (%want == %cl.trWant)
            %cl.trWantCount++;
         else
         {
            %cl.trWant = %want;
            %cl.trWantCount = 1;
         }
         %cl.trWantTime = %now;
         if ((%strength >= $Tron::TurnStrength || %cl.trWantCount >= 2)
            && %want != %cl.trHeading && ((%want + 2) % 4) != %cl.trHeading)
            %cl.trHeading = %want;
      }
      if (mAbs(%px) > $Tron::Drift || mAbs(%py - $Tron::CamY) > $Tron::Drift || mAbs(%pz - $Tron::CamZ) > $Tron::Drift)
      {
         %cam.setTransform(%cl.trAnchor);
         %cl.trLastCam = getWords(%cl.trAnchor, 0, 2);
      }
      else
      {
         %cam.setTransform(%px SPC %py SPC %pz SPC $Tron::CamRot);
         %cl.trLastCam = %px SPC %py SPC %pz;
      }
      // advance
      %d = $Tron::Dirs[%cl.trHeading];
      %cl.trX += getWord(%d, 0) * $Tron::Speed * %dt;
      %cl.trY += getWord(%d, 1) * $Tron::Speed * %dt;
      %cl.player.setTransform(%cl.trX SPC %cl.trY SPC $Tron::GroundZ SPC "0 0 1" SPC $Tron::Yaw[%cl.trHeading]);
      %cl.player.setVelocity("0 0 0");
      if (isObject(%cl.trHead))
         %cl.trHead.setTransform((%cl.trX - $Tron::HeadSize / 2) SPC (%cl.trY - $Tron::HeadSize / 2) SPC $Tron::GroundZ);
      if (mAbs(%cl.trX) > $Tron::Half || mAbs(%cl.trY) > $Tron::Half)
      {
         tronKillRider(%cl, "bounds");
         continue;
      }
      %cx = tronCell(%cl.trX);
      %cy = tronCell(%cl.trY);
      %key = %cx @ "_" @ %cy;
      if (%key $= %cl.trCell)
         continue;
      if ($Tron::Occ[%key] !$= "" && %key !$= %cl.trPrevA && %key !$= %cl.trPrevB)
      {
         tronKillRider(%cl, "trail " @ $Tron::Occ[%key]);
         continue;
      }
      // lay a trail piece in the cell just left
      if (%cl.trCell !$= "")
      {
         %px = getWord(strreplace(%cl.trCell, "_", " "), 0) * $Tron::Cell;
         %py = getWord(strreplace(%cl.trCell, "_", " "), 1) * $Tron::Cell;
         %piece = tronField(tronFieldBlock(%cl), %px, %py, $Tron::Cell, $Tron::Cell, $Tron::WallHeight);
         $Tron::Trail.add(%piece);
      }
      $Tron::Occ[%key] = %cl;
      $Tron::Key[$Tron::nKeys] = %key;
      $Tron::nKeys++;
      %cl.trPrevB = %cl.trPrevA;
      %cl.trPrevA = %cl.trCell;
      %cl.trCell = %key;
   }
}

// Switch the package on and off with the mission, so stock DM missions are untouched.
function tronWatch()
{
   cancel($Tron::WatchThread);
   $Tron::WatchThread = schedule(250, 0, tronWatch);
   %on = ($CurrentMission $= $Tron::Mission);
   if (%on && !$Tron::PkgOn)
   {
      activatePackage(T2LaunchTron);
      $Tron::PkgOn = true;
      $Tron::LastTick = "";
      $Tron::RoundEnding = false;
      tronTick();
      echo("Tron ruleset on");
   }
   else if (!%on && $Tron::PkgOn)
   {
      deactivatePackage(T2LaunchTron);
      $Tron::PkgOn = false;
      cancel($Tron::TickThread);
      echo("Tron ruleset off");
   }
}

package T2LaunchTronBoot
{
   function CreateServer(%mission, %missionType)
   {
      Parent::CreateServer(%mission, %missionType);
      tronDatablocks();
   }
};

package T2LaunchTron
{
   function DMGame::equip(%game, %player)
   {
      // light cycles carry nothing
   }

   function DMGame::createPlayer(%game, %client, %spawnLoc, %respawn)
   {
      Parent::createPlayer(%game, %client, %spawnLoc, %respawn);
      if (%client.trSeat $= "")
      {
         %client.trSeat = $Tron::nSeats + 0;
         $Tron::nSeats++;
      }
      tronInitRider(%client);
   }

   function DMGame::onClientKilled(%game, %clVictim, %clKiller, %damageType, %implement, %damageLoc)
   {
      // no stock respawn timer or orbit camera: the camera stays locked overhead
      %clVictim.deaths++;
      %clVictim.trAlive = false;
      if (isObject(%clVictim.player))
         %clVictim.player.schedule(2500, "delete");
      tronCheckRound();
   }

   function DMGame::ObserverOnTrigger(%game, %data, %obj, %trigger, %state)
   {
      %client = %obj.getControllingClient();
      // a rider (alive or waiting for the next round) must not leave the locked camera
      if (%client && %client.team > 0)
         return false;
      return Parent::ObserverOnTrigger(%game, %data, %obj, %trigger, %state);
   }

   function DMGame::clientJoinTeam(%game, %client, %team, %respawn)
   {
      Parent::clientJoinTeam(%game, %client, %team, %respawn);
   }

   function DMGame::onClientLeaveGame(%game, %client)
   {
      %client.trAlive = false;
      if (isObject(%client.trHead))
         %client.trHead.delete();
      Parent::onClientLeaveGame(%game, %client);
      tronCheckRound();
   }
};

if (!$Tron::Installed)
{
   $Tron::Installed = true;
   activatePackage(T2LaunchTronBoot);
   tronDatablocks();
   tronWatch();
   echo("Tron gametype script loaded");
}
