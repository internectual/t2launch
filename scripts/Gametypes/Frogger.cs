// Frogger: server-side ruleset for the MatrixFrogger mission.
// Evaluated in memory by the t2launch hook (server.gametype_script, after Retro.cs); stock clients download nothing.
// The package is active only while $CurrentMission is MatrixFrogger.
//
// Rules: hop from the bottom lawn to one of the five bays at the top. Five lanes of traffic first (touch a vehicle
// and you are flattened), then a median, then five lanes of river: logs and turtles carry you sideways, water or being
// carried off the edge drowns you. Land in an empty bay for 50; each new row forward scores 10; filling all five bays
// scores 1000 and starts a faster round. 40 seconds per life, three lives. Each player gets their own board.
// Controls (stock keybinds): a key press is one hop (forward / back / strafe left / strafe right); holding a key does
// not repeat. Input is read by Retro.cs.

$Frg::Mission = "MatrixFrogger";
$Frg::HalfW = 15;                    // board half-width in metres
$Frg::Cell = 2;                      // metres per row / hop
$Frg::Rows = 13;                     // 0 lawn, 1-5 road, 6 median, 7-11 river, 12 bays
$Frg::Z0 = 300;                      // world height of the board floor
$Frg::Spacing = 40;                  // between boards
$Frg::Depth = 2;
$Frg::Seats = 4;
$Frg::SeatOffset[0] = 0;  $Frg::SeatOffset[1] = 1;  $Frg::SeatOffset[2] = -1; $Frg::SeatOffset[3] = 2;
$Frg::Period = 36;                   // lane loop length in metres (wider than the board, so items enter and leave unseen)
$Frg::FrogSize = 1.4;
$Frg::LifeSecs = 40;
$Frg::Lives = 3;
$Frg::RespawnMs = 900;
$Frg::GameOverMs = 4500;
$Frg::CamDist = 44;
$Frg::CamZShift = -1;
$Frg::TickMs = 32;
$Frg::ParkX = 3000;                  // inactive blocks wait far outside every client's scope
$Frg::ParkY = 3000;
$Frg::BandY = 4;                     // the lane backgrounds sit well behind the moving blocks: a force field draws white where
                                     // it is close in depth to another one behind it
$Frg::BandK = 1.12;                  // so the backgrounds are scaled up about the view centre to line up with the rows
$Frg::Debug = 0;

// lanes: direction, speed (m/s), items, length (cells), block datablock
function frgTables()
{
   frgLane(1, 1, 5, 3, 1, "FrgCarA");
   frgLane(2, -1, 7, 3, 1, "FrgCarB");
   frgLane(3, 1, 4, 2, 2, "FrgTruck");
   frgLane(4, -1, 10, 2, 1, "FrgCarC");
   frgLane(5, 1, 7, 3, 1.5, "FrgCarD");
   frgLane(7, -1, 5, 3, 3, "FrgLog");
   frgLane(8, 1, 7, 3, 2, "FrgTurtle");
   frgLane(9, -1, 8, 2, 4, "FrgLog");
   frgLane(10, 1, 5, 3, 2, "FrgTurtle");
   frgLane(11, -1, 6, 3, 3, "FrgLog");
}

function frgLane(%lane, %dir, %speed, %n, %len, %db)
{
   $Frg::LDir[%lane] = %dir;
   $Frg::LSpeed[%lane] = %speed;
   $Frg::LN[%lane] = %n;
   $Frg::LLen[%lane] = %len * $Frg::Cell - 0.3;
   $Frg::LDb[%lane] = %db;
}

function frgIsRiver(%row)
{
   return %row >= 7 && %row <= 11;
}

function frgIsRoad(%row)
{
   return %row >= 1 && %row <= 5;
}

// ---------------------------------------------------------------------------------------------------------
// datablocks: deleted by DestroyServer, so defined at install and again after every CreateServer

