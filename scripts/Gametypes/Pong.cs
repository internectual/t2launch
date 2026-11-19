// Pong: server-side ruleset for the MatrixPong mission.
// Evaluated in memory by the t2launch hook (server.gametype_script); stock clients download nothing.
// The package is active only while $CurrentMission is MatrixPong.
//
// Two paddles, a square ball, a dashed net and big seven-segment scores, all force-field blocks on an upright
// wall seen side-on (like Tetris.cs and Snake.cs). The first player to join takes the left paddle, the second
// the right; an empty side is played by the CPU (a beatable one). The ball speeds up with every hit and leaves
// a paddle at an angle set by where it hit; first to 11 wins and a new match starts shortly after.
// Controls (stock keybinds): forward and back move your paddle up and down. The client's control object is its
// Camera; its movement each tick is read as the d-pad. Anyone else can watch and fly around; a spectator takes
// a free paddle by pressing fire.

$Png::Mission = "MatrixPong";
$Png::HalfW = 24;                    // court half-width in metres
$Png::HalfH = 15;
$Png::CenterZ = 320;                 // world height of the court centre
$Png::Depth = 2;                     // thickness of everything, towards the viewer
$Png::PadX = 21.5;                   // paddle centre distance from the middle
$Png::PadW = 1;
$Png::PadH = 5;
$Png::PadSpeed = 28;                 // metres/second
$Png::CpuSpeed = 20;                 // the CPU paddle is slower than a person
$Png::CpuMiss = 2.4;                 // the CPU aims this far off the ball at most, re-rolled on every hit
$Png::BallSize = 1;
$Png::BallSpeed0 = 18;
$Png::BallSpeedMax = 44;
$Png::SpeedUp = 1.2;                 // added to the ball speed with every paddle hit
$Png::MaxAngle = 1.05;               // radians off straight, for a hit on the paddle's edge
$Png::WinScore = 11;
$Png::ServeMs = 1500;
$Png::MatchEndMs = 5000;
$Png::CamDist = 46;
$Png::CamZShift = -1.5;
$Png::KeyGraceMs = 110;
$Png::RecentreMs = 250;
$Png::IgnoreMs = 100;
$Png::TickMs = 32;
$Png::DigitW = 3;                    // seven-segment digit size
$Png::DigitH = 5;
$Png::SegT = 0.7;
$Png::ParkX = 3000;                  // unused blocks wait far outside every client's scope
$Png::ParkY = 3000;
$Png::Debug = 0;

// segments: a top, b top right, c bottom right, d bottom, e bottom left, f top left, g middle
$Png::Digit[0] = "abcdef";  $Png::Digit[1] = "bc";     $Png::Digit[2] = "abdeg";  $Png::Digit[3] = "abcdg";
$Png::Digit[4] = "bcfg";    $Png::Digit[5] = "acdfg";  $Png::Digit[6] = "acdefg"; $Png::Digit[7] = "abc";
$Png::Digit[8] = "abcdefg"; $Png::Digit[9] = "abcdfg";

// ---------------------------------------------------------------------------------------------------------
// datablocks: deleted by DestroyServer, so defined at install and again after every CreateServer

function pngDatablocks()
{
   if (isObject(PngLeft))
      return;
   eval("datablock ForceFieldBareData(PngLeft) : defaultForceFieldBare { color = \"0.10 0.85 0.95\"; baseTranslucency = 0.9; needsNoPower = true; };");
   eval("datablock ForceFieldBareData(PngRight) : defaultForceFieldBare { color = \"1.00 0.55 0.10\"; baseTranslucency = 0.9; needsNoPower = true; };");
   eval("datablock ForceFieldBareData(PngBall) : defaultForceFieldBare { color = \"1.00 1.00 1.00\"; baseTranslucency = 0.95; needsNoPower = true; };");
   eval("datablock ForceFieldBareData(PngNet) : defaultForceFieldBare { color = \"0.75 0.75 0.80\"; baseTranslucency = 0.6; needsNoPower = true; };");
   eval("datablock ForceFieldBareData(PngDigit) : defaultForceFieldBare { color = \"0.95 0.95 1.00\"; baseTranslucency = 0.9; needsNoPower = true; };");
}

