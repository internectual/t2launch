// Tetris: server-side falling-block ruleset for the MatrixTetris mission.
// Evaluated in memory by the t2launch hook (server.gametype_script); stock clients download nothing.
// The package is active only while $CurrentMission is MatrixTetris.
//
// Rules follow the modern guideline: 10x20 board, SRS rotation and wall kicks, 7-bag, hold, ghost piece,
// lock delay, soft/hard drop scoring, T-spins, back-to-back, combos. Every player gets a board in a row;
// line clears send garbage to a random opponent.
//
// Controls (stock keybinds): strafe = move (with DAS/ARR), forward = hard drop, back = soft drop,
// fire = rotate counter-clockwise, jump or jet = rotate clockwise, alt-fire or throw = hold.
// The client's control object is its Camera; its movement each tick is read as the d-pad (as in Tron.cs) and
// press events arrive through Observer::onTrigger.

$Tet::Mission = "MatrixTetris";
$Tet::Cell = 3;                      // metres per block
$Tet::Cols = 10;
$Tet::Rows = 20;                     // visible rows; pieces spawn in hidden rows 20-21
$Tet::Z0 = 300;                      // bottom of the board
$Tet::Spacing = 46;                  // between boards
$Tet::CamDist = 70;                  // camera distance in front of the board plane
$Tet::CamZShift = -3;                // camera below the board centre, so the board sits above the HUD
$Tet::TickMs = 32;
$Tet::KeyGraceMs = 110;              // a key counts as held this long after its last sample
$Tet::DasMs = 210;                   // delay before auto-repeat
$Tet::ArrMs = 40;                    // auto-repeat interval
$Tet::LockMs = 500;
$Tet::LockResets = 15;
$Tet::HardDropGapMs = 220;           // forward must be released this long before it hard-drops again
$Tet::NextCount = 5;
$Tet::RespawnMs = 3500;
$Tet::Debug = 0;

// piece kinds: 0 I, 1 O, 2 T, 3 S, 4 Z, 5 J, 6 L
$Tet::Color[0] = "0.00 0.90 0.95";
$Tet::Color[1] = "0.95 0.90 0.10";
$Tet::Color[2] = "0.70 0.20 0.90";
$Tet::Color[3] = "0.20 0.90 0.20";
$Tet::Color[4] = "0.95 0.15 0.15";
$Tet::Color[5] = "0.15 0.30 0.98";
$Tet::Color[6] = "0.98 0.55 0.05";

// SRS shapes as "col row" pairs inside the piece box (row 0 is the top). States: 0, R, 2, L.
$Tet::S[0,0] = "0 1 1 1 2 1 3 1";  $Tet::S[0,1] = "2 0 2 1 2 2 2 3";  $Tet::S[0,2] = "0 2 1 2 2 2 3 2";  $Tet::S[0,3] = "1 0 1 1 1 2 1 3";
$Tet::S[1,0] = "1 0 2 0 1 1 2 1";  $Tet::S[1,1] = "1 0 2 0 1 1 2 1";  $Tet::S[1,2] = "1 0 2 0 1 1 2 1";  $Tet::S[1,3] = "1 0 2 0 1 1 2 1";
$Tet::S[2,0] = "1 0 0 1 1 1 2 1";  $Tet::S[2,1] = "1 0 1 1 2 1 1 2";  $Tet::S[2,2] = "0 1 1 1 2 1 1 2";  $Tet::S[2,3] = "1 0 0 1 1 1 1 2";
$Tet::S[3,0] = "1 0 2 0 0 1 1 1";  $Tet::S[3,1] = "1 0 1 1 2 1 2 2";  $Tet::S[3,2] = "1 1 2 1 0 2 1 2";  $Tet::S[3,3] = "0 0 0 1 1 1 1 2";
$Tet::S[4,0] = "0 0 1 0 1 1 2 1";  $Tet::S[4,1] = "2 0 1 1 2 1 1 2";  $Tet::S[4,2] = "0 1 1 1 1 2 2 2";  $Tet::S[4,3] = "1 0 0 1 1 1 0 2";
$Tet::S[5,0] = "0 0 0 1 1 1 2 1";  $Tet::S[5,1] = "1 0 2 0 1 1 1 2";  $Tet::S[5,2] = "0 1 1 1 2 1 2 2";  $Tet::S[5,3] = "1 0 1 1 0 2 1 2";
$Tet::S[6,0] = "2 0 0 1 1 1 2 1";  $Tet::S[6,1] = "1 0 1 1 1 2 2 2";  $Tet::S[6,2] = "0 1 1 1 2 1 0 2";  $Tet::S[6,3] = "0 0 1 0 1 1 1 2";