function frgDatablocks()
{
   if (isObject(FrgFrog))
      return;
   %defs = "FrgFrog|0.3 1.0 0.3;FrgCarA|1.0 0.3 0.3;FrgCarB|1.0 0.9 0.2;FrgCarC|0.9 0.4 1.0;FrgCarD|1.0 0.6 0.2;"
         @ "FrgTruck|0.9 0.9 0.95;FrgLog|0.65 0.4 0.15;FrgTurtle|0.2 0.8 0.5;FrgBay|1.0 1.0 0.6;"
         @ "FrgGrass|0.2 0.7 0.2;FrgRoad|0.35 0.35 0.4;FrgWater|0.15 0.3 1.0;FrgDark|0.05 0.05 0.1";
   for (%i = 0; %i < getFieldCount(strreplace(%defs, ";", "\t")); %i++)
   {
      %def = getField(strreplace(%defs, ";", "\t"), %i);
      %name = getSubStr(%def, 0, strstr(%def, "|"));
      %color = getSubStr(%def, strstr(%def, "|") + 1, 64);
      %alpha = 0.9;
      if (%name $= "FrgGrass" || %name $= "FrgRoad" || %name $= "FrgWater")
         %alpha = 0.3;
      eval("datablock ForceFieldBareData(" @ %name @ ") : defaultForceFieldBare { framesPerSec = 1; numFrames = 1; scrollSpeed = 0; color = \"" @ %color
         @ "\"; baseTranslucency = " @ %alpha @ "; needsNoPower = true; };");
   }
}

function frgField(%db, %x, %y, %z, %sx, %sy, %sz)
{
   return new ForceFieldBare() {
      dataBlock = %db;
      position = %x SPC %y SPC %z;
      rotation = "1 0 0 0";
      scale = %sx SPC %sy SPC %sz;
   };
}

function frgPurgeZones()
{
   %z = nameToID("MissionCleanup/PZones");
   if (%z > 0)
      %z.delete();
}

// ---------------------------------------------------------------------------------------------------------
// geometry: (u, v) = across from the centre line, and up from the floor; row r is centred at v = r * cell + cell / 2

function frgBoardX(%seat)
{
   return $Frg::SeatOffset[%seat] * $Frg::Spacing;
}

function frgAnchor(%seat)
{
   return frgBoardX(%seat) SPC (-$Frg::CamDist) SPC ($Frg::Z0 + $Frg::Rows * $Frg::Cell / 2 + $Frg::CamZShift) SPC "1 0 0 0";
}

function frgRowV(%row)
{
   return %row * $Frg::Cell + $Frg::Cell / 2;
}

// a block's position is its lower-left corner; (%u, %v) is its centre
function frgPlace(%obj, %seat, %u, %v, %w, %h)
{
   %obj.setTransform((frgBoardX(%seat) + %u - %w / 2) SPC (-$Frg::Depth / 2) SPC ($Frg::Z0 + %v - %h / 2));
}

function frgPark(%obj)
{
   %obj.setTransform($Frg::ParkX SPC $Frg::ParkY SPC 300);
}

function frgFreeSeat()
{
   for (%s = 0; %s < $Frg::Seats; %s++)
   {
      %used = false;
      for (%i = 0; %i < ClientGroup.getCount(); %i++)
         if (ClientGroup.getObject(%i).frgSeat $= %s)
            %used = true;
      if (!%used)
         return %s;
   }
   return "";
}

// the centre of item %i of a lane
function frgItemU(%seat, %lane, %i)
{
   %p = $Frg::Period;
   %x = $Frg::Ph[%seat, %lane] + %i * %p / $Frg::LN[%lane];
   %x = %x - %p * mFloor(%x / %p);
   return %x - %p / 2;
}

// ---------------------------------------------------------------------------------------------------------
// one board

