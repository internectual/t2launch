// Pacman: server-side ruleset for the MatrixPacman mission.
// Evaluated in memory by the t2launch hook (server.gametype_script, after Retro.cs); stock clients download nothing.
// The package is active only while $CurrentMission is MatrixPacman.
//
// Rules: a 19 x 21 maze of force-field walls full of dots. Eat every dot to clear the level; four ghosts hunt you
// (red chases, pink aims ahead, cyan flanks, orange shies away when close) and switch between scatter and chase.
// The four corner pellets turn the ghosts blue for a few seconds: eat them for 200, 400, 800 and 1600. A ghost that
// is eaten returns to the house and comes out again. Both ends of the middle row are a tunnel. Dots score 10,
// pellets 50. Three lives. Every player gets their own maze (two at most) on an upright wall, seen side-on.
// Controls (stock keybinds): forward / back / strafe steer up / down / left / right; the turn is remembered until
// the maze allows it. Input is read by Retro.cs.

$Pm::Mission = "MatrixPacman";
$Pm::Cols = 19;
$Pm::Rows = 21;
$Pm::Cell = 1.8;                     // metres per maze cell
$Pm::Z0 = 300;                       // world height of the maze floor
$Pm::Spacing = 50;                   // between mazes
$Pm::Depth = 2;
$Pm::Seats = 2;
$Pm::SeatOffset[0] = 0;  $Pm::SeatOffset[1] = 1;
$Pm::PacSpeed = 6.5;                 // cells per second
$Pm::GhostSpeed = 5.8;
$Pm::ScaredSpeed = 3.6;
$Pm::Lives = 3;
$Pm::RespawnMs = 1800;
$Pm::GameOverMs = 4500;
$Pm::LevelMs = 2200;
$Pm::CamDist = 56;
$Pm::CamZShift = -1.5;
$Pm::TickMs = 32;
$Pm::FillPerTick = 8;                 // dots placed per tick at the start of a level
$Pm::ParkX = 3000;                   // inactive blocks wait far outside every client's scope
$Pm::ParkY = 3000;
$Pm::Debug = 0;
$Pm::DC[0] = 0;  $Pm::DC[1] = 1;  $Pm::DC[2] = 0;  $Pm::DC[3] = -1;     // 0 up, 1 right, 2 down, 3 left
$Pm::DR[0] = -1; $Pm::DR[1] = 0;  $Pm::DR[2] = 1;  $Pm::DR[3] = 0;

// ---------------------------------------------------------------------------------------------------------
// the maze (# wall, . dot, o pellet, - ghost door, G ghost house, P start)

function pmTables()
{
   $Pm::Row[0]  = "###################";
   $Pm::Row[1]  = "#........#........#";
   $Pm::Row[2]  = "#o##.###.#.###.##o#";
   $Pm::Row[3]  = "#.................#";
   $Pm::Row[4]  = "#.##.#.#####.#.##.#";
   $Pm::Row[5]  = "#....#...#...#....#";
   $Pm::Row[6]  = "####.### # ###.####";
   $Pm::Row[7]  = "####.#       #.####";
   $Pm::Row[8]  = "####.# ##-## #.####";
   $Pm::Row[9]  = "    .  #GGG#  .    ";
   $Pm::Row[10] = "####.# ##### #.####";
   $Pm::Row[11] = "####.#       #.####";
   $Pm::Row[12] = "####.### # ###.####";
   $Pm::Row[13] = "#........#........#";
   $Pm::Row[14] = "#.##.###.#.###.##.#";
   $Pm::Row[15] = "#o.#.....P.....#.o#";
   $Pm::Row[16] = "##.#.#.#####.#.#.##";
   $Pm::Row[17] = "#....#...#...#....#";
   $Pm::Row[18] = "#.######.#.######.#";
   $Pm::Row[19] = "#.................#";
   $Pm::Row[20] = "###################";
   $Pm::Dots = 0;
   for (%r = 0; %r < 21; %r++)
      for (%c = 0; %c < 19; %c++)
      {
         %ch = getSubStr($Pm::Row[%r], %c, 1);
         if (%ch $= "." || %ch $= "o")
            $Pm::Dots++;
      }
}

function pmChar(%c, %r)
{
   return getSubStr($Pm::Row[%r], %c, 1);
}

// can something step into (%c, %r)? ghosts may use the door; the middle row wraps around the sides
function pmOpen(%c, %r, %door)
{
   if (%r < 0 || %r >= $Pm::Rows)
      return false;
   if (%c < 0 || %c >= $Pm::Cols)
      return %r == 9;
   %ch = pmChar(%c, %r);
   if (%ch $= "#")
      return false;
   if (%ch $= "-")
      return %door;
   return true;
}