// SRS wall kicks as "x y" pairs (y up). Group 0 = J L S T Z, group 1 = I. O does not kick.
$Tet::K[0,0,1] = "0 0 -1 0 -1 1 0 -2 -1 -2";  $Tet::K[0,1,0] = "0 0 1 0 1 -1 0 2 1 2";
$Tet::K[0,1,2] = "0 0 1 0 1 -1 0 2 1 2";      $Tet::K[0,2,1] = "0 0 -1 0 -1 1 0 -2 -1 -2";
$Tet::K[0,2,3] = "0 0 1 0 1 1 0 -2 1 -2";     $Tet::K[0,3,2] = "0 0 -1 0 -1 -1 0 2 -1 2";
$Tet::K[0,3,0] = "0 0 -1 0 -1 -1 0 2 -1 2";   $Tet::K[0,0,3] = "0 0 1 0 1 1 0 -2 1 -2";
$Tet::K[1,0,1] = "0 0 -2 0 1 0 -2 -1 1 2";    $Tet::K[1,1,0] = "0 0 2 0 -1 0 2 1 -1 -2";
$Tet::K[1,1,2] = "0 0 -1 0 2 0 -1 2 2 -1";    $Tet::K[1,2,1] = "0 0 1 0 -2 0 1 -2 -2 1";
$Tet::K[1,2,3] = "0 0 2 0 -1 0 2 1 -1 -2";    $Tet::K[1,3,2] = "0 0 -2 0 1 0 -2 -1 1 2";
$Tet::K[1,3,0] = "0 0 1 0 -2 0 1 -2 -2 1";    $Tet::K[1,0,3] = "0 0 -1 0 2 0 -1 2 2 -1";

// seat order, centre outwards
$Tet::SeatOffset[0] = 0;  $Tet::SeatOffset[1] = 1;  $Tet::SeatOffset[2] = -1; $Tet::SeatOffset[3] = 2;
$Tet::SeatOffset[4] = -2; $Tet::SeatOffset[5] = 3;  $Tet::SeatOffset[6] = -3; $Tet::SeatOffset[7] = 4;
$Tet::Seats = 8;

// garbage lines sent per combo count
$Tet::ComboGarbage[0] = 0; $Tet::ComboGarbage[1] = 0; $Tet::ComboGarbage[2] = 1; $Tet::ComboGarbage[3] = 1;
$Tet::ComboGarbage[4] = 2; $Tet::ComboGarbage[5] = 2; $Tet::ComboGarbage[6] = 3; $Tet::ComboGarbage[7] = 3;
$Tet::ComboGarbage[8] = 4; $Tet::ComboGarbage[9] = 4; $Tet::ComboGarbage[10] = 4; $Tet::ComboGarbage[11] = 5;

// ---------------------------------------------------------------------------------------------------------
// datablocks: deleted by DestroyServer, so defined at install and again after every CreateServer

function tetDatablocks()
{
   if (isObject(TetCol0))
      return;
   for (%i = 0; %i < 7; %i++)
      eval("datablock ForceFieldBareData(TetCol" @ %i @ ") : defaultForceFieldBare {"
         @ " color = \"" @ $Tet::Color[%i] @ "\"; baseTranslucency = 0.6; needsNoPower = true; };");
   eval("datablock ForceFieldBareData(TetGhost) : defaultForceFieldBare {"
      @ " color = \"0.95 0.95 0.95\"; baseTranslucency = 0.3; needsNoPower = true; };");
   eval("datablock ForceFieldBareData(TetGarbage) : defaultForceFieldBare {"
      @ " color = \"0.55 0.55 0.60\"; baseTranslucency = 0.6; needsNoPower = true; };");
   eval("datablock ForceFieldBareData(TetFrame) : defaultForceFieldBare {"
      @ " color = \"0.9 0.9 0.9\"; baseTranslucency = 0.35; needsNoPower = true; };");
}

function tetField(%datablock, %x, %y, %z, %sx, %sy, %sz)
{
   return new ForceFieldBare() {
      dataBlock = %datablock;
      position = %x SPC %y SPC %z;
      rotation = "1 0 0 0";
      scale = %sx SPC %sy SPC %sz;
   };
}

// every force field adds a PhysicalZone in MissionCleanup/PZones; they are useless here, so drop them
function tetPurgeZones()
{
   %zones = nameToID("MissionCleanup/PZones");
   if (%zones > 0)
      %zones.delete();
}

// ---------------------------------------------------------------------------------------------------------
// geometry helpers

function tetBoardX(%seat)
{
   return $Tet::SeatOffset[%seat] * $Tet::Spacing - $Tet::Cols * $Tet::Cell / 2;
}

function tetCellX(%seat, %col)
{
   return tetBoardX(%seat) + %col * $Tet::Cell;
}

function tetCellZ(%row)
{
   return $Tet::Z0 + %row * $Tet::Cell;
}

function tetAnchor(%seat)
{
   return (tetBoardX(%seat) + $Tet::Cols * $Tet::Cell / 2) SPC (-$Tet::CamDist) SPC ($Tet::Z0 + $Tet::Rows * $Tet::Cell / 2 + $Tet::CamZShift) SPC "1 0 0 0";
}

// "c r c r c r c r" board cells of a piece whose box top-left is at (col, row)
function tetCells(%kind, %rot, %col, %row)
{
   %shape = $Tet::S[%kind, %rot];
   %out = "";
   for (%i = 0; %i < 4; %i++)
      %out = %out SPC (%col + getWord(%shape, %i * 2)) SPC (%row - getWord(%shape, %i * 2 + 1));
   return trim(%out);
}

