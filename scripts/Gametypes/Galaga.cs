// Galaga: server-side ruleset for the MatrixGalaga mission.
// Evaluated in memory by the t2launch hook (server.gametype_script, after Retro.cs); stock clients download nothing.
// The package is active only while $CurrentMission is MatrixGalaga.
//
// Rules: 36 enemies (4 bosses, 16 butterflies, 16 bees) swoop in from the top corners along curved paths and settle
// into a swaying formation. Afterwards single enemies peel off and dive at your ship, firing as they come; a diving
// boss takes up to two butterflies with it. Your ship fires at most two shots at a time. Hit a diver for double
// points. Every fourth stage is a challenging stage: the enemies only fly through, there is no shooting, and
// shooting all 36 earns a bonus. Three lives. Every player gets their own board on an upright wall, seen side-on.
// Not included: the tractor beam and the dual fighter.
// Controls (stock keybinds): strafe moves the ship, fire shoots. Input is read by Retro.cs.

$Gal::Mission = "MatrixGalaga";
$Gal::HalfW = 18;                    // court half-width in metres
$Gal::Height = 36;
$Gal::Z0 = 300;                      // world height of the court floor
$Gal::Spacing = 44;                  // between boards
$Gal::Depth = 2;
$Gal::Seats = 4;
$Gal::SeatOffset[0] = 0;  $Gal::SeatOffset[1] = 1;  $Gal::SeatOffset[2] = -1; $Gal::SeatOffset[3] = 2;
$Gal::ShipV = 2;                     // ship centre height
$Gal::ShipW = 2.6;
$Gal::ShipH = 1.4;
$Gal::ShipSpeed = 20;
$Gal::MaxShots = 2;
$Gal::ShotSpeed = 40;
$Gal::MaxBombs = 4;
$Gal::BombSpeed = 13;
$Gal::Enemies = 36;
$Gal::EnW = 1.8;
$Gal::EnH = 1.3;
$Gal::SlotDU = 2.6;                  // formation spacing
$Gal::SlotDV = 2.3;
$Gal::BaseV = 31;                    // height of the top formation row
$Gal::SwayMax = 3;
$Gal::SwayStepMs = 140;
$Gal::SwayStep = 0.5;
$Gal::EntryDur = 2.4;                // seconds a swoop takes
$Gal::GroupGapMs = 1300;
$Gal::StaggerMs = 180;
$Gal::DiveDur = 2.4;
$Gal::Lives = 3;
$Gal::RespawnMs = 1500;
$Gal::GameOverMs = 4500;
$Gal::NextStageMs = 2200;
$Gal::CamDist = 46;
$Gal::CamZShift = -1.5;
$Gal::TickMs = 32;
$Gal::ParkX = 3000;                  // inactive blocks wait far outside every client's scope
$Gal::ParkY = 3000;
$Gal::Debug = 0;

// enemy i: row 0 holds the 4 bosses (columns 2..5), rows 1..4 hold 8 each; the entry order is the butterflies, then
// the bosses with the next butterflies, then the bees
function galTables()
{
   for (%i = 0; %i < 36; %i++)
   {
      if (%i < 4)
      {
         $Gal::ERow[%i] = 0;
         $Gal::ECol[%i] = %i + 2;
      }
      else
      {
         $Gal::ERow[%i] = mFloor((%i - 4) / 8) + 1;
         $Gal::ECol[%i] = (%i - 4) % 8;
      }
   }
   %o = "";
   for (%i = 4; %i < 12; %i++)
      %o = %o SPC %i;
   for (%i = 0; %i < 4; %i++)
      %o = %o SPC %i;
   for (%i = 12; %i < 36; %i++)
      %o = %o SPC %i;
   $Gal::Order = trim(%o);
}

// ---------------------------------------------------------------------------------------------------------
// datablocks: deleted by DestroyServer, so defined at install and again after every CreateServer

function galDatablocks()
{
   if (isObject(GalShip))
      return;
   %defs = "GalBoss|0.3 1.0 0.5;GalFly|1.0 0.25 0.25;GalBee|1.0 0.9 0.2;GalShip|0.9 0.95 1.0;"
         @ "GalShot|1.0 1.0 1.0;GalBomb|1.0 0.55 0.1;GalWall|0.8 0.8 0.85";
   for (%i = 0; %i < getFieldCount(strreplace(%defs, ";", "\t")); %i++)
   {
      %def = getField(strreplace(%defs, ";", "\t"), %i);
      %name = getSubStr(%def, 0, strstr(%def, "|"));
      %color = getSubStr(%def, strstr(%def, "|") + 1, 64);
      %alpha = 0.9;
      if (%name $= "GalWall")
         %alpha = 0.3;
      eval("datablock ForceFieldBareData(" @ %name @ ") : defaultForceFieldBare { framesPerSec = 1; numFrames = 1; scrollSpeed = 0; color = \"" @ %color
         @ "\"; baseTranslucency = " @ %alpha @ "; needsNoPower = true; };");
   }
}

