// Donkey: server-side ruleset for the MatrixDonkey mission (a Donkey Kong clone), built from real game models: cargo
// containers for the girders, poles for the ladders, a heavy Bioderm carrying a mortar as the ape, a tiny human as Mario, a
// human lady, and round grenade shells with a fire on them for the barrels. The girders, ladders and oil drum are TSStatic
// shapes in the mission file (scripts/Matrix/tools/make_donkey_mis.py writes it). The people are standalone players of the
// stock armors. The barrels are static shapes of a shape datablock defined here (DkBomb, the stock grenade shell), which
// needs retroRefreshCrc() from Retro.cs: the server's CRC challenge is computed before the script runs, so without the
// refresh every client is dropped a few seconds after joining. A particle emitter cannot be moved once a client has it, so
// each barrel's fire is a short-lived emitter created on the barrel every few tens of milliseconds. A pale force-field
// backdrop makes the dark models stand out. Mario's walk cycle is chosen here, because nothing is solid under a standalone
// player and the engine would never pick one.
// Evaluated in memory by the t2launch hook (server.gametype_script, after Retro.cs); clients download nothing.
// The package is active only while $CurrentMission is MatrixDonkey.
//
// Rules: climb six girders to the top, where Pauline waits, while the ape at the top rolls barrels down at you. Barrels
// roll along a girder, drop off its end onto the next one, and sometimes take a ladder. Jump a barrel for 100; touch one
// and you lose a life. Reaching Pauline scores 1000 plus a bonus that shrinks while you climb, then the barrels come
// faster. Three lives. Every player gets their own board on an upright wall, seen side-on.
// Not included: the hammer, fireballs, the other three stages.
// Controls (stock keybinds): strafe walks, forward / back climbs a ladder you are standing at, jump or fire jumps.
// Input is read by Retro.cs.

$Dk::Mission = "MatrixDonkey";
$Dk::HalfW = 15;                     // board half-width in metres
$Dk::Height = 32;
$Dk::Z0 = 300;                       // world height of the board floor
$Dk::Spacing = 40;                   // between boards
$Dk::Depth = 2;
$Dk::Seats = 4;
$Dk::SeatOffset[0] = 0;  $Dk::SeatOffset[1] = 1;  $Dk::SeatOffset[2] = -1; $Dk::SeatOffset[3] = 2;
$Dk::Girders = 7;                    // 0 bottom .. 5 the ape's, 6 Pauline's
$Dk::GirderGap = 4.6;                // height between girders
$Dk::GirderV0 = 1.0;                 // surface of the bottom girder
$Dk::Thick = 0.6;
$Dk::ManSize = 1.4;
$Dk::WalkSpeed = 6;
$Dk::ClimbSpeed = 4;
$Dk::JumpSecs = 0.7;
$Dk::JumpHeight = 2.6;
$Dk::BarrelSize = 1.2;
$Dk::BarrelScale = 5;                   // the grenade shell is a few centimetres across
$Dk::BarrelDrop = 0;                 // from the shell's origin to its middle
$Dk::BarrelSpeed = 5.5;
$Dk::BarrelFall = 22;                // gravity for a falling barrel
$Dk::MaxBarrels = 7;
$Dk::SparkMs = 40;                    // a new flame puff behind each barrel this often
$Dk::SparkLifeMs = 60;               // and it burns this long
$Dk::LadderChance = 0.22;
$Dk::Lives = 3;
$Dk::RespawnMs = 1500;
$Dk::GameOverMs = 4500;
$Dk::WinMs = 2500;
$Dk::CamDist = 46;
$Dk::CamZShift = -0.5;
$Dk::LampRadius = 45;                // metres: the lamps in front of the board
$Dk::LadderY = 1.6;                  // the ladders stand just behind the walkway
$Dk::ManScale = 0.56;                // a human is 2.5 m tall
$Dk::ApeScale = 1.25;                // the heavy Bioderm is 3 m tall
$Dk::BackY = 4;                      // the backdrop sits well behind everything: a force field draws white where it is close in depth
                                     // to another one behind it
$Dk::TickMs = 32;
$Dk::ParkX = 3000;                   // inactive blocks wait far outside every client's scope
$Dk::ParkY = 3000;