function tetFits(%seat, %cells)
{
   for (%i = 0; %i < 4; %i++)
   {
      %c = getWord(%cells, %i * 2);
      %r = getWord(%cells, %i * 2 + 1);
      if (%c < 0 || %c >= $Tet::Cols || %r < 0 || %r > 39)
         return false;
      if ($Tet::O[%seat, %r, %c] !$= "")
         return false;
   }
   return true;
}

function tetFilled(%seat, %c, %r)
{
   if (%c < 0 || %c >= $Tet::Cols || %r < 0)
      return true;
   return $Tet::O[%seat, %r, %c] !$= "";
}

// seconds per row for a level, as milliseconds: (0.8 - (level - 1) * 0.007) ^ (level - 1)
function tetGravityMs(%level)
{
   %base = 0.8 - (%level - 1) * 0.007;
   %t = 1.0;
   for (%i = 1; %i < %level; %i++)
      %t = %t * %base;
   %ms = %t * 1000;
   if (%ms < 17)
      %ms = 17;
   return %ms;
}

// ---------------------------------------------------------------------------------------------------------
// seats and frames

function tetFreeSeat()
{
   for (%s = 0; %s < $Tet::Seats; %s++)
   {
      %used = false;
      for (%i = 0; %i < ClientGroup.getCount(); %i++)
         if (ClientGroup.getObject(%i).tetSeat $= %s)
            %used = true;
      if (!%used)
         return %s;
   }
   return -1;
}

function tetMakeFrame(%cl)
{
   %seat = %cl.tetSeat;
   %bx = tetBoardX(%seat);
   %w = $Tet::Cols * $Tet::Cell;
   %h = $Tet::Rows * $Tet::Cell;
   %t = 1.2;
   %g = new SimGroup();
   MissionCleanup.add(%g);
   %g.add(tetField(TetFrame, %bx - %t, -1.5, $Tet::Z0 - %t, %t, 3, %h + %t));
   %g.add(tetField(TetFrame, %bx + %w, -1.5, $Tet::Z0 - %t, %t, 3, %h + %t));
   %g.add(tetField(TetFrame, %bx - %t, -1.5, $Tet::Z0 - %t, %w + %t * 2, 3, %t));
   %g.add(tetField(TetFrame, %bx - %t, -1.5, $Tet::Z0 + %h, %w + %t * 2, 3, 0.4));   // top of the visible field
   %cl.tetFrame = %g;
}

function tetFreeBoard(%cl)
{
   if (isObject(%cl.tetGroup))
      %cl.tetGroup.delete();
   if (isObject(%cl.tetPrev))
      %cl.tetPrev.delete();
   %cl.tetGroup = "";
   %cl.tetPrev = "";
   %cl.tetAct = "";
   %cl.tetGhost = "";
   if (%cl.tetSeat !$= "")
      for (%r = 0; %r < 40; %r++)
         for (%c = 0; %c < $Tet::Cols; %c++)
            $Tet::O[%cl.tetSeat, %r, %c] = "";
}

// ---------------------------------------------------------------------------------------------------------
// the game for one player

function tetRefill(%cl)
{
   while (getWordCount(%cl.tetQ) < $Tet::NextCount + 7)
   {
      %bag = "0 1 2 3 4 5 6";
      for (%i = 6; %i > 0; %i--)
      {
         %j = mFloor(getRandom() * (%i + 1));
         %a = getWord(%bag, %i);
         %bag = setWord(%bag, %i, getWord(%bag, %j));
         %bag = setWord(%bag, %j, %a);
      }
      %cl.tetQ = %cl.tetQ $= "" ? %bag : %cl.tetQ SPC %bag;
   }
}

function tetPopNext(%cl)
{
   tetRefill(%cl);
   %k = getWord(%cl.tetQ, 0);
   %cl.tetQ = getWords(%cl.tetQ, 1);
   return %k;
}

function tetStartBoard(%cl)
{
   tetFreeBoard(%cl);
   %cl.tetGroup = new SimGroup();
   MissionCleanup.add(%cl.tetGroup);
   %cl.tetScore = 0;
   %cl.tetLines = 0;
   %cl.tetLevel = 1;
   %cl.tetCombo = -1;
   %cl.tetB2B = false;
   %cl.tetPending = 0;
   %cl.tetHold = "";
   %cl.tetQ = "";
   %cl.tetAlive = true;
   %cl.tetQueueIn = "";
   %cl.tetLastCam = "";
   %cl.tetDir = 0;
   tetSpawn(%cl, tetPopNext(%cl));
   tetHud(%cl);
}

function tetHud(%cl)
{
   %text = "SCORE " @ %cl.tetScore @ "    LINES " @ %cl.tetLines @ "    LEVEL " @ %cl.tetLevel;
   if (%cl.tetPending > 0)
      %text = %text @ "    INCOMING " @ %cl.tetPending;
   bottomPrint(%cl, %text, 0, 1);
}

function tetDeleteList(%list)
{
   for (%i = 0; %i < getWordCount(%list); %i++)
   {
      %o = getWord(%list, %i);
      if (isObject(%o))
         %o.delete();
   }
}

