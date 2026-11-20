// Tron: server-side light-cycle ruleset for the MatrixTron mission.
// Evaluated in memory by the t2launch hook (server.gametype_script); stock clients download nothing.
// The package is active only while $CurrentMission is MatrixTron.
//
// The arena is an upright wall seen side-on (like Tetris.cs and Snake.cs): the engine clamps camera pitch to
// about 80 degrees, so a true top-down view is impossible, but a level camera is not clamped. Riders move
// in the x/z plane (plane Y = up) and everyone shares one arena.
// Control model: the client's control object is its Camera. Its movement each tick is read as the d-pad
// (forward = up, back = down, strafe = left/right); the server moves the riders and their trails.

$Tron::Mission = "MatrixTron";
$Tron::HalfX = 30;                   // arena half-width in metres
$Tron::HalfY = 17;                   // arena half-height
$Tron::CenterZ = 320;                // world height of the arena centre
$Tron::Cell = 3;                     // trail cell size
$Tron::Speed = 10;                   // metres/second
$Tron::CamDist = 44;                 // camera distance in front of the wall
$Tron::CamZShift = -2;               // camera a little below the centre, so the arena sits above the HUD
$Tron::TickMs = 32;
$Tron::WallDepth = 3;                // thickness of trail blocks (towards the viewer)
$Tron::HeadSize = 2;                 // the rider's head block
$Tron::HeadDepth = 5;
$Tron::BorderDepth = 5;
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
$Tron::Dirs[0] = "0 1";      // up
$Tron::Dirs[1] = "1 0";      // right
$Tron::Dirs[2] = "0 -1";     // down
$Tron::Dirs[3] = "-1 0";     // left

// Force-field datablocks must exist before clients connect, and DestroyServer deletes them, so they are
// defined at install and again after every CreateServer.
function tronDatablocks()
{
   if (isObject(TronField0))
      return;
   for (%i = 0; %i < 8; %i++)
      eval("datablock ForceFieldBareData(TronField" @ %i @ ") : defaultForceFieldBare { framesPerSec = 1; numFrames = 1; scrollSpeed = 0;"
         @ " color = \"" @ $Tron::Palette[%i] @ "\"; baseTranslucency = 0.45; needsNoPower = true; };");
   eval("datablock ForceFieldBareData(TronBorder) : defaultForceFieldBare { framesPerSec = 1; numFrames = 1; scrollSpeed = 0;"
      @ " color = \"0.9 0.9 0.9\"; baseTranslucency = 0.25; needsNoPower = true; };");
}

function tronFieldBlock(%client)
{
   return "TronField" @ ((%client.team + 7) % 8);
}

// a force-field box in the wall plane: (x, y) is its lower-left corner with y up, sizes are width and height
function tronField(%datablock, %x, %y, %sx, %sy, %depth)
{
   %f = new ForceFieldBare() {
      dataBlock = %datablock;
      position = %x SPC (-%depth / 2) SPC ($Tron::CenterZ + %y);
      rotation = "1 0 0 0";
      scale = %sx SPC %depth SPC %sy;
   };
   return %f;
}

function tronAnchor()
{
   return "0" SPC (-$Tron::CamDist) SPC ($Tron::CenterZ + $Tron::CamZShift) SPC "1 0 0 0";
}