// the girders (left end, right end) and the ladders (lower girder, position)
function dkTables()
{
   $Dk::BackK = ($Dk::CamDist + $Dk::BackY + 0.25) / ($Dk::CamDist - 1);     // scales the backdrop up to line up with the board
   dkGirder(0, -15, 15);
   dkGirder(1, -15, 11);
   dkGirder(2, -11, 15);
   dkGirder(3, -15, 11);
   dkGirder(4, -11, 15);
   dkGirder(5, -15, 11);
   dkGirder(6, -6, 2);
   $Dk::Ladders = 0;
   dkLadder(0, -9);  dkLadder(0, 4);
   dkLadder(1, -6);  dkLadder(1, 8);
   dkLadder(2, -7);  dkLadder(2, 4);
   dkLadder(3, -4);  dkLadder(3, 9);
   dkLadder(4, -8);  dkLadder(4, 6);
   dkLadder(5, -1);
}

function dkGirder(%k, %x0, %x1)
{
   $Dk::PX0[%k] = %x0;
   $Dk::PX1[%k] = %x1;
   $Dk::PV[%k] = $Dk::GirderV0 + %k * $Dk::GirderGap;
}

function dkLadder(%k, %u)
{
   $Dk::LK[$Dk::Ladders] = %k;
   $Dk::LU[$Dk::Ladders] = %u;
   $Dk::Ladders++;
}

// the ladder at girder %k (going up) that is within %tol of %u, or -1
function dkLadderUp(%k, %u, %tol)
{
   for (%j = 0; %j < $Dk::Ladders; %j++)
      if ($Dk::LK[%j] == %k && mAbs($Dk::LU[%j] - %u) <= %tol)
         return %j;
   return -1;
}

// ---------------------------------------------------------------------------------------------------------
// datablocks: deleted by DestroyServer, so defined at install and again after every CreateServer

// a stock shape at a world position
// a player model standing still: %armor on %x %y %z, scaled, facing along %yaw
// Force-field datablocks (no shape file, so they are safe to define from a script): a pale backdrop so the dark models stand
// out, and an orange glow that rides behind each barrel
function dkDatablocks()
{
   if (isObject(DkBack))
      return;
   // the barrels are the grenade shell, a round dot from any side (an item cannot be tipped over, a static shape can); a new shape datablock needs
   // the server's CRC challenge recomputed or every client is dropped a few seconds later
   eval("datablock StaticShapeData(DkBomb) { shapeFile = \"grenade_projectile.dts\"; };");
   retroRefreshCrc();
   // a short, dense flame: the stock debris fire, living a third as long and ejecting twice as often
   eval("datablock ParticleData(DkFlame) : DebrisFireParticle { lifetimeMS = 100; sizes[0] = 0.6; sizes[1] = 1.1; sizes[2] = 0.6; };");
   eval("datablock ParticleEmitterData(DkFlameEmitter) : DebrisFireEmitter { ejectionPeriodMS = 8; particles = \"DkFlame\"; };");
   eval("datablock ForceFieldBareData(DkBack) : defaultForceFieldBare { framesPerSec = 1; numFrames = 1; scrollSpeed = 0; color = \"0.72 0.8 0.95\"; baseTranslucency = 0.22;"
      @ " needsNoPower = true; };");
}

function dkField(%db, %x, %y, %z, %sx, %sy, %sz)
{
   return new ForceFieldBare() {
      dataBlock = %db;
      position = %x SPC %y SPC %z;
      rotation = "1 0 0 0";
      scale = %sx SPC %sy SPC %sz;
   };
}

function dkPurgeZones()
{
   %z = nameToID("MissionCleanup/PZones");
   if (%z > 0)
      %z.delete();
}

function dkActor(%armor, %scale, %x, %y, %z, %yaw)
{
   %p = new Player() {
      dataBlock = %armor;
   };
   %p.setScale(%scale SPC %scale SPC %scale);
   %p.setTransform(%x SPC %y SPC %z SPC "0 0 1" SPC %yaw);
   %p.setVelocity("0 0 0");
   return %p;
}

// ---------------------------------------------------------------------------------------------------------
// geometry: (u, v) = across from the centre line, and up from the floor

function dkBoardX(%seat)
{
   return $Dk::SeatOffset[%seat] * $Dk::Spacing;
}

function dkAnchor(%seat)
{
   return dkBoardX(%seat) SPC (-$Dk::CamDist) SPC ($Dk::Z0 + $Dk::Height / 2 + $Dk::CamZShift) SPC "1 0 0 0";
}