function galField(%db, %x, %z, %sx, %sz)
{
   return new ForceFieldBare() {
      dataBlock = %db;
      position = %x SPC (-$Gal::Depth / 2) SPC %z;
      rotation = "1 0 0 0";
      scale = %sx SPC $Gal::Depth SPC %sz;
   };
}

function galPurgeZones()
{
   %z = nameToID("MissionCleanup/PZones");
   if (%z > 0)
      %z.delete();
}

// ---------------------------------------------------------------------------------------------------------
// geometry: court coordinates are (u, v) = across from the centre line, and up from the floor

function galBoardX(%seat)
{
   return $Gal::SeatOffset[%seat] * $Gal::Spacing;
}

function galAnchor(%seat)
{
   return galBoardX(%seat) SPC (-$Gal::CamDist) SPC ($Gal::Z0 + $Gal::Height / 2 + $Gal::CamZShift) SPC "1 0 0 0";
}

// a block's position is its lower-left corner; (%u, %v) is its centre
function galPlace(%obj, %seat, %u, %v, %w, %h)
{
   %obj.setTransform((galBoardX(%seat) + %u - %w / 2) SPC (-$Gal::Depth / 2) SPC ($Gal::Z0 + %v - %h / 2));
}

function galPark(%obj)
{
   %obj.setTransform($Gal::ParkX SPC $Gal::ParkY SPC 300);
}

function galSlotU(%cl, %i)
{
   return ($Gal::ECol[%i] - 3.5) * $Gal::SlotDU + %cl.galSway;
}

function galSlotV(%i)
{
   return $Gal::BaseV - $Gal::ERow[%i] * $Gal::SlotDV;
}

function galKind(%i)
{
   if ($Gal::ERow[%i] == 0)
      return "GalBoss";
   if ($Gal::ERow[%i] <= 2)
      return "GalFly";
   return "GalBee";
}

function galPoints(%i, %diving)
{
   if ($Gal::ERow[%i] == 0)
      return %diving ? 400 : 150;
   if ($Gal::ERow[%i] <= 2)
      return %diving ? 160 : 80;
   return %diving ? 100 : 50;
}

function galFreeSeat()
{
   for (%s = 0; %s < $Gal::Seats; %s++)
   {
      %used = false;
      for (%i = 0; %i < ClientGroup.getCount(); %i++)
         if (ClientGroup.getObject(%i).galSeat $= %s)
            %used = true;
      if (!%used)
         return %s;
   }
   return "";
}

// ---------------------------------------------------------------------------------------------------------
// one board

function galMakeBoard(%cl)
{
   %seat = %cl.galSeat;
   %g = new SimGroup();
   MissionCleanup.add(%g);
   %cl.galGroup = %g;
   %h = $Gal::Height;
   %w = $Gal::HalfW;
   %g.add(galField(GalWall, galBoardX(%seat) - %w - 0.6, $Gal::Z0, 0.6, %h + 0.6));
   %g.add(galField(GalWall, galBoardX(%seat) + %w, $Gal::Z0, 0.6, %h + 0.6));
   %g.add(galField(GalWall, galBoardX(%seat) - %w - 0.6, $Gal::Z0 + %h, %w * 2 + 1.2, 0.6));
   %g.add(galField(GalWall, galBoardX(%seat) - %w - 0.6, $Gal::Z0 - 0.6, %w * 2 + 1.2, 0.6));
   %cl.galShip = galField(GalShip, 0, 0, $Gal::ShipW, $Gal::ShipH);
   %g.add(%cl.galShip);
   galPark(%cl.galShip);
   for (%k = 0; %k < $Gal::MaxShots; %k++)
   {
      %o = galField(GalShot, 0, 0, 0.4, 1.2);
      %g.add(%o);
      galPark(%o);
      $Gal::Shot[%seat, %k] = %o;
      $Gal::ShotOn[%seat, %k] = false;
   }
   for (%k = 0; %k < $Gal::MaxBombs; %k++)
   {
      %o = galField(GalBomb, 0, 0, 0.5, 1.0);
      %g.add(%o);
      galPark(%o);
      $Gal::Bomb[%seat, %k] = %o;
      $Gal::BombOn[%seat, %k] = false;
   }
   for (%i = 0; %i < $Gal::Enemies; %i++)
   {
      %o = galField(galKind(%i), 0, 0, $Gal::EnW, $Gal::EnH);
      %g.add(%o);
      galPark(%o);
      $Gal::E[%seat, %i] = %o;
      $Gal::St[%seat, %i] = 0;
   }
   galPurgeZones();
}