// create the four blocks of the falling piece and its ghost
function tetMakePiece(%cl)
{
   tetDeleteList(%cl.tetAct);
   tetDeleteList(%cl.tetGhost);
   %cl.tetAct = "";
   %cl.tetGhost = "";
   for (%i = 0; %i < 4; %i++)
   {
      %b = tetField("TetCol" @ %cl.tetKind, 0, -1.5, 0, $Tet::Cell, 3, $Tet::Cell);
      %cl.tetGroup.add(%b);
      %cl.tetAct = %cl.tetAct SPC %b;
      %g = tetField(TetGhost, 0, -1.2, 0, $Tet::Cell - 0.6, 2.4, $Tet::Cell - 0.6);
      %cl.tetGroup.add(%g);
      %cl.tetGhost = %cl.tetGhost SPC %g;
   }
   %cl.tetAct = trim(%cl.tetAct);
   %cl.tetGhost = trim(%cl.tetGhost);
}

function tetGhostRow(%cl)
{
   %row = %cl.tetRow;
   while (tetFits(%cl.tetSeat, tetCells(%cl.tetKind, %cl.tetRot, %cl.tetCol, %row - 1)))
      %row--;
   return %row;
}

// move the blocks and ghost to where the piece is
function tetPlace(%cl)
{
   %seat = %cl.tetSeat;
   %cells = tetCells(%cl.tetKind, %cl.tetRot, %cl.tetCol, %cl.tetRow);
   %gcells = tetCells(%cl.tetKind, %cl.tetRot, %cl.tetCol, tetGhostRow(%cl));
   for (%i = 0; %i < 4; %i++)
   {
      %b = getWord(%cl.tetAct, %i);
      if (isObject(%b))
         %b.setTransform(tetCellX(%seat, getWord(%cells, %i * 2)) SPC "-1.5" SPC tetCellZ(getWord(%cells, %i * 2 + 1)));
      %g = getWord(%cl.tetGhost, %i);
      if (isObject(%g))
         %g.setTransform((tetCellX(%seat, getWord(%gcells, %i * 2)) + 0.3) SPC "-1.2" SPC (tetCellZ(getWord(%gcells, %i * 2 + 1)) + 0.3));
   }
}

// small piece previews: held piece on the left, the next few on the right
function tetDrawPreviews(%cl)
{
   if (isObject(%cl.tetPrev))
      %cl.tetPrev.delete();
   %g = new SimGroup();
   MissionCleanup.add(%g);
   %cl.tetPrev = %g;
   %bx = tetBoardX(%cl.tetSeat);
   %s = 2;
   %top = $Tet::Z0 + $Tet::Rows * $Tet::Cell - 2;
   if (%cl.tetHold !$= "")
      tetPreviewPiece(%g, %cl.tetHold, %bx - 14, %top, %s, %cl.tetHoldUsed);
   for (%n = 0; %n < $Tet::NextCount; %n++)
      tetPreviewPiece(%g, getWord(%cl.tetQ, %n), %bx + $Tet::Cols * $Tet::Cell + 5, %top - %n * 9, %s, false);
}

function tetPreviewPiece(%group, %kind, %x, %z, %size, %dim)
{
   %shape = $Tet::S[%kind, 0];
   %db = %dim ? "TetGarbage" : "TetCol" @ %kind;
   for (%i = 0; %i < 4; %i++)
      %group.add(tetField(%db, %x + getWord(%shape, %i * 2) * %size, -1, %z - getWord(%shape, %i * 2 + 1) * %size, %size, 2, %size));
}

function tetSpawn(%cl, %kind)
{
   %cl.tetKind = %kind;
   %cl.tetRot = 0;
   %cl.tetCol = 3;
   %cl.tetRow = 21;
   %cl.tetFall = 0;
   %cl.tetLock = 0;
   %cl.tetResets = 0;
   %cl.tetLowest = 21;
   %cl.tetRotated = false;
   %cl.tetKick = 0;
   %cl.tetHoldUsed = false;
   if (!tetFits(%cl.tetSeat, tetCells(%kind, 0, 3, 21)))
   {
      tetTopOut(%cl);
      return;
   }
   // guideline: the piece drops one row at once if it can
   if (tetFits(%cl.tetSeat, tetCells(%kind, 0, 3, 20)))
   {
      %cl.tetRow = 20;
      %cl.tetLowest = 20;
   }
   tetMakePiece(%cl);
   tetPlace(%cl);
   tetDrawPreviews(%cl);
   tetPurgeZones();
}

function tetTopOut(%cl)
{
   %cl.tetAlive = false;
   %cl.tetRespawnAt = getSimTime() + $Tet::RespawnMs;
   tetDeleteList(%cl.tetAct);
   tetDeleteList(%cl.tetGhost);
   %cl.tetAct = "";
   %cl.tetGhost = "";
   bottomPrint(%cl, "TOP OUT    SCORE " @ %cl.tetScore @ "    LINES " @ %cl.tetLines, 0, 1);
   messageAll('MsgTetOut', '\c1%1 topped out with %2 points.', %cl.name, %cl.tetScore);
}

// -- moves ------------------------------------------------------------------------------------------------

function tetGrounded(%cl)
{
   return !tetFits(%cl.tetSeat, tetCells(%cl.tetKind, %cl.tetRot, %cl.tetCol, %cl.tetRow - 1));
}