// parked barrels wait behind the camera: in scope (so the client has them) but out of sight; far away they would never be sent
function dkPark(%obj)
{
   %obj.setTransform("0 -300 316");
}

function dkFreeSeat()
{
   for (%s = 0; %s < $Dk::Seats; %s++)
   {
      %used = false;
      for (%i = 0; %i < ClientGroup.getCount(); %i++)
         if (ClientGroup.getObject(%i).dkSeat $= %s)
            %used = true;
      if (!%used)
         return %s;
   }
   return "";
}

// ---------------------------------------------------------------------------------------------------------
// one board

// DEBUG: every candidate shape in a grid, to see which look good
function dkMakeBoard(%cl)
{
   %seat = %cl.dkSeat;
   %g = new SimGroup();
   MissionCleanup.add(%g);
   %cl.dkGroup = %g;
   %bx = dkBoardX(%seat);
   // the girders, ladders, drum and barrels are game models in the mission file (make_donkey_mis.py); only the people
   // are made here, from the stock armors
   // the backdrop: one pale panel behind the board, scaled up about the view centre to line up with it
   %k = $Dk::BackK;
   %vc = $Dk::Height / 2 + $Dk::CamZShift;
   %g.add(dkField(DkBack, %bx - 16 * %k, $Dk::BackY, $Dk::Z0 + %vc + (-1 - %vc) * %k, 32 * %k, 0.5, 34 * %k));
   %cl.dkApe = dkActor("HeavyMaleBiodermArmor", $Dk::ApeScale, %bx - 12, 0, $Dk::Z0 + $Dk::PV[5] + 0.05, 1.5708);
   %g.add(%cl.dkApe);
   // the ape carries a mortar and fires the barrels with it (no ammo, so the weapon never shoots a real shell)
   %cl.dkApe.mountImage(MortarImage, 0);
   %cl.dkLady = dkActor("MediumFemaleHumanArmor", $Dk::ManScale, %bx - 3, 0, $Dk::Z0 + $Dk::PV[6] + 0.05, 1.5708);
   %g.add(%cl.dkLady);
   %cl.dkMan = dkActor("MediumMaleHumanArmor", $Dk::ManScale, %bx - 5, 0, $Dk::Z0 + $Dk::PV[0] + 0.05, 1.5708);
   %g.add(%cl.dkMan);
   // the barrels: stock mortar barrel packs lying around as props (a mission-file shape cannot be moved once the client has
   // it, and a script cannot add a shape datablock; an item of a stock datablock can do both)
   for (%i = 0; %i < $Dk::MaxBarrels; %i++)
   {
      %o = new StaticShape() {
         dataBlock = "DkBomb";
         position = "0 -300 316";
         rotation = "1 0 0 0";
      };
      %o.dkBarrel = true;
      %o.setScale($Dk::BarrelScale SPC $Dk::BarrelScale SPC $Dk::BarrelScale);
      %g.add(%o);
      $Dk::BObj[%seat, %i] = %o;
      $Dk::BOn[%seat, %i] = 0;
      dkPark(%o);
   }
   dkPurgeZones();
}

// a barrel's shape and flame at (%u, %v): lying along the girder, or nose down while it falls or climbs down
function dkPlaceBarrel(%seat, %i, %u, %v, %dir, %falling)
{
   %x = dkBoardX(%seat) + %u;
   %z = $Dk::Z0 + %v - $Dk::BarrelDrop;
   %o = $Dk::BObj[%seat, %i];
   // the shell's nose is its +y: rolling it points the way it travels, falling it points down (this engine's angles turn the
   // other way round: +90 degrees about z faces right, about x it points the nose down)
   if (%falling)
      %o.setTransform(%x SPC 0 SPC %z SPC "1 0 0 1.5708");
   else if (%dir > 0)
      %o.setTransform(%x SPC 0 SPC %z SPC "0 0 1 1.5708");
   else
      %o.setTransform(%x SPC 0 SPC %z SPC "0 0 1 -1.5708");
   // the flame, on the bomb itself: a particle emitter cannot be moved once a client has it, so a short-lived one is created at
   // the bomb every so often (a little ahead of it, as the bomb moves while the emitter lives) and deletes itself
   %now = getSimTime();
   if (%now >= $Dk::BSpark[%seat, %i])
   {
      $Dk::BSpark[%seat, %i] = %now + $Dk::SparkMs;
      if (%falling)
         dkSpark(%x, $Dk::Z0 + %v - 0.3);
      else
         dkSpark(%x + %dir * 0.3, $Dk::Z0 + %v);
   }
}

