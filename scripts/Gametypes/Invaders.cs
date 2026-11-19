// Invaders: server-side ruleset for the MatrixInvaders mission.
// Evaluated in memory by the t2launch hook (server.gametype_script); stock clients download nothing.
// The package is active only while $CurrentMission is MatrixInvaders.
//
// Arcade rules: a formation of 5 rows x 11 aliens marches sideways and drops a row at each edge, faster as fewer
// are left; the bottom alien of a random column drops a bomb every second or so (three at most); four shields
// erode under shots and bombs; a mystery ship crosses the top now and then; one cannon shot at a time; three
// lives; the top row scores 30, the middle two 20 and the bottom two 10; the game ends when you are out of lives
// or the aliens land. A cleared wave comes back lower and faster, with fresh shields.
// Every player gets their own board in a row on an upright wall, seen side-on (like Breakout.cs).
// Controls (stock keybinds): strafe moves the cannon, fire shoots. The client's control object is its Camera;
// its movement each tick is read as the d-pad.

$Inv::Mission = "MatrixInvaders";
$Inv::HalfW = 18;                    // court half-width in metres
$Inv::Height = 36;
$Inv::Z0 = 300;                      // world height of the court floor
$Inv::Spacing = 44;                  // between boards
$Inv::Depth = 2;
$Inv::Seats = 4;
$Inv::SeatOffset[0] = 0;  $Inv::SeatOffset[1] = 1;  $Inv::SeatOffset[2] = -1; $Inv::SeatOffset[3] = 2;
$Inv::PadV = 2;                      // cannon centre height
$Inv::PadW = 3;
$Inv::PadH = 1.2;
$Inv::PadSpeed = 20;
$Inv::Cols = 11;
$Inv::Rows = 5;
$Inv::AlienDU = 2.6;                 // spacing of the formation
$Inv::AlienDV = 2.2;
$Inv::AlienW = 2;
$Inv::AlienH = 1.4;
$Inv::FormationV = 21;               // height of the bottom row at the start of a wave
$Inv::StepU = 0.7;                   // sideways move per step
$Inv::DropV = 1.2;
$Inv::LandV = 4.2;                   // the aliens have landed when their bottom reaches this height
$Inv::BulletSpeed = 30;
$Inv::BombSpeed = 11;
$Inv::MaxBombs = 3;
$Inv::UfoSpeed = 9;
$Inv::UfoV = 33;
$Inv::Lives = 3;
$Inv::RespawnMs = 1500;
$Inv::GameOverMs = 4500;
$Inv::ShieldU[0] = -12; $Inv::ShieldU[1] = -4; $Inv::ShieldU[2] = 4; $Inv::ShieldU[3] = 12;
$Inv::ShieldV = 7;                   // bottom of the shields
$Inv::ShieldCols = 7;
$Inv::ShieldRows = 4;
$Inv::Cell = 0.8;
$Inv::CamDist = 46;
$Inv::CamZShift = -1.5;
$Inv::KeyGraceMs = 110;
$Inv::RecentreMs = 250;
$Inv::IgnoreMs = 100;
$Inv::TickMs = 32;
$Inv::ParkX = 3000;                  // inactive blocks wait far outside every client's scope
$Inv::ParkY = 3000;
$Inv::Debug = 0;

// ---------------------------------------------------------------------------------------------------------
// datablocks: deleted by DestroyServer, so defined at install and again after every CreateServer

function invDatablocks()
{
   if (isObject(InvCannon))
      return;
   %defs = "InvAlien0|0.2 0.95 0.3;InvAlien1|0.2 0.85 1.0;InvAlien2|0.95 0.3 0.95;InvShield|0.3 0.95 0.35;"
         @ "InvCannon|0.9 0.95 1.0;InvBullet|1.0 1.0 1.0;InvBomb|1.0 0.85 0.2;InvUfo|1.0 0.2 0.2;InvWall|0.8 0.8 0.85";
   for (%i = 0; %i < getFieldCount(strreplace(%defs, ";", "\t")); %i++)
   {
      %def = getField(strreplace(%defs, ";", "\t"), %i);
      %name = getSubStr(%def, 0, strstr(%def, "|"));
      %color = getSubStr(%def, strstr(%def, "|") + 1, 64);
      %alpha = 0.9;
      if (%name $= "InvWall")
         %alpha = 0.3;
      eval("datablock ForceFieldBareData(" @ %name @ ") : defaultForceFieldBare { color = \"" @ %color
         @ "\"; baseTranslucency = " @ %alpha @ "; needsNoPower = true; };");
   }
}

