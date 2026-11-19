// Snake: server-side ruleset for the MatrixSnake mission.
// Evaluated in memory by the t2launch hook (server.gametype_script); stock clients download nothing.
// The package is active only while $CurrentMission is MatrixSnake.
//
// Classic rules: a 20x20 grid, one step per tick, food grows the snake and speeds the game up, hitting a wall
// or yourself ends it, no 180-degree turns. Every player gets their own board in a row. The board stands
// upright (like Tetris.cs) because the engine clamps camera pitch to ~80 degrees, so a true top-down view is
// impossible; a level camera is not clamped.
// Controls (stock keybinds): forward/back/strafe = up/down/left/right. The client's control object is its
// Camera; its movement each tick is read as the d-pad.

$Snk::Mission = "MatrixSnake";
$Snk::N = 20;                        // cells per side
$Snk::Cell = 3;                      // metres per cell
$Snk::Z0 = 300;                      // bottom of the board
$Snk::Spacing = 80;                  // between boards
$Snk::CamDist = 70;                  // camera distance in front of the board plane
$Snk::CamZShift = -3;                // camera below the board centre, so the board sits above the HUD
$Snk::StartLength = 3;
$Snk::StepMs = 150;                  // time per step at the start
$Snk::MinStepMs = 70;
$Snk::SpeedUpMs = 4;                 // faster per food eaten
$Snk::FoodPoints = 10;
$Snk::TickMs = 32;
// The camera is the input device and the stock fly speed is 40 m/s, so a key press used to slide the view. The
// engine reads $Camera::movementSpeed, so while this ruleset is on the camera crawls instead: a key moves it by
// centimetres, which is still readable as input but leaves the view effectively fixed.
$Snk::CamSpeed = 1;                  // metres/second while this ruleset is on (stock: 40)
$Snk::Eps = 0.02;                    // the camera is off its anchor by more than this
$Snk::InputMin = 0.01;               // camera movement per tick that counts as a key sample
$Snk::RecentreMs = 250;
$Snk::IgnoreMs = 100;
$Snk::Drift = 0.4;                   // re-anchor when it has crept this far
$Snk::RespawnMs = 3000;
$Snk::MaxQueue = 3;
$Snk::Debug = 0;
$Snk::Dirs[0] = "0 1";  $Snk::Dirs[1] = "1 0";  $Snk::Dirs[2] = "0 -1";  $Snk::Dirs[3] = "-1 0";   // N E S W

$Snk::Pal[0] = "0.28 0.89 0.31";
$Snk::Pal[1] = "0.00 0.55 0.99";
$Snk::Pal[2] = "1.00 0.90 0.10";
$Snk::Pal[3] = "0.85 0.20 0.95";
$Snk::Pal[4] = "0.10 0.95 0.95";
$Snk::Pal[5] = "1.00 0.45 0.00";
$Snk::Pal[6] = "0.95 0.95 0.95";
$Snk::Pal[7] = "0.60 0.60 1.00";

$Snk::SeatOffset[0] = 0;  $Snk::SeatOffset[1] = 1;  $Snk::SeatOffset[2] = -1; $Snk::SeatOffset[3] = 2;
$Snk::SeatOffset[4] = -2; $Snk::SeatOffset[5] = 3;  $Snk::SeatOffset[6] = -3; $Snk::SeatOffset[7] = 4;
$Snk::Seats = 8;

// ---------------------------------------------------------------------------------------------------------
// datablocks: deleted by DestroyServer, so defined at install and again after every CreateServer

