// GalagaReal: server-side ruleset for the MatrixGalagaReal mission, Galaga with real game models. The same rules as Galaga.cs
// (swooping entries, a swaying formation, dives, challenging stages), but the enemies are Bioderm players (heavy bosses,
// medium butterflies, light bees), the ship is a Shrike, the shots are grenade items and the bombs are mortar ammo items.
// No shape datablock is defined (one defined by a script needs retroRefreshCrc() from Retro.cs, or the server's CRC
// challenge drops the client): everything is a stock datablock, and the only datablock made here is a pale force-field backdrop so
// the dark models show.
// Evaluated in memory by the t2launch hook (server.gametype_script, after Retro.cs). The package is active only while
// $CurrentMission is MatrixGalagaReal.
//
$Glr::Mission = "MatrixGalagaReal";
$Glr::HalfW = 18;                    // court half-width in metres
$Glr::Height = 36;
$Glr::Z0 = 300;                      // world height of the court floor
$Glr::Spacing = 44;                  // between boards
$Glr::Depth = 2;
$Glr::Seats = 4;
$Glr::SeatOffset[0] = 0;  $Glr::SeatOffset[1] = 1;  $Glr::SeatOffset[2] = -1; $Glr::SeatOffset[3] = 2;
$Glr::ShipV = 5.2;                     // ship centre height
$Glr::ShipW = 2.6;
$Glr::ShipH = 1.4;
$Glr::ShipSpeed = 20;
$Glr::MaxShots = 2;
$Glr::ShotSpeed = 40;
$Glr::MaxBombs = 4;
$Glr::BombSpeed = 13;
$Glr::Enemies = 36;
$Glr::EnW = 1.8;
$Glr::EnH = 1.3;
$Glr::SlotDU = 2.6;                  // formation spacing
$Glr::SlotDV = 2.3;
$Glr::BaseV = 31;                    // height of the top formation row
$Glr::SwayMax = 3;
$Glr::SwayStepMs = 140;
$Glr::SwayStep = 0.5;
$Glr::EntryDur = 2.4;                // seconds a swoop takes
$Glr::GroupGapMs = 1300;
$Glr::StaggerMs = 180;
$Glr::DiveDur = 2.4;
$Glr::Lives = 3;
$Glr::RespawnMs = 1500;
$Glr::GameOverMs = 4500;
$Glr::NextStageMs = 2200;
$Glr::CamDist = 46;
$Glr::CamZShift = -1.5;
$Glr::TickMs = 32;
$Glr::ShipScale = 0.2;               // the Shrike is about 12 m across with its wings out
$Glr::ShotScale = 7;
$Glr::BombScale = 5;
$Glr::BackY = 4;                     // the backdrop sits well behind everything
$Glr::ParkX = 3000;                  // inactive blocks wait far outside every client's scope
$Glr::ParkY = 3000;
$Glr::Debug = 0;

// enemy i: row 0 holds the 4 bosses (columns 2..5), rows 1..4 hold 8 each; the entry order is the butterflies, then
// the bosses with the next butterflies, then the bees
function glrTables()
{
   $Glr::BackK = ($Glr::CamDist + $Glr::BackY + 0.25) / ($Glr::CamDist - 1);
   for (%i = 0; %i < 36; %i++)
   {
      if (%i < 4)
      {
         $Glr::ERow[%i] = 0;
         $Glr::ECol[%i] = %i + 2;
      }
      else
      {
         $Glr::ERow[%i] = mFloor((%i - 4) / 8) + 1;
         $Glr::ECol[%i] = (%i - 4) % 8;
      }
   }
   %o = "";
   for (%i = 4; %i < 12; %i++)
      %o = %o SPC %i;
   for (%i = 0; %i < 4; %i++)
      %o = %o SPC %i;
   for (%i = 12; %i < 36; %i++)
      %o = %o SPC %i;
   $Glr::Order = trim(%o);
}

// ---------------------------------------------------------------------------------------------------------
// datablocks: deleted by DestroyServer, so defined at install and again after every CreateServer

function glrDatablocks()
{
   if (isObject(GlrBack))
      return;
   eval("datablock ForceFieldBareData(GlrBack) : defaultForceFieldBare { framesPerSec = 1; numFrames = 1; scrollSpeed = 0; color = \"0.72 0.8 0.95\"; baseTranslucency = 0.22;"
      @ " needsNoPower = true; };");
}

function glrField(%db, %x, %y, %z, %sx, %sy, %sz)
{
   return new ForceFieldBare() {
      dataBlock = %db;
      position = %x SPC %y SPC %z;
      rotation = "1 0 0 0";
      scale = %sx SPC %sy SPC %sz;
   };
}