function invField(%db, %x, %z, %sx, %sz)
{
   return new ForceFieldBare() {
      dataBlock = %db;
      position = %x SPC (-$Inv::Depth / 2) SPC %z;
      rotation = "1 0 0 0";
      scale = %sx SPC $Inv::Depth SPC %sz;
   };
}

function invPurgeZones()
{
   %z = nameToID("MissionCleanup/PZones");
   if (%z > 0)
      %z.delete();
}

// ---------------------------------------------------------------------------------------------------------
// geometry: court coordinates are (u, v) = across from the centre line, and up from the floor

function invBoardX(%seat)
{
   return $Inv::SeatOffset[%seat] * $Inv::Spacing;
}

function invAnchor(%seat)
{
   return invBoardX(%seat) SPC (-$Inv::CamDist) SPC ($Inv::Z0 + $Inv::Height / 2 + $Inv::CamZShift) SPC "1 0 0 0";
}

// a block's position is its lower-left corner; (%u, %v) is its centre
function invPlace(%obj, %seat, %u, %v, %w, %h)
{
   %obj.setTransform((invBoardX(%seat) + %u - %w / 2) SPC (-$Inv::Depth / 2) SPC ($Inv::Z0 + %v - %h / 2));
}

function invPark(%obj)
{
   %obj.setTransform($Inv::ParkX SPC $Inv::ParkY SPC 300);
}

function invAlienU(%cl, %col)
{
   return %cl.invFU + (%col - ($Inv::Cols - 1) / 2) * $Inv::AlienDU;
}

function invAlienV(%cl, %row)
{
   return %cl.invFV + %row * $Inv::AlienDV;
}

function invRowPoints(%row)
{
   if (%row >= 4)
      return 30;
   if (%row >= 2)
      return 20;
   return 10;
}

function invFreeSeat()
{
   for (%s = 0; %s < $Inv::Seats; %s++)
   {
      %used = false;
      for (%i = 0; %i < ClientGroup.getCount(); %i++)
         if (ClientGroup.getObject(%i).invSeat $= %s)
            %used = true;
      if (!%used)
         return %s;
   }
   return "";
}

// ---------------------------------------------------------------------------------------------------------
// one board

function invMakeBoard(%cl)
{
   %seat = %cl.invSeat;
   %g = new SimGroup();
   MissionCleanup.add(%g);
   %cl.invGroup = %g;
   %h = $Inv::Height;
   %w = $Inv::HalfW;
   %g.add(invField(InvWall, invBoardX(%seat) - %w - 0.6, $Inv::Z0, 0.6, %h + 0.6));
   %g.add(invField(InvWall, invBoardX(%seat) + %w, $Inv::Z0, 0.6, %h + 0.6));
   %g.add(invField(InvWall, invBoardX(%seat) - %w - 0.6, $Inv::Z0 + %h, %w * 2 + 1.2, 0.6));
   %g.add(invField(InvWall, invBoardX(%seat) - %w - 0.6, $Inv::Z0 - 0.6, %w * 2 + 1.2, 0.6));
   %cl.invPad = invField(InvCannon, 0, 0, $Inv::PadW, $Inv::PadH);
   %cl.invBullet = invField(InvBullet, 0, 0, 0.4, 1.2);
   %cl.invUfo = invField(InvUfo, 0, 0, 3, 1.2);
   %g.add(%cl.invPad);
   %g.add(%cl.invBullet);
   %g.add(%cl.invUfo);
   invPark(%cl.invBullet);
   invPark(%cl.invUfo);
   for (%b = 0; %b < $Inv::MaxBombs; %b++)
   {
      %o = invField(InvBomb, 0, 0, 0.5, 1.0);
      %g.add(%o);
      invPark(%o);
      $Inv::Bomb[%seat, %b] = %o;
   }
   for (%row = 0; %row < $Inv::Rows; %row++)
      for (%col = 0; %col < $Inv::Cols; %col++)
      {
         %a = invField("InvAlien" @ invAlienKind(%row), 0, 0, $Inv::AlienW, $Inv::AlienH);
         %g.add(%a);
         invPark(%a);
         $Inv::Alien[%seat, %row * $Inv::Cols + %col] = %a;
      }
   for (%s = 0; %s < 4; %s++)
      for (%r = 0; %r < $Inv::ShieldRows; %r++)
         for (%c = 0; %c < $Inv::ShieldCols; %c++)
         {
            %i = (%s * $Inv::ShieldRows + %r) * $Inv::ShieldCols + %c;
            %blk = invField(InvShield, 0, 0, $Inv::Cell - 0.08, $Inv::Cell - 0.08);
            %g.add(%blk);
            invPark(%blk);
            $Inv::Shield[%seat, %i] = %blk;
            $Inv::ShieldOn[%seat, %i] = false;
         }
   invPurgeZones();
}