// a successful move or rotation of a resting piece restarts the lock delay, a limited number of times
function tetMoved(%cl)
{
   if (tetGrounded(%cl) && %cl.tetResets < $Tet::LockResets)
   {
      %cl.tetLock = 0;
      %cl.tetResets++;
   }
}

function tetTryMove(%cl, %dc, %dr)
{
   %col = %cl.tetCol + %dc;
   %row = %cl.tetRow + %dr;
   if (!tetFits(%cl.tetSeat, tetCells(%cl.tetKind, %cl.tetRot, %col, %row)))
      return false;
   %cl.tetCol = %col;
   %cl.tetRow = %row;
   %cl.tetRotated = false;
   if (%row < %cl.tetLowest)
   {
      %cl.tetLowest = %row;
      %cl.tetResets = 0;
      %cl.tetLock = 0;
   }
   else if (%dr == 0)
      tetMoved(%cl);
   %cl.tetDirty = true;
   return true;
}

function tetRotate(%cl, %dir)
{
   if (%cl.tetKind == 1)
      return;
   %from = %cl.tetRot;
   %to = (%from + %dir + 4) % 4;
   %kicks = $Tet::K[%cl.tetKind == 0 ? 1 : 0, %from, %to];
   for (%i = 0; %i < getWordCount(%kicks) / 2; %i++)
   {
      %col = %cl.tetCol + getWord(%kicks, %i * 2);
      %row = %cl.tetRow + getWord(%kicks, %i * 2 + 1);
      if (tetFits(%cl.tetSeat, tetCells(%cl.tetKind, %to, %col, %row)))
      {
         %cl.tetRot = %to;
         %cl.tetCol = %col;
         %cl.tetRow = %row;
         %cl.tetRotated = true;
         %cl.tetKick = %i;
         if (%row < %cl.tetLowest)
         {
            %cl.tetLowest = %row;
            %cl.tetResets = 0;
            %cl.tetLock = 0;
         }
         else
            tetMoved(%cl);
         %cl.tetDirty = true;
         return;
      }
   }
}

function tetHoldPiece(%cl)
{
   if (%cl.tetHoldUsed)
      return;
   %cur = %cl.tetKind;
   if (%cl.tetHold $= "")
   {
      %cl.tetHold = %cur;
      tetSpawn(%cl, tetPopNext(%cl));
   }
   else
   {
      %next = %cl.tetHold;
      %cl.tetHold = %cur;
      tetSpawn(%cl, %next);
   }
   %cl.tetHoldUsed = true;
   tetDrawPreviews(%cl);
}

function tetHardDrop(%cl)
{
   %n = 0;
   while (tetTryMove(%cl, 0, -1))
      %n++;
   %cl.tetScore += %n * 2;
   tetLock(%cl);
}

// -- locking, line clears, scoring ------------------------------------------------------------------------

// 0 none, 1 mini, 2 full: three of the four corners around the T's centre must be filled
function tetTSpin(%cl)
{
   if (%cl.tetKind != 2 || !%cl.tetRotated)
      return 0;
   %cx = %cl.tetCol + 1;
   %cy = %cl.tetRow - 1;
   %a = tetFilled(%cl.tetSeat, %cx - 1, %cy + 1);   // top-left
   %b = tetFilled(%cl.tetSeat, %cx + 1, %cy + 1);   // top-right
   %c = tetFilled(%cl.tetSeat, %cx - 1, %cy - 1);   // bottom-left
   %d = tetFilled(%cl.tetSeat, %cx + 1, %cy - 1);   // bottom-right
   %n = (%a ? 1 : 0) + (%b ? 1 : 0) + (%c ? 1 : 0) + (%d ? 1 : 0);
   if (%n < 3)
      return 0;
   // the two corners the T points to are the "front" ones
   switch (%cl.tetRot)
   {
      case 0: %front = (%a ? 1 : 0) + (%b ? 1 : 0);
      case 1: %front = (%b ? 1 : 0) + (%d ? 1 : 0);
      case 2: %front = (%c ? 1 : 0) + (%d ? 1 : 0);
      case 3: %front = (%a ? 1 : 0) + (%c ? 1 : 0);
   }
   if (%front == 2 || %cl.tetKick == 4)
      return 2;
   return 1;
}