// a stock player model as an actor: %scale, %armor; it hangs where it is put (no gravity matters: it is re-placed every tick)
function glrActor(%armor, %scale)
{
   %p = new Player() {
      dataBlock = %armor;
   };
   %p.setScale(%scale SPC %scale SPC %scale);
   %p.glrHalf = %scale * 1.25;          // from its feet to its middle
   %p.glrRot = "0 0 1 3.1416";          // facing the viewer
   return %p;
}

// a stock item as a prop (shots and bombs)
function glrItem(%db, %scale)
{
   %o = new Item() {
      dataBlock = %db;
      position = "0 -300 315.1";
      rotation = "1 0 0 0";
      static = true;
      rotate = false;
   };
   %o.glrProp = true;
   %o.setScale(%scale SPC %scale SPC %scale);
   %o.glrHalf = 0;
   %o.glrRot = "1 0 0 0";
   return %o;
}

function glrPurgeZones()
{
   %z = nameToID("MissionCleanup/PZones");
   if (%z > 0)
      %z.delete();
}

// ---------------------------------------------------------------------------------------------------------
// geometry: court coordinates are (u, v) = across from the centre line, and up from the floor

function glrBoardX(%seat)
{
   return $Glr::SeatOffset[%seat] * $Glr::Spacing;
}

function glrAnchor(%seat)
{
   return glrBoardX(%seat) SPC (-$Glr::CamDist) SPC ($Glr::Z0 + $Glr::Height / 2 + $Glr::CamZShift) SPC "1 0 0 0";
}

// an object's middle at (%u, %v) on the board
function glrPlace(%obj, %seat, %u, %v, %w, %h)
{
   %obj.setTransform((glrBoardX(%seat) + %u) SPC 0 SPC ($Glr::Z0 + %v - %obj.glrHalf) SPC %obj.glrRot);
   %obj.setVelocity("0 0 0");           // gravity would otherwise build up between two placements
}

// parked objects wait behind the camera: in scope (so the client has them) but out of sight
function glrPark(%obj)
{
   %obj.setTransform("0 -300 315.1");
   %obj.setVelocity("0 0 0");
}

// nothing in this mission is solid, so a parked player or vehicle would fall forever and be destroyed out of bounds: they
// park on a solid slab behind the camera instead (RetroFloor is Retro.cs's invisible solid floor)
function glrParkingFloor()
{
   if (isObject($Glr::ParkFloor))
      return;
   retroDatablocks();
   $Glr::ParkFloor = new ForceFieldBare() {
      dataBlock = RetroFloor;
      position = "-40 -340 314";
      rotation = "1 0 0 0";
      scale = "80 80 1";
   };
   MissionCleanup.add($Glr::ParkFloor);
}

function glrParkShip(%cl)
{
   %cl.glrShip.setTransform("0 -300 335" SPC %cl.glrShip.glrRot);
   %cl.glrShip.setVelocity("0 0 0");
}

// the ship stays where the game says it is, every tick, whatever the game is doing
function glrHoldShip(%cl)
{
   // parked in the air above the slab (a vehicle left overlapping the slab is wrecked); it is re-placed every tick, so it
   // does not need to stand on anything
   if (%cl.glrState $= "dead" || %cl.glrState $= "over")
      glrParkShip(%cl);
   else
      glrPlace(%cl.glrShip, %cl.glrSeat, %cl.glrU, $Glr::ShipV, 0, 0);
}

function glrSlotU(%cl, %i)
{
   return ($Glr::ECol[%i] - 3.5) * $Glr::SlotDU + %cl.glrSway;
}

function glrSlotV(%i)
{
   return $Glr::BaseV - $Glr::ERow[%i] * $Glr::SlotDV;
}

function glrKind(%i)
{
   if ($Glr::ERow[%i] == 0)
      return "GalBoss";
   if ($Glr::ERow[%i] <= 2)
      return "GalFly";
   return "GalBee";
}

function glrPoints(%i, %diving)
{
   if ($Glr::ERow[%i] == 0)
      return %diving ? 400 : 150;
   if ($Glr::ERow[%i] <= 2)
      return %diving ? 160 : 80;
   return %diving ? 100 : 50;
}

function glrFreeSeat()
{
   for (%s = 0; %s < $Glr::Seats; %s++)
   {
      %used = false;
      for (%i = 0; %i < ClientGroup.getCount(); %i++)
         if (ClientGroup.getObject(%i).glrSeat $= %s)
            %used = true;
      if (!%used)
         return %s;
   }
   return "";
}

// ---------------------------------------------------------------------------------------------------------
// one board