// a background strip over rows %row0..%row1, drawn behind the blocks and scaled to match the perspective
function frgBand(%g, %seat, %db, %row0, %row1)
{
   %k = $Frg::BandK;
   %vc = $Frg::Rows * $Frg::Cell / 2 - 1;                 // the camera's height above the floor
   %v0 = %row0 * $Frg::Cell;
   %v1 = (%row1 + 1) * $Frg::Cell;
   %w = $Frg::HalfW * 2 * %k;
   %z = %vc + (%v0 - %vc) * %k;
   %h = (%v1 - %v0) * %k;
   %o = frgField(%db, frgBoardX(%seat) - %w / 2, $Frg::BandY, $Frg::Z0 + %z, %w, 0.5, %h);
   %g.add(%o);
}

// a marker at (%u, row 12) of size (%w, %h), on the same background plane
function frgBayMarker(%g, %seat, %u, %w)
{
   %k = $Frg::BandK;
   %vc = $Frg::Rows * $Frg::Cell / 2 - 1;
   %v0 = 12 * $Frg::Cell + 0.1;
   %h = ($Frg::Cell - 0.2) * %k;
   %z = %vc + (%v0 - %vc) * %k;
   %o = frgField(FrgDark, frgBoardX(%seat) + (%u - %w / 2) * %k, $Frg::BandY - 0.3, $Frg::Z0 + %z, %w * %k, 0.5, %h);
   %g.add(%o);
}

function frgMakeBoard(%cl)
{
   %seat = %cl.frgSeat;
   %g = new SimGroup();
   MissionCleanup.add(%g);
   %cl.frgGroup = %g;
   frgBand(%g, %seat, FrgGrass, 0, 0);
   frgBand(%g, %seat, FrgRoad, 1, 5);
   frgBand(%g, %seat, FrgGrass, 6, 6);
   frgBand(%g, %seat, FrgWater, 7, 11);
   frgBand(%g, %seat, FrgWater, 12, 12);
   // the five bays: a dark marker each, and a frog block that shows once it is filled
   for (%b = 0; %b < 5; %b++)
   {
      %u = (%b - 2) * 6;
      frgBayMarker(%g, %seat, %u, 2.6);
      %f = frgField(FrgFrog, 0, -$Frg::Depth / 2, 0, 1.6, $Frg::Depth, 1.6);
      %g.add(%f);
      frgPark(%f);
      $Frg::BayObj[%seat, %b] = %f;
      $Frg::BayOn[%seat, %b] = false;
   }
   // the traffic and the logs
   for (%lane = 1; %lane <= 11; %lane++)
   {
      if (%lane == 6)
         continue;
      $Frg::Ph[%seat, %lane] = 0;
      for (%i = 0; %i < $Frg::LN[%lane]; %i++)
      {
         %o = frgField($Frg::LDb[%lane], 0, -$Frg::Depth / 2, 0, $Frg::LLen[%lane], $Frg::Depth, $Frg::Cell - 0.4);
         %g.add(%o);
         frgPark(%o);
         $Frg::Obj[%seat, %lane * 10 + %i] = %o;
      }
   }
   %cl.frgFrog = frgField(FrgFrog, 0, -$Frg::Depth / 2, 0, $Frg::FrogSize, $Frg::Depth, $Frg::FrogSize);
   %g.add(%cl.frgFrog);
   frgPark(%cl.frgFrog);
   frgPurgeZones();
}

// ---------------------------------------------------------------------------------------------------------
// the game

function frgStartGame(%cl)
{
   %cl.frgScore = 0;
   %cl.frgLives = $Frg::Lives;
   %cl.frgLevel = 1;
   frgNewRound(%cl);
}

function frgNewRound(%cl)
{
   %seat = %cl.frgSeat;
   for (%b = 0; %b < 5; %b++)
   {
      $Frg::BayOn[%seat, %b] = false;
      frgPark($Frg::BayObj[%seat, %b]);
   }
   %cl.frgBays = 0;
   frgSpawn(%cl);
   %cl.frgState = "play";
   frgHud(%cl);
}

function frgSpawn(%cl)
{
   %cl.frgU = 0;
   %cl.frgRow = 0;
   %cl.frgBest = 0;
   %cl.frgDeadline = getSimTime() + $Frg::LifeSecs * 1000;
   %cl.retroLastMask = 0;
}