function tetLock(%cl)
{
   tetPlace(%cl);   // make sure the blocks are where the piece ended up before they become board cells
   %seat = %cl.tetSeat;
   %cells = tetCells(%cl.tetKind, %cl.tetRot, %cl.tetCol, %cl.tetRow);
   %tspin = tetTSpin(%cl);
   %allHigh = true;
   %rows = "";
   for (%i = 0; %i < 4; %i++)
   {
      %c = getWord(%cells, %i * 2);
      %r = getWord(%cells, %i * 2 + 1);
      $Tet::O[%seat, %r, %c] = getWord(%cl.tetAct, %i);
      if (%r < $Tet::Rows)
         %allHigh = false;
      if (strstr(" " @ %rows @ " ", " " @ %r @ " ") < 0)
         %rows = %rows SPC %r;
   }
   tetDeleteList(%cl.tetGhost);
   %cl.tetAct = "";
   %cl.tetGhost = "";
   // full rows among those just touched
   %cleared = "";
   %lines = 0;
   for (%i = 0; %i < getWordCount(%rows); %i++)
   {
      %r = getWord(%rows, %i);
      %full = true;
      for (%c = 0; %c < $Tet::Cols; %c++)
         if ($Tet::O[%seat, %r, %c] $= "")
            %full = false;
      if (%full)
      {
         %cleared = %cleared SPC %r;
         %lines++;
      }
   }
   if (%allHigh && %lines == 0)
   {
      tetTopOut(%cl);
      return;
   }
   if (%lines > 0)
      tetClearRows(%cl, trim(%cleared));
   tetScoreLock(%cl, %lines, %tspin);
   // incoming garbage lands when a piece locks without clearing
   if (%lines == 0 && %cl.tetPending > 0)
   {
      %n = %cl.tetPending > 8 ? 8 : %cl.tetPending;
      %cl.tetPending -= %n;
      tetAddGarbage(%cl, %n);
   }
   %cl.tetLevel = 1 + mFloor(%cl.tetLines / 10);
   tetHud(%cl);
   if (%cl.tetOverflow)
   {
      %cl.tetOverflow = false;
      tetTopOut(%cl);
   }
   if (%cl.tetAlive)
      tetSpawn(%cl, tetPopNext(%cl));
}

function tetClearRows(%cl, %cleared)
{
   %seat = %cl.tetSeat;
   // sort ascending
   %n = getWordCount(%cleared);
   for (%i = 0; %i < %n; %i++)
      for (%j = %i + 1; %j < %n; %j++)
         if (getWord(%cleared, %j) < getWord(%cleared, %i))
         {
            %t = getWord(%cleared, %i);
            %cleared = setWord(%cleared, %i, getWord(%cleared, %j));
            %cleared = setWord(%cleared, %j, %t);
         }
   for (%i = 0; %i < %n; %i++)
   {
      %r = getWord(%cleared, %i);
      for (%c = 0; %c < $Tet::Cols; %c++)
      {
         %o = $Tet::O[%seat, %r, %c];
         if (isObject(%o))
            %o.delete();
         $Tet::O[%seat, %r, %c] = "";
      }
   }
   // drop everything above the cleared rows
   for (%r = 0; %r < 40; %r++)
   {
      %skip = false;
      %below = 0;
      for (%i = 0; %i < %n; %i++)
      {
         if (getWord(%cleared, %i) == %r)
            %skip = true;
         if (getWord(%cleared, %i) < %r)
            %below++;
      }
      if (%skip || %below == 0)
         continue;
      for (%c = 0; %c < $Tet::Cols; %c++)
      {
         %o = $Tet::O[%seat, %r, %c];
         if (%o $= "")
            continue;
         $Tet::O[%seat, %r - %below, %c] = %o;
         $Tet::O[%seat, %r, %c] = "";
         if (isObject(%o))
            %o.setTransform(tetCellX(%seat, %c) SPC "-1.5" SPC tetCellZ(%r - %below));
      }
   }
}

function tetAddGarbage(%cl, %n)
{
   %seat = %cl.tetSeat;
   %hole = mFloor(getRandom() * $Tet::Cols);
   for (%r = 39; %r >= 0; %r--)
      for (%c = 0; %c < $Tet::Cols; %c++)
      {
         %o = $Tet::O[%seat, %r, %c];
         if (%o $= "")
            continue;
         $Tet::O[%seat, %r, %c] = "";
         if (%r + %n > 39)
         {
            if (isObject(%o))
               %o.delete();
            %cl.tetOverflow = true;
            continue;
         }
         $Tet::O[%seat, %r + %n, %c] = %o;
         if (isObject(%o))
            %o.setTransform(tetCellX(%seat, %c) SPC "-1.5" SPC tetCellZ(%r + %n));
      }
   for (%r = 0; %r < %n; %r++)
      for (%c = 0; %c < $Tet::Cols; %c++)
         if (%c != %hole)
         {
            %b = tetField(TetGarbage, tetCellX(%seat, %c), -1.5, tetCellZ(%r), $Tet::Cell, 3, $Tet::Cell);
            %cl.tetGroup.add(%b);
            $Tet::O[%seat, %r, %c] = %b;
         }
   tetPurgeZones();
}