function snkDatablocks()
{
   if (isObject(SnkBody0))
      return;
   for (%i = 0; %i < 8; %i++)
   {
      eval("datablock ForceFieldBareData(SnkBody" @ %i @ ") : defaultForceFieldBare {"
         @ " color = \"" @ $Snk::Pal[%i] @ "\"; baseTranslucency = 0.5; needsNoPower = true; };");
      eval("datablock ForceFieldBareData(SnkHead" @ %i @ ") : defaultForceFieldBare {"
         @ " color = \"" @ $Snk::Pal[%i] @ "\"; baseTranslucency = 0.95; needsNoPower = true; };");
   }
   eval("datablock ForceFieldBareData(SnkFood) : defaultForceFieldBare {"
      @ " color = \"1.0 0.15 0.15\"; baseTranslucency = 0.9; needsNoPower = true; };");
   eval("datablock ForceFieldBareData(SnkFrame) : defaultForceFieldBare {"
      @ " color = \"0.9 0.9 0.9\"; baseTranslucency = 0.3; needsNoPower = true; };");
}

function snkField(%datablock, %x, %y, %z, %sx, %sy, %sz)
{
   return new ForceFieldBare() {
      dataBlock = %datablock;
      position = %x SPC %y SPC %z;
      rotation = "1 0 0 0";
      scale = %sx SPC %sy SPC %sz;
   };
}

// every force field adds a PhysicalZone in MissionCleanup/PZones; they are useless here, so drop them
function snkPurgeZones()
{
   %zones = nameToID("MissionCleanup/PZones");
   if (%zones > 0)
      %zones.delete();
}

// ---------------------------------------------------------------------------------------------------------
// geometry

function snkBoardX(%seat)
{
   return $Snk::SeatOffset[%seat] * $Snk::Spacing - $Snk::N * $Snk::Cell / 2;
}

function snkCellX(%seat, %cx)
{
   return snkBoardX(%seat) + %cx * $Snk::Cell;
}

function snkCellZ(%cy)
{
   return $Snk::Z0 + %cy * $Snk::Cell;
}

function snkAnchor(%seat)
{
   return (snkBoardX(%seat) + $Snk::N * $Snk::Cell / 2) SPC (-$Snk::CamDist)
      SPC ($Snk::Z0 + $Snk::N * $Snk::Cell / 2 + $Snk::CamZShift) SPC "1 0 0 0";
}

// put a block (a cell-sized force field, in the board plane y = 0) in cell (cx, cy); tall = extra depth toward the viewer
function snkPlace(%obj, %seat, %cx, %cy)
{
   %obj.setTransform(snkCellX(%seat, %cx) SPC "-1.5" SPC snkCellZ(%cy));
}

function snkPlaceDeep(%obj, %seat, %cx, %cy)
{
   %obj.setTransform(snkCellX(%seat, %cx) SPC "-3" SPC snkCellZ(%cy));
}

function snkFreeSeat()
{
   for (%s = 0; %s < $Snk::Seats; %s++)
   {
      %used = false;
      for (%i = 0; %i < ClientGroup.getCount(); %i++)
         if (ClientGroup.getObject(%i).snkSeat $= %s)
            %used = true;
      if (!%used)
         return %s;
   }
   return -1;
}

function snkMakeFrame(%cl)
{
   %seat = %cl.snkSeat;
   %bx = snkBoardX(%seat);
   %w = $Snk::N * $Snk::Cell;
   %t = 1.2;
   %g = new SimGroup();
   MissionCleanup.add(%g);
   %g.add(snkField(SnkFrame, %bx - %t, -1.5, $Snk::Z0 - %t, %t, 3, %w + %t * 2));
   %g.add(snkField(SnkFrame, %bx + %w, -1.5, $Snk::Z0 - %t, %t, 3, %w + %t * 2));
   %g.add(snkField(SnkFrame, %bx, -1.5, $Snk::Z0 - %t, %w, 3, %t));
   %g.add(snkField(SnkFrame, %bx, -1.5, $Snk::Z0 + %w, %w, 3, %t));
   %cl.snkFrame = %g;
}

function snkFreeBoard(%cl)
{
   if (isObject(%cl.snkGroup))
      %cl.snkGroup.delete();
   %cl.snkGroup = "";
   if (%cl.snkSeat !$= "")
      for (%x = 0; %x < $Snk::N; %x++)
         for (%y = 0; %y < $Snk::N; %y++)
            $Snk::Occ[%cl.snkSeat, %x, %y] = "";
}