function pngField(%db, %x, %z, %sx, %sz)
{
   return new ForceFieldBare() {
      dataBlock = %db;
      position = %x SPC (-$Png::Depth / 2) SPC %z;
      rotation = "1 0 0 0";
      scale = %sx SPC $Png::Depth SPC %sz;
   };
}

function pngPurgeZones()
{
   %z = nameToID("MissionCleanup/PZones");
   if (%z > 0)
      %z.delete();
}

// ---------------------------------------------------------------------------------------------------------
// the court

function pngPark(%obj)
{
   %obj.setTransform($Png::ParkX SPC $Png::ParkY SPC 300);
}

// court coordinates: x across, y up from the centre; a block's position is its lower-left corner
function pngPlace(%obj, %cx, %cy, %w, %h)
{
   %obj.setTransform((%cx - %w / 2) SPC (-$Png::Depth / 2) SPC ($Png::CenterZ + %cy - %h / 2));
}

function pngInitCourt()
{
   if (isObject($Png::Group))
      $Png::Group.delete();
   $Png::Group = new SimGroup();
   MissionCleanup.add($Png::Group);
   %g = $Png::Group;
   $Png::Pad[0] = pngField(PngLeft, 0, 0, $Png::PadW, $Png::PadH);
   $Png::Pad[1] = pngField(PngRight, 0, 0, $Png::PadW, $Png::PadH);
   $Png::Ball = pngField(PngBall, 0, 0, $Png::BallSize, $Png::BallSize);
   %g.add($Png::Pad[0]);
   %g.add($Png::Pad[1]);
   %g.add($Png::Ball);
   // top and bottom walls, and a dashed net
   %w = $Png::HalfW * 2;
   %g.add(pngField(PngNet, -$Png::HalfW, $Png::CenterZ + $Png::HalfH, %w, 0.6));
   %g.add(pngField(PngNet, -$Png::HalfW, $Png::CenterZ - $Png::HalfH - 0.6, %w, 0.6));
   for (%y = -$Png::HalfH + 1; %y < $Png::HalfH - 1; %y += 2.5)
      %g.add(pngField(PngNet, -0.25, $Png::CenterZ + %y, 0.5, 1.4));
   // seven-segment digits: 2 sides x 2 places x 7 segments, parked until used
   for (%side = 0; %side < 2; %side++)
      for (%pos = 0; %pos < 2; %pos++)
         for (%k = 0; %k < 7; %k++)
         {
            %seg = pngField(PngDigit, 0, 0, 1, 1);
            %g.add(%seg);
            pngPark(%seg);
            $Png::Seg[%side, %pos, %k] = %seg;
            $Png::SegOn[%side, %pos, %k] = false;
         }
   pngPurgeZones();
   $Png::Score[0] = 0;
   $Png::Score[1] = 0;
   $Png::PadY[0] = 0;
   $Png::PadY[1] = 0;
   $Png::Over = false;
   pngShowScores();
   pngPlaceAll();
   pngServe(getRandom() < 0.5 ? 0 : 1);
}

function pngPlaceAll()
{
   pngPlace($Png::Pad[0], -$Png::PadX, $Png::PadY[0], $Png::PadW, $Png::PadH);
   pngPlace($Png::Pad[1], $Png::PadX, $Png::PadY[1], $Png::PadW, $Png::PadH);
   pngPlace($Png::Ball, $Png::BX, $Png::BY, $Png::BallSize, $Png::BallSize);
}