function glrMakeBoard(%cl)
{
   %seat = %cl.glrSeat;
   %g = new SimGroup();
   MissionCleanup.add(%g);
   %cl.glrGroup = %g;
   glrParkingFloor();
   %bx = glrBoardX(%seat);
   // the backdrop: one pale panel behind the court, scaled up about the view centre to line up with it
   %k = $Glr::BackK;
   %vc = $Glr::Height / 2 + $Glr::CamZShift;
   %g.add(glrField(GlrBack, %bx - ($Glr::HalfW + 1) * %k, $Glr::BackY, $Glr::Z0 + %vc + (-1 - %vc) * %k, ($Glr::HalfW * 2 + 2) * %k, 0.5, ($Glr::Height + 2) * %k));
   // the ship: a Shrike seen from above (nose up, top toward the viewer)
   %cl.glrShip = new FlyingVehicle() {
      dataBlock = "ScoutFlyer";
      position = "0 -300 335";
      rotation = "1 0 0 -1.5708";
   };
   %cl.glrShip.setScale($Glr::ShipScale SPC $Glr::ShipScale SPC $Glr::ShipScale);
   %cl.glrShip.glrHalf = 0;
   %cl.glrShip.glrRot = "1 0 0 -1.5708";
   %g.add(%cl.glrShip);
   glrParkShip(%cl);
   for (%k = 0; %k < $Glr::MaxShots; %k++)
   {
      %o = glrItem("Grenade", $Glr::ShotScale);
      %g.add(%o);
      glrPark(%o);
      $Glr::Shot[%seat, %k] = %o;
      $Glr::ShotOn[%seat, %k] = false;
   }
   for (%k = 0; %k < $Glr::MaxBombs; %k++)
   {
      %o = glrItem("MortarAmmo", $Glr::BombScale);
      %g.add(%o);
      glrPark(%o);
      $Glr::Bomb[%seat, %k] = %o;
      $Glr::BombOn[%seat, %k] = false;
   }
   for (%i = 0; %i < $Glr::Enemies; %i++)
   {
      if ($Glr::ERow[%i] == 0)
         %o = glrActor("HeavyMaleBiodermArmor", 0.62);
      else if ($Glr::ERow[%i] <= 2)
         %o = glrActor("MediumMaleBiodermArmor", 0.7);
      else
         %o = glrActor("LightMaleBiodermArmor", 0.7);
      %g.add(%o);
      glrPark(%o);
      $Glr::E[%seat, %i] = %o;
      $Glr::St[%seat, %i] = 0;
   }
   glrPurgeZones();
}

function glrStartGame(%cl)
{
   %cl.glrScore = 0;
   %cl.glrLives = $Glr::Lives;
   %cl.glrStage = 1;
   %cl.glrU = 0;
   glrNewStage(%cl);
}

function glrChallenge(%cl)
{
   return %cl.glrStage % 4 == 3;
}

function glrNewStage(%cl)
{
   %seat = %cl.glrSeat;
   glrClearShots(%cl);
   %cl.glrSway = 0;
   %cl.glrSwayDir = 1;
   %cl.glrSwayAt = getSimTime() + 1000;
   %cl.glrLeft = $Glr::Enemies;
   %cl.glrHits = 0;
   %cl.glrStageAt = getSimTime() + 900;
   %cl.glrAttackAt = 0;
   for (%i = 0; %i < $Glr::Enemies; %i++)
   {
      $Glr::St[%seat, %i] = 1;                  // waiting to enter
      glrPark($Glr::E[%seat, %i]);
   }
   %cl.glrState = "play";
   glrHud(%cl);
}

function glrClearShots(%cl)
{
   %seat = %cl.glrSeat;
   for (%k = 0; %k < $Glr::MaxShots; %k++)
   {
      $Glr::ShotOn[%seat, %k] = false;
      glrPark($Glr::Shot[%seat, %k]);
   }
   for (%k = 0; %k < $Glr::MaxBombs; %k++)
   {
      $Glr::BombOn[%seat, %k] = false;
      glrPark($Glr::Bomb[%seat, %k]);
   }
}

function glrHud(%cl)
{
   if (%cl.glrState $= "over")
   {
      bottomPrint(%cl, "GAME OVER    SCORE " @ %cl.glrScore, 0, 1);
      return;
   }
   if (%cl.glrState $= "result")
   {
      if (%cl.glrHits >= $Glr::Enemies)
         bottomPrint(%cl, "PERFECT!  " @ %cl.glrHits @ " of " @ $Glr::Enemies @ " hit    BONUS 10000", 0, 1);
      else
         bottomPrint(%cl, "CHALLENGING STAGE OVER    " @ %cl.glrHits @ " of " @ $Glr::Enemies @ " hit", 0, 1);
      return;
   }
   %tag = glrChallenge(%cl) ? "   CHALLENGING STAGE" : "";
   bottomPrint(%cl, "SCORE " @ %cl.glrScore @ "    LIVES " @ %cl.glrLives @ "    STAGE " @ %cl.glrStage @ %tag @ "     strafe = move, fire = shoot", 0, 1);
}

// ---------------------------------------------------------------------------------------------------------
// enemy movement