// ---------------------------------------------------------------------------------------------------------
// the game for one player

function snkHud(%cl)
{
   bottomPrint(%cl, "SCORE " @ %cl.snkScore @ "    LENGTH " @ getWordCount(%cl.snkCells) / 2 @ "    BEST " @ %cl.snkBest, 0, 1);
}

function snkStartBoard(%cl)
{
   snkFreeBoard(%cl);
   %cl.snkGroup = new SimGroup();
   MissionCleanup.add(%cl.snkGroup);
   %seat = %cl.snkSeat;
   %cl.snkScore = 0;
   if (%cl.snkBest $= "")
      %cl.snkBest = 0;
   %cl.snkGrow = 0;
   %cl.snkStepMs = $Snk::StepMs;
   %cl.snkAcc = 0;
   %cl.snkDir = 1;
   %cl.snkQueue = "";
   %cl.snkAlive = true;
   %cl.snkLastCam = "";
   %cl.snkCells = "";
   %cl.snkBlks = "";
   %mid = mFloor($Snk::N / 2);
   // tail first, head last, heading east
   for (%i = 0; %i < $Snk::StartLength; %i++)
   {
      %cx = %mid - $Snk::StartLength + 1 + %i;
      %cl.snkCells = %cl.snkCells SPC %cx SPC %mid;
      $Snk::Occ[%seat, %cx, %mid] = 1;
      if (%i < $Snk::StartLength - 1)
      {
         %b = snkField("SnkBody" @ %seat, 0, -1.5, 0, $Snk::Cell, 3, $Snk::Cell);
         %cl.snkGroup.add(%b);
         snkPlace(%b, %seat, %cx, %mid);
         %cl.snkBlks = %cl.snkBlks SPC %b;
      }
   }
   %cl.snkCells = trim(%cl.snkCells);
   %cl.snkBlks = trim(%cl.snkBlks);
   %cl.snkHeadObj = snkField("SnkHead" @ %seat, 0, -3, 0, $Snk::Cell, 6, $Snk::Cell);
   %cl.snkGroup.add(%cl.snkHeadObj);
   snkPlaceDeep(%cl.snkHeadObj, %seat, %mid, %mid);
   %cl.snkFoodObj = snkField(SnkFood, 0, -3, 0, $Snk::Cell, 6, $Snk::Cell);
   %cl.snkGroup.add(%cl.snkFoodObj);
   snkNewFood(%cl);
   snkPurgeZones();
   snkHud(%cl);
}

function snkNewFood(%cl)
{
   %seat = %cl.snkSeat;
   for (%try = 0; %try < 300; %try++)
   {
      %cx = mFloor(getRandom() * $Snk::N);
      %cy = mFloor(getRandom() * $Snk::N);
      if ($Snk::Occ[%seat, %cx, %cy] $= "")
      {
         snkSetFood(%cl, %cx, %cy);
         return;
      }
   }
   // crowded board: scan for any free cell
   for (%cx = 0; %cx < $Snk::N; %cx++)
      for (%cy = 0; %cy < $Snk::N; %cy++)
         if ($Snk::Occ[%seat, %cx, %cy] $= "")
         {
            snkSetFood(%cl, %cx, %cy);
            return;
         }
   // no free cell left: the board is full, which is a win
   snkDie(%cl, "board full");
}

function snkSetFood(%cl, %cx, %cy)
{
   %cl.snkFoodX = %cx;
   %cl.snkFoodY = %cy;
   snkPlaceDeep(%cl.snkFoodObj, %cl.snkSeat, %cx, %cy);
}