function galStartGame(%cl)
{
   %cl.galScore = 0;
   %cl.galLives = $Gal::Lives;
   %cl.galStage = 1;
   %cl.galU = 0;
   galNewStage(%cl);
}

function galChallenge(%cl)
{
   return %cl.galStage % 4 == 3;
}

function galNewStage(%cl)
{
   %seat = %cl.galSeat;
   galClearShots(%cl);
   %cl.galSway = 0;
   %cl.galSwayDir = 1;
   %cl.galSwayAt = getSimTime() + 1000;
   %cl.galLeft = $Gal::Enemies;
   %cl.galHits = 0;
   %cl.galStageAt = getSimTime() + 900;
   %cl.galAttackAt = 0;
   for (%i = 0; %i < $Gal::Enemies; %i++)
   {
      $Gal::St[%seat, %i] = 1;                  // waiting to enter
      galPark($Gal::E[%seat, %i]);
   }
   %cl.galState = "play";
   galHud(%cl);
}

function galClearShots(%cl)
{
   %seat = %cl.galSeat;
   for (%k = 0; %k < $Gal::MaxShots; %k++)
   {
      $Gal::ShotOn[%seat, %k] = false;
      galPark($Gal::Shot[%seat, %k]);
   }
   for (%k = 0; %k < $Gal::MaxBombs; %k++)
   {
      $Gal::BombOn[%seat, %k] = false;
      galPark($Gal::Bomb[%seat, %k]);
   }
}

function galHud(%cl)
{
   if (%cl.galState $= "over")
   {
      bottomPrint(%cl, "GAME OVER    SCORE " @ %cl.galScore, 0, 1);
      return;
   }
   if (%cl.galState $= "result")
   {
      if (%cl.galHits >= $Gal::Enemies)
         bottomPrint(%cl, "PERFECT!  " @ %cl.galHits @ " of " @ $Gal::Enemies @ " hit    BONUS 10000", 0, 1);
      else
         bottomPrint(%cl, "CHALLENGING STAGE OVER    " @ %cl.galHits @ " of " @ $Gal::Enemies @ " hit", 0, 1);
      return;
   }
   %tag = galChallenge(%cl) ? "   CHALLENGING STAGE" : "";
   bottomPrint(%cl, "SCORE " @ %cl.galScore @ "    LIVES " @ %cl.galLives @ "    STAGE " @ %cl.galStage @ %tag @ "     strafe = move, fire = shoot", 0, 1);
}

// ---------------------------------------------------------------------------------------------------------
// enemy movement

// the point at time %t (0..1) of the quadratic curve stored for enemy %i
function galCurve(%seat, %i, %t)
{
   %a = (1 - %t) * (1 - %t);
   %b = 2 * (1 - %t) * %t;
   %c = %t * %t;
   %u = %a * $Gal::P0U[%seat, %i] + %b * $Gal::P1U[%seat, %i] + %c * $Gal::P2U[%seat, %i];
   %v = %a * $Gal::P0V[%seat, %i] + %b * $Gal::P1V[%seat, %i] + %c * $Gal::P2V[%seat, %i];
   return %u SPC %v;
}

function galSetPos(%cl, %i, %u, %v)
{
   %seat = %cl.galSeat;
   $Gal::EU[%seat, %i] = %u;
   $Gal::EV[%seat, %i] = %v;
   galPlace($Gal::E[%seat, %i], %seat, %u, %v, $Gal::EnW, $Gal::EnH);
}