// the point at time %t (0..1) of the quadratic curve stored for enemy %i
function glrCurve(%seat, %i, %t)
{
   %a = (1 - %t) * (1 - %t);
   %b = 2 * (1 - %t) * %t;
   %c = %t * %t;
   %u = %a * $Glr::P0U[%seat, %i] + %b * $Glr::P1U[%seat, %i] + %c * $Glr::P2U[%seat, %i];
   %v = %a * $Glr::P0V[%seat, %i] + %b * $Glr::P1V[%seat, %i] + %c * $Glr::P2V[%seat, %i];
   return %u SPC %v;
}

function glrSetPos(%cl, %i, %u, %v)
{
   %seat = %cl.glrSeat;
   $Glr::EU[%seat, %i] = %u;
   $Glr::EV[%seat, %i] = %v;
   glrPlace($Glr::E[%seat, %i], %seat, %u, %v, $Glr::EnW, $Glr::EnH);
}

// send enemy %i swooping in from a top corner; group %g decides the side and the shape of the swoop
function glrEnter(%cl, %i, %g)
{
   %seat = %cl.glrSeat;
   %side = 1;
   if (%g % 2 == 0)
      %side = -1;
   $Glr::St[%seat, %i] = 2;
   $Glr::T[%seat, %i] = 0;
   $Glr::P0U[%seat, %i] = %side * ($Glr::HalfW + 1.5);
   $Glr::P0V[%seat, %i] = $Glr::Height + 1.5;
   $Glr::P1U[%seat, %i] = -%side * ($Glr::HalfW - 3);
   $Glr::P1V[%seat, %i] = 10 + (%g % 3) * 4;
   if (glrChallenge(%cl))
   {
      // no formation: leave through the bottom on the far side
      $Glr::P2U[%seat, %i] = %side * ($Glr::ECol[%i] - 3.5) * 2;
      $Glr::P2V[%seat, %i] = -3;
   }
   else
   {
      $Glr::P2U[%seat, %i] = glrSlotU(%cl, %i);
      $Glr::P2V[%seat, %i] = glrSlotV(%i);
   }
   glrSetPos(%cl, %i, $Glr::P0U[%seat, %i], $Glr::P0V[%seat, %i]);
}

function glrDive(%cl, %i, %offset)
{
   %seat = %cl.glrSeat;
   %u = $Glr::EU[%seat, %i];
   %swing = 12;
   if (%u > 0)
      %swing = -12;
   %p1 = %u + %swing;
   if (%p1 > $Glr::HalfW - 1) %p1 = $Glr::HalfW - 1;
   if (%p1 < -$Glr::HalfW + 1) %p1 = -$Glr::HalfW + 1;
   %p2 = %cl.glrU * 0.7 + (getRandom() - 0.5) * 8 + %offset;
   if (%p2 > $Glr::HalfW - 1) %p2 = $Glr::HalfW - 1;
   if (%p2 < -$Glr::HalfW + 1) %p2 = -$Glr::HalfW + 1;
   $Glr::St[%seat, %i] = 4;
   $Glr::Ph[%seat, %i] = 0;
   $Glr::T[%seat, %i] = 0;
   $Glr::Fired[%seat, %i] = false;
   $Glr::FireAt[%seat, %i] = 0.5 + getRandom() * 0.6;
   $Glr::P0U[%seat, %i] = %u;
   $Glr::P0V[%seat, %i] = $Glr::EV[%seat, %i];
   $Glr::P1U[%seat, %i] = %p1;
   $Glr::P1V[%seat, %i] = 16;
   $Glr::P2U[%seat, %i] = %p2;
   $Glr::P2V[%seat, %i] = 2.5;
}

function glrDiverCount(%cl)
{
   %seat = %cl.glrSeat;
   %n = 0;
   for (%i = 0; %i < $Glr::Enemies; %i++)
      if ($Glr::St[%seat, %i] == 4 || $Glr::St[%seat, %i] == 5)
         %n++;
   return %n;
}

// one formation enemy peels off (a boss takes up to two butterflies with it)
function glrAttack(%cl)
{
   %seat = %cl.glrSeat;
   %max = 2 + mFloor(%cl.glrStage / 2);
   if (%max > 6)
      %max = 6;
   if (glrDiverCount(%cl) >= %max)
      return;
   %start = mFloor(getRandom() * 35.99);
   for (%k = 0; %k < $Glr::Enemies; %k++)
   {
      %i = (%start + %k) % $Glr::Enemies;
      if ($Glr::St[%seat, %i] != 3)
         continue;
      glrDive(%cl, %i, 0);
      if ($Glr::ERow[%i] == 0)
      {
         %c = $Glr::ECol[%i];
         %e1 = 4 + %c;
         %e2 = 4 + %c + 1;
         if ($Glr::St[%seat, %e1] == 3)
            glrDive(%cl, %e1, -2.5);
         if (%e2 < 12 && $Glr::St[%seat, %e2] == 3)
            glrDive(%cl, %e2, 2.5);
      }
      return;
   }
}

