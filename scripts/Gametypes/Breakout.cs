// Breakout: server-side ruleset for the MatrixBreakout mission.
// Evaluated in memory by the t2launch hook (server.gametype_script); stock clients download nothing.
// The package is active only while $CurrentMission is MatrixBreakout.
//
// Arcade rules: eight rows of fourteen bricks (yellow 1 point, green 3, orange 5, red 7, two rows each), three
// lives, the ball speeds up at the 4th and 12th brick and on the first orange and red hits, and the paddle
// shrinks to half size when the ball first reaches the ceiling. A cleared wall comes back, a little faster.
// Every player gets their own board in a row on an upright wall, seen side-on (like Tetris.cs and Snake.cs).
// Controls (stock keybinds): strafe moves the paddle, fire serves (the ball is also served after a few seconds).
// The client's control object is its Camera; its movement each tick is read as the d-pad.

$Brk::Mission = "MatrixBreakout";
$Brk::HalfW = 18;                    // court half-width in metres
$Brk::Height = 34;
$Brk::Z0 = 300;                      // world height of the court floor
$Brk::Spacing = 44;                  // between boards
$Brk::Depth = 2;
$Brk::Cols = 14;
$Brk::Rows = 8;
$Brk::BrickW = 2.4;
$Brk::BrickH = 1;
$Brk::BrickV0 = 22;                  // height of the bottom brick row above the floor
$Brk::PadV = 2;                      // paddle centre height
$Brk::PadH = 1;
$Brk::PadW = 5;
$Brk::PadSpeed = 36;
$Brk::BallSize = 1;
$Brk::BaseSpeed = 15;
$Brk::SpeedStep = 3;
$Brk::MaxSpeed = 36;
$Brk::MaxAngle = 0.96;               // radians off vertical for a hit on the paddle's edge
$Brk::Lives = 3;
$Brk::AutoServeMs = 4000;
$Brk::GameOverMs = 4500;
$Brk::CamDist = 44;
$Brk::CamZShift = -1.5;
$Brk::KeyGraceMs = 110;
$Brk::RecentreMs = 250;
$Brk::IgnoreMs = 100;
$Brk::TickMs = 32;
$Brk::ParkX = 3000;                  // destroyed bricks wait far outside every client's scope
$Brk::ParkY = 3000;
$Brk::Debug = 0;
$Brk::Seats = 8;
$Brk::SeatOffset[0] = 0;  $Brk::SeatOffset[1] = 1;  $Brk::SeatOffset[2] = -1; $Brk::SeatOffset[3] = 2;
$Brk::SeatOffset[4] = -2; $Brk::SeatOffset[5] = 3;  $Brk::SeatOffset[6] = -3; $Brk::SeatOffset[7] = 4;

// ---------------------------------------------------------------------------------------------------------
// datablocks: deleted by DestroyServer, so defined at install and again after every CreateServer

function brkDatablocks()
{
   if (isObject(BrkPaddle))
      return;
   %colors = "1.0 0.9 0.1 0.2 0.9 0.2 1.0 0.55 0.1 1.0 0.15 0.1";    // yellow, green, orange, red
   for (%i = 0; %i < 4; %i++)
      eval("datablock ForceFieldBareData(BrkBrick" @ %i @ ") : defaultForceFieldBare { framesPerSec = 1; numFrames = 1; scrollSpeed = 0; color = \""
         @ getWords(%colors, %i * 3, %i * 3 + 2) @ "\"; baseTranslucency = 0.85; needsNoPower = true; };");
   eval("datablock ForceFieldBareData(BrkPaddle) : defaultForceFieldBare { framesPerSec = 1; numFrames = 1; scrollSpeed = 0; color = \"0.15 0.6 1.0\"; baseTranslucency = 0.9; needsNoPower = true; };");
   eval("datablock ForceFieldBareData(BrkBall) : defaultForceFieldBare { framesPerSec = 1; numFrames = 1; scrollSpeed = 0; color = \"1.0 1.0 1.0\"; baseTranslucency = 0.95; needsNoPower = true; };");
   eval("datablock ForceFieldBareData(BrkWall) : defaultForceFieldBare { framesPerSec = 1; numFrames = 1; scrollSpeed = 0; color = \"0.8 0.8 0.85\"; baseTranslucency = 0.3; needsNoPower = true; };");
}