function invAlienKind(%row)
{
   if (%row >= 4)
      return 2;
   if (%row >= 2)
      return 1;
   return 0;
}

function invResetShields(%cl)
{
   %seat = %cl.invSeat;
   for (%s = 0; %s < 4; %s++)
      for (%r = 0; %r < $Inv::ShieldRows; %r++)
         for (%c = 0; %c < $Inv::ShieldCols; %c++)
         {
            %i = (%s * $Inv::ShieldRows + %r) * $Inv::ShieldCols + %c;
            // the notch under each shield: three cells missing from the bottom middle
            %notch = %r == 0 && %c >= 2 && %c <= 4;
            if (%notch)
            {
               invPark($Inv::Shield[%seat, %i]);
               $Inv::ShieldOn[%seat, %i] = false;
               continue;
            }
            $Inv::ShieldOn[%seat, %i] = true;
            invPlace($Inv::Shield[%seat, %i], %seat, $Inv::ShieldU[%s] + (%c - 3) * $Inv::Cell,
               $Inv::ShieldV + %r * $Inv::Cell + $Inv::Cell / 2, $Inv::Cell - 0.08, $Inv::Cell - 0.08);
         }
}

function invStartGame(%cl)
{
   %cl.invScore = 0;
   %cl.invLives = $Inv::Lives;
   %cl.invWave = 1;
   invNewWave(%cl);
}

function invNewWave(%cl)
{
   %seat = %cl.invSeat;
   invResetShields(%cl);
   %cl.invFU = 0;
   %lower = %cl.invWave - 1;
   if (%lower > 4)
      %lower = 4;
   %cl.invFV = $Inv::FormationV - %lower * $Inv::DropV;
   %cl.invDir = 1;
   %cl.invCount = 0;
   for (%i = 0; %i < $Inv::Rows * $Inv::Cols; %i++)
   {
      $Inv::AlienOn[%seat, %i] = true;
      %cl.invCount++;
   }
   invDrawAliens(%cl);
   invClearShots(%cl);
   %cl.invU = 0;
   %cl.invState = "play";
   %cl.invStepAt = getSimTime() + 900;
   %cl.invBombAt = getSimTime() + 2500;
   %cl.invUfoAt = getSimTime() + 12000 + getRandom() * 12000;
   invHud(%cl);
}

function invDrawAliens(%cl)
{
   %seat = %cl.invSeat;
   for (%row = 0; %row < $Inv::Rows; %row++)
      for (%col = 0; %col < $Inv::Cols; %col++)
      {
         %i = %row * $Inv::Cols + %col;
         if ($Inv::AlienOn[%seat, %i])
            invPlace($Inv::Alien[%seat, %i], %seat, invAlienU(%cl, %col), invAlienV(%cl, %row), $Inv::AlienW, $Inv::AlienH);
      }
}

function invClearShots(%cl)
{
   %seat = %cl.invSeat;
   %cl.invBulletOn = false;
   invPark(%cl.invBullet);
   for (%b = 0; %b < $Inv::MaxBombs; %b++)
   {
      $Inv::BombOn[%seat, %b] = false;
      invPark($Inv::Bomb[%seat, %b]);
   }
   %cl.invUfoOn = false;
   invPark(%cl.invUfo);
}

function invHud(%cl)
{
   if (%cl.invState $= "over")
   {
      bottomPrint(%cl, "GAME OVER    SCORE " @ %cl.invScore, 0, 1);
      return;
   }
   bottomPrint(%cl, "SCORE " @ %cl.invScore @ "    LIVES " @ %cl.invLives @ "    WAVE " @ %cl.invWave @ "     strafe = move, fire = shoot", 0, 1);
}

// ---------------------------------------------------------------------------------------------------------
// the game