function frgHud(%cl)
{
   if (%cl.frgState $= "over")
   {
      bottomPrint(%cl, "GAME OVER    SCORE " @ %cl.frgScore, 0, 1);
      return;
   }
   %left = mCeil((%cl.frgDeadline - getSimTime()) / 1000);
   if (%left < 0)
      %left = 0;
   bottomPrint(%cl, "SCORE " @ %cl.frgScore @ "    LIVES " @ %cl.frgLives @ "    ROUND " @ %cl.frgLevel @ "    TIME " @ %left @ "     press a key = one hop", 0, 1);
}

function frgDie(%cl)
{
   if (%cl.frgState !$= "play")
      return;
   %cl.frgLives--;
   if (%cl.frgLives <= 0)
   {
      %cl.frgState = "over";
      %cl.frgOverAt = getSimTime() + $Frg::GameOverMs;
      if (%cl.frgScore > %cl.frgHigh)
         %cl.frgHigh = %cl.frgScore;
      messageAll('MsgFrogger', '\c1%1 is done (%2 points).', %cl.name, %cl.frgScore);
   }
   else
   {
      %cl.frgState = "dead";
      %cl.frgRespawnAt = getSimTime() + $Frg::RespawnMs;
   }
   frgPark(%cl.frgFrog);
   frgHud(%cl);
}

// the bay (0..4) a frog at %u lands in, or -1
function frgBayAt(%u)
{
   for (%b = 0; %b < 5; %b++)
      if (mAbs(%u - (%b - 2) * 6) <= 1.4)
         return %b;
   return -1;
}

function frgHop(%cl, %dr, %du)
{
   %row = %cl.frgRow + %dr;
   if (%row < 0 || %row >= $Frg::Rows)
      return;
   %u = %cl.frgU + %du;
   if (mAbs(%u) > $Frg::HalfW - 1)
      return;
   %cl.frgRow = %row;
   %cl.frgU = %u;
   if (%row == 12)
   {
      %b = frgBayAt(%u);
      if (%b < 0 || $Frg::BayOn[%cl.frgSeat, %b])
      {
         frgDie(%cl);
         return;
      }
      $Frg::BayOn[%cl.frgSeat, %b] = true;
      frgPlace($Frg::BayObj[%cl.frgSeat, %b], %cl.frgSeat, (%b - 2) * 6, frgRowV(12), 1.6, 1.6);
      %cl.frgScore += 50;
      %cl.frgBays++;
      if (%cl.frgBays >= 5)
      {
         %cl.frgScore += 1000;
         %cl.frgLevel++;
         frgNewRound(%cl);
         return;
      }
      frgSpawn(%cl);
      frgHud(%cl);
      return;
   }
   if (%row > %cl.frgBest)
   {
      %cl.frgBest = %row;
      %cl.frgScore += 10;
      frgHud(%cl);
   }
}

// vehicles and water for the frog's current row
function frgCheckFrog(%cl, %dt)
{
   %seat = %cl.frgSeat;
   %row = %cl.frgRow;
   if (frgIsRoad(%row))
   {
      %half = $Frg::LLen[%row] / 2 + $Frg::FrogSize / 2 - 0.35;
      for (%i = 0; %i < $Frg::LN[%row]; %i++)
         if (mAbs(%cl.frgU - frgItemU(%seat, %row, %i)) < %half)
         {
            frgDie(%cl);
            return;
         }
   }
   else if (frgIsRiver(%row))
   {
      %on = false;
      for (%i = 0; %i < $Frg::LN[%row]; %i++)
         if (mAbs(%cl.frgU - frgItemU(%seat, %row, %i)) <= $Frg::LLen[%row] / 2)
            %on = true;
      if (!%on)
      {
         frgDie(%cl);
         return;
      }
      %cl.frgU += $Frg::LDir[%row] * $Frg::LSpeed[%row] * %cl.frgSpeed * %dt;
      if (mAbs(%cl.frgU) > $Frg::HalfW)
         frgDie(%cl);
   }
}