function dkSpark(%x, %z)
{
   %e = new ParticleEmissionDummy() {
      dataBlock = "defaultEmissionDummy";
      emitter = "DkFlameEmitter";
      position = %x SPC -0.3 SPC %z;
      rotation = "1 0 0 0";
      scale = "1 1 1";
   };
   MissionCleanup.add(%e);
   %e.schedule($Dk::SparkLifeMs, "delete");
}

function dkParkBarrel(%seat, %i)
{
   dkPark($Dk::BObj[%seat, %i]);
}

// Mario's model at his position, moving at the velocity that makes it run, climb or fly
function dkPlaceMan(%cl)
{
   %seat = %cl.dkSeat;
   %feet = %cl.dkMV - $Dk::ManSize / 2;
   %yaw = 1.5708;                       // this engine turns the other way round: +90 degrees faces right
   if (%cl.dkFace < 0)
      %yaw = -1.5708;
   if (%cl.dkMode $= "climb")
      %yaw = 0;
   %cl.dkMan.setTransform((dkBoardX(%seat) + %cl.dkU) SPC 0 SPC ($Dk::Z0 + %feet + 0.03) SPC "0 0 1" SPC %yaw);
   %cl.dkMan.setVelocity(%cl.dkVX SPC 0 SPC %cl.dkVZ);
   // nothing is solid under a standalone player, so the engine's own pick never chooses a walk cycle; choose it here. The
   // thread must not be held (a held thread stands still) and must not wait for its end
   %anim = "root";
   if (%cl.dkMode $= "jump")
      %anim = "fall";
   else if (%cl.dkVX != 0 || %cl.dkVZ != 0)
      %anim = "run";
   if (%anim !$= %cl.dkAnim)
   {
      %cl.dkAnim = %anim;
      %cl.dkMan.setActionThread(%anim, false, false);
   }
}

// ---------------------------------------------------------------------------------------------------------
// the game

function dkStartGame(%cl)
{
   %cl.dkScore = 0;
   %cl.dkLives = $Dk::Lives;
   %cl.dkLevel = 1;
   dkNewLevel(%cl);
}

function dkNewLevel(%cl)
{
   %cl.dkBonus = 3000;
   %cl.dkBonusAt = getSimTime() + 1500;
   dkRespawn(%cl);
   %cl.dkState = "play";
   dkHud(%cl);
}

// Mario back at the start, the barrels cleared
function dkRespawn(%cl)
{
   %seat = %cl.dkSeat;
   for (%i = 0; %i < $Dk::MaxBarrels; %i++)
   {
      $Dk::BOn[%seat, %i] = 0;
      dkParkBarrel(%seat, %i);
   }
   %cl.dkU = -5;
   %cl.dkFace = 1;
   %cl.dkVX = 0;
   %cl.dkVZ = 0;
   %cl.dkMV = $Dk::PV[0] + $Dk::ManSize / 2;
   %cl.dkAnim = "";                    // dkPlaceMan picks the animation again after a death
   dkPlaceMan(%cl);
   %cl.dkPl = 0;
   %cl.dkMode = "ground";
   %cl.dkJump = false;
   %cl.dkThrowAt = getSimTime() + 1800;
}

function dkHud(%cl)
{
   if (%cl.dkState $= "over")
   {
      bottomPrint(%cl, "GAME OVER    SCORE " @ %cl.dkScore, 0, 1);
      return;
   }
   if (%cl.dkState $= "win")
   {
      bottomPrint(%cl, "YOU SAVED HER!    BONUS " @ %cl.dkBonus @ "    SCORE " @ %cl.dkScore, 0, 1);
      return;
   }
   bottomPrint(%cl, "SCORE " @ %cl.dkScore @ "    LIVES " @ %cl.dkLives @ "    LEVEL " @ %cl.dkLevel @ "    BONUS " @ %cl.dkBonus @ "     jump = jump", 0, 1);
}