// send enemy %i swooping in from a top corner; group %g decides the side and the shape of the swoop
function galEnter(%cl, %i, %g)
{
   %seat = %cl.galSeat;
   %side = 1;
   if (%g % 2 == 0)
      %side = -1;
   $Gal::St[%seat, %i] = 2;
   $Gal::T[%seat, %i] = 0;
   $Gal::P0U[%seat, %i] = %side * ($Gal::HalfW + 1.5);
   $Gal::P0V[%seat, %i] = $Gal::Height + 1.5;
   $Gal::P1U[%seat, %i] = -%side * ($Gal::HalfW - 3);
   $Gal::P1V[%seat, %i] = 10 + (%g % 3) * 4;
   if (galChallenge(%cl))
   {
      // no formation: leave through the bottom on the far side
      $Gal::P2U[%seat, %i] = %side * ($Gal::ECol[%i] - 3.5) * 2;
      $Gal::P2V[%seat, %i] = -3;
   }
   else
   {
      $Gal::P2U[%seat, %i] = galSlotU(%cl, %i);
      $Gal::P2V[%seat, %i] = galSlotV(%i);
   }
   galSetPos(%cl, %i, $Gal::P0U[%seat, %i], $Gal::P0V[%seat, %i]);
}

function galDive(%cl, %i, %offset)
{
   %seat = %cl.galSeat;
   %u = $Gal::EU[%seat, %i];
   %swing = 12;
   if (%u > 0)
      %swing = -12;
   %p1 = %u + %swing;
   if (%p1 > $Gal::HalfW - 1) %p1 = $Gal::HalfW - 1;
   if (%p1 < -$Gal::HalfW + 1) %p1 = -$Gal::HalfW + 1;
   %p2 = %cl.galU * 0.7 + (getRandom() - 0.5) * 8 + %offset;
   if (%p2 > $Gal::HalfW - 1) %p2 = $Gal::HalfW - 1;
   if (%p2 < -$Gal::HalfW + 1) %p2 = -$Gal::HalfW + 1;
   $Gal::St[%seat, %i] = 4;
   $Gal::Ph[%seat, %i] = 0;
   $Gal::T[%seat, %i] = 0;
   $Gal::Fired[%seat, %i] = false;
   $Gal::FireAt[%seat, %i] = 0.5 + getRandom() * 0.6;
   $Gal::P0U[%seat, %i] = %u;
   $Gal::P0V[%seat, %i] = $Gal::EV[%seat, %i];
   $Gal::P1U[%seat, %i] = %p1;
   $Gal::P1V[%seat, %i] = 16;
   $Gal::P2U[%seat, %i] = %p2;
   $Gal::P2V[%seat, %i] = 2.5;
}

function galDiverCount(%cl)
{
   %seat = %cl.galSeat;
   %n = 0;
   for (%i = 0; %i < $Gal::Enemies; %i++)
      if ($Gal::St[%seat, %i] == 4 || $Gal::St[%seat, %i] == 5)
         %n++;
   return %n;
}

// one formation enemy peels off (a boss takes up to two butterflies with it)
function galAttack(%cl)
{
   %seat = %cl.galSeat;
   %max = 2 + mFloor(%cl.galStage / 2);
   if (%max > 6)
      %max = 6;
   if (galDiverCount(%cl) >= %max)
      return;
   %start = mFloor(getRandom() * 35.99);
   for (%k = 0; %k < $Gal::Enemies; %k++)
   {
      %i = (%start + %k) % $Gal::Enemies;
      if ($Gal::St[%seat, %i] != 3)
         continue;
      galDive(%cl, %i, 0);
      if ($Gal::ERow[%i] == 0)
      {
         %c = $Gal::ECol[%i];
         %e1 = 4 + %c;
         %e2 = 4 + %c + 1;
         if ($Gal::St[%seat, %e1] == 3)
            galDive(%cl, %e1, -2.5);
         if (%e2 < 12 && $Gal::St[%seat, %e2] == 3)
            galDive(%cl, %e2, 2.5);
      }
      return;
   }
}

function galFireBomb(%cl, %i)
{
   %seat = %cl.galSeat;
   for (%k = 0; %k < $Gal::MaxBombs; %k++)
      if (!$Gal::BombOn[%seat, %k])
      {
         %u = $Gal::EU[%seat, %i];
         %v = $Gal::EV[%seat, %i];
         $Gal::BombOn[%seat, %k] = true;
         $Gal::BombU[%seat, %k] = %u;
         $Gal::BombV[%seat, %k] = %v - 1;
         %du = (%cl.galU - %u) / ((%v - $Gal::ShipV) / $Gal::BombSpeed);
         if (%du > 7) %du = 7;
         if (%du < -7) %du = -7;
         $Gal::BombDU[%seat, %k] = %du;
         return;
      }
}

// ---------------------------------------------------------------------------------------------------------
// the game