function pmWrap(%c)
{
   if (%c < 0)
      return $Pm::Cols - 1;
   if (%c >= $Pm::Cols)
      return 0;
   return %c;
}

// ---------------------------------------------------------------------------------------------------------
// datablocks: deleted by DestroyServer, so defined at install and again after every CreateServer

function pmDatablocks()
{
   if (isObject(PmPac))
      return;
   %defs = "PmPac|1.0 0.9 0.1;PmBlinky|1.0 0.15 0.15;PmPinky|1.0 0.55 0.8;PmInky|0.2 0.95 1.0;PmClyde|1.0 0.6 0.1;"
         @ "PmScared|0.2 0.3 1.0;PmDot|1.0 0.9 0.75;PmPellet|1.0 0.95 0.85;PmWall|0.25 0.35 1.0";
   for (%i = 0; %i < getFieldCount(strreplace(%defs, ";", "\t")); %i++)
   {
      %def = getField(strreplace(%defs, ";", "\t"), %i);
      %name = getSubStr(%def, 0, strstr(%def, "|"));
      %color = getSubStr(%def, strstr(%def, "|") + 1, 64);
      %alpha = 0.9;
      if (%name $= "PmWall")
         %alpha = 0.55;
      eval("datablock ForceFieldBareData(" @ %name @ ") : defaultForceFieldBare { color = \"" @ %color
         @ "\"; baseTranslucency = " @ %alpha @ "; needsNoPower = true; };");
   }
}

function pmField(%db, %x, %z, %sx, %sz)
{
   return new ForceFieldBare() {
      dataBlock = %db;
      position = %x SPC (-$Pm::Depth / 2) SPC %z;
      rotation = "1 0 0 0";
      scale = %sx SPC $Pm::Depth SPC %sz;
   };
}

function pmPurgeZones()
{
   %z = nameToID("MissionCleanup/PZones");
   if (%z > 0)
      %z.delete();
}

// ---------------------------------------------------------------------------------------------------------
// geometry: (u, v) = across from the centre line, and up from the floor

function pmBoardX(%seat)
{
   return $Pm::SeatOffset[%seat] * $Pm::Spacing;
}

function pmAnchor(%seat)
{
   return pmBoardX(%seat) SPC (-$Pm::CamDist) SPC ($Pm::Z0 + $Pm::Rows * $Pm::Cell / 2 + $Pm::CamZShift) SPC "1 0 0 0";
}

function pmCellU(%c)
{
   return (%c - ($Pm::Cols - 1) / 2) * $Pm::Cell;
}

function pmCellV(%r)
{
   return ($Pm::Rows - 1 - %r) * $Pm::Cell + $Pm::Cell / 2;
}

// a block's position is its lower-left corner; (%u, %v) is its centre
function pmPlace(%obj, %seat, %u, %v, %w, %h)
{
   %obj.setTransform((pmBoardX(%seat) + %u - %w / 2) SPC (-$Pm::Depth / 2) SPC ($Pm::Z0 + %v - %h / 2));
}

function pmPark(%obj)
{
   %obj.setTransform($Pm::ParkX SPC $Pm::ParkY SPC 300);
}

function pmFreeSeat()
{
   for (%s = 0; %s < $Pm::Seats; %s++)
   {
      %used = false;
      for (%i = 0; %i < ClientGroup.getCount(); %i++)
         if (ClientGroup.getObject(%i).pmSeat $= %s)
            %used = true;
      if (!%used)
         return %s;
   }
   return "";
}

function pmGhostDb(%k)
{
   return getWord("PmBlinky PmPinky PmInky PmClyde", %k);
}

// ---------------------------------------------------------------------------------------------------------
// one maze