function dkDie(%cl)
{
   if (%cl.dkState !$= "play")
      return;
   %cl.dkLives--;
   %cl.dkVX = 0;
   %cl.dkVZ = 0;
   %cl.dkMan.setVelocity("0 0 0");
   %cl.dkMan.setActionThread("death1", true);
   if (%cl.dkLives <= 0)
   {
      %cl.dkState = "over";
      %cl.dkOverAt = getSimTime() + $Dk::GameOverMs;
      if (%cl.dkScore > %cl.dkHigh)
         %cl.dkHigh = %cl.dkScore;
      messageAll('MsgDonkey', '\c1%1 is done (%2 points).', %cl.name, %cl.dkScore);
   }
   else
   {
      %cl.dkState = "dead";
      %cl.dkRespawnAt = getSimTime() + $Dk::RespawnMs;
   }
   dkHud(%cl);
}

// ---------------------------------------------------------------------------------------------------------
// Mario: on a girder, on a ladder, or in the air

function dkMan(%cl, %dt, %now)
{
   %m = %cl.dkPadMask;
   %walk = 0;
   if (%m & 4)
      %walk = -1;
   else if (%m & 8)
      %walk = 1;
   if (%walk != 0)
      %cl.dkFace = %walk;
   %cl.dkVX = 0;
   %cl.dkVZ = 0;
   %k = %cl.dkPl;
   if (%cl.dkMode $= "ground")
   {
      %cl.dkU += %walk * $Dk::WalkSpeed * %dt;
      %cl.dkVX = %walk * $Dk::WalkSpeed;
      %x0 = $Dk::PX0[%k] + 0.7;
      if (%k == 5)
         %x0 = -9;                                  // the ape's corner
      %x1 = $Dk::PX1[%k] - 0.7;
      if (%cl.dkU < %x0) %cl.dkU = %x0;
      if (%cl.dkU > %x1) %cl.dkU = %x1;
      if (%cl.dkJump)
      {
         %cl.dkMode = "jump";
         %cl.dkT = 0;
         %cl.dkVx = %walk * $Dk::WalkSpeed;
      }
      else if (%m & 1)
      {
         %j = dkLadderUp(%k, %cl.dkU, 0.8);
         if (%j >= 0)
         {
            %cl.dkMode = "climb";
            %cl.dkLad = %j;
            %cl.dkU = $Dk::LU[%j];
            %cl.dkV = $Dk::PV[%k];
         }
      }
      else if ((%m & 2) && %k > 0)
      {
         %j = dkLadderUp(%k - 1, %cl.dkU, 0.8);
         if (%j >= 0)
         {
            %cl.dkMode = "climb";
            %cl.dkLad = %j;
            %cl.dkU = $Dk::LU[%j];
            %cl.dkV = $Dk::PV[%k];
         }
      }
      %cl.dkJump = false;
   }
   else if (%cl.dkMode $= "climb")
   {
      %lo = $Dk::LK[%cl.dkLad];
      %v0 = $Dk::PV[%lo];
      %v1 = $Dk::PV[%lo + 1];
      if (%m & 1)
      {
         %cl.dkV += $Dk::ClimbSpeed * %dt;
         %cl.dkVZ = $Dk::ClimbSpeed;
      }
      else if (%m & 2)
      {
         %cl.dkV -= $Dk::ClimbSpeed * %dt;
         %cl.dkVZ = -$Dk::ClimbSpeed;
      }
      if (%cl.dkV >= %v1)
      {
         %cl.dkPl = %lo + 1;
         %cl.dkMode = "ground";
      }
      else if (%cl.dkV <= %v0)
      {
         %cl.dkPl = %lo;
         %cl.dkMode = "ground";
      }
      %cl.dkJump = false;
   }
   else
   {
      // in the air: carry on sideways, up and down along an arc
      %cl.dkT += %dt;
      %cl.dkU += %cl.dkVx * %dt;
      %cl.dkVX = %cl.dkVx;
      %cl.dkVZ = $Dk::JumpHeight * 4 * (1 - 2 * %cl.dkT / $Dk::JumpSecs) / $Dk::JumpSecs;
      %x0 = $Dk::PX0[%k] + 0.7;
      if (%k == 5)
         %x0 = -9;
      %x1 = $Dk::PX1[%k] - 0.7;
      if (%cl.dkU < %x0) %cl.dkU = %x0;
      if (%cl.dkU > %x1) %cl.dkU = %x1;
      if (%cl.dkT >= $Dk::JumpSecs)
         %cl.dkMode = "ground";
      %cl.dkJump = false;
   }
   // where he is: the height above the surface he stands on
   if (%cl.dkMode $= "climb")
      %cl.dkH = 0;
   else if (%cl.dkMode $= "jump")
   {
      %s = %cl.dkT / $Dk::JumpSecs;
      %cl.dkH = $Dk::JumpHeight * 4 * %s * (1 - %s);
   }
   else
      %cl.dkH = 0;
   if (%cl.dkMode $= "climb")
      %cl.dkMV = %cl.dkV + $Dk::ManSize / 2;
   else
      %cl.dkMV = $Dk::PV[%cl.dkPl] + $Dk::ManSize / 2 + %cl.dkH;
   // the goal
   if (%cl.dkPl == 6 && %cl.dkMode $= "ground" && %cl.dkU < -1)
   {
      %cl.dkScore += 1000 + %cl.dkBonus;
      %cl.dkState = "win";
      %cl.dkWinAt = %now + $Dk::WinMs;
      dkHud(%cl);
   }
}