function galKillEnemy(%cl, %i)
{
   %seat = %cl.galSeat;
   %st = $Gal::St[%seat, %i];
   if (galChallenge(%cl))
   {
      %cl.galScore += 100;
      %cl.galHits++;
   }
   else
      %cl.galScore += galPoints(%i, %st == 4 || %st == 5);
   $Gal::St[%seat, %i] = 0;
   galPark($Gal::E[%seat, %i]);
   %cl.galLeft--;
   galHud(%cl);
}

function galPlayerHit(%cl)
{
   %cl.galLives--;
   galClearShots(%cl);
   galPark(%cl.galShip);
   if (%cl.galLives <= 0)
   {
      galGameOver(%cl);
      return;
   }
   %cl.galState = "dead";
   %cl.galRespawnAt = getSimTime() + $Gal::RespawnMs;
   galHud(%cl);
}

function galGameOver(%cl)
{
   if (%cl.galState $= "over")
      return;
   %cl.galState = "over";
   %cl.galOverAt = getSimTime() + $Gal::GameOverMs;
   if (%cl.galScore > %cl.galHigh)
      %cl.galHigh = %cl.galScore;
   messageAll('MsgGalaga', '\c1%1 is done (%2 points).', %cl.name, %cl.galScore);
   // everything left on the board goes away
   for (%i = 0; %i < $Gal::Enemies; %i++)
   {
      $Gal::St[%cl.galSeat, %i] = 0;
      galPark($Gal::E[%cl.galSeat, %i]);
   }
   galHud(%cl);
}

function galFire(%cl)
{
   if (%cl.galState !$= "play")
      return;
   %seat = %cl.galSeat;
   for (%k = 0; %k < $Gal::MaxShots; %k++)
      if (!$Gal::ShotOn[%seat, %k])
      {
         $Gal::ShotOn[%seat, %k] = true;
         $Gal::ShotU[%seat, %k] = %cl.galU;
         $Gal::ShotV[%seat, %k] = $Gal::ShipV + $Gal::ShipH / 2 + 0.6;
         return;
      }
}