function brkField(%db, %x, %z, %sx, %sz)
{
   return new ForceFieldBare() {
      dataBlock = %db;
      position = %x SPC (-$Brk::Depth / 2) SPC %z;
      rotation = "1 0 0 0";
      scale = %sx SPC $Brk::Depth SPC %sz;
   };
}

function brkPurgeZones()
{
   %z = nameToID("MissionCleanup/PZones");
   if (%z > 0)
      %z.delete();
}

// ---------------------------------------------------------------------------------------------------------
// geometry: court coordinates are (u, v) = across from the centre line, and up from the floor

function brkBoardX(%seat)
{
   return $Brk::SeatOffset[%seat] * $Brk::Spacing;
}

function brkAnchor(%seat)
{
   return brkBoardX(%seat) SPC (-$Brk::CamDist) SPC ($Brk::Z0 + $Brk::Height / 2 + $Brk::CamZShift) SPC "1 0 0 0";
}

// a block's position is its lower-left corner; (%u, %v) is its centre
function brkPlace(%obj, %seat, %u, %v, %w, %h)
{
   %obj.setTransform((brkBoardX(%seat) + %u - %w / 2) SPC (-$Brk::Depth / 2) SPC ($Brk::Z0 + %v - %h / 2));
}

function brkBrickU(%col)
{
   return (%col - ($Brk::Cols - 1) / 2) * $Brk::BrickW;
}

function brkBrickV(%row)
{
   return $Brk::BrickV0 + %row * $Brk::BrickH + $Brk::BrickH / 2;
}

function brkRowColor(%row)
{
   return mFloor(%row / 2);                  // 0 yellow, 1 green, 2 orange, 3 red
}

function brkRowPoints(%row)
{
   return 1 + mFloor(%row / 2) * 2;          // 1, 3, 5, 7
}

function brkFreeSeat()
{
   for (%s = 0; %s < $Brk::Seats; %s++)
   {
      %used = false;
      for (%i = 0; %i < ClientGroup.getCount(); %i++)
         if (ClientGroup.getObject(%i).brkSeat $= %s)
            %used = true;
      if (!%used)
         return %s;
   }
   return "";
}

// ---------------------------------------------------------------------------------------------------------
// one board

function brkMakeBoard(%cl)
{
   %seat = %cl.brkSeat;
   %g = new SimGroup();
   MissionCleanup.add(%g);
   %cl.brkGroup = %g;
   // side walls and a ceiling
   %h = $Brk::Height;
   %w = $Brk::HalfW;
   %g.add(brkField(BrkWall, brkBoardX(%seat) - %w - 0.6, $Brk::Z0, 0.6, %h + 0.6));
   %g.add(brkField(BrkWall, brkBoardX(%seat) + %w, $Brk::Z0, 0.6, %h + 0.6));
   %g.add(brkField(BrkWall, brkBoardX(%seat) - %w - 0.6, $Brk::Z0 + %h, %w * 2 + 1.2, 0.6));
   %cl.brkPad = brkField(BrkPaddle, 0, 0, $Brk::PadW, $Brk::PadH);
   %cl.brkPadShown = $Brk::PadW;
   %cl.brkBall = brkField(BrkBall, 0, 0, $Brk::BallSize, $Brk::BallSize);
   %g.add(%cl.brkPad);
   %g.add(%cl.brkBall);
   for (%row = 0; %row < $Brk::Rows; %row++)
      for (%col = 0; %col < $Brk::Cols; %col++)
      {
         %b = brkField("BrkBrick" @ brkRowColor(%row), 0, 0, $Brk::BrickW - 0.12, $Brk::BrickH - 0.12);
         %g.add(%b);
         $Brk::Obj[%seat, %row * $Brk::Cols + %col] = %b;
      }
   brkPurgeZones();
}