function snkDie(%cl, %why)
{
   if (!%cl.snkAlive)
      return;
   %cl.snkAlive = false;
   %cl.snkRespawnAt = getSimTime() + $Snk::RespawnMs;
   if (%cl.snkScore > %cl.snkBest)
      %cl.snkBest = %cl.snkScore;
   if (isObject(%cl.snkHeadObj))
      %cl.snkHeadObj.delete();
   bottomPrint(%cl, "GAME OVER (" @ %why @ ")    SCORE " @ %cl.snkScore @ "    BEST " @ %cl.snkBest, 0, 1);
   messageAll('MsgSnakeOver', '\c1%1 ended a snake of length %2 (%3 points).', %cl.name, getWordCount(%cl.snkCells) / 2, %cl.snkScore);
}

// queue a turn unless it repeats or reverses the last direction
function snkEnqueue(%cl, %dir)
{
   %n = getWordCount(%cl.snkQueue);
   %last = %n > 0 ? getWord(%cl.snkQueue, %n - 1) : %cl.snkDir;
   if (%dir == %last || ((%dir + 2) % 4) == %last)
      return;
   if (%n >= $Snk::MaxQueue)
      return;
   %cl.snkQueue = %cl.snkQueue $= "" ? %dir : %cl.snkQueue SPC %dir;
}

function snkStep(%cl)
{
   %seat = %cl.snkSeat;
   if (%cl.snkQueue !$= "")
   {
      %cl.snkDir = getWord(%cl.snkQueue, 0);
      %cl.snkQueue = getWords(%cl.snkQueue, 1);
   }
   %d = $Snk::Dirs[%cl.snkDir];
   %n = getWordCount(%cl.snkCells) / 2;
   %hx = getWord(%cl.snkCells, (%n - 1) * 2);
   %hy = getWord(%cl.snkCells, (%n - 1) * 2 + 1);
   %nx = %hx + getWord(%d, 0);
   %ny = %hy + getWord(%d, 1);
   if (%nx < 0 || %nx >= $Snk::N || %ny < 0 || %ny >= $Snk::N)
   {
      snkDie(%cl, "wall");
      return;
   }
   %tx = getWord(%cl.snkCells, 0);
   %ty = getWord(%cl.snkCells, 1);
   %growing = %cl.snkGrow > 0;
   // the tail cell is free this step unless the snake is growing
   if ($Snk::Occ[%seat, %nx, %ny] !$= "" && (%growing || %nx != %tx || %ny != %ty))
   {
      snkDie(%cl, "self");
      return;
   }
   // the old head becomes body
   if (%growing)
   {
      %b = snkField("SnkBody" @ %seat, 0, -1.5, 0, $Snk::Cell, 3, $Snk::Cell);
      %cl.snkGroup.add(%b);
      %cl.snkGrow--;
   }
   else
   {
      %b = getWord(%cl.snkBlks, 0);
      %cl.snkBlks = getWords(%cl.snkBlks, 1);
      %cl.snkCells = getWords(%cl.snkCells, 2);
      $Snk::Occ[%seat, %tx, %ty] = "";
   }
   snkPlace(%b, %seat, %hx, %hy);
   %cl.snkBlks = %cl.snkBlks $= "" ? %b : %cl.snkBlks SPC %b;
   %cl.snkCells = %cl.snkCells SPC %nx SPC %ny;
   $Snk::Occ[%seat, %nx, %ny] = 1;
   snkPlaceDeep(%cl.snkHeadObj, %seat, %nx, %ny);
   // food
   if (%nx == %cl.snkFoodX && %ny == %cl.snkFoodY)
   {
      %cl.snkGrow++;
      %cl.snkScore += $Snk::FoodPoints;
      %cl.snkStepMs -= $Snk::SpeedUpMs;
      if (%cl.snkStepMs < $Snk::MinStepMs)
         %cl.snkStepMs = $Snk::MinStepMs;
      snkNewFood(%cl);
      snkPurgeZones();
      snkHud(%cl);
   }
}

// ---------------------------------------------------------------------------------------------------------
// the loop