// the lowest alive alien of a column, or -1
function invLowestInColumn(%cl, %col)
{
   %seat = %cl.invSeat;
   for (%row = 0; %row < $Inv::Rows; %row++)
      if ($Inv::AlienOn[%seat, %row * $Inv::Cols + %col])
         return %row;
   return -1;
}

function invInterval(%cl)
{
   // seconds per step: slow with a full formation, frantic with one alien left
   %t = 0.03 + %cl.invCount * 0.0135;
   %t = %t * (1 - (%cl.invWave - 1) * 0.07);
   if (%t < 0.03)
      %t = 0.03;
   return %t * 1000;
}

function invStep(%cl)
{
   %seat = %cl.invSeat;
   // extents of the surviving aliens
   %minCol = 99;
   %maxCol = -1;
   %minRow = 99;
   for (%col = 0; %col < $Inv::Cols; %col++)
   {
      %r = invLowestInColumn(%cl, %col);
      if (%r < 0)
         continue;
      if (%col < %minCol) %minCol = %col;
      if (%col > %maxCol) %maxCol = %col;
      if (%r < %minRow) %minRow = %r;
   }
   if (%maxCol < 0)
      return;
   %right = invAlienU(%cl, %maxCol) + $Inv::AlienW / 2;
   %left = invAlienU(%cl, %minCol) - $Inv::AlienW / 2;
   %edge = $Inv::HalfW - 0.8;
   if (%cl.invDir > 0 && %right + $Inv::StepU > %edge)
   {
      %cl.invFV -= $Inv::DropV;
      %cl.invDir = -1;
   }
   else if (%cl.invDir < 0 && %left - $Inv::StepU < -%edge)
   {
      %cl.invFV -= $Inv::DropV;
      %cl.invDir = 1;
   }
   else
      %cl.invFU += %cl.invDir * $Inv::StepU;
   invDrawAliens(%cl);
   // the formation grinds shields away as it passes over them
   %bottom = invAlienV(%cl, %minRow) - $Inv::AlienH / 2;
   if (%bottom < $Inv::ShieldV + $Inv::ShieldRows * $Inv::Cell)
      invGrindShields(%cl, %bottom, invAlienU(%cl, %minCol) - $Inv::AlienW / 2, invAlienU(%cl, %maxCol) + $Inv::AlienW / 2);
   if (%bottom < $Inv::LandV)
      invGameOver(%cl, "the aliens landed");
}

function invGrindShields(%cl, %bottom, %left, %right)
{
   %seat = %cl.invSeat;
   for (%s = 0; %s < 4; %s++)
      for (%r = 0; %r < $Inv::ShieldRows; %r++)
      {
         %v = $Inv::ShieldV + %r * $Inv::Cell + $Inv::Cell / 2;
         if (%v < %bottom)
            continue;
         for (%c = 0; %c < $Inv::ShieldCols; %c++)
         {
            %i = (%s * $Inv::ShieldRows + %r) * $Inv::ShieldCols + %c;
            %u = $Inv::ShieldU[%s] + (%c - 3) * $Inv::Cell;
            if ($Inv::ShieldOn[%seat, %i] && %u >= %left && %u <= %right)
            {
               $Inv::ShieldOn[%seat, %i] = false;
               invPark($Inv::Shield[%seat, %i]);
            }
         }
      }
}

// a point against the shields: removes the block it lands in; returns true on a hit
function invShieldHit(%cl, %u, %v)
{
   %seat = %cl.invSeat;
   for (%s = 0; %s < 4; %s++)
   {
      %c = mFloor((%u - ($Inv::ShieldU[%s] - $Inv::ShieldCols * $Inv::Cell / 2)) / $Inv::Cell);
      %r = mFloor((%v - $Inv::ShieldV) / $Inv::Cell);
      if (%c < 0 || %c >= $Inv::ShieldCols || %r < 0 || %r >= $Inv::ShieldRows)
         continue;
      %i = (%s * $Inv::ShieldRows + %r) * $Inv::ShieldCols + %c;
      if ($Inv::ShieldOn[%seat, %i])
      {
         $Inv::ShieldOn[%seat, %i] = false;
         invPark($Inv::Shield[%seat, %i]);
         return true;
      }
   }
   return false;
}