function pmMakeBoard(%cl)
{
   %seat = %cl.pmSeat;
   %g = new SimGroup();
   MissionCleanup.add(%g);
   %cl.pmGroup = %g;
   %cs = $Pm::Cell;
   // walls: runs of # on a row become one block
   for (%r = 0; %r < $Pm::Rows; %r++)
   {
      %c = 0;
      while (%c < $Pm::Cols)
      {
         if (pmChar(%c, %r) !$= "#")
         {
            %c++;
            continue;
         }
         %start = %c;
         while (%c < $Pm::Cols && pmChar(%c, %r) $= "#")
            %c++;
         %len = %c - %start;
         %u = (pmCellU(%start) + pmCellU(%c - 1)) / 2;
         %o = pmField(PmWall, 0, 0, %len * %cs - 0.25, %cs - 0.25);
         %g.add(%o);
         pmPlace(%o, %seat, %u, pmCellV(%r), %len * %cs - 0.25, %cs - 0.25);
      }
   }
   // dots and pellets
   for (%r = 0; %r < $Pm::Rows; %r++)
      for (%c = 0; %c < $Pm::Cols; %c++)
      {
         %ch = pmChar(%c, %r);
         %idx = %r * $Pm::Cols + %c;
         $Pm::DotObj[%seat, %idx] = "";
         if (%ch $= ".")
            %o = pmField(PmDot, 0, 0, 0.45, 0.45);
         else if (%ch $= "o")
            %o = pmField(PmPellet, 0, 0, 1.0, 1.0);
         else
            continue;
         %g.add(%o);
         pmPark(%o);
         $Pm::DotObj[%seat, %idx] = %o;
         $Pm::DotOn[%seat, %idx] = false;
      }
   %cl.pmPac = pmField(PmPac, 0, 0, %cs - 0.2, %cs - 0.2);
   %g.add(%cl.pmPac);
   pmPark(%cl.pmPac);
   for (%k = 0; %k < 4; %k++)
   {
      %o = pmField(pmGhostDb(%k), 0, 0, %cs - 0.2, %cs - 0.2);
      %g.add(%o);
      pmPark(%o);
      $Pm::GObj[%seat, %k] = %o;
      %s = pmField(PmScared, 0, 0, %cs - 0.2, %cs - 0.2);
      %g.add(%s);
      pmPark(%s);
      $Pm::GScared[%seat, %k] = %s;
      $Pm::GShown[%seat, %k] = "";
   }
   pmPurgeZones();
}

// ---------------------------------------------------------------------------------------------------------
// the game

function pmStartGame(%cl)
{
   %cl.pmScore = 0;
   %cl.pmLives = $Pm::Lives;
   %cl.pmLevel = 1;
   pmNewLevel(%cl);
}

function pmNewLevel(%cl)
{
   %seat = %cl.pmSeat;
   %cl.pmDotsLeft = 0;
   for (%r = 0; %r < $Pm::Rows; %r++)
      for (%c = 0; %c < $Pm::Cols; %c++)
      {
         %idx = %r * $Pm::Cols + %c;
         %o = $Pm::DotObj[%seat, %idx];
         if (%o $= "")
            continue;
         $Pm::DotOn[%seat, %idx] = true;
         %cl.pmDotsLeft++;
      }
   // the dots appear a few at a time (see pmFillDots), which spreads the burst of new objects sent to the client
   %cl.pmFillIdx = 0;
   pmResetActors(%cl);
   %cl.pmState = "play";
   %cl.pmModeAt = getSimTime() + 7000;
   %cl.pmChase = false;
   %cl.pmPhase = 0;
   pmHud(%cl);
}

// every actor back on its start cell
function pmResetActors(%cl)
{
   %seat = %cl.pmSeat;
   pmSetActor(%seat, 0, 9, 15, 3);
   %cl.pmWant = -1;
   %cl.pmFrUntil = 0;
   %cl.pmCombo = 0;
   // Blinky waits outside the house, the others inside
   pmSetActor(%seat, 1, 9, 7, 3);
   pmSetActor(%seat, 2, 9, 9, 0);
   pmSetActor(%seat, 3, 8, 9, 0);
   pmSetActor(%seat, 4, 10, 9, 0);
   %lvl = %cl.pmLevel;
   %now = getSimTime();
   for (%k = 0; %k < 4; %k++)
   {
      $Pm::Fr[%seat, %k] = false;
      $Pm::GShown[%seat, %k] = "";
   }
   $Pm::GMode[%seat, 0] = 2;
   $Pm::GMode[%seat, 1] = 0;
   $Pm::GMode[%seat, 2] = 0;
   $Pm::GMode[%seat, 3] = 0;
   $Pm::GRelease[%seat, 0] = 0;
   $Pm::GRelease[%seat, 1] = %now + 1500;
   $Pm::GRelease[%seat, 2] = %now + 4500 - %lvl * 300;
   $Pm::GRelease[%seat, 3] = %now + 8500 - %lvl * 600;
   pmDraw(%cl);
}