function tetScoreLock(%cl, %lines, %tspin)
{
   %level = %cl.tetLevel;
   %points = 0;
   if (%tspin == 2)
      %points = getWord("400 800 1200 1600", %lines);
   else if (%tspin == 1)
      %points = getWord("100 200 400 400", %lines);
   else if (%lines > 0)
      %points = getWord("0 100 300 500 800", %lines);
   %points = %points * %level;
   %difficult = %lines == 4 || (%tspin > 0 && %lines > 0);
   %attack = 0;
   if (%lines > 0)
   {
      %cl.tetCombo++;
      %points += 50 * %cl.tetCombo * %level;
      if (%tspin == 2)
         %attack = getWord("0 2 4 6", %lines);
      else if (%tspin == 1)
         %attack = %lines;
      else
         %attack = getWord("0 0 1 2 4", %lines);
      if (%difficult && %cl.tetB2B)
      {
         %points = mFloor(%points * 1.5);
         %attack++;
      }
      %cl.tetB2B = %difficult;
      %combo = %cl.tetCombo > 11 ? 11 : %cl.tetCombo;
      %attack += $Tet::ComboGarbage[%combo];
      %cl.tetLines += %lines;
      %label = %lines == 4 ? "TETRIS" : getWord("X SINGLE DOUBLE TRIPLE", %lines);
      if (%tspin == 2)
         %label = "T-SPIN " @ %label;
      else if (%tspin == 1)
         %label = "T-SPIN MINI " @ %label;
      if (%cl.tetB2B && %difficult && %points > 0)
         %label = %label @ "  (BACK-TO-BACK)";
      if (%cl.tetCombo > 0)
         %label = %label @ "  " @ %cl.tetCombo @ " COMBO";
      centerPrint(%cl, %label, 2, 1);
   }
   else
   {
      %cl.tetCombo = -1;
      if (%tspin > 0)
      {
         %points += (%tspin == 2 ? 400 : 100) * %level;
         centerPrint(%cl, %tspin == 2 ? "T-SPIN" : "T-SPIN MINI", 2, 1);
      }
   }
   %cl.tetScore += %points;
   // cancel our own pending garbage first, then send the rest to a random opponent
   if (%attack > 0)
   {
      %cancel = %attack < %cl.tetPending ? %attack : %cl.tetPending;
      %cl.tetPending -= %cancel;
      %attack -= %cancel;
      if (%attack > 0)
         tetSendGarbage(%cl, %attack);
   }
}

function tetSendGarbage(%cl, %n)
{
   %list = "";
   for (%i = 0; %i < ClientGroup.getCount(); %i++)
   {
      %o = ClientGroup.getObject(%i);
      if (%o != %cl && %o.tetAlive)
         %list = %list SPC %o;
   }
   %list = trim(%list);
   if (%list $= "")
      return;
   %target = getWord(%list, mFloor(getRandom() * getWordCount(%list)));
   %target.tetPending += %n;
   tetHud(%target);
}

// ---------------------------------------------------------------------------------------------------------
// the loop

function tetInput(%cl, %now)
{
   // the keys are read by Retro.cs from the player's tiny velocity; a held key refreshes its timestamp
   %m = retroPad(%cl);
   if (%m & 4) %cl.tetKeyL = %now;
   if (%m & 8) %cl.tetKeyR = %now;
   if (%m & 2) %cl.tetKeyB = %now;
   if (%m & 1)
   {
      if (%now - %cl.tetKeyF > $Tet::HardDropGapMs)
         %cl.tetHardDrop = true;
      %cl.tetKeyF = %now;
   }
}

function tetStep(%cl, %dt, %now)
{
   if (!%cl.tetAlive)
   {
      if (%now >= %cl.tetRespawnAt)
         tetStartBoard(%cl);
      return;
   }
   // press events (rotate, hold) queued by the trigger handler
   %q = %cl.tetActions;
   %cl.tetActions = "";
   for (%i = 0; %i < getWordCount(%q) && %cl.tetAlive; %i++)
   {
      %a = getWord(%q, %i);
      if (%a $= "cw")
         tetRotate(%cl, 1);
      else if (%a $= "ccw")
         tetRotate(%cl, 3);
      else if (%a $= "hold")
         tetHoldPiece(%cl);
   }
   if (!%cl.tetAlive)
      return;
   if (%cl.tetHardDrop)
   {
      %cl.tetHardDrop = false;
      tetHardDrop(%cl);
      return;
   }
   // left/right with DAS and ARR
   %heldL = %now - %cl.tetKeyL < $Tet::KeyGraceMs;
   %heldR = %now - %cl.tetKeyR < $Tet::KeyGraceMs;
   %dir = 0;
   if (%heldR && !%heldL)
      %dir = 1;
   else if (%heldL && !%heldR)
      %dir = -1;
   else if (%heldL && %heldR)
      %dir = -1;
      if (%cl.tetKeyR > %cl.tetKeyL)
         %dir = 1;
   if (%dir != %cl.tetDir)
   {
      %cl.tetDir = %dir;
      if (%dir != 0)
      {
         tetTryMove(%cl, %dir, 0);
         %cl.tetDas = %now + $Tet::DasMs;
      }
   }
   else if (%dir != 0 && %now >= %cl.tetDas)
   {
      tetTryMove(%cl, %dir, 0);
      %cl.tetDas = %now + $Tet::ArrMs;
   }
   // gravity, faster while back is held
   %interval = tetGravityMs(%cl.tetLevel);
   %soft = %now - %cl.tetKeyB < $Tet::KeyGraceMs;
   if (%soft)
   {
      %interval = %interval / 20;
      if (%interval < 17)
         %interval = 17;
   }
   %cl.tetFall += %dt;
   %guard = 0;
   while (%cl.tetFall >= %interval && %guard < 25)
   {
      %cl.tetFall -= %interval;
      %guard++;
      if (tetTryMove(%cl, 0, -1))
      {
         if (%soft)
            %cl.tetScore++;
      }
      else
      {
         %cl.tetFall = 0;
         break;
      }
   }
   // lock delay while resting on something
   if (tetGrounded(%cl))
   {
      %cl.tetLock += %dt;
      if (%cl.tetLock >= $Tet::LockMs)
      {
         tetLock(%cl);
         return;
      }
   }
   else
      %cl.tetLock = 0;
   if (%cl.tetDirty)
   {
      %cl.tetDirty = false;
      tetPlace(%cl);
   }
}