function frgDrawItems(%cl)
{
   %seat = %cl.frgSeat;
   for (%lane = 1; %lane <= 11; %lane++)
   {
      if (%lane == 6)
         continue;
      %len = $Frg::LLen[%lane];
      for (%i = 0; %i < $Frg::LN[%lane]; %i++)
      {
         %u = frgItemU(%seat, %lane, %i);
         %o = $Frg::Obj[%seat, %lane * 10 + %i];
         if (mAbs(%u) > $Frg::HalfW + 0.5)
            frgPark(%o);
         else
            frgPlace(%o, %seat, %u, frgRowV(%lane), %len, $Frg::Cell - 0.4);
      }
   }
}

function frgPhysics(%cl, %dt, %now)
{
   %seat = %cl.frgSeat;
   %cl.frgSpeed = 1 + (%cl.frgLevel - 1) * 0.15;
   if (%cl.frgSpeed > 2)
      %cl.frgSpeed = 2;
   // the lanes keep moving in every state
   for (%lane = 1; %lane <= 11; %lane++)
      if (%lane != 6)
         $Frg::Ph[%seat, %lane] += $Frg::LDir[%lane] * $Frg::LSpeed[%lane] * %cl.frgSpeed * %dt;
   if ($Frg::TickN % 2 == 0)
      frgDrawItems(%cl);
   if (%cl.frgState $= "over")
   {
      if (%now >= %cl.frgOverAt)
         frgStartGame(%cl);
      return;
   }
   if (%cl.frgState $= "dead")
   {
      if (%now >= %cl.frgRespawnAt)
      {
         frgSpawn(%cl);
         %cl.frgState = "play";
         frgHud(%cl);
      }
      return;
   }
   // one hop per key press
   %new = retroPressed(%cl, %cl.frgPadMask);
   if (%new & 1) frgHop(%cl, 1, 0);
   else if (%new & 2) frgHop(%cl, -1, 0);
   else if (%new & 4) frgHop(%cl, 0, -$Frg::Cell);
   else if (%new & 8) frgHop(%cl, 0, $Frg::Cell);
   if (%cl.frgState !$= "play")
      return;
   frgCheckFrog(%cl, %dt);
   if (%cl.frgState !$= "play")
      return;
   if (%now >= %cl.frgDeadline)
   {
      frgDie(%cl);
      return;
   }
   if ($Frg::TickN % 15 == 0)
      frgHud(%cl);
   frgPlace(%cl.frgFrog, %seat, %cl.frgU, frgRowV(%cl.frgRow), $Frg::FrogSize, $Frg::FrogSize);
}

// ---------------------------------------------------------------------------------------------------------
// input: the player's keys, read by Retro.cs from its (tiny) velocity

function frgTick()
{
   cancel($Frg::TickThread);
   $Frg::TickThread = schedule($Frg::TickMs, 0, frgTick);
   if (!isObject(MissionCleanup))
      return;
   $Frg::TickN++;
   %now = getSimTime();
   %dt = $Frg::LastTick $= "" ? 0.032 : (%now - $Frg::LastTick) / 1000;
   $Frg::LastTick = %now;
   if (%dt > 0.1) %dt = 0.1;
   for (%i = 0; %i < ClientGroup.getCount(); %i++)
   {
      %cl = ClientGroup.getObject(%i);
      if (!%cl.frgJoined || !isObject(%cl.camera))
         continue;
      if (%cl.frgSeat $= "")
      {
         // spectators have the stock free-fly camera; the stand-in Player is parked out of the way
         if (isObject(%cl.player))
         {
            %cl.player.setVelocity("0 0 0");
            if ($Frg::TickN % 30 == 0)
               %cl.player.setTransform("0 -150 300 1 0 0 0");
         }
         if (%cl.getControlObject() != %cl.camera)
            %cl.setControlObject(%cl.camera);
         continue;
      }
      %cl.frgPadMask = retroPad(%cl);
      frgPhysics(%cl, %dt, %now);
   }
}