// segment rectangle for letter %c in a digit whose centre is (%cx, %cy): "x y w h" (centre and size)
function pngSegRect(%c, %cx, %cy)
{
   %w = $Png::DigitW;
   %h = $Png::DigitH;
   %t = $Png::SegT;
   switch$ (%c)
   {
      case "a": return %cx SPC (%cy + %h / 2 - %t / 2) SPC %w SPC %t;
      case "b": return (%cx + %w / 2 - %t / 2) SPC (%cy + %h / 4) SPC %t SPC (%h / 2);
      case "c": return (%cx + %w / 2 - %t / 2) SPC (%cy - %h / 4) SPC %t SPC (%h / 2);
      case "d": return %cx SPC (%cy - %h / 2 + %t / 2) SPC %w SPC %t;
      case "e": return (%cx - %w / 2 + %t / 2) SPC (%cy - %h / 4) SPC %t SPC (%h / 2);
      case "f": return (%cx - %w / 2 + %t / 2) SPC (%cy + %h / 4) SPC %t SPC (%h / 2);
      default: return %cx SPC %cy SPC %w SPC %t;
   }
}

function pngShowDigit(%side, %pos, %value)
{
   // a tens digit only when it is needed
   %cx = (%side == 0 ? -9 : 9) + (%pos == 0 ? -2.6 : 2.6);
   %cy = $Png::HalfH - 4;
   %letters = %value $= "" ? "" : $Png::Digit[%value];
   %letterOrder = "abcdefg";
   for (%k = 0; %k < 7; %k++)
   {
      %letter = getSubStr(%letterOrder, %k, 1);
      %lit = %value !$= "" && strstr(%letters, %letter) >= 0;
      %seg = $Png::Seg[%side, %pos, %k];
      if (%lit && !$Png::SegOn[%side, %pos, %k])
      {
         %r = pngSegRect(%letter, %cx, %cy);
         %seg.setScale(getWord(%r, 2) SPC $Png::Depth SPC getWord(%r, 3));
         pngPlace(%seg, getWord(%r, 0), getWord(%r, 1), getWord(%r, 2), getWord(%r, 3));
         $Png::SegOn[%side, %pos, %k] = true;
      }
      else if (!%lit && $Png::SegOn[%side, %pos, %k])
      {
         pngPark(%seg);
         $Png::SegOn[%side, %pos, %k] = false;
      }
   }
}

function pngShowScores()
{
   for (%side = 0; %side < 2; %side++)
   {
      %s = $Png::Score[%side];
      pngShowDigit(%side, 0, %s >= 10 ? mFloor(%s / 10) : "");
      pngShowDigit(%side, 1, %s % 10);
   }
}

// ---------------------------------------------------------------------------------------------------------
// the game

// %toward: the side (0 left, 1 right) the ball is served to
function pngServe(%toward)
{
   $Png::BX = 0;
   $Png::BY = 0;
   $Png::Speed = $Png::BallSpeed0;
   %a = (getRandom() - 0.5) * 0.7;
   %sign = 1;
   if (%toward == 0)
      %sign = -1;
   $Png::BVX = $Png::Speed * mCos(%a) * %sign;
   $Png::BVY = $Png::Speed * mSin(%a);
   $Png::CpuAim = (getRandom() - 0.5) * 2 * $Png::CpuMiss;
   $Png::ServeAt = getSimTime() + $Png::ServeMs;
   $Png::Serving = true;
}

function pngPadHit(%side, %by)
{
   $Png::CpuAim = (getRandom() - 0.5) * 2 * $Png::CpuMiss;
   // where the ball hit decides the angle: the middle sends it straight back, the edge sharply
   %off = (%by - $Png::PadY[%side]) / ($Png::PadH / 2 + $Png::BallSize / 2);
   if (%off > 1) %off = 1;
   if (%off < -1) %off = -1;
   %a = %off * $Png::MaxAngle;
   $Png::Speed += $Png::SpeedUp;
   if ($Png::Speed > $Png::BallSpeedMax)
      $Png::Speed = $Png::BallSpeedMax;
   %sign = -1;
   if (%side == 0)
      %sign = 1;
   $Png::BVX = $Png::Speed * mCos(%a) * %sign;
   $Png::BVY = $Png::Speed * mSin(%a);
}