// the enemies: entry, formation, dives
function galEnemies(%cl, %dt, %now)
{
   %seat = %cl.galSeat;
   // enemies enter one after another in groups of eight
   %since = %now - %cl.galStageAt;
   %n = 0;
   %k = 0;
   %entered = true;
   for (%idx = 0; %idx < getWordCount($Gal::Order); %idx++)
   {
      %i = getWord($Gal::Order, %idx);
      if ($Gal::St[%seat, %i] == 1)
      {
         %g = mFloor(%idx / 8);
         %at = %g * $Gal::GroupGapMs + (%idx % 8) * $Gal::StaggerMs;
         if (%since >= %at)
            galEnter(%cl, %i, %g);
         else
            %entered = false;
      }
   }
   // formation sway
   %swayed = false;
   if (!galChallenge(%cl) && %now >= %cl.galSwayAt)
   {
      %cl.galSwayAt = %now + $Gal::SwayStepMs;
      %cl.galSway += %cl.galSwayDir * $Gal::SwayStep;
      if (%cl.galSway >= $Gal::SwayMax)
         %cl.galSwayDir = -1;
      if (%cl.galSway <= -$Gal::SwayMax)
         %cl.galSwayDir = 1;
      %swayed = true;
   }
   %cantAttack = false;
   for (%i = 0; %i < $Gal::Enemies; %i++)
   {
      %st = $Gal::St[%seat, %i];
      if (%st == 0 || %st == 1)
      {
         if (%st == 1)
            %cantAttack = true;
         continue;
      }
      if (%st == 2)
      {
         %cantAttack = true;
         $Gal::T[%seat, %i] += %dt;
         %dur = $Gal::EntryDur;
         if (galChallenge(%cl))
            %dur = $Gal::EntryDur * 1.5;
         %t = $Gal::T[%seat, %i] / %dur;
         if (!galChallenge(%cl))
         {
            $Gal::P2U[%seat, %i] = galSlotU(%cl, %i);
            $Gal::P2V[%seat, %i] = galSlotV(%i);
         }
         if (%t >= 1)
         {
            if (galChallenge(%cl))
            {
               $Gal::St[%seat, %i] = 0;
               galPark($Gal::E[%seat, %i]);
               %cl.galLeft--;
            }
            else
            {
               $Gal::St[%seat, %i] = 3;
               galSetPos(%cl, %i, galSlotU(%cl, %i), galSlotV(%i));
            }
            continue;
         }
         %p = galCurve(%seat, %i, %t);
         galSetPos(%cl, %i, getWord(%p, 0), getWord(%p, 1));
      }
      else if (%st == 3)
      {
         if (%swayed)
            galSetPos(%cl, %i, galSlotU(%cl, %i), galSlotV(%i));
      }
      else if (%st == 4)
      {
         $Gal::T[%seat, %i] += %dt;
         %ph = $Gal::Ph[%seat, %i];
         if (%ph == 0)
         {
            %t = $Gal::T[%seat, %i] / $Gal::DiveDur;
            if (%t >= 1)
            {
               %ph = 1;
               $Gal::Ph[%seat, %i] = 1;
               $Gal::T[%seat, %i] = 0;
               $Gal::P0U[%seat, %i] = $Gal::P2U[%seat, %i];
               $Gal::P0V[%seat, %i] = $Gal::P2V[%seat, %i];
            }
            else
            {
               if (!$Gal::Fired[%seat, %i] && $Gal::T[%seat, %i] >= $Gal::FireAt[%seat, %i] && %cl.galState $= "play")
               {
                  $Gal::Fired[%seat, %i] = true;
                  galFireBomb(%cl, %i);
               }
               %p = galCurve(%seat, %i, %t);
               galSetPos(%cl, %i, getWord(%p, 0), getWord(%p, 1));
            }
         }
         if (%ph == 1)
         {
            // straight off the bottom, then back in from the top
            %t = $Gal::T[%seat, %i] / 0.4;
            if (%t >= 1)
            {
               $Gal::St[%seat, %i] = 5;
               $Gal::T[%seat, %i] = 0;
            }
            else
               galSetPos(%cl, %i, $Gal::P0U[%seat, %i], $Gal::P0V[%seat, %i] - %t * 6);
         }
      }
      else if (%st == 5)
      {
         $Gal::T[%seat, %i] += %dt;
         %t = $Gal::T[%seat, %i] / 1.1;
         if (%t >= 1)
         {
            $Gal::St[%seat, %i] = 3;
            galSetPos(%cl, %i, galSlotU(%cl, %i), galSlotV(%i));
         }
         else
            galSetPos(%cl, %i, galSlotU(%cl, %i), $Gal::Height + 1.5 + (galSlotV(%i) - $Gal::Height - 1.5) * %t);
      }
   }
   // after the entry, single enemies start diving
   if (!galChallenge(%cl) && !%cantAttack && %entered && %cl.galState $= "play")
   {
      if (%cl.galAttackAt == 0)
         %cl.galAttackAt = %now + 1200;
      if (%now >= %cl.galAttackAt)
      {
         %gap = 2400 - %cl.galStage * 180;
         if (%gap < 700)
            %gap = 700;
         %cl.galAttackAt = %now + %gap * (0.7 + getRandom() * 0.6);
         galAttack(%cl);
      }
   }
}