function brkResetBricks(%cl)
{
   %seat = %cl.brkSeat;
   for (%row = 0; %row < $Brk::Rows; %row++)
      for (%col = 0; %col < $Brk::Cols; %col++)
      {
         %i = %row * $Brk::Cols + %col;
         $Brk::Alive[%seat, %i] = true;
         brkPlace($Brk::Obj[%seat, %i], %seat, brkBrickU(%col), brkBrickV(%row), $Brk::BrickW - 0.12, $Brk::BrickH - 0.12);
      }
   %cl.brkBricks = $Brk::Rows * $Brk::Cols;
}

function brkStartGame(%cl)
{
   %cl.brkScore = 0;
   %cl.brkLives = $Brk::Lives;
   %cl.brkLevel = 1;
   brkNewWall(%cl);
}

function brkNewWall(%cl)
{
   brkResetBricks(%cl);
   %cl.brkHits = 0;
   %cl.brkOrange = false;
   %cl.brkRed = false;
   %cl.brkPadW = $Brk::PadW;
   brkReady(%cl);
}

// the ball waits on the paddle until fire (or a timeout)
function brkReady(%cl)
{
   %cl.brkState = "ready";
   %cl.brkReadyAt = getSimTime() + $Brk::AutoServeMs;
   %cl.brkSpeed = $Brk::BaseSpeed + (%cl.brkLevel - 1);
   %cl.brkSteps = 0;
   brkHud(%cl);
}

function brkLaunch(%cl)
{
   if (%cl.brkState !$= "ready")
      return;
   %cl.brkState = "play";
   %a = (getRandom() - 0.5) * 0.9;
   %cl.brkVU = %cl.brkSpeed * mSin(%a);
   %cl.brkVV = %cl.brkSpeed * mCos(%a);
   brkHud(%cl);
}

function brkHud(%cl)
{
   if (%cl.brkState $= "over")
   {
      bottomPrint(%cl, "GAME OVER    SCORE " @ %cl.brkScore, 0, 1);
      return;
   }
   %text = "SCORE " @ %cl.brkScore @ "    LIVES " @ %cl.brkLives @ "    WALL " @ %cl.brkLevel;
   if (%cl.brkState $= "ready")
      %text = %text @ "    fire = serve";
   bottomPrint(%cl, %text, 0, 1);
}

function brkSetSpeed(%cl, %extra)
{
   %cl.brkSpeed += %extra;
   if (%cl.brkSpeed > $Brk::MaxSpeed)
      %cl.brkSpeed = $Brk::MaxSpeed;
   // keep the direction, change the length
   %len = mSqrt(%cl.brkVU * %cl.brkVU + %cl.brkVV * %cl.brkVV);
   if (%len > 0.01)
   {
      %cl.brkVU = %cl.brkVU * %cl.brkSpeed / %len;
      %cl.brkVV = %cl.brkVV * %cl.brkSpeed / %len;
   }
}

function brkBreak(%cl, %i)
{
   %seat = %cl.brkSeat;
   %row = mFloor(%i / $Brk::Cols);
   $Brk::Alive[%seat, %i] = false;
   $Brk::Obj[%seat, %i].setTransform($Brk::ParkX SPC $Brk::ParkY SPC 300);
   %cl.brkBricks--;
   %cl.brkScore += brkRowPoints(%row);
   %cl.brkHits++;
   // speed-ups: the 4th and 12th brick, and the first orange and red hits
   if (%cl.brkHits == 4 || %cl.brkHits == 12)
      brkSetSpeed(%cl, $Brk::SpeedStep);
   if (%row >= 4 && !%cl.brkOrange)
   {
      %cl.brkOrange = true;
      brkSetSpeed(%cl, $Brk::SpeedStep);
   }
   if (%row >= 6 && !%cl.brkRed)
   {
      %cl.brkRed = true;
      brkSetSpeed(%cl, $Brk::SpeedStep);
   }
   brkHud(%cl);
   if (%cl.brkBricks <= 0)
   {
      %cl.brkLevel++;
      brkNewWall(%cl);
   }
}