function pngScore(%side)
{
   $Png::Score[%side]++;
   pngShowScores();
   %w = $Png::WinScore;
   if ($Png::Score[%side] >= %w)
   {
      $Png::Over = true;
      $Png::Serving = false;
      $Png::BVX = 0;
      $Png::BVY = 0;
      $Png::BX = 0;
      $Png::BY = 0;
      messageAll('MsgPong', '\c2%1 wins %2 to %3.', pngSideName(%side), $Png::Score[%side], $Png::Score[1 - %side]);
      $Png::NextMatchAt = getSimTime() + $Png::MatchEndMs;
      pngHud();
      return;
   }
   // the side that was scored on receives the next serve
   pngServe(1 - %side);
   pngHud();
}

function pngSideName(%side)
{
   %cl = pngClientOnSide(%side);
   if (%cl)
      return %cl.name;
   return "the CPU";
}

function pngClientOnSide(%side)
{
   for (%i = 0; %i < ClientGroup.getCount(); %i++)
   {
      %c = ClientGroup.getObject(%i);
      if (%c.pngSide $= %side)
         return %c;
   }
   return 0;
}

function pngHud()
{
   %l = pngClientOnSide(0);
   %r = pngClientOnSide(1);
   %ln = "CPU";
   %rn = "CPU";
   if (%l)
      %ln = %l.nameBase;
   if (%r)
      %rn = %r.nameBase;
   %text = %ln @ "  " @ $Png::Score[0] @ " : " @ $Png::Score[1] @ "  " @ %rn @ "      forward / back move your paddle";
   for (%i = 0; %i < ClientGroup.getCount(); %i++)
   {
      %c = ClientGroup.getObject(%i);
      if (%c.pngJoined)
         bottomPrint(%c, %text, 0, 1);
   }
}