function galPhysics(%cl, %dt, %now)
{
   %seat = %cl.galSeat;
   // the ship
   %dir = 0;
   if (%cl.galPadMask & 1)
      %dir = -1;
   else if (%cl.galPadMask & 2)
      %dir = 1;
   %max = $Gal::HalfW - $Gal::ShipW / 2;
   if (%cl.galState $= "play")
   {
      %cl.galU += %dir * $Gal::ShipSpeed * %dt;
      if (%cl.galU > %max) %cl.galU = %max;
      if (%cl.galU < -%max) %cl.galU = -%max;
   }
   if (%cl.galState $= "over")
   {
      if (%now >= %cl.galOverAt)
         galStartGame(%cl);
      return;
   }
   if (%cl.galState $= "result")
   {
      if (%now >= %cl.galResultAt)
      {
         %cl.galStage++;
         galNewStage(%cl);
      }
      return;
   }
   if (%cl.galState $= "dead" && %now >= %cl.galRespawnAt)
   {
      %cl.galState = "play";
      galHud(%cl);
   }
   galEnemies(%cl, %dt, %now);
   // the ship's shots
   for (%k = 0; %k < $Gal::MaxShots; %k++)
   {
      if (!$Gal::ShotOn[%seat, %k])
         continue;
      for (%sub = 0; %sub < 2 && $Gal::ShotOn[%seat, %k]; %sub++)
      {
         $Gal::ShotV[%seat, %k] += $Gal::ShotSpeed * %dt / 2;
         %u = $Gal::ShotU[%seat, %k];
         %v = $Gal::ShotV[%seat, %k];
         if (%v > $Gal::Height - 0.6)
         {
            $Gal::ShotOn[%seat, %k] = false;
            break;
         }
         for (%i = 0; %i < $Gal::Enemies; %i++)
         {
            %st = $Gal::St[%seat, %i];
            if (%st < 2)
               continue;
            if (mAbs(%u - $Gal::EU[%seat, %i]) <= $Gal::EnW / 2 + 0.2 && mAbs(%v - $Gal::EV[%seat, %i]) <= $Gal::EnH / 2 + 0.6)
            {
               $Gal::ShotOn[%seat, %k] = false;
               galKillEnemy(%cl, %i);
               break;
            }
         }
      }
      if ($Gal::ShotOn[%seat, %k])
         galPlace($Gal::Shot[%seat, %k], %seat, %u, %v, 0.4, 1.2);
      else
         galPark($Gal::Shot[%seat, %k]);
   }
   // the stage is over when no enemy is left
   if (%cl.galLeft <= 0)
   {
      galClearShots(%cl);
      if (galChallenge(%cl))
      {
         if (%cl.galHits >= $Gal::Enemies)
            %cl.galScore += 10000;
         %cl.galState = "result";
         %cl.galResultAt = %now + $Gal::NextStageMs + 1500;
      }
      else
      {
         %cl.galState = "result";
         %cl.galResultAt = %now + $Gal::NextStageMs;
         %cl.galHits = -1;
      }
      if (%cl.galHits < 0)
         bottomPrint(%cl, "STAGE " @ %cl.galStage @ " CLEARED    SCORE " @ %cl.galScore, 0, 1);
      else
         galHud(%cl);
      return;
   }
   if (%cl.galState !$= "play")
   {
      if (%cl.galState $= "dead")
         galPark(%cl.galShip);
      return;
   }
   // the enemies' shots
   for (%k = 0; %k < $Gal::MaxBombs; %k++)
   {
      if (!$Gal::BombOn[%seat, %k])
         continue;
      $Gal::BombV[%seat, %k] -= $Gal::BombSpeed * %dt;
      $Gal::BombU[%seat, %k] += $Gal::BombDU[%seat, %k] * %dt;
      %bu = $Gal::BombU[%seat, %k];
      %bv = $Gal::BombV[%seat, %k];
      if (%bv < 0.5 || mAbs(%bu) > $Gal::HalfW)
      {
         $Gal::BombOn[%seat, %k] = false;
         galPark($Gal::Bomb[%seat, %k]);
         continue;
      }
      if (%bv <= $Gal::ShipV + $Gal::ShipH / 2 + 0.4 && %bv >= $Gal::ShipV - 1 && mAbs(%bu - %cl.galU) <= $Gal::ShipW / 2 + 0.3)
      {
         galPlayerHit(%cl);
         return;
      }
      galPlace($Gal::Bomb[%seat, %k], %seat, %bu, %bv, 0.5, 1.0);
   }
   // a diving enemy that reaches the ship takes it down with it
   if (!galChallenge(%cl))
      for (%i = 0; %i < $Gal::Enemies; %i++)
      {
         %st = $Gal::St[%seat, %i];
         if (%st != 4 && %st != 2)
            continue;
         if (mAbs($Gal::EU[%seat, %i] - %cl.galU) <= ($Gal::EnW + $Gal::ShipW) / 2 - 0.3
            && mAbs($Gal::EV[%seat, %i] - $Gal::ShipV) <= ($Gal::EnH + $Gal::ShipH) / 2 - 0.2)
         {
            galKillEnemy(%cl, %i);
            galPlayerHit(%cl);
            return;
         }
      }
   galPlace(%cl.galShip, %seat, %cl.galU, $Gal::ShipV, $Gal::ShipW, $Gal::ShipH);
}

// ---------------------------------------------------------------------------------------------------------
// input: the player's keys, read by Retro.cs from its (tiny) velocity; 1 while strafe-left is held, 2 for right

function galReadPad(%cl)
{
   %m = retroPad(%cl);
   %out = 0;
   if (%m & 4)
      %out = %out | 1;
   if (%m & 8)
      %out = %out | 2;
   return %out;
}