function glrFireBomb(%cl, %i)
{
   %seat = %cl.glrSeat;
   for (%k = 0; %k < $Glr::MaxBombs; %k++)
      if (!$Glr::BombOn[%seat, %k])
      {
         %u = $Glr::EU[%seat, %i];
         %v = $Glr::EV[%seat, %i];
         $Glr::BombOn[%seat, %k] = true;
         $Glr::BombU[%seat, %k] = %u;
         $Glr::BombV[%seat, %k] = %v - 1;
         %du = (%cl.glrU - %u) / ((%v - $Glr::ShipV) / $Glr::BombSpeed);
         if (%du > 7) %du = 7;
         if (%du < -7) %du = -7;
         $Glr::BombDU[%seat, %k] = %du;
         return;
      }
}

// ---------------------------------------------------------------------------------------------------------
// the game

function glrKillEnemy(%cl, %i)
{
   %seat = %cl.glrSeat;
   %st = $Glr::St[%seat, %i];
   if (glrChallenge(%cl))
   {
      %cl.glrScore += 100;
      %cl.glrHits++;
   }
   else
      %cl.glrScore += glrPoints(%i, %st == 4 || %st == 5);
   $Glr::St[%seat, %i] = 0;
   glrPark($Glr::E[%seat, %i]);
   %cl.glrLeft--;
   glrHud(%cl);
}

function glrPlayerHit(%cl)
{
   %cl.glrLives--;
   glrClearShots(%cl);
   glrParkShip(%cl);
   if (%cl.glrLives <= 0)
   {
      glrGameOver(%cl);
      return;
   }
   %cl.glrState = "dead";
   %cl.glrRespawnAt = getSimTime() + $Glr::RespawnMs;
   glrHud(%cl);
}

function glrGameOver(%cl)
{
   if (%cl.glrState $= "over")
      return;
   %cl.glrState = "over";
   %cl.glrOverAt = getSimTime() + $Glr::GameOverMs;
   if (%cl.glrScore > %cl.glrHigh)
      %cl.glrHigh = %cl.glrScore;
   messageAll('MsgGalagaReal', '\c1%1 is done (%2 points).', %cl.name, %cl.glrScore);
   // everything left on the board goes away
   for (%i = 0; %i < $Glr::Enemies; %i++)
   {
      $Glr::St[%cl.glrSeat, %i] = 0;
      glrPark($Glr::E[%cl.glrSeat, %i]);
   }
   glrHud(%cl);
}

function glrFire(%cl)
{
   if (%cl.glrState !$= "play")
      return;
   %seat = %cl.glrSeat;
   for (%k = 0; %k < $Glr::MaxShots; %k++)
      if (!$Glr::ShotOn[%seat, %k])
      {
         $Glr::ShotOn[%seat, %k] = true;
         $Glr::ShotU[%seat, %k] = %cl.glrU;
         $Glr::ShotV[%seat, %k] = $Glr::ShipV + $Glr::ShipH / 2 + 0.6;
         return;
      }
}