// ---------------------------------------------------------------------------------------------------------
// the barrels: on a girder (1), falling to the girder below (2), down a ladder (3)

function dkThrow(%cl)
{
   %seat = %cl.dkSeat;
   %cl.dkApe.setActionThread("cel2");
   %cl.dkApeReset = getSimTime() + 1200;
   for (%i = 0; %i < $Dk::MaxBarrels; %i++)
      if ($Dk::BOn[%seat, %i] == 0)
      {
         $Dk::BOn[%seat, %i] = 1;
         $Dk::BU[%seat, %i] = -9.3;
         $Dk::BPl[%seat, %i] = 5;
         $Dk::BD[%seat, %i] = 1;
         $Dk::BV[%seat, %i] = $Dk::PV[5] + $Dk::BarrelSize / 2;
         $Dk::BLast[%seat, %i] = -1;
         $Dk::BScored[%seat, %i] = false;
         return;
      }
}

function dkBarrels(%cl, %dt, %now)
{
   %seat = %cl.dkSeat;
   %speed = $Dk::BarrelSpeed * (1 + (%cl.dkLevel - 1) * 0.12);
   if (%speed > 9)
      %speed = 9;
   for (%i = 0; %i < $Dk::MaxBarrels; %i++)
   {
      %st = $Dk::BOn[%seat, %i];
      if (%st == 0)
         continue;
      %k = $Dk::BPl[%seat, %i];
      if (%st == 1)
      {
         $Dk::BU[%seat, %i] += $Dk::BD[%seat, %i] * %speed * %dt;
         %u = $Dk::BU[%seat, %i];
         if (%k == 0 && %u < -13.8)
         {
            $Dk::BOn[%seat, %i] = 0;
            dkParkBarrel(%seat, %i);
            continue;
         }
         if (%k > 0 && (%u < $Dk::PX0[%k] || %u > $Dk::PX1[%k]))
         {
            // off the end: fall to the girder below and come back the other way
            $Dk::BOn[%seat, %i] = 2;
            $Dk::BVy[%seat, %i] = 0;
            $Dk::BD[%seat, %i] = -$Dk::BD[%seat, %i];
            $Dk::BPl[%seat, %i] = %k - 1;
         }
         else if (%k > 0)
         {
            // maybe take a ladder down
            %j = dkLadderUp(%k - 1, %u, 0.25);
            if (%j >= 0 && $Dk::BLast[%seat, %i] != %j)
            {
               $Dk::BLast[%seat, %i] = %j;
               if (getRandom() < $Dk::LadderChance)
               {
                  $Dk::BOn[%seat, %i] = 3;
                  $Dk::BU[%seat, %i] = $Dk::LU[%j];
                  $Dk::BPl[%seat, %i] = %k - 1;
               }
            }
         }
      }
      else if (%st == 2)
      {
         $Dk::BVy[%seat, %i] += $Dk::BarrelFall * %dt;
         $Dk::BV[%seat, %i] -= $Dk::BVy[%seat, %i] * %dt;
         $Dk::BU[%seat, %i] += $Dk::BD[%seat, %i] * 1.5 * %dt;
         %land = $Dk::PV[$Dk::BPl[%seat, %i]] + $Dk::BarrelSize / 2;
         if ($Dk::BV[%seat, %i] <= %land)
         {
            $Dk::BV[%seat, %i] = %land;
            $Dk::BOn[%seat, %i] = 1;
         }
      }
      else
      {
         %land = $Dk::PV[$Dk::BPl[%seat, %i]] + $Dk::BarrelSize / 2;
         $Dk::BV[%seat, %i] -= 4 * %dt;
         if ($Dk::BV[%seat, %i] <= %land)
         {
            $Dk::BV[%seat, %i] = %land;
            $Dk::BOn[%seat, %i] = 1;
         }
      }
      if ($Dk::BOn[%seat, %i] == 1)
         $Dk::BV[%seat, %i] = $Dk::PV[$Dk::BPl[%seat, %i]] + $Dk::BarrelSize / 2;
      // contact with Mario: under him while he jumps is a clean jump
      %du = mAbs($Dk::BU[%seat, %i] - %cl.dkU);
      %dv = %cl.dkMV - $Dk::BV[%seat, %i];
      if (%du < 1.0)
      {
         if (mAbs(%dv) < 1.2)
         {
            dkDie(%cl);
            return;
         }
         if (%dv > 1.2 && %dv < 3.8 && %cl.dkMode $= "jump" && !$Dk::BScored[%seat, %i])
         {
            $Dk::BScored[%seat, %i] = true;
            %cl.dkScore += 100;
            dkHud(%cl);
         }
      }
      dkPlaceBarrel(%seat, %i, $Dk::BU[%seat, %i], $Dk::BV[%seat, %i], $Dk::BD[%seat, %i], $Dk::BOn[%seat, %i] != 1);
   }
}