function pngStep(%dt, %now)
{
   // paddles
   for (%side = 0; %side < 2; %side++)
   {
      %cl = pngClientOnSide(%side);
      %dir = 0;
      if (%cl)
      {
         // plain assignments: a nested ternary that yields -1 misbehaves in arithmetic in this engine
         if (%cl.pngPad & 1)
            %dir = 1;
         else if (%cl.pngPad & 2)
            %dir = -1;
      }
      %speed = $Png::PadSpeed;
      if (!%cl)
      {
         // the CPU follows the ball while it is coming its way, and drifts back to the middle otherwise
         %coming = (%side == 0 && $Png::BVX < 0) || (%side == 1 && $Png::BVX > 0);
         %target = %coming ? $Png::BY + $Png::CpuAim : 0;
         %diff = %target - $Png::PadY[%side];
         if (%diff > 1.2)
            %dir = 1;
         else if (%diff < -1.2)
            %dir = -1;
         %speed = $Png::CpuSpeed;
      }
      %max = $Png::HalfH - $Png::PadH / 2 - 0.6;
      if ($Png::Debug)
         echo("PNGDBG side=" @ %side @ " dir=" @ %dir @ " speed=" @ %speed @ " dt=" @ %dt @ " before=" @ $Png::PadY[%side] @ " max=" @ %max);
      $Png::PadY[%side] += %dir * %speed * %dt;
      if ($Png::Debug)
         echo("PNGDBG side=" @ %side @ " after+=" @ $Png::PadY[%side]);
      if ($Png::PadY[%side] > %max) $Png::PadY[%side] = %max;
      if ($Png::PadY[%side] < -%max) $Png::PadY[%side] = -%max;
   }
   // the ball
   if ($Png::Over)
   {
      if (%now >= $Png::NextMatchAt)
      {
         $Png::Score[0] = 0;
         $Png::Score[1] = 0;
         $Png::Over = false;
         pngShowScores();
         pngServe(getRandom() < 0.5 ? 0 : 1);
         pngHud();
      }
   }
   else if ($Png::Serving)
   {
      if (%now >= $Png::ServeAt)
         $Png::Serving = false;
   }
   else
   {
      %steps = mCeil($Png::Speed * %dt / 0.4);
      if (%steps < 1) %steps = 1;
      %sdt = %dt / %steps;
      %half = $Png::BallSize / 2;
      for (%k = 0; %k < %steps; %k++)
      {
         $Png::BX += $Png::BVX * %sdt;
         $Png::BY += $Png::BVY * %sdt;
         %wall = $Png::HalfH - %half;
         if ($Png::BY > %wall) { $Png::BY = %wall; $Png::BVY = -$Png::BVY; }
         if ($Png::BY < -%wall) { $Png::BY = -%wall; $Png::BVY = -$Png::BVY; }
         // left paddle, then right paddle
         for (%side = 0; %side < 2; %side++)
         {
            %px = %side == 0 ? -$Png::PadX : $Png::PadX;
            %face = %side == 0 ? %px + $Png::PadW / 2 : %px - $Png::PadW / 2;
            %incoming = (%side == 0 && $Png::BVX < 0) || (%side == 1 && $Png::BVX > 0);
            if (!%incoming)
               continue;
            %reach = mAbs($Png::BX - %px) <= $Png::PadW / 2 + %half;
            %inz = mAbs($Png::BY - $Png::PadY[%side]) <= $Png::PadH / 2 + %half;
            if (%reach && %inz)
            {
               $Png::BX = %side == 0 ? %face + %half + 0.01 : %face - %half - 0.01;
               pngPadHit(%side, $Png::BY);
            }
         }
         if ($Png::BX < -$Png::HalfW) { pngScore(1); break; }
         if ($Png::BX > $Png::HalfW) { pngScore(0); break; }
      }
   }
   pngPlaceAll();
}

// ---------------------------------------------------------------------------------------------------------
// input: the player's keys, read by Retro.cs from its (tiny) velocity; 1 while forward is held, 2 while back is

function pngReadPad(%cl)
{
   return retroPad(%cl) & 3;
}

function pngAnchor()
{
   return "0" SPC (-$Png::CamDist) SPC ($Png::CenterZ + $Png::CamZShift) SPC "1 0 0 0";
}

function pngTick()
{
   cancel($Png::TickThread);
   $Png::TickThread = schedule($Png::TickMs, 0, pngTick);
   if (!isObject(MissionCleanup) || !isObject($Png::Group))
      return;
   $Png::TickN++;
   %now = getSimTime();
   %dt = $Png::LastTick $= "" ? 0.032 : (%now - $Png::LastTick) / 1000;
   $Png::LastTick = %now;
   if (%dt > 0.1) %dt = 0.1;
   for (%i = 0; %i < ClientGroup.getCount(); %i++)
   {
      %cl = ClientGroup.getObject(%i);
      if (!%cl.pngJoined || !isObject(%cl.camera))
         continue;
      if (%cl.pngSide $= "")
      {
         if (isObject(%cl.player))
         {
            %cl.player.setVelocity("0 0 0");
            if ($Png::TickN % 30 == 0)
               %cl.player.setTransform("0 -150 300 1 0 0 0");
         }
         if (%cl.getControlObject() != %cl.camera)
            %cl.setControlObject(%cl.camera);
         continue;
      }
      %cl.pngPad = pngReadPad(%cl);
   }
   pngStep(%dt, %now);
}

function pngFreeSide()
{
   for (%s = 0; %s < 2; %s++)
      if (!pngClientOnSide(%s))
         return %s;
   return "";
}