function brkLoseBall(%cl)
{
   %cl.brkLives--;
   if (%cl.brkLives <= 0)
   {
      %cl.brkState = "over";
      %cl.brkOverAt = getSimTime() + $Brk::GameOverMs;
      messageAll('MsgBreakout', '\c1%1 finished with %2 points.', %cl.name, %cl.brkScore);
      brkHud(%cl);
      return;
   }
   brkReady(%cl);
}

function brkPhysics(%cl, %dt, %now)
{
   %seat = %cl.brkSeat;
   // the paddle
   %dir = 0;
   if (%cl.brkPadMask & 1)
      %dir = -1;
   else if (%cl.brkPadMask & 2)
      %dir = 1;
   %max = $Brk::HalfW - %cl.brkPadW / 2;
   %cl.brkU += %dir * $Brk::PadSpeed * %dt;
   if (%cl.brkU > %max) %cl.brkU = %max;
   if (%cl.brkU < -%max) %cl.brkU = -%max;
   %half = $Brk::BallSize / 2;
   if (%cl.brkState $= "ready")
   {
      %cl.brkBU = %cl.brkU;
      %cl.brkBV = $Brk::PadV + $Brk::PadH / 2 + %half + 0.05;
      if (%now >= %cl.brkReadyAt)
         brkLaunch(%cl);
   }
   else if (%cl.brkState $= "over")
   {
      if (%now >= %cl.brkOverAt)
         brkStartGame(%cl);
   }
   else if (%cl.brkState $= "play")
   {
      %speed = mSqrt(%cl.brkVU * %cl.brkVU + %cl.brkVV * %cl.brkVV);
      %steps = mCeil(%speed * %dt / 0.3);
      if (%steps < 1) %steps = 1;
      %sdt = %dt / %steps;
      for (%k = 0; %k < %steps; %k++)
      {
         %pu = %cl.brkBU;
         %pv = %cl.brkBV;
         %cl.brkBU += %cl.brkVU * %sdt;
         %cl.brkBV += %cl.brkVV * %sdt;
         // walls and ceiling
         %wall = $Brk::HalfW - %half;
         if (%cl.brkBU > %wall) { %cl.brkBU = %wall; %cl.brkVU = -%cl.brkVU; }
         if (%cl.brkBU < -%wall) { %cl.brkBU = -%wall; %cl.brkVU = -%cl.brkVU; }
         if (%cl.brkBV > $Brk::Height - %half)
         {
            %cl.brkBV = $Brk::Height - %half;
            %cl.brkVV = -%cl.brkVV;
            if (%cl.brkPadW > $Brk::PadW / 2)
               %cl.brkPadW = $Brk::PadW / 2;          // the arcade paddle halves once the ball reaches the top
         }
         // bricks
         %col = mFloor((%cl.brkBU + $Brk::Cols * $Brk::BrickW / 2) / $Brk::BrickW);
         %row = mFloor((%cl.brkBV - $Brk::BrickV0) / $Brk::BrickH);
         if (%col >= 0 && %col < $Brk::Cols && %row >= 0 && %row < $Brk::Rows
            && $Brk::Alive[%seat, %row * $Brk::Cols + %col])
         {
            %pcol = mFloor((%pu + $Brk::Cols * $Brk::BrickW / 2) / $Brk::BrickW);
            if (%pcol != %col)
               %cl.brkVU = -%cl.brkVU;
            else
               %cl.brkVV = -%cl.brkVV;
            %cl.brkBU = %pu;
            %cl.brkBV = %pv;
            brkBreak(%cl, %row * $Brk::Cols + %col);
            if (%cl.brkState !$= "play")
               break;
         }
         // paddle: only while the ball is falling
         if (%cl.brkVV < 0)
         {
            %top = $Brk::PadV + $Brk::PadH / 2;
            if (%cl.brkBV - %half <= %top && %cl.brkBV + %half >= $Brk::PadV - $Brk::PadH / 2
               && mAbs(%cl.brkBU - %cl.brkU) <= %cl.brkPadW / 2 + %half)
            {
               %off = (%cl.brkBU - %cl.brkU) / (%cl.brkPadW / 2 + %half);
               if (%off > 1) %off = 1;
               if (%off < -1) %off = -1;
               %a = %off * $Brk::MaxAngle;
               %sp = %cl.brkSpeed;
               %cl.brkVU = %sp * mSin(%a);
               %cl.brkVV = %sp * mCos(%a);
               %cl.brkBV = %top + %half + 0.02;
            }
         }
         // the floor
         if (%cl.brkBV < -2)
         {
            brkLoseBall(%cl);
            break;
         }
      }
   }
   // a force field cannot be resized once made, so the shrunken paddle is a new block
   if (%cl.brkPadShown != %cl.brkPadW)
   {
      %cl.brkPad.delete();
      %cl.brkPad = brkField(BrkPaddle, 0, 0, %cl.brkPadW, $Brk::PadH);
      %cl.brkGroup.add(%cl.brkPad);
      %cl.brkPadShown = %cl.brkPadW;
   }
   brkPlace(%cl.brkPad, %seat, %cl.brkU, $Brk::PadV, %cl.brkPadW, $Brk::PadH);
   brkPlace(%cl.brkBall, %seat, %cl.brkBU, %cl.brkBV, $Brk::BallSize, $Brk::BallSize);
}