// the ape throws with a flourish, the lady waves now and then
function dkApeLife(%cl, %now)
{
   if (%now >= %cl.dkLadyAt)
   {
      %cl.dkLadyAt = %now + 3500;
      %cl.dkLady.setActionThread("cel2");
   }
   if (%cl.dkApeReset > 0 && %now >= %cl.dkApeReset)
   {
      %cl.dkApeReset = 0;
      %cl.dkApe.setActionThread("root", false, false);
   }
}

function dkPhysics(%cl, %dt, %now)
{
   %seat = %cl.dkSeat;
   if (%cl.dkState $= "over")
   {
      if (%now >= %cl.dkOverAt)
         dkStartGame(%cl);
      return;
   }
   if (%cl.dkState $= "dead")
   {
      if (%now >= %cl.dkRespawnAt)
      {
         dkRespawn(%cl);
         %cl.dkState = "play";
         dkHud(%cl);
      }
      return;
   }
   if (%cl.dkState $= "win")
   {
      if (%now >= %cl.dkWinAt)
      {
         %cl.dkLevel++;
         dkNewLevel(%cl);
      }
      return;
   }
   dkMan(%cl, %dt, %now);
   if (%cl.dkState !$= "play")
      return;
   // the ape throws, the bonus shrinks
   if (%now >= %cl.dkThrowAt)
   {
      dkThrow(%cl);
      %gap = 2700 - %cl.dkLevel * 250;
      if (%gap < 1200)
         %gap = 1200;
      %cl.dkThrowAt = %now + %gap * (0.7 + getRandom() * 0.6);
   }
   if (%now >= %cl.dkBonusAt)
   {
      %cl.dkBonusAt = %now + 1500;
      if (%cl.dkBonus > 0)
      {
         %cl.dkBonus -= 100;
         dkHud(%cl);
      }
   }
   dkBarrels(%cl, %dt, %now);
   if (%cl.dkState !$= "play")
      return;
   dkPlaceMan(%cl);
   dkApeLife(%cl, %now);
}

// ---------------------------------------------------------------------------------------------------------
// input: the player's keys, read by Retro.cs from its (tiny) velocity; a jump arrives as a trigger press

function dkTick()
{
   cancel($Dk::TickThread);
   $Dk::TickThread = schedule($Dk::TickMs, 0, dkTick);
   if (!isObject(MissionCleanup))
      return;
   $Dk::TickN++;
   %now = getSimTime();
   %dt = $Dk::LastTick $= "" ? 0.032 : (%now - $Dk::LastTick) / 1000;
   $Dk::LastTick = %now;
   if (%dt > 0.1) %dt = 0.1;
   for (%i = 0; %i < ClientGroup.getCount(); %i++)
   {
      %cl = ClientGroup.getObject(%i);
      if (!%cl.dkJoined || !isObject(%cl.camera))
         continue;
      if (%cl.dkSeat $= "")
      {
         // spectators have the stock free-fly camera; the stand-in Player is parked out of the way
         if (isObject(%cl.player))
         {
            %cl.player.setVelocity("0 0 0");
            if ($Dk::TickN % 30 == 0)
               %cl.player.setTransform("0 -150 300 1 0 0 0");
         }
         if (%cl.getControlObject() != %cl.camera)
            %cl.setControlObject(%cl.camera);
         continue;
      }
      %cl.dkPadMask = retroPad(%cl);
      dkPhysics(%cl, %dt, %now);
   }
}