function invKillAlien(%cl, %i)
{
   %seat = %cl.invSeat;
   $Inv::AlienOn[%seat, %i] = false;
   invPark($Inv::Alien[%seat, %i]);
   %cl.invCount--;
   %cl.invScore += invRowPoints(mFloor(%i / $Inv::Cols));
   invHud(%cl);
   if (%cl.invCount <= 0)
   {
      %cl.invWave++;
      invNewWave(%cl);
   }
}

function invPlayerHit(%cl)
{
   %cl.invLives--;
   invClearShots(%cl);
   if (%cl.invLives <= 0)
   {
      invGameOver(%cl, "out of lives");
      return;
   }
   %cl.invState = "dead";
   %cl.invRespawnAt = getSimTime() + $Inv::RespawnMs;
   invHud(%cl);
}

function invGameOver(%cl, %why)
{
   if (%cl.invState $= "over")
      return;
   %cl.invState = "over";
   %cl.invOverAt = getSimTime() + $Inv::GameOverMs;
   if (%cl.invScore > %cl.invHigh)
      %cl.invHigh = %cl.invScore;
   messageAll('MsgInvaders', '\c1%1 is done: %2 (%3 points).', %cl.name, %why, %cl.invScore);
   invHud(%cl);
}

function invFire(%cl)
{
   if (%cl.invState !$= "play" || %cl.invBulletOn)
      return;
   %cl.invBulletOn = true;
   %cl.invBU = %cl.invU;
   %cl.invBV = $Inv::PadV + $Inv::PadH / 2 + 0.6;
}