// the enemies: entry, formation, dives
function glrEnemies(%cl, %dt, %now)
{
   %seat = %cl.glrSeat;
   // enemies enter one after another in groups of eight
   %since = %now - %cl.glrStageAt;
   %n = 0;
   %k = 0;
   %entered = true;
   for (%idx = 0; %idx < getWordCount($Glr::Order); %idx++)
   {
      %i = getWord($Glr::Order, %idx);
      if ($Glr::St[%seat, %i] == 1)
      {
         %g = mFloor(%idx / 8);
         %at = %g * $Glr::GroupGapMs + (%idx % 8) * $Glr::StaggerMs;
         if (%since >= %at)
            glrEnter(%cl, %i, %g);
         else
            %entered = false;
      }
   }
   // formation sway
   %swayed = false;
   if (!glrChallenge(%cl) && %now >= %cl.glrSwayAt)
   {
      %cl.glrSwayAt = %now + $Glr::SwayStepMs;
      %cl.glrSway += %cl.glrSwayDir * $Glr::SwayStep;
      if (%cl.glrSway >= $Glr::SwayMax)
         %cl.glrSwayDir = -1;
      if (%cl.glrSway <= -$Glr::SwayMax)
         %cl.glrSwayDir = 1;
      %swayed = true;
   }
   %cantAttack = false;
   for (%i = 0; %i < $Glr::Enemies; %i++)
   {
      %st = $Glr::St[%seat, %i];
      if (%st == 0 || %st == 1)
      {
         if (%st == 1)
            %cantAttack = true;
         continue;
      }
      if (%st == 2)
      {
         %cantAttack = true;
         $Glr::T[%seat, %i] += %dt;
         %dur = $Glr::EntryDur;
         if (glrChallenge(%cl))
            %dur = $Glr::EntryDur * 1.5;
         %t = $Glr::T[%seat, %i] / %dur;
         if (!glrChallenge(%cl))
         {
            $Glr::P2U[%seat, %i] = glrSlotU(%cl, %i);
            $Glr::P2V[%seat, %i] = glrSlotV(%i);
         }
         if (%t >= 1)
         {
            if (glrChallenge(%cl))
            {
               $Glr::St[%seat, %i] = 0;
               glrPark($Glr::E[%seat, %i]);
               %cl.glrLeft--;
            }
            else
            {
               $Glr::St[%seat, %i] = 3;
               glrSetPos(%cl, %i, glrSlotU(%cl, %i), glrSlotV(%i));
            }
            continue;
         }
         %p = glrCurve(%seat, %i, %t);
         glrSetPos(%cl, %i, getWord(%p, 0), getWord(%p, 1));
      }
      else if (%st == 3)
      {
         if (%swayed)
            glrSetPos(%cl, %i, glrSlotU(%cl, %i), glrSlotV(%i));
      }
      else if (%st == 4)
      {
         $Glr::T[%seat, %i] += %dt;
         %ph = $Glr::Ph[%seat, %i];
         if (%ph == 0)
         {
            %t = $Glr::T[%seat, %i] / $Glr::DiveDur;
            if (%t >= 1)
            {
               %ph = 1;
               $Glr::Ph[%seat, %i] = 1;
               $Glr::T[%seat, %i] = 0;
               $Glr::P0U[%seat, %i] = $Glr::P2U[%seat, %i];
               $Glr::P0V[%seat, %i] = $Glr::P2V[%seat, %i];
            }
            else
            {
               if (!$Glr::Fired[%seat, %i] && $Glr::T[%seat, %i] >= $Glr::FireAt[%seat, %i] && %cl.glrState $= "play")
               {
                  $Glr::Fired[%seat, %i] = true;
                  glrFireBomb(%cl, %i);
               }
               %p = glrCurve(%seat, %i, %t);
               glrSetPos(%cl, %i, getWord(%p, 0), getWord(%p, 1));
            }
         }
         if (%ph == 1)
         {
            // straight off the bottom, then back in from the top
            %t = $Glr::T[%seat, %i] / 0.4;
            if (%t >= 1)
            {
               $Glr::St[%seat, %i] = 5;
               $Glr::T[%seat, %i] = 0;
            }
            else
               glrSetPos(%cl, %i, $Glr::P0U[%seat, %i], $Glr::P0V[%seat, %i] - %t * 6);
         }
      }
      else if (%st == 5)
      {
         $Glr::T[%seat, %i] += %dt;
         %t = $Glr::T[%seat, %i] / 1.1;
         if (%t >= 1)
         {
            $Glr::St[%seat, %i] = 3;
            glrSetPos(%cl, %i, glrSlotU(%cl, %i), glrSlotV(%i));
         }
         else
            glrSetPos(%cl, %i, glrSlotU(%cl, %i), $Glr::Height + 1.5 + (glrSlotV(%i) - $Glr::Height - 1.5) * %t);
      }
   }
   // after the entry, single enemies start diving
   if (!glrChallenge(%cl) && !%cantAttack && %entered && %cl.glrState $= "play")
   {
      if (%cl.glrAttackAt == 0)
         %cl.glrAttackAt = %now + 1200;
      if (%now >= %cl.glrAttackAt)
      {
         %gap = 2400 - %cl.glrStage * 180;
         if (%gap < 700)
            %gap = 700;
         %cl.glrAttackAt = %now + %gap * (0.7 + getRandom() * 0.6);
         glrAttack(%cl);
      }
   }
}