function snkInput(%cl, %now)
{
   %cam = %cl.camera;
   if (%cl.getControlObject() != %cam)
   {
      %cl.setControlObject(%cam);
      %cam.setTransform(snkAnchor(%cl.snkSeat));
      %cl.snkLastCam = "";
      return;
   }
   // Read the d-pad as how far the camera moved since the last tick. The camera is only written to when its
   // rotation was disturbed (the mouse) or it needs bringing back to the board; rewriting it every tick would
   // make the client correct itself constantly.
   %t = %cam.getTransform();
   %px = getWord(%t, 0);
   %py = getWord(%t, 1);
   %pz = getWord(%t, 2);
   if (%cl.snkLastCam $= "")
      %cl.snkLastCam = %px SPC %py SPC %pz;
   %dx = %px - getWord(%cl.snkLastCam, 0);
   %fwd = %py - getWord(%cl.snkLastCam, 1);     // level camera: forward moves it along +Y, toward the board
   %cl.snkLastCam = %px SPC %py SPC %pz;
   if (%now < %cl.snkIgnoreUntil)
   {
      // just re-centred: the camera may echo a sample or two
      %dx = 0;
      %fwd = 0;
   }
   %ax = mAbs(%dx);
   %af = mAbs(%fwd);
   if (%ax > $Snk::InputMin || %af > $Snk::InputMin)
   {
      %cl.snkSampleAt = %now;
      if ($Snk::Debug)
         echo("SNK input t=" @ %now SPC "d=" @ %dx SPC %fwd);
      if (%af >= %ax)
         %want = %fwd > 0 ? 0 : 2;
      else
         %want = %dx > 0 ? 1 : 3;
      // a held key repeats the same direction; only a change is a new turn
      if (%want !$= %cl.snkLastWant)
      {
         %cl.snkLastWant = %want;
         snkEnqueue(%cl, %want);
      }
   }
   else if (%now - %cl.snkSampleAt > 150)
      %cl.snkLastWant = "";
   %a = snkAnchor(%cl.snkSeat);
   // once the keys have been quiet for a moment, bring the camera back to the board
   if (%now - %cl.snkSampleAt > $Snk::RecentreMs
      && (mAbs(%px - getWord(%a, 0)) > $Snk::Eps || mAbs(%py - getWord(%a, 1)) > $Snk::Eps || mAbs(%pz - getWord(%a, 2)) > $Snk::Eps))
   {
      %cam.setTransform(%a);
      %cam.setVelocity("0 0 0");
      %cl.snkLastCam = getWords(%a, 0, 2);
      %cl.snkIgnoreUntil = %now + $Snk::IgnoreMs;
      return;
   }
   if (mAbs(%px - getWord(%a, 0)) > $Snk::Drift || mAbs(%py - getWord(%a, 1)) > $Snk::Drift || mAbs(%pz - getWord(%a, 2)) > $Snk::Drift)
   {
      %cam.setTransform(%a);
      %cl.snkLastCam = getWords(%a, 0, 2);
      return;
   }
   // the mouse turned the view: put the rotation back, keeping the position
   if (mAbs(getWord(%t, 6)) > 0.004)
      %cam.setTransform(%px SPC %py SPC %pz SPC "1 0 0 0");
}

function snkTick()
{
   cancel($Snk::TickThread);
   $Snk::TickThread = schedule($Snk::TickMs, 0, snkTick);
   if (!isObject(MissionCleanup))
      return;
   $Snk::TickN++;
   %now = getSimTime();
   %dt = $Snk::LastTick $= "" ? 32 : %now - $Snk::LastTick;
   $Snk::LastTick = %now;
   if (%dt > 200)
      %dt = 200;
   for (%i = 0; %i < ClientGroup.getCount(); %i++)
   {
      %cl = ClientGroup.getObject(%i);
      if (%cl.snkSeat $= "" || !isObject(%cl.camera))
         continue;
      snkInput(%cl, %now);
      if (!%cl.snkAlive)
      {
         if (%now >= %cl.snkRespawnAt)
            snkStartBoard(%cl);
      }
      else
      {
         %cl.snkAcc += %dt;
         %guard = 0;
         while (%cl.snkAlive && %cl.snkAcc >= %cl.snkStepMs && %guard < 4)
         {
            %cl.snkAcc -= %cl.snkStepMs;
            %guard++;
            snkStep(%cl);
         }
      }
      // the stand-in Player never moves
      if (isObject(%cl.player))
      {
         %cl.player.setVelocity("0 0 0");
         if ($Snk::TickN % 30 == 0)
            %cl.player.setTransform("0 -150 300 1 0 0 0");
      }
   }
}