function invPhysics(%cl, %dt, %now)
{
   %seat = %cl.invSeat;
   // the cannon
   %dir = 0;
   if (%cl.invPadMask & 1)
      %dir = -1;
   else if (%cl.invPadMask & 2)
      %dir = 1;
   %max = $Inv::HalfW - $Inv::PadW / 2;
   if (%cl.invState $= "play")
   {
      %cl.invU += %dir * $Inv::PadSpeed * %dt;
      if (%cl.invU > %max) %cl.invU = %max;
      if (%cl.invU < -%max) %cl.invU = -%max;
   }
   if (%cl.invState $= "over")
   {
      if (%now >= %cl.invOverAt)
         invStartGame(%cl);
   }
   else if (%cl.invState $= "dead")
   {
      if (%now >= %cl.invRespawnAt)
      {
         %cl.invState = "play";
         %cl.invBombAt = %now + 1500;
         invHud(%cl);
      }
   }
   else
   {
      // the formation
      if (%now >= %cl.invStepAt)
      {
         invStep(%cl);
         %cl.invStepAt = %now + invInterval(%cl);
      }
      // the cannon's shot
      if (%cl.invBulletOn)
      {
         %steps = mCeil($Inv::BulletSpeed * %dt / 0.4);
         if (%steps < 1) %steps = 1;
         for (%k = 0; %k < %steps && %cl.invBulletOn; %k++)
         {
            %cl.invBV += $Inv::BulletSpeed * %dt / %steps;
            %u = %cl.invBU;
            %v = %cl.invBV;
            if (%v > $Inv::Height - 0.6)
            {
               %cl.invBulletOn = false;
               break;
            }
            if (invShieldHit(%cl, %u, %v))
            {
               %cl.invBulletOn = false;
               break;
            }
            // aliens
            %col = mFloor((%u - (%cl.invFU - ($Inv::Cols - 1) / 2 * $Inv::AlienDU) + $Inv::AlienDU / 2) / $Inv::AlienDU);
            %row = mFloor((%v - %cl.invFV + $Inv::AlienDV / 2) / $Inv::AlienDV);
            if (%col >= 0 && %col < $Inv::Cols && %row >= 0 && %row < $Inv::Rows)
            {
               %i = %row * $Inv::Cols + %col;
               if ($Inv::AlienOn[%seat, %i] && mAbs(%u - invAlienU(%cl, %col)) <= $Inv::AlienW / 2
                  && mAbs(%v - invAlienV(%cl, %row)) <= $Inv::AlienH / 2 + 0.3)
               {
                  %cl.invBulletOn = false;
                  invKillAlien(%cl, %i);
                  break;
               }
            }
            // the mystery ship
            if (%cl.invUfoOn && mAbs(%v - $Inv::UfoV) <= 1.0 && mAbs(%u - %cl.invUfoU) <= 1.7)
            {
               %cl.invBulletOn = false;
               %cl.invUfoOn = false;
               invPark(%cl.invUfo);
               %pts = getWord("50 100 150 300", mFloor(getRandom() * 3.99));
               %cl.invScore += %pts;
               invHud(%cl);
               break;
            }
         }
      }
      if (%cl.invState !$= "play")
         return;
      // bombs
      for (%b = 0; %b < $Inv::MaxBombs; %b++)
      {
         if (!$Inv::BombOn[%seat, %b])
            continue;
         $Inv::BombV[%seat, %b] -= $Inv::BombSpeed * %dt;
         %bu = $Inv::BombU[%seat, %b];
         %bv = $Inv::BombV[%seat, %b];
         if (%bv < 0.5 || invShieldHit(%cl, %bu, %bv))
         {
            $Inv::BombOn[%seat, %b] = false;
            invPark($Inv::Bomb[%seat, %b]);
            continue;
         }
         if (%bv <= $Inv::PadV + $Inv::PadH / 2 + 0.4 && %bv >= $Inv::PadV - 1 && mAbs(%bu - %cl.invU) <= $Inv::PadW / 2 + 0.3)
         {
            invPlayerHit(%cl);
            return;
         }
         invPlace($Inv::Bomb[%seat, %b], %seat, %bu, %bv, 0.5, 1.0);
      }
      // a new bomb from the bottom of a random column
      if (%now >= %cl.invBombAt)
      {
         %cl.invBombAt = %now + 700 + getRandom() * 900;
         for (%b = 0; %b < $Inv::MaxBombs; %b++)
            if (!$Inv::BombOn[%seat, %b])
            {
               %col = mFloor(getRandom() * $Inv::Cols * 0.999);
               %row = invLowestInColumn(%cl, %col);
               if (%row >= 0)
               {
                  $Inv::BombOn[%seat, %b] = true;
                  $Inv::BombU[%seat, %b] = invAlienU(%cl, %col);
                  $Inv::BombV[%seat, %b] = invAlienV(%cl, %row) - 1;
               }
               break;
            }
      }
      // the mystery ship
      if (%cl.invUfoOn)
      {
         %cl.invUfoU += %cl.invUfoDir * $Inv::UfoSpeed * %dt;
         if (mAbs(%cl.invUfoU) > $Inv::HalfW + 2)
         {
            %cl.invUfoOn = false;
            invPark(%cl.invUfo);
         }
         else
            invPlace(%cl.invUfo, %seat, %cl.invUfoU, $Inv::UfoV, 3, 1.2);
      }
      else if (%now >= %cl.invUfoAt)
      {
         %cl.invUfoAt = %now + 15000 + getRandom() * 15000;
         %cl.invUfoOn = true;
         %cl.invUfoDir = 1;
         %cl.invUfoU = -$Inv::HalfW - 1.5;
         if (getRandom() < 0.5)
         {
            %cl.invUfoDir = -1;
            %cl.invUfoU = $Inv::HalfW + 1.5;
         }
      }
   }
   // the cannon and its shot are drawn every tick
   if (%cl.invState $= "dead")
      invPark(%cl.invPad);
   else
      invPlace(%cl.invPad, %seat, %cl.invU, $Inv::PadV, $Inv::PadW, $Inv::PadH);
   if (%cl.invBulletOn)
      invPlace(%cl.invBullet, %seat, %cl.invBU, %cl.invBV, 0.4, 1.2);
   else
      invPark(%cl.invBullet);
}

// ---------------------------------------------------------------------------------------------------------
// input: the player's keys, read by Retro.cs from its (tiny) velocity; 1 while strafe-left is held, 2 for right

function invReadPad(%cl)
{
   %m = retroPad(%cl);
   %out = 0;
   if (%m & 4)
      %out = %out | 1;
   if (%m & 8)
      %out = %out | 2;
   return %out;
}