function glrPhysics(%cl, %dt, %now)
{
   %seat = %cl.glrSeat;
   // the ship
   %dir = 0;
   if (%cl.glrPadMask & 1)
      %dir = -1;
   else if (%cl.glrPadMask & 2)
      %dir = 1;
   %max = $Glr::HalfW - $Glr::ShipW / 2;
   if (%cl.glrState $= "play")
   {
      %cl.glrU += %dir * $Glr::ShipSpeed * %dt;
      if (%cl.glrU > %max) %cl.glrU = %max;
      if (%cl.glrU < -%max) %cl.glrU = -%max;
   }
   if (%cl.glrState $= "over")
   {
      if (%now >= %cl.glrOverAt)
         glrStartGame(%cl);
      return;
   }
   if (%cl.glrState $= "result")
   {
      if (%now >= %cl.glrResultAt)
      {
         %cl.glrStage++;
         glrNewStage(%cl);
      }
      return;
   }
   if (%cl.glrState $= "dead" && %now >= %cl.glrRespawnAt)
   {
      %cl.glrState = "play";
      glrHud(%cl);
   }
   glrEnemies(%cl, %dt, %now);
   // the ship's shots
   for (%k = 0; %k < $Glr::MaxShots; %k++)
   {
      if (!$Glr::ShotOn[%seat, %k])
         continue;
      for (%sub = 0; %sub < 2 && $Glr::ShotOn[%seat, %k]; %sub++)
      {
         $Glr::ShotV[%seat, %k] += $Glr::ShotSpeed * %dt / 2;
         %u = $Glr::ShotU[%seat, %k];
         %v = $Glr::ShotV[%seat, %k];
         if (%v > $Glr::Height - 0.6)
         {
            $Glr::ShotOn[%seat, %k] = false;
            break;
         }
         for (%i = 0; %i < $Glr::Enemies; %i++)
         {
            %st = $Glr::St[%seat, %i];
            if (%st < 2)
               continue;
            if (mAbs(%u - $Glr::EU[%seat, %i]) <= $Glr::EnW / 2 + 0.2 && mAbs(%v - $Glr::EV[%seat, %i]) <= $Glr::EnH / 2 + 0.6)
            {
               $Glr::ShotOn[%seat, %k] = false;
               glrKillEnemy(%cl, %i);
               break;
            }
         }
      }
      if ($Glr::ShotOn[%seat, %k])
         glrPlace($Glr::Shot[%seat, %k], %seat, %u, %v, 0.4, 1.2);
      else
         glrPark($Glr::Shot[%seat, %k]);
   }
   // the stage is over when no enemy is left
   if (%cl.glrLeft <= 0)
   {
      glrClearShots(%cl);
      if (glrChallenge(%cl))
      {
         if (%cl.glrHits >= $Glr::Enemies)
            %cl.glrScore += 10000;
         %cl.glrState = "result";
         %cl.glrResultAt = %now + $Glr::NextStageMs + 1500;
      }
      else
      {
         %cl.glrState = "result";
         %cl.glrResultAt = %now + $Glr::NextStageMs;
         %cl.glrHits = -1;
      }
      if (%cl.glrHits < 0)
         bottomPrint(%cl, "STAGE " @ %cl.glrStage @ " CLEARED    SCORE " @ %cl.glrScore, 0, 1);
      else
         glrHud(%cl);
      return;
   }
   if (%cl.glrState !$= "play")
   {
      if (%cl.glrState $= "dead")
         glrParkShip(%cl);
      return;
   }
   // the enemies' shots
   for (%k = 0; %k < $Glr::MaxBombs; %k++)
   {
      if (!$Glr::BombOn[%seat, %k])
         continue;
      $Glr::BombV[%seat, %k] -= $Glr::BombSpeed * %dt;
      $Glr::BombU[%seat, %k] += $Glr::BombDU[%seat, %k] * %dt;
      %bu = $Glr::BombU[%seat, %k];
      %bv = $Glr::BombV[%seat, %k];
      if (%bv < 0.5 || mAbs(%bu) > $Glr::HalfW)
      {
         $Glr::BombOn[%seat, %k] = false;
         glrPark($Glr::Bomb[%seat, %k]);
         continue;
      }
      if (%bv <= $Glr::ShipV + $Glr::ShipH / 2 + 0.4 && %bv >= $Glr::ShipV - 1 && mAbs(%bu - %cl.glrU) <= $Glr::ShipW / 2 + 0.3)
      {
         glrPlayerHit(%cl);
         return;
      }
      glrPlace($Glr::Bomb[%seat, %k], %seat, %bu, %bv, 0.5, 1.0);
   }
   // a diving enemy that reaches the ship takes it down with it
   if (!glrChallenge(%cl))
      for (%i = 0; %i < $Glr::Enemies; %i++)
      {
         %st = $Glr::St[%seat, %i];
         if (%st != 4 && %st != 2)
            continue;
         if (mAbs($Glr::EU[%seat, %i] - %cl.glrU) <= ($Glr::EnW + $Glr::ShipW) / 2 - 0.3
            && mAbs($Glr::EV[%seat, %i] - $Glr::ShipV) <= ($Glr::EnH + $Glr::ShipH) / 2 - 0.2)
         {
            glrKillEnemy(%cl, %i);
            glrPlayerHit(%cl);
            return;
         }
      }
   glrPlace(%cl.glrShip, %seat, %cl.glrU, $Glr::ShipV, $Glr::ShipW, $Glr::ShipH);
}

// ---------------------------------------------------------------------------------------------------------
// input: the player's keys, read by Retro.cs from its (tiny) velocity; 1 while strafe-left is held, 2 for right

function glrReadPad(%cl)
{
   %m = retroPad(%cl);
   %out = 0;
   if (%m & 4)
      %out = %out | 1;
   if (%m & 8)
      %out = %out | 2;
   return %out;
}