function tetTick()
{
   cancel($Tet::TickThread);
   $Tet::TickThread = schedule($Tet::TickMs, 0, tetTick);
   if (!isObject(MissionCleanup))
      return;
   $Tet::TickN++;
   %now = getSimTime();
   %dt = $Tet::LastTick $= "" ? 32 : %now - $Tet::LastTick;
   $Tet::LastTick = %now;
   if (%dt > 200)
      %dt = 200;
   for (%i = 0; %i < ClientGroup.getCount(); %i++)
   {
      %cl = ClientGroup.getObject(%i);
      if (%cl.tetSeat $= "" || !isObject(%cl.player))
         continue;
      tetInput(%cl, %now);
      tetStep(%cl, %dt, %now);
   }
}

function tetInitPlayer(%cl)
{
   if (%cl.tetSeat $= "")
   {
      %seat = tetFreeSeat();
      if (%seat < 0)
         return;
      %cl.tetSeat = %seat;
      tetMakeFrame(%cl);
   }
   %a = tetAnchor(%cl.tetSeat);
   retroAttach(%cl, getWord(%a, 0), getWord(%a, 1), getWord(%a, 2));
   commandToClient(%cl, 'setHudMode', 'Standard');
   if (!%cl.tetAlive && !isObject(%cl.tetGroup))
      tetStartBoard(%cl);
}

function tetFreeClient(%cl)
{
   retroDetach(%cl);
   tetFreeBoard(%cl);
   if (isObject(%cl.tetFrame))
      %cl.tetFrame.delete();
   %cl.tetFrame = "";
   %cl.tetSeat = "";
   %cl.tetAlive = false;
}

// Switch the package on and off with the mission, so stock DM missions are untouched.
function tetWatch()
{
   cancel($Tet::WatchThread);
   $Tet::WatchThread = schedule(250, 0, tetWatch);
   %on = ($CurrentMission $= $Tet::Mission);
   if (%on && !$Tet::PkgOn)
   {
      activatePackage(T2LaunchTetris);
      $Tet::PkgOn = true;
      $Tet::LastTick = "";
      tetTick();
      echo("Tetris ruleset on");
   }
   else if (!%on && $Tet::PkgOn)
   {
      deactivatePackage(T2LaunchTetris);
      $Tet::PkgOn = false;
      cancel($Tet::TickThread);
      echo("Tetris ruleset off");
   }
}

package T2LaunchTetrisBoot
{
   function CreateServer(%mission, %missionType)
   {
      Parent::CreateServer(%mission, %missionType);
      tetDatablocks();
   }
};

package T2LaunchTetris
{
   function DMGame::equip(%game, %player)
   {
      // blocks carry nothing
   }

   function DMGame::createPlayer(%game, %client, %spawnLoc, %respawn)
   {
      Parent::createPlayer(%game, %client, %spawnLoc, %respawn);
      tetInitPlayer(%client);
   }

   function DMGame::onClientKilled(%game, %clVictim, %clKiller, %damageType, %implement, %damageLoc)
   {
      // the stand-in Player is not part of the game: no respawn timer or orbit camera
      %clVictim.deaths++;
   }

   // press events: fire = counter-clockwise, jump and jet = clockwise, alt-fire and throw = hold
   function DMGame::ObserverOnTrigger(%game, %data, %obj, %trigger, %state)
   {
      %client = %obj.getControllingClient();
      if (%client && %client.tetSeat !$= "")
      {
         if (%state)
         {
            if (%trigger == 0)
               %client.tetActions = %client.tetActions SPC "ccw";
            else if (%trigger == 2 || %trigger == 3)
               %client.tetActions = %client.tetActions SPC "cw";
            else if (%trigger == 1 || %trigger == 4)
               %client.tetActions = %client.tetActions SPC "hold";
            %client.tetActions = trim(%client.tetActions);
         }
         return false;
      }
      return Parent::ObserverOnTrigger(%game, %data, %obj, %trigger, %state);
   }

   // a seated player is a Player now: its triggers go through the same handler
   function Armor::onTrigger(%data, %player, %trigger, %val)
   {
      %client = %player.getControllingClient();
      if (%client && %client.tetSeat !$= "")
      {
         DMGame::ObserverOnTrigger(Game, %data, %player, %trigger, %val);
         return;
      }
      Parent::onTrigger(%data, %player, %trigger, %val);
   }

   function DMGame::onClientLeaveGame(%game, %client)
   {
      tetFreeClient(%client);
      Parent::onClientLeaveGame(%game, %client);
   }
};

if (!$Tet::Installed)
{
   $Tet::Installed = true;
   activatePackage(T2LaunchTetrisBoot);
   tetDatablocks();
   tetWatch();
   echo("Tetris gametype script loaded");
}