function invTick()
{
   cancel($Inv::TickThread);
   $Inv::TickThread = schedule($Inv::TickMs, 0, invTick);
   if (!isObject(MissionCleanup))
      return;
   $Inv::TickN++;
   %now = getSimTime();
   %dt = $Inv::LastTick $= "" ? 0.032 : (%now - $Inv::LastTick) / 1000;
   $Inv::LastTick = %now;
   if (%dt > 0.1) %dt = 0.1;
   for (%i = 0; %i < ClientGroup.getCount(); %i++)
   {
      %cl = ClientGroup.getObject(%i);
      if (!%cl.invJoined || !isObject(%cl.camera))
         continue;
      if (%cl.invSeat $= "")
      {
         // spectators have the stock free-fly camera; the stand-in Player is parked out of the way
         if (isObject(%cl.player))
         {
            %cl.player.setVelocity("0 0 0");
            if ($Inv::TickN % 30 == 0)
               %cl.player.setTransform("0 -150 300 1 0 0 0");
         }
         if (%cl.getControlObject() != %cl.camera)
            %cl.setControlObject(%cl.camera);
         continue;
      }
      %cl.invPadMask = invReadPad(%cl);
      invPhysics(%cl, %dt, %now);
   }
}

function invInitPlayer(%cl)
{
   %cl.invJoined = true;
   if (%cl.invSeat $= "")
   {
      %seat = invFreeSeat();
      if (%seat !$= "")
      {
         %cl.invSeat = %seat;
         invMakeBoard(%cl);
      }
   }
   if (%cl.invSeat $= "")
   {
      // a spectator: the free-fly camera, parked player
      %cam = %cl.camera;
      %cam.setFlyMode();
      %cam.setTransform(invAnchor(0));
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
   %a = invAnchor(%cl.invSeat);
   retroAttach(%cl, getWord(%a, 0), getWord(%a, 1), getWord(%a, 2));
   commandToClient(%cl, 'setHudMode', 'Standard');
   invStartGame(%cl);
}

function invFreeClient(%cl)
{
   retroDetach(%cl);
   if (isObject(%cl.invGroup))
      %cl.invGroup.delete();
   %cl.invGroup = "";
   %cl.invSeat = "";
   %cl.invJoined = false;
}

// Switch the package on and off with the mission, so stock DM missions are untouched.
function invWatch()
{
   cancel($Inv::WatchThread);
   $Inv::WatchThread = schedule(250, 0, invWatch);
   %on = ($CurrentMission $= $Inv::Mission && isObject(MissionCleanup));
   if (%on && !$Inv::PkgOn)
   {
      activatePackage(T2LaunchInvaders);
      $Inv::PkgOn = true;
      $Inv::LastTick = "";
      invTick();
      echo("Invaders ruleset on");
   }
   else if (!%on && $Inv::PkgOn)
   {
      deactivatePackage(T2LaunchInvaders);
      $Inv::PkgOn = false;
      cancel($Inv::TickThread);
      echo("Invaders ruleset off");
   }
}

package T2LaunchInvadersBoot
{
   function CreateServer(%mission, %missionType)
   {
      Parent::CreateServer(%mission, %missionType);
      invDatablocks();
   }
};

package T2LaunchInvaders
{
   function DMGame::equip(%game, %player)
   {
      // nobody carries anything
   }

   function DMGame::createPlayer(%game, %client, %spawnLoc, %respawn)
   {
      Parent::createPlayer(%game, %client, %spawnLoc, %respawn);
      invInitPlayer(%client);
   }

   function DMGame::onClientKilled(%game, %clVictim, %clKiller, %damageType, %implement, %damageLoc)
   {
      // the stand-in Player is not part of the game: no respawn timer or orbit camera
      %clVictim.deaths++;
   }

   function DMGame::ObserverOnTrigger(%game, %data, %obj, %trigger, %state)
   {
      %client = %obj.getControllingClient();
      if (%client && %client.invJoined)
      {
         if (%state && %trigger == 0 && %client.invSeat !$= "")
            invFire(%client);
         return false;
      }
      return Parent::ObserverOnTrigger(%game, %data, %obj, %trigger, %state);
   }

   // a seated player is a Player now, so its triggers arrive here: fire shoots
   function Armor::onTrigger(%data, %player, %trigger, %val)
   {
      %client = %player.getControllingClient();
      if (%client && %client.invJoined)
      {
         if (%val && %trigger == 0 && %client.invSeat !$= "")
            invFire(%client);
         return;
      }
      Parent::onTrigger(%data, %player, %trigger, %val);
   }

   function DMGame::onClientLeaveGame(%game, %client)
   {
      invFreeClient(%client);
      Parent::onClientLeaveGame(%game, %client);
   }
};

if (!$Inv::Installed)
{
   $Inv::Installed = true;
   activatePackage(T2LaunchInvadersBoot);
   invDatablocks();
   invWatch();
   echo("Invaders gametype script loaded");
}