function galTick()
{
   cancel($Gal::TickThread);
   $Gal::TickThread = schedule($Gal::TickMs, 0, galTick);
   if (!isObject(MissionCleanup))
      return;
   $Gal::TickN++;
   %now = getSimTime();
   %dt = $Gal::LastTick $= "" ? 0.032 : (%now - $Gal::LastTick) / 1000;
   $Gal::LastTick = %now;
   if (%dt > 0.1) %dt = 0.1;
   for (%i = 0; %i < ClientGroup.getCount(); %i++)
   {
      %cl = ClientGroup.getObject(%i);
      if (!%cl.galJoined || !isObject(%cl.camera))
         continue;
      if (%cl.galSeat $= "")
      {
         // spectators have the stock free-fly camera; the stand-in Player is parked out of the way
         if (isObject(%cl.player))
         {
            %cl.player.setVelocity("0 0 0");
            if ($Gal::TickN % 30 == 0)
               %cl.player.setTransform("0 -150 300 1 0 0 0");
         }
         if (%cl.getControlObject() != %cl.camera)
            %cl.setControlObject(%cl.camera);
         continue;
      }
      %cl.galPadMask = galReadPad(%cl);
      galPhysics(%cl, %dt, %now);
   }
}

function galInitPlayer(%cl)
{
   %cl.galJoined = true;
   if (%cl.galSeat $= "")
   {
      %seat = galFreeSeat();
      if (%seat !$= "")
      {
         %cl.galSeat = %seat;
         galMakeBoard(%cl);
      }
   }
   if (%cl.galSeat $= "")
   {
      // a spectator: the free-fly camera, parked player
      %cam = %cl.camera;
      %cam.setFlyMode();
      %cam.setTransform(galAnchor(0));
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
   %a = galAnchor(%cl.galSeat);
   retroAttach(%cl, getWord(%a, 0), getWord(%a, 1), getWord(%a, 2));
   commandToClient(%cl, 'setHudMode', 'Standard');
   galStartGame(%cl);
}

function galFreeClient(%cl)
{
   retroDetach(%cl);
   if (isObject(%cl.galGroup))
      %cl.galGroup.delete();
   %cl.galGroup = "";
   %cl.galSeat = "";
   %cl.galJoined = false;
}

// Switch the package on and off with the mission, so stock DM missions are untouched.
function galWatch()
{
   cancel($Gal::WatchThread);
   $Gal::WatchThread = schedule(250, 0, galWatch);
   %on = ($CurrentMission $= $Gal::Mission && isObject(MissionCleanup));
   if (%on && !$Gal::PkgOn)
   {
      activatePackage(T2LaunchGalaga);
      $Gal::PkgOn = true;
      $Gal::LastTick = "";
      galTick();
      echo("Galaga ruleset on");
   }
   else if (!%on && $Gal::PkgOn)
   {
      deactivatePackage(T2LaunchGalaga);
      $Gal::PkgOn = false;
      cancel($Gal::TickThread);
      echo("Galaga ruleset off");
   }
}

package T2LaunchGalagaBoot
{
   function CreateServer(%mission, %missionType)
   {
      Parent::CreateServer(%mission, %missionType);
      galDatablocks();
   }
};

package T2LaunchGalaga
{
   function DMGame::equip(%game, %player)
   {
      // nobody carries anything
   }

   function DMGame::createPlayer(%game, %client, %spawnLoc, %respawn)
   {
      Parent::createPlayer(%game, %client, %spawnLoc, %respawn);
      galInitPlayer(%client);
   }

   function DMGame::onClientKilled(%game, %clVictim, %clKiller, %damageType, %implement, %damageLoc)
   {
      // the stand-in Player is not part of the game: no respawn timer or orbit camera
      %clVictim.deaths++;
   }

   function DMGame::ObserverOnTrigger(%game, %data, %obj, %trigger, %state)
   {
      %client = %obj.getControllingClient();
      if (%client && %client.galJoined)
      {
         if (%state && %trigger == 0 && %client.galSeat !$= "")
            galFire(%client);
         return false;
      }
      return Parent::ObserverOnTrigger(%game, %data, %obj, %trigger, %state);
   }

   // a seated player is a Player now, so its triggers arrive here: fire shoots
   function Armor::onTrigger(%data, %player, %trigger, %val)
   {
      %client = %player.getControllingClient();
      if (%client && %client.galJoined)
      {
         if (%val && %trigger == 0 && %client.galSeat !$= "")
            galFire(%client);
         return;
      }
      Parent::onTrigger(%data, %player, %trigger, %val);
   }

   function DMGame::onClientLeaveGame(%game, %client)
   {
      galFreeClient(%client);
      Parent::onClientLeaveGame(%game, %client);
   }
};

if (!$Gal::Installed)
{
   $Gal::Installed = true;
   activatePackage(T2LaunchGalagaBoot);
   galTables();
   galDatablocks();
   galWatch();
   echo("Galaga gametype script loaded");
}