function tronDump()
{
   for (%i = 0; %i < ClientGroup.getCount(); %i++)
   {
      %c = ClientGroup.getObject(%i);
      echo("TT> client=" @ %c @ " ctl=" @ %c.getControlObject() @ " cam=" @ %c.camera
         @ " alive=" @ %c.trAlive @ " heading=" @ %c.trHeading @ " pos=" @ %c.trX SPC %c.trY
         @ " camtr=" @ %c.camera.getTransform()
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
   // four seats facing inward: heading 0 up, 1 right, 2 down, 3 left
   switch (%n % 4)
   {
      case 0: return (-$Tron::HalfX / 2) SPC (-$Tron::HalfY + 3) SPC 0;
      case 1: return ($Tron::HalfX / 2) SPC ($Tron::HalfY - 3) SPC 2;
      case 2: return (-$Tron::HalfX + 4) SPC ($Tron::HalfY / 2) SPC 1;
      case 3: return ($Tron::HalfX - 4) SPC (-$Tron::HalfY / 2) SPC 3;
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
   tronMakeHead(%client);
   // the rider is the head block and the trail; its Player only reads the keys, standing in front of the wall
   %a = tronAnchor();
   retroAttach(%client, getWord(%a, 0), getWord(%a, 1), getWord(%a, 2));
   commandToClient(%client, 'setHudMode', 'Standard');
}

function tronMakeHead(%client)
{
   if (isObject(%client.trHead))
      %client.trHead.delete();
   %half = $Tron::HeadSize / 2;
   %head = tronField(tronFieldBlock(%client), %client.trX - %half, %client.trY - %half,
      $Tron::HeadSize, $Tron::HeadSize, $Tron::HeadDepth);
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
   MissionCleanup.add($Tron::Border);
   %hx = $Tron::HalfX;
   %hy = $Tron::HalfY;
   %w = %hx * 2 + 1;
   %h = %hy * 2 + 1;
   $Tron::Border.add(tronField(TronBorder, -%hx - 0.5, %hy, %w, 0.5, $Tron::BorderDepth));
   $Tron::Border.add(tronField(TronBorder, -%hx - 0.5, -%hy - 0.5, %w, 0.5, $Tron::BorderDepth));
   $Tron::Border.add(tronField(TronBorder, %hx, -%hy - 0.5, 0.5, %h, $Tron::BorderDepth));
   $Tron::Border.add(tronField(TronBorder, -%hx - 0.5, -%hy - 0.5, 0.5, %h, $Tron::BorderDepth));
}

function tronKillRider(%client, %why)
{
   if (!%client.trAlive)
      return;
   echo("TRON kill " @ %client.nameBase @ " why=" @ %why @ " pos=" @ %client.trX SPC %client.trY @ " heading=" @ %client.trHeading @ " cell=" @ %client.trCell);
   %client.trAlive = false;
   if (isObject(%client.trHead))
      %client.trHead.delete();
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
      if (%cl.team <= 0 || %cl.trSeat $= "")
         continue;
      %cl.trSeat = %seat;
      %seat++;
      tronInitRider(%cl);
   }
}

// Read the d-pad as how far the camera moved since the last tick. The camera is only written to when its
// rotation was disturbed (the mouse) or it needs bringing back to the wall; rewriting it every tick would
// make the client correct itself constantly. Returns the wanted heading, or "" for none.
// the heading the held keys ask for (0 up the wall, 1 right, 2 down, 3 left), or "" when none is held
function tronInput(%cl, %now)
{
   %m = retroPad(%cl);
   if (%m & 1) return 0;
   if (%m & 8) return 1;
   if (%m & 2) return 2;
   if (%m & 4) return 3;
   return "";
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
   $Tron::TickN++;
   %now = getSimTime();
   %dt = $Tron::LastTick $= "" ? 0.032 : (%now - $Tron::LastTick) / 1000;
   $Tron::LastTick = %now;
   if (%dt > 0.2) %dt = 0.2;
   for (%i = 0; %i < ClientGroup.getCount(); %i++)
   {
      %cl = ClientGroup.getObject(%i);
      if (%cl.trSeat $= "" || !isObject(%cl.player))
         continue;
      %want = tronInput(%cl, %now);
      if (!%cl.trAlive)
         continue;
      // turn: never straight back into the trail
      if (%want !$= "" && %want != %cl.trHeading && ((%want + 2) % 4) != %cl.trHeading)
         %cl.trHeading = %want;
      // advance
      %d = $Tron::Dirs[%cl.trHeading];
      %cl.trX += getWord(%d, 0) * $Tron::Speed * %dt;
      %cl.trY += getWord(%d, 1) * $Tron::Speed * %dt;
      if (isObject(%cl.trHead))
         %cl.trHead.setTransform((%cl.trX - $Tron::HeadSize / 2) SPC (-$Tron::HeadDepth / 2) SPC ($Tron::CenterZ + %cl.trY - $Tron::HeadSize / 2));
      if (mAbs(%cl.trX) > $Tron::HalfX || mAbs(%cl.trY) > $Tron::HalfY)
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
      // lay a trail block in the cell just left
      if (%cl.trCell !$= "")
      {
         %px = getWord(strreplace(%cl.trCell, "_", " "), 0) * $Tron::Cell;
         %py = getWord(strreplace(%cl.trCell, "_", " "), 1) * $Tron::Cell;
         %piece = tronField(tronFieldBlock(%cl), %px, %py, $Tron::Cell, $Tron::Cell, $Tron::WallDepth);
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
      // the stand-in Player is not part of the game: no respawn timer or orbit camera
      %clVictim.deaths++;
   }

   function DMGame::clientJoinTeam(%game, %client, %team, %respawn)
   {
      Parent::clientJoinTeam(%game, %client, %team, %respawn);
   }

   function DMGame::onClientLeaveGame(%game, %client)
   {
      %client.trAlive = false;
      retroDetach(%client);
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