// ---------------------------------------------------------------------------------------------------------
// input: the player's keys, read by Retro.cs from its (tiny) velocity; 1 while strafe-left is held, 2 for right

function brkReadPad(%cl)
{
   %m = retroPad(%cl);
   %out = 0;
   if (%m & 4)
      %out = %out | 1;
   if (%m & 8)
      %out = %out | 2;
   return %out;
}

function brkTick()
{
   cancel($Brk::TickThread);
   $Brk::TickThread = schedule($Brk::TickMs, 0, brkTick);
   if (!isObject(MissionCleanup))
      return;
   $Brk::TickN++;
   %now = getSimTime();
   %dt = $Brk::LastTick $= "" ? 0.032 : (%now - $Brk::LastTick) / 1000;
   $Brk::LastTick = %now;
   if (%dt > 0.1) %dt = 0.1;
   for (%i = 0; %i < ClientGroup.getCount(); %i++)
   {
      %cl = ClientGroup.getObject(%i);
      if (!%cl.brkJoined || !isObject(%cl.camera))
         continue;
      if (%cl.brkSeat $= "")
      {
         // spectators have the stock free-fly camera; the stand-in Player is parked out of the way
         if (isObject(%cl.player))
         {
            %cl.player.setVelocity("0 0 0");
            if ($Brk::TickN % 30 == 0)
               %cl.player.setTransform("0 -150 300 1 0 0 0");
         }
         if (%cl.getControlObject() != %cl.camera)
            %cl.setControlObject(%cl.camera);
         continue;
      }
      %cl.brkPadMask = brkReadPad(%cl);
      brkPhysics(%cl, %dt, %now);
   }
}