function pmSetActor(%seat, %e, %c, %r, %d)
{
   $Pm::C[%seat, %e] = %c;
   $Pm::R[%seat, %e] = %r;
   $Pm::NC[%seat, %e] = %c;
   $Pm::NR[%seat, %e] = %r;
   $Pm::P[%seat, %e] = 0;
   $Pm::D[%seat, %e] = %d;
   $Pm::Moving[%seat, %e] = false;
}

function pmHud(%cl)
{
   if (%cl.pmState $= "over")
   {
      bottomPrint(%cl, "GAME OVER    SCORE " @ %cl.pmScore, 0, 1);
      return;
   }
   bottomPrint(%cl, "SCORE " @ %cl.pmScore @ "    LIVES " @ %cl.pmLives @ "    LEVEL " @ %cl.pmLevel @ "     forward / back / strafe = steer", 0, 1);
}

// actor position in cells (fractional), as "col row"
function pmActorPos(%seat, %e)
{
   %c = $Pm::C[%seat, %e];
   %r = $Pm::R[%seat, %e];
   %nc = $Pm::NC[%seat, %e];
   %nr = $Pm::NR[%seat, %e];
   %p = $Pm::P[%seat, %e];
   if (mAbs(%nc - %c) > 1)
      return %c SPC %r;                         // wrapping through the tunnel
   return (%c + (%nc - %c) * %p) SPC (%r + (%nr - %r) * %p);
}

function pmPlaceCell(%obj, %seat, %pc, %pr)
{
   pmPlace(%obj, %seat, (%pc - ($Pm::Cols - 1) / 2) * $Pm::Cell, ($Pm::Rows - 1 - %pr) * $Pm::Cell + $Pm::Cell / 2,
      $Pm::Cell - 0.2, $Pm::Cell - 0.2);
}

function pmDraw(%cl)
{
   %seat = %cl.pmSeat;
   %p = pmActorPos(%seat, 0);
   if (%cl.pmState $= "dead" || %cl.pmState $= "over")
      pmPark(%cl.pmPac);
   else
      pmPlaceCell(%cl.pmPac, %seat, getWord(%p, 0), getWord(%p, 1));
   for (%k = 0; %k < 4; %k++)
   {
      %mode = $Pm::GMode[%seat, %k];
      %shown = $Pm::Fr[%seat, %k] ? "s" : "n";
      if (%mode == 3 || %cl.pmState $= "over")
      {
         pmPark($Pm::GObj[%seat, %k]);
         pmPark($Pm::GScared[%seat, %k]);
         $Pm::GShown[%seat, %k] = "";
         continue;
      }
      if ($Pm::GShown[%seat, %k] !$= %shown)
      {
         pmPark($Pm::GObj[%seat, %k]);
         pmPark($Pm::GScared[%seat, %k]);
         $Pm::GShown[%seat, %k] = %shown;
      }
      %p = pmActorPos(%seat, %k + 1);
      if (%shown $= "s")
         pmPlaceCell($Pm::GScared[%seat, %k], %seat, getWord(%p, 0), getWord(%p, 1));
      else
         pmPlaceCell($Pm::GObj[%seat, %k], %seat, getWord(%p, 0), getWord(%p, 1));
   }
}

// ---------------------------------------------------------------------------------------------------------
// movement: each actor walks cell to cell; at every cell it picks its next direction

function pmReverse(%seat, %e)
{
   %d = $Pm::D[%seat, %e];
   if (%d < 0 || !$Pm::Moving[%seat, %e])
      return;
   %c = $Pm::C[%seat, %e];
   %r = $Pm::R[%seat, %e];
   $Pm::C[%seat, %e] = $Pm::NC[%seat, %e];
   $Pm::R[%seat, %e] = $Pm::NR[%seat, %e];
   $Pm::NC[%seat, %e] = %c;
   $Pm::NR[%seat, %e] = %r;
   $Pm::P[%seat, %e] = 1 - $Pm::P[%seat, %e];
   $Pm::D[%seat, %e] = (%d + 2) % 4;
}