function glrTick()
{
   cancel($Glr::TickThread);
   $Glr::TickThread = schedule($Glr::TickMs, 0, glrTick);
   if (!isObject(MissionCleanup))
      return;
   $Glr::TickN++;
   %now = getSimTime();
   %dt = $Glr::LastTick $= "" ? 0.032 : (%now - $Glr::LastTick) / 1000;
   $Glr::LastTick = %now;
   if (%dt > 0.1) %dt = 0.1;
   for (%i = 0; %i < ClientGroup.getCount(); %i++)
   {
      %cl = ClientGroup.getObject(%i);
      if (!%cl.glrJoined || !isObject(%cl.camera))
         continue;
      if (%cl.glrSeat $= "")
      {
         // spectators have the stock free-fly camera; the stand-in Player is parked out of the way
         if (isObject(%cl.player))
         {
            %cl.player.setVelocity("0 0 0");
            if ($Glr::TickN % 30 == 0)
               %cl.player.setTransform("0 -150 300 1 0 0 0");
         }
         if (%cl.getControlObject() != %cl.camera)
            %cl.setControlObject(%cl.camera);
         continue;
      }
      %cl.glrPadMask = glrReadPad(%cl);
      glrPhysics(%cl, %dt, %now);
      glrHoldShip(%cl);
   }
}

function glrInitPlayer(%cl)
{
   %cl.glrJoined = true;
   if (%cl.glrSeat $= "")
   {
      %seat = glrFreeSeat();
      if (%seat !$= "")
      {
         %cl.glrSeat = %seat;
         glrMakeBoard(%cl);
      }
   }
   if (%cl.glrSeat $= "")
   {
      // a spectator: the free-fly camera, parked player
      %cam = %cl.camera;
      %cam.setFlyMode();
      %cam.setTransform(glrAnchor(0));
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
   %a = glrAnchor(%cl.glrSeat);
   retroAttach(%cl, getWord(%a, 0), getWord(%a, 1), getWord(%a, 2));
   commandToClient(%cl, 'setHudMode', 'Standard');
   glrStartGame(%cl);
}

function glrFreeClient(%cl)
{
   retroDetach(%cl);
   if (isObject(%cl.glrGroup))
      %cl.glrGroup.delete();
   %cl.glrGroup = "";
   %cl.glrSeat = "";
   %cl.glrJoined = false;
}

// Switch the package on and off with the mission, so stock DM missions are untouched.
function glrWatch()
{
   cancel($Glr::WatchThread);
   $Glr::WatchThread = schedule(250, 0, glrWatch);
   %on = ($CurrentMission $= $Glr::Mission && isObject(MissionCleanup));
   if (%on && !$Glr::PkgOn)
   {
      activatePackage(T2LaunchGalagaReal);
      $Glr::PkgOn = true;
      $Glr::LastTick = "";
      glrTick();
      echo("Galaga (real models) ruleset on");
   }
   else if (!%on && $Glr::PkgOn)
   {
      deactivatePackage(T2LaunchGalagaReal);
      $Glr::PkgOn = false;
      cancel($Glr::TickThread);
      echo("Galaga (real models) ruleset off");
   }
}

package T2LaunchGalagaRealBoot
{
   function CreateServer(%mission, %missionType)
   {
      Parent::CreateServer(%mission, %missionType);
      glrDatablocks();
   }
};

package T2LaunchGalagaReal
{
   // the shot and bomb props are not for picking up
   function ItemData::onCollision(%data, %obj, %col)
   {
      if (%obj.glrProp)
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
      glrInitPlayer(%client);
   }

   function DMGame::onClientKilled(%game, %clVictim, %clKiller, %damageType, %implement, %damageLoc)
   {
      // the stand-in Player is not part of the game: no respawn timer or orbit camera
      %clVictim.deaths++;
   }

   function DMGame::ObserverOnTrigger(%game, %data, %obj, %trigger, %state)
   {
      %client = %obj.getControllingClient();
      if (%client && %client.glrJoined)
      {
         if (%state && %trigger == 0 && %client.glrSeat !$= "")
            glrFire(%client);
         return false;
      }
      return Parent::ObserverOnTrigger(%game, %data, %obj, %trigger, %state);
   }

   // a seated player is a Player now, so its triggers arrive here: fire shoots
   function Armor::onTrigger(%data, %player, %trigger, %val)
   {
      %client = %player.getControllingClient();
      if (%client && %client.glrJoined)
      {
         if (%val && %trigger == 0 && %client.glrSeat !$= "")
            glrFire(%client);
         return;
      }
      Parent::onTrigger(%data, %player, %trigger, %val);
   }

   function DMGame::onClientLeaveGame(%game, %client)
   {
      glrFreeClient(%client);
      Parent::onClientLeaveGame(%game, %client);
   }
};

if (!$Glr::Installed)
{
   $Glr::Installed = true;
   activatePackage(T2LaunchGalagaRealBoot);
   glrTables();
   glrDatablocks();
   glrWatch();
   echo("Galaga (real models) gametype script loaded");
}
