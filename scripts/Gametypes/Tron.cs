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
$Tron::InputMin = 0.3;               // camera offset per tick that counts as a key press
$Tron::TickMs = 32;
$Tron::HeadScale = 3;                // the rider head marker (the Player model is tiny from ~90 m up)
$Tron::PieceScale = 1.2;
$Tron::PostScale = 0.8;              // arena border posts
$Tron::PostSpacing = 6;
$Tron::Debug = 0;
$Tron::nSeats = 0;
$Tron::Dirs[0] = "0 1";  $Tron::Yaw[0] = "0";
$Tron::Dirs[1] = "1 0";  $Tron::Yaw[1] = "-1.5708";
$Tron::Dirs[2] = "0 -1"; $Tron::Yaw[2] = "3.1416";
$Tron::Dirs[3] = "-1 0"; $Tron::Yaw[3] = "1.5708";

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
   commandToClient(%client, 'setHudMode', 'Observer');
   // the stock join code hands control to the player right after spawning; take it back
   %client.schedule(100, "setControlObject", %cam);
}

function tronMakeHead(%client)
{
   if (isObject(%client.trHead))
      %client.trHead.delete();
   %head = new StaticShape() { dataBlock = "ExteriorFlagStand"; };
   %head.setTransform(%client.trX SPC %client.trY SPC $Tron::GroundZ);
   %head.setScale($Tron::HeadScale SPC $Tron::HeadScale SPC $Tron::HeadScale);
   if (isObject(MissionCleanup))
      MissionCleanup.add(%head);
   %client.trHead = %head;
}

function tronMakeBorder()
{
   if (isObject($Tron::Border))
      return;
   $Tron::Border = new SimGroup();
   if (isObject(MissionCleanup))
      MissionCleanup.add($Tron::Border);
   %e = $Tron::Half + 1.5;
   for (%v = -$Tron::Half; %v <= $Tron::Half; %v += $Tron::PostSpacing)
   {
      for (%k = 0; %k < 4; %k++)
      {
         switch (%k)
         {
            case 0: %x = %v; %y = %e;
            case 1: %x = %v; %y = -%e;
            case 2: %x = %e; %y = %v;
            case 3: %x = -%e; %y = %v;
         }
         %post = new StaticShape() { dataBlock = "ExteriorFlagStand"; };
         %post.setTransform(%x SPC %y SPC $Tron::GroundZ);
         %post.setScale($Tron::PostScale SPC $Tron::PostScale SPC $Tron::PostScale);
         $Tron::Border.add(%post);
      }
   }
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
      // read the key input as the camera's offset from its anchor, then snap back
      %t = %cam.getTransform();
      %dx = getWord(%t, 0);
      %dy = getWord(%t, 1);
      %dz = getWord(%t, 2) - $Tron::CamZ;
      %fwd = -%dz;                 // looking down, fly-mode forward moves the camera along -Z
      %side = %dx;
      if ($Tron::Debug && (mAbs(%fwd) > 0.01 || mAbs(%side) > 0.01))
         echo("TRON input " @ %cl.nameBase @ " d=" @ %dx SPC %dy SPC %dz);
      %want = %cl.trHeading;
      if (mAbs(%fwd) >= mAbs(%side) && mAbs(%fwd) > $Tron::InputMin)
         %want = %fwd > 0 ? 0 : 2;
      else if (mAbs(%side) > $Tron::InputMin)
         %want = %side > 0 ? 1 : 3;
      if (%want != %cl.trHeading && ((%want + 2) % 4) != %cl.trHeading)
         %cl.trHeading = %want;
      %cam.setTransform(%cl.trAnchor);
      // advance
      %d = $Tron::Dirs[%cl.trHeading];
      %cl.trX += getWord(%d, 0) * $Tron::Speed * %dt;
      %cl.trY += getWord(%d, 1) * $Tron::Speed * %dt;
      %cl.player.setTransform(%cl.trX SPC %cl.trY SPC $Tron::GroundZ SPC "0 0 1" SPC $Tron::Yaw[%cl.trHeading]);
      %cl.player.setVelocity("0 0 0");
      if (isObject(%cl.trHead))
         %cl.trHead.setTransform(%cl.trX SPC %cl.trY SPC $Tron::GroundZ SPC "0 0 1" SPC $Tron::Yaw[%cl.trHeading]);
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
         %px = (getWord(strreplace(%cl.trCell, "_", " "), 0) + 0.5) * $Tron::Cell;
         %py = (getWord(strreplace(%cl.trCell, "_", " "), 1) + 0.5) * $Tron::Cell;
         %piece = new StaticShape() { dataBlock = "ExteriorFlagStand"; };
         %piece.setTransform(%px SPC %py SPC $Tron::GroundZ);
         %piece.setScale($Tron::PieceScale SPC $Tron::PieceScale SPC $Tron::PieceScale);
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
   tronWatch();
   echo("Tron gametype script loaded");
}