// the direction the actor should take from the cell it stands on, or -1 to stand still
function pmDecide(%cl, %e)
{
   %seat = %cl.pmSeat;
   %c = $Pm::C[%seat, %e];
   %r = $Pm::R[%seat, %e];
   %d = $Pm::D[%seat, %e];
   if (%e == 0)
   {
      %w = %cl.pmWant;
      if (%w >= 0 && pmOpen(%c + $Pm::DC[%w], %r + $Pm::DR[%w], false))
      {
         %cl.pmWant = -1;
         return %w;
      }
      if (%d >= 0 && pmOpen(%c + $Pm::DC[%d], %r + $Pm::DR[%d], false))
         return %d;
      return -1;
   }
   %k = %e - 1;
   %mode = $Pm::GMode[%seat, %k];
   if (%mode == 0)
      return -1;
   %door = (%mode == 1);
   %back = (%d + 2) % 4;
   // candidate directions in the classic tie-break order: up, left, down, right
   %best = -1;
   %bestDist = 999999;
   %n = 0;
   %target = pmTarget(%cl, %k);
   %tc = getWord(%target, 0);
   %tr = getWord(%target, 1);
   for (%i = 0; %i < 4; %i++)
   {
      %nd = getWord("0 3 2 1", %i);
      if (%nd == %back && %d >= 0)
         continue;
      %nc = %c + $Pm::DC[%nd];
      %nr = %r + $Pm::DR[%nd];
      if (!pmOpen(%nc, %nr, %door))
         continue;
      %n++;
      if ($Pm::Fr[%seat, %k] && %mode == 2)
      {
         // frightened: wander
         %score = getRandom();
      }
      else
         %score = (%nc - %tc) * (%nc - %tc) + (%nr - %tr) * (%nr - %tr);
      if (%score < %bestDist)
      {
         %bestDist = %score;
         %best = %nd;
      }
   }
   if (%best < 0 && %d >= 0 && pmOpen(%c + $Pm::DC[%back], %r + $Pm::DR[%back], %door))
      return %back;                                // a dead end: turn around
   return %best;
}

// where ghost %k is heading
function pmTarget(%cl, %k)
{
   %seat = %cl.pmSeat;
   %mode = $Pm::GMode[%seat, %k];
   if (%mode == 1)
      return "9 7";
   if (!%cl.pmChase)
   {
      if (%k == 0) return "17 0";
      if (%k == 1) return "1 0";
      if (%k == 2) return "18 20";
      return "0 20";
   }
   %pc = $Pm::C[%seat, 0];
   %pr = $Pm::R[%seat, 0];
   %pd = $Pm::D[%seat, 0];
   if (%pd < 0)
      %pd = 3;
   if (%k == 0)
      return %pc SPC %pr;
   if (%k == 1)
      return (%pc + $Pm::DC[%pd] * 4) SPC (%pr + $Pm::DR[%pd] * 4);
   if (%k == 2)
   {
      %ac = %pc + $Pm::DC[%pd] * 2;
      %ar = %pr + $Pm::DR[%pd] * 2;
      return (2 * %ac - $Pm::C[%seat, 1]) SPC (2 * %ar - $Pm::R[%seat, 1]);
   }
   %dc = $Pm::C[%seat, 4] - %pc;
   %dr = $Pm::R[%seat, 4] - %pr;
   if (%dc * %dc + %dr * %dr > 64)
      return %pc SPC %pr;
   return "0 20";
}

// advance actor %e by %dist cells
function pmMove(%cl, %e, %dist)
{
   %seat = %cl.pmSeat;
   %left = %dist;
   %guard = 0;
   while (%left > 0 && %guard < 8)
   {
      %guard++;
      if (!$Pm::Moving[%seat, %e])
      {
         %d = pmDecide(%cl, %e);
         if (%d < 0)
            return;
         $Pm::D[%seat, %e] = %d;
         $Pm::NC[%seat, %e] = pmWrap($Pm::C[%seat, %e] + $Pm::DC[%d]);
         $Pm::NR[%seat, %e] = $Pm::R[%seat, %e] + $Pm::DR[%d];
         $Pm::P[%seat, %e] = 0;
         $Pm::Moving[%seat, %e] = true;
      }
      %room = 1 - $Pm::P[%seat, %e];
      if (%left < %room)
      {
         $Pm::P[%seat, %e] += %left;
         return;
      }
      %left -= %room;
      $Pm::C[%seat, %e] = $Pm::NC[%seat, %e];
      $Pm::R[%seat, %e] = $Pm::NR[%seat, %e];
      $Pm::P[%seat, %e] = 0;
      $Pm::Moving[%seat, %e] = false;
      if (%e == 0)
         pmEat(%cl);
      else
         pmArrive(%cl, %e - 1);
   }
}

// a ghost has reached a cell centre
function pmArrive(%cl, %k)
{
   %seat = %cl.pmSeat;
   if ($Pm::GMode[%seat, %k] == 1 && $Pm::R[%seat, %k + 1] <= 7 && $Pm::C[%seat, %k + 1] == 9)
      $Pm::GMode[%seat, %k] = 2;
}