function frgInitPlayer(%cl)
{
   %cl.frgJoined = true;
   if (%cl.frgSeat $= "")
   {
      %seat = frgFreeSeat();
      if (%seat !$= "")
      {
         %cl.frgSeat = %seat;
         frgMakeBoard(%cl);
      }
   }
   if (%cl.frgSeat $= "")
   {
      // a spectator: the free-fly camera, parked player
      %cam = %cl.camera;
      %cam.setFlyMode();
      %cam.setTransform(frgAnchor(0));
      %cl.setControlObject(%cam);
      commandToClient(%cl, 'setHudMode', 'Observer');
      %cl.schedule(100, "setControlObject", %cam);
      if (isObject(%cl.player))
      {
         %cl.player.setTransform("0 -150 300 1 0 0 0");
         %cl.player.setVelocity("0 0 0");
      }
      bottomPrint(%cl, "Watching: every board is taken.", 8, 1);
      return;
   }
   %a = frgAnchor(%cl.frgSeat);
   retroAttach(%cl, getWord(%a, 0), getWord(%a, 1), getWord(%a, 2));
   commandToClient(%cl, 'setHudMode', 'Standard');
   frgStartGame(%cl);
}

function frgFreeClient(%cl)
{
   retroDetach(%cl);
   if (isObject(%cl.frgGroup))
      %cl.frgGroup.delete();
   %cl.frgGroup = "";
   %cl.frgSeat = "";
   %cl.frgJoined = false;
}

// Switch the package on and off with the mission, so stock DM missions are untouched.
function frgWatch()
{
   cancel($Frg::WatchThread);
   $Frg::WatchThread = schedule(250, 0, frgWatch);
   %on = ($CurrentMission $= $Frg::Mission && isObject(MissionCleanup));
   if (%on && !$Frg::PkgOn)
   {
      activatePackage(T2LaunchFrogger);
      $Frg::PkgOn = true;
      $Frg::LastTick = "";
      frgTick();
      echo("Frogger ruleset on");
   }
   else if (!%on && $Frg::PkgOn)
   {
      deactivatePackage(T2LaunchFrogger);
      $Frg::PkgOn = false;
      cancel($Frg::TickThread);
      echo("Frogger ruleset off");
   }
}

package T2LaunchFroggerBoot
{
   function CreateServer(%mission, %missionType)
   {
      Parent::CreateServer(%mission, %missionType);
      frgDatablocks();
   }
};

package T2LaunchFrogger
{
   function DMGame::equip(%game, %player)
   {
      // nobody carries anything
   }

   function DMGame::createPlayer(%game, %client, %spawnLoc, %respawn)
   {
      Parent::createPlayer(%game, %client, %spawnLoc, %respawn);
      frgInitPlayer(%client);
   }

   function DMGame::onClientKilled(%game, %clVictim, %clKiller, %damageType, %implement, %damageLoc)
   {
      // the stand-in Player is not part of the game: no respawn timer or orbit camera
      %clVictim.deaths++;
   }

   function DMGame::ObserverOnTrigger(%game, %data, %obj, %trigger, %state)
   {
      %client = %obj.getControllingClient();
      if (%client && %client.frgJoined)
         return false;
      return Parent::ObserverOnTrigger(%game, %data, %obj, %trigger, %state);
   }

   // a seated player is a Player now: swallow its triggers (nothing is carried)
   function Armor::onTrigger(%data, %player, %trigger, %val)
   {
      %client = %player.getControllingClient();
      if (%client && %client.frgJoined)
         return;
      Parent::onTrigger(%data, %player, %trigger, %val);
   }

   function DMGame::onClientLeaveGame(%game, %client)
   {
      frgFreeClient(%client);
      Parent::onClientLeaveGame(%game, %client);
   }
};

if (!$Frg::Installed)
{
   $Frg::Installed = true;
   frgTables();
   activatePackage(T2LaunchFroggerBoot);
   frgDatablocks();
   frgWatch();
   echo("Frogger gametype script loaded");
}