function snkInitPlayer(%cl)
{
   if (%cl.snkSeat $= "")
   {
      %seat = snkFreeSeat();
      if (%seat < 0)
         return;
      %cl.snkSeat = %seat;
      snkMakeFrame(%cl);
   }
   %cam = %cl.camera;
   %cam.setFlyMode();
   %cam.setTransform(snkAnchor(%cl.snkSeat));
   %cl.setControlObject(%cam);
   commandToClient(%cl, 'setHudMode', 'Observer');
   %cl.schedule(100, "setControlObject", %cam);
   if (isObject(%cl.player))
   {
      %cl.player.setTransform("0 -150 300 1 0 0 0");
      %cl.player.setVelocity("0 0 0");
   }
   if (!%cl.snkAlive && !isObject(%cl.snkGroup))
      snkStartBoard(%cl);
}

function snkFreeClient(%cl)
{
   snkFreeBoard(%cl);
   if (isObject(%cl.snkFrame))
      %cl.snkFrame.delete();
   %cl.snkFrame = "";
   %cl.snkSeat = "";
   %cl.snkAlive = false;
}

// Switch the package on and off with the mission, so stock DM missions are untouched.
function snkWatch()
{
   cancel($Snk::WatchThread);
   $Snk::WatchThread = schedule(250, 0, snkWatch);
   %on = ($CurrentMission $= $Snk::Mission);
   if (%on && !$Snk::PkgOn)
   {
      activatePackage(T2LaunchSnake);
      $Snk::PkgOn = true;
      $Snk::SavedSpeed = $Camera::movementSpeed;
      $Camera::movementSpeed = $Snk::CamSpeed;
      $Snk::LastTick = "";
      snkTick();
      echo("Snake ruleset on");
   }
   else if (!%on && $Snk::PkgOn)
   {
      deactivatePackage(T2LaunchSnake);
      $Snk::PkgOn = false;
      $Camera::movementSpeed = $Snk::SavedSpeed;
      cancel($Snk::TickThread);
      echo("Snake ruleset off");
   }
}

package T2LaunchSnakeBoot
{
   function CreateServer(%mission, %missionType)
   {
      Parent::CreateServer(%mission, %missionType);
      snkDatablocks();
   }
};

package T2LaunchSnake
{
   function DMGame::equip(%game, %player)
   {
      // snakes carry nothing
   }

   function DMGame::createPlayer(%game, %client, %spawnLoc, %respawn)
   {
      Parent::createPlayer(%game, %client, %spawnLoc, %respawn);
      snkInitPlayer(%client);
   }

   function DMGame::onClientKilled(%game, %clVictim, %clKiller, %damageType, %implement, %damageLoc)
   {
      // the stand-in Player is not part of the game: no respawn timer or orbit camera
      %clVictim.deaths++;
   }

   function DMGame::ObserverOnTrigger(%game, %data, %obj, %trigger, %state)
   {
      %client = %obj.getControllingClient();
      if (%client && %client.snkSeat !$= "")
         return false;   // a playing snake must not leave the locked camera
      return Parent::ObserverOnTrigger(%game, %data, %obj, %trigger, %state);
   }

   function DMGame::onClientLeaveGame(%game, %client)
   {
      snkFreeClient(%client);
      Parent::onClientLeaveGame(%game, %client);
   }
};

if (!$Snk::Installed)
{
   $Snk::Installed = true;
   activatePackage(T2LaunchSnakeBoot);
   snkDatablocks();
   snkWatch();
   echo("Snake gametype script loaded");
}