function pmEat(%cl)
{
   %seat = %cl.pmSeat;
   %idx = $Pm::R[%seat, 0] * $Pm::Cols + $Pm::C[%seat, 0];
   if (!$Pm::DotOn[%seat, %idx])
      return;
   $Pm::DotOn[%seat, %idx] = false;
   pmPark($Pm::DotObj[%seat, %idx]);
   %cl.pmDotsLeft--;
   if (pmChar($Pm::C[%seat, 0], $Pm::R[%seat, 0]) $= "o")
   {
      %cl.pmScore += 50;
      %cl.pmCombo = 0;
      %cl.pmFrUntil = getSimTime() + (6500 - %cl.pmLevel * 500 > 2000 ? 6500 - %cl.pmLevel * 500 : 2000);
      for (%k = 0; %k < 4; %k++)
         if ($Pm::GMode[%seat, %k] == 2 && !$Pm::Fr[%seat, %k])
         {
            $Pm::Fr[%seat, %k] = true;
            pmReverse(%seat, %k + 1);
         }
   }
   else
      %cl.pmScore += 10;
   pmHud(%cl);
}

function pmPlayerHit(%cl)
{
   %cl.pmLives--;
   if (%cl.pmLives <= 0)
   {
      pmGameOver(%cl);
      return;
   }
   %cl.pmState = "dead";
   %cl.pmRespawnAt = getSimTime() + $Pm::RespawnMs;
   pmHud(%cl);
}

function pmGameOver(%cl)
{
   if (%cl.pmState $= "over")
      return;
   %cl.pmState = "over";
   %cl.pmOverAt = getSimTime() + $Pm::GameOverMs;
   if (%cl.pmScore > %cl.pmHigh)
      %cl.pmHigh = %cl.pmScore;
   messageAll('MsgPacman', '\c1%1 is done (%2 points).', %cl.name, %cl.pmScore);
   pmHud(%cl);
}

// place the next few dots of a new level
function pmFillDots(%cl)
{
   %seat = %cl.pmSeat;
   %total = $Pm::Rows * $Pm::Cols;
   %n = 0;
   while (%cl.pmFillIdx < %total && %n < $Pm::FillPerTick)
   {
      %idx = %cl.pmFillIdx;
      %cl.pmFillIdx++;
      %o = $Pm::DotObj[%seat, %idx];
      if (%o $= "" || !$Pm::DotOn[%seat, %idx])
         continue;
      %c = %idx % $Pm::Cols;
      %r = mFloor(%idx / $Pm::Cols);
      %sz = 0.45;
      if (pmChar(%c, %r) $= "o")
         %sz = 1.0;
      pmPlace(%o, %seat, pmCellU(%c), pmCellV(%r), %sz, %sz);
      %n++;
   }
}