function pngInitPlayer(%cl)
{
   %cl.pngJoined = true;
   if (%cl.pngSide $= "")
      %cl.pngSide = pngFreeSide();
   if (%cl.pngSide $= "")
   {
      %cam = %cl.camera;
      %cam.setFlyMode();
      %cam.setTransform(pngAnchor());
      %cl.setControlObject(%cam);
      commandToClient(%cl, 'setHudMode', 'Observer');
      %cl.schedule(100, "setControlObject", %cam);
      if (isObject(%cl.player))
      {
         %cl.player.setTransform("0 -150 300 1 0 0 0");
         %cl.player.setVelocity("0 0 0");
      }
      bottomPrint(%cl, "Watching. Both paddles are taken; press fire when one frees up, and you can fly around freely.", 8, 1);
      return;
   }
   retroAttach(%cl, 0, -$Png::CamDist, $Png::CenterZ + $Png::CamZShift);
   commandToClient(%cl, 'setHudMode', 'Standard');
   pngHud();
}

function pngFreeClient(%cl)
{
   retroDetach(%cl);
   %cl.pngSide = "";
   %cl.pngJoined = false;
   %cl.pngPad = 0;
}

// Switch the package on and off with the mission, so stock DM missions are untouched.
function pngWatch()
{
   cancel($Png::WatchThread);
   $Png::WatchThread = schedule(250, 0, pngWatch);
   %on = ($CurrentMission $= $Png::Mission && isObject(MissionCleanup));
   if (%on && !$Png::PkgOn)
   {
      activatePackage(T2LaunchPong);
      $Png::PkgOn = true;
      $Png::LastTick = "";
      pngInitCourt();
      pngTick();
      echo("Pong ruleset on");
   }
   else if (!%on && $Png::PkgOn)
   {
      deactivatePackage(T2LaunchPong);
      $Png::PkgOn = false;
      cancel($Png::TickThread);
      echo("Pong ruleset off");
   }
}

package T2LaunchPongBoot
{
   function CreateServer(%mission, %missionType)
   {
      Parent::CreateServer(%mission, %missionType);
      pngDatablocks();
   }
};

package T2LaunchPong
{
   function DMGame::equip(%game, %player)
   {
      // nobody carries anything
   }

   function DMGame::createPlayer(%game, %client, %spawnLoc, %respawn)
   {
      Parent::createPlayer(%game, %client, %spawnLoc, %respawn);
      pngInitPlayer(%client);
   }

   function DMGame::onClientKilled(%game, %clVictim, %clKiller, %damageType, %implement, %damageLoc)
   {
      // the stand-in Player is not part of the game: no respawn timer or orbit camera
      %clVictim.deaths++;
   }

   function DMGame::ObserverOnTrigger(%game, %data, %obj, %trigger, %state)
   {
      %client = %obj.getControllingClient();
      if (%client && %client.pngJoined)
      {
         // a spectator takes a free paddle with fire
         if (%client.pngSide $= "" && %state && %trigger == 0)
         {
            %side = pngFreeSide();
            if (%side !$= "")
            {
               %client.pngSide = %side;
               pngInitPlayer(%client);
            }
         }
         return false;
      }
      return Parent::ObserverOnTrigger(%game, %data, %obj, %trigger, %state);
   }

   function Armor::onTrigger(%data, %player, %trigger, %val)
   {
      %client = %player.getControllingClient();
      if (%client && %client.pngJoined)
         return;
      Parent::onTrigger(%data, %player, %trigger, %val);
   }

   function DMGame::onClientLeaveGame(%game, %client)
   {
      pngFreeClient(%client);
      Parent::onClientLeaveGame(%game, %client);
      pngHud();
   }
};

if (!$Png::Installed)
{
   $Png::Installed = true;
   activatePackage(T2LaunchPongBoot);
   pngDatablocks();
   pngWatch();
   echo("Pong gametype script loaded");
}