function dkInitPlayer(%cl)
{
   %cl.dkJoined = true;
   if (%cl.dkSeat $= "")
   {
      %seat = dkFreeSeat();
      if (%seat !$= "")
      {
         %cl.dkSeat = %seat;
         dkMakeBoard(%cl);
      }
   }
   if (%cl.dkSeat $= "")
   {
      // a spectator: the free-fly camera, parked player
      %cam = %cl.camera;
      %cam.setFlyMode();
      %cam.setTransform(dkAnchor(0));
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
   %a = dkAnchor(%cl.dkSeat);
   retroAttach(%cl, getWord(%a, 0), getWord(%a, 1), getWord(%a, 2));
   commandToClient(%cl, 'setHudMode', 'Standard');
   dkStartGame(%cl);
}

function dkFreeClient(%cl)
{
   retroDetach(%cl);
   if (isObject(%cl.dkGroup))
      %cl.dkGroup.delete();
   %cl.dkGroup = "";
   %cl.dkSeat = "";
   %cl.dkJoined = false;
}

// Switch the package on and off with the mission, so stock DM missions are untouched.
function dkWatch()
{
   cancel($Dk::WatchThread);
   $Dk::WatchThread = schedule(250, 0, dkWatch);
   %on = ($CurrentMission $= $Dk::Mission && isObject(MissionCleanup));
   if (%on && !$Dk::PkgOn)
   {
      activatePackage(T2LaunchDonkey);
      $Dk::PkgOn = true;
      $Dk::LastTick = "";
      dkTick();
      echo("Donkey ruleset on");
   }
   else if (!%on && $Dk::PkgOn)
   {
      deactivatePackage(T2LaunchDonkey);
      $Dk::PkgOn = false;
      cancel($Dk::TickThread);
      echo("Donkey ruleset off");
   }
}

package T2LaunchDonkeyBoot
{
   function CreateServer(%mission, %missionType)
   {
      Parent::CreateServer(%mission, %missionType);
      dkDatablocks();
   }
};

package T2LaunchDonkey
{
   // the barrel props are not for picking up
   function ItemData::onCollision(%data, %obj, %col)
   {
      if (%obj.dkBarrel)
         return;
      Parent::onCollision(%data, %obj, %col);
   }

   function DMGame::equip(%game, %player)
   {
      // nobody carries anything
   }

   function DMGame::createPlayer(%game, %client, %spawnLoc, %respawn)
   {
      Parent::createPlayer(%game, %client, %spawnLoc, %respawn);
      dkInitPlayer(%client);
   }

   function DMGame::onClientKilled(%game, %clVictim, %clKiller, %damageType, %implement, %damageLoc)
   {
      // the stand-in Player is not part of the game: no respawn timer or orbit camera
      %clVictim.deaths++;
   }

   function DMGame::ObserverOnTrigger(%game, %data, %obj, %trigger, %state)
   {
      %client = %obj.getControllingClient();
      if (%client && %client.dkJoined)
         return false;
      return Parent::ObserverOnTrigger(%game, %data, %obj, %trigger, %state);
   }

   // a seated player is a Player now, so its triggers arrive here: fire and jump jump
   function Armor::onTrigger(%data, %player, %trigger, %val)
   {
      %client = %player.getControllingClient();
      if (%client && %client.dkJoined)
      {
         if (%val && (%trigger == 0 || %trigger == 2) && %client.dkSeat !$= "")
            %client.dkJump = true;
         return;
      }
      Parent::onTrigger(%data, %player, %trigger, %val);
   }

   function DMGame::onClientLeaveGame(%game, %client)
   {
      dkFreeClient(%client);
      Parent::onClientLeaveGame(%game, %client);
   }
};

if (!$Dk::Installed)
{
   $Dk::Installed = true;
   dkTables();
   activatePackage(T2LaunchDonkeyBoot);
   dkDatablocks();
   dkWatch();
   echo("Donkey gametype script loaded");
}