function pmPhysics(%cl, %dt, %now)
{
   %seat = %cl.pmSeat;
   if (%cl.pmFillIdx < $Pm::Rows * $Pm::Cols)
      pmFillDots(%cl);
   if (%cl.pmState $= "over")
   {
      pmDraw(%cl);
      if (%now >= %cl.pmOverAt)
         pmStartGame(%cl);
      return;
   }
   if (%cl.pmState $= "level")
   {
      if (%now >= %cl.pmLevelAt)
      {
         %cl.pmLevel++;
         pmNewLevel(%cl);
      }
      return;
   }
   if (%cl.pmState $= "dead")
   {
      pmDraw(%cl);
      if (%now >= %cl.pmRespawnAt)
      {
         %cl.pmState = "play";
         pmResetActors(%cl);
         pmHud(%cl);
      }
      return;
   }
   // the steering keys: a newly held direction wins over the one we are already going in
   %m = %cl.pmPadMask;
   %cur = $Pm::D[%seat, 0];
   %want = -1;
   for (%dir = 0; %dir < 4; %dir++)
      if ((%m & (1 << %dir)) && %dir != %cur)
         %want = %dir;
   if (%want < 0 && %cur >= 0 && (%m & (1 << %cur)))
      %want = %cur;
   if (%want >= 0)
      %cl.pmWant = %want;
   // an about-turn takes effect at once
   if (%cl.pmWant >= 0 && %cur >= 0 && %cl.pmWant == (%cur + 2) % 4)
   {
      pmReverse(%seat, 0);
      %cl.pmWant = -1;
   }
   // scatter / chase schedule
   if (%now >= %cl.pmModeAt)
   {
      %cl.pmPhase++;
      %cl.pmChase = !%cl.pmChase;
      if (%cl.pmPhase >= 6)
         %cl.pmModeAt = %now + 99999999;
      else
         %cl.pmModeAt = %now + (%cl.pmChase ? 20000 : 6000);
      for (%k = 0; %k < 4; %k++)
         if ($Pm::GMode[%seat, %k] == 2 && !$Pm::Fr[%seat, %k])
            pmReverse(%seat, %k + 1);
   }
   // frightened time runs out
   if (%cl.pmFrUntil > 0 && %now >= %cl.pmFrUntil)
   {
      %cl.pmFrUntil = 0;
      for (%k = 0; %k < 4; %k++)
         $Pm::Fr[%seat, %k] = false;
   }
   // actors
   %scale = 1 + (%cl.pmLevel - 1) * 0.05;
   if (%scale > 1.3)
      %scale = 1.3;
   pmMove(%cl, 0, $Pm::PacSpeed * %dt);
   for (%k = 0; %k < 4; %k++)
   {
      %mode = $Pm::GMode[%seat, %k];
      if (%mode == 3)
      {
         if (%now >= $Pm::GRelease[%seat, %k])
         {
            pmSetActor(%seat, %k + 1, 9, 9, 0);
            $Pm::GMode[%seat, %k] = 1;
            $Pm::Fr[%seat, %k] = false;
         }
         continue;
      }
      if (%mode == 0)
      {
         if (%now >= $Pm::GRelease[%seat, %k])
            $Pm::GMode[%seat, %k] = 1;
         continue;
      }
      %speed = $Pm::GhostSpeed * %scale;
      if ($Pm::Fr[%seat, %k])
         %speed = $Pm::ScaredSpeed;
      pmMove(%cl, %k + 1, %speed * %dt);
   }
   // contact between Pac-Man and a ghost
   %pp = pmActorPos(%seat, 0);
   for (%k = 0; %k < 4; %k++)
   {
      if ($Pm::GMode[%seat, %k] == 0 || $Pm::GMode[%seat, %k] == 3)
         continue;
      %gp = pmActorPos(%seat, %k + 1);
      %dx = getWord(%pp, 0) - getWord(%gp, 0);
      %dy = getWord(%pp, 1) - getWord(%gp, 1);
      if (%dx * %dx + %dy * %dy > 0.45)
         continue;
      if ($Pm::Fr[%seat, %k])
      {
         %pts = 200;
         for (%j = 0; %j < %cl.pmCombo; %j++)
            %pts = %pts * 2;
         %cl.pmCombo++;
         %cl.pmScore += %pts;
         $Pm::GMode[%seat, %k] = 3;
         $Pm::Fr[%seat, %k] = false;
         $Pm::GRelease[%seat, %k] = %now + 2500;
         pmHud(%cl);
      }
      else
      {
         pmPlayerHit(%cl);
         return;
      }
   }
   if (%cl.pmDotsLeft <= 0)
   {
      %cl.pmState = "level";
      %cl.pmLevelAt = %now + $Pm::LevelMs;
      bottomPrint(%cl, "LEVEL " @ %cl.pmLevel @ " CLEARED    SCORE " @ %cl.pmScore, 0, 1);
      return;
   }
   pmDraw(%cl);
}

// ---------------------------------------------------------------------------------------------------------
// input: the player's keys, read by Retro.cs from its (tiny) velocity; bit 0 up, 1 right, 2 down, 3 left

function pmReadPad(%cl)
{
   %m = retroPad(%cl);
   %out = 0;
   if (%m & 1) %out = %out | 1;      // forward: up
   if (%m & 8) %out = %out | 2;      // strafe right
   if (%m & 2) %out = %out | 4;      // back: down
   if (%m & 4) %out = %out | 8;      // strafe left
   return %out;
}

function pmTick()
{
   cancel($Pm::TickThread);
   $Pm::TickThread = schedule($Pm::TickMs, 0, pmTick);
   if (!isObject(MissionCleanup))
      return;
   $Pm::TickN++;
   %now = getSimTime();
   %dt = $Pm::LastTick $= "" ? 0.032 : (%now - $Pm::LastTick) / 1000;
   $Pm::LastTick = %now;
   if (%dt > 0.1) %dt = 0.1;
   for (%i = 0; %i < ClientGroup.getCount(); %i++)
   {
      %cl = ClientGroup.getObject(%i);
      if (!%cl.pmJoined || !isObject(%cl.camera))
         continue;
      if (%cl.pmSeat $= "")
      {
         // spectators have the stock free-fly camera; the stand-in Player is parked out of the way
         if (isObject(%cl.player))
         {
            %cl.player.setVelocity("0 0 0");
            if ($Pm::TickN % 30 == 0)
               %cl.player.setTransform("0 -150 300 1 0 0 0");
         }
         if (%cl.getControlObject() != %cl.camera)
            %cl.setControlObject(%cl.camera);
         continue;
      }
      %cl.pmPadMask = pmReadPad(%cl);
      pmPhysics(%cl, %dt, %now);
   }
}