function brkInitPlayer(%cl)
{
   %cl.brkJoined = true;
   if (%cl.brkSeat $= "")
   {
      %seat = brkFreeSeat();
      if (%seat !$= "")
      {
         %cl.brkSeat = %seat;
         brkMakeBoard(%cl);
         %cl.brkU = 0;
         %cl.brkBU = 0;
         %cl.brkBV = 5;
      }
   }
   if (%cl.brkSeat $= "")
   {
      // a spectator: the free-fly camera, parked player
      %cam = %cl.camera;
      %cam.setFlyMode();
      %cam.setTransform(brkAnchor(0));
      %cl.setControlObject(%cam);
      commandToClient(%cl, 'setHudMode', 'Observer');
      %cl.schedule(100, "setControlObject", %cam);
      if (isObject(%cl.player))
      {
         %cl.player.setTransform("0 -150 300 1 0 0 0");
         %cl.player.setVelocity("0 0 0");
      }
      bottomPrint(%cl, "Watching: every board is taken. You can fly around freely.", 8, 1);
      return;
   }
   %a = brkAnchor(%cl.brkSeat);
   retroAttach(%cl, getWord(%a, 0), getWord(%a, 1), getWord(%a, 2));
   commandToClient(%cl, 'setHudMode', 'Standard');
   brkStartGame(%cl);
}

function brkFreeClient(%cl)
{
   retroDetach(%cl);
   if (isObject(%cl.brkGroup))
      %cl.brkGroup.delete();
   %cl.brkGroup = "";
   %cl.brkSeat = "";
   %cl.brkJoined = false;
}

// Switch the package on and off with the mission, so stock DM missions are untouched.
function brkWatch()
{
   cancel($Brk::WatchThread);
   $Brk::WatchThread = schedule(250, 0, brkWatch);
   %on = ($CurrentMission $= $Brk::Mission && isObject(MissionCleanup));
   if (%on && !$Brk::PkgOn)
   {
      activatePackage(T2LaunchBreakout);
      $Brk::PkgOn = true;
      $Brk::LastTick = "";
      brkTick();
      echo("Breakout ruleset on");
   }
   else if (!%on && $Brk::PkgOn)
   {
      deactivatePackage(T2LaunchBreakout);
      $Brk::PkgOn = false;
      cancel($Brk::TickThread);
      echo("Breakout ruleset off");
   }
}

package T2LaunchBreakoutBoot
{
   function CreateServer(%mission, %missionType)
   {
      Parent::CreateServer(%mission, %missionType);
      brkDatablocks();
   }
};

package T2LaunchBreakout
{
   function DMGame::equip(%game, %player)
   {
      // nobody carries anything
   }

   function DMGame::createPlayer(%game, %client, %spawnLoc, %respawn)
   {
      Parent::createPlayer(%game, %client, %spawnLoc, %respawn);
      brkInitPlayer(%client);
   }

   function DMGame::onClientKilled(%game, %clVictim, %clKiller, %damageType, %implement, %damageLoc)
   {
      // the stand-in Player is not part of the game: no respawn timer or orbit camera
      %clVictim.deaths++;
   }

   function DMGame::ObserverOnTrigger(%game, %data, %obj, %trigger, %state)
   {
      %client = %obj.getControllingClient();
      if (%client && %client.brkJoined)
      {
         if (%state && %trigger == 0 && %client.brkSeat !$= "")
            brkLaunch(%client);
         return false;
      }
      return Parent::ObserverOnTrigger(%game, %data, %obj, %trigger, %state);
   }

   // a seated player is a Player now, so its triggers arrive here: fire serves the ball
   function Armor::onTrigger(%data, %player, %trigger, %val)
   {
      %client = %player.getControllingClient();
      if (%client && %client.brkJoined)
      {
         if (%val && %trigger == 0 && %client.brkSeat !$= "")
            brkLaunch(%client);
         return;
      }
      Parent::onTrigger(%data, %player, %trigger, %val);
   }

   function DMGame::onClientLeaveGame(%game, %client)
   {
      brkFreeClient(%client);
      Parent::onClientLeaveGame(%game, %client);
   }
};

if (!$Brk::Installed)
{
   $Brk::Installed = true;
   activatePackage(T2LaunchBreakoutBoot);
   brkDatablocks();
   brkWatch();
   echo("Breakout gametype script loaded");
}