function pmInitPlayer(%cl)
{
   %cl.pmJoined = true;
   if (%cl.pmSeat $= "")
   {
      %seat = pmFreeSeat();
      if (%seat !$= "")
      {
         %cl.pmSeat = %seat;
         pmMakeBoard(%cl);
      }
   }
   if (%cl.pmSeat $= "")
   {
      // a spectator: the free-fly camera, parked player
      %cam = %cl.camera;
      %cam.setFlyMode();
      %cam.setTransform(pmAnchor(0));
      %cl.setControlObject(%cam);
      commandToClient(%cl, 'setHudMode', 'Observer');
      %cl.schedule(100, "setControlObject", %cam);
      if (isObject(%cl.player))
      {
         %cl.player.setTransform("0 -150 300 1 0 0 0");
         %cl.player.setVelocity("0 0 0");
      }
      bottomPrint(%cl, "Watching: both mazes are taken.", 8, 1);
      return;
   }
   %a = pmAnchor(%cl.pmSeat);
   retroAttach(%cl, getWord(%a, 0), getWord(%a, 1), getWord(%a, 2));
   commandToClient(%cl, 'setHudMode', 'Standard');
   pmStartGame(%cl);
}

function pmFreeClient(%cl)
{
   retroDetach(%cl);
   if (isObject(%cl.pmGroup))
      %cl.pmGroup.delete();
   %cl.pmGroup = "";
   %cl.pmSeat = "";
   %cl.pmJoined = false;
}

// Switch the package on and off with the mission, so stock DM missions are untouched.
function pmWatch()
{
   cancel($Pm::WatchThread);
   $Pm::WatchThread = schedule(250, 0, pmWatch);
   %on = ($CurrentMission $= $Pm::Mission && isObject(MissionCleanup));
   if (%on && !$Pm::PkgOn)
   {
      activatePackage(T2LaunchPacman);
      $Pm::PkgOn = true;
      $Pm::LastTick = "";
      pmTick();
      echo("Pacman ruleset on");
   }
   else if (!%on && $Pm::PkgOn)
   {
      deactivatePackage(T2LaunchPacman);
      $Pm::PkgOn = false;
      cancel($Pm::TickThread);
      echo("Pacman ruleset off");
   }
}

package T2LaunchPacmanBoot
{
   function CreateServer(%mission, %missionType)
   {
      Parent::CreateServer(%mission, %missionType);
      pmDatablocks();
   }
};

package T2LaunchPacman
{
   function DMGame::equip(%game, %player)
   {
      // nobody carries anything
   }

   function DMGame::createPlayer(%game, %client, %spawnLoc, %respawn)
   {
      Parent::createPlayer(%game, %client, %spawnLoc, %respawn);
      pmInitPlayer(%client);
   }

   function DMGame::onClientKilled(%game, %clVictim, %clKiller, %damageType, %implement, %damageLoc)
   {
      // the stand-in Player is not part of the game: no respawn timer or orbit camera
      %clVictim.deaths++;
   }

   function DMGame::ObserverOnTrigger(%game, %data, %obj, %trigger, %state)
   {
      %client = %obj.getControllingClient();
      if (%client && %client.pmJoined)
         return false;
      return Parent::ObserverOnTrigger(%game, %data, %obj, %trigger, %state);
   }

   // a seated player is a Player now: swallow its triggers (nothing is carried)
   function Armor::onTrigger(%data, %player, %trigger, %val)
   {
      %client = %player.getControllingClient();
      if (%client && %client.pmJoined)
         return;
      Parent::onTrigger(%data, %player, %trigger, %val);
   }

   function DMGame::onClientLeaveGame(%game, %client)
   {
      pmFreeClient(%client);
      Parent::onClientLeaveGame(%game, %client);
   }
};

if (!$Pm::Installed)
{
   $Pm::Installed = true;
   pmTables();
   activatePackage(T2LaunchPacmanBoot);
   pmDatablocks();
   pmWatch();
   echo("Pacman gametype script loaded");
}
